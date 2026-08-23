#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/protocolo.h"

int conectar_ialearner(int id_ventana) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error al crear el socket");
        return -1;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PUERTO_DEFECTO);
    
    if (inet_pton(AF_INET, HOST_DEFECTO, &serv_addr.sin_addr) <= 0) {
        perror("Dirección IP inválida");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Conexión fallida con el IALearner");
        close(sock);
        return -1;
    }

    // Enviar el ID de la ventana al conectarnos según el protocolo
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s %d\n", PROTO_ID, id_ventana);
    write(sock, buffer, strlen(buffer));

    return sock;
}

int main(int argc, char *argv[]) {
    // Tomar el ID de la ventana desde los argumentos (pasado por el launcher) o por defecto 1
    int id_ventana = (argc > 1) ? atoi(argv[1]) : 1;

    int sock = conectar_ialearner(id_ventana);
    if (sock < 0) {
        fprintf(stderr, "Aviso: Ejecutando ventana sin conexión al IALearner.\n");
    }

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display\n");
        if (sock >= 0) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%s\n", PROTO_FIN);
            write(sock, buffer, strlen(buffer));
            close(sock);
        }
        return 1;
    }

    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        10, 10, 400, 200,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);

    XEvent event;
    while (1) {
        XNextEvent(display, &event);
        if (event.type == KeyPress) {
            KeySym keysym = XLookupKeysym(&event.xkey, 0);
            
            if (keysym == XK_Escape) {
                break;
            }

            // Si presiona Enter / Return -> Enviar protocolo RET
            if (keysym == XK_Return || keysym == XK_KP_Enter) {
                if (sock >= 0) {
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "%s\n", PROTO_RET);
                    write(sock, buffer, strlen(buffer));
                }
                printf("\n[Ventana %d] Oración enviada (Enter).\n", id_ventana);
            } else {
                char *name = XKeysymToString(keysym);
                if (name) {
                    char c = '\0';
                    if (strlen(name) == 1) {
                        c = name[0];
                    } else if (keysym == XK_space) {
                        c = ' ';
                    }

                    if (c != '\0') {
                        printf("%c", c);
                        fflush(stdout);
                        if (sock >= 0) {
                            char buffer[64];
                            snprintf(buffer, sizeof(buffer), "%s %c\n", PROTO_CHAR, c);
                            write(sock, buffer, strlen(buffer));
                        }
                    }
                }
            }
        }
    }

    // Al cerrar la ventana, avisar al IALearner con FIN
    if (sock >= 0) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%s\n", PROTO_FIN);
        write(sock, buffer, strlen(buffer));
        close(sock);
    }

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}