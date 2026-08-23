# **Universidad EAFIT \- Evaluación Práctica** (primer parcial)**: Desarrollo de un Editor de Texto en Unix**

**Asignatura:** Sistemas Operativos  
**Tema:** Llamadas al Sistema (System Calls) e Interfaz POSIX (CLI) en Ambiente Linux (Sin Concurrencia)  
**Lenguaje de Programación:** C

## **1\. Objetivos de la Evaluación**

> * Comprender el funcionamiento interno de las herramientas CLI (Command Line Interface) para la edición de texto en sistemas Linux (inspirado en ed o vi).  
> * Aplicar llamadas al sistema (*system calls*) de bajo nivel para la manipulación de archivos y control estricto de entrada/salida.  
> * **Demostrar pensamiento crítico** en la toma de decisiones arquitectónicas, análisis de sistemas y en la resolución de problemas de bajo nivel.  
> * Desarrollar habilidades avanzadas en la gestión de memoria dinámica, punteros y búferes en el lenguaje C. (investigación y preparación para el tema que sigue)  
> * Fomentar el trabajo colaborativo adaptando la complejidad del software a la capacidad operativa del equipo de desarrollo.

## **2\. Descripción del Proyecto e Integración Arquitectónica**

Los estudiantes deberán construir en lenguaje C un editor de texto interactivo operado estrictamente por CLI en un entorno Linux (no GUI). El programa permitirá leer, modificar y guardar archivos planos utilizando llamadas al sistema POSIX. La arquitectura del sistema debe estar preparada para escalar en funcionalidades dependiendo del tamaño del equipo de trabajo asignado.  
**Integración Estratégica con el Shell:** Como requerimiento fundamental para evaluar el **pensamiento crítico**, el editor de texto desarrollado deberá integrarse funcionalmente con el Shell estudiado en clase ([Repositorio SO2026B \- Shell](https://github.com/evalenciEAFIT/SO2026B/tree/main/shell)). El equipo de trabajo deberá analizar rigurosamente la arquitectura de este shell y tomar una decisión argumentada sobre cómo invocar el editor. Deben determinar si el llamado debe clasificarse dentro de una categoría existente o si es necesario crear y justificar una categoría completamente nueva dentro de la estructura del shell para este tipo de aplicaciones.  
**Restricción Crítica de I/O:** Queda prohibido el uso de las funciones de alto nivel de la biblioteca estándar de C (como fopen, fread, fwrite o fclose) para la manipulación del archivo de texto. Todo acceso a disco debe realizarse mediante open, read, write, lseek, ftruncate y close. etc. Solo se permite I/O estándar (printf, scanf, fgets) para la lectura de los comandos en STDIN y la impresión de la consola en STDOUT.

## **3\. Comandos Base CLI (Requisito Mínimo)**

Independientemente del tamaño del equipo, el editor debe ejecutarse en un ciclo interactivo y soportar los siguientes comandos:

| Comando | Descripción | System Calls Recomendadas |
| :---- | :---- | :---- |
| o \[archivo\] | Abre un archivo en disco. Si no existe, lo crea con los permisos adecuados. | open() con O\_RDWR | O\_CREAT, mode\_t |
| p \[n\] | Imprime la línea *n*. Sin parámetros, imprime todo el archivo recorriendo bytes hasta \\n. | read(), lseek(), write() (FD 1\) |
| a \[texto\] | Añade el texto provisto como una nueva línea al final del archivo. | lseek() (SEEK\_END), write() |
| d \[n\] | Borra la línea *n* del archivo (desplazando los bytes posteriores y truncando). | read(), write(), lseek(), ftruncate() |
| q | Cierra el File Descriptor y sale de la aplicación sin dejar fugas de memoria. | close(), exit() |

## **4\. Escalamiento de Dificultad por Tamaño de Equipo**

Para garantizar la equidad en la evaluación, los equipos más grandes deberán implementar funcionalidades adicionales que requieran un uso más avanzado de los recursos del Sistema Operativo Linux. **NOTA IMPORTANTE: Los requisitos son estrictamente acumulativos. Un equipo debe implementar los comandos base más todos los requisitos de los equipos de menor tamaño, sumados a sus requisitos específicos.** Dado que el tema de concurrencia no ha sido abordado, todos los retos deben resolverse utilizando procesos de un solo hilo y gestión secuencial.

| Integrantes | Requisitos Acumulativos a Implementar | Reto Técnico de OS   |
| :---- | :---- | :---- |
| **1 (Individual)** | Implementación completa de los comandos base (Sección 3). Integración en el Shell de clase. | Dominio de open, read, write, lseek. Gestión básica de descriptores de archivos. |
| **2 (Parejas)** | Todo lo anterior \+ **1\. Inserción arbitraria:** i \[n\] \[texto\] (Inserta texto en la línea *n* desplazando el resto). **2\. Búsqueda simple:** s \[palabra\]. | Manejo avanzado de lseek y manipulación de *buffers* dinámicos en memoria (malloc/free) para no perder datos al desplazar bytes. |
| **3 (Tres)** | Todo lo anterior \+ **1\. Metadatos del archivo:** Comando m que imprima tamaño, permisos, inodo y modificación. **2\. Copiar y Pegar:** y \[n\] y x \[n\]. | Integración de la system call fstat() para acceder a la metadata en los inodos. Gestión de un portapapeles secuencial local. |
| **4 (Cuatro)** | Todo lo anterior \+ **1\. Deshacer/Rehacer (Undo/Redo):** Comandos u (Undo) y r (Redo). | Creación de archivos temporales (archivos de *swap*) en el directorio /tmp. Uso de la system call unlink() al cerrar para limpiar. |
| **5 (Cinco)** | Todo lo anterior \+ **1\. Múltiples buffers:** Comando b \[archivo\] para mantener varios archivos a la vez. **2\. Bloqueo de archivos (Locks):** fcntl(). | Gestión manual de una tabla de descriptores de archivos (FDs). Implementación de *File Locking* a través de fcntl() con F\_SETLK. |

## **5\. Rúbrica de Evaluación**

> * **40% \- Implementación Técnica y Pensamiento Crítico:** Correcto uso de las llamadas al sistema, compilación limpia y robustez de la integración con el Shell evidenciando decisiones de diseño sólidas.  
> * **30% \- Completitud según Equipo:** Cumplimiento estricto de las funcionalidades acumulativas asignadas, validando casos borde.  
> * **15% \- Manejo de Errores:** Verificación de retornos (ej. si open devuelve \-1), usando perror() apropiadamente.  
> * **15% \- Entregables (Sustentación, Demo y Documentación):** Claridad en el video, calidad del PDF justificando la arquitectura, y exhaustividad del script de pruebas.

## **6\. Instrucciones de Entrega (Entregables)**

La entrega oficial del proyecto debe constar estrictamente de los siguientes elementos, los cuales son evaluados en su totalidad:

> 1. **Código Fuente:** Archivos .c y .h debidamente documentados, acompañados del archivo Makefile configurado para compilar correctamente (targets: all, clean). Debe incluir el código actualizado del Shell integrando el editor.  
> 2. **Código de Demostración:** Un script de pruebas o código adicional que permita ejecutar y validar los comandos requeridos, demostrando la robustez del editor ante diferentes escenarios.  
> 3. **Documento de Sustentación (PDF):** Un reporte ejecutivo y técnico en formato PDF. En este documento es **imperativo demostrar el pensamiento crítico** del equipo: justificando minuciosamente las decisiones de diseño, las estructuras de datos seleccionadas, y de manera crucial, la estrategia de integración elegida dentro de la arquitectura del Shell de clase (clasificación de la categoría del comando).  
> 4. **Video Explicativo:** Un video corto (máximo 5 a 7 minutos) complementario al PDF, donde los integrantes presenten de forma dinámica la estrategia diseñada, expongan el funcionamiento y sustenten cómo abordaron los retos técnicos impuestos por el sistema operativo.

Presentar las evidencias del trabajo antes de la semana 14, en el buzón de interactiva. Cada integrante del equipo debe enviar las evidencias.