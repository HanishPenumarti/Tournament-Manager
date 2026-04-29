#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_PORT 9001
#define MAX_CLIENTS 64
#define MAX_USERS 256
#define MAX_LINE 512
#define MAX_SESSIONS 128
#define MAX_PLAYERS 8
#define MAX_MATCHES 64

#define ADMIN_USERNAME "admin"
#define ADMIN_PASSWORD "penumarti@69"

#define POINTS_FILE "points_table.txt"

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
    {"ravi", "rv123"},
    {"sita", "st123"},
};

static int player_in_database(const char *username, const char *password) {
    for (size_t i = 0; i < sizeof(player_db) / sizeof(player_db[0]); ++i) {
        if (strcmp(player_db[i].username, username) == 0 &&
            strcmp(player_db[i].password, password) == 0) {
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
static int admin_fd = -1;
static pthread_mutex_t admin_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int fd;
    int active;
    int logged_in;
    char role[16];
    char username[32];
} Session;

static Session sessions[MAX_SESSIONS];
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;
static int registered_player_count = 0;

/* ===== Tournament state ===== */
typedef enum {
    MATCH_PENDING = 0,    /* not yet scheduled / started */
    MATCH_SCHEDULED,      /* admin has triggered start, waiting for both players */
    MATCH_IN_PROGRESS,
    MATCH_DONE,
    MATCH_BYE             /* one player absent → opponent wins automatically */
} MatchStatus;

typedef enum {
    STAGE_GROUP = 0,
    STAGE_SEMI,
    STAGE_FINAL
} MatchStage;

typedef struct {
    int match_id;
    MatchStage stage;
    char p1[32];
    char p2[32];
    char winner[32];  /* empty until done */
    MatchStatus status;
    int group;        /* 0=A, 1=B, -1=knockout */
} Match;

/* Tournament data – written/read under tournament_mutex */
static Match matches[MAX_MATCHES];
static int match_count = 0;
static int next_match_id = 1;
static char registered_players[MAX_PLAYERS][32];
static int tournament_started = 0;   /* 1 once admin begins group stage */
static int group_stage_done = 0;
static pthread_mutex_t tournament_mutex = PTHREAD_MUTEX_INITIALIZER;

/* busy[i] == 1 if player i is currently in a match */
static int player_busy[MAX_PLAYERS];

/* ===== Helper: fifo names per match id ===== */
static void fifo_names(int match_id, char *f1, char *f2, size_t sz) {
    snprintf(f1, sz, "/tmp/tm_p1_to_p2_%d_fifo", match_id);
    snprintf(f2, sz, "/tmp/tm_p2_to_p1_%d_fifo", match_id);
}

/* ===== Session helpers ===== */
static void clear_session_fd(int fd) {
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].fd == fd) {
            sessions[i].active = 0;
            sessions[i].logged_in = 0;
            sessions[i].role[0] = '\0';
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
        if (!sessions[i].active && slot == -1) slot = i;
    }
    if (slot >= 0) {
        sessions[slot].fd = fd;
        sessions[slot].active = 1;
        sessions[slot].logged_in = logged_in;
        strncpy(sessions[slot].role, role, sizeof(sessions[slot].role) - 1);
        sessions[slot].role[sizeof(sessions[slot].role)-1] = '\0';
        strncpy(sessions[slot].username, username, sizeof(sessions[slot].username) - 1);
        sessions[slot].username[sizeof(sessions[slot].username)-1] = '\0';
    }
    pthread_mutex_unlock(&sessions_mutex);
}

static int get_logged_in_fd(const char *role, const char *username) {
    int fd = -1;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in &&
            strcmp(sessions[i].role, role) == 0 &&
            strcmp(sessions[i].username, username) == 0) {
            fd = sessions[i].fd;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return fd;
}

static int is_logged_in_role_fd(int fd, const char *role) {
    int ok = 0;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in && sessions[i].fd == fd &&
            strcmp(sessions[i].role, role) == 0) { ok = 1; break; }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return ok;
}

