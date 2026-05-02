#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_PORT 9001
#define SERVER_ADDR "127.0.0.1"
#define MAX_LINE 256
#define SCORE_FILE "match_score.txt"

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

static int match_active = 0;
static int last_set1 = -1;
static int last_set2 = -1;
static char last_game_points[32] = "";
static char last_p1_name[32] = "";
static char last_p2_name[32] = "";
static char last_serving_name[32] = "";
static char last_toss_summary[256] = "";

static int split_game_points(const char *game_points, char *left, char *right, size_t len) {
    const char *sep = strchr(game_points, '-');
    if (!sep) return 0;
    size_t left_len = sep - game_points;
    size_t right_len = strlen(sep + 1);
    if (left_len >= len || right_len >= len) return 0;
    memcpy(left, game_points, left_len);
    left[left_len] = '\0';
    strncpy(right, sep + 1, len - 1);
    right[len - 1] = '\0';
    return 1;
}

static int read_score_snapshot(int *set1, int *set2, char *p1_name, char *p2_name,
                               char *game_points, size_t game_points_len,
                               char *serving_name, size_t serving_len,
                               char *toss_summary, size_t toss_summary_len) {
    int fd = open(SCORE_FILE, O_RDONLY);
    if (fd < 0) return 0;

    /*
     * 4.2 File Locking (read side):
     * Acquire a shared read lock (F_RDLCK) before reading.
     * This prevents a dirty read if the player is simultaneously
     * writing a new snapshot (which holds F_WRLCK).
     * Multiple viewers can hold concurrent read locks safely.
     */
    struct flock rdlock;
    memset(&rdlock, 0, sizeof(rdlock));
    rdlock.l_type   = F_RDLCK;
    rdlock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLKW, &rdlock) == -1) {
        close(fd);
        return 0;
    }

    char data[1024];
    ssize_t n = read(fd, data, sizeof(data) - 1);

    /* Release read lock before closing */
    rdlock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &rdlock);
    close(fd);
    if (n <= 0) return 0;
    data[n] = '\0';

    *set1 = -1;
    *set2 = -1;
    p1_name[0] = '\0';
    p2_name[0] = '\0';
    game_points[0] = '\0';
    serving_name[0] = '\0';
    toss_summary[0] = '\0';

    char *line = strtok(data, "\n");
    while (line) {
        if (strncmp(line, "Set Games:", 10) == 0) {
            sscanf(line, "Set Games: %31s %d - %d %31s", p1_name, set1, set2, p2_name);
        } else if (strncmp(line, "Current Game Points:", 20) == 0) {
            const char *p = line + 20;
            while (*p == ' ') p++;
            strncpy(game_points, p, game_points_len - 1);
            game_points[game_points_len - 1] = '\0';
        } else if (strncmp(line, "Serving:", 8) == 0) {
            const char *p = line + 8;
            while (*p == ' ') p++;
            strncpy(serving_name, p, serving_len - 1);
            serving_name[serving_len - 1] = '\0';
        } else if (strncmp(line, "Toss:", 5) == 0) {
            const char *p = line + 5;
            while (*p == ' ') p++;
            strncpy(toss_summary, p, toss_summary_len - 1);
            toss_summary[toss_summary_len - 1] = '\0';
        }
        line = strtok(NULL, "\n");
    }
    return (*set1 >= 0 && *set2 >= 0 && p1_name[0] && p2_name[0] && game_points[0] != '\0');
}

static void print_match_header(const char *left_name, int left_set, int right_set,
                               const char *right_name, const char *game_points,
                               const char *toss_summary) {
    char left_score[32];
    char right_score[32];
    printf("\n============= LIVE MATCH SCORE =============\n");
    printf("Set Games: %s %d - %d %s\n", left_name, left_set, right_set, right_name);
    if (split_game_points(game_points, left_score, right_score, sizeof(left_score))) {
        printf("Current Game Points: %s %s - %s %s\n", left_name, left_score, right_score, right_name);
    } else {
        printf("Current Game Points: %s\n", game_points);
    }
    if (toss_summary[0]) {
        printf("Toss: %s\n", toss_summary);
    }
}

static void print_game_update(const char *left_name, const char *right_name, const char *game_points) {
    char left_score[32];
    char right_score[32];
    if (split_game_points(game_points, left_score, right_score, sizeof(left_score))) {
        printf("\nCurrent Game Points: %s %s - %s %s\n", left_name, left_score, right_score, right_name);
    } else {
        printf("\nCurrent Game Points: %s\n", game_points);
    }
}

static void print_set_update(const char *left_name, int left_set, int right_set, const char *right_name,
                             const char *game_points) {
    char left_score[32];
    char right_score[32];
    printf("\nSet Games: %s %d - %d %s\n", left_name, left_set, right_set, right_name);
    if (split_game_points(game_points, left_score, right_score, sizeof(left_score))) {
        printf("Current Game Points: %s %s - %s %s\n", left_name, left_score, right_score, right_name);
    } else {
        printf("Current Game Points: %s\n", game_points);
    }
}

static int handle_server_line(const char *line, int *logged_in, char *logged_in_username) {
    if (strcmp(line, "SCORE_UPDATE") == 0) {
        int set1 = -1;
        int set2 = -1;
        char p1_name[32] = "";
        char p2_name[32] = "";
        char game_points[32] = "";
        char serving_name[32] = "";
        char toss_summary[256] = "";
        if (read_score_snapshot(&set1, &set2, p1_name, p2_name, game_points, sizeof(game_points), serving_name, sizeof(serving_name), toss_summary, sizeof(toss_summary))) {
            const char *left_name = serving_name[0] ? serving_name : p1_name;
            const char *right_name = (strcmp(left_name, p1_name) == 0) ? p2_name : p1_name;
            int left_set = (strcmp(left_name, p1_name) == 0) ? set1 : set2;
            int right_set = (strcmp(left_name, p1_name) == 0) ? set2 : set1;
            if (!match_active) {
                match_active = 1;
                last_set1 = set1;
                last_set2 = set2;
                strncpy(last_p1_name, p1_name, sizeof(last_p1_name) - 1);
                last_p1_name[sizeof(last_p1_name) - 1] = '\0';
                strncpy(last_p2_name, p2_name, sizeof(last_p2_name) - 1);
                last_p2_name[sizeof(last_p2_name) - 1] = '\0';
                strncpy(last_serving_name, serving_name, sizeof(last_serving_name) - 1);
                last_serving_name[sizeof(last_serving_name) - 1] = '\0';
                strncpy(last_toss_summary, toss_summary, sizeof(last_toss_summary) - 1);
                last_toss_summary[sizeof(last_toss_summary) - 1] = '\0';
                strncpy(last_game_points, game_points, sizeof(last_game_points) - 1);
                last_game_points[sizeof(last_game_points) - 1] = '\0';
                print_match_header(left_name, left_set, right_set, right_name, game_points, toss_summary);
            } else if (set1 != last_set1 || set2 != last_set2 || strcmp(serving_name, last_serving_name) != 0 || strcmp(toss_summary, last_toss_summary) != 0) {
                print_set_update(left_name, left_set, right_set, right_name, game_points);
                last_set1 = set1;
                last_set2 = set2;
                strncpy(last_serving_name, serving_name, sizeof(last_serving_name) - 1);
                last_serving_name[sizeof(last_serving_name) - 1] = '\0';
                strncpy(last_toss_summary, toss_summary, sizeof(last_toss_summary) - 1);
                last_toss_summary[sizeof(last_toss_summary) - 1] = '\0';
                strncpy(last_game_points, game_points, sizeof(last_game_points) - 1);
                last_game_points[sizeof(last_game_points) - 1] = '\0';
            } else if (strcmp(game_points, last_game_points) != 0) {
                print_game_update(left_name, right_name, game_points);
                strncpy(last_game_points, game_points, sizeof(last_game_points) - 1);
                last_game_points[sizeof(last_game_points) - 1] = '\0';
            }
        } else {
            printf("\n[Viewer] Unable to read score file.\n");
        }
        return 1;
    }
    printf("Server: %s\n", line);
    if (strcmp(line, "OK Login successful") == 0) {
        *logged_in = 1;
    } else if (strcmp(line, "OK Logout successful") == 0) {
        *logged_in = 0;
        logged_in_username[0] = '\0';
    }
    return 1;
}

int main(void) {
    printf("Viewer client connecting to %s:%d...\n", SERVER_ADDR, SERVER_PORT);
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
        if (logged_in && !match_active) {
            printf("\nLogged in as %s\n", logged_in_username);
            printf("1) Logout\n");
            printf("2) Quit\n");
            printf("Choice: ");
            fflush(stdout);

            fd_set wait_set;
            FD_ZERO(&wait_set);
            FD_SET(STDIN_FILENO, &wait_set);
            FD_SET(sock, &wait_set);
            int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
            int ready = select(maxfd + 1, &wait_set, NULL, NULL, NULL);
            if (ready < 0) {
                perror("select");
                break;
            }
            if (FD_ISSET(sock, &wait_set)) {
                n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                handle_server_line(recv_buffer, &logged_in, logged_in_username);
                continue;
            }
        } else if (logged_in && match_active) {
            fd_set wait_set;
            FD_ZERO(&wait_set);
            FD_SET(sock, &wait_set);
            int ready = select(sock + 1, &wait_set, NULL, NULL, NULL);
            if (ready < 0) {
                perror("select");
                break;
            }
            if (FD_ISSET(sock, &wait_set)) {
                n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                handle_server_line(recv_buffer, &logged_in, logged_in_username);
                continue;
            }
        } else {
            printf("\nViewer menu:\n");
            printf("1) Register\n");
            printf("2) Login\n");
            printf("3) Quit\n");
            printf("Choice: ");
            fflush(stdout);
        }

        char choice[8];
        if (!fgets(choice, sizeof(choice), stdin)) break;
        trim_newline(choice);
        if (logged_in) {
            if (strcmp(choice, "1") == 0 || strcasecmp(choice, "logout") == 0) {
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "LOGOUT viewer %s\n", logged_in_username);
                send(sock, send_buffer, strlen(send_buffer), 0);
            } else if (strcmp(choice, "2") == 0 || strcasecmp(choice, "quit") == 0) {
                send(sock, "QUIT\n", 5, 0);
                break;
            } else {
                printf("Invalid choice.\n");
                continue;
            }
        } else {
            if (strcmp(choice, "1") == 0 || strcasecmp(choice, "register") == 0) {
                char username[64];
                char password[64];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "CHECK viewer %s\n", username);
                send(sock, send_buffer, strlen(send_buffer), 0);

                n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                if (strncmp(recv_buffer, "OK AVAILABLE", 12) != 0) {
                    printf("Server: %s\n", recv_buffer);
                    continue;
                }

                printf("Password: ");
                if (!fgets(password, sizeof(password), stdin)) break;
                trim_newline(password);
                snprintf(send_buffer, sizeof(send_buffer), "REGISTER viewer %s %s\n", username, password);
                send(sock, send_buffer, strlen(send_buffer), 0);
            } else if (strcmp(choice, "2") == 0 || strcasecmp(choice, "login") == 0) {
                char username[64];
                char password[64];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "CHECK viewer %s\n", username);
                send(sock, send_buffer, strlen(send_buffer), 0);

                n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                if (strncmp(recv_buffer, "OK EXISTS", 9) != 0) {
                    printf("Server: ERROR No account found for this role and username\n");
                    continue;
                }

                printf("Password: ");
                if (!fgets(password, sizeof(password), stdin)) break;
                trim_newline(password);
                snprintf(send_buffer, sizeof(send_buffer), "LOGIN viewer %s %s\n", username, password);
                send(sock, send_buffer, strlen(send_buffer), 0);
                // Store username for logout
                strcpy(logged_in_username, username);
            } else if (strcmp(choice, "3") == 0 || strcasecmp(choice, "quit") == 0) {
                send(sock, "QUIT\n", 5, 0);
                break;
            } else {
                printf("Invalid choice.\n");
                continue;
            }
        }

        if (!logged_in || strcmp(choice, "1") == 0 || strcasecmp(choice, "logout") == 0) {
            n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
            if (n <= 0) break;
            handle_server_line(recv_buffer, &logged_in, logged_in_username);
        }
    }

    close(sock);
    return 0;
}
