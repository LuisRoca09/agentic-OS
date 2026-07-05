#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>

#include "../include/protocolo.h"

/* ══════════════════════════════════════════════
 * MUTEX GLOBAL DE CONSOLA
 * Evita que los printf de distintos hilos se entrelacen.
 * ══════════════════════════════════════════════ */
static pthread_mutex_t g_mutex_consola = PTHREAD_MUTEX_INITIALIZER;
#define CONSOLA_LOCK()   pthread_mutex_lock(&g_mutex_consola)
#define CONSOLA_UNLOCK() pthread_mutex_unlock(&g_mutex_consola)

static volatile sig_atomic_t g_activo = 1;

/* ══════════════════════════════════════════════
 * DICCIONARIOS (tal como los dio el profesor, en inglés)
 * ══════════════════════════════════════════════ */
static const char *DICC_CORREO[] = {
    "thank", "please", "regards", "meeting", "attached",
    "information", "update", "schedule", "team", "project", NULL
};
static const char *DICC_ARTICULO[] = {
    "data", "analysis", "results", "method", "study",
    "model", "research", "system", "significant", "effect", NULL
};
static const char *DICC_REPORTE[] = {
    "system", "data", "network", "security", "application",
    "server", "user", "performance", "service", "infrastructure", NULL
};
static const char **DICCIONARIOS[NUM_CLASES] = {
    DICC_CORREO, DICC_ARTICULO, DICC_REPORTE
};
static const char *NOMBRE_CLASE[NUM_CLASES] = {
    "Correo electronico", "Articulo cientifico", "Reporte"
};
static const char *NOMBRE_USUARIO[] = {
    "Personal administrativo", "Personal tecnico",
    "Profesor", "Estudiante", "Indeterminado"
};

/* ══════════════════════════════════════════════
 * TDA: BOLSA DE PALABRAS (bag of words)
 * ══════════════════════════════════════════════ */
typedef struct {
    char palabra[TAM_MAX_PALABRA];
    int  frecuencia;
} EntradaFrecuencia;

typedef struct {
    EntradaFrecuencia entradas[MAX_VOCABULARIO];
    int tamano;
} BolsaPalabras;

static void bolsa_inicializar(BolsaPalabras *b) {
    memset(b, 0, sizeof(*b));
}

static void a_minusculas(const char *origen, char *destino, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && origen[i]; i++)
        destino[i] = (char)tolower((unsigned char)origen[i]);
    destino[i] = '\0';
}

static void bolsa_agregar(BolsaPalabras *b, const char *palabra) {
    if (!palabra || palabra[0] == '\0') return;

    char minus[TAM_MAX_PALABRA];
    a_minusculas(palabra, minus, sizeof(minus));

    for (int i = 0; i < b->tamano; i++) {
        if (strncmp(b->entradas[i].palabra, minus, TAM_MAX_PALABRA) == 0) {
            b->entradas[i].frecuencia++;
            return;
        }
    }
    if (b->tamano < MAX_VOCABULARIO) {
        strncpy(b->entradas[b->tamano].palabra, minus, TAM_MAX_PALABRA - 1);
        b->entradas[b->tamano].frecuencia = 1;
        b->tamano++;
    }
}

static int bolsa_frecuencia(const BolsaPalabras *b, const char *palabra) {
    char minus[TAM_MAX_PALABRA];
    a_minusculas(palabra, minus, sizeof(minus));
    for (int i = 0; i < b->tamano; i++)
        if (strncmp(b->entradas[i].palabra, minus, TAM_MAX_PALABRA) == 0)
            return b->entradas[i].frecuencia;
    return 0;
}

/* ══════════════════════════════════════════════
 * TDA: DOCUMENTO (una ventana)
 * ══════════════════════════════════════════════ */