static void notify_all_viewers_score_update(void) {
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in &&
            strcmp(sessions[i].role, "viewer") == 0) {
            send(sessions[i].fd, "SCORE_UPDATE\n", 13, 0);
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
}

/* notify a specific player by username */
static void notify_player(const char *username, const char *msg) {
    int fd = get_logged_in_fd("player", username);
    if (fd >= 0) {
        char buf[MAX_LINE];
        snprintf(buf, sizeof(buf), "%s\n", msg);
        send(fd, buf, strlen(buf), 0);
    }
}

/* notify all logged-in viewers and players about match list update */
static void notify_match_list_update(void) {
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in) {
            send(sessions[i].fd, "MATCH_LIST_UPDATE\n", 18, 0);
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
}

/* notify all logged-in viewers and players about points table update */
static void notify_points_update(void) {
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].logged_in) {
            send(sessions[i].fd, "POINTS_UPDATE\n", 14, 0);
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len && (s[len-1] == '\n' || s[len-1] == '\r')) {
        s[len-1] = '\0';
        trim_newline(s);
    }
}

static int find_user(const char *role, const char *username) {
    for (int i = 0; i < user_count; ++i)
        if (strcmp(users[i].role, role) == 0 && strcmp(users[i].username, username) == 0)
            return i;
    return -1;
}

static void send_response(int client_fd, const char *message) {
    char buffer[MAX_LINE];
    snprintf(buffer, sizeof(buffer), "%s\n", message);
    send(client_fd, buffer, strlen(buffer), 0);
}

/* ===== player_index: index in registered_players array ===== */
static int player_index(const char *name) {
    for (int i = 0; i < registered_player_count; i++)
        if (strcmp(registered_players[i], name) == 0) return i;
    return -1;
}

/* ===== Send match list to a single fd ===== */
/* Format sent to client (one line per match, then "END_MATCH_LIST"):
   MATCH_INFO <id> <stage> <group> <p1> <p2> <status> <winner>
*/
static void send_match_list_to_fd(int fd) {
    pthread_mutex_lock(&tournament_mutex);
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "MATCH_LIST_BEGIN %d\n", match_count);
    send(fd, buf, strlen(buf), 0);
    for (int i = 0; i < match_count; i++) {
        const char *stage_str = (matches[i].stage == STAGE_GROUP) ? "GROUP" :
                                (matches[i].stage == STAGE_SEMI) ? "SEMI" : "FINAL";
        const char *status_str = (matches[i].status == MATCH_PENDING)    ? "PENDING" :
                                 (matches[i].status == MATCH_SCHEDULED)  ? "SCHEDULED" :
                                 (matches[i].status == MATCH_IN_PROGRESS)? "IN_PROGRESS" :
                                 (matches[i].status == MATCH_DONE)       ? "DONE" : "BYE";
        const char *winner = matches[i].winner[0] ? matches[i].winner : "NONE";
        snprintf(buf, sizeof(buf), "MATCH_INFO %d %s %d %s %s %s %s\n",
                 matches[i].match_id, stage_str, matches[i].group,
                 matches[i].p1, matches[i].p2, status_str, winner);
        send(fd, buf, strlen(buf), 0);
    }
    send(fd, "END_MATCH_LIST\n", 15, 0);
    pthread_mutex_unlock(&tournament_mutex);
}

/* ===== Generate group-stage schedule ===== */
/* Must be called with tournament_mutex held */
static void generate_group_schedule(void) {
    int n = registered_player_count;
    int half = n / 2;
    /* Group A: first half, Group B: second half */
    /* Round-robin within each group */
    for (int g = 0; g < 2; g++) {
        int base = g * half;
        for (int i = 0; i < half; i++) {
            for (int j = i + 1; j < half; j++) {
                Match *m = &matches[match_count];
                m->match_id = next_match_id++;
                m->stage = STAGE_GROUP;
                m->group = g;
                strncpy(m->p1, registered_players[base + i], 31);
                strncpy(m->p2, registered_players[base + j], 31);
                m->winner[0] = '\0';
                m->status = MATCH_PENDING;
                match_count++;
            }
        }
    }
}

