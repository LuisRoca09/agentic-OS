# Agentic-OS

**Autor:** _(completa aquí tu nombre — el enunciado pide que el ZIP de
entrega lleve tu nombre)_

Sistema compuesto por tres programas en C que simulan un "backdoor" en el
API de ventanas X11: cada tecla presionada en una ventana gráfica se envía
por socket TCP a un servidor remoto (`IALearner`), que arma oraciones,
las clasifica con la técnica *bag of words* y, al finalizar todos los
procesos, infiere el **contexto de usuario** (Personal administrativo,
Personal técnico, Profesor o Estudiante).

## Estructura del proyecto

```
agentic-OS/
├── include/
│   └── protocolo.h      # Constantes y protocolo compartido
├── src/
│   ├── ia_learner.c     # Servidor multi-hilo (clasificador)
│   ├── launcher.c       # Consola interactiva (crea/monitorea procesos)
│   └── ventana_x11.c    # Cliente gráfico X11 (envía teclas por socket)
├── ia_learner           # binario compilado
├── launcher             # binario compilado
├── ventana_x11           # binario compilado
└── README.md
```

## Requisitos

- Linux o WSL con servidor X11 corriendo (`echo $DISPLAY` no debe estar vacío)
- `gcc`
- Librería de desarrollo de X11 (`libx11-dev` en Debian/Ubuntu/WSL)
- Librería pthreads (incluida en glibc, no requiere instalación aparte)

Instalar dependencias en Debian/Ubuntu/WSL si falta X11:

```bash
sudo apt update
sudo apt install libx11-dev
```

## Compilación

Desde la raíz del proyecto (`agentic-OS/`):

```bash
gcc src/ia_learner.c  -o ia_learner  -Wall -Wextra -lpthread
gcc src/launcher.c    -o launcher    -Wall -Wextra
gcc src/ventana_x11.c -o ventana_x11 -Wall -Wextra -lX11
```

Verifica que los tres binarios se hayan generado:

```bash
ls -la ia_learner launcher ventana_x11
```

## Ejecución

El sistema requiere **dos terminales**: una para el servidor, otra para el
launcher. El launcher se encarga de todo lo demás (avisar el total de
ventanas al servidor y lanzar los procesos gráficos).

**Terminal 1 — servidor (arrancar siempre primero):**

```bash
./ia_learner
```

Por defecto escucha en el puerto `9500`. Puede indicarse otro puerto:

```bash
./ia_learner 9500
```

**Terminal 2 — launcher:**

```bash
./launcher <N_ventanas> [host] [puerto]
```

Ejemplos:

```bash
./launcher 1                      # 1 ventana, servidor local, puerto 9500
./launcher 3                      # 3 ventanas simultáneas
./launcher 2 127.0.0.1 9500       # host y puerto explícitos
```

Esto:

1. Notifica automáticamente `TOTAL <n>` al `IALearner`
2. Abre `N` ventanas gráficas (`fork()` + `execv()` de `ventana_x11`)
3. Muestra un menú interactivo en consola:

```
1. Ver estado de procesos
2. Cerrar todas las ventanas
3. Lanzar N ventanas nuevas
4. Salir
```

**Uso de cada ventana gráfica:**

- Haz clic sobre la ventana para darle el foco antes de escribir
- Escribe libremente; cada tecla se envía letra por letra al servidor
- Presiona **Enter** para cerrar una oración (el servidor la clasifica al
  terminar el proceso, no en cada línea)
- Presiona **Escape** para cerrar la ventana; el launcher detecta el
  cierre vía `SIGCHLD` sin dejar procesos zombie

Cuando **todas** las ventanas lanzadas por un mismo `launcher` terminan,
`IALearner` muestra en su terminal el contexto de usuario inferido.

## Protocolo interno (IPC vía sockets TCP)

Cada línea enviada por socket termina en `\n`. Los mensajes válidos son:

| Mensaje | Dirección | Significado |
|---|---|---|
| `ID <n>` | ventana → IALearner | La ventana `n` se identifica al conectar |
| `CHAR <c>` | ventana → IALearner | Se presionó la tecla imprimible `c` |
| `RET` | ventana → IALearner | Se presionó Enter (fin de oración) |
| `FIN` | ventana → IALearner | La ventana se cerró (Escape) |
| `TOTAL <n>` | launcher → IALearner | Avisa cuántas ventanas esperar en total |

El launcher usa una conexión TCP corta y separada solo para enviar
`TOTAL <n>` y cerrarla; cada ventana gráfica mantiene su propia conexión
abierta durante toda su ejecución, enviando un mensaje `CHAR` por cada
tecla y terminando con `FIN` antes de cerrar el socket.

