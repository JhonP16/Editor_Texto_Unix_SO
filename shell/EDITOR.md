# Editor de Texto CLI sobre llamadas al sistema POSIX

Editor de texto interactivo estilo `ed`, escrito en C, integrado como una **categoría
nueva (`edicion`)** dentro del shell educativo de la asignatura.

Todo el acceso a disco se hace exclusivamente con llamadas al sistema:
`open(2)`, `read(2)`, `write(2)`, `lseek(2)`, `ftruncate(2)`, `fstat(2)`, `close(2)`.
No se usa `fopen`/`fread`/`fwrite`/`fclose` en ningún punto. La biblioteca estándar sólo
interviene para leer los comandos del usuario (`fgets`) e imprimir mensajes de consola
(`printf`); el **contenido del archivo siempre se emite con `write(2)` sobre el FD 1**.

---

## 1. Compilación y ejecución

```bash
make            # compila los dos binarios: eafitOS (shell) y editor (el editor independiente)
make run        # lanza el shell interactivo
make run-editor    # lanza el editor independiente
make test       # ejecuta la batería de pruebas (50 casos)
make asan       # ejecuta las pruebas bajo AddressSanitizer + UBSan + LeakSanitizer
make valgrind   # ejecuta las pruebas bajo valgrind (requiere tenerlo instalado)
make clean      # borra binarios, objetos y residuos de pruebas
```

> **Importante para WSL:** si el repositorio está en `/mnt/c/...`, ese montaje (DrvFs) no
> conserva permisos POSIX y todo archivo aparece con modo `0777`. El script de pruebas lo
> detecta solo y se traslada a `/tmp` para poder validar el modo `0644` de verdad.

### Dos formas de invocarlo

```bash
# 1. Integrado en el shell (estrategia principal: mismo proceso)
./eafitOS
eafitOS> edit notas.txt

# 2. Como binario independiente
./editor notas.txt

# 3. Integrado pero aislado en un proceso hijo (fork + execvp)
eafitOS> edit_ext notas.txt
```

---

## 2. Comandos del editor

| Comando | Descripción | Llamadas al sistema |
|---|---|---|
| `o <archivo>` | Abre el archivo; lo crea con modo `0644` si no existe | `open(O_RDWR\|O_CREAT)`, `close` |
| `p` | Imprime todo el archivo recorriendo bytes hasta cada `\n` | `lseek`, `read`, `write` (FD 1) |
| `p <n>` | Imprime únicamente la línea *n* | `lseek`, `read`, `write` (FD 1) |
| `a <texto>` | Añade el texto como nueva última línea | `lseek(SEEK_END)`, `write` |
| `d <n>` | Borra la línea *n*, desplaza la cola y trunca | `read`, `write`, `lseek`, `ftruncate` |
| `i <n> <texto>` | Inserta el texto en la línea *n* desplazando el resto | `lseek`, `read`, `write` |
| `s <palabra>` | Lista las líneas que contienen la palabra | `read` |
| `m` | Metadatos del inodo: tamaño, permisos, inodo, fechas | `fstat` |
| `y <n>` | **Copia** la línea *n* al portapapeles (acumula) | `lseek`, `read` |
| `x <n>` | **Pega** el portapapeles a partir de la línea *n* | `lseek`, `read`, `write` |
| `q` | Cierra el descriptor y sale sin fugas de memoria | `close` |

**Extensiones de conveniencia** (documentadas, fuera del requisito mínimo):

| Comando | Descripción |
|---|---|
| `y` | Muestra el contenido actual del portapapeles |
| `y -` | Vacía el portapapeles y libera su memoria |
| `t` | Activa/desactiva la traza de syscalls estilo `strace` |
| `h` o `?` | Ayuda dentro del editor |

Las líneas se numeran **desde 1**. El texto de `a` e `i` se toma **literal hasta el fin de
la línea**, con o sin comillas: `a hola mundo` funciona sin necesidad de comillar.

---

## 3. Decisiones de arquitectura

Esta sección es el material de base para el documento de sustentación en PDF.

### 3.1. Integración con el shell: ¿categoría existente o categoría nueva?

**Decisión: se creó la categoría nueva `edicion`.**

