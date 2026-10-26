#include "../include/gantt_svg.h"
#include <stdio.h>
#include <stdint.h>

// Crear un Gantt sencillo en SVG a partir de gantt.csv
int generar_gantt_svg(const char *csv, const char *svg) {
    FILE *f = fopen(csv, "r");
    if (!f) {
        return -1;
    }
    unsigned int max_tick = 0;
    char linea[120];
    fgets(linea, sizeof(linea), f); // Encabezado
    while (fgets(linea, sizeof(linea), f)) {
        int nodo, tarea;
        unsigned int tick;
        if (sscanf(linea, "%d,%u,%d", &nodo, &tick, &tarea) == 3 && tick > max_tick)
            max_tick = tick;
    }
    // El grafico muestra hasta 5000 ticks, el CSV guarda el historial completo.
    unsigned int limite = max_tick > 4999 ? 4999 : max_tick;
    int ancho = 120 + (int)(limite + 1) * 5;
    FILE *out = fopen(svg, "w");
    if (!out) {
        fclose(f);
        return -1;
    }
    fprintf(out, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"190\">\n", ancho);
    fprintf(out, "<rect width=\"100%%\" height=\"100%%\" fill=\"white\"/>\n");
    fprintf(out, "<text x=\"15\" y=\"23\" font-size=\"15\">Gantt por nodo (cada rectangulo = 1 tick)</text>\n");
    for (int n = 1; n <= 2; n++) {
        int y = n == 1 ? 55 : 115;
        fprintf(out, "<text x=\"12\" y=\"%d\" font-size=\"13\">Nodo %d</text>\n", y + 18, n);
        fprintf(out, "<rect x=\"100\" y=\"%d\" width=\"%d\" height=\"32\" fill=\"#eeeeee\"/>\n", y, (int)(limite + 1) * 5);
    }
    static const char *colores[] = {"#d0d0d0", "#3b82f6", "#16a34a", "#f59e0b", "#a855f7", "#ef4444", "#06b6d4"};
    rewind(f);
    fgets(linea, sizeof(linea), f);
    while (fgets(linea, sizeof(linea), f)) {
        int nodo, tarea;
        unsigned int tick;
        if (sscanf(linea, "%d,%u,%d", &nodo, &tick, &tarea) != 3 ||
            nodo < 1 || nodo > 2 || tick > limite || tarea < 0) continue;
        int y = nodo == 1 ? 55 : 115;
        fprintf(out, "<rect x=\"%u\" y=\"%d\" width=\"5\" height=\"32\" fill=\"%s\"><title>Nodo %d, tick %u, tarea %d</title></rect>\n",
                100 + tick * 5, y, colores[tarea % 7], nodo, tick, tarea);
    }
    for (unsigned int t = 0; t <= limite; t += 25) {
        fprintf(out, "<text x=\"%u\" y=\"163\" font-size=\"10\">%u</text>\n", 100 + t * 5, t);
    }
    fprintf(out, "<text x=\"12\" y=\"182\" font-size=\"10\">Colores por tarea (id modulo 7); gris = ocioso. SVG parcial si hay mas de 5000 ticks.</text>\n");
    fprintf(out, "</svg>\n");
    fclose(f); fclose(out);
    return 0;
}
