# CE4303 - Host Linux, parte azul (usando los archivos de Dari)

Esta version parte de los `.c` y `.h` entregados por el usuario, no de
una estructura completamente nueva. Se mantuvieron los nombres de
funciones en ingles (`ipc_create`, `serial_send`, `node_calibration`...),
comentarios con `//`, y se separo cada responsabilidad en archivos pequenos.

**LEER PRIMERO:** `CAMBIOS_PARA_TU_REPOSITORIO.md` explica archivo por
archivo lo que se cambio; `PROTOCOLO_PARA_EL_GRUPO.md` explica el
formato que DEBEN utilizar los microcontroladores.

## Compilar y probar en Linux

Necesitas GCC, Make y Python 3 (Python solo para el simulador de pruebas).

```bash
make clean
make
make test
```

`make test` ejecuta dos integraciones SIN hardware:

1. Dos nodos virtuales: calibracion, configuracion de N, despliegue,
   inicio, recepcion, Gantt, STOP y reporte.
2. Desconexion simulada: deteccion de nodo caido y registro de rechazos.

El planificador host implementa admission RMS/EDF, seleccion de nodo,
reubicacion y admision manual. Las pruebas PTY actuales ejercitan comunicacion
y reportes; aun falta probar por separado las decisiones del planificador con
casos limite y resultados esperados para cada algoritmo.

## Ejecutar con ESP32 y Raspberry Pi Pico

Abrir `host/config/host.conf` y cambiar:

```ini
puerto_esp32=/dev/ttyUSB0
puerto_pico=/dev/ttyACM0
```

Son EJEMPLOS. Averigua tus puertos con `ls /dev/ttyUSB* /dev/ttyACM*`.
Si Linux no deja abrirlos, revisar permisos del dispositivo (ej. `dialout`).
El firmware tiene que usar 115200 baudios y respetar el documento del protocolo.

Despues:

```bash
./host_app host/config/host.conf
```

Dentro del programa, presionar `q` para terminar. En una terminal,
se puede teclear `a ID P/S W T D criticidad` para intentar insertar una
tarea; el host la asigna solo si supera la admision RMS/EDF y hay un nodo activo.

## Archivos que genera

- `output/eventos.csv`: eventos recibidos, fallas y rechazos.
- `output/gantt.csv`: tareas que se ejecutaron en cada tick reportado.
- `output/gantt.svg`: Gantt sencillo a partir del CSV, para abrir en navegador.
- `output/resumen.txt`: utilizacion teorica inicial, estadisticas,
  cambios de tarea, deadlines y datos de reporte.

El SVG dibuja hasta 5000 ticks por tamaño, pero el CSV conserva todos los
recibidos. Si no llegan ticks de ejecucion por `MSG_EXEC_BATCH`, el Gantt
queda incompleto. No se inventan tiempos.

## Distribucion de carpetas

```text
common/
    include/        protocol.h, crc16.h, task_types.h, wire_format.h
    src/            protocol.c, crc16.c
host/
    include/        archivos .h de cada modulo
    src/            main.c, driver.c, reporter.c, ipc_shm.c, etc.
    config/         host.conf, tasks_input.txt, asignaciones.csv
tools/
    test_integracion.py
BASE_ORIGINAL_USUARIO/   los archivos adjuntos originales
Makefile
```

En el repositorio principal `firmware/` debe permanecer como una carpeta
hermana de `host/` y `common/` (no meterla dentro de `host/`).

## Pendiente para cumplir la especificacion

- Implementar el motor de tiempo real en `firmware/core/rts_engine.c` y
  conectarlo a las aplicaciones de Pico y ESP32: liberacion de tareas,
  planificacion RMS/EDF, deadlines, ejecucion y activacion de esporadicas.
- Agregar pruebas unitarias del planificador host para RMS, EDF, seleccion de
  nodo, admision manual y reubicacion; comprobar los resultados con las
  formulas y restricciones del documento del proyecto.
- Completar la integracion UART/timer en ambos firmwares y verificar en
  hardware el protocolo existente, la sincronizacion de inicio, los reportes
  y la recuperacion ante la caida de un nodo. No cambiar el protocolo sin
  contrastar primero el comportamiento observado con el documento.
- Medir que los reportes y la cola compartida soporten la carga requerida y
  documentar los resultados de las pruebas RMS/EDF y FF/WF.

Si una ejecucion se cierra de forma abrupta y queda la SHM vieja,
revisa que no exista ningun host activo y despues borra solo su objeto:
`rm /dev/shm/ce4303_host`.

**NO compilar `simulado.c` junto con `main.c`.** Se conserva solo para
comparar tus pruebas anteriores; el host real ya no lo utiliza.
