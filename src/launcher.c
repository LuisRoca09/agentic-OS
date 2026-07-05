#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

#include "../include/protocolo.h"

/* ══════════════════════════════════════════════
 * TABLA DE PROCESOS HIJOS (compartida con el manejador de SIGCHLD)
 * ══════════════════════════════════════════════ */
static ProcesoHijo g_procesos[MAX_VENTANAS];
static volatile sig_atomic_t g_cantidad_procesos = 0;
static volatile sig_atomic_t g_terminados         = 0;

/* Guardados en main() para reutilizarlos en lanzar_ventanas_extra() */
static const char *g_host   = NULL;
static int         g_puerto = 0;

/* ══════════════════════════════════════════════
 * MANEJADOR DE SIGCHLD
 * El bucle es obligatorio: si dos hijos terminan casi al mismo
 * tiempo, el kernel puede colapsar sus SIGCHLD en una sola señal.
 * Sin el bucle, un waitpid() se perdería y quedaria un zombie.
 * ══════════════════════════════════════════════ */
static void manejar_sigchld(int sig) {
    (void)sig;
    int   estado;
    pid_t pid;

    while ((pid = waitpid(-1, &estado, WNOHANG)) > 0) {
        for (int i = 0; i < g_cantidad_procesos; i++) {
            if (g_procesos[i].pid == pid) {
                g_procesos[i].estado        = PROC_TERMINADO;
                g_procesos[i].codigo_salida = WIFEXITED(estado) ? WEXITSTATUS(estado) : -1;
                g_terminados++;
                break;
            }
        }
    }
}

/* ══════════════════════════════════════════════
 * NOTIFICAR TOTAL AL IALEARNER
 * Conexion TCP corta: solo envia "TOTAL <n>" y cierra.
 * ══════════════════════════════════════════════ */
static int notificar_total(const char *host, int puerto, int n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket (TOTAL)");
        return -1;
    }

    struct sockaddr_in direccion;
    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family = AF_INET;
    direccion.sin_port   = htons((uint16_t)puerto);

    if (inet_pton(AF_INET, host, &direccion.sin_addr) <= 0) {
        fprintf(stderr, "[Launcher] Host invalido: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&direccion, sizeof(direccion)) < 0) {
        fprintf(stderr, "[Launcher] No se pudo conectar a IALearner (%s:%d): %s\n",
                host, puerto, strerror(errno));
        close(fd);
        return -1;
    }

    char mensaje[TAM_MAX_MSG];
    snprintf(mensaje, sizeof(mensaje), "%s %d\n", PROTO_TOTAL, n);
    send(fd, mensaje, strlen(mensaje), MSG_NOSIGNAL);
    close(fd);

    printf("[Launcher] IALearner notificado: esperar %d ventana(s)\n", n);
    return 0;
}

/* ══════════════════════════════════════════════
 * LANZAR UN CLIENTE X11 (fork + execv)
 * ══════════════════════════════════════════════ */
static pid_t lanzar_cliente(int id_ventana, const char *host, int puerto) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* ── proceso hijo ── */
        char str_id[16], str_puerto[16];
        snprintf(str_id, sizeof(str_id), "%d", id_ventana);
        snprintf(str_puerto, sizeof(str_puerto), "%d", puerto);

        char *args[] = {
            (char *)"./ventana_x11",
            (char *)host,
            str_puerto,
            str_id,
            NULL
        };

        execv("./ventana_x11", args);
        /* si execv retorna, fallo */
        fprintf(stderr, "[Launcher] execv fallo: %s\n", strerror(errno));
        _exit(EXIT_FAILURE);
    }

    return pid;  /* proceso padre: retorna el PID del hijo */
}

/* ══════════════════════════════════════════════
 * IMPRIMIR ESTADO DE LOS PROCESOS
 * ══════════════════════════════════════════════ */
static void imprimir_estado(void) {
    printf("\n--------------------------------------------------\n");
    printf(" %-6s %-10s %-12s %-10s\n", "ID", "PID", "Estado", "Cod.Sal.");
    printf("--------------------------------------------------\n");

    for (int i = 0; i < g_cantidad_procesos; i++) {
        const char *estado_txt = (g_procesos[i].estado == PROC_ACTIVO)
                                  ? "ACTIVO" : "TERMINADO";
        if (g_procesos[i].estado == PROC_TERMINADO) {
            printf(" %-6d %-10d %-12s %-10d\n",
                   g_procesos[i].id_ventana, g_procesos[i].pid,
                   estado_txt, g_procesos[i].codigo_salida);
        } else {
            printf(" %-6d %-10d %-12s %-10s\n",
                   g_procesos[i].id_ventana, g_procesos[i].pid,
                   estado_txt, "-");
        }
    }

    printf("--------------------------------------------------\n");
    printf(" Activos: %d | Terminados: %d | Total: %d\n\n",
           (int)(g_cantidad_procesos - g_terminados),
           (int)g_terminados, (int)g_cantidad_procesos);
}

