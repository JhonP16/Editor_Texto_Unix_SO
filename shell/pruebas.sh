#!/usr/bin/env bash
# ======================================================================================
#  pruebas.sh - Script de demostración y validación del editor de texto CLI
# ======================================================================================
#  Ejecuta el binario ./editor alimentándolo por STDIN con guiones de comandos, y compara
#  byte a byte el archivo resultante contra el contenido esperado. Cubre el camino feliz
#  de los 9 comandos y una batería de casos borde.
#
#  Uso:      ./pruebas.sh            (o bien:  make test)
#            VALGRIND=1 ./pruebas.sh (o bien:  make valgrind)
#
#  Código de salida: 0 si todas las pruebas pasan, 1 si alguna falla.
# ======================================================================================
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
# El binario a probar puede sobrescribirse por entorno (lo usa el target "make asan").
# Se llama EDITOR_BIN y no EDITOR porque esta ultima ya es una variable estandar de Unix.
EDITOR_BIN="${EDITOR_BIN:-$DIR/editor}"
TMP="$DIR/pruebas_tmp"

VERDE=$'\033[1;32m'; ROJO=$'\033[1;31m'; AZUL=$'\033[1;36m'
GRIS=$'\033[0;90m';  NEGRITA=$'\033[1;97m'; FIN=$'\033[0m'

PASADAS=0
FALLADAS=0

if [ ! -x "$EDITOR_BIN" ]; then
    echo "${ROJO}No existe el binario '$EDITOR_BIN'. Ejecute 'make' primero.${FIN}"
    exit 1
fi

rm -rf "$TMP"
mkdir -p "$TMP"

# --------------------------------------------------------------------------------------
# El editor crea los archivos con modo 0644 y el comando m lee esos permisos con fstat.
# Si el proyecto vive en una unidad de Windows montada en WSL (/mnt/c, sistema DrvFs),
# esos permisos no existen de verdad: todo aparece como 0777 y la prueba fallaria sin
# que el editor tenga ningun defecto. En ese caso las pruebas se trasladan a /tmp, que
# si es un sistema de archivos POSIX real.
# --------------------------------------------------------------------------------------
: > "$TMP/.prueba_permisos"
chmod 644 "$TMP/.prueba_permisos" 2>/dev/null
if [ "$(stat -c '%a' "$TMP/.prueba_permisos" 2>/dev/null)" != "644" ]; then
    echo "${GRIS}Aviso: '$TMP' no conserva permisos POSIX (montaje tipo DrvFs de WSL).${FIN}"
    echo "${GRIS}       Las pruebas se ejecutaran en /tmp para poder validar el modo 0644.${FIN}"
    rm -rf "$TMP"
    TMP="$(mktemp -d /tmp/editor_pruebas.XXXXXX)"
else
    rm -f "$TMP/.prueba_permisos"
fi

# --------------------------------------------------------------------------------------
# correr <archivo>   -- lanza el editor sobre <archivo> con los comandos que lleguen por
#                       STDIN. La salida del editor queda en $TMP/salida.txt.
# --------------------------------------------------------------------------------------
correr() {
    if [ "${VALGRIND:-0}" = "1" ]; then
        valgrind --leak-check=full --error-exitcode=99 --log-file="$TMP/valgrind.log" \
                 "$EDITOR_BIN" "$1" > "$TMP/salida.txt" 2>&1
    else
        "$EDITOR_BIN" "$1" > "$TMP/salida.txt" 2>&1
    fi
    # Copia sin códigos de color: los patrones de búsqueda se aplican sobre ella
    sed -e 's/\x1b\[[0-9;]*m//g' "$TMP/salida.txt" > "$TMP/salida_plana.txt"
}

# --------------------------------------------------------------------------------------
# verificar_archivo <descripción> <archivo>   -- compara <archivo> contra el contenido
#                                                esperado que llega por STDIN.
# --------------------------------------------------------------------------------------
verificar_archivo() {
    local desc="$1" archivo="$2"
    cat > "$TMP/esperado.txt"
    if diff -u "$TMP/esperado.txt" "$archivo" > "$TMP/diff.txt" 2>&1; then
        echo "  ${VERDE}[PASA]${FIN} $desc"
        PASADAS=$((PASADAS + 1))
    else
        echo "  ${ROJO}[FALLA]${FIN} $desc"
        sed 's/^/          /' "$TMP/diff.txt"
        FALLADAS=$((FALLADAS + 1))
    fi
}

