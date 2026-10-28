#!/bin/sh
# Genera las figuras de la documentacion (doc/figuras/) a partir de las
# carpetas de resultados/. Las figuras que no tengan datos se saltan y el
# documento muestra un recuadro en su lugar.
#
# Uso (desde la raiz del repo):  sh tools/figuras_doc.sh
OUT=doc/figuras
mkdir -p "$OUT"

# Gantt de las 4 combinaciones (tools/correr_experimentos.sh)
for c in edf_ff edf_wf rms_ff rms_wf; do
    f="resultados/tareas_rms_vs_edf_$c/gantt.csv"
    if [ -f "$f" ]; then
        python3 tools/gantt_pdf.py "$f" "$OUT/gantt_$c.pdf" 0 60
    fi
done

# RMS contra EDF en una sola placa (tools/fake_host.py ... RMS / EDF)
for a in rms edf; do
    f="resultados/fake_host_${a}_gantt.csv"
    if [ -f "$f" ]; then
        python3 tools/gantt_pdf.py "$f" "$OUT/fake_$a.pdf" 0 70
    fi
done

# Caida de un nodo: la ventana se centra en el ultimo tick del nodo caido
f="resultados/caida/gantt.csv"
if [ -f "$f" ]; then
    T=$(python3 - "$f" <<'EOF'
import csv, sys
ultimo = {}
for r in csv.DictReader(open(sys.argv[1])):
    n, t = int(r["nodo"]), int(r["tick"])
    ultimo[n] = max(ultimo.get(n, 0), t)
print(min(ultimo.values()))
EOF
)
    DESDE=$((T - 40))
    [ "$DESDE" -lt 0 ] && DESDE=0
    python3 tools/gantt_pdf.py "$f" "$OUT/gantt_caida.pdf" "$DESDE" $((T + 60))
fi

# Simulacion del calendarizador en PC (tools/test_rts.c, caso 1)
python3 tools/gantt_pdf.py --cadena \
    "RMS[7]=11222 11222 112.2 11222 11222 11222 1122." \
    "EDF=11222 21122 22112 11222 11222 21122 1122." \
    "$OUT/rms_vs_edf_sim.pdf"
