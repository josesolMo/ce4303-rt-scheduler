# Makefile del proyecto (host Linux y herramientas de prueba en PC).
# El firmware de la Pico se compila con CMake (ver firmware/pico).
#
#   make            compila el host (host_app)
#   make test       pruebas de integracion del host con nodos virtuales
#   make test_rts   prueba del calendarizador local en la PC
#   make test_planner  prueba del planificador del host (FF/WF, admision, caida)
#   make pc_node    nodo de PC (firmware de la Pico corriendo en Linux)
#   make clean      borra lo compilado

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDLIBS  = -lrt -lpthread -lm

# simulado.c no se compila: es solo de referencia (ver README)
HOST_SRC = host/src/calibration.c host/src/driver.c host/src/gantt_svg.c \
           host/src/host_config.c host/src/ipc_shm.c host/src/main.c \
           host/src/node_events.c host/src/reporter.c host/src/scheduler.c \
           host/src/serial_driver.c host/src/task_deploy.c \
           common/src/protocol.c common/src/crc16.c

NODE_INC = -I common/include -I firmware/core

.PHONY: all test clean

all: host_app

host_app: $(HOST_SRC) $(wildcard host/include/*.h) $(wildcard common/include/*.h)
	$(CC) $(CFLAGS) $(HOST_SRC) -o $@ $(LDLIBS)

test: host_app
	python3 tools/test_integracion.py
	python3 tools/test_integracion.py --caida

test_rts: tools/test_rts.c firmware/core/rts_engine.c firmware/core/ut.c
	$(CC) $(CFLAGS) $(NODE_INC) $^ -o $@

test_planner: tools/test_planner.c host/src/scheduler.c host/src/host_config.c
	$(CC) $(CFLAGS) $^ -o $@ -lm

pc_node:
	sh tools/pc_node/build.sh

clean:
	rm -f host_app test_rts test_planner pc_node
