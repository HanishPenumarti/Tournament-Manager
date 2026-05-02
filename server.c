/*
 * Tournament Manager - Server
 *
 * OS Concepts demonstrated:
 *  4.1 Role-Based Authorization  - admin / player / viewer roles with access control
 *  4.2 File Locking              - F_WRLCK (write) via fcntl when writing match_score.txt
 *  4.3 Concurrency Control       - pthreads (one thread per client) + mutexes + semaphore
 *  4.4 Data Consistency          - all shared state guarded by mutexes; semaphore prevents
 *                                  overload races; pipe score updates are atomic
 *  4.5 Socket Programming        - TCP client-server on SERVER_PORT
 *  4.6 IPC                       - (a) Named FIFOs for player-to-player rally messages
 *                                  (b) Named pipe /tmp/tm_score_pipe for player->server
 *                                      score-event notifications (second IPC mechanism)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_PORT   9001
#define MAX_CLIENTS   64
#define MAX_USERS     256
#define MAX_LINE      256
#define MAX_SESSIONS  128

/*
 * 4.6b - Second IPC mechanism: named pipe for score events.
 * Players write "SCORE_UPDATE\n" here; the score_pipe_reader thread
 * reads it and fans out to all connected viewers.
 * This is completely separate from the rally FIFOs (first IPC mechanism).
 */
#define SCORE_PIPE_PATH "/tmp/tm_score_pipe"

/* 4.3 - Semaphore: limits concurrent active connections */
#define MAX_CONCURRENT_CONNECTIONS 60

#define ADMIN_USERNAME "admin"
#define ADMIN_PASSWORD "penumarti@69"

/* ---------- player credential database ---------- */
static const struct { const char *username; const char *password; } player_db[] = {
    {"praveen", "ppj123"},
    {"arnav",   "ao123"},
    {"karthik", "skc123"},
    {"varun",   "ve123"},
    {"hanish",  "hp123"},
    {"amartya", "av123"},
};

static int player_in_database(const char *username, const char *password) {
    for (size_t i = 0; i < sizeof(player_db)/sizeof(player_db[0]); ++i)
        if (strcmp(player_db[i].username, username) == 0 &&
            strcmp(player_db[i].password, password) == 0)
            return 1;
    return 0;
}

/* ---------- user / session structures ---------- */
typedef struct {
    char role[16];
    char username[32];
    char password[32];
    int  logged_in;
} User;

typedef struct {
    int  fd;
    int  active;
    int  logged_in;
    char role[16];
    char username[32];
} Session;

/* ---------- global state ---------- */
static User     users[MAX_USERS];
static int      user_count = 0;
static pthread_mutex_t users_mutex    = PTHREAD_MUTEX_INITIALIZER;

static Session  sessions[MAX_SESSIONS];
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

static int  admin_logged_in = 0;
static int  admin_fd        = -1;
static pthread_mutex_t admin_mutex    = PTHREAD_MUTEX_INITIALIZER;

static int  registered_player_count = 0;
static int  listen_fd    = -1;
static int  score_pipe_fd = -1;   /* read end of the score named pipe */

/* 4.3 - Semaphore: counts available connection slots */
static sem_t conn_semaphore;

/* ---------- helpers ---------- */
static int is_valid_role(const char *r) {
    return strcmp(r,"admin")==0 || strcmp(r,"player")==0 || strcmp(r,"viewer")==0;
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len && (s[len-1]=='\n' || s[len-1]=='\r')) { s[len-1]='\0'; trim_newline(s); }
}

static void send_response(int fd, const char *msg) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    send(fd, buf, strlen(buf), 0);
}