El shell de clase organiza sus comandos en una tabla `Command` (`shell.h`) con cuatro
categorías: `datos`, `memoria`, `monitoreo` y `utilidades`. A primera vista el editor
encajaría en `datos`, puesto que usa `open`, `read` y `write` igual que `d_create` o
`d_read`. Ese razonamiento clasifica por **familia de syscalls**, y es el que se descartó.

Al analizar la arquitectura del shell aparece una invariante más fuerte que la familia de
syscalls: **las cuatro categorías originales agrupan comandos de un solo disparo y sin
estado**. Cada handler ejecuta, imprime el resultado y retorna; no deja nada vivo entre
invocaciones, no conserva memoria y nunca lee de STDIN.

El editor rompe esa invariante en tres puntos simultáneos:

1. **Mantiene un descriptor de archivo abierto** entre muchos comandos sucesivos.
2. **Conserva estado en memoria dinámica** (el portapapeles) durante toda la sesión.
3. **Se apropia de STDIN** con su propio bucle de lectura de comandos (sub-REPL).

La diferencia, entonces, no es de familia de syscalls sino de **modelo de interacción**.
Meterlo en `datos` obligaría a que esa categoría dejara de significar "demostración
puntual de una syscall de archivos" y pasara a significar dos cosas distintas a la vez.
Crear `edicion` mantiene cada categoría con un significado único y deja la puerta abierta
a futuras aplicaciones interactivas (un visor, un hexdump navegable) sin volver a discutir
la clasificación.

**Lo que la integración NO necesitó tocar** es la mejor evidencia de que la decisión
respeta la arquitectura existente: el contrato de la tabla es

```c
int (*handler)(int argc, char **argv);
```

y no impone ninguna restricción de duración. Un handler puede leer STDIN y ciclar
indefinidamente. Registrar el editor fue añadir dos entradas a `commands[]` y dos
prototipos en `shell.h`; **el REPL del shell y su tokenizador quedaron intactos**.

### 3.2. Refactor de `print_help()`

Al añadir la categoría apareció un problema real en el código base: `print_help()` traía
las cuatro categorías escritas a mano en una cadena de `strcmp`, y era el único punto del
shell que **no** se alimentaba de la tabla de comandos.

```c
/* Antes */
if (strcmp(arg, "datos") == 0 || strcmp(arg, "memoria") == 0 ||
    strcmp(arg, "monitoreo") == 0 || strcmp(arg, "utilidades") == 0) { ... }

/* Ahora */
if (es_categoria(arg)) { ... }   /* deducido recorriendo commands[] */
```

Con `es_categoria()` y `listar_categorias()`, registrar un comando con una categoría nueva
basta para que `help` y `help <categoria>` funcionen solos. La tabla
`descripciones_categoria[]` que queda es puramente cosmética: sólo aporta el texto
descriptivo, y si falta, la categoría se sigue listando.

### 3.3. En proceso vs. proceso aislado: se implementaron ambas

| | `edit` (en proceso) | `edit_ext` (proceso aislado) |
|---|---|---|
| Mecanismo | Llamada a función | `fork(2)` + `execvp(3)` + `waitpid(2)` |
| Costo de arranque | Nulo | Un `fork` + un `exec` por invocación |
| Tabla de descriptores | La del shell (visible con `m_info`) | Propia; el shell no ve el archivo |
| Fallo grave del editor | Se lleva por delante el shell | Sólo muere el hijo; el shell sobrevive |
| Requisitos | Ninguno | El binario `./editor` compilado y accesible |

**`edit` es la estrategia principal.** El shell ya ofrece `p_exec` para lanzar binarios
externos: integrar el editor por esa vía no habría requerido ninguna decisión de diseño
—habría sido escribir `p_exec ./editor` con otro nombre— y habría desperdiciado la
oportunidad de que el editor comparta la tabla de descriptores del shell, algo que se
puede comprobar en vivo ejecutando `m_info` antes y después de abrir un archivo.

La contrapartida se asume explícitamente: un fallo grave del editor mata el shell. Se
mitiga verificando el retorno de **todas** las llamadas al sistema, y se contrasta
empíricamente con `edit_ext`, que ejecuta el mismo `editor_main()` en un proceso hijo.
Que ambas rutas compartan el código hace que la comparación aísle exactamente el mecanismo
de invocación y no diferencias de funcionalidad.

