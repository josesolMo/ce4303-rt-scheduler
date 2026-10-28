#!/usr/bin/env python3
"""Prueba integral: 2 nodos virtuales, calibracion, despliegue, ticks y q."""
import os
import select
import subprocess
import tempfile
import threading
import time
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def crc16(buf):
    crc = 0xFFFF
    for b in buf:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def trama(tipo, data=b''):
    b = bytes((0xAA, tipo, len(data))) + data
    return b + crc16(b).to_bytes(2, 'little')


class Nodo(threading.Thread):
    def __init__(self, nodo):
        super().__init__(daemon=True)
        self.nodo = nodo
        self.master, self.slave = os.openpty()
        self.path = os.ttyname(self.slave)
        self.acabado = threading.Event()
        self.iniciado = False
        self.siguientetick = 5
        self.proxenvio = 0
        self.tareas = []
        self.errores = []
        self.vistos = []

    def enviar(self, tipo, datos=b''):
        os.write(self.master, trama(tipo, datos))

    def ejecutar_mensaje(self, tipo, datos):
        self.vistos.append(tipo)
        if tipo == 0x01:  # calibracion
            assert len(datos) == 4 and int.from_bytes(datos, 'little') == 10000
            self.enviar(0x02, (25 if self.nodo == 1 else 15).to_bytes(4, 'little'))
        elif tipo == 0x10:
            assert len(datos) == 2 and int.from_bytes(datos, 'little') == 5
            self.enviar(0x04, b'\x00')
        elif tipo == 0x03:
            assert len(datos) == 16
            id_tarea = datos[1]
            self.tareas.append(id_tarea)
            self.enviar(0x04, bytes((id_tarea,)))
        elif tipo == 0x05:
            assert self.tareas
            self.enviar(0x04, b'\x00')
            self.iniciado = True
            self.proxenvio = time.monotonic() + 0.05
        elif tipo == 0x09:
            self.enviar(0x04, b'\x00')
            self.iniciado = False
        elif tipo == 0x0F:
            self.enviar(0x04, b'\x00')

    def run(self):
        buf = bytearray()
        try:
            while not self.acabado.is_set():
                r, _, _ = select.select([self.master], [], [], 0.01)
                if r:
                    bloque = os.read(self.master, 4096)
                    if not bloque:
                        break
                    buf.extend(bloque)
                while buf:
                    if buf[0] != 0xAA:
                        del buf[0]
                        continue
                    if len(buf) < 3:
                        break
                    n = buf[2] + 5
                    if len(buf) < n:
                        break
                    b = bytes(buf[:n]); del buf[:n]
                    assert crc16(b[:-2]) == int.from_bytes(b[-2:], 'little')
                    self.ejecutar_mensaje(b[1], b[3:-2])
                if self.iniciado and time.monotonic() >= self.proxenvio:
                    tick = self.siguientetick
                    tarea = self.tareas[0]
                    self.enviar(0x06, tick.to_bytes(4, 'little') + bytes((tarea,)))
                    # Cada tick del lote se registra en el Gantt.
                    self.enviar(0x0C, (tick - 4).to_bytes(4, 'little') + b'\x05' + bytes((tarea,)) * 5)
                    if tick == 5:
                        self.enviar(0x0A, tick.to_bytes(4, 'little') + bytes((tarea,)))
                    if tick == 10:
                        self.enviar(0x0B, tick.to_bytes(4, 'little') + bytes((tarea,)))
                    if tick == 15 and self.nodo == 2:
                        self.enviar(0x07, tick.to_bytes(4, 'little') + bytes((4,)))
                    self.siguientetick += 5
                    self.proxenvio = time.monotonic() + 0.05
        except Exception as exc:
            self.errores.append(repr(exc))
        finally:
            os.close(self.master)
            os.close(self.slave)


def main():
    probar_caida = '--caida' in sys.argv
    nodos = [Nodo(1), Nodo(2)]
    for n in nodos:
        n.start()
    with tempfile.TemporaryDirectory(prefix='ce4303_') as tmp:
        conf = Path(tmp) / 'host.conf'
        reportes = Path(tmp) / 'output'
        conf.write_text('\n'.join([
            'algoritmo=EDF', 'heuristica=WF', 'tick_ms=10', 'reporte_cada=5',
            'reportes_perdidos=4',
            f'puerto_esp32={nodos[0].path}', f'puerto_pico={nodos[1].path}',
            'archivo_tareas=host/config/tasks_input.txt',
            f'carpeta_reporte={reportes}', '',
        ]))
        p = subprocess.Popen(['./host_app', str(conf)], cwd=ROOT,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        try:
            if probar_caida:
                time.sleep(0.45)
                nodos[1].acabado.set()
                nodos[1].join(timeout=2)
                time.sleep(0.55)
            else:
                time.sleep(0.85)
            salida, _ = p.communicate('q\n', timeout=15)
        except Exception:
            p.kill(); salida, _ = p.communicate()
            raise
        finally:
            for n in nodos:
                n.acabado.set()
            for n in nodos:
                n.join(timeout=2)
        print(salida)
        assert p.returncode == 0, 'Host fallo'
        assert not any(n.errores for n in nodos), [n.errores for n in nodos]
        assert all(0x01 in n.vistos and 0x03 in n.vistos and 0x10 in n.vistos and 0x05 in n.vistos for n in nodos), [n.vistos for n in nodos]
        if not probar_caida:
            assert all(0x09 in n.vistos for n in nodos), [n.vistos for n in nodos]
        assert all((reportes / f).exists() for f in ('eventos.csv','gantt.csv','gantt.svg','resumen.txt'))
        eventos = (reportes / 'eventos.csv').read_text()
        gantt = (reportes / 'gantt.csv').read_text()
        resumen = (reportes / 'resumen.txt').read_text()
        assert 'TICK,1' in eventos and 'TICK,2' in eventos
        if probar_caida:
            assert 'NODO_CAIDO,2' in eventos and 'RECHAZADA,2' in eventos, eventos
        assert 'EJECUCION,1' in eventos and 'EJECUCION,2' in eventos
        assert len(gantt.splitlines()) >= 20
        assert 'Capacidad' not in resumen or 'UT/tick' in resumen
        print('OK: prueba de caida y rechazos registrados' if probar_caida else
              'OK: calibracion, despliegue, inicio, eventos, Gantt, stop y reporte')


if __name__ == '__main__':
    main()
