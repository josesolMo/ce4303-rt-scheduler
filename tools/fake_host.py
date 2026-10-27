#!/usr/bin/env python3
"""
Host falso corregido para probar un nodo por serial, sin el host real.

Uso:
    python3 tools/fake_host.py [puerto] [tick_ms]

    puerto   por defecto /dev/ttyACM0
    tick_ms  duración del tick, por defecto 10. Con 500 se alcanzan a
             ver los LEDs.

Después del START hay 8 segundos para presionar el botón físico.

Hace la secuencia normal: calibración, configuración, despliegue de
tareas, inicio, un disparo de la esporádica y detención. Imprime lo
que responde el nodo. Mientras corre muestra el Gantt (EXEC_BATCH, un
caracter por tick) y los eventos importantes. Solo usa la librería
estándar de Python.
"""
import os
import select
import sys
import termios
import time
import tty

NOMBRES = {
    0x02: "CALIB_RESP", 0x04: "ACK", 0x06: "TICK_REPORT",
    0x07: "EVENT_SPORADIC", 0x08: "EVENT_DEADLINE", 0x0A: "EVENT_RELEASE",
    0x0B: "EVENT_FINISH", 0x0C: "EXEC_BATCH", 0x0D: "SPORADIC_SKIP",
    0x0E: "EVENT_START", 0xFF: "NACK",
}


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def trama(tipo, payload=b""):
    b = bytes((0xAA, tipo, len(payload))) + payload
    return b + crc16(b).to_bytes(2, "little")


def u32(n):
    return n.to_bytes(4, "little")


def tarea(alg, id_, tipo, W, T, D, crit):
    return bytes((alg, id_, ord(tipo))) + u32(W) + u32(T) + u32(D) + bytes((crit,))


class Puerto:
    def __init__(self, ruta):
        self.fd = os.open(ruta, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        # Fijar la misma velocidad que Serial.begin(115200) en el ESP32.
        attrs = termios.tcgetattr(self.fd)
        attrs[4] = termios.B115200  # velocidad de entrada
        attrs[5] = termios.B115200  # velocidad de salida
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buf = b""

    def enviar(self, tipo, payload=b""):
        os.write(self.fd, trama(tipo, payload))

    def leer_tramas(self, segundos, resumen=False):
        """Lee e imprime tramas durante 'segundos'. Retorna la lista.
        Con resumen=True solo imprime el Gantt (EXEC_BATCH) y los eventos
        importantes, y al final cuenta cuántos llegaron de cada tipo."""
        fin = time.time() + segundos
        recibidas = []
        cuenta = {}
        while time.time() < fin:
            r, _, _ = select.select([self.fd], [], [], max(0, fin - time.time()))
            if r:
                self.buf += os.read(self.fd, 512)
            while True:
                i = self.buf.find(b"\xAA")
                if i < 0:
                    self.buf = b""
                    break
                self.buf = self.buf[i:]
                if len(self.buf) < 5:
                    break
                total = 3 + self.buf[2] + 2
                if len(self.buf) < total:
                    break
                fr, self.buf = self.buf[:total], self.buf[total:]
                if crc16(fr[:-2]) != int.from_bytes(fr[-2:], "little"):
                    print("   !! trama con CRC malo")
                    continue
                tipo, datos = fr[1], fr[3:-2]
                recibidas.append((tipo, datos))
                cuenta[tipo] = cuenta.get(tipo, 0) + 1
                if not resumen or tipo in (0x0C, 0x08, 0x07, 0x0D, 0x04, 0xFF):
                    mostrar(tipo, datos)
        if resumen:
            print("   resumen: " + ", ".join(
                f"{NOMBRES.get(t, hex(t))}={n}" for t, n in sorted(cuenta.items())))
        return recibidas


def mostrar(tipo, datos):
    nombre = NOMBRES.get(tipo, hex(tipo))
    if tipo == 0x02:
        print(f"   <- {nombre}: s_n = {int.from_bytes(datos, 'little')} UT por tick")
    elif tipo in (0x04, 0xFF):
        print(f"   <- {nombre} [{datos[0] if datos else '-'}]")
    elif tipo == 0x0C:
        tick, n = int.from_bytes(datos[:4], "little"), datos[4]
        ids = "".join("." if x == 0 else str(x) for x in datos[5:5 + n])
        print(f"   <- {nombre} desde tick {tick}: {ids}")
    elif len(datos) == 5:
        print(f"   <- {nombre}: tick {int.from_bytes(datos[:4], 'little')}, tarea {datos[4]}")
    else:
        print(f"   <- {nombre}: {datos.hex(' ')}")


def main():
    ruta = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    tick_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    p = Puerto(ruta)
    EDF = 1
    # Con ticks lentos hay pocos eventos y se pueden imprimir todos
    resumen = tick_ms < 100

    print(f"-> CALIB_REQ (tick = {tick_ms * 1000} us)")
    p.enviar(0x01, u32(tick_ms * 1000))
    # El nodo hace 20 rondas, cada una de duración tick_ms.
    calib_wait = max(2.0, (20 * tick_ms / 1000.0) + 1.0)
    r = p.leer_tramas(calib_wait)
    sn = next((int.from_bytes(d, "little") for t, d in r if t == 0x02), None)
    if sn is None:
        print("No llegó la calibración. ¿Está cargado el programa y cerrado screen?")
        return

    # Buscar un lote aproximadamente cada segundo para que el Gantt también
    # aparezca cuando se usan ticks visuales de 100 o 500 ms.
    report_every = max(1, min(50, 1000 // max(1, tick_ms)))
    print(f"-> NODE_CONFIG (reporte cada {report_every} ticks)")
    p.enviar(0x10, report_every.to_bytes(2, "little"))
    p.leer_tramas(0.5)

    # Las mismas tareas de las pruebas anteriores, con W en UT reales
    tareas = [
        tarea(EDF, 1, "P", 2 * sn, 5, 5, 1),
        tarea(EDF, 2, "P", 4 * sn, 7, 7, 1),
        tarea(EDF, 3, "S", 2 * sn, 10, 10, 2),
        tarea(EDF, 1, "P", 10, 5, 5, 1),   # id repetido: debe dar NACK
    ]
    for t in tareas:
        print(f"-> TASK_DEPLOY id {t[1]}")
        p.enviar(0x03, t)
        p.leer_tramas(0.5)

    print("-> trama corrupta (debe dar NACK)")
    os.write(p.fd, bytes((0xAA, 0x05, 0x00, 0x12, 0x34)))
    p.leer_tramas(0.5)

    print("-> START_SYNC (los LEDs deberían empezar, puedes presionar el botón)")
    p.enviar(0x05)
    p.leer_tramas(8, resumen=resumen)

    print("-> TRIGGER_TASK 3 (como presionar el botón)")
    p.enviar(0x0F, bytes((3,)))
    p.leer_tramas(2, resumen=resumen)

    print("-> STOP_REQ (los LEDs se apagan)")
    p.enviar(0x09)
    p.leer_tramas(1, resumen=resumen)

    os.close(p.fd)


if __name__ == "__main__":
    main()