/* Find index of match in matches[] (hold mutex) */
static int find_match_by_id(int id) {
    for (int i = 0; i < match_count; i++)
        if (matches[i].match_id == id) return i;
    return -1;
}

/* ===== Try to auto-schedule any pending group match whose players are both free ===== */
/* Must be called with tournament_mutex held. Returns 1 if scheduled anything. */
static int try_schedule_pending(void) {
    int did_something = 0;
    for (int i = 0; i < match_count; i++) {
        if (matches[i].status != MATCH_PENDING) continue;
        int pi1 = player_index(matches[i].p1);
        int pi2 = player_index(matches[i].p2);
        if (pi1 < 0 || pi2 < 0) continue;
        if (!player_busy[pi1] && !player_busy[pi2]) {
            /* Check if both are logged in */
            int fd1 = get_logged_in_fd("player", matches[i].p1);
            int fd2 = get_logged_in_fd("player", matches[i].p2);
            if (fd1 < 0 || fd2 < 0) {
                /* One or both absent – grant bye immediately */
                if (fd1 < 0 && fd2 < 0) {
                    /* Both absent: p1 wins by default (arbitrary) */
                    strncpy(matches[i].winner, matches[i].p1, 31);
                } else if (fd1 < 0) {
                    strncpy(matches[i].winner, matches[i].p2, 31);
                } else {
                    strncpy(matches[i].winner, matches[i].p1, 31);
                }
                matches[i].status = MATCH_BYE;
                did_something = 1;
                continue;
            }
            /* Both logged in and free → schedule */
            matches[i].status = MATCH_SCHEDULED;
            player_busy[pi1] = 1;
            player_busy[pi2] = 1;

            char f1[128], f2[128];
            fifo_names(matches[i].match_id, f1, f2, sizeof(f1));
            char msg[MAX_LINE];
            snprintf(msg, sizeof(msg), "MATCH_START P1 %s %s %s\n", matches[i].p2, f1, f2);
            send(fd1, msg, strlen(msg), 0);
            snprintf(msg, sizeof(msg), "MATCH_START P2 %s %s %s\n", matches[i].p1, f2, f1);
            send(fd2, msg, strlen(msg), 0);
            did_something = 1;
        }
    }
    return did_something;
}

/* ===== Check if group stage is complete ===== */
/* Must hold tournament_mutex */
static int check_group_stage_done(void) {
    for (int i = 0; i < match_count; i++) {
        if (matches[i].stage == STAGE_GROUP &&
            matches[i].status != MATCH_DONE &&
            matches[i].status != MATCH_BYE) return 0;
    }
    return 1;
}

/* Compute group standings (wins, games_won) */
typedef struct { char name[32]; int wins; int games_won; } Standing;

