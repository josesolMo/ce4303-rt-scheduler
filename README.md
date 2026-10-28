# CE4303 — Calendarizador de Tiempo Real Heterogéneo

Proyecto 1 de Principios de Sistemas Operativos (TEC). Una computadora con Linux
(el **host**) reparte tareas periódicas y esporádicas entre dos microcontroladores,
un **ESP32** y una **Raspberry Pi Pico W**. Cada nodo las ejecuta con un
calendarizador local propio (**RMS** o **EDF**), y el host calibra los nodos,
hace la admisión y la asignación (**First-Fit** o **Worst-Fit**), vigila la caída
de nodos y genera el reporte.

La documentación completa (diseño, protocolo, experimentos y atributo de Trabajo
en Equipo) está en [`doc/Informe_Proyecto_SO.pdf`](doc/Informe_Proyecto_SO.pdf).

**Integrantes:** Jose Pablo Fernández Rojas, Darío Garro Moya,
Jarod David de la O Segura y José Andrés Solano Mora.

## Estructura del repositorio

```text
common/            Protocolo compartido host-nodos (tramas, CRC-16, wire_format.h)
firmware/core/     Calendarizador local (rts_engine) y unidad de trabajo (ut.c, sin modificar)
firmware/pico/     Firmware de la Raspberry Pi Pico W (Pico SDK, CMake)
firmware/esp32/    Firmware del ESP32 (PlatformIO, framework Arduino)
host/              Host en C para Linux (planificador, drivers, reportero, IPC)
host/config/       host.conf y archivos de tareas de ejemplo
tools/             Pruebas, host falso, nodo de PC y scripts de experimentos
doc/               Documentación (LaTeX y PDF) y figuras
Makefile           Compilación del host y de las herramientas
```

## Host (Linux)

Requisitos: GCC, Make y Python 3.

```bash
make            # compila host_app
make run        # ejecuta el host con host/config/host.conf
```

Dentro del host:

- `q` termina: detiene los nodos, libera los recursos y genera el reporte.
- `a ID P/S W T D criticidad` + Enter agrega una tarea con el sistema en marcha
  (por ejemplo `a 9 P 300 20 20 1`).

### Configuración (`host/config/host.conf`)

| Clave | Ejemplo | Significado |
|---|---|---|
| `algoritmo` | `EDF` | `RMS` o `EDF` |
| `heuristica` | `FF` | `FF` (First-Fit) o `WF` (Worst-Fit) |
| `tick_ms` | `10` | Duración del tick en milisegundos |
| `reporte_cada` | `5` | Ticks entre reportes de tick de cada nodo |
| `reportes_perdidos` | `4` | Reportes seguidos sin llegar para declarar un nodo caído |
| `puerto_esp32` | `/dev/ttyUSB0` | Puerto serial del ESP32 (nodo 1) |
| `puerto_pico` | `/dev/ttyACM0` | Puerto serial de la Pico (nodo 2) |
| `archivo_tareas` | `host/config/tareas_rms_vs_edf.txt` | Archivo de tareas |
| `carpeta_reporte` | `output` | Carpeta del reporte |

Los puertos se averiguan con `ls /dev/ttyUSB* /dev/ttyACM*`.

### Archivo de tareas

Una tarea por línea: `id,tipo,W,T,D,criticidad`, con tipo `P` (periódica) o
`S` (esporádica). Las líneas que empiezan con `#` son comentarios. Ejemplos en
`host/config/`:

- `tareas_rms_vs_edf.txt`: carga de los experimentos (RMS/EDF y FF/WF).
- `tareas_rechazos.txt`: tarea que no cabe, tercera esporádica y línea inválida.
- `tareas_miss_esporadica.txt`: esporádicas que pierden su plazo al presionar el botón.
- `tasks_input.txt`: carga liviana.

### Reporte

Al terminar, en `carpeta_reporte`:

- `resumen.txt`: utilización teórica y medida por nodo, estadísticas por tarea,
  cambios de tarea, caídas y tareas aceptadas y rechazadas.
- `eventos.csv`: todos los eventos y los rechazos con su motivo.
- `gantt.csv` y `gantt.svg`: la tarea que corrió en cada tick de cada nodo.

## Nodo Raspberry Pi Pico W

Con VS Code y la extensión **Raspberry Pi Pico** (SDK 2.3.1):

1. Abrir la carpeta `firmware/pico` (no la raíz del repo) e importarla con
   **Import Project**.
2. **Compile Project** genera `build/pico_firmware.uf2`.
3. **Run Project** lo carga por USB. La primera vez, conectar la placa con el
   botón **BOOTSEL** presionado.

## Nodo ESP32

Con VS Code y la extensión **PlatformIO IDE**:

1. Abrir la carpeta `firmware/esp32`.
2. **Build** (✓) y **Upload** (→). Si se queda en `Connecting...`, mantener
   presionado el botón **BOOT** de la placa.

`firmware/esp32/lib/core` es un enlace simbólico a `firmware/core`.

## Conexiones

Cada LED lleva una resistencia de 220–330 Ω a GND. El botón va a GND (usa el
pull-up interno).

| Función | Pico W (GPIO / pin) | ESP32 (GPIO) |
|---|---|---|
| LED tarea, bit 0 | GP2 / 4 | 32 |
| LED tarea, bit 1 | GP3 / 5 | 33 |
| LED tarea, bit 2 | GP4 / 6 | 26 |
| LED tarea, bit 3 | GP5 / 7 | 25 |
| LED plazo perdido | GP6 / 9 | 23 |
| Botón | GP15 / 20 | 22 |

## Pruebas sin hardware

```bash
make test                               # host con dos nodos simulados (normal y caída)
make test_planner && ./test_planner     # planificador: FF/WF, RMS/EDF, rechazos, caída
make test_rts && ./test_rts             # calendarizador local en la PC
make pc_node && ./pc_node /tmp/ttyNODO  # firmware de la Pico corriendo en Linux
```

Para probar un nodo solo, sin el host:

```bash
python3 tools/fake_host.py /dev/ttyACM0 [tick_ms] [RMS|EDF]
```

## Experimentos

```bash
sh tools/correr_experimentos.sh   # EDF/RMS x FF/WF con las dos placas -> resultados/
sh tools/figuras_doc.sh           # figuras de Gantt -> doc/figuras/
```

## Problemas comunes

- **"Permission denied" al abrir el puerto:** agregar el usuario al grupo
  `dialout` (Ubuntu) o `uucp` (Arch) y volver a iniciar sesión.
- **No aparece `/dev/ttyUSB0` con el ESP32 (Ubuntu):** `sudo apt remove brltty`.
- **"SHM anterior?" al iniciar el host:** quedó la memoria compartida de una
  ejecución interrumpida. Si no hay otro host corriendo: `rm /dev/shm/ce4303_host`.
- **El host no termina con `Q`:** la tecla es `q` minúscula.