### 3.4. Modelo de datos: recorrido byte a byte, sin índice en memoria

**Decisión: el archivo en disco es la única fuente de verdad. No se mantiene ningún
índice de offsets de línea en memoria.**

La alternativa habitual es cachear en un arreglo el offset donde empieza cada línea, lo
que da acceso O(1) a la línea *n*. Se descartó: cada mutación (`a`, `i`, `d`, `x`)
invalida todos los offsets posteriores, y mantener ese índice sincronizado con el disco es
la principal fuente de corrupción silenciosa en editores de este tipo. El fallo típico no
es un `segfault` sino un archivo mal escrito que sólo se nota mucho después.

| | Índice en memoria | Recorrido byte a byte (elegido) |
|---|---|---|
| Localizar la línea *n* | O(1) | O(n) |
| Riesgo de desincronización | Alto, en cada mutación | **Nulo por construcción** |
| Memoria adicional | Un `off_t` por línea | Un búfer fijo de 4096 B |

Para el tamaño de archivos que maneja un ejercicio de este tipo el costo O(n) es
irrelevante, y a cambio se elimina por completo una clase entera de errores.

**Matiz importante:** "byte a byte" describe la lógica, no la granularidad de las
syscalls. El recorrido lee en **bloques de 4096 bytes** con `read(2)` y escanea el bloque
ya en memoria buscando `\n`. Hacer una syscall por byte sería correcto pero absurdo: cada
byte costaría un cambio de contexto al kernel. Es exactamente la razón por la que existe
el buffering de `stdio`.

Excepción deliberada: el comando `s` sí acumula la línea en curso en un búfer dinámico que
crece por duplicación con `realloc(3)`, para recorrer el archivo **una sola vez** (O(n)).
Consultar la línea *k* con `ed_offset_linea()` para *k* = 1..N daría O(n²).

### 3.5. Cómo se mueven los bytes en `d`, `i` y `x`

Un archivo no ofrece una operación "borrar o insertar en el medio": hay que compactarlo o
abrirle hueco a mano.

```
BORRAR la línea n (comando d)
                inicio        cola_inicio
   [ cabeza ][ línea n a borrar ][ ........ cola ........ ]
                |<-- ocupa -->|

   1. leer la cola ENTERA a memoria (malloc)
   2. lseek(inicio) + write(cola)        -> la cola se desplaza hacia atrás
   3. ftruncate(inicio + cola_len)       -> se recorta el residuo del final
```

Dos detalles que son errores clásicos si se omiten:

- **La cola se lee completa a memoria antes de escribir nada.** Las regiones de origen y
  destino se solapan; escribir mientras se lee destruiría bytes aún no leídos.
- **`ftruncate(2)` es obligatorio.** Sin él el archivo conservaría al final un residuo
  duplicado de la cola, porque `write` sobrescribe pero nunca acorta.

`i` es la operación simétrica —leer la cola, escribir el texto nuevo, reescribir la
cola— y **no** necesita `ftruncate` porque el archivo sólo crece. `x` hace lo mismo pero
arma primero **un solo búfer** con todas las líneas del portapapeles y hace **una sola**
escritura: pegar línea por línea obligaría a releer y desplazar la cola una vez por línea,
con costo cuadrático.

### 3.6. Portapapeles secuencial

Estructura elegida: **arreglo dinámico de cadenas** (`char **`) con crecimiento por
duplicación de capacidad.

Se descartó la lista enlazada: el portapapeles se recorre siempre completo y en orden al
pegar, nunca se inserta ni se borra por el medio. El arreglo da localidad de memoria y una
liberación trivial; la lista pagaría un puntero por línea sin aportar nada.

Es **secuencial** porque `y` *acumula* en lugar de reemplazar: se pueden recoger varias
líneas sueltas del archivo y pegarlas juntas como un bloque con una sola `x`. El
portapapeles no se consume al pegar (se puede pegar el mismo bloque varias veces) y se
vacía explícitamente con `y -`.

### 3.7. `fstat(2)` y no `stat(2)`

