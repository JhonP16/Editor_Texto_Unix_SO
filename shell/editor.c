#include "shell.h"
#include "editor.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <stdarg.h>

#define ED_RUTA_MAX   4096   /* Longitud máxima de una ruta (PATH_MAX típico en Linux) */
#define ED_LINEA_MAX  8192   /* Longitud máxima de una línea de comando leída de STDIN  */
#define ED_BLOQUE     4096   /* Tamaño del búfer de lectura: 1 página / 1 bloque de FS  */

/**
 * Portapapeles secuencial: arreglo dinámico de líneas (sin el '\n' terminador).
 * Crece por duplicación de capacidad con realloc(3) para amortizar el costo.
 */
typedef struct {
    char  **lineas;   /* Arreglo de punteros a cadenas reservadas con malloc(3) */
    size_t  n;        /* Número de líneas almacenadas actualmente               */
    size_t  cap;      /* Capacidad reservada del arreglo                        */
} Portapapeles;

/**
 * Estado completo de una sesión del editor.
 * Se pasa por referencia a todos los comandos; no hay estado global.
 */
typedef struct {
    int          fd;                  /* Descriptor del archivo abierto, -1 si no hay */
    char         ruta[ED_RUTA_MAX];   /* Ruta del archivo actualmente abierto         */
    int          abierto;             /* 1 si hay un archivo abierto, 0 si no         */
    int          trazar;              /* 1 = imprimir cada syscall estilo strace      */
    Portapapeles pp;                  /* Portapapeles secuencial local                */
} Editor;

                        /* Tamaño vía lseek SEEK_END*/

/**
 * ====================================================================================
 * MACROS DE TRAZADO CONDICIONAL
 * ====================================================================================
 * Reutilizan las macros LOG_SYSCALL del shell (shell.h) para mantener una única
 * estética de trazado en todo el proyecto, pero sólo imprimen si el usuario activó
 * el modo traza con el comando 't'.
 *
 * ¿Por qué condicional y apagado por defecto? Porque el recorrido byte a byte hace
 * varias lecturas por comando: trazar siempre convertiría la salida del editor en
 * ruido ilegible. El trazado es una herramienta de estudio, no el modo normal de uso.
 */
#define TRAZA(ed, nombre, fmt, ...) \
    do { if ((ed)->trazar) { LOG_SYSCALL(nombre, fmt, ##__VA_ARGS__); fflush(stdout); } } while (0)

#define TRAZA_OK(ed, res) \
    do { if ((ed)->trazar) { LOG_SYSCALL_RESULT(res); fflush(stdout); } } while (0)

#define TRAZA_ERR(ed) \
    do { if ((ed)->trazar) { LOG_SYSCALL_ERROR(strerror(errno)); fflush(stdout); } } while (0)

/**
 * Informa un fallo de llamada al sistema al usuario.
 * Cumple el requisito de la rúbrica de usar perror(3): imprime en STDERR el mensaje
 * de contexto seguido de la descripción textual de errno.
 */
static void ed_error(const char *contexto) {
    fprintf(stderr, COLOR_ERROR);
    fflush(stderr);
    perror(contexto);
    fprintf(stderr, COLOR_RESET);
    fflush(stderr);
}

/* Mensaje de error propio del editor (no proviene de una syscall, no hay errno). */
static void ed_aviso(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, COLOR_ERROR);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, COLOR_RESET "\n");
}

/* Verifica que haya un archivo abierto antes de operar sobre él. */
static int ed_requiere_archivo(const Editor *ed) {
    if (!ed->abierto || ed->fd < 0) {
        ed_aviso("No hay ningún archivo abierto. Use: o <archivo>");
        return 0;
    }
    return 1;
}

/**
 * ====================================================================================
 * ESCRITURA COMPLETA: write(2) puede escribir MENOS bytes de los pedidos
 * ====================================================================================
 * write(2) devuelve el número de bytes realmente escritos, que puede ser menor que el
 * solicitado (escrituras parciales por señales, límites del dispositivo, etc.).
 * Ignorar esto es un error clásico que corrompe archivos silenciosamente: por eso
 * TODA escritura del editor pasa por aquí, que reintenta hasta completar.
 *
 * Retorna 0 en éxito, -1 en error (errno queda posicionado).
 */
static int ed_escribir_todo(Editor *ed, int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t escritos = 0;

    while (escritos < n) {
        TRAZA(ed, "write", "%d, buf+%zu, %zu", fd, escritos, n - escritos);
        ssize_t w = write(fd, p + escritos, n - escritos);
        if (w == -1) {
            if (errno == EINTR) continue;   /* Interrumpido por señal: reintentar */
            TRAZA_ERR(ed);
            return -1;
        }
        TRAZA_OK(ed, w);
        escritos += (size_t)w;
    }
    return 0;
}

/**
 * Lectura completa de 'n' bytes desde el offset actual del descriptor.
 * Igual que write(2), read(2) puede devolver menos bytes de los pedidos sin que sea
 * un error (lecturas cortas). Retorna el total leído, o -1 en error.
 */
static ssize_t ed_leer_todo(Editor *ed, int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t leidos = 0;

    while (leidos < n) {
        TRAZA(ed, "read", "%d, buf+%zu, %zu", fd, leidos, n - leidos);
        ssize_t r = read(fd, p + leidos, n - leidos);
        if (r == -1) {
            if (errno == EINTR) continue;
            TRAZA_ERR(ed);
            return -1;
        }
        TRAZA_OK(ed, r);
        if (r == 0) break;                  /* Fin de archivo (EOF) */
        leidos += (size_t)r;
    }
    return (ssize_t)leidos;
}

/**
 * Imprime bytes crudos en STDOUT usando write(2) sobre el descriptor 1.
 *
 * ATENCIÓN — DETALLE CRÍTICO DE E/S:
 * printf(3) escribe en un búfer de usuario que stdio vacía cuando le conviene, pero
 * write(2) va directo al kernel. Si se mezclan sin cuidado, el contenido del archivo
 * aparecería ANTES que los mensajes impresos con printf, aunque el código diga lo
 * contrario. Por eso se fuerza fflush(stdout) antes de cada write(1, ...).
 */
static int ed_escribir_stdout(Editor *ed, const void *buf, size_t n) {
    fflush(stdout);
    return ed_escribir_todo(ed, STDOUT_FILENO, buf, n);
}

/* Reposiciona el cursor del archivo con lseek(2) y traza la llamada. */
static off_t ed_seek(Editor *ed, off_t off, int whence, const char *nombre_whence) {
    TRAZA(ed, "lseek", "%d, %ld, %s", ed->fd, (long)off, nombre_whence);
    off_t r = lseek(ed->fd, off, whence);
    if (r == -1) { TRAZA_ERR(ed); ed_error("lseek"); }
    else         { TRAZA_OK(ed, r); }
    return r;
}

/* Tamaño actual del archivo, obtenido moviendo el cursor al final con lseek(2). */
static off_t ed_tam_archivo(Editor *ed) {
    return ed_seek(ed, 0, SEEK_END, "SEEK_END");
}

/**
 * ====================================================================================
 * NÚCLEO DEL RECORRIDO BYTE A BYTE
 * ====================================================================================
 * Todas las operaciones del editor se apoyan en estas tres funciones. Ninguna mantiene
 * estado entre llamadas: siempre releen el archivo desde disco, que es lo que garantiza
 * que jamás haya desincronización entre memoria y disco.
 */

/**
 * Cuenta cuántas líneas tiene el archivo recorriendo sus bytes y contando '\n'.
 *
 * Convención adoptada (y documentada al usuario):
 *   - Archivo vacío                  -> 0 líneas.
 *   - "hola\nmundo\n"                -> 2 líneas.
 *   - "hola\nmundo"   (sin '\n' final) -> 2 líneas; la última es una línea incompleta.
 *
 * Retorna el número de líneas, o -1 si falla alguna syscall.
 */
static long ed_contar_lineas(Editor *ed) {
    char buf[ED_BLOQUE];
    long lineas = 0;
    int  ultimo_byte_es_nl = 1;   /* Un archivo vacío se trata como "ya terminado" */
    ssize_t r;

    if (ed_seek(ed, 0, SEEK_SET, "SEEK_SET") == -1) return -1;

    while (1) {
        TRAZA(ed, "read", "%d, buf, %d", ed->fd, ED_BLOQUE);
        r = read(ed->fd, buf, sizeof(buf));
        if (r == -1) {
            if (errno == EINTR) continue;
            TRAZA_ERR(ed);
            ed_error("read");
            return -1;
        }
        TRAZA_OK(ed, r);
        if (r == 0) break;                       /* EOF */

        /* Escaneo del bloque en memoria buscando fines de línea */
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') lineas++;
        }
        ultimo_byte_es_nl = (buf[r - 1] == '\n');
    }

    /* Cola sin '\n' final: cuenta como una línea más (línea incompleta) */
    if (!ultimo_byte_es_nl) lineas++;
    return lineas;
}

