# ======================================================================================
#  Makefile - Shell educativo (eafitOS) + Editor de texto CLI (editor)
# ======================================================================================
#  Targets:
#    make          / make all   -> compila AMBOS binarios (no ejecuta nada)
#    make run                   -> compila y lanza el shell interactivo
#    make run-editor            -> compila y lanza el editor independiente
#    make test                  -> compila y ejecuta el script de pruebas
#    make asan                  -> corre las pruebas con AddressSanitizer/UBSan/LeakSanitizer
#    make valgrind              -> corre el script de pruebas bajo valgrind
#    make clean                 -> borra binarios, objetos y residuos de pruebas
#
#  Nota: 'all' NO ejecuta el binario a proposito. El Makefile original lanzaba el shell
#  al terminar de enlazar, lo que impedia usar 'make' dentro de un script de pruebas o
#  de un pipeline. Ejecutar es responsabilidad del target 'run'.
# ======================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE

SHELL_BIN    = eafitOS
EDITOR_BIN   = editor
EDITOR_ASAN  = editor_asan

SANFLAGS = -fsanitize=address,undefined

# editor.c se comparte entre los dos binarios: un solo editor, dos puntos de entrada.
SRCS_COMUNES = editor.c
SRCS_SHELL   = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c $(SRCS_COMUNES)
SRCS_EDITOR  = main_editor.c $(SRCS_COMUNES)

OBJS_SHELL  = $(SRCS_SHELL:.c=.o)
OBJS_EDITOR = $(SRCS_EDITOR:.c=.o)
OBJS_TODOS  = $(sort $(OBJS_SHELL) $(OBJS_EDITOR))

all: $(SHELL_BIN) $(EDITOR_BIN)

$(SHELL_BIN): $(OBJS_SHELL)
	$(CC) $(CFLAGS) -o $@ $^

$(EDITOR_BIN): $(OBJS_EDITOR)
	$(CC) $(CFLAGS) -o $@ $^

# Todo objeto depende de las dos cabeceras: cambiarlas fuerza la recompilacion
%.o: %.c shell.h editor.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(SHELL_BIN)
	./$(SHELL_BIN)

run-editor: $(EDITOR_BIN)
	./$(EDITOR_BIN)

test: all
	./pruebas.sh

# Alternativa a valgrind sin instalar nada: los sanitizers que trae GCC.
# Detectan fugas (LeakSanitizer), accesos invalidos (AddressSanitizer) y
# comportamiento indefinido (UBSan) durante toda la bateria de pruebas.
asan: $(EDITOR_ASAN) $(SHELL_BIN)
	EDITOR_BIN=./$(EDITOR_ASAN) ./pruebas.sh

$(EDITOR_ASAN): $(SRCS_EDITOR) shell.h editor.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ $(SRCS_EDITOR)

valgrind: all
	@command -v valgrind >/dev/null || { echo "valgrind no esta instalado: sudo apt install valgrind (o use 'make asan')"; exit 1; }
	VALGRIND=1 ./pruebas.sh

clean:
	rm -f $(SHELL_BIN) $(EDITOR_BIN) $(EDITOR_ASAN) $(OBJS_TODOS)
	rm -rf pruebas_tmp

.PHONY: all run run-editor test asan valgrind clean