typedef struct {
    int             id_ventana;
    int             en_uso;
    BolsaPalabras   bolsa;              /* acumulada durante toda la sesion */
    ClaseDocumento  clase;

    char            oracion_actual[TAM_MAX_ORACION]; /* solo para mostrar  */
    char            palabra_actual[TAM_MAX_PALABRA];  /* palabra en armado */
    int             pos_palabra;

    pthread_mutex_t mutex;
} RegistroDocumento;

/* ══════════════════════════════════════════════
 * TDA: TABLA DE DOCUMENTOS (estado global compartido)
 * ══════════════════════════════════════════════ */
typedef struct {
    RegistroDocumento documentos[MAX_VENTANAS];
    int             registrados;
    int             terminados;
    int             total_esperado;   /* 0 = aun no informado por Launcher */
    pthread_mutex_t mutex;
    pthread_cond_t  cambio;
} TablaDocumentos;

static TablaDocumentos g_tabla;

static void tabla_inicializar(TablaDocumentos *t) {
    memset(t, 0, sizeof(*t));
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->cambio, NULL);
    for (int i = 0; i < MAX_VENTANAS; i++) {
        pthread_mutex_init(&t->documentos[i].mutex, NULL);
        t->documentos[i].clase = CLASE_DESCONOCIDA;
        bolsa_inicializar(&t->documentos[i].bolsa);
    }
}

static RegistroDocumento *tabla_registrar(TablaDocumentos *t, int id_ventana) {
    RegistroDocumento *slot = NULL;
    pthread_mutex_lock(&t->mutex);
    for (int i = 0; i < MAX_VENTANAS; i++) {
        if (!t->documentos[i].en_uso) {
            slot = &t->documentos[i];
            slot->en_uso            = 1;
            slot->id_ventana        = id_ventana;
            slot->clase             = CLASE_DESCONOCIDA;
            slot->oracion_actual[0] = '\0';
            slot->palabra_actual[0] = '\0';
            slot->pos_palabra       = 0;
            bolsa_inicializar(&slot->bolsa);
            t->registrados++;
            break;
        }
    }
    pthread_mutex_unlock(&t->mutex);
    return slot;
}

static void tabla_marcar_terminado(TablaDocumentos *t) {
    pthread_mutex_lock(&t->mutex);
    t->terminados++;
    pthread_cond_broadcast(&t->cambio);
    pthread_mutex_unlock(&t->mutex);
}

static void tabla_fijar_total(TablaDocumentos *t, int n) {
    pthread_mutex_lock(&t->mutex);
    t->total_esperado = n;
    pthread_cond_broadcast(&t->cambio);
    pthread_mutex_unlock(&t->mutex);
}

/* ══════════════════════════════════════════════
 * CLASIFICADOR DE DOCUMENTO
 * ══════════════════════════════════════════════ */
static int puntuar_clase(const BolsaPalabras *b, const char **diccionario, int *coincidencias) {
    int suma = 0, matches = 0;
    for (int i = 0; diccionario[i] != NULL; i++) {
        int f = bolsa_frecuencia(b, diccionario[i]);
        if (f > 0) { suma += f; matches++; }
    }
    if (coincidencias) *coincidencias = matches;
    return suma;
}

static ClaseDocumento clasificar_documento(const BolsaPalabras *b, int id_ventana) {
    ClaseDocumento mejor = CLASE_DESCONOCIDA;
    int mejor_suma = -1;

    CONSOLA_LOCK();
    printf("\n[IALearner] Clasificando ventana %d:\n", id_ventana);
    for (int c = 0; c < NUM_CLASES; c++) {
        int matches = 0;
        int suma = puntuar_clase(b, DICCIONARIOS[c], &matches);
        printf("  %-22s -> %d coincidencias, frecuencia total = %d",
               NOMBRE_CLASE[c], matches, suma);
        if (matches >= MIN_COINCIDENCIAS) {
            printf(" [ELEGIBLE]");
            if (suma > mejor_suma) { mejor_suma = suma; mejor = (ClaseDocumento)c; }
        }
        printf("\n");
    }
    if (mejor == CLASE_DESCONOCIDA)
        printf("  -> No clasificable (menos de %d coincidencias en cualquier diccionario)\n",
               MIN_COINCIDENCIAS);
    else
        printf("  -> Clase asignada: %s\n", NOMBRE_CLASE[mejor]);
    CONSOLA_UNLOCK();

    return mejor;
}