static void compute_group_standings(int group, Standing *out, int *out_count) {
    int n = registered_player_count / 2;
    int base = group * n;
    *out_count = n;
    for (int i = 0; i < n; i++) {
        strncpy(out[i].name, registered_players[base + i], 31);
        out[i].name[31] = '\0';
        out[i].wins = 0;
        out[i].games_won = 0;
    }
    /* Count wins from DONE/BYE matches in this group */
    /* games_won: not tracked server-side (only admin writes points table).
       Server only needs rank for semifinal seeding.
       We approximate games_won = 0 (admin manages it). Server uses wins only. */
    for (int i = 0; i < match_count; i++) {
        if (matches[i].stage != STAGE_GROUP || matches[i].group != group) continue;
        if (matches[i].status != MATCH_DONE && matches[i].status != MATCH_BYE) continue;
        if (!matches[i].winner[0]) continue;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].name, matches[i].winner) == 0) {
                out[j].wins++;
            }
        }
    }
    /* Sort by wins descending (simple bubble) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (out[j].wins > out[i].wins) {
                Standing tmp = out[i]; out[i] = out[j]; out[j] = tmp;
            }
        }
    }
}

/* ===== Generate semifinal/final matches ===== */
/* Must hold tournament_mutex */
static void generate_knockout(void) {
    Standing sa[MAX_PLAYERS/2], sb[MAX_PLAYERS/2];
    int ca, cb;
    compute_group_standings(0, sa, &ca);
    compute_group_standings(1, sb, &cb);

    /* Semi 1: A1 vs B2 */
    Match *s1 = &matches[match_count++];
    s1->match_id = next_match_id++;
    s1->stage = STAGE_SEMI;
    s1->group = -1;
    strncpy(s1->p1, sa[0].name, 31);
    strncpy(s1->p2, sb[1 < cb ? 1 : 0].name, 31);
    s1->winner[0] = '\0';
    s1->status = MATCH_PENDING;

    /* Semi 2: B1 vs A2 */
    Match *s2 = &matches[match_count++];
    s2->match_id = next_match_id++;
    s2->stage = STAGE_SEMI;
    s2->group = -1;
    strncpy(s2->p1, sb[0].name, 31);
    strncpy(s2->p2, sa[1 < ca ? 1 : 0].name, 31);
    s2->winner[0] = '\0';
    s2->status = MATCH_PENDING;
}

/* Must hold tournament_mutex */
static void generate_final(void) {
    /* Find semi winners */
    char semi_winners[2][32] = {"",""};
    int sw = 0;
    for (int i = 0; i < match_count && sw < 2; i++) {
        if (matches[i].stage == STAGE_SEMI &&
            (matches[i].status == MATCH_DONE || matches[i].status == MATCH_BYE) &&
            matches[i].winner[0]) {
            strncpy(semi_winners[sw++], matches[i].winner, 31);
        }
    }
    if (sw < 2) return;
    Match *f = &matches[match_count];
    f->match_id = next_match_id++;
    f->stage = STAGE_FINAL;
    f->group = -1;
    strncpy(f->p1, semi_winners[0], 31);
    strncpy(f->p2, semi_winners[1], 31);
    f->winner[0] = '\0';
    f->status = MATCH_PENDING;
    match_count++;
}

/* ===== Called when a match is completed (MATCH_DONE/BYE) ===== */
/* Must be called WITHOUT tournament_mutex (acquires it internally) */
static void on_match_complete(int match_id, const char *winner) {
    pthread_mutex_lock(&tournament_mutex);

    int idx = find_match_by_id(match_id);
    if (idx < 0) { pthread_mutex_unlock(&tournament_mutex); return; }

    strncpy(matches[idx].winner, winner, 31);
    matches[idx].winner[31] = '\0';
    matches[idx].status = MATCH_DONE;

    /* Free players */
    int pi1 = player_index(matches[idx].p1);
    int pi2 = player_index(matches[idx].p2);
    if (pi1 >= 0) player_busy[pi1] = 0;
    if (pi2 >= 0) player_busy[pi2] = 0;

    /* Determine what stage this was */
    MatchStage stage = matches[idx].stage;

    if (stage == STAGE_GROUP) {
        /* Try to schedule more group matches */
        try_schedule_pending();

        /* Check if all group matches done */
        if (check_group_stage_done() && !group_stage_done) {
            group_stage_done = 1;
            generate_knockout();
            /* Try scheduling semis immediately */
            try_schedule_pending();
        }
    } else if (stage == STAGE_SEMI) {
        /* Check if both semis done */
        int sdone = 0;
        for (int i = 0; i < match_count; i++) {
            if (matches[i].stage == STAGE_SEMI &&
                (matches[i].status == MATCH_DONE || matches[i].status == MATCH_BYE))
                sdone++;
        }
        int total_semis = 0;
        for (int i = 0; i < match_count; i++)
            if (matches[i].stage == STAGE_SEMI) total_semis++;
        if (sdone == total_semis) {
            generate_final();
            try_schedule_pending();
        }
    }
    /* STAGE_FINAL: tournament over */

    pthread_mutex_unlock(&tournament_mutex);

    /* Notify admin to update points table */
    pthread_mutex_lock(&admin_mutex);
    if (admin_fd >= 0) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "MATCH_COMPLETE %d %s\n", match_id, winner);
        send(admin_fd, msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&admin_mutex);

    notify_match_list_update();
}