/**
 * Devuelve el offset en bytes donde COMIENZA la línea n (base 1).
 *
 * La línea 1 empieza en el byte 0. La línea k empieza en el byte siguiente al
 * (k-1)-ésimo '\n' del archivo. Se permite además consultar la posición
 * "una más allá de la última línea" (n == total+1), que equivale al fin de archivo:
 * eso es lo que hace que 'i' pueda insertar al final sin un caso especial aparte.
 *
 * Retorna el offset, o -1 si la línea no existe o falla una syscall.
 */
static off_t ed_offset_linea(Editor *ed, long n) {
    char buf[ED_BLOQUE];
    long  linea_actual = 1;
    off_t recorrido = 0;
    ssize_t r;

    if (n < 1) return -1;
    if (n == 1) return 0;                        /* La primera línea siempre está en 0 */

    if (ed_seek(ed, 0, SEEK_SET, "SEEK_SET") == -1) return -1;

    while (1) {
        TRAZA(ed, "read", "%d, buf, %d", ed->fd, ED_BLOQUE);
        r = read(ed->fd, buf, sizeof(buf));
        if (r == -1) {
            if (errno == EINTR) continue;
            TRAZA_ERR(ed);
            ed_error("read");
            return -1;
        }
        TRAZA_OK(ed, r);
        if (r == 0) break;                       /* EOF sin haber llegado a la línea n */

        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                linea_actual++;
                if (linea_actual == n) {
                    /* La línea n arranca justo después de este '\n' */
                    return recorrido + i + 1;
                }
            }
        }
        recorrido += r;
    }
    return -1;                                   /* La línea n no existe */
}

/**
 * Calcula la longitud de la línea que empieza en 'inicio'.
 *
 * Parámetros de salida:
 *   - len_sin_nl: bytes de texto de la línea, SIN contar el '\n'.
 *   - tiene_nl:   1 si la línea termina en '\n' (no es la última línea incompleta).
 *
 * La longitud TOTAL que ocupa la línea en disco es len_sin_nl + tiene_nl. Esa es la
 * cifra que usan 'd' y 'x' para saber cuántos bytes eliminar o desplazar.
 *
 * Retorna 0 en éxito, -1 en error.
 */
static int ed_longitud_linea(Editor *ed, off_t inicio, size_t *len_sin_nl, int *tiene_nl) {
    char buf[ED_BLOQUE];
    size_t len = 0;
    ssize_t r;

    *len_sin_nl = 0;
    *tiene_nl   = 0;

    if (ed_seek(ed, inicio, SEEK_SET, "SEEK_SET") == -1) return -1;

    while (1) {
        TRAZA(ed, "read", "%d, buf, %d", ed->fd, ED_BLOQUE);
        r = read(ed->fd, buf, sizeof(buf));
        if (r == -1) {
            if (errno == EINTR) continue;
            TRAZA_ERR(ed);
            ed_error("read");
            return -1;
        }
        TRAZA_OK(ed, r);
        if (r == 0) break;                       /* EOF: línea incompleta, sin '\n' */

        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                *len_sin_nl = len;
                *tiene_nl   = 1;
                return 0;
            }
            len++;
        }
    }
    *len_sin_nl = len;
    *tiene_nl   = 0;
    return 0;
}

/**
 * Lee 'len' bytes desde el offset 'inicio' a un búfer recién reservado con malloc(3).
 * El búfer devuelto lleva un '\0' extra al final para poder tratarlo como cadena;
 * ese byte NO forma parte de los datos del archivo.
 *
 * Es responsabilidad del llamador liberarlo con free(3).
 * Retorna NULL en error (o si len == 0 se devuelve un búfer de 1 byte con '\0').
 */
static char *ed_leer_rango(Editor *ed, off_t inicio, size_t len) {
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        ed_error("malloc");
        return NULL;
    }
    if (len == 0) { buf[0] = '\0'; return buf; }

    if (ed_seek(ed, inicio, SEEK_SET, "SEEK_SET") == -1) { free(buf); return NULL; }

    ssize_t leidos = ed_leer_todo(ed, ed->fd, buf, len);
    if (leidos == -1) { ed_error("read"); free(buf); return NULL; }

    buf[leidos] = '\0';
    return buf;
}

/**
 * ====================================================================================
 * COMANDO: o [archivo]   -- Abrir o crear
 * ====================================================================================
 * Syscalls: open(2) con O_RDWR|O_CREAT y modo 0644, close(2) del archivo anterior.
 *
 * Por que O_RDWR y no O_WRONLY: el editor necesita LEER para localizar las lineas y
 * ESCRIBIR para modificarlas sobre el mismo descriptor. Ademas ftruncate(2), que usa
 * el comando d, exige que el descriptor este abierto para escritura.
 *
 * Por que NO O_TRUNC: abrir un archivo existente en un editor jamas debe destruir su
 * contenido. O_CREAT sin O_TRUNC crea si no existe y respeta si ya existe.
 *
 * El modo 0644 (-rw-r--r--) solo se aplica cuando el archivo se CREA, y el kernel aun
 * le resta la mascara umask del proceso.
 */