# --------------------------------------------------------------------------------------
# verificar_salida <descripción> <patrón>   -- comprueba que la salida del editor
#                                              contenga el patrón indicado.
# --------------------------------------------------------------------------------------
verificar_salida() {
    local desc="$1" patron="$2"
    if grep -q -- "$patron" "$TMP/salida_plana.txt"; then
        echo "  ${VERDE}[PASA]${FIN} $desc"
        PASADAS=$((PASADAS + 1))
    else
        echo "  ${ROJO}[FALLA]${FIN} $desc  (no se encontró: '$patron')"
        sed 's/^/          /' "$TMP/salida_plana.txt" | tail -12
        FALLADAS=$((FALLADAS + 1))
    fi
}

# --------------------------------------------------------------------------------------
# verificar_ausencia <descripción> <patrón>  -- comprueba que el patrón NO aparezca.
# --------------------------------------------------------------------------------------
verificar_ausencia() {
    local desc="$1" patron="$2"
    if grep -q -- "$patron" "$TMP/salida_plana.txt"; then
        echo "  ${ROJO}[FALLA]${FIN} $desc  (apareció lo que no debía: '$patron')"
        FALLADAS=$((FALLADAS + 1))
    else
        echo "  ${VERDE}[PASA]${FIN} $desc"
        PASADAS=$((PASADAS + 1))
    fi
}

titulo() { echo; echo "${NEGRITA}== $1 ==${FIN}"; }

echo "${AZUL}========================================================${FIN}"
echo "${AZUL}  Pruebas del Editor de Texto CLI  (binario: ./editor)${FIN}"
if [ "${VALGRIND:-0}" = "1" ]; then
    echo "${GRIS}  Modo valgrind ACTIVADO${FIN}"
fi
echo "${AZUL}========================================================${FIN}"

# ======================================================================================
titulo "1. Comandos base: o, a, p, q"
# ======================================================================================
correr "$TMP/t1.txt" <<'IN'
a primera
a segunda
a tercera
p
q
IN
verificar_archivo "o crea el archivo y 'a' añade tres líneas al final" "$TMP/t1.txt" <<'ESP'
primera
segunda
tercera
ESP
verificar_salida "p sin argumentos imprime las tres líneas numeradas" "3 | tercera"

correr "$TMP/t1.txt" <<'IN'
p 2
q
IN
verificar_salida "p 2 imprime únicamente la línea 2" "2 | segunda"
verificar_ausencia "p 2 no imprime ninguna otra línea" "primera"

# ======================================================================================
titulo "2. Borrado: d"
# ======================================================================================
correr "$TMP/t2.txt" <<'IN'
a alfa
a beta
a gamma
a delta
d 2
q
IN
verificar_archivo "d 2 borra la línea del medio y compacta el archivo" "$TMP/t2.txt" <<'ESP'
alfa
gamma
delta
ESP

correr "$TMP/t2.txt" <<'IN'
d 3
q
IN
verificar_archivo "d sobre la última línea deja el archivo bien terminado" "$TMP/t2.txt" <<'ESP'
alfa
gamma
ESP

correr "$TMP/t2.txt" <<'IN'
d 1
d 1
q
IN
verificar_archivo "d repetido hasta vaciar deja el archivo en 0 bytes" "$TMP/t2.txt" <<'ESP'
ESP

# ======================================================================================
titulo "3. Inserción arbitraria: i"
# ======================================================================================
correr "$TMP/t3.txt" <<'IN'
a uno
a tres
i 2 dos
q
IN
verificar_archivo "i 2 inserta desplazando el resto hacia abajo" "$TMP/t3.txt" <<'ESP'
uno
dos
tres
ESP

correr "$TMP/t3.txt" <<'IN'
i 1 cero
q
IN
verificar_archivo "i 1 inserta al principio del archivo" "$TMP/t3.txt" <<'ESP'
cero
uno
dos
tres
ESP

correr "$TMP/t3.txt" <<'IN'
i 5 cuatro
q
IN
verificar_archivo "i en total+1 equivale a añadir al final" "$TMP/t3.txt" <<'ESP'
cero
uno
dos
tres
cuatro
ESP

# ======================================================================================
titulo "4. Búsqueda: s"
# ======================================================================================
correr "$TMP/t4.txt" <<'IN'
a el gato duerme
a el perro corre
a otro gato salta
s gato
q
IN
verificar_salida "s encuentra la primera coincidencia con su número de línea" "1 | el gato duerme"
verificar_salida "s encuentra la segunda coincidencia" "3 | otro gato salta"
verificar_ausencia "s no muestra líneas que no coinciden" "el perro corre"
verificar_salida "s informa cuántas líneas coinciden" "2 línea(s) coinciden"

correr "$TMP/t4.txt" <<'IN'
s inexistente
q
IN
verificar_salida "s avisa cuando no hay coincidencias" "Sin coincidencias"
verificar_archivo "s no modifica el archivo" "$TMP/t4.txt" <<'ESP'
el gato duerme
el perro corre
otro gato salta
ESP

