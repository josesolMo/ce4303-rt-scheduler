#!/bin/sh
# Compila el nodo de PC. Ejecutar desde la raíz del repo:
#   sh tools/pc_node/build.sh
set -e
INC="-I tools/pc_node -I common/include -I firmware/core -I firmware/pico"
gcc -O2 -Wall $INC -Dmain=pico_main -c firmware/pico/main.c -o tools/pc_node/main_pc.o
gcc -O2 -Wall $INC tools/pc_node/pc_stubs.c tools/pc_node/main_pc.o \
    firmware/pico/link.c firmware/core/rts_engine.c firmware/core/ut.c \
    common/src/protocol.c common/src/crc16.c \
    -o pc_node -lutil
rm -f tools/pc_node/main_pc.o
echo "Listo: ./pc_node /tmp/ttyNODO"
