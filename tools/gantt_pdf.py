#!/usr/bin/env python3
"""
Genera un diagrama de Gantt (PDF o PNG) para la documentacion.

Desde el gantt.csv del host (o el de fake_host.py):
    python3 tools/gantt_pdf.py resultados/x/gantt.csv salida.pdf [desde] [hasta]

    desde, hasta: ventana de ticks a dibujar (por defecto 0 a 100).

Desde cadenas, un caracter por tick ('.' = libre), una fila por cadena:
    python3 tools/gantt_pdf.py --cadena "RMS=1122211222" "EDF=1122221122" salida.pdf
    Para marcar plazos perdidos, los ticks van entre corchetes en la etiqueta:
    "RMS[7,14]=1122211222" dibuja una marca en los ticks 7 y 14 de esa fila.

Requiere matplotlib (pip install matplotlib).
"""
import csv
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

# Colores por tarea, en orden fijo (la tarea con menor id usa el primero)
PALETA = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100",
          "#e87ba4", "#008300", "#6250d6", "#e34948"]
TINTA = "#0b0b0b"
TINTA_SECUNDARIA = "#52514e"
REJILLA = "#e4e3df"
NOMBRES_NODO = {1: "Nodo 1 (ESP32)", 2: "Nodo 2 (Pico)"}


def leer_csv(ruta, desde, hasta):
    """Retorna {etiqueta_fila: {tick: id}} desde un gantt.csv."""
    filas = {}
    with open(ruta) as f:
        for r in csv.DictReader(f):
            tick = int(r["tick"])
            if desde <= tick < hasta:
                nodo = int(r["nodo"])
                filas.setdefault(NOMBRES_NODO.get(nodo, f"Nodo {nodo}"), {})[tick] = int(r["tarea"])
    return dict(sorted(filas.items()))


def leer_cadenas(cadenas):
    """Retorna ({etiqueta: {tick: id}}, {etiqueta: [ticks con plazo perdido]})
    desde cadenas 'Etiqueta[7,14]=1122.2'."""
    filas, perdidos = {}, {}
    for c in cadenas:
        etiqueta, texto = c.split("=", 1)
        marcas = []
        if "[" in etiqueta:
            etiqueta, resto = etiqueta.split("[", 1)
            marcas = [int(x) for x in resto.rstrip("]").split(",") if x]
        texto = texto.replace(" ", "")
        filas[etiqueta] = {i: (0 if ch == "." else int(ch)) for i, ch in enumerate(texto)}
        perdidos[etiqueta] = marcas
    return filas, perdidos


def tramos(ticks):
    """Agrupa ticks seguidos de la misma tarea: [(inicio, largo, id)]."""
    res = []
    for t in sorted(ticks):
        tid = ticks[t]
        if tid == 0:
            continue
        if res and res[-1][2] == tid and res[-1][0] + res[-1][1] == t:
            res[-1] = (res[-1][0], res[-1][1] + 1, tid)
        else:
            res.append((t, 1, tid))
    return res


def dibujar(filas, salida, desde, hasta, perdidos=None):
    ids = sorted({i for f in filas.values() for i in f.values() if i != 0})
    color = {tid: PALETA[k % len(PALETA)] for k, tid in enumerate(ids)}

    alto = 0.9 + 0.55 * len(filas)
    fig, ax = plt.subplots(figsize=(7.0, alto))
    etiquetas = list(filas.keys())
    for y, etiqueta in enumerate(reversed(etiquetas)):
        for inicio, largo, tid in tramos(filas[etiqueta]):
            ax.broken_barh([(inicio, largo)], (y - 0.32, 0.64),
                           facecolors=color[tid], edgecolor="white", linewidth=1.0)

    # Plazos perdidos: una marca roja sobre la fila, en el tick del evento
    hay_perdidos = False
    for y, etiqueta in enumerate(reversed(etiquetas)):
        for t in (perdidos or {}).get(etiqueta, []):
            ax.plot([t, t], [y - 0.42, y + 0.42], color="#e34948", linewidth=2.0)
            ax.plot(t, y + 0.48, marker="v", color="#e34948", markersize=6)
            hay_perdidos = True

    ax.set_yticks(range(len(etiquetas)))
    ax.set_yticklabels(list(reversed(etiquetas)), color=TINTA, fontsize=9)
    ax.set_xlim(desde, hasta)
    ax.set_ylim(-0.6, len(etiquetas) - 0.4)
    ax.set_xlabel("tick", color=TINTA_SECUNDARIA, fontsize=9)
    ax.tick_params(axis="x", colors=TINTA_SECUNDARIA, labelsize=8)
    ax.tick_params(axis="y", length=0)
    ax.grid(axis="x", color=REJILLA, linewidth=0.6)
    ax.set_axisbelow(True)
    for lado in ("top", "right", "left"):
        ax.spines[lado].set_visible(False)
    ax.spines["bottom"].set_color(REJILLA)

    if ids:
        handles = [Patch(facecolor=color[t], label=f"T{t}") for t in ids]
        if hay_perdidos:
            handles.append(plt.Line2D([], [], color="#e34948", marker="v",
                                      linewidth=2.0, label="plazo perdido"))
        ax.legend(handles=handles, loc="lower right", bbox_to_anchor=(1.0, 1.0),
                  ncol=len(handles), frameon=False, fontsize=8,
                  handlelength=1.2, columnspacing=1.0, borderaxespad=0.2)
    fig.tight_layout()
    fig.savefig(salida, bbox_inches="tight")
    print(f"Figura guardada en {salida}")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    if args[0] == "--cadena":
        salida = args[-1]
        filas, perdidos = leer_cadenas(args[1:-1])
        hasta = max(len(f) for f in filas.values())
        dibujar(filas, salida, 0, hasta, perdidos)
        return
    ruta, salida = args[0], args[1]
    desde = int(args[2]) if len(args) > 2 else 0
    hasta = int(args[3]) if len(args) > 3 else 100
    filas = leer_csv(ruta, desde, hasta)
    if not filas:
        print("No hay ticks en esa ventana")
        return
    dibujar(filas, salida, desde, hasta)


if __name__ == "__main__":
    main()
