#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/protocolo.h"

/* Diccionarios para Bag of Words */
const char *dic_correo[] = {
    "thank", "please", "regards", "meeting", "attached", 
    "information", "update", "schedule", "team", "project", NULL
};

const char *dic_articulo[] = {
    "data", "analysis", "results", "method", "study", 
    "model", "research", "system", "significant", "effect", NULL
};

const char *dic_reporte[] = {
    "system", "data", "network", "security", "application", 
    "server", "user", "performance", "service", "infrastructure", NULL
};

/* Estructura de documentos y oraciones */
typedef struct {
    int id_ventana;
    char buffer_oracion[TAM_MAX_ORACION];
    int pos_buffer;
    int conteo_clases[NUM_CLASES];
    ClaseDocumento clase_asignada;
    int activa;
} DocumentoVentana;

typedef struct {
    int id_ventana;
    char texto[TAM_MAX_ORACION];
} OracionPendiente;

DocumentoVentana docs[MAX_VENTANAS];
OracionPendiente cola_oraciones[TAM_COLA_ORACIONES];
int fin_cola = 0;
int total_oraciones_pendientes = 0;
int parametro_P = P_DEFECTO;

int total_ventanas_esperadas = 0;
int ventanas_finalizadas = 0;

/* Sincronización */
pthread_mutex_t mutex_sistema = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_loader = PTHREAD_COND_INITIALIZER;

void a_minusculas(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower((unsigned char)str[i]);
    }
}

/* Evalúa y clasifica el tipo de usuario de manera asincrónica */
void evaluar_tipo_usuario() {
    int docs_por_clase[NUM_CLASES] = {0};
    int total_docs = 0;

    for (int i = 0; i < MAX_VENTANAS; i++) {
        if (docs[i].clase_asignada != CLASE_DESCONOCIDA) {
            docs_por_clase[docs[i].clase_asignada]++;
            total_docs++;
        }
    }

    int tiene_correo = (docs_por_clase[CLASE_CORREO] > 0);
    int tiene_articulo = (docs_por_clase[CLASE_ARTICULO] > 0);
    int tiene_reporte = (docs_por_clase[CLASE_REPORTE] > 0);

    TipoUsuario perfil = USUARIO_INDETERMINADO;

    if (tiene_correo && !tiene_articulo && tiene_reporte) {
        perfil = USUARIO_ADMINISTRATIVO; // Según tabla de decisión ajustada
    } else if (tiene_correo && !tiene_articulo && !tiene_reporte) {
        perfil = USUARIO_TECNICO;
    } else if (tiene_correo && tiene_articulo && !tiene_reporte) {
        perfil = USUARIO_PROFESOR;
    } else if (!tiene_correo && tiene_articulo && tiene_reporte) {
        perfil = USUARIO_ESTUDIANTE;
    }

    const char *nombres_usuarios[] = {
        "Personal Administrativo", "Personal Técnico", 
        "Profesor", "Estudiante", "Indeterminado"
    };

    printf("\n================================================\n");
    printf("  CONTEXTO DE USUARIO ACTUALIZADO: [%s]\n", nombres_usuarios[perfil]);
    printf("================================================\n");
}

/* Función ejecutada en paralelo por cada hilo del lote P */
void *analizar_oracion_paralelo(void *arg) {
    OracionPendiente *op = (OracionPendiente *)arg;
    
    int conteo_local[NUM_CLASES] = {0};
    char copia[TAM_MAX_ORACION];
    strncpy(copia, op->texto, sizeof(copia));
    copia[sizeof(copia) - 1] = '\0';

    char *token = strtok(copia, " \t\r\n.,;:!?");
    while (token != NULL) {
        char palabra[TAM_MAX_PALABRA];
        strncpy(palabra, token, sizeof(palabra));
        palabra[sizeof(palabra) - 1] = '\0';
        a_minusculas(palabra);

        for (int i = 0; dic_correo[i] != NULL; i++) {
            if (strcmp(palabra, dic_correo[i]) == 0) { conteo_local[CLASE_CORREO]++; break; }
        }
        for (int i = 0; dic_articulo[i] != NULL; i++) {
            if (strcmp(palabra, dic_articulo[i]) == 0) { conteo_local[CLASE_ARTICULO]++; break; }
        }
        for (int i = 0; dic_reporte[i] != NULL; i++) {
            if (strcmp(palabra, dic_reporte[i]) == 0) { conteo_local[CLASE_REPORTE]++; break; }
        }
        token = strtok(NULL, " \t\r\n.,;:!?");
    }

    pthread_mutex_lock(&mutex_sistema);
    DocumentoVentana *doc = &docs[op->id_ventana % MAX_VENTANAS];
    for (int c = 0; c < NUM_CLASES; c++) {
        doc->conteo_clases[c] += conteo_local[c];
    }

    // Determinar clase del documento si cumple el mínimo de 3
    int max_freq = 0;
    ClaseDocumento elegida = doc->clase_asignada;
    for (int c = 0; c < NUM_CLASES; c++) {
        if (doc->conteo_clases[c] >= MIN_COINCIDENCIAS && doc->conteo_clases[c] > max_freq) {
            max_freq = doc->conteo_clases[c];
            elegida = (ClaseDocumento)c;
        }
    }
    doc->clase_asignada = elegida;

    printf("[IALearner Paralelo] Oración de Ventana %d analizada. Clase actual del doc: %d\n", op->id_ventana, elegida);
    evaluar_tipo_usuario();
    
    pthread_mutex_unlock(&mutex_sistema);
    free(op);
    return NULL;
}

