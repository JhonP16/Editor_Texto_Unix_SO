#ifndef EDITOR_H
#define EDITOR_H

#include <sys/types.h>
#include <stddef.h>

/**
 * ====================================================================================
 * EDITOR DE TEXTO CLI  (categoría "edicion" del shell eafitOS)
 * ====================================================================================
 * Editor de texto interactivo inspirado en 'ed', construido EXCLUSIVAMENTE sobre
 * llamadas al sistema POSIX para todo acceso a disco:
 *
 *      open(2), read(2), write(2), lseek(2), ftruncate(2), fstat(2), close(2)
 *
 * PROHIBIDO (y no usado) para el archivo editado: fopen/fread/fwrite/fclose.
 * La biblioteca estándar sólo se emplea para leer los comandos del usuario desde
 * STDIN (fgets) y para imprimir mensajes de la consola (printf).
 *
 * ------------------------------------------------------------------------------------
 * DECISIÓN DE DISEÑO 1: Recorrido byte a byte, sin índice de líneas en memoria.
 * ------------------------------------------------------------------------------------
 * El archivo es la ÚNICA fuente de verdad. No se mantiene un arreglo de offsets de
 * línea porque cualquier mutación (a/i/d/x) lo invalidaría y sincronizarlo es la
 * principal fuente de corrupción de datos en editores de este tipo.
 * Cada comando localiza su línea recorriendo los bytes del archivo contando '\n'.
 *
 *   Costo:     O(n) por comando en lugar de O(1).
 *   Beneficio: imposible que la vista en memoria y el disco se desincronicen.
 *
 * El recorrido lee en BLOQUES de ED_BLOQUE bytes con read(2) y escanea el bloque en
 * memoria buscando '\n'. Es "byte a byte" en la lógica, pero no hace una syscall por
 * byte: eso sería correcto pero absurdamente costoso (una llamada al kernel por
 * carácter). Es la misma razón por la que existe el buffering de stdio.
 *
 * ------------------------------------------------------------------------------------
 * DECISIÓN DE DISEÑO 2: Numeración de líneas base 1.
 * ------------------------------------------------------------------------------------
 * La línea 1 es la primera del archivo, igual que en 'ed', 'vi', 'sed' y los mensajes
 * de error de los compiladores. Un archivo vacío tiene 0 líneas.
 *
 * ------------------------------------------------------------------------------------
 * DECISIÓN DE DISEÑO 3: Portapapeles secuencial acumulativo.
 * ------------------------------------------------------------------------------------
 * 'y' NO reemplaza el portapapeles: le AÑADE la línea copiada al final. Así se pueden
 * copiar varias líneas sueltas y pegarlas juntas como un bloque con una sola 'x'.
 * Esto es lo que el enunciado llama "portapapeles secuencial local".
 */

/**
 * Unico punto de entrada del editor. Lo usan por igual el comando 'edit' del shell
 * (definido en editor.c) y el binario independiente 'edi' (edi_main.c).
 *
 * Todo lo demas -- las estructuras Editor y Portapapeles, los comandos y las rutinas de
 * recorrido -- es interno a editor.c y esta declarado 'static' alli: nada fuera del
 * modulo lo necesita, y exponerlo solo agrandaria la superficie del enlazado.
 *
 * 'ruta_inicial' puede ser NULL para arrancar sin ningun archivo abierto.
 * Retorna 0 al terminar la sesion.
 */
int editor_main(const char *ruta_inicial);

#endif /* EDITOR_H */