/* ══════════════════════════════════════════════
 * INFERENCIA DE TIPO DE USUARIO
 * ══════════════════════════════════════════════ */
static TipoUsuario inferir_tipo_usuario(const TablaDocumentos *t) {
    int hay_correo = 0, hay_articulo = 0, hay_reporte = 0, total = 0;

    for (int i = 0; i < MAX_VENTANAS; i++) {
        if (!t->documentos[i].en_uso) continue;
        total++;
        switch (t->documentos[i].clase) {
            case CLASE_CORREO:   hay_correo   = 1; break;
            case CLASE_ARTICULO: hay_articulo = 1; break;
            case CLASE_REPORTE:  hay_reporte  = 1; break;
            default: break;
        }
    }

    CONSOLA_LOCK();
    printf("\n[IALearner] Resumen de %d documento(s): correo=%d articulo=%d reporte=%d\n",
           total, hay_correo, hay_articulo, hay_reporte);
    CONSOLA_UNLOCK();

    if (total == 0) return USUARIO_INDETERMINADO;

    if (hay_correo && !hay_articulo && !hay_reporte) return USUARIO_ADMINISTRATIVO;
    if (hay_correo && !hay_articulo &&  hay_reporte) return USUARIO_TECNICO;
    if (hay_correo &&  hay_articulo && !hay_reporte) return USUARIO_PROFESOR;
    if (!hay_correo &&  hay_articulo &&  hay_reporte) return USUARIO_ESTUDIANTE;

    return USUARIO_INDETERMINADO;  /* combinacion ambigua */
}

/* ══════════════════════════════════════════════
 * HILO CLASIFICADOR FINAL
 * Espera a que terminen todas las ventanas esperadas.
 * ══════════════════════════════════════════════ */
static void *hilo_clasificador_final(void *arg) {
    (void)arg;

    pthread_mutex_lock(&g_tabla.mutex);
    while (g_activo &&
           !(g_tabla.total_esperado > 0 &&
             g_tabla.terminados >= g_tabla.total_esperado)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_tabla.cambio, &g_tabla.mutex, &ts);
    }
    pthread_mutex_unlock(&g_tabla.mutex);

    if (!g_activo) return NULL;

    TipoUsuario usuario = inferir_tipo_usuario(&g_tabla);

    CONSOLA_LOCK();
    printf("\n================================================\n");
    printf("  CONTEXTO DE USUARIO DETECTADO: %s\n", NOMBRE_USUARIO[usuario]);
    printf("================================================\n\n");
    CONSOLA_UNLOCK();

    return NULL;
}

/* ══════════════════════════════════════════════
 * PROCESAR UN CARACTER RECIBIDO DE UNA VENTANA
 * ══════════════════════════════════════════════ */
static void procesar_char(RegistroDocumento *doc, char c) {
    pthread_mutex_lock(&doc->mutex);

    if (c == ' ') {
        /* fin de palabra: agregarla a la bolsa y reiniciar buffer */
        if (doc->pos_palabra > 0) {
            bolsa_agregar(&doc->bolsa, doc->palabra_actual);
            doc->pos_palabra = 0;
            doc->palabra_actual[0] = '\0';
        }
    } else if (doc->pos_palabra < TAM_MAX_PALABRA - 1) {
        doc->palabra_actual[doc->pos_palabra++] = c;
        doc->palabra_actual[doc->pos_palabra]   = '\0';
    }

    /* buffer de oracion, solo para mostrar en pantalla */
    size_t len = strlen(doc->oracion_actual);
    if (len < TAM_MAX_ORACION - 1) {
        doc->oracion_actual[len]   = c;
        doc->oracion_actual[len+1] = '\0';
    }

    pthread_mutex_unlock(&doc->mutex);
}

