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

/* ══════════════════════════════════════════════
 * Diccionarios para Bag of Words
 * ══════════════════════════════════════════════ */
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

/* ══════════════════════════════════════════════
 * Estructuras de datos para el contexto
 * ══════════════════════════════════════════════ */
typedef struct {
    int id_ventana;
    char buffer_oracion[TAM_MAX_ORACION];
    int pos_buffer;
    int conteo_clases[NUM_CLASES]; // Frecuencias totales por clase
    int palabras_unicas[NUM_CLASES]; // Palabras distintas encontradas
    ClaseDocumento clase_asignada;
    int activa;
} DocumentoVentana;

DocumentoVentana docs[MAX_VENTANAS];
pthread_mutex_t mutex_docs = PTHREAD_MUTEX_INITIALIZER;
int total_ventanas_esperadas = 0;
int ventanas_finalizadas = 0;

/* Convierte un string a minúsculas */
void a_minusculas(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower((unsigned char)str[i]);
    }
}

/* Procesa una oración mediante Bag of Words */
void procesar_oracion(DocumentoVentana *doc) {
    if (strlen(doc->buffer_oracion) == 0) return;

    char copia[TAM_MAX_ORACION];
    strncpy(copia, doc->buffer_oracion, sizeof(copia));
    copia[sizeof(copia) - 1] = '\0';
    
    char *token = strtok(copia, " \t\r\n.,;:!?");
    while (token != NULL) {
        char palabra[TAM_MAX_PALABRA];
        strncpy(palabra, token, sizeof(palabra));
        palabra[sizeof(palabra) - 1] = '\0';
        a_minusculas(palabra);

        // Comparar con Diccionario Correo
        for (int i = 0; dic_correo[i] != NULL; i++) {
            if (strcmp(palabra, dic_correo[i]) == 0) {
                doc->conteo_clases[CLASE_CORREO]++;
                break;
            }
        }
        // Comparar con Diccionario Artículo
        for (int i = 0; dic_articulo[i] != NULL; i++) {
            if (strcmp(palabra, dic_articulo[i]) == 0) {
                doc->conteo_clases[CLASE_ARTICULO]++;
                break;
            }
        }
        // Comparar con Diccionario Reporte
        for (int i = 0; dic_reporte[i] != NULL; i++) {
            if (strcmp(palabra, dic_reporte[i]) == 0) {
                doc->conteo_clases[CLASE_REPORTE]++;
                break;
            }
        }

        token = strtok(NULL, " \t\r\n.,;:!?");
    }

    // Limpiar el buffer de la oración
    memset(doc->buffer_oracion, 0, sizeof(doc->buffer_oracion));
    doc->pos_buffer = 0;
}

/* Clasifica el documento de una ventana terminada */
void clasificar_documento(DocumentoVentana *doc) {
    int max_frecuencia = 0;
    ClaseDocumento elegida = CLASE_DESCONOCIDA;

    for (int c = 0; c < NUM_CLASES; c++) {
        if (doc->conteo_clases[c] >= MIN_COINCIDENCIAS) {
            if (doc->conteo_clases[c] > max_frecuencia) {
                max_frecuencia = doc->conteo_clases[c];
                elegida = (ClaseDocumento)c;
            }
        }
    }

    doc->clase_asignada = elegida;
    const char *nombres_clases[] = {"Correo electrónico", "Artículo científico", "Reporte", "Desconocido"};
    printf("\n[IALearner] -> Ventana %d clasificada como: [%s] (Frecuencias: Correo=%d, Articulo=%d, Reporte=%d)\n",
           doc->id_ventana, nombres_clases[elegida], 
           doc->conteo_clases[CLASE_CORREO], doc->conteo_clases[CLASE_ARTICULO], doc->conteo_clases[CLASE_REPORTE]);
}