/* ---------- session helpers ---------- */
static void clear_session_fd(int fd) {
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].fd == fd) {
            sessions[i].active    = 0;
            sessions[i].logged_in = 0;
            sessions[i].role[0]   = '\0';
            sessions[i].username[0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
}

static void upsert_session_login(int fd, const char *role, const char *username, int logged_in) {
    pthread_mutex_lock(&sessions_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].fd == fd) { slot = i; break; }
        if (!sessions[i].active && slot == -1)            slot = i;
    }
    if (slot >= 0) {
        sessions[slot].fd        = fd;
        sessions[slot].active    = 1;
        sessions[slot].logged_in = logged_in;
        strncpy(sessions[slot].role,     role,     sizeof(sessions[slot].role)-1);
        strncpy(sessions[slot].username, username, sizeof(sessions[slot].username)-1);
        sessions[slot].role[sizeof(sessions[slot].role)-1]         = '\0';
        sessions[slot].username[sizeof(sessions[slot].username)-1] = '\0';
    }
    pthread_mutex_unlock(&sessions_mutex);
}

static int get_logged_in_fd(const char *role, const char *username) {
    int fd = -1;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in &&
            strcmp(sessions[i].role,     role)     == 0 &&
            strcmp(sessions[i].username, username) == 0) {
            fd = sessions[i].fd; break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return fd;
}

static int is_logged_in_role_fd(int fd, const char *role) {
    int ok = 0;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in &&
            sessions[i].fd == fd &&
            strcmp(sessions[i].role, role) == 0) { ok = 1; break; }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return ok;
}

static void notify_all_viewers_score_update(void) {
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in &&
            strcmp(sessions[i].role, "viewer") == 0)
            send(sessions[i].fd, "SCORE_UPDATE\n", 13, 0);
    }
    pthread_mutex_unlock(&sessions_mutex);
}

static int find_user(const char *role, const char *username) {
    for (int i = 0; i < user_count; ++i)
        if (strcmp(users[i].role, role) == 0 &&
            strcmp(users[i].username, username) == 0)
            return i;
    return -1;
}

/*
 * 4.6b - Score pipe reader thread (second IPC mechanism)
 *
 * Blocks on read() from the named pipe SCORE_PIPE_PATH.
 * Player processes write "SCORE_UPDATE\n" to this pipe after every
 * score change.  This thread picks it up and fans the notification
 * out to all connected viewers via their TCP sockets.
 *
 * Named FIFOs (rally messages between players) = first IPC mechanism.
 * This named pipe (score events player -> server) = second IPC mechanism.
 */
static void *score_pipe_reader(void *arg) {
    (void)arg;
    char buf[128];
    while (1) {
        ssize_t n = read(score_pipe_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            if (errno == EINTR) continue;
            /* Pipe was closed (e.g. server shutting down) */
            break;
        }
        buf[n] = '\0';
        if (strstr(buf, "SCORE_UPDATE"))
            notify_all_viewers_score_update();
    }
    return NULL;
}

/* ================================================================
 *  Per-client thread
 * ================================================================ */