static void procesar_return(RegistroDocumento *doc, int id_ventana) {
    pthread_mutex_lock(&doc->mutex);

    /* si quedo una palabra sin espacio final, agregarla tambien */
    if (doc->pos_palabra > 0) {
        bolsa_agregar(&doc->bolsa, doc->palabra_actual);
        doc->pos_palabra = 0;
        doc->palabra_actual[0] = '\0';
    }

    CONSOLA_LOCK();
    printf("[IALearner] Ventana %d - oracion: \"%s\"\n", id_ventana, doc->oracion_actual);
    CONSOLA_UNLOCK();

    doc->oracion_actual[0] = '\0';
    pthread_mutex_unlock(&doc->mutex);
}

/* ══════════════════════════════════════════════
 * HILO POR CONEXION
 * ══════════════════════════════════════════════ */
typedef struct { int fd_cliente; } ArgHilo;

static void *hilo_conexion(void *arg) {
    ArgHilo *a = (ArgHilo *)arg;
    int fd = a->fd_cliente;
    free(a);

    char buffer[TAM_MAX_MSG];
    ssize_t nbytes;
    int id_ventana = -1;
    RegistroDocumento *doc = NULL;

    while ((nbytes = recv(fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[nbytes] = '\0';

        char *guardado;
        char *linea = strtok_r(buffer, "\n", &guardado);

        while (linea != NULL) {
            size_t l = strlen(linea);
            if (l > 0 && linea[l-1] == '\r') linea[--l] = '\0';

            if (l == 0) { linea = strtok_r(NULL, "\n", &guardado); continue; }

            if (strncmp(linea, PROTO_TOTAL, strlen(PROTO_TOTAL)) == 0) {
                long n = strtol(linea + strlen(PROTO_TOTAL) + 1, NULL, 10);
                if (n > 0) {
                    tabla_fijar_total(&g_tabla, (int)n);
                    CONSOLA_LOCK();
                    printf("[IALearner] Launcher informo: esperar %ld ventana(s)\n", n);
                    CONSOLA_UNLOCK();
                }
                close(fd);
                return NULL;   /* conexion corta del Launcher, no es documento */
            }
            else if (strncmp(linea, PROTO_ID, strlen(PROTO_ID)) == 0) {
                long id = strtol(linea + strlen(PROTO_ID) + 1, NULL, 10);
                if (id <= 0 || id > MAX_VENTANAS) {
                    CONSOLA_LOCK();
                    fprintf(stderr, "[IALearner] ID de ventana invalido: %ld\n", id);
                    CONSOLA_UNLOCK();
                    close(fd);
                    return NULL;
                }
                id_ventana = (int)id;
                doc = tabla_registrar(&g_tabla, id_ventana);
                if (!doc) {
                    CONSOLA_LOCK();
                    fprintf(stderr, "[IALearner] Tabla de documentos llena\n");
                    CONSOLA_UNLOCK();
                    close(fd);
                    return NULL;
                }
                CONSOLA_LOCK();
                printf("[IALearner] Ventana %d registrada\n", id_ventana);
                CONSOLA_UNLOCK();
            }
            else if (strncmp(linea, PROTO_CHAR, strlen(PROTO_CHAR)) == 0) {
                if (doc != NULL) {
                    char c = linea[strlen(PROTO_CHAR) + 1];
                    procesar_char(doc, c);
                }
            }
            else if (strncmp(linea, PROTO_RET, strlen(PROTO_RET)) == 0) {
                if (doc != NULL) procesar_return(doc, id_ventana);
            }
            else if (strncmp(linea, PROTO_FIN, strlen(PROTO_FIN)) == 0) {
                goto fin_ventana;
            }

            linea = strtok_r(NULL, "\n", &guardado);
        }
    }

fin_ventana:
    close(fd);

    if (doc != NULL) {
        pthread_mutex_lock(&doc->mutex);
        doc->clase = clasificar_documento(&doc->bolsa, id_ventana);
        pthread_mutex_unlock(&doc->mutex);
        tabla_marcar_terminado(&g_tabla);

        CONSOLA_LOCK();
        printf("[IALearner] Ventana %d finalizada.\n", id_ventana);
        CONSOLA_UNLOCK();
    }

    return NULL;
}

/* ══════════════════════════════════════════════
 * SEÑAL SIGINT
 * ══════════════════════════════════════════════ */
static void manejar_sigint(int sig) {
    (void)sig;
    g_activo = 0;
}

/* ══════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    int puerto = PUERTO_DEFECTO;

    if (argc == 2) {
        char *fin_parse;
        long p = strtol(argv[1], &fin_parse, 10);
        if (*fin_parse != '\0' || p <= 0 || p > 65535) {
            fprintf(stderr, "Puerto invalido: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        puerto = (int)p;
    } else if (argc > 2) {
        fprintf(stderr, "Uso: %s [puerto]\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, manejar_sigint);
    signal(SIGPIPE, SIG_IGN);

    tabla_inicializar(&g_tabla);

    pthread_t hilo_clf;
    pthread_create(&hilo_clf, NULL, hilo_clasificador_final, NULL);

    int fd_servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_servidor < 0) { perror("socket"); return EXIT_FAILURE; }

    int opt = 1;
    setsockopt(fd_servidor, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd_servidor, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in direccion;
    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family      = AF_INET;
    direccion.sin_addr.s_addr = INADDR_ANY;
    direccion.sin_port        = htons((uint16_t)puerto);

    if (bind(fd_servidor, (struct sockaddr *)&direccion, sizeof(direccion)) < 0) {
        perror("bind");
        close(fd_servidor);
        return EXIT_FAILURE;
    }
    if (listen(fd_servidor, MAX_VENTANAS) < 0) {
        perror("listen");
        close(fd_servidor);
        return EXIT_FAILURE;
    }

    printf("[IALearner] Escuchando en puerto %d... (Ctrl+C para detener)\n\n", puerto);

    while (g_activo) {
        struct sockaddr_in dir_cliente;
        socklen_t len_cliente = sizeof(dir_cliente);

        int fd_cliente = accept(fd_servidor, (struct sockaddr *)&dir_cliente, &len_cliente);
        if (fd_cliente < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) break;
            perror("accept");
            break;
        }

        struct timeval sin_tv = { 0, 0 };
        setsockopt(fd_cliente, SOL_SOCKET, SO_RCVTIMEO, &sin_tv, sizeof(sin_tv));

        ArgHilo *arg = malloc(sizeof(ArgHilo));
        if (!arg) { close(fd_cliente); continue; }
        arg->fd_cliente = fd_cliente;

        pthread_t tid;
        if (pthread_create(&tid, NULL, hilo_conexion, arg) != 0) {
            perror("pthread_create");
            free(arg);
            close(fd_cliente);
            continue;
        }
        pthread_detach(tid);
    }

    close(fd_servidor);

    pthread_mutex_lock(&g_tabla.mutex);
    pthread_cond_broadcast(&g_tabla.cambio);
    pthread_mutex_unlock(&g_tabla.mutex);
    pthread_join(hilo_clf, NULL);

    for (int i = 0; i < MAX_VENTANAS; i++)
        pthread_mutex_destroy(&g_tabla.documentos[i].mutex);
    pthread_mutex_destroy(&g_tabla.mutex);
    pthread_cond_destroy(&g_tabla.cambio);

    printf("[IALearner] Servidor detenido.\n");
    return EXIT_SUCCESS;
}