# ======================================================================================
titulo "5. Metadatos: m (fstat)"
# ======================================================================================
correr "$TMP/t5.txt" <<'IN'
a contenido de prueba
m
q
IN
verificar_salida "m muestra el número de inodo" "Inodo:"
verificar_salida "m muestra el tamaño en bytes" "20 byte(s)"
verificar_salida "m muestra los permisos en octal y simbólicos" "(0644)"
verificar_salida "m muestra la fecha de modificación" "Modificación:"

# ======================================================================================
titulo "6. Portapapeles: y (copiar) y x (pegar)"
# ======================================================================================
correr "$TMP/t6.txt" <<'IN'
a uno
a dos
a tres
y 1
x 3
q
IN
verificar_archivo "y 1 copia y x 3 pega esa línea antes de la tercera" "$TMP/t6.txt" <<'ESP'
uno
dos
uno
tres
ESP

correr "$TMP/t6b.txt" <<'IN'
a alfa
a beta
a gamma
y 1
y 3
x 2
q
IN
verificar_archivo "y acumula: dos copias se pegan juntas como un bloque" "$TMP/t6b.txt" <<'ESP'
alfa
alfa
gamma
beta
gamma
ESP

correr "$TMP/t6c.txt" <<'IN'
a linea unica
y 1
x 2
q
IN
verificar_archivo "x en total+1 pega al final del archivo" "$TMP/t6c.txt" <<'ESP'
linea unica
linea unica
ESP

correr "$TMP/t6d.txt" <<'IN'
a texto
y 1
y -
x 1
q
IN
verificar_salida "y - vacía el portapapeles" "Portapapeles vaciado"
verificar_salida "x con el portapapeles vacío es rechazado" "El portapapeles está vacío"
verificar_archivo "x rechazado no altera el archivo" "$TMP/t6d.txt" <<'ESP'
texto
ESP

# ======================================================================================
titulo "7. Casos borde y manejo de errores"
# ======================================================================================
correr "$TMP/t7_vacio.txt" <<'IN'
p
p 1
d 1
q
IN
verificar_salida "p sobre archivo vacío lo informa sin fallar" "el archivo está vacío"
verificar_salida "d sobre archivo vacío es rechazado" "no hay nada que borrar"

correr "$TMP/t7.txt" <<'IN'
a unica
p 99
d 99
i 99 texto
y 99
d 0
d -3
q
IN
verificar_salida "p con línea fuera de rango es rechazado" "Línea 99 fuera de rango"
verificar_salida "d con línea 0 es rechazado" "Línea 0 fuera de rango"
verificar_salida "d con línea negativa es rechazado" "Línea -3 fuera de rango"
verificar_archivo "ningún comando fuera de rango modificó el archivo" "$TMP/t7.txt" <<'ESP'
unica
ESP

# Archivo cuya última línea NO termina en salto de línea (caso clásico de corrupción)
printf 'con salto\nsin salto final' > "$TMP/t7_sinnl.txt"
correr "$TMP/t7_sinnl.txt" <<'IN'
p
a nueva
q
IN
verificar_salida "se detecta la última línea sin salto de línea" "no termina en salto"
verificar_archivo "a sobre archivo sin salto final no pega el texto a la línea previa" "$TMP/t7_sinnl.txt" <<'ESP'
con salto
sin salto final
nueva
ESP

printf 'uno\ndos\ntres sin salto' > "$TMP/t7_sinnl2.txt"
correr "$TMP/t7_sinnl2.txt" <<'IN'
d 3
q
IN
verificar_archivo "d de la última línea sin salto recorta bien el archivo" "$TMP/t7_sinnl2.txt" <<'ESP'
uno
dos
ESP

# Comandos que exigen archivo abierto, sin haber abierto ninguno
"$EDITOR_BIN" > "$TMP/salida.txt" 2>&1 <<'IN'
p
a algo
m
q
IN
sed -e 's/\x1b\[[0-9;]*m//g' "$TMP/salida.txt" > "$TMP/salida_plana.txt"
verificar_salida "sin archivo abierto los comandos avisan en vez de fallar" "No hay ningún archivo abierto"

correr "$TMP/t7.txt" <<'IN'
zzz
q
IN
verificar_salida "un comando inexistente se reporta con ayuda" "no reconocido"

# Rutas y textos con espacios
correr "$TMP/t7_esp.txt" <<'IN'
a esto es una linea con varios espacios
a "entre comillas tambien funciona"
q
IN
verificar_archivo "el texto de a se toma literal, con o sin comillas" "$TMP/t7_esp.txt" <<'ESP'
esto es una linea con varios espacios
entre comillas tambien funciona
ESP

