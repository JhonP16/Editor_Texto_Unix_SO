# ======================================================================================
#  Makefile - Shell educativo (eafitOS) + Editor de texto CLI (edi)
# ======================================================================================
#  Targets:
#    make          / make all   -> compila AMBOS binarios (no ejecuta nada)
#    make run                   -> compila y lanza el shell interactivo
#    make run-edi               -> compila y lanza el editor independiente
#    make test                  -> compila y ejecuta el script de pruebas
#    make valgrind              -> corre el script de pruebas bajo valgrind
#    make asan                  -> corre las pruebas con AddressSanitizer/UBSan/LeakSanitizer
#    make clean                 -> borra binarios, objetos y residuos de pruebas
#
#  Nota: 'all' NO ejecuta el binario a propósito. El Makefile original lanzaba el shell
#  al terminar de enlazar, lo que impedía usar 'make' dentro de un script de pruebas o
#  de un pipeline. Ejecutar es responsabilidad del target 'run'.
# ======================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE

SHELL_BIN = eafitOS
EDI_BIN   = edi
EDI_ASAN  = edi_asan

SANFLAGS  = -fsanitize=address,undefined

# editor.c se comparte entre los dos binarios: un solo editor, dos puntos de entrada.
SRCS_COMUNES = editor.c
SRCS_SHELL   = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c $(SRCS_COMUNES)
SRCS_EDI     = edi_main.c $(SRCS_COMUNES)

OBJS_SHELL = $(SRCS_SHELL:.c=.o)
OBJS_EDI   = $(SRCS_EDI:.c=.o)
OBJS_TODOS = $(sort $(OBJS_SHELL) $(OBJS_EDI))

all: $(SHELL_BIN) $(EDI_BIN)

$(SHELL_BIN): $(OBJS_SHELL)
	$(CC) $(CFLAGS) -o $@ $^

$(EDI_BIN): $(OBJS_EDI)
	$(CC) $(CFLAGS) -o $@ $^

# Todo objeto depende de las dos cabeceras: cambiarlas fuerza la recompilación
%.o: %.c shell.h editor.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(SHELL_BIN)
	./$(SHELL_BIN)

run-edi: $(EDI_BIN)
	./$(EDI_BIN)

test: all
	./pruebas.sh

valgrind: all
	@command -v valgrind >/dev/null || 	  { echo "valgrind no esta instalado: sudo apt install valgrind (o use 'make asan')"; exit 1; }
	VALGRIND=1 ./pruebas.sh

# Alternativa a valgrind sin instalar nada: los sanitizers que trae GCC.
# Detectan fugas (LeakSanitizer), accesos invalidos (AddressSanitizer) y
# comportamiento indefinido (UBSan) durante toda la bateria de pruebas.
asan: $(EDI_ASAN)
	EDI=./$(EDI_ASAN) ./pruebas.sh

$(EDI_ASAN): $(SRCS_EDI) shell.h editor.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ $(SRCS_EDI)

clean:
	rm -f $(SHELL_BIN) $(EDI_BIN) $(EDI_ASAN) $(OBJS_TODOS)
	rm -rf pruebas_tmp

.PHONY: all run run-edi test valgrind asan clean
