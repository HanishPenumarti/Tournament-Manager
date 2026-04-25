#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_PORT 9000
#define MAX_CLIENTS 64
#define MAX_USERS 256
#define MAX_LINE 256

#define ADMIN_USERNAME "admin"
#define ADMIN_PASSWORD "penumarti@69"

static const struct {
    const char *username;
    const char *password;
} player_db[] = {
    {"praveen", "ppj123"},
    {"arnav", "ao123"},
    {"karthik", "skc123"},
    {"varun", "ve123"},
    {"hanish", "hp123"},
    {"amartya", "av123"},
};

static int player_in_database(const char *username, const char *password) {
    for (size_t i = 0; i < sizeof(player_db) / sizeof(player_db[0]); ++i) {
        if (strcmp(player_db[i].username, username) == 0 && strcmp(player_db[i].password, password) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char role[16];
    char username[32];
    char password[32];
    int logged_in;
} User;

static int is_valid_role(const char *role) {
    return strcmp(role, "admin") == 0 || strcmp(role, "player") == 0 || strcmp(role, "viewer") == 0;
}

static User users[MAX_USERS];
static int user_count = 0;
static pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;
static int listen_fd = -1;
static int admin_logged_in = 0;
static pthread_mutex_t admin_mutex = PTHREAD_MUTEX_INITIALIZER;

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        trim_newline(s);
    }
}