static int ed_cmd_o(Editor *ed, const char *ruta) {
    if (ruta == NULL || ruta[0] == '\0') {
        ed_aviso("Uso: o <archivo>");
        return 1;
    }
    if (strlen(ruta) >= ED_RUTA_MAX) {
        ed_aviso("La ruta excede el máximo de %d caracteres.", ED_RUTA_MAX - 1);
        return 1;
    }

    /* Cerrar el archivo anterior para no filtrar descriptores al cambiar de archivo */
    if (ed->abierto && ed->fd >= 0) {
        TRAZA(ed, "close", "%d", ed->fd);
        if (close(ed->fd) == -1) { TRAZA_ERR(ed); ed_error("close"); }
        else                     { TRAZA_OK(ed, 0); }
        ed->fd = -1;
        ed->abierto = 0;
    }

    TRAZA(ed, "open", "\"%s\", O_RDWR|O_CREAT, 0644", ruta);
    int fd = open(ruta, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        TRAZA_ERR(ed);
        ed_error("open");
        return 1;
    }
    TRAZA_OK(ed, fd);

    ed->fd = fd;
    ed->abierto = 1;
    strncpy(ed->ruta, ruta, ED_RUTA_MAX - 1);
    ed->ruta[ED_RUTA_MAX - 1] = '\0';

    long lineas = ed_contar_lineas(ed);
    off_t tam   = ed_tam_archivo(ed);
    printf(COLOR_RESULT "Archivo '%s' abierto" COLOR_RESET
           " (fd=%d, %ld línea(s), %ld byte(s)).\n", ruta, fd, lineas, (long)tam);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: p [n]   -- Imprimir
 * ====================================================================================
 * Sin argumento imprime todo el archivo; con argumento imprime sólo la línea n.
 *
 * Syscalls: lseek(2), read(2), y write(2) sobre el DESCRIPTOR 1 (STDOUT).
 *
 * El contenido del archivo se emite con write(2) al FD 1 tal como exige el enunciado,
 * no con printf. Sólo los números de línea y los separadores usan printf, y por eso
 * cada write va precedido de un fflush(stdout) (ver ed_escribir_stdout).
 */
static int ed_cmd_p(Editor *ed, long n, int todo) {
    if (!ed_requiere_archivo(ed)) return 1;

    long total = ed_contar_lineas(ed);
    if (total < 0) return 1;

    if (total == 0) {
        printf(COLOR_INFO "(el archivo está vacío)\n" COLOR_RESET);
        return 0;
    }

    /* ---- Caso 1: imprimir UNA línea concreta ---- */
    if (!todo) {
        if (n < 1 || n > total) {
            ed_aviso("Línea %ld fuera de rango (el archivo tiene %ld línea(s)).", n, total);
            return 1;
        }
        off_t inicio = ed_offset_linea(ed, n);
        if (inicio == -1) { ed_aviso("No se pudo localizar la línea %ld.", n); return 1; }

        size_t len; int tiene_nl;
        if (ed_longitud_linea(ed, inicio, &len, &tiene_nl) == -1) return 1;

        char *linea = ed_leer_rango(ed, inicio, len);
        if (linea == NULL) return 1;

        printf(COLOR_PARAM "%4ld" COLOR_RESET " | ", n);
        ed_escribir_stdout(ed, linea, len);       /* write(2) al FD 1 */
        ed_escribir_stdout(ed, "\n", 1);
        free(linea);
        return 0;
    }

    /* ---- Caso 2: imprimir TODO el archivo recorriendo bytes hasta cada '\n' ---- */
    printf(COLOR_TITLE "--- %s (%ld línea(s)) ---\n" COLOR_RESET, ed->ruta, total);

    char buf[ED_BLOQUE];
    long num = 1;
    int  inicio_de_linea = 1;
    ssize_t r;

    if (ed_seek(ed, 0, SEEK_SET, "SEEK_SET") == -1) return 1;

    while (1) {
        TRAZA(ed, "read", "%d, buf, %d", ed->fd, ED_BLOQUE);
        r = read(ed->fd, buf, sizeof(buf));
        if (r == -1) {
            if (errno == EINTR) continue;
            TRAZA_ERR(ed); ed_error("read"); return 1;
        }
        TRAZA_OK(ed, r);
        if (r == 0) break;                                  /* EOF */

        ssize_t i = 0;
        while (i < r) {
            if (inicio_de_linea) {
                printf(COLOR_PARAM "%4ld" COLOR_RESET " | ", num);
                inicio_de_linea = 0;
            }
            /* Avanzar byte a byte hasta el fin de línea dentro de este bloque */
            ssize_t j = i;
            while (j < r && buf[j] != '\n') j++;

            if (j < r) {                        /* Se encontró el '\n' */
                ed_escribir_stdout(ed, buf + i, (size_t)(j - i + 1));
                num++;
                inicio_de_linea = 1;
                i = j + 1;
            } else {                            /* La línea sigue en el próximo bloque */
                ed_escribir_stdout(ed, buf + i, (size_t)(r - i));
                i = r;
            }
        }
    }

    /* Última línea sin '\n' final: se cierra visualmente y se advierte al usuario */
    if (!inicio_de_linea) {
        ed_escribir_stdout(ed, "\n", 1);
        printf(COLOR_INFO "(la última línea no termina en salto de línea)\n" COLOR_RESET);
    }
    printf(COLOR_TITLE "----------------------------------------\n" COLOR_RESET);
    return 0;
}

/**
 * Garantiza que el archivo termine en '\n' antes de añadir contenido nuevo.
 *
 * Sin esto, hacer 'a' sobre un archivo cuya última línea está incompleta pegaría el
 * texto nuevo al final de esa línea en lugar de crear una línea nueva.
 * Retorna 0 en éxito, -1 en error.
 */
static int ed_asegurar_nl_final(Editor *ed) {
    off_t tam = ed_tam_archivo(ed);
    if (tam == -1) return -1;
    if (tam == 0)  return 0;                    /* Archivo vacío: nada que arreglar */

    char ultimo;
    if (ed_seek(ed, tam - 1, SEEK_SET, "SEEK_SET") == -1) return -1;
    if (ed_leer_todo(ed, ed->fd, &ultimo, 1) != 1) { ed_error("read"); return -1; }

    if (ultimo != '\n') {
        if (ed_seek(ed, 0, SEEK_END, "SEEK_END") == -1) return -1;
        if (ed_escribir_todo(ed, ed->fd, "\n", 1) == -1) { ed_error("write"); return -1; }
    }
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: a [texto]   -- Añadir al final
 * ====================================================================================
 * Syscalls: lseek(2) con SEEK_END, write(2).
 *
 * Es la mutación más barata del editor: no hay que desplazar ningún byte, basta con
 * posicionar el cursor al final del archivo y escribir. Por eso no necesita malloc.
 */
static int ed_cmd_a(Editor *ed, const char *texto) {
    if (!ed_requiere_archivo(ed)) return 1;
    if (texto == NULL) texto = "";              /* 'a' sin texto añade una línea vacía */

    if (ed_asegurar_nl_final(ed) == -1) return 1;
    if (ed_seek(ed, 0, SEEK_END, "SEEK_END") == -1) return 1;

    size_t len = strlen(texto);
    if (ed_escribir_todo(ed, ed->fd, texto, len) == -1) { ed_error("write"); return 1; }
    if (ed_escribir_todo(ed, ed->fd, "\n", 1)   == -1) { ed_error("write"); return 1; }

    printf(COLOR_RESULT "Añadida línea %ld" COLOR_RESET " (%zu byte(s) + salto).\n",
           ed_contar_lineas(ed), len);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: d [n]   -- Borrar la línea n
 * ====================================================================================
 * Syscalls: lseek(2), read(2), write(2), ftruncate(2), malloc(3)/free(3).
 *
 * Un archivo no ofrece una operación "borrar en el medio": hay que compactarlo a mano.
 *
 *   1. Localizar el inicio de la línea n y cuántos bytes ocupa (texto + '\n').
 *   2. Reservar con malloc(3) un búfer del tamaño de la COLA (lo que va después).
 *   3. Leer la cola completa a memoria.
 *   4. Reposicionar el cursor al inicio de la línea n y escribir la cola encima,
 *      desplazando los bytes posteriores hacia atrás.
 *   5. ftruncate(2) para recortar los bytes sobrantes que quedaron al final.
 *
 * El paso 5 es imprescindible: sin él quedaría un residuo duplicado de la cola al
 * final del archivo. Y la cola se lee ENTERA a memoria antes de escribir porque las
 * regiones de origen y destino se solapan: escribir mientras se lee corrompería datos
 * que todavía no se han leído.
 */
static int ed_cmd_d(Editor *ed, long n) {
    if (!ed_requiere_archivo(ed)) return 1;

    long total = ed_contar_lineas(ed);
    if (total < 0) return 1;
    if (total == 0) { ed_aviso("El archivo está vacío: no hay nada que borrar."); return 1; }
    if (n < 1 || n > total) {
        ed_aviso("Línea %ld fuera de rango (el archivo tiene %ld línea(s)).", n, total);
        return 1;
    }

    off_t inicio = ed_offset_linea(ed, n);
    if (inicio == -1) { ed_aviso("No se pudo localizar la línea %ld.", n); return 1; }

    size_t len; int tiene_nl;
    if (ed_longitud_linea(ed, inicio, &len, &tiene_nl) == -1) return 1;
    size_t ocupa = len + (size_t)tiene_nl;      /* Bytes que la línea ocupa en disco */

    off_t tam = ed_tam_archivo(ed);
    if (tam == -1) return 1;

    off_t  cola_inicio = inicio + (off_t)ocupa;
    size_t cola_len    = (size_t)(tam - cola_inicio);

    /* Pasos 2 y 3: llevar la cola a memoria dinámica */
    char *cola = ed_leer_rango(ed, cola_inicio, cola_len);
    if (cola == NULL) return 1;

    /* Paso 4: desplazar la cola hacia atrás, encima de la línea borrada */
    if (ed_seek(ed, inicio, SEEK_SET, "SEEK_SET") == -1) { free(cola); return 1; }
    if (cola_len > 0 && ed_escribir_todo(ed, ed->fd, cola, cola_len) == -1) {
        ed_error("write"); free(cola); return 1;
    }
    free(cola);

    /* Paso 5: recortar el archivo a su nuevo tamaño real */
    off_t nuevo_tam = inicio + (off_t)cola_len;
    TRAZA(ed, "ftruncate", "%d, %ld", ed->fd, (long)nuevo_tam);
    if (ftruncate(ed->fd, nuevo_tam) == -1) {
        TRAZA_ERR(ed); ed_error("ftruncate"); return 1;
    }
    TRAZA_OK(ed, 0);

    printf(COLOR_RESULT "Línea %ld borrada" COLOR_RESET
           " (%zu byte(s); el archivo pasa de %ld a %ld bytes).\n",
           n, ocupa, (long)tam, (long)nuevo_tam);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: i [n] [texto]   -- Inserción arbitraria (requisito de equipos de 2+)
 * ====================================================================================
 * Syscalls: lseek(2), read(2), write(2), malloc(3)/free(3).
 *
 * Es la operación inversa de 'd': en lugar de compactar, abre hueco.
 *
 *   1. Localizar el inicio de la línea n (la línea que quedará DESPLAZADA hacia abajo).
 *   2. Leer a memoria toda la cola desde ese punto.
 *   3. Escribir el texto nuevo + '\n' en esa posición.
 *   4. Escribir la cola inmediatamente después.
 *
 * Aquí NO hace falta ftruncate(2): el archivo sólo crece, nunca quedan bytes residuales.
 * Igual que en 'd', la cola debe estar completa en memoria antes de escribir nada,
 * porque el texto nuevo pisa exactamente los bytes que hay que conservar.
 *
 * Se admite n == total+1 como "insertar justo después de la última línea", lo que hace
 * que 'i' degenere elegantemente en 'a' sin necesitar un comando aparte.
 */
static int ed_cmd_i(Editor *ed, long n, const char *texto) {
    if (!ed_requiere_archivo(ed)) return 1;
    if (texto == NULL) texto = "";

    long total = ed_contar_lineas(ed);
    if (total < 0) return 1;

    if (n < 1 || n > total + 1) {
        ed_aviso("Línea %ld fuera de rango (puede insertar entre 1 y %ld).", n, total + 1);
        return 1;
    }

    size_t len = strlen(texto);

    /* Caso degenerado: insertar más allá de la última línea equivale a añadir al final */
    if (n == total + 1) {
        if (ed_asegurar_nl_final(ed) == -1) return 1;
        if (ed_seek(ed, 0, SEEK_END, "SEEK_END") == -1) return 1;
        if (ed_escribir_todo(ed, ed->fd, texto, len) == -1) { ed_error("write"); return 1; }
        if (ed_escribir_todo(ed, ed->fd, "\n", 1)   == -1) { ed_error("write"); return 1; }
        printf(COLOR_RESULT "Insertada línea %ld" COLOR_RESET " (al final del archivo).\n", n);
        return 0;
    }

    off_t inicio = ed_offset_linea(ed, n);
    if (inicio == -1) { ed_aviso("No se pudo localizar la línea %ld.", n); return 1; }

    off_t tam = ed_tam_archivo(ed);
    if (tam == -1) return 1;
    size_t cola_len = (size_t)(tam - inicio);

    /* Paso 2: la cola completa a memoria dinámica antes de tocar el disco */
    char *cola = ed_leer_rango(ed, inicio, cola_len);
    if (cola == NULL) return 1;

    /* Pasos 3 y 4: texto nuevo y luego la cola desplazada */
    if (ed_seek(ed, inicio, SEEK_SET, "SEEK_SET") == -1) { free(cola); return 1; }
    if (ed_escribir_todo(ed, ed->fd, texto, len) == -1) { ed_error("write"); free(cola); return 1; }
    if (ed_escribir_todo(ed, ed->fd, "\n", 1)   == -1) { ed_error("write"); free(cola); return 1; }
    if (cola_len > 0 && ed_escribir_todo(ed, ed->fd, cola, cola_len) == -1) {
        ed_error("write"); free(cola); return 1;
    }
    free(cola);

    printf(COLOR_RESULT "Insertada línea %ld" COLOR_RESET
           " (%zu byte(s) desplazado(s) hacia abajo).\n", n, cola_len);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: s [palabra]   -- Búsqueda simple (requisito de equipos de 2+)
 * ====================================================================================
 * Syscalls: lseek(2), read(2). Memoria: malloc(3)/realloc(3)/free(3).
 *
 * Recorre el archivo UNA sola vez en bloques, acumulando la línea en curso en un búfer
 * dinámico que crece por duplicación con realloc(3). Al llegar a cada '\n' comprueba
 * si la línea contiene la palabra.
 *
 * La alternativa ingenua sería pedir la línea k con ed_offset_linea() para k = 1..N,
 * pero cada consulta recorre el archivo desde el principio: eso da O(N^2) lecturas.
 * Este recorrido en streaming es O(N) y es la razón de usar un búfer que crece.
 */
static int ed_cmd_s(Editor *ed, const char *palabra) {
    if (!ed_requiere_archivo(ed)) return 1;
    if (palabra == NULL || palabra[0] == '\0') {
        ed_aviso("Uso: s <palabra>");
        return 1;
    }

    char   bloque[ED_BLOQUE];
    size_t cap  = 256;                       /* Capacidad inicial del búfer de línea */
    size_t len  = 0;                         /* Bytes usados de la línea en curso    */
    char  *linea = malloc(cap);
    if (linea == NULL) { ed_error("malloc"); return 1; }

    long num = 1, hallazgos = 0;
    ssize_t r;
    int fallo = 0;

    if (ed_seek(ed, 0, SEEK_SET, "SEEK_SET") == -1) { free(linea); return 1; }

    printf(COLOR_TITLE "--- Coincidencias de \"%s\" ---\n" COLOR_RESET, palabra);

    while (!fallo) {
        TRAZA(ed, "read", "%d, bloque, %d", ed->fd, ED_BLOQUE);
        r = read(ed->fd, bloque, sizeof(bloque));
        if (r == -1) {
            if (errno == EINTR) continue;
            TRAZA_ERR(ed); ed_error("read"); fallo = 1; break;
        }
        TRAZA_OK(ed, r);
        if (r == 0) break;                   /* EOF */

        for (ssize_t k = 0; k < r; k++) {
            if (bloque[k] == '\n') {
                linea[len] = '\0';           /* Cerrar la línea para poder usar strstr */
                if (strstr(linea, palabra) != NULL) {
                    printf(COLOR_PARAM "%4ld" COLOR_RESET " | ", num);
                    ed_escribir_stdout(ed, linea, len);
                    ed_escribir_stdout(ed, "\n", 1);
                    hallazgos++;
                }
                num++;
                len = 0;                     /* Reiniciar para la siguiente línea */
            } else {
                /* Crecer el búfer por duplicación: +1 para el '\0' terminador */
                if (len + 1 >= cap) {
                    size_t nueva_cap = cap * 2;
                    char *tmp = realloc(linea, nueva_cap);
                    if (tmp == NULL) { ed_error("realloc"); fallo = 1; break; }
                    linea = tmp;
                    cap   = nueva_cap;
                }
                linea[len++] = bloque[k];
            }
        }
    }

    /* Última línea sin '\n' final: también se examina */
    if (!fallo && len > 0) {
        linea[len] = '\0';
        if (strstr(linea, palabra) != NULL) {
            printf(COLOR_PARAM "%4ld" COLOR_RESET " | ", num);
            ed_escribir_stdout(ed, linea, len);
            ed_escribir_stdout(ed, "\n", 1);
            hallazgos++;
        }
    }
    free(linea);
    if (fallo) return 1;

    if (hallazgos == 0) printf(COLOR_INFO "Sin coincidencias.\n" COLOR_RESET);
    else printf(COLOR_RESULT "%ld línea(s) coinciden.\n" COLOR_RESET, hallazgos);
    printf(COLOR_TITLE "----------------------------------------\n" COLOR_RESET);
    return 0;
}

/**
 * Traduce el campo st_mode a la cadena de permisos estilo 'ls -l' (ej: -rw-r--r--).
 * 'destino' debe tener espacio para al menos 11 caracteres.
 */
static void ed_formatear_permisos(mode_t modo, char *destino) {
    destino[0] = S_ISDIR(modo)  ? 'd' :
                 S_ISLNK(modo)  ? 'l' :
                 S_ISCHR(modo)  ? 'c' :
                 S_ISBLK(modo)  ? 'b' :
                 S_ISFIFO(modo) ? 'p' :
                 S_ISSOCK(modo) ? 's' : '-';
    destino[1] = (modo & S_IRUSR) ? 'r' : '-';
    destino[2] = (modo & S_IWUSR) ? 'w' : '-';
    destino[3] = (modo & S_IXUSR) ? 'x' : '-';
    destino[4] = (modo & S_IRGRP) ? 'r' : '-';
    destino[5] = (modo & S_IWGRP) ? 'w' : '-';
    destino[6] = (modo & S_IXGRP) ? 'x' : '-';
    destino[7] = (modo & S_IROTH) ? 'r' : '-';
    destino[8] = (modo & S_IWOTH) ? 'w' : '-';
    destino[9] = (modo & S_IXOTH) ? 'x' : '-';
    destino[10] = '\0';
}

/**
 * ====================================================================================
 * COMANDO: m   -- Metadatos del archivo (requisito de equipos de 3)
 * ====================================================================================
 * Syscall: fstat(2).
 *
 * Por qué fstat(2) y no stat(2): stat(2) resuelve una RUTA, fstat(2) consulta el
 * DESCRIPTOR ya abierto. La diferencia importa aquí: si alguien renombra o borra el
 * archivo mientras el editor lo tiene abierto, stat("ruta") fallaría o describiría un
 * archivo distinto, mientras que fstat(fd) sigue describiendo exactamente el inodo que
 * el editor está editando. El descriptor es una referencia al inodo, no al nombre.
 * (El shell base usa stat(2) en d_info porque allí no hay ningún archivo abierto.)
 *
 * Todos los datos mostrados viven en el INODO, no en el contenido del archivo.
 */
static int ed_cmd_m(Editor *ed) {
    if (!ed_requiere_archivo(ed)) return 1;

    struct stat st;
    TRAZA(ed, "fstat", "%d, &st", ed->fd);
    if (fstat(ed->fd, &st) == -1) {
        TRAZA_ERR(ed); ed_error("fstat"); return 1;
    }
    TRAZA_OK(ed, 0);

    /* Se cuenta antes de imprimir: si no, la traza de estas lecturas partiria
       el reporte de metadatos por la mitad. */
    long lineas = ed_contar_lineas(ed);

    char permisos[11];
    ed_formatear_permisos(st.st_mode, permisos);

    struct passwd *pw = getpwuid(st.st_uid);
    struct group  *gr = getgrgid(st.st_gid);

    printf(COLOR_TITLE "--- Metadatos de '%s' (fstat sobre fd=%d) ---\n" COLOR_RESET,
           ed->ruta, ed->fd);
    printf("  Inodo:            " COLOR_PARAM "%ld" COLOR_RESET "\n", (long)st.st_ino);
    printf("  Tamaño:           " COLOR_RESULT "%ld byte(s)" COLOR_RESET "\n", (long)st.st_size);
    printf("  Permisos:         " COLOR_PARAM "%s (%04o)" COLOR_RESET "\n",
           permisos, st.st_mode & 07777);
    printf("  Modificación:     %s", ctime(&st.st_mtime));   /* ctime ya añade '\n' */
    printf("  Último acceso:    %s", ctime(&st.st_atime));
    printf("  Cambio de inodo:  %s", ctime(&st.st_ctime));
    printf("  Enlaces (nlink):  %ld\n", (long)st.st_nlink);
    printf("  Propietario:      %ld (%s)\n", (long)st.st_uid, pw ? pw->pw_name : "?");
    printf("  Grupo:            %ld (%s)\n", (long)st.st_gid, gr ? gr->gr_name : "?");
    printf("  Bloques de 512B:  %ld\n", (long)st.st_blocks);
    printf("  Bloque optimo E/S: %ld byte(s)\n", (long)st.st_blksize);
    printf("  Líneas (contadas): " COLOR_PARAM "%ld" COLOR_RESET "\n", lineas);
    printf(COLOR_TITLE "----------------------------------------\n" COLOR_RESET);
    return 0;
}

/**
 * ====================================================================================
 * PORTAPAPELES SECUENCIAL LOCAL (requisito de equipos de 3)
 * ====================================================================================
 * Estructura elegida: arreglo dinámico de cadenas, con crecimiento por duplicación.
 *
 * Por qué un arreglo y no una lista enlazada: el portapapeles se recorre siempre
 * completo y en orden al pegar, nunca se inserta ni se borra por el medio. Un arreglo
 * da localidad de memoria y un free() trivial; la lista enlazada pagaría un puntero
 * por línea sin aportar nada.
 *
 * Es "secuencial" porque 'y' ACUMULA: cada copia se añade al final, de modo que se
 * pueden recoger varias líneas sueltas del archivo y pegarlas juntas como un bloque.
 */

/* Añade una copia de 'texto' al final del portapapeles. Retorna 0 en éxito, -1 si no. */
static int ed_pp_agregar(Portapapeles *pp, const char *texto, size_t len) {
    if (pp->n == pp->cap) {
        size_t nueva_cap = (pp->cap == 0) ? 8 : pp->cap * 2;
        char **tmp = realloc(pp->lineas, nueva_cap * sizeof(char *));
        if (tmp == NULL) { ed_error("realloc"); return -1; }
        pp->lineas = tmp;
        pp->cap    = nueva_cap;
    }
    char *copia = malloc(len + 1);
    if (copia == NULL) { ed_error("malloc"); return -1; }
    memcpy(copia, texto, len);
    copia[len] = '\0';

    pp->lineas[pp->n++] = copia;
    return 0;
}

/* Libera todas las líneas y el arreglo. Deja el portapapeles utilizable y vacío. */
static void ed_pp_limpiar(Portapapeles *pp) {
    for (size_t k = 0; k < pp->n; k++) free(pp->lineas[k]);
    free(pp->lineas);
    pp->lineas = NULL;
    pp->n = 0;
    pp->cap = 0;
}

/* Muestra el contenido actual del portapapeles ('y' sin argumentos). */
static void ed_pp_mostrar(const Portapapeles *pp) {
    if (pp->n == 0) {
        printf(COLOR_INFO "El portapapeles está vacío.\n" COLOR_RESET);
        return;
    }
    printf(COLOR_TITLE "--- Portapapeles (%zu línea(s)) ---\n" COLOR_RESET, pp->n);
    for (size_t k = 0; k < pp->n; k++) {
        printf(COLOR_PARAM "%4zu" COLOR_RESET " | %s\n", k + 1, pp->lineas[k]);
    }
    printf(COLOR_TITLE "----------------------------------------\n" COLOR_RESET);
}

/**
 * ====================================================================================
 * COMANDO: y [n]   -- COPIAR la línea n al portapapeles
 * ====================================================================================
 * Syscalls: lseek(2), read(2). El archivo NO se modifica: 'y' es de sólo lectura.
 * La línea se guarda sin su '\n'; el salto se vuelve a añadir al pegar con 'x'.
 *
 * Extensiones documentadas:  'y'   sin argumentos muestra el portapapeles.
 *                            'y -' lo vacía y libera su memoria.
 */
static int ed_cmd_y(Editor *ed, long n) {
    if (!ed_requiere_archivo(ed)) return 1;

    long total = ed_contar_lineas(ed);
    if (total < 0) return 1;
    if (n < 1 || n > total) {
        ed_aviso("Línea %ld fuera de rango (el archivo tiene %ld línea(s)).", n, total);
        return 1;
    }

    off_t inicio = ed_offset_linea(ed, n);
    if (inicio == -1) { ed_aviso("No se pudo localizar la línea %ld.", n); return 1; }

    size_t len; int tiene_nl;
    if (ed_longitud_linea(ed, inicio, &len, &tiene_nl) == -1) return 1;

    char *linea = ed_leer_rango(ed, inicio, len);
    if (linea == NULL) return 1;

    int r = ed_pp_agregar(&ed->pp, linea, len);
    free(linea);
    if (r == -1) return 1;

    printf(COLOR_RESULT "Línea %ld copiada" COLOR_RESET
           " (el portapapeles tiene ahora %zu línea(s)).\n", n, ed->pp.n);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: x [n]   -- PEGAR el portapapeles en la línea n
 * ====================================================================================
 * Syscalls: lseek(2), read(2), write(2). Memoria: malloc(3)/free(3).
 *
 * Inserta TODAS las líneas del portapapeles, en el orden en que se copiaron, a partir
 * de la línea n, desplazando hacia abajo lo que había. Igual que 'i' pero con un bloque
 * de varias líneas.
 *
 * Detalle de diseño: primero se arma en memoria UN solo búfer con todas las líneas ya
 * separadas por '\n', y se hace UNA sola escritura del bloque. Escribir línea por línea
 * obligaría a releer y desplazar la cola una vez por línea, con costo cuadrático.
 *
 * El portapapeles NO se consume al pegar: se puede pegar el mismo bloque varias veces.
 * Se vacía explícitamente con 'y -'.
 */
static int ed_cmd_x(Editor *ed, long n) {
    if (!ed_requiere_archivo(ed)) return 1;

    if (ed->pp.n == 0) {
        ed_aviso("El portapapeles está vacío. Copie primero con: y <n>");
        return 1;
    }

    long total = ed_contar_lineas(ed);
    if (total < 0) return 1;
    if (n < 1 || n > total + 1) {
        ed_aviso("Línea %ld fuera de rango (puede pegar entre 1 y %ld).", n, total + 1);
        return 1;
    }

    /* 1. Armar en memoria el bloque completo a insertar: línea + '\n' por cada entrada */
    size_t bloque_len = 0;
    for (size_t k = 0; k < ed->pp.n; k++) bloque_len += strlen(ed->pp.lineas[k]) + 1;

    char *bloque = malloc(bloque_len);
    if (bloque == NULL) { ed_error("malloc"); return 1; }

    size_t off = 0;
    for (size_t k = 0; k < ed->pp.n; k++) {
        size_t l = strlen(ed->pp.lineas[k]);
        memcpy(bloque + off, ed->pp.lineas[k], l);
        off += l;
        bloque[off++] = '\n';
    }

    /* 2. Caso "pegar al final": no hay cola que desplazar */
    if (n == total + 1) {
        if (ed_asegurar_nl_final(ed) == -1)            { free(bloque); return 1; }
        if (ed_seek(ed, 0, SEEK_END, "SEEK_END") == -1) { free(bloque); return 1; }
        if (ed_escribir_todo(ed, ed->fd, bloque, bloque_len) == -1) {
            ed_error("write"); free(bloque); return 1;
        }
        free(bloque);
        printf(COLOR_RESULT "Pegadas %zu línea(s)" COLOR_RESET " al final del archivo.\n",
               ed->pp.n);
        return 0;
    }

    /* 3. Caso general: leer la cola, escribir el bloque y volver a escribir la cola */
    off_t inicio = ed_offset_linea(ed, n);
    if (inicio == -1) { ed_aviso("No se pudo localizar la línea %ld.", n); free(bloque); return 1; }

    off_t tam = ed_tam_archivo(ed);
    if (tam == -1) { free(bloque); return 1; }
    size_t cola_len = (size_t)(tam - inicio);

    char *cola = ed_leer_rango(ed, inicio, cola_len);
    if (cola == NULL) { free(bloque); return 1; }

    if (ed_seek(ed, inicio, SEEK_SET, "SEEK_SET") == -1) { free(bloque); free(cola); return 1; }
    if (ed_escribir_todo(ed, ed->fd, bloque, bloque_len) == -1) {
        ed_error("write"); free(bloque); free(cola); return 1;
    }
    if (cola_len > 0 && ed_escribir_todo(ed, ed->fd, cola, cola_len) == -1) {
        ed_error("write"); free(bloque); free(cola); return 1;
    }
    free(bloque);
    free(cola);

    printf(COLOR_RESULT "Pegadas %zu línea(s)" COLOR_RESET
           " a partir de la línea %ld (%zu byte(s) desplazado(s)).\n",
           ed->pp.n, n, cola_len);
    return 0;
}

/**
 * ====================================================================================
 * CICLO DE VIDA DE LA SESIÓN
 * ====================================================================================
 */

/* Deja el estado del editor en limpio. No reserva nada todavía. */
static void ed_init(Editor *ed) {
    ed->fd      = -1;
    ed->abierto = 0;
    ed->trazar  = 0;            /* La traza de syscalls arranca apagada */
    ed->ruta[0] = '\0';
    ed->pp.lineas = NULL;
    ed->pp.n      = 0;
    ed->pp.cap    = 0;
}

/**
 * Libera TODOS los recursos de la sesión: el descriptor con close(2) y toda la memoria
 * dinámica del portapapeles con free(3). Es idempotente, así que puede llamarse en
 * cualquier camino de salida sin riesgo de doble liberación.
 */
static void ed_liberar(Editor *ed) {
    if (ed->abierto && ed->fd >= 0) {
        TRAZA(ed, "close", "%d", ed->fd);
        if (close(ed->fd) == -1) { TRAZA_ERR(ed); ed_error("close"); }
        else                     { TRAZA_OK(ed, 0); }
    }
    ed->fd      = -1;
    ed->abierto = 0;
    ed_pp_limpiar(&ed->pp);
}

/* Ayuda del editor: se muestra con 'h' o '?'. */
static void ed_ayuda(void) {
    printf(COLOR_TITLE "\n--- Editor de texto CLI (comandos) ---\n" COLOR_RESET);
    printf("  " COLOR_PROMPT "o <archivo>" COLOR_RESET "     Abre el archivo; lo crea si no existe.  " COLOR_INFO "open" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "p [n]" COLOR_RESET "           Imprime la línea n, o todo el archivo.  " COLOR_INFO "read/lseek/write" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "a <texto>" COLOR_RESET "       Añade el texto como última línea.       " COLOR_INFO "lseek/write" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "d <n>" COLOR_RESET "           Borra la línea n y compacta el archivo. " COLOR_INFO "read/write/ftruncate" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "i <n> <texto>" COLOR_RESET "   Inserta el texto en la línea n.         " COLOR_INFO "lseek/read/write" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "s <palabra>" COLOR_RESET "     Busca la palabra y lista coincidencias. " COLOR_INFO "read" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "m" COLOR_RESET "               Metadatos del inodo del archivo.        " COLOR_INFO "fstat" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "y <n>" COLOR_RESET "           Copia la línea n al portapapeles.       " COLOR_INFO "lseek/read" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "y" COLOR_RESET "               Muestra el portapapeles.\n");
    printf("  " COLOR_PROMPT "y -" COLOR_RESET "             Vacía el portapapeles.\n");
    printf("  " COLOR_PROMPT "x <n>" COLOR_RESET "           Pega el portapapeles en la línea n.     " COLOR_INFO "lseek/read/write" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "t" COLOR_RESET "               Activa/desactiva la traza de syscalls.\n");
    printf("  " COLOR_PROMPT "h" COLOR_RESET " o " COLOR_PROMPT "?" COLOR_RESET "           Muestra esta ayuda.\n");
    printf("  " COLOR_PROMPT "q" COLOR_RESET "               Cierra el archivo y sale del editor.    " COLOR_INFO "close" COLOR_RESET "\n");
    printf(COLOR_INFO "\n  Las líneas se numeran desde 1. El texto de 'a' e 'i' se toma literal\n");
    printf("  hasta el fin de la línea, con o sin comillas.\n" COLOR_RESET);
}

/**
 * ====================================================================================
 * ANALIZADOR DE LA LÍNEA DE COMANDOS DEL EDITOR
 * ====================================================================================
 * DECISIÓN DE DISEÑO: el editor NO reutiliza el tokenizador del shell (parse_line).
 *
 * El tokenizador del shell parte la línea por espacios y exige comillas para agrupar,
 * lo cual es correcto para un shell pero pésimo para un editor: obligaría a escribir
 *     a "hola mundo"
 * y volvería imposible añadir una línea que contenga comillas dobles.
 *
 * Aquí se separa sólo el comando (y el número, cuando el comando lo lleva) y el RESTO
 * de la línea se toma literalmente como texto. Es el mismo criterio de 'ed'.
 */

/**
 * Separador de tokens. Incluye '\n' y '\r' porque fgets(3) conserva el salto de
 * linea al final de lo leido: sin contemplarlo, el comando "p" llegaria como "p\n"
 * y ningun strcmp lo reconoceria.
 */
static int ed_es_blanco(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Elimina espacios en blanco al inicio y al final, modificando la cadena in situ. */
static char *ed_trim(char *s) {
    while (ed_es_blanco(*s)) s++;
    if (*s == '\0') return s;

    char *fin = s + strlen(s) - 1;
    while (fin > s && ed_es_blanco(*fin)) {
        *fin-- = '\0';
    }
    if (ed_es_blanco(*fin)) *fin = '\0';
    return s;
}

/* Si la cadena está enteramente entre comillas dobles, se las quita. */
static char *ed_quitar_comillas(char *s) {
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        return s + 1;
    }
    return s;
}

/**
 * Extrae el primer token de 'linea' (el comando) y devuelve por 'resto' un puntero al
 * primer carácter no blanco que le sigue. Modifica 'linea' insertando un '\0'.
 */
static char *ed_tomar_comando(char *linea, char **resto) {
    char *p = linea;
    while (ed_es_blanco(*p)) p++;
    char *cmd = p;

    while (*p && !ed_es_blanco(*p)) p++;
    if (*p != '\0') {
        *p++ = '\0';
        while (ed_es_blanco(*p)) p++;
    }
    *resto = p;
    return cmd;
}

/**
 * Extrae un número entero del inicio de 'resto' y avanza el puntero más allá de él.
 * Retorna 1 si encontró un número válido, 0 si no.
 */
static int ed_tomar_numero(char **resto, long *valor) {
    char *p = *resto;
    char *fin = NULL;

    if (*p == '\0') return 0;
    *valor = strtol(p, &fin, 10);
    if (fin == p) return 0;                 /* No había ningún dígito */

    while (ed_es_blanco(*fin)) fin++;
    *resto = fin;
    return 1;
}

/**
 * ====================================================================================
 * BUCLE INTERACTIVO DEL EDITOR (sub-REPL)
 * ====================================================================================
 * Este bucle se ejecuta DENTRO del proceso del shell cuando el editor se invoca como
 * comando integrado, y como bucle principal cuando se ejecuta el binario 'edi'.
 * En ambos casos el código es exactamente el mismo.
 *
 * Retorna 0 al salir con 'q' o con EOF (Ctrl+D).
 */
static int ed_repl(Editor *ed) {
    char linea[ED_LINEA_MAX];

    while (1) {
        printf(COLOR_PROMPT "edi[%s]> " COLOR_RESET,
               ed->abierto ? ed->ruta : "sin archivo");
        fflush(stdout);

        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            printf("\n" COLOR_INFO "EOF recibido. Cerrando el editor.\n" COLOR_RESET);
            break;
        }

        char *resto = NULL;
        char *cmd   = ed_tomar_comando(linea, &resto);
        resto = ed_trim(resto);

        if (cmd[0] == '\0') continue;                  /* Línea vacía: ignorar */

        /* --- q : cerrar y salir ------------------------------------------------ */
        if (strcmp(cmd, "q") == 0) {
            printf(COLOR_INFO "Cerrando el archivo y saliendo del editor.\n" COLOR_RESET);
            break;
        }
        /* --- h / ? : ayuda ----------------------------------------------------- */
        if (strcmp(cmd, "h") == 0 || strcmp(cmd, "?") == 0) { ed_ayuda(); continue; }

        /* --- t : conmutar la traza de syscalls --------------------------------- */
        if (strcmp(cmd, "t") == 0) {
            ed->trazar = !ed->trazar;
            printf(COLOR_INFO "Traza de syscalls: %s\n" COLOR_RESET,
                   ed->trazar ? "ACTIVADA" : "desactivada");
            continue;
        }
        /* --- o <archivo> ------------------------------------------------------- */
        if (strcmp(cmd, "o") == 0) { ed_cmd_o(ed, ed_quitar_comillas(resto)); continue; }

        /* --- m : metadatos ----------------------------------------------------- */
        if (strcmp(cmd, "m") == 0) { ed_cmd_m(ed); continue; }

        /* --- p [n] ------------------------------------------------------------- */
        if (strcmp(cmd, "p") == 0) {
            long n = 0;
            if (resto[0] == '\0')            ed_cmd_p(ed, 0, 1);       /* todo */
            else if (ed_tomar_numero(&resto, &n)) ed_cmd_p(ed, n, 0);  /* una línea */
            else ed_aviso("Uso: p [n]  (n debe ser un número de línea)");
            continue;
        }
        /* --- a <texto> --------------------------------------------------------- */
        if (strcmp(cmd, "a") == 0) { ed_cmd_a(ed, ed_quitar_comillas(resto)); continue; }

        /* --- s <palabra> ------------------------------------------------------- */
        if (strcmp(cmd, "s") == 0) { ed_cmd_s(ed, ed_quitar_comillas(resto)); continue; }

        /* --- d <n> ------------------------------------------------------------- */
        if (strcmp(cmd, "d") == 0) {
            long n;
            if (ed_tomar_numero(&resto, &n)) ed_cmd_d(ed, n);
            else ed_aviso("Uso: d <n>");
            continue;
        }
        /* --- i <n> <texto> ----------------------------------------------------- */
        if (strcmp(cmd, "i") == 0) {
            long n;
            if (ed_tomar_numero(&resto, &n)) ed_cmd_i(ed, n, ed_quitar_comillas(resto));
            else ed_aviso("Uso: i <n> <texto>");
            continue;
        }
        /* --- y [n] | y - ------------------------------------------------------- */
        if (strcmp(cmd, "y") == 0) {
            long n;
            if (resto[0] == '\0')      ed_pp_mostrar(&ed->pp);
            else if (resto[0] == '-' && resto[1] == '\0') {
                ed_pp_limpiar(&ed->pp);
                printf(COLOR_INFO "Portapapeles vaciado.\n" COLOR_RESET);
            }
            else if (ed_tomar_numero(&resto, &n)) ed_cmd_y(ed, n);
            else ed_aviso("Uso: y <n> | y | y -");
            continue;
        }
        /* --- x <n> ------------------------------------------------------------- */
        if (strcmp(cmd, "x") == 0) {
            long n;
            if (ed_tomar_numero(&resto, &n)) ed_cmd_x(ed, n);
            else ed_aviso("Uso: x <n>");
            continue;
        }

        ed_aviso("Comando '%s' no reconocido. Escriba 'h' para ver la ayuda.", cmd);
    }
    return 0;
}

/**
 * Punto de entrada único del editor. Lo usan por igual el comando integrado del shell
 * y el binario independiente 'edi', de modo que ambos ejecutan EXACTAMENTE el mismo
 * código: no hay dos versiones del editor que puedan divergir.
 *
 * 'ruta_inicial' puede ser NULL (arrancar sin archivo abierto).
 */
int editor_main(const char *ruta_inicial) {
    Editor ed;
    ed_init(&ed);

    /* Misma razon que en el shell (ver main.c): sin bufer, el editor consume solo sus
     * propias lineas y deja intacto lo que siga en la tuberia para el proceso padre. */
    setvbuf(stdin, NULL, _IONBF, 0);

    printf(COLOR_TITLE "\n========================================================\n" COLOR_RESET);
    printf(COLOR_TITLE "   Editor de Texto CLI sobre llamadas al sistema POSIX\n" COLOR_RESET);
    printf(COLOR_INFO  "   Escriba 'h' para la ayuda, 'q' para salir.\n" COLOR_RESET);
    printf(COLOR_TITLE "========================================================\n\n" COLOR_RESET);

    if (ruta_inicial != NULL && ruta_inicial[0] != '\0') {
        ed_cmd_o(&ed, ruta_inicial);
    } else {
        printf(COLOR_INFO "No se indicó archivo. Abra uno con: o <archivo>\n" COLOR_RESET);
    }

    int r = ed_repl(&ed);

    /* Salida limpia: close(2) del descriptor y free(3) de todo el portapapeles */
    ed_liberar(&ed);
    return r;
}

/**
 * ====================================================================================
 * INTEGRACIÓN CON EL SHELL — ESTRATEGIA 1 (principal): EN PROCESO
 * ====================================================================================
 * Comando del shell:  edit [archivo]     (categoría "edicion")
 *
 * El editor corre DENTRO del proceso del shell: es una simple llamada a función que
 * abre su propio sub-REPL y retorna cuando el usuario escribe 'q'.
 *
 * Por qué esta es la estrategia principal:
 *   - El contrato de la tabla de comandos del shell, int (*handler)(int, char**), no
 *     impone ninguna duración: un handler puede leer STDIN y ciclar. No hubo que
 *     modificar ni el REPL del shell ni su tokenizador para integrarlo.
 *   - El archivo se abre en la MISMA tabla de descriptores del proceso shell, lo que
 *     se puede comprobar en vivo con 'm_info' o 'p_monitor' antes y después.
 *   - Se reutiliza el trazado LOG_SYSCALL del shell, manteniendo una sola estética
 *     pedagógica en todo el proyecto.
 *
 * Contrapartida asumida: un fallo grave del editor se llevaría por delante al shell.
 * Por eso existe la estrategia 2, y por eso cada retorno de syscall se verifica.
 */
int cmd_edit(int argc, char **argv) {
    const char *ruta = (argc > 1) ? argv[1] : NULL;
    return editor_main(ruta);
}

/**
 * ====================================================================================
 * INTEGRACIÓN CON EL SHELL — ESTRATEGIA 2 (alternativa): PROCESO AISLADO
 * ====================================================================================
 * Comando del shell:  edit_ext [archivo]     (categoría "edicion")
 *
 * Syscalls: fork(2), execvp(3) sobre execve(2), waitpid(2).
 *
 * Aquí el shell se bifurca y el hijo REEMPLAZA su imagen de proceso por el binario
 * './edi'. Es el mismo mecanismo que usa 'p_exec' y que usa cualquier shell real.
 *
 * Existe para poder contrastar empíricamente las dos estrategias en la sustentación:
 *
 *   En proceso (edit)          | Proceso aislado (edit_ext)
 *   ---------------------------|------------------------------------------------
 *   Sin costo de creación      | fork + exec por invocación
 *   Comparte la tabla de FDs   | Tabla de FDs propia; el shell no ve el archivo
 *   Un segfault mata el shell  | Un segfault sólo mata al hijo; el shell sobrevive
 *   Sin binario extra          | Requiere './edi' compilado y accesible
 *
 * Ambas rutas ejecutan el mismo editor_main(), así que la comparación aísla justamente
 * el mecanismo de invocación y no diferencias de funcionalidad.
 */
int cmd_edit_ext(int argc, char **argv) {
    char *args[3];
    args[0] = (char *)"./edi";
    args[1] = (argc > 1) ? argv[1] : NULL;
    args[2] = NULL;

    /* Vaciar stdout ANTES de fork(2): lo que quede en el bufer del padre se duplicaria
     * en el hijo y se imprimiria dos veces al terminar cada proceso. */
    fflush(stdout);

    LOG_SYSCALL("fork", "");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* PROCESO HIJO: se convierte en el editor */
        execvp(args[0], args);

        /* Sólo se llega aquí si execvp falló */
        perror("execvp ./edi");
        fprintf(stderr, COLOR_ERROR
                "No se pudo ejecutar './edi'. Compílelo con 'make' y ejecute el shell "
                "desde el mismo directorio.\n" COLOR_RESET);
        _exit(127);                 /* _exit: no vaciar los búferes heredados del padre */
    }

    /* PROCESO PADRE: espera a que el editor termine */
    LOG_SYSCALL_RESULT(pid);
    printf(COLOR_INFO "Editor lanzado como proceso hijo (PID %d). Esperando...\n" COLOR_RESET, pid);

    int estado;
    LOG_SYSCALL("waitpid", "%d, &estado, 0", pid);
    pid_t terminado = waitpid(pid, &estado, 0);
    if (terminado == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        perror("waitpid");
        return 1;
    }
    LOG_SYSCALL_RESULT(terminado);

    if (WIFEXITED(estado)) {
        printf(COLOR_INFO "El editor (PID %d) terminó con código %d.\n" COLOR_RESET,
               terminado, WEXITSTATUS(estado));
        return WEXITSTATUS(estado);
    }
    if (WIFSIGNALED(estado)) {
        printf(COLOR_ERROR "El editor (PID %d) fue terminado por la señal %d.\n" COLOR_RESET,
               terminado, WTERMSIG(estado));
        printf(COLOR_INFO "Nótese que el shell sigue vivo: ésa es la ventaja del aislamiento.\n" COLOR_RESET);
    }
    return 1;
}