/* ===== MATCH_RESULT command handler (called by a player after match ends) ===== */
/* Protocol: MATCH_RESULT <match_id> <winner_username> */
static void handle_match_result(int client_fd, int match_id, const char *winner) {
    on_match_complete(match_id, winner);

    /* Notify both players to show points table */
    pthread_mutex_lock(&tournament_mutex);
    int idx = find_match_by_id(match_id);
    if (idx >= 0) {
        notify_player(matches[idx].p1, "POINTS_UPDATE");
        notify_player(matches[idx].p2, "POINTS_UPDATE");
    }
    pthread_mutex_unlock(&tournament_mutex);

    notify_points_update();
    send_response(client_fd, "OK Match result recorded");
}

/* ===== GET_MATCH_LIST command ===== */
static void handle_get_match_list(int client_fd) {
    send_match_list_to_fd(client_fd);
}

/* ===== WATCH_MATCH command ===== */
/* Protocol: WATCH_MATCH <match_id> */
/* Server tells viewer which score file to watch */
static void handle_watch_match(int client_fd, int match_id) {
    pthread_mutex_lock(&tournament_mutex);
    int idx = find_match_by_id(match_id);
    if (idx < 0) {
        pthread_mutex_unlock(&tournament_mutex);
        send_response(client_fd, "ERROR Match not found");
        return;
    }
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "WATCH_INFO %d %s %s %s",
             match_id, matches[idx].p1, matches[idx].p2,
             (matches[idx].status == MATCH_IN_PROGRESS || matches[idx].status == MATCH_SCHEDULED)
               ? "LIVE" : "OTHER");
    pthread_mutex_unlock(&tournament_mutex);
    send_response(client_fd, buf);
}

