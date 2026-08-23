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

int conectar_ialearner(int id_ventana, const char *host, int puerto) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error al crear el socket");
        return -1;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(puerto);
    
    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        perror("Dirección IP inválida");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Conexión fallida con el IALearner");
        close(sock);
        return -1;
    }

    // Enviar el ID real de la ventana al servidor
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s %d\n", PROTO_ID, id_ventana);
    write(sock, buffer, strlen(buffer));

    return sock;
}

int main(int argc, char *argv[]) {
    // El launcher pasa: ./ventana_x11 <id> <host> <puerto>
    int id_ventana = (argc > 1) ? atoi(argv[1]) : 1;
    const char *host = (argc > 2) ? argv[2] : HOST_DEFECTO;
    int puerto = (argc > 3) ? atoi(argv[3]) : PUERTO_DEFECTO;

    int sock = conectar_ialearner(id_ventana, host, puerto);
    if (sock < 0) {
        fprintf(stderr, "Aviso: Ventana %d sin conexión al IALearner.\n", id_ventana);
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