## Diccionarios de clasificación (bag of words)

| Correo electrónico | Artículo científico | Reporte |
|---|---|---|
| thank, please, regards, meeting, attached, information, update, schedule, team, project | data, analysis, results, method, study, model, research, system, significant, effect | system, data, network, security, application, server, user, performance, service, infrastructure |

Un documento (ventana) se clasifica en una clase si aparecen al menos
**3 palabras** de su diccionario. Si califica para más de una clase, se
asigna la de mayor frecuencia total acumulada.

## Inferencia de tipo de usuario

Una vez terminan todos los procesos, se evalúa qué clases de documento
estuvieron presentes (al menos un documento de esa clase) y se compara
contra la tabla de referencia:

| Tipo de usuario | Correo | Artículo | Reporte |
|---|---|---|---|
| Personal administrativo | X | | |
| Personal técnico | X | | X |
| Profesor | X | X | |
| Estudiante | | X | X |

> **Nota de diseño:** el enunciado indica que "X puede ser la proporción
> de documentos", sin especificar un umbral numérico ni cómo comparar
> proporciones entre clases. Dado que los cuatro patrones de la tabla son
> combinaciones binarias mutuamente excluyentes (ninguna fila comparte el
> mismo patrón de presencia/ausencia), se optó por interpretar cada `X`
> como presencia binaria de esa clase en el conjunto de documentos. Esta
> interpretación no genera ambigüedad en la salida, ya que no existen dos
> filas con el mismo patrón que requieran desempate por magnitud.

## Notas de diseño (concurrencia e IPC)

- **IALearner** acepta una conexión TCP por ventana y atiende cada una en
  un hilo (`pthread`) independiente y *detached*, con su propia estructura
  de datos (`RegistroDocumento`) protegida por su propio mutex, evitando
  mezclar el contexto de distintas ventanas.
- Un hilo adicional (`hilo_clasificador_final`) espera con
  `pthread_cond_timedwait` hasta que el número de procesos terminados
  alcance el total informado por el launcher, y solo entonces calcula e
  imprime el contexto de usuario.
- Toda escritura a consola pasa por un mutex global (`g_mutex_consola`)
  para evitar que los mensajes de distintos hilos se entrelacen.
- **Launcher** usa `fork()` + `execv()` para crear cada ventana y un
  manejador de `SIGCHLD` con `waitpid(-1, &estado, WNOHANG)` en bucle para
  recoger todos los hijos terminados sin bloquear el menú interactivo y
  sin dejar procesos zombie. Al cerrar, primero intenta `SIGTERM` y, si
  algún proceso no responde en 2 segundos, aplica `SIGKILL`.

## Ejemplo de salida esperada

Con `./ia_learner` corriendo y luego `./launcher 1`, escribiendo en la
ventana `please schedule the meeting and update the project team` +
Enter + Escape, la Terminal del servidor debería mostrar algo como:

```
[IALearner] Launcher informo: esperar 1 ventana(s)
[IALearner] Ventana 1 registrada
[IALearner] Ventana 1 - oracion: "please schedule the meeting and update the project team"

[IALearner] Clasificando ventana 1:
  Correo electronico     -> 6 coincidencias, frecuencia total = 6 [ELEGIBLE]
  Articulo cientifico    -> 0 coincidencias, frecuencia total = 0
  Reporte                -> 0 coincidencias, frecuencia total = 0
  -> Clase asignada: Correo electronico
[IALearner] Ventana 1 finalizada.

[IALearner] Resumen de 1 documento(s): correo=1 articulo=0 reporte=0
================================================
  CONTEXTO DE USUARIO DETECTADO: Personal administrativo
================================================
```

Y en la Terminal del launcher, al elegir la opción `1` del menú después de
cerrar la ventana:

```
--------------------------------------------------
 ID     PID        Estado       Cod.Sal.
--------------------------------------------------
 1      <pid>      TERMINADO    0
--------------------------------------------------
 Activos: 0 | Terminados: 1 | Total: 1
```

## Solución de problemas

- **`bind: Address already in use`**: quedó un `ia_learner` anterior
  escuchando en el puerto. Verifica con `lsof -i :9500` y mata el proceso
  con `kill -9 <PID>` antes de reiniciar.
- **`No se encontro './ventana_x11'`**: compílalo primero (ver sección
  de compilación) y ejecuta el launcher desde la misma carpeta donde
  están los tres binarios.
- **La ventana gráfica no aparece / `Cannot open display`**: confirma que
  tienes un servidor X11 corriendo (en WSL, instala y abre un servidor
  X como VcXsrv/WSLg) y que `echo $DISPLAY` no está vacío.