/* ══════════════════════════════════════════════
 * CERRAR TODOS LOS PROCESOS ACTIVOS (SIGTERM, luego SIGKILL)
 * ══════════════════════════════════════════════ */
static void cerrar_todos_los_procesos(void) {
    int activos = 0;
    for (int i = 0; i < g_cantidad_procesos; i++)
        if (g_procesos[i].estado == PROC_ACTIVO) activos++;

    if (activos == 0) {
        printf("[Launcher] No hay ventanas activas.\n");
        return;
    }

    printf("[Launcher] Enviando SIGTERM a %d ventana(s)...\n", activos);
    for (int i = 0; i < g_cantidad_procesos; i++)
        if (g_procesos[i].estado == PROC_ACTIVO)
            kill(g_procesos[i].pid, SIGTERM);

    /* esperar hasta 2s en pasos de 100ms; sigchld_handler actualiza estados */
    for (int intento = 0; intento < 20; intento++) {
        usleep(100000);
        int todos_terminados = 1;
        for (int i = 0; i < g_cantidad_procesos; i++)
            if (g_procesos[i].estado == PROC_ACTIVO) { todos_terminados = 0; break; }
        if (todos_terminados) break;
    }

    /* forzar con SIGKILL los que sigan vivos */
    int forzados = 0;
    for (int i = 0; i < g_cantidad_procesos; i++) {
        if (g_procesos[i].estado == PROC_ACTIVO) {
            kill(g_procesos[i].pid, SIGKILL);
            forzados++;
        }
    }
    if (forzados > 0)
        printf("[Launcher] SIGKILL enviado a %d proceso(s) sin respuesta\n", forzados);

    /* recoger zombies residuales */
    int estado;
    while (waitpid(-1, &estado, WNOHANG) > 0)
        ;

    printf("[Launcher] Ventanas cerradas.\n");
}

/* ══════════════════════════════════════════════
 * LANZAR VENTANAS ADICIONALES (opcion de menu)
 * ══════════════════════════════════════════════ */
static void lanzar_ventanas_extra(int n) {
    if (n < 1) return;

    if (g_cantidad_procesos + n > MAX_VENTANAS) {
        fprintf(stderr, "[Launcher] %d + %d supera el maximo de %d ventanas.\n",
                (int)g_cantidad_procesos, n, MAX_VENTANAS);
        return;
    }

    int nuevo_total = (int)g_cantidad_procesos + n;
    if (notificar_total(g_host, g_puerto, nuevo_total) < 0)
        fprintf(stderr, "[Launcher] ADVERTENCIA: sin conexion a IALearner\n");

    for (int i = 0; i < n; i++) {
        int id = (int)g_cantidad_procesos + 1;
        pid_t pid = lanzar_cliente(id, g_host, g_puerto);
        if (pid < 0) {
            fprintf(stderr, "[Launcher] Fallo al lanzar ventana %d\n", id);
            continue;
        }
        int idx = (int)g_cantidad_procesos;
        g_procesos[idx].pid           = pid;
        g_procesos[idx].id_ventana    = id;
        g_procesos[idx].estado        = PROC_ACTIVO;
        g_procesos[idx].codigo_salida = 0;
        g_cantidad_procesos++;

        printf("[Launcher] Ventana %d lanzada (PID %d)\n", id, pid);
        usleep(100000);
    }
}

/* ══════════════════════════════════════════════
 * MENU INTERACTIVO
 * ══════════════════════════════════════════════ */
static void imprimir_menu(void) {
    printf("==================================\n");
    printf("       AGENTIC-OS LAUNCHER        \n");
    printf("==================================\n");
    printf(" 1. Ver estado de procesos\n");
    printf(" 2. Cerrar todas las ventanas\n");
    printf(" 3. Lanzar N ventanas nuevas\n");
    printf(" 4. Salir\n");
    printf("==================================\n");
    printf("Opcion: ");
    fflush(stdout);
}

