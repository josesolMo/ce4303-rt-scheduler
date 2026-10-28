#!/bin/sh
# Corre el host con las 4 combinaciones (EDF/RMS x FF/WF) y guarda cada
# resultado en su propia carpeta dentro de resultados/.
#
# Uso (desde la raiz del repo, con las dos placas conectadas):
#   sh tools/correr_experimentos.sh [archivo_tareas] [segundos]
#
# Por defecto usa host/config/tareas_rms_vs_edf.txt y corre 15 s cada una.
# Los puertos y el resto de la configuracion se toman de host/config/host.conf.
TAREAS=${1:-host/config/tareas_rms_vs_edf.txt}
SEG=${2:-15}
BASE=host/config/host.conf
CARGA=$(basename "$TAREAS" .txt)

make host_app >/dev/null || exit 1
mkdir -p resultados

for ALG in EDF RMS; do
    for H in FF WF; do
        alg=$(echo "$ALG" | tr 'A-Z' 'a-z')
        h=$(echo "$H" | tr 'A-Z' 'a-z')
        DIR="resultados/${CARGA}_${alg}_${h}"
        CONF=$(mktemp)
        sed -e "s#^algoritmo=.*#algoritmo=$ALG#" \
            -e "s#^heuristica=.*#heuristica=$H#" \
            -e "s#^archivo_tareas=.*#archivo_tareas=$TAREAS#" \
            -e "s#^carpeta_reporte=.*#carpeta_reporte=$DIR#" \
            "$BASE" > "$CONF"
        mkdir -p "$DIR"
        echo "== $ALG + $H ($SEG s) -> $DIR"
        # La q llega despues de SEG segundos y termina el host
        (sleep "$SEG"; printf 'q') | ./host_app "$CONF" | tee "$DIR/consola.txt"
        rm -f "$CONF"
        sleep 2
    done
done
echo "Listo. Resultados en resultados/"
