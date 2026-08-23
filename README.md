# Editor de Texto Interactivo CLI en C

**Universidad EAFIT — Sistemas Operativos (SO2026B)**
Evaluación práctica del primer parcial · Equipo de 3 integrantes

Editor de texto interactivo, escrito en C, integrado como una **categoría
nueva (`edicion`)** dentro del shell educativo de la asignatura.

**Integrantes:**

- Yesid Hurtado Montoya
- Andrés Felipe Eusse
- Jhon Jairo Pulgarín Restrepo

---

## 1. Entregables

| Entregable del enunciado | Dónde está |
|---|---|
| **Código fuente** `.c` / `.h` documentados | `shell/` — ver el [mapa de archivos](#9-mapa-de-archivos) |
| **Makefile** con targets `all` y `clean` | `shell/Makefile` (además: `run`, `run-editor`, `test`, `asan`, `valgrind`) |
| **Shell actualizado** integrando el editor | `shell/main.c`, `shell/shell.h` — ver [§5](#5-integración-con-el-shell) |
| **Código de demostración / pruebas** | `shell/pruebas.sh` — 46 casos, ver [§6](#6-pruebas) |
| **Documento de sustentación (PDF)** | En el código fuente del respositorio, nombrado como `DocumentoSustentación_EditorTexo.pdf` |
| **Video explicativo** |  |


---

## 2. Inicio rápido

Requiere un entorno Linux con `gcc` y `make` (en Windows, WSL).

```bash
cd shell
make            # compila los dos binarios: eafitOS (shell) y editor
./eafitOS       # lanza el shell
```

Y dentro del shell:

```
eafitOS> edit notas.txt
```
Si no pones archivo, después se te pedirá y lo podrá poner con el comando `o`

### Targets disponibles

| Comando | Qué hace |
|---|---|
| `make` / `make all` | Compila `eafitOS` y `editor`. No ejecuta nada. |
| `make run` | Compila y lanza el shell interactivo |
| `make run-editor` | Compila y lanza el editor independiente |
| `make test` | Ejecuta la batería de 46 pruebas |
| `make asan` | Las mismas pruebas con AddressSanitizer + UBSan + LeakSanitizer |
| `make valgrind` | Las mismas pruebas bajo valgrind (si está instalado) |
| `make clean` | Borra binarios, objetos y residuos de pruebas |

> **Nota para WSL:** si el repositorio está en `/mnt/c/...`, ese montaje (DrvFs) no
> conserva permisos POSIX y todo archivo aparece con modo `0777`. El script de pruebas lo
> detecta solo y se traslada a `/tmp` para poder validar el modo `0644` de verdad.


---

## 3. Requisitos cubiertos (equipo de 3)

| Nivel | Requisito | Estado |
|---|---|---|
| Base | `o`, `p`, `a`, `d`, `q` | Implementado |
| Base | Integración en el shell de clase | Categoría nueva `edicion` — ver [§5](#5-integración-con-el-shell) |
| Equipos de 2 | `i <n> <texto>` — inserción arbitraria | Implementado |
| Equipos de 2 | `s <palabra>` — búsqueda simple | Implementado |
| Equipos de 2 | Búferes dinámicos `malloc`/`realloc`/`free` | `d`, `i`, `s`, `x` y el portapapeles |
| Equipos de 3 | `m` — tamaño, permisos, inodo y modificación vía `fstat(2)` | Implementado |
| Equipos de 3 | `y <n>` copiar y `x <n>` pegar, portapapeles secuencial | Implementado |

> **Restricción de E/S cumplida.** Todo el acceso a disco se hace exclusivamente con
> llamadas al sistema: `open(2)`, `read(2)`, `write(2)`, `lseek(2)`, `ftruncate(2)`,
> `fstat(2)` y `close(2)`. No se usa `fopen`/`fread`/`fwrite`/`fclose` en ningún punto.

---

## 4. Comandos del editor

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

## 5. Integración con el shell

### 5.1. Las dos formas de invocar el editor

Hay **un solo editor**: `editor.c` se compila tanto dentro del shell como dentro del
binario suelto. No son dos programas parecidos, son el mismo programa con dos puntos de
entrada (ver [§7.10](#710-un-solo-editor-dos-puntos-de-entrada)).

| Forma | Cómo se lanza | Cuándo usarla |
|---|---|---|
| `edit <archivo>` | Dentro de `./eafitOS` | **La principal.** Es la integración que se evalúa. |
| `./editor <archivo>` | Desde la terminal | Probar el editor solo. Es lo que usa `pruebas.sh`. |

### 5.2. Catálogo de comandos del shell

El shell mantiene sus cuatro categorías originales y suma la quinta:

| Categoría | Comandos | Syscalls que demuestra |
|---|---|---|
| `datos` | `d_create`, `d_read`, `d_info`, `d_copy` | `open`, `read`, `write`, `stat`, `close` |
| `memoria` | `m_sbrk`, `m_mmap`, `m_info` | `sbrk`, `mmap`, `munmap`, `/proc/self/status` |
| `monitoreo` | `p_fork`, `p_exec`, `p_kill`, `p_monitor` | `fork`, `execvp`, `waitpid`, `kill`, `getrusage` |
| **`edicion`** | **`edit`** | **`open`, `read`, `write`, `lseek`, `ftruncate`, `fstat`, `close`** |
| `utilidades` | `saludar`, `despedir`, `hora`, `fecha` | `getuid`, `time` |

Ayuda interactiva: `help` lista las categorías, `help <categoria>` sus comandos y
`help <comando>` el detalle de uno. La lista de categorías **se deriva automáticamente**
de la tabla de comandos (ver [§7.2](#72-refactor-de-print_help)).

### 5.3. Qué se tocó del shell original

De las 100 líneas añadidas a `main.c` y `shell.h`, la mayor parte es el refactor de
`print_help()` y sus comentarios. Lo estrictamente necesario para integrar el editor
fueron **ocho líneas**:

- `shell/shell.h` — un prototipo: `cmd_edit()`
- `shell/main.c` — una entrada en la tabla `commands[]`, con el mismo formato que las
  catorce existentes

**Intactos quedaron** el REPL principal, el tokenizador `parse_line()`, la estructura
`Command`, la firma del handler y los catorce comandos originales. Que una aplicación
interactiva con estado quepa sin modificar el núcleo del shell es la evidencia de que la
integración respeta su arquitectura en lugar de forzarla.

---

## 6. Pruebas

```bash
make test    # 46 casos
make asan    # los mismos casos con AddressSanitizer + UBSan + LeakSanitizer
```

`pruebas.sh` alimenta el binario `./editor` por STDIN con guiones de comandos y compara
**byte a byte** el archivo resultante contra el contenido esperado.

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
| 9. Integración | `help` lista la categoría `edicion`; `edit` escribe bien y devuelve el control al shell |

Casos borde que merecen mención especial:

- **Archivo sin salto de línea final:** `a` detecta la línea incompleta y añade el `\n`
  que falta antes de escribir, para no pegar el texto nuevo al final de la línea previa.
- **Archivo mayor que el bloque de lectura:** con 400 líneas se fuerzan varias iteraciones
  de `read(2)`, comprobando que el recorrido no pierde líneas partidas entre bloques.
- **Borrar hasta vaciar:** el archivo debe quedar en exactamente 0 bytes, lo que sólo
  ocurre si `ftruncate(2)` se aplica correctamente.


---

## 7. Manejo de errores

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

Verificado con `make asan`: los 46 casos pasan bajo AddressSanitizer, UBSan y
LeakSanitizer sin un solo hallazgo.

---

## 8. Mapa de archivos

```
editor_texto/
├── README.md                 este documento
├── sustentacion.tex          fuente LaTeX del PDF de sustentación (Overleaf)
├── proyecto_editor_texto.md  el enunciado de la evaluación
└── shell/
    ├── Makefile
    ├── pruebas.sh            batería de 46 pruebas automatizadas
    ├── editor.h  editor.c    el editor
    ├── main_editor.c         main() del binario independiente
    ├── shell.h   main.c      el shell de clase, con la categoría 'edicion'
    ├── cat_datos.c  cat_memoria.c  cat_monitoreo.c  cat_util.c
    └── GUIA_DIDACTICA.md     material de clase sobre la arquitectura del shell
```

| Archivo | Contenido |
|---|---|
| `editor.h` | Interfaz del módulo: las decisiones de diseño y `editor_main()`, lo único que se usa desde fuera |
| `editor.c` | Todo el editor: estructuras, recorrido, comandos, sub-REPL e integración. El resto de funciones son `static` |
| `main_editor.c` | Sólo el `main()` del binario independiente `editor` |
| `shell.h` | Cabecera del shell + prototipos de la categoría `edicion` |
| `main.c` | Tabla de comandos, `help` derivado de la tabla, REPL del shell |
| `cat_*.c` | Los comandos de las cuatro categorías originales del shell (sin modificar) |
| `pruebas.sh` | Batería de 46 pruebas automatizadas |
| `Makefile` | Targets `all`, `run`, `run-editor`, `test`, `asan`, `valgrind`, `clean` |
