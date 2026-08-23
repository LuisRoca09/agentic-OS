#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include "../include/protocolo.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: ./launcher <n_ventanas> [host_ialearner] [puerto]\n");
        printf("  Ejemplo: ./launcher 3 127.0.0.1 9500\n");
        return 1;
    }

    int n_ventanas = atoi(argv[1]);
    const char *host = (argc > 2) ? argv[2] : HOST_DEFECTO;
    int puerto = (argc > 3) ? atoi(argv[3]) : PUERTO_DEFECTO;

    if (n_ventanas <= 0 || n_ventanas > MAX_VENTANAS) {
        printf("Número de ventanas inválido (Máximo %d).\n", MAX_VENTANAS);
        return 1;
    }

    printf("[Launcher] Agentic-OS - %d ventana(s) | IALearner en %s:%d\n", n_ventanas, host, puerto);

    // Notificar al IALearner cuántas ventanas se esperan
    int sock_notif = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_notif >= 0) {
        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(puerto);
        inet_pton(AF_INET, host, &serv_addr.sin_addr);
        
        if (connect(sock_notif, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%s %d\n", PROTO_TOTAL, n_ventanas);
            write(sock_notif, buffer, strlen(buffer));
            close(sock_notif);
            printf("[Launcher] IALearner notificado: esperar %d ventana(s)\n", n_ventanas);
        }
    }

    ProcesoHijo procesos[MAX_VENTANAS];

    // Lanzar N procesos gráficos
    for (int i = 0; i < n_ventanas; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("Error al crear proceso con fork");
            exit(1);
        } else if (pid == 0) {
            // Proceso Hijo: Convertir ID, host y puerto a strings para el exec
            char id_str[16], puerto_str[16];
            snprintf(id_str, sizeof(id_str), "%d", i + 1);
            snprintf(puerto_str, sizeof(puerto_str), "%d", puerto);
            
            execl("./ventana_x11", "ventana_x11", id_str, host, puerto_str, NULL);
            perror("Error al ejecutar execl de ventana_x11");
            exit(1);
        } else {
            procesos[i].pid = pid;
            procesos[i].id_ventana = i + 1;
            procesos[i].estado = PROC_ACTIVO;
            printf("[Launcher] Ventana %d lanzada (PID %d)\n", i + 1, pid);
        }
    }

    // Menú interactivo del launcher
    int opcion = 0;
    while (1) {
        printf("\n==================================\n");
        printf("       AGENTIC-OS LAUNCHER        \n");
        printf("==================================\n");
        printf(" 1. Ver estado de procesos\n");
        printf(" 2. Cerrar todas las ventanas\n");
        printf(" 3. Lanzar N ventanas nuevas\n");
        printf(" 4. Salir\n");
        printf("==================================\n");
        printf("Opcion: ");
        
        if (scanf("%d", &opcion) != 1) {
            while(getchar() != '\n'); // Limpiar buffer
            continue;
        }

        if (opcion == 1) {
            printf("\n--------------------------------------------------\n");
            printf(" ID      PID      Estado       Cod.Sal.  \n");
            printf("--------------------------------------------------\n");
            int activos = 0, terminados = 0;
            for (int i = 0; i < n_ventanas; i++) {
                int status;
                pid_t res = waitpid(procesos[i].pid, &status, WNOHANG);
                if (res > 0) {
                    procesos[i].estado = PROC_TERMINADO;
                    procesos[i].codigo_salida = WEXITSTATUS(status);
                }

                if (procesos[i].estado == PROC_ACTIVO) {
                    printf(" %-7d %-8d %-12s -\n", procesos[i].id_ventana, procesos[i].pid, "ACTIVO");
                    activos++;
                } else {
                    printf(" %-7d %-8d %-12s %-9d\n", procesos[i].id_ventana, procesos[i].pid, "TERMINADO", procesos[i].codigo_salida);
                    terminados++;
                }
            }
            printf("--------------------------------------------------\n");
            printf(" Activos: %d | Terminados: %d | Total: %d\n", activos, terminados, n_ventanas);

        } else if (opcion == 2 || opcion == 4) {
            printf("[Launcher] Cerrando todas las ventanas activas...\n");
            for (int i = 0; i < n_ventanas; i++) {
                if (procesos[i].estado == PROC_ACTIVO) {
                    kill(procesos[i].pid, SIGTERM);
                }
            }
            // Esperar a que todos los hijos terminen
            for (int i = 0; i < n_ventanas; i++) {
                waitpid(procesos[i].pid, NULL, 0);
            }
            printf("[Launcher] Ventanas cerradas. Fin.\n");
            break;
        } else if (opcion == 3) {
            printf("Funcionalidad de expansión rápida no requerida para la prueba básica.\n");
        } else {
            printf("Opcion invalida.\n");
        }
    }

    return 0;
}