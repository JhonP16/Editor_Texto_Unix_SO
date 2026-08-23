#include "shell.h"
#include "editor.h"

#include <unistd.h>
#include <string.h>

/**
 * ====================================================================================
 * BINARIO INDEPENDIENTE DEL EDITOR:  ./editor [archivo]
 * ====================================================================================
 * Este archivo contiene ÚNICAMENTE el main() y el manejo de argumentos. Toda la lógica
 * vive en editor.c, que se compila también dentro del shell.
 *
 * Consecuencia de diseño (deliberada): el editor integrado en el shell y el binario
 * suelto no son dos programas parecidos, son el MISMO programa con dos puntos de
 * entrada. Corregir un fallo en editor.c lo corrige en ambos a la vez.
 *
 * Este binario es el que utiliza el script de pruebas pruebas.sh para ejercitar el
 * editor sin depender del shell.
 */
static void uso(const char *prog) {
    printf(COLOR_TITLE "Uso: %s [archivo]\n" COLOR_RESET, prog);
    printf("  Abre el editor de texto CLI. Si se indica un archivo, lo abre (o lo crea)\n");
    printf("  al arrancar. Sin argumentos, arranca sin archivo: use 'o <archivo>'.\n\n");
    printf("  " COLOR_PARAM "-h, --help" COLOR_RESET "   Muestra esta ayuda y termina.\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        uso(argv[0]);
        return 0;
    }
    if (argc > 2) {
        fprintf(stderr, COLOR_ERROR "Demasiados argumentos.\n" COLOR_RESET);
        uso(argv[0]);
        return 1;
    }

    return editor_main(argc == 2 ? argv[1] : NULL);
}
