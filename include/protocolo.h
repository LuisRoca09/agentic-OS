#ifndef PROTOCOLO_H
#define PROTOCOLO_H

/* ══════════════════════════════════════════════
 * Constantes generales del sistema
 * ══════════════════════════════════════════════ */
#define MAX_VENTANAS      16      /* procesos gráficos simultáneos máx. */
#define PUERTO_DEFECTO    9500
#define HOST_DEFECTO      "127.0.0.1"
#define TAM_MAX_MSG       64      /* un mensaje = un carácter, sobra espacio */
#define TAM_MAX_ORACION   512
#define TAM_MAX_PALABRA   32
#define MAX_VOCABULARIO   64
#define MIN_COINCIDENCIAS 3       /* regla del enunciado: mínimo 3 palabras */

/* ══════════════════════════════════════════════
 * Prefijos del protocolo (mensajes terminados en '\n')
 *   ID <n>      -> la ventana n se identifica ante IALearner
 *   CHAR <c>    -> un carácter fue tecleado (c puede ser una letra o ' ')
 *   RET         -> el usuario presionó Enter (fin de oración)
 *   FIN         -> la ventana se cerró
 *   TOTAL <n>   -> el launcher avisa cuántas ventanas esperar
 *
 * Nota: se usa el prefijo PROTO_ en vez de MSG_ porque MSG_ choca
 * con macros ya definidas en sys/socket.h (ej. MSG_FIN, MSG_PEEK).
 * ══════════════════════════════════════════════ */
#define PROTO_ID     "ID"
#define PROTO_CHAR   "CHAR"
#define PROTO_RET    "RET"
#define PROTO_FIN    "FIN"
#define PROTO_TOTAL  "TOTAL"

/* ══════════════════════════════════════════════
 * Clases de documento y tipos de usuario
 * ══════════════════════════════════════════════ */
typedef enum { CLASE_CORREO = 0, CLASE_ARTICULO, CLASE_REPORTE,
               NUM_CLASES, CLASE_DESCONOCIDA } ClaseDocumento;

typedef enum { USUARIO_ADMINISTRATIVO = 0, USUARIO_TECNICO,
               USUARIO_PROFESOR, USUARIO_ESTUDIANTE,
               USUARIO_INDETERMINADO } TipoUsuario;

/* ══════════════════════════════════════════════
 * Estado de un proceso hijo (para el launcher)
 * ══════════════════════════════════════════════ */
typedef enum { PROC_ACTIVO, PROC_TERMINADO } EstadoProceso;

typedef struct {
    pid_t         pid;
    int           id_ventana;
    EstadoProceso estado;
    int           codigo_salida;
} ProcesoHijo;

#endif