/* ══════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    /* validacion defensiva de argumentos */
    if (argc < 2 || argc > 4) {
        fprintf(stderr,
            "Uso: %s <n_ventanas> [host_ialearner] [puerto]\n"
            "  Ejemplo: %s 3 127.0.0.1 9500\n",
            argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    char *fin_parse;
    long n_ventanas = strtol(argv[1], &fin_parse, 10);
    if (*fin_parse != '\0' || n_ventanas < 1 || n_ventanas > MAX_VENTANAS) {
        fprintf(stderr, "n_ventanas debe ser un entero entre 1 y %d\n", MAX_VENTANAS);
        return EXIT_FAILURE;
    }

    const char *host = (argc >= 3) ? argv[2] : HOST_DEFECTO;
    int puerto = PUERTO_DEFECTO;
    if (argc == 4) {
        long p = strtol(argv[3], &fin_parse, 10);
        if (*fin_parse != '\0' || p <= 0 || p > 65535) {
            fprintf(stderr, "Puerto invalido: %s\n", argv[3]);
            return EXIT_FAILURE;
        }
        puerto = (int)p;
    }

    /* verificar que el binario del cliente exista antes de arrancar nada */
    if (access("./ventana_x11", X_OK) != 0) {
        fprintf(stderr,
            "[Launcher] No se encontro './ventana_x11' o no es ejecutable.\n"
            "  Compilalo primero: gcc src/ventana_x11.c -o ventana_x11 -lX11\n");
        return EXIT_FAILURE;
    }

    g_host   = host;
    g_puerto = puerto;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejar_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    printf("[Launcher] Agentic-OS - %ld ventana(s) | IALearner en %s:%d\n\n",
           n_ventanas, host, puerto);

    if (notificar_total(host, puerto, (int)n_ventanas) < 0)
        fprintf(stderr, "[Launcher] ADVERTENCIA: sin conexion a IALearner\n");

    memset(g_procesos, 0, sizeof(g_procesos));

    for (int i = 0; i < (int)n_ventanas; i++) {
        int id = i + 1;
        pid_t pid = lanzar_cliente(id, host, puerto);
        if (pid < 0) {
            fprintf(stderr, "[Launcher] Fallo al lanzar ventana %d\n", id);
            cerrar_todos_los_procesos();
            return EXIT_FAILURE;
        }
        g_procesos[i].pid           = pid;
        g_procesos[i].id_ventana    = id;
        g_procesos[i].estado        = PROC_ACTIVO;
        g_procesos[i].codigo_salida = 0;
        g_cantidad_procesos++;

        printf("[Launcher] Ventana %d lanzada (PID %d)\n", id, pid);
        usleep(100000);
    }

    printf("\n");

    char entrada[64];
    int  ejecutando = 1;

    while (ejecutando) {
        imprimir_menu();

        if (fgets(entrada, sizeof(entrada), stdin) == NULL)
            break;

        entrada[strcspn(entrada, "\n")] = '\0';

        char *fin_opcion;
        long opcion = strtol(entrada, &fin_opcion, 10);
        if (fin_opcion == entrada || *fin_opcion != '\0') {
            printf("Opcion invalida. Ingresa 1, 2, 3 o 4.\n\n");
            continue;
        }

        switch (opcion) {
        case 1:
            imprimir_estado();
            break;

        case 2:
            cerrar_todos_los_procesos();
            continue;  /* evita el chequeo de auto-salida de abajo */

        case 3: {
            int libres = MAX_VENTANAS - (int)g_cantidad_procesos;
            if (libres <= 0) {
                printf("[Launcher] Ya se alcanzo el maximo de %d ventanas.\n\n", MAX_VENTANAS);
                continue;
            }
            printf("Cuantas ventanas nuevas deseas abrir? (max %d): ", libres);
            fflush(stdout);

            char buf_n[32];
            if (fgets(buf_n, sizeof(buf_n), stdin) == NULL) continue;
            buf_n[strcspn(buf_n, "\n")] = '\0';

            char *fin_n;
            long n = strtol(buf_n, &fin_n, 10);
            if (fin_n == buf_n || *fin_n != '\0' || n < 1 || n > libres) {
                printf("[Launcher] Cantidad invalida.\n\n");
                continue;
            }
            lanzar_ventanas_extra((int)n);
            continue;
        }

        case 4:
            ejecutando = 0;
            break;

        default:
            printf("Opcion invalida. Ingresa 1, 2, 3 o 4.\n\n");
            break;
        }

        /* salida automatica si todos los hijos ya terminaron */
        if (g_cantidad_procesos > 0 && g_terminados >= g_cantidad_procesos) {
            printf("[Launcher] Todas las ventanas se cerraron.\n");
            ejecutando = 0;
        }
    }

    cerrar_todos_los_procesos();
    printf("[Launcher] Fin.\n");
    return EXIT_SUCCESS;
}