/* Determina el tipo de usuario final en base a todos los documentos */
void inferir_tipo_usuario() {
    int docs_por_clase[NUM_CLASES] = {0};
    int total_docs = 0;

    for (int i = 0; i < MAX_VENTANAS; i++) {
        if (docs[i].clase_asignada != CLASE_DESCONOCIDA) {
            docs_por_clase[docs[i].clase_asignada]++;
            total_docs++;
        }
    }

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("           RESULTADO DE INFERENCIA DEL CONTEXTO            \n");
    printf("════════════════════════════════════════════════════════════\n");
    
    int tiene_correo = (docs_por_clase[CLASE_CORREO] > 0);
    int tiene_articulo = (docs_por_clase[CLASE_ARTICULO] > 0);
    int tiene_reporte = (docs_por_clase[CLASE_REPORTE] > 0);

    TipoUsuario perfil = USUARIO_INDETERMINADO;

    if (tiene_correo && !tiene_articulo && !tiene_reporte) {
        perfil = USUARIO_ADMINISTRATIVO;
    } else if (tiene_correo && tiene_reporte && !tiene_articulo) {
        perfil = USUARIO_TECNICO;
    } else if (tiene_correo && tiene_articulo && !tiene_reporte) {
        perfil = USUARIO_PROFESOR;
    } else if (tiene_articulo && tiene_reporte && !tiene_correo) {
        perfil = USUARIO_ESTUDIANTE;
    }

    const char *nombres_usuarios[] = {
        "Personal Administrativo", "Personal Técnico", 
        "Profesor", "Estudiante", "Indeterminado"
    };

    printf("Total Documentos Analizados: %d\n", total_docs);
    printf(" - Correos: %d\n", docs_por_clase[CLASE_CORREO]);
    printf(" - Artículos: %d\n", docs_por_clase[CLASE_ARTICULO]);
    printf(" - Reportes: %d\n", docs_por_clase[CLASE_REPORTE]);
    printf("\n>> CONTEXTO INFERIDO: [%s] <<\n", nombres_usuarios[perfil]);
    printf("════════════════════════════════════════════════════════════\n\n");
}

/* Manejador de cada conexión cliente (Hilo pthread) */
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
        buffer[strcspn(buffer, "\r\n")] = 0; // Quitar salto de línea

        char comando[32];
        sscanf(buffer, "%s", comando);

        if (strcmp(comando, PROTO_TOTAL) == 0) {
            sscanf(buffer, "%*s %d", &total_ventanas_esperadas);
            printf("[IALearner] Total de ventanas esperadas registrado: %d\n", total_ventanas_esperadas);
        } else if (strcmp(comando, PROTO_ID) == 0) {
            sscanf(buffer, "%*s %d", &id_ventana);
            pthread_mutex_lock(&mutex_docs);
            mi_doc = &docs[id_ventana % MAX_VENTANAS];
            mi_doc->id_ventana = id_ventana;
            mi_doc->activa = 1;
            mi_doc->pos_buffer = 0;
            mi_doc->clase_asignada = CLASE_DESCONOCIDA;
            memset(mi_doc->conteo_clases, 0, sizeof(mi_doc->conteo_clases));
            memset(mi_doc->buffer_oracion, 0, sizeof(mi_doc->buffer_oracion));
            pthread_mutex_unlock(&mutex_docs);
            printf("[IALearner] Conectada Ventana ID: %d\n", id_ventana);
        } else if (strcmp(comando, PROTO_CHAR) == 0) {
            char c = buffer[5]; // El carácter tras "CHAR "
            if (mi_doc && mi_doc->pos_buffer < TAM_MAX_ORACION - 1) {
                mi_doc->buffer_oracion[mi_doc->pos_buffer++] = c;
                mi_doc->buffer_oracion[mi_doc->pos_buffer] = '\0';
            }
        } else if (strcmp(comando, PROTO_RET) == 0) {
            if (mi_doc) {
                pthread_mutex_lock(&mutex_docs);
                procesar_oracion(mi_doc);
                pthread_mutex_unlock(&mutex_docs);
            }
        } else if (strcmp(comando, PROTO_FIN) == 0) {
            break;
        }
    }

    // Al desconectarse la ventana
    pthread_mutex_lock(&mutex_docs);
    if (mi_doc) {
        procesar_oracion(mi_doc); // procesar lo que haya quedado pendiente
        clasificar_documento(mi_doc);
        mi_doc->activa = 0;
    }
    ventanas_finalizadas++;
    if (total_ventanas_esperadas > 0 && ventanas_finalizadas >= total_ventanas_esperadas) {
        inferir_tipo_usuario();
    }
    pthread_mutex_unlock(&mutex_docs);

    fclose(socket_stream);
    return NULL;
}

int main(void) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Error creando el socket del servidor");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PUERTO_DEFECTO);

    if (bind(server_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error en bind del servidor");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, MAX_VENTANAS) < 0) {
        perror("Error en listen");
        close(server_sock);
        return 1;
    }

    printf("=====================================================\n");
    printf("      [IALearner Data Center] Servidor Iniciado      \n");
    printf("      Escuchando en el puerto: %d                    \n", PUERTO_DEFECTO);
    printf("=====================================================\n");

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr *)&cli_addr, &cli_len);

        if (*client_sock < 0) {
            free(client_sock);
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, atender_cliente, client_sock) != 0) {
            perror("Error al crear hilo para cliente");
            free(client_sock);
        } else {
            pthread_detach(tid); // Liberación automática de recursos del hilo
        }
    }

    close(server_sock);
    return 0;
}