static void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    send_response(client_fd, "Welcome to Tennis Tournament Server");

    char buffer[MAX_LINE];
    while (1) {
        ssize_t total = 0;
        while (total < (ssize_t)sizeof(buffer) - 1) {
            ssize_t n = recv(client_fd, buffer + total, 1, 0);
            if (n <= 0) goto cleanup;
            if (buffer[total] == '\n') { total += n; break; }
            total += n;
        }
        if (total <= 0) break;
        buffer[total] = '\0';
        trim_newline(buffer);
        if (strlen(buffer) == 0) continue;

        char command[16], role[16], username[32], password[32], ranking_str[16] = "0";
        int fields = sscanf(buffer, "%15s %15s %31s %31s %15s",
                            command, role, username, password, ranking_str);
        if (fields < 1) { send_response(client_fd, "ERROR Invalid command format"); continue; }

        for (size_t i = 0; command[i]; ++i) command[i] = (char)toupper((unsigned char)command[i]);

        if (strcmp(command, "QUIT") == 0) { send_response(client_fd, "Goodbye"); break; }

        if (strcmp(command, "SCORE_UPDATE") == 0) {
            if (!is_logged_in_role_fd(client_fd, "player")) {
                send_response(client_fd, "ERROR Only logged-in players can send score updates");
                continue;
            }
            notify_all_viewers_score_update();
            continue;
        }

        if (strcmp(command, "CHECK") == 0) {
            if (fields != 3) { send_response(client_fd, "ERROR Expected: CHECK <role> <username>"); continue; }
            if (!is_valid_role(role)) { send_response(client_fd, "ERROR Role must be admin, player, or viewer"); continue; }
            if (strcmp(role, "admin") == 0) {
                send_response(client_fd, strcmp(username, ADMIN_USERNAME) == 0 ? "OK EXISTS" : "ERROR wrong username");
                continue;
            }
            pthread_mutex_lock(&users_mutex);
            int idx = find_user(role, username);
            pthread_mutex_unlock(&users_mutex);
            send_response(client_fd, idx >= 0 ? "OK EXISTS" : "OK AVAILABLE");
            continue;
        }

        if (strcmp(command, "REGISTER") == 0) {
            if (!is_valid_role(role)) { send_response(client_fd, "ERROR Role must be admin, player, or viewer"); continue; }
            if (strcmp(role, "admin") == 0) { send_response(client_fd, "ERROR Admin registration not allowed"); continue; }

            if (strcmp(role, "player") == 0) {
                if (fields != 5) { send_response(client_fd, "ERROR Expected: REGISTER player <username> <password> <ranking>"); continue; }
                if (registered_player_count >= 2) { send_response(client_fd, "ERROR Only first two player registrations are allowed"); continue; }
                int ranking = atoi(ranking_str);
                if (ranking <= 0) { send_response(client_fd, "ERROR Ranking must be a positive integer"); continue; }
                int db_ok = player_in_database(username, password);
                if (!db_ok && ranking >= 20) {
                    pthread_mutex_lock(&admin_mutex);
                    if (admin_fd == -1) { pthread_mutex_unlock(&admin_mutex); send_response(client_fd, "ERROR No admin logged in"); continue; }
                    char req[MAX_LINE];
                    snprintf(req, sizeof(req), "APPROVE_REQUEST %s %d\n", username, ranking);
                    send(admin_fd, req, strlen(req), 0);
                    char resp[MAX_LINE]; ssize_t rn = 0;
                    while (rn < (ssize_t)sizeof(resp)-1) {
                        ssize_t m = recv(admin_fd, resp+rn, 1, 0);
                        if (m <= 0) break;
                        if (resp[rn] == '\n') { rn += m; break; }
                        rn += m;
                    }
                    if (rn <= 0) { pthread_mutex_unlock(&admin_mutex); send_response(client_fd, "ERROR Admin connection lost"); continue; }
                    resp[rn] = '\0'; trim_newline(resp);
                    int approve = strcmp(resp, "APPROVE") == 0;
                    pthread_mutex_unlock(&admin_mutex);
                    if (!approve) { send_response(client_fd, "ERROR Registration denied by admin"); continue; }
                }
            } else if (fields != 4) {
                send_response(client_fd, "ERROR Expected: REGISTER <role> <username> <password>"); continue;
            }

            pthread_mutex_lock(&users_mutex);
            if (find_user(role, username) >= 0) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR Username already exists for this role"); continue; }
            if (user_count >= MAX_USERS)         { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR User limit reached"); continue; }
            strncpy(users[user_count].role,     role,     sizeof(users[user_count].role)-1);
            strncpy(users[user_count].username, username, sizeof(users[user_count].username)-1);
            strncpy(users[user_count].password, password, sizeof(users[user_count].password)-1);
            users[user_count].role[sizeof(users[user_count].role)-1]         = '\0';
            users[user_count].username[sizeof(users[user_count].username)-1] = '\0';
            users[user_count].password[sizeof(users[user_count].password)-1] = '\0';
            users[user_count].logged_in = 0;
            user_count++;
            if (strcmp(role, "player") == 0) registered_player_count++;
            pthread_mutex_unlock(&users_mutex);
            send_response(client_fd, "OK Registered successfully");

        } else if (strcmp(command, "LOGIN") == 0) {
            if (!is_valid_role(role)) { send_response(client_fd, "ERROR Role must be admin, player, or viewer"); continue; }
            if (strcmp(role, "admin") == 0) {
                if (strcmp(username, ADMIN_USERNAME) != 0) { send_response(client_fd, "ERROR No account found for this role and username"); continue; }
                if (strcmp(password, ADMIN_PASSWORD) != 0) { send_response(client_fd, "ERROR Incorrect password"); continue; }
                pthread_mutex_lock(&admin_mutex);
                if (admin_logged_in) { pthread_mutex_unlock(&admin_mutex); send_response(client_fd, "ERROR Already logged in"); continue; }
                admin_logged_in = 1; admin_fd = client_fd;
                pthread_mutex_unlock(&admin_mutex);
                upsert_session_login(client_fd, "admin", ADMIN_USERNAME, 1);
                send_response(client_fd, "OK Login successful"); continue;
            }
            pthread_mutex_lock(&users_mutex);
            int idx = find_user(role, username);
            if (idx < 0) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR No account found for this role and username"); continue; }
            if (strcmp(users[idx].password, password) != 0) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR Incorrect password"); continue; }
            if (users[idx].logged_in) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR Already logged in"); continue; }
            users[idx].logged_in = 1;
            pthread_mutex_unlock(&users_mutex);
            upsert_session_login(client_fd, role, username, 1);
            send_response(client_fd, "OK Login successful");

        } else if (strcmp(command, "LOGOUT") == 0) {
            if (fields != 3) { send_response(client_fd, "ERROR Expected: LOGOUT <role> <username>"); continue; }
            if (!is_valid_role(role)) { send_response(client_fd, "ERROR Role must be admin, player, or viewer"); continue; }
            if (strcmp(role, "admin") == 0) {
                if (strcmp(username, ADMIN_USERNAME) != 0) { send_response(client_fd, "ERROR No account found for this role and username"); continue; }
                pthread_mutex_lock(&admin_mutex);
                if (!admin_logged_in) { pthread_mutex_unlock(&admin_mutex); send_response(client_fd, "ERROR Not logged in"); continue; }
                admin_logged_in = 0; admin_fd = -1;
                pthread_mutex_unlock(&admin_mutex);
                upsert_session_login(client_fd, "admin", ADMIN_USERNAME, 0);
                send_response(client_fd, "OK Logout successful"); continue;
            }
            pthread_mutex_lock(&users_mutex);
            int idx = find_user(role, username);
            if (idx < 0) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR No account found for this role and username"); continue; }
            if (!users[idx].logged_in) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR Not logged in"); continue; }
            users[idx].logged_in = 0;
            pthread_mutex_unlock(&users_mutex);
            upsert_session_login(client_fd, role, username, 0);
            send_response(client_fd, "OK Logout successful");

        } else if (strcmp(command, "START_MATCH") == 0) {
            char p1[32], p2[32];
            if (sscanf(buffer, "%*s %31s %31s", p1, p2) != 2) { send_response(client_fd, "ERROR Expected: START_MATCH <player1> <player2>"); continue; }
            pthread_mutex_lock(&admin_mutex);
            int is_admin_sender = admin_logged_in && admin_fd == client_fd;
            pthread_mutex_unlock(&admin_mutex);
            if (!is_admin_sender) { send_response(client_fd, "ERROR Only logged-in admin can start match"); continue; }
            if (strcmp(p1, p2) == 0) { send_response(client_fd, "ERROR Players must be different"); continue; }
            int p1_fd = get_logged_in_fd("player", p1);
            int p2_fd = get_logged_in_fd("player", p2);
            if (p1_fd < 0 || p2_fd < 0) { send_response(client_fd, "ERROR Both players must be logged in"); continue; }
            const char *fifo_a = "/tmp/tm_p1_to_p2_fifo";
            const char *fifo_b = "/tmp/tm_p2_to_p1_fifo";
            char msg[MAX_LINE];
            snprintf(msg, sizeof(msg), "MATCH_START P1 %s %s %s\n", p2, fifo_a, fifo_b);
            send(p1_fd, msg, strlen(msg), 0);
            snprintf(msg, sizeof(msg), "MATCH_START P2 %s %s %s\n", p1, fifo_b, fifo_a);
            send(p2_fd, msg, strlen(msg), 0);
            send_response(client_fd, "OK Match start sent");

        } else {
            send_response(client_fd, "ERROR Unknown command");
        }
    }

cleanup:
    /* Release connection semaphore slot (4.3) */
    sem_post(&conn_semaphore);

    pthread_mutex_lock(&admin_mutex);
    if (admin_fd == client_fd) { admin_fd = -1; admin_logged_in = 0; }
    pthread_mutex_unlock(&admin_mutex);

    pthread_mutex_lock(&users_mutex);
    for (int i = 0; i < user_count; ++i) {
        int sfd = get_logged_in_fd(users[i].role, users[i].username);
        if (sfd == client_fd) users[i].logged_in = 0;
    }
    pthread_mutex_unlock(&users_mutex);
    clear_session_fd(client_fd);
    close(client_fd);
    return NULL;
}