/* Hilo Loader: Controla el límite P y lanza los hilos de análisis en paralelo */
void *hilo_loader(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&mutex_sistema);
        while (total_oraciones_pendientes < parametro_P) {
            pthread_cond_wait(&cond_loader, &mutex_sistema);
        }

        printf("\n[Loader] Límite P=%d alcanzado. Despertando hilos de análisis en paralelo...\n", parametro_P);

        // Extraer lote de P oraciones
        OracionPendiente *lote = malloc(sizeof(OracionPendiente) * parametro_P);
        for (int i = 0; i < parametro_P; i++) {
            lote[i] = cola_oraciones[i];
        }
        // Desplazar cola
        int restantes = total_oraciones_pendientes - parametro_P;
        for (int i = 0; i < restantes; i++) {
            cola_oraciones[i] = cola_oraciones[i + parametro_P];
        }
        total_oraciones_pendientes = restantes;
        fin_cola = total_oraciones_pendientes;

        pthread_mutex_unlock(&mutex_sistema);

        // Crear P hilos para ejecutar el análisis en paralelo simultáneamente
        pthread_t hilos_P[P_DEFECTO];
        for (int i = 0; i < parametro_P; i++) {
            OracionPendiente *op = malloc(sizeof(OracionPendiente));
            *op = lote[i];
            pthread_create(&hilos_P[i], NULL, analizar_oracion_paralelo, op);
        }

        // Esperar a que terminen los P hilos del lote
        for (int i = 0; i < parametro_P; i++) {
            pthread_join(hilos_P[i], NULL);
        }

        free(lote);
        printf("[Loader] Lote de P oraciones procesado en paralelo con éxito.\n");
    }
    return NULL;
}

/* Manejador de cada conexión cliente */
void *atender_cliente(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);

    char buffer[TAM_MAX_MSG];
    int id_ventana = 0;
    DocumentoVentana *mi_doc = NULL;

    FILE *socket_stream = fdopen(client_sock, "r");
    if (!socket_stream) {
        close(client_sock);
        return NULL;
    }

    while (fgets(buffer, sizeof(buffer), socket_stream) != NULL) {
        buffer[strcspn(buffer, "\r\n")] = 0;
        char comando[32];
        sscanf(buffer, "%s", comando);

        if (strcmp(comando, PROTO_TOTAL) == 0) {
            sscanf(buffer, "%*s %d", &total_ventanas_esperadas);
        } else if (strcmp(comando, PROTO_ID) == 0) {
            sscanf(buffer, "%*s %d", &id_ventana);
            pthread_mutex_lock(&mutex_sistema);
            mi_doc = &docs[id_ventana % MAX_VENTANAS];
            mi_doc->id_ventana = id_ventana;
            mi_doc->activa = 1;
            mi_doc->pos_buffer = 0;
            mi_doc->clase_asignada = CLASE_DESCONOCIDA;
            memset(mi_doc->conteo_clases, 0, sizeof(mi_doc->conteo_clases));
            memset(mi_doc->buffer_oracion, 0, sizeof(mi_doc->buffer_oracion));
            pthread_mutex_unlock(&mutex_sistema);
        } else if (strcmp(comando, PROTO_CHAR) == 0) {
            char c = buffer[5];
            if (mi_doc && mi_doc->pos_buffer < TAM_MAX_ORACION - 1) {
                mi_doc->buffer_oracion[mi_doc->pos_buffer++] = c;
                mi_doc->buffer_oracion[mi_doc->pos_buffer] = '\0';
            }
        } else if (strcmp(comando, PROTO_RET) == 0) {
            if (mi_doc && strlen(mi_doc->buffer_oracion) > 0) {
                pthread_mutex_lock(&mutex_sistema);
                if (total_oraciones_pendientes < TAM_COLA_ORACIONES) {
                    cola_oraciones[fin_cola].id_ventana = mi_doc->id_ventana;
                    strncpy(cola_oraciones[fin_cola].texto, mi_doc->buffer_oracion, TAM_MAX_ORACION);
                    fin_cola++;
                    total_oraciones_pendientes++;
                    
                    printf("[IALearner] Oración acumulada de Ventana %d. Pendientes: %d/%d\n", 
                           mi_doc->id_ventana, total_oraciones_pendientes, parametro_P);

                    // Si se alcanza el parámetro P, se avisa al hilo Loader
                    if (total_oraciones_pendientes >= parametro_P) {
                        pthread_cond_signal(&cond_loader);
                    }
                }
                memset(mi_doc->buffer_oracion, 0, sizeof(mi_doc->buffer_oracion));
                mi_doc->pos_buffer = 0;
                pthread_mutex_unlock(&mutex_sistema);
            }
        } else if (strcmp(comando, PROTO_FIN) == 0) {
            break;
        }
    }

    pthread_mutex_lock(&mutex_sistema);
    if (mi_doc) mi_doc->activa = 0;
    ventanas_finalizadas++;
    pthread_mutex_unlock(&mutex_sistema);

    fclose(socket_stream);
    close(client_sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        parametro_P = atoi(argv[1]);
        if (parametro_P <= 0) parametro_P = P_DEFECTO;
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PUERTO_DEFECTO);

    bind(server_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    listen(server_sock, MAX_VENTANAS);

    printf("=====================================================\n");
    printf("   [IALearner V2] Servidor Activo (Parámetro P = %d)   \n", parametro_P);
    printf("=====================================================\n");

    // Iniciar el hilo Loader independiente
    pthread_t tid_loader;
    pthread_create(&tid_loader, NULL, hilo_loader, NULL);
    pthread_detach(tid_loader);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr *)&cli_addr, &cli_len);

        pthread_t tid;
        pthread_create(&tid, NULL, atender_cliente, client_sock);
        pthread_detach(tid);
    }

    close(server_sock);
    return 0;
}