/* ===== Main client handler ===== */
static void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    send_response(client_fd, "Welcome to Tennis Tournament Server");

    char buffer[MAX_LINE];
    while (1) {
        ssize_t total = 0;
        while (total < (ssize_t)sizeof(buffer) - 1) {
            ssize_t n = recv(client_fd, buffer + total, 1, 0);
            if (n <= 0) goto done;
            if (buffer[total] == '\n') { total += n; break; }
            total += n;
        }
        buffer[total] = '\0';
        trim_newline(buffer);
        if (strlen(buffer) == 0) continue;

        char command[32];
        char role[16];
        char username[32];
        char password[32];
        char ranking_str[16] = "0";

        int fields = sscanf(buffer, "%31s %15s %31s %31s %15s",
                            command, role, username, password, ranking_str);
        if (fields < 1) { send_response(client_fd, "ERROR Invalid command format"); continue; }

        for (size_t i = 0; command[i]; ++i)
            command[i] = (char)toupper((unsigned char)command[i]);

        if (strcmp(command, "QUIT") == 0) {
            send_response(client_fd, "Goodbye");
            break;
        }

        if (strcmp(command, "SCORE_UPDATE") == 0) {
            if (!is_logged_in_role_fd(client_fd, "player")) {
                send_response(client_fd, "ERROR Only logged-in players can send score updates");
                continue;
            }
            notify_all_viewers_score_update();
            continue;
        }

        /* Player reports match result */
        if (strcmp(command, "MATCH_RESULT") == 0) {
            if (!is_logged_in_role_fd(client_fd, "player")) {
                send_response(client_fd, "ERROR Not authorized");
                continue;
            }
            int mid = -1;
            char win[32] = "";
            if (sscanf(buffer, "%*s %d %31s", &mid, win) != 2) {
                send_response(client_fd, "ERROR Expected: MATCH_RESULT <id> <winner>");
                continue;
            }
            handle_match_result(client_fd, mid, win);
            continue;
        }

        /* Viewer / player requests match list */
        if (strcmp(command, "GET_MATCH_LIST") == 0) {
            handle_get_match_list(client_fd);
            continue;
        }

        /* Viewer requests to watch a match */
        if (strcmp(command, "WATCH_MATCH") == 0) {
            int mid = -1;
            sscanf(buffer, "%*s %d", &mid);
            handle_watch_match(client_fd, mid);
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

                pthread_mutex_lock(&users_mutex);
                if (registered_player_count >= MAX_PLAYERS) {
                    pthread_mutex_unlock(&users_mutex);
                    send_response(client_fd, "ERROR Maximum 8 players allowed");
                    continue;
                }
                pthread_mutex_unlock(&users_mutex);

                int ranking = atoi(ranking_str);
                if (ranking <= 0) { send_response(client_fd, "ERROR Ranking must be a positive integer"); continue; }

                int db_ok = player_in_database(username, password);
                if (!db_ok && ranking >= 20) {
                    /* Ask admin */
                    pthread_mutex_lock(&admin_mutex);
                    if (admin_fd == -1) {
                        pthread_mutex_unlock(&admin_mutex);
                        send_response(client_fd, "ERROR No admin logged in");
                        continue;
                    }
                    char request[MAX_LINE];
                    snprintf(request, sizeof(request), "APPROVE_REQUEST %s %d\n", username, ranking);
                    send(admin_fd, request, strlen(request), 0);
                    char response[MAX_LINE];
                    ssize_t n = 0;
                    while (n < (ssize_t)sizeof(response) - 1) {
                        ssize_t m = recv(admin_fd, response + n, 1, 0);
                        if (m <= 0) break;
                        if (response[n] == '\n') { n += m; break; }
                        n += m;
                    }
                    if (n <= 0) {
                        pthread_mutex_unlock(&admin_mutex);
                        send_response(client_fd, "ERROR Admin connection lost");
                        continue;
                    }
                    response[n] = '\0';
                    trim_newline(response);
                    int approve = strcmp(response, "APPROVE") == 0;
                    pthread_mutex_unlock(&admin_mutex);
                    if (!approve) { send_response(client_fd, "ERROR Registration denied by admin"); continue; }
                }
            } else if (fields != 4) {
                send_response(client_fd, "ERROR Expected: REGISTER <role> <username> <password>");
                continue;
            }

            pthread_mutex_lock(&users_mutex);
            if (find_user(role, username) >= 0) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR Username already exists for this role");
                continue;
            }
            if (user_count >= MAX_USERS) {
                pthread_mutex_unlock(&users_mutex);
                send_response(client_fd, "ERROR User limit reached");
                continue;
            }
            strncpy(users[user_count].role, role, 15);
            strncpy(users[user_count].username, username, 31);
            strncpy(users[user_count].password, password, 31);
            users[user_count].role[15] = '\0';
            users[user_count].username[31] = '\0';
            users[user_count].password[31] = '\0';
            user_count++;
            if (strcmp(role, "player") == 0) {
                strncpy(registered_players[registered_player_count], username, 31);
                registered_players[registered_player_count][31] = '\0';
                registered_player_count++;
            }
            pthread_mutex_unlock(&users_mutex);
            send_response(client_fd, "OK Registered successfully");

        } else if (strcmp(command, "LOGIN") == 0) {
            if (!is_valid_role(role)) { send_response(client_fd, "ERROR Role must be admin, player, or viewer"); continue; }
            if (strcmp(role, "admin") == 0) {
                if (strcmp(username, ADMIN_USERNAME) != 0) { send_response(client_fd, "ERROR No account found for this role and username"); continue; }
                if (strcmp(password, ADMIN_PASSWORD) != 0) { send_response(client_fd, "ERROR Incorrect password"); continue; }
                pthread_mutex_lock(&admin_mutex);
                if (admin_logged_in) { pthread_mutex_unlock(&admin_mutex); send_response(client_fd, "ERROR Already logged in"); continue; }
                admin_logged_in = 1;
                admin_fd = client_fd;
                pthread_mutex_unlock(&admin_mutex);
                upsert_session_login(client_fd, "admin", ADMIN_USERNAME, 1);
                send_response(client_fd, "OK Login successful");
                continue;
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

            /* If tournament already started and player just logged in, try scheduling */
            if (strcmp(role, "player") == 0 && tournament_started) {
                pthread_mutex_lock(&tournament_mutex);
                try_schedule_pending();
                pthread_mutex_unlock(&tournament_mutex);
                notify_match_list_update();
            }

        } else if (strcmp(command, "LOGOUT") == 0) {
            if (fields != 3) { send_response(client_fd, "ERROR Expected: LOGOUT <role> <username>"); continue; }
            if (!is_valid_role(role)) { send_response(client_fd, "ERROR Role must be admin, player, or viewer"); continue; }
            if (strcmp(role, "admin") == 0) {
                if (strcmp(username, ADMIN_USERNAME) != 0) { send_response(client_fd, "ERROR No account found"); continue; }
                pthread_mutex_lock(&admin_mutex);
                if (!admin_logged_in) { pthread_mutex_unlock(&admin_mutex); send_response(client_fd, "ERROR Not logged in"); continue; }
                admin_logged_in = 0;
                admin_fd = -1;
                pthread_mutex_unlock(&admin_mutex);
                upsert_session_login(client_fd, "admin", ADMIN_USERNAME, 0);
                send_response(client_fd, "OK Logout successful");
                continue;
            }
            pthread_mutex_lock(&users_mutex);
            int idx = find_user(role, username);
            if (idx < 0) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR No account found"); continue; }
            if (!users[idx].logged_in) { pthread_mutex_unlock(&users_mutex); send_response(client_fd, "ERROR Not logged in"); continue; }
            users[idx].logged_in = 0;
            pthread_mutex_unlock(&users_mutex);
            upsert_session_login(client_fd, role, username, 0);
            send_response(client_fd, "OK Logout successful");

        } else if (strcmp(command, "START_TOURNAMENT") == 0) {
            /* Admin starts the tournament – generate group schedule */
            pthread_mutex_lock(&admin_mutex);
            int is_admin = admin_logged_in && admin_fd == client_fd;
            pthread_mutex_unlock(&admin_mutex);
            if (!is_admin) { send_response(client_fd, "ERROR Only admin can start tournament"); continue; }

            pthread_mutex_lock(&users_mutex);
            int np = registered_player_count;
            pthread_mutex_unlock(&users_mutex);

            if (np < 2 || np % 2 != 0) {
                send_response(client_fd, "ERROR Need 2-8 even number of players");
                continue;
            }
            pthread_mutex_lock(&tournament_mutex);
            if (tournament_started) {
                pthread_mutex_unlock(&tournament_mutex);
                send_response(client_fd, "ERROR Tournament already started");
                continue;
            }
            tournament_started = 1;
            memset(player_busy, 0, sizeof(player_busy));
            generate_group_schedule();
            /* Auto-schedule any pending matches now */
            try_schedule_pending();
            pthread_mutex_unlock(&tournament_mutex);

            send_response(client_fd, "OK Tournament started");
            notify_match_list_update();

        } else if (strcmp(command, "START_MATCH") == 0) {
            /* Legacy / manual start – admin manually triggers a specific pending match */
            char p1[32], p2[32];
            if (sscanf(buffer, "%*s %31s %31s", p1, p2) != 2) {
                send_response(client_fd, "ERROR Expected: START_MATCH <player1> <player2>");
                continue;
            }
            pthread_mutex_lock(&admin_mutex);
            int is_admin = admin_logged_in && admin_fd == client_fd;
            pthread_mutex_unlock(&admin_mutex);
            if (!is_admin) { send_response(client_fd, "ERROR Only logged-in admin can start match"); continue; }

            /* Find a pending match with these players */
            pthread_mutex_lock(&tournament_mutex);
            int found = -1;
            for (int i = 0; i < match_count; i++) {
                if (matches[i].status == MATCH_PENDING &&
                    ((strcmp(matches[i].p1, p1) == 0 && strcmp(matches[i].p2, p2) == 0) ||
                     (strcmp(matches[i].p1, p2) == 0 && strcmp(matches[i].p2, p1) == 0))) {
                    found = i; break;
                }
            }
            if (found < 0) {
                pthread_mutex_unlock(&tournament_mutex);
                send_response(client_fd, "ERROR No pending match found for these players");
                continue;
            }

            int fd1 = get_logged_in_fd("player", matches[found].p1);
            int fd2 = get_logged_in_fd("player", matches[found].p2);

            if (fd1 < 0 || fd2 < 0) {
                /* Grant bye */
                if (fd1 < 0 && fd2 < 0) strncpy(matches[found].winner, matches[found].p1, 31);
                else if (fd1 < 0)        strncpy(matches[found].winner, matches[found].p2, 31);
                else                      strncpy(matches[found].winner, matches[found].p1, 31);
                matches[found].status = MATCH_BYE;
                int mid = matches[found].match_id;
                char wname[32];
                strncpy(wname, matches[found].winner, 31);
                pthread_mutex_unlock(&tournament_mutex);

                pthread_mutex_lock(&admin_mutex);
                if (admin_fd >= 0) {
                    char msg[MAX_LINE];
                    snprintf(msg, sizeof(msg), "MATCH_COMPLETE %d %s\n", mid, wname);
                    send(admin_fd, msg, strlen(msg), 0);
                }
                pthread_mutex_unlock(&admin_mutex);

                notify_match_list_update();
                send_response(client_fd, "OK Bye granted (player not logged in)");
                continue;
            }

            matches[found].status = MATCH_SCHEDULED;
            int pi1 = player_index(matches[found].p1);
            int pi2 = player_index(matches[found].p2);
            if (pi1 >= 0) player_busy[pi1] = 1;
            if (pi2 >= 0) player_busy[pi2] = 1;

            char f1[128], f2[128];
            fifo_names(matches[found].match_id, f1, f2, sizeof(f1));
            char msg[MAX_LINE];
            int mid = matches[found].match_id;
            snprintf(msg, sizeof(msg), "MATCH_START P1 %s %s %s\n", matches[found].p2, f1, f2);
            send(fd1, msg, strlen(msg), 0);
            snprintf(msg, sizeof(msg), "MATCH_START P2 %s %s %s\n", matches[found].p1, f2, f1);
            send(fd2, msg, strlen(msg), 0);
            (void)mid;
            pthread_mutex_unlock(&tournament_mutex);

            send_response(client_fd, "OK Match start sent");

        } else if (strcmp(command, "POINTS_UPDATED") == 0) {
            /* Admin signals that points table file has been updated */
            pthread_mutex_lock(&admin_mutex);
            int is_admin = admin_logged_in && admin_fd == client_fd;
            pthread_mutex_unlock(&admin_mutex);
            if (!is_admin) { send_response(client_fd, "ERROR Not authorized"); continue; }
            notify_points_update();
            send_response(client_fd, "OK Notified");

        } else {
            send_response(client_fd, "ERROR Unknown command");
        }
    }

done:
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
    printf("\nServer shutting down.\n");
    exit(0);
}

int main(void) {
    signal(SIGINT, shutdown_server);
    signal(SIGTERM, shutdown_server);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, MAX_CLIENTS) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }

    printf("Tennis Tournament Server is running on port %d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));
        if (!client_fd) { perror("malloc"); continue; }
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