static void shutdown_server(int signum) {
    (void)signum;
    if (listen_fd >= 0) close(listen_fd);
    if (score_pipe_fd >= 0) close(score_pipe_fd);
    unlink(SCORE_PIPE_PATH);
    sem_destroy(&conn_semaphore);
    printf("\nServer shutting down.\n");
    exit(0);
}

int main(void) {
    signal(SIGINT,  shutdown_server);
    signal(SIGTERM, shutdown_server);

    /*
     * 4.3 Semaphore initialisation
     * Counts available connection slots (starts at MAX_CONCURRENT_CONNECTIONS).
     * sem_wait() before spawning each client thread — blocks if limit reached.
     * sem_post() when thread exits — releases the slot.
     * Prevents unbounded thread creation and the data-consistency hazard of
     * too many concurrent writers to the shared users[] array.
     */
    if (sem_init(&conn_semaphore, 0, MAX_CONCURRENT_CONNECTIONS) != 0) {
        perror("sem_init"); return 1;
    }

    /*
     * 4.6b Named pipe: second IPC mechanism (alongside rally FIFOs).
     * Create /tmp/tm_score_pipe; open it O_RDWR so the read end does
     * not block waiting for a writer.  The score_pipe_reader thread
     * blocks on read() until a player writes a score event.
     */
    unlink(SCORE_PIPE_PATH);
    if (mkfifo(SCORE_PIPE_PATH, 0666) != 0) {
        perror("mkfifo score pipe"); return 1;
    }
    score_pipe_fd = open(SCORE_PIPE_PATH, O_RDWR);
    if (score_pipe_fd < 0) {
        perror("open score pipe"); return 1;
    }

    pthread_t pipe_thread;
    pthread_create(&pipe_thread, NULL, score_pipe_reader, NULL);
    pthread_detach(pipe_thread);

    /* TCP socket */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);
    if (bind(listen_fd,   (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind");   close(listen_fd); return 1; }
    if (listen(listen_fd, MAX_CLIENTS) < 0)                            { perror("listen"); close(listen_fd); return 1; }

    printf("Tennis Tournament Server running on port %d\n", SERVER_PORT);
    printf("[IPC]  Score named pipe ready: %s\n", SCORE_PIPE_PATH);
    printf("[IPC]  Rally named FIFOs: /tmp/tm_p1_to_p2_fifo, /tmp/tm_p2_to_p1_fifo\n");
    printf("[SYNC] Connection semaphore initialised (max %d concurrent)\n", MAX_CONCURRENT_CONNECTIONS);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));
        if (!client_fd) { perror("malloc"); continue; }

        *client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (*client_fd < 0) {
            free(client_fd);
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

        /*
         * 4.3 Semaphore: acquire a slot before spawning the client thread.
         * Blocks here if MAX_CONCURRENT_CONNECTIONS threads are already running,
         * preventing resource exhaustion.
         */
        sem_wait(&conn_semaphore);

        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, client_fd) != 0) {
            perror("pthread_create");
            sem_post(&conn_semaphore);
            close(*client_fd);
            free(client_fd);
            continue;
        }
        pthread_detach(thread);
    }

    close(listen_fd);
    close(score_pipe_fd);
    unlink(SCORE_PIPE_PATH);
    sem_destroy(&conn_semaphore);
    return 0;
}