`stat(2)` resuelve una **ruta**; `fstat(2)` consulta un **descriptor ya abierto**. La
diferencia importa aquí: si alguien renombra o borra el archivo mientras el editor lo
tiene abierto, `stat("ruta")` fallaría o describiría un archivo distinto, mientras que
`fstat(fd)` sigue describiendo exactamente el inodo que el editor está editando. Un
descriptor es una referencia al inodo, no al nombre. (El shell base usa `stat(2)` en
`d_info` porque allí no hay ningún archivo abierto: es la elección correcta en ese caso.)

### 3.8. `printf` vs `write(2)`: un peligro real de mezcla

El enunciado exige emitir el contenido del archivo con `write(2)` sobre el FD 1, mientras
que los números de línea y los mensajes se imprimen con `printf`. Ambos no comparten
camino: `printf` escribe en un búfer de usuario que `stdio` vacía cuando le conviene, y
`write(2)` va directo al kernel.

Mezclarlos sin cuidado hace que **el contenido aparezca antes que los mensajes**, aunque
el código diga lo contrario. Por eso toda salida a pantalla pasa por
`ed_escribir_stdout()`, que fuerza `fflush(stdout)` antes de cada `write(1, ...)`, y las
macros de traza también vacían el búfer tras imprimir.

### 3.9. Analizador de comandos propio

El editor **no** reutiliza `parse_line()` del shell. Ese tokenizador parte por espacios y
exige comillas para agrupar, lo cual es correcto para un shell pero pésimo para un editor:
obligaría a escribir `a "hola mundo"` y volvería imposible añadir una línea que contenga
comillas dobles.

El analizador del editor separa sólo el comando (y el número, cuando el comando lo lleva)
y toma **el resto de la línea literalmente**. Es el mismo criterio de `ed`. Las comillas
envolventes se aceptan y se descartan, por compatibilidad con quien las escriba por
costumbre.

### 3.10. STDIN sin bufer: un fallo real que apareció al integrar

`edit_ext` funcionaba a mano en la terminal pero fallaba al guionarlo desde un script: el
editor hijo arrancaba y veía EOF de inmediato.

La causa es la interacción entre el buffering de `stdio` y `fork(2)`. Por defecto, la
primera lectura de `stdin` llena un búfer de varios KB. Al leer la línea
`edit_ext archivo` de una tubería, `stdio` se lleva **también** los comandos que venían
detrás, que quedan atrapados en la memoria del padre. El hijo hereda el descriptor 0 con
`fork(2)`, pero **no** hereda ese búfer: encuentra la tubería vacía.

La corrección es una línea, `setvbuf(stdin, NULL, _IONBF, 0)`, en el shell y en el editor:
cada `fgets` consume exactamente los bytes de su línea y deja el resto disponible para
quien lea después. Interactivamente no se notaba porque una terminal es de por sí de línea.

En la misma línea, `cmd_edit_ext()` hace `fflush(stdout)` **antes** de `fork(2)`: lo que
quede sin vaciar en el búfer del padre se duplica en el hijo y acabaría imprimiéndose dos
veces.

> Nota: el comando `p_exec` del shell base arrastra exactamente el mismo problema de
> entrada bufereada. La corrección de `setvbuf` en `main()` también lo beneficia.

### 3.11. Un solo editor, dos puntos de entrada

`editor.c` se compila **tanto** dentro del shell **como** dentro del binario `editor`;
`main_editor.c` contiene únicamente el `main()` y el manejo de argumentos. El editor integrado
y el binario suelto no son dos programas parecidos: son el mismo programa. Corregir un
fallo en `editor.c` lo corrige en ambos a la vez, y es lo que permite que `edit_ext`
compare mecanismos de invocación sin comparar funcionalidades distintas.

---

## 4. Manejo de errores

Toda llamada al sistema verifica su valor de retorno. Los fallos se reportan por STDERR
con `perror(3)`, que añade la descripción textual de `errno`:

```
editor[notas.txt]> o /raiz/prohibido.txt
open: Permission denied
```

Además:

- `write(2)` y `read(2)` pueden transferir **menos** bytes de los pedidos sin que sea un
  error. Todas las transferencias pasan por `ed_escribir_todo()` / `ed_leer_todo()`, que
  reintentan hasta completar y reintentan también ante `EINTR`.
- Los comandos que exigen un archivo abierto avisan en vez de fallar.
- Los índices fuera de rango (`0`, negativos, mayores que el total) se rechazan **antes**
  de tocar el disco, de modo que un comando inválido nunca deja el archivo a medio escribir.
- `q` y la salida por EOF (Ctrl+D) recorren el mismo camino de liberación:
  `close(2)` del descriptor y `free(3)` de todo el portapapeles.

---

## 5. Pruebas

```bash
make test    # 50 casos
make asan    # los mismos casos con AddressSanitizer + UBSan + LeakSanitizer
```

`pruebas.sh` alimenta el binario `./editor` por STDIN con guiones de comandos y compara
**byte a byte** el archivo resultante contra el contenido esperado. Cubre:

| Bloque | Qué valida |
|---|---|
| 1. Comandos base | `o` crea el archivo, `a` añade, `p` imprime todo y por línea, `q` cierra |
| 2. Borrado | `d` en el medio, en la última línea, y hasta vaciar el archivo (0 bytes) |
| 3. Inserción | `i` en el medio, al principio, y en `total+1` (degenera en `a`) |
| 4. Búsqueda | Coincidencias múltiples, ausencia de falsos positivos, sin coincidencias |
| 5. Metadatos | `m` reporta inodo, tamaño, permisos y fecha de modificación |
| 6. Portapapeles | `y`/`x`, acumulación de varias copias, pegado al final, `y -` |
| 7. Casos borde | Archivo vacío, línea 0/negativa/fuera de rango, archivo sin `\n` final, sin archivo abierto, comando inexistente, texto con espacios, archivo multi-bloque (400 líneas > 4096 B) |
| 8. Permisos | El archivo se crea con modo `0644`; cierre ordenado |
| 9. Integración | `help` lista la categoría `edicion`; `edit` y `edit_ext` escriben bien y devuelven el control al shell |

Casos borde que merecen mención especial:

- **Archivo sin salto de línea final:** `a` detecta la línea incompleta y añade el `\n`
  que falta antes de escribir, para no pegar el texto nuevo al final de la línea previa.
- **Archivo mayor que el bloque de lectura:** con 400 líneas se fuerzan varias iteraciones
  de `read(2)`, comprobando que el recorrido no pierde líneas partidas entre bloques.
- **Borrar hasta vaciar:** el archivo debe quedar en exactamente 0 bytes, lo que sólo
  ocurre si `ftruncate(2)` se aplica correctamente.

---

## 6. Mapa de archivos

| Archivo | Contenido |
|---|---|
| `editor.h` | Interfaz del módulo: las decisiones de diseño y `editor_main()`, lo único que se usa desde fuera |
| `editor.c` | Todo el editor: estructuras, recorrido, comandos, sub-REPL e integración. El resto de funciones son `static` |
| `main_editor.c` | Sólo el `main()` del binario independiente `editor` |
| `shell.h` | Cabecera del shell + prototipos de la categoría `edicion` |
| `main.c` | Tabla de comandos, `help` derivado de la tabla, REPL del shell |
| `pruebas.sh` | Batería de 50 pruebas automatizadas |
| `Makefile` | Targets `all`, `run`, `run-editor`, `test`, `asan`, `valgrind`, `clean` |

---

## 7. Requisitos cubiertos (equipo de 3, acumulativos)

| Nivel | Requisito | Estado |
|---|---|---|
| Base | `o`, `p`, `a`, `d`, `q` + integración en el shell | Implementado |
| Equipos de 2 | `i <n> <texto>` (inserción arbitraria) | Implementado |
| Equipos de 2 | `s <palabra>` (búsqueda simple) | Implementado |
| Equipos de 2 | Búferes dinámicos con `malloc`/`realloc`/`free` | `d`, `i`, `s`, `x`, portapapeles |
| Equipos de 3 | `m` (tamaño, permisos, inodo, modificación) vía `fstat(2)` | Implementado |
| Equipos de 3 | `y <n>` copiar y `x <n>` pegar, portapapeles secuencial | Implementado |
