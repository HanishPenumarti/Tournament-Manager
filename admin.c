#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>

#define SERVER_PORT 9001
#define SERVER_ADDR "127.0.0.1"
#define MAX_LINE 256

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        trim_newline(s);
    }
}

static int connect_to_server(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_ADDR, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    return sock;
}

static int recv_line(int sock, char *buf, size_t buflen) {
    static char pending[MAX_LINE];
    static size_t pending_len = 0;

    while (1) {
        for (size_t i = 0; i < pending_len; ++i) {
            if (pending[i] == '\n') {
                size_t copy_len = i < buflen - 1 ? i : buflen - 1;
                memcpy(buf, pending, copy_len);
                buf[copy_len] = '\0';
                size_t remain = pending_len - (i + 1);
                memmove(pending, pending + i + 1, remain);
                pending_len = remain;
                return (int)copy_len;
            }
        }

        ssize_t n = recv(sock, pending + pending_len, sizeof(pending) - pending_len - 1, 0);
        if (n <= 0) {
            return (int)n;
        }
        pending_len += (size_t)n;
    }
}

static int handle_server_message(int sock, int *logged_in, char *logged_in_username, const char *msg) {
    if (strncmp(msg, "APPROVE_REQUEST ", 16) == 0) {
        char username[32];
        int ranking = 0;
        if (sscanf(msg, "APPROVE_REQUEST %31s %d", username, &ranking) == 2) {
            if (ranking < 20) {
                send(sock, "APPROVE\n", 8, 0);
                printf("\nAuto-approved registration for %s (ranking %d).\n", username, ranking);
            } else {
                send(sock, "DENY\n", 5, 0);
                printf("\nAuto-denied registration for %s (ranking %d).\n", username, ranking);
            }
        }
        return 1;
    }

    printf("Server: %s\n", msg);
    if (strcmp(msg, "OK Login successful") == 0) {
        *logged_in = 1;
        strcpy(logged_in_username, "admin");
    } else if (strcmp(msg, "OK Logout successful") == 0) {
        *logged_in = 0;
        logged_in_username[0] = '\0';
    }
    return 1;
}

int main(void) {
    printf("Admin client connecting to %s:%d...\n", SERVER_ADDR, SERVER_PORT);
    int sock = connect_to_server();
    if (sock < 0) return 1;

    int logged_in = 0;
    char logged_in_username[64] = "";

    ssize_t n;
    char recv_buffer[MAX_LINE];
    if (recv_line(sock, recv_buffer, sizeof(recv_buffer)) > 0) {
        printf("%s\n", recv_buffer);
    }

    while (1) {
        if (logged_in) {
            printf("\nLogged in as %s\n", logged_in_username);
            printf("1) Start Match\n");
            printf("2) Logout\n");
            printf("3) Quit\n");
            printf("Choice: ");
            fflush(stdout);

            fd_set set;
            FD_ZERO(&set);
            FD_SET(sock, &set);
            FD_SET(STDIN_FILENO, &set);
            int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
            int ready = select(maxfd + 1, &set, NULL, NULL, NULL);
            if (ready < 0) {
                perror("select");
                break;
            }

            if (FD_ISSET(sock, &set)) {
                n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                if (!handle_server_message(sock, &logged_in, logged_in_username, recv_buffer)) break;
                continue;
            }
        } else {
            printf("\nAdmin menu:\n");
            printf("1) Login\n");
            printf("2) Quit\n");
            printf("Choice: ");
            fflush(stdout);
        }

        char choice[8];
        if (!fgets(choice, sizeof(choice), stdin)) break;
        trim_newline(choice);
        if (logged_in) {
            if (strcmp(choice, "1") == 0 || strcasecmp(choice, "start") == 0) {
                char p1[64], p2[64];
                char send_buffer[MAX_LINE];
                printf("Player 1 username: ");
                if (!fgets(p1, sizeof(p1), stdin)) break;
                trim_newline(p1);
                printf("Player 2 username: ");
                if (!fgets(p2, sizeof(p2), stdin)) break;
                trim_newline(p2);
                snprintf(send_buffer, sizeof(send_buffer), "START_MATCH %s %s\n", p1, p2);
                send(sock, send_buffer, strlen(send_buffer), 0);
            } else if (strcmp(choice, "2") == 0 || strcasecmp(choice, "logout") == 0) {
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "LOGOUT admin %s\n", logged_in_username);
                send(sock, send_buffer, strlen(send_buffer), 0);
            } else if (strcmp(choice, "3") == 0 || strcasecmp(choice, "quit") == 0) {
                send(sock, "QUIT\n", 5, 0);
                break;
            } else {
                printf("Invalid choice.\n");
                continue;
            }
        } else {
            if (strcmp(choice, "1") == 0 || strcasecmp(choice, "login") == 0) {
                char username[64];
                char password[64];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                if (strcmp(username, "admin") != 0) {
                    printf("ERROR wrong username\n");
                    continue;
                }
                printf("Password: ");
                if (!fgets(password, sizeof(password), stdin)) break;
                trim_newline(password);
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "LOGIN admin %s %s\n", username, password);
                send(sock, send_buffer, strlen(send_buffer), 0);
            } else if (strcmp(choice, "2") == 0 || strcasecmp(choice, "quit") == 0) {
                send(sock, "QUIT\n", 5, 0);
                break;
            } else {
                printf("Invalid choice.\n");
                continue;
            }
        }

        n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
        if (n <= 0) break;
        if (!handle_server_message(sock, &logged_in, logged_in_username, recv_buffer)) break;
    }

    close(sock);
    return 0;
}