static int find_user(const char *role, const char *username) {
    for (int i = 0; i < user_count; ++i) {
        if (strcmp(users[i].role, role) == 0 && strcmp(users[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

static void send_response(int client_fd, const char *message) {
    char buffer[MAX_LINE];
    snprintf(buffer, sizeof(buffer), "%s\n", message);
    send(client_fd, buffer, strlen(buffer), 0);
}

static void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    send_response(client_fd, "Welcome to Tennis Tournament Server");
    send_response(client_fd, "Commands: CHECK <role> <username>, REGISTER <role> <username> <password> [ranking], LOGIN <role> <username> <password>, LOGOUT <role> <username>, QUIT");

    char buffer[MAX_LINE];
    while (1) {
        ssize_t total = 0;
        while (total < (ssize_t)sizeof(buffer) - 1) {
            ssize_t n = recv(client_fd, buffer + total, 1, 0);
            if (n <= 0) break;
            if (buffer[total] == '\n') {
                total += n;
                break;
            }
            total += n;
        }
        if (total <= 0) break;
        buffer[total] = '\0';
        trim_newline(buffer);
        if (strlen(buffer) == 0) continue;

        char command[16];
        char role[16];
        char username[32];
        char password[32];
        char ranking_str[16] = "0";

        int fields = sscanf(buffer, "%15s %15s %31s %31s %15s", command, role, username, password, ranking_str);
        if (fields < 1) {
            send_response(client_fd, "ERROR Invalid command format");
            continue;
        }

        if (strcasecmp(command, "QUIT") == 0) {
            send_response(client_fd, "Goodbye");
            break;
        }

        if (strcasecmp(command, "CHECK") == 0) {
            if (fields != 3) {
                send_response(client_fd, "ERROR Expected: CHECK <role> <username>");
                continue;
            }
            if (!is_valid_role(role)) {
                send_response(client_fd, "ERROR Role must be admin, player, or viewer");
                continue;
            }
            if (strcmp(role, "admin") == 0) {
                if (strcmp(username, ADMIN_USERNAME) == 0) {
                    send_response(client_fd, "OK EXISTS");
                } else {
                    send_response(client_fd, "ERROR wrong username");
                }
                continue;
            }
            pthread_mutex_lock(&users_mutex);
            int idx = find_user(role, username);
            pthread_mutex_unlock(&users_mutex);
            if (idx >= 0) {
                send_response(client_fd, "OK EXISTS");
            } else {
                send_response(client_fd, "OK AVAILABLE");
            }
            continue;
        }

        if (strcasecmp(command, "REGISTER") == 0) {
            if (!is_valid_role(role)) {
                send_response(client_fd, "ERROR Role must be admin, player, or viewer");
                continue;
            }
            if (strcmp(role, "admin") == 0) {
                send_response(client_fd, "ERROR Admin registration not allowed");
                continue;
            }
            if (strcmp(role, "player") == 0) {
                if (fields != 5) {
                    send_response(client_fd, "ERROR Expected: REGISTER player <username> <password> <ranking>");
                    continue;
                }
                int ranking = atoi(ranking_str);
                if (ranking <= 0) {
                    send_response(client_fd, "ERROR Ranking must be a positive integer");
                    continue;
                }
                int db_ok = player_in_database(username, password);
                if (!db_ok && ranking >= 20) {
                    send_response(client_fd, "ERROR Registration denied: ranking must be below 20 or credentials in database");
                    continue;
                }
            } else if (fields != 4) {
                send_response(client_fd, "ERROR Expected: REGISTER <role> <username> <password>");
                continue;
            }
            pthread_mutex_lock(&users_mutex);
            int existing = find_user(role, username);
            if (existing >= 0) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR Username already exists for this role");
                continue;
            }
            if (user_count >= MAX_USERS) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR User limit reached");
                continue;
            }
            strncpy(users[user_count].role, role, sizeof(users[user_count].role) - 1);
            strncpy(users[user_count].username, username, sizeof(users[user_count].username) - 1);
            strncpy(users[user_count].password, password, sizeof(users[user_count].password) - 1);
            users[user_count].role[sizeof(users[user_count].role) - 1] = '\0';
            users[user_count].username[sizeof(users[user_count].username) - 1] = '\0';
            users[user_count].password[sizeof(users[user_count].password) - 1] = '\0';
            user_count++;
            pthread_mutex_unlock(&users_mutex);
            send_response(client_fd, "OK Registered successfully");

        } else if (strcasecmp(command, "LOGIN") == 0) {
            if (!is_valid_role(role)) {
                send_response(client_fd, "ERROR Role must be admin, player, or viewer");
                continue;
            }
            if (strcmp(role, "admin") == 0) {
                if (strcmp(username, ADMIN_USERNAME) != 0) {
                    send_response(client_fd, "ERROR No account found for this role and username");
                    continue;
                }
                if (strcmp(password, ADMIN_PASSWORD) != 0) {
                    send_response(client_fd, "ERROR Incorrect password");
                    continue;
                }
                pthread_mutex_lock(&admin_mutex);
                if (admin_logged_in) {
                    pthread_mutex_unlock(&admin_mutex);
                    send_response(client_fd, "ERROR Already logged in");
                    continue;
                }
                admin_logged_in = 1;
                pthread_mutex_unlock(&admin_mutex);
                send_response(client_fd, "OK Login successful");
                continue;
            }
            pthread_mutex_lock(&users_mutex);
            int idx = find_user(role, username);
            if (idx < 0) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR No account found for this role and username");
                continue;
            }
            if (strcmp(users[idx].password, password) != 0) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR Incorrect password");
                continue;
            }
            if (users[idx].logged_in) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR Already logged in");
                continue;
            }
            users[idx].logged_in = 1;
            pthread_mutex_unlock(&users_mutex);
            send_response(client_fd, "OK Login successful");

        } else if (strcasecmp(command, "LOGOUT") == 0) {
            if (fields != 3) {
                send_response(client_fd, "ERROR Expected: LOGOUT <role> <username>");
                continue;
            }
            if (!is_valid_role(role)) {
                send_response(client_fd, "ERROR Role must be admin, player, or viewer");
                continue;
            }
            if (strcmp(role, "admin") == 0) {
                if (strcmp(username, ADMIN_USERNAME) != 0) {
                    send_response(client_fd, "ERROR No account found for this role and username");
                    continue;
                }
                pthread_mutex_lock(&admin_mutex);
                if (!admin_logged_in) {
                    pthread_mutex_unlock(&admin_mutex);
                    send_response(client_fd, "ERROR Not logged in");
                    continue;
                }
                admin_logged_in = 0;
                pthread_mutex_unlock(&admin_mutex);
                send_response(client_fd, "OK Logout successful");
                continue;
            }
            pthread_mutex_lock(&users_mutex);
            int idx = find_user(role, username);
            if (idx < 0) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR No account found for this role and username");
                continue;
            }
            if (!users[idx].logged_in) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR Not logged in");
                continue;
            }
            users[idx].logged_in = 0;
            pthread_mutex_unlock(&users_mutex);
            send_response(client_fd, "OK Logout successful");

        } else {
            send_response(client_fd, "ERROR Unknown command");
        }
    }

    close(client_fd);
    return NULL;
}

static void shutdown_server(int signum) {
    (void)signum;
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    printf("\nServer shutting down.\n");
    exit(0);
}

int main(void) {
    signal(SIGINT, shutdown_server);
    signal(SIGTERM, shutdown_server);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("Tennis Tournament Server is running on port %d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));
        if (!client_fd) {
            perror("malloc");
            continue;
        }
        *client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (*client_fd < 0) {
            free(client_fd);
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, client_fd);
        pthread_detach(thread);
    }

    close(listen_fd);
    return 0;
}