# Archivo más grande que el bloque de lectura (4096 B): fuerza varias iteraciones de read
{ for i in $(seq 1 400); do echo "linea numero $i de relleno para superar el bloque"; done; } > "$TMP/t7_grande.txt"
correr "$TMP/t7_grande.txt" <<'IN'
d 1
s linea numero 400
q
IN
verificar_salida "en archivo multi-bloque la búsqueda ubica la línea correcta" "399 | linea numero 400"
if [ "$(wc -l < "$TMP/t7_grande.txt")" = "399" ]; then
    echo "  ${VERDE}[PASA]${FIN} d sobre archivo multi-bloque deja 399 líneas"
    PASADAS=$((PASADAS + 1))
else
    echo "  ${ROJO}[FALLA]${FIN} d sobre archivo multi-bloque: quedaron $(wc -l < "$TMP/t7_grande.txt") líneas"
    FALLADAS=$((FALLADAS + 1))
fi

# ======================================================================================
titulo "8. Permisos del archivo creado y cierre limpio"
# ======================================================================================
correr "$TMP/t8.txt" <<'IN'
a x
q
IN
PERM="$(stat -c '%a' "$TMP/t8.txt")"
if [ "$PERM" = "644" ] || [ "$PERM" = "664" ]; then
    echo "  ${VERDE}[PASA]${FIN} el archivo se crea con modo 0644 (umask aplicada: $PERM)"
    PASADAS=$((PASADAS + 1))
else
    echo "  ${ROJO}[FALLA]${FIN} permisos inesperados: $PERM"
    FALLADAS=$((FALLADAS + 1))
fi
verificar_salida "q cierra el archivo y sale ordenadamente" "Cerrando el archivo"

if [ "${VALGRIND:-0}" = "1" ] && [ -f "$TMP/valgrind.log" ]; then
    if grep -q "All heap blocks were freed" "$TMP/valgrind.log" || \
       grep -q "definitely lost: 0 bytes" "$TMP/valgrind.log"; then
        echo "  ${VERDE}[PASA]${FIN} valgrind: sin fugas de memoria"
        PASADAS=$((PASADAS + 1))
    else
        echo "  ${ROJO}[FALLA]${FIN} valgrind reportó fugas (ver $TMP/valgrind.log)"
        grep -A4 "LEAK SUMMARY" "$TMP/valgrind.log" | sed 's/^/          /'
        FALLADAS=$((FALLADAS + 1))
    fi
fi


# ======================================================================================
titulo "9. Integración con el shell (categoría 'edicion')"
# ======================================================================================
SHELL_BIN="$DIR/eafitOS"
if [ -x "$SHELL_BIN" ]; then
    rm -f "$TMP/t9.txt" "$TMP/t9b.txt"

    "$SHELL_BIN" > "$TMP/salida.txt" 2>&1 <<IN
help
help edicion
edit $TMP/t9.txt
a linea escrita desde el shell
q
saludar
exit
IN
    sed -e 's/\x1b\[[0-9;]*m//g' "$TMP/salida.txt" > "$TMP/salida_plana.txt"
    verificar_salida "help lista la categoría nueva 'edicion'" "edicion"
    verificar_salida "help edicion muestra el comando edit" "edit "
    verificar_salida "help edicion muestra el comando edit_ext" "edit_ext"
    verificar_salida "el shell recupera el control tras salir del editor" "Bienvenido al Shell"
    verificar_archivo "edit (en proceso) escribe correctamente en el archivo" "$TMP/t9.txt" <<'ESP'
linea escrita desde el shell
ESP

    "$SHELL_BIN" > "$TMP/salida.txt" 2>&1 <<IN
edit_ext $TMP/t9b.txt
a linea escrita por el proceso hijo
q
exit
IN
    sed -e 's/\x1b\[[0-9;]*m//g' "$TMP/salida.txt" > "$TMP/salida_plana.txt"
    verificar_salida "edit_ext lanza el editor con fork y espera con waitpid" "Editor lanzado como proceso hijo"
    verificar_salida "edit_ext reporta el código de salida del hijo" "terminó con código 0"
    verificar_archivo "edit_ext (proceso aislado) escribe correctamente" "$TMP/t9b.txt" <<'ESP'
linea escrita por el proceso hijo
ESP
else
    echo "  ${GRIS}[OMITIDA]${FIN} el shell 'eafitOS' no está compilado; ejecute 'make'"
fi

# ======================================================================================
echo
echo "${AZUL}========================================================${FIN}"
echo "  Resultado:  ${VERDE}${PASADAS} prueba(s) pasada(s)${FIN}   ${ROJO}${FALLADAS} fallida(s)${FIN}"
echo "${AZUL}========================================================${FIN}"
echo "${GRIS}  Archivos de trabajo en: $TMP${FIN}"

[ "$FALLADAS" -eq 0 ] || exit 1
exit 0
