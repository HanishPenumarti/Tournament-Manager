#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#define SERVER_PORT 9001
#define SERVER_ADDR "127.0.0.1"
#define MAX_LINE 512
#define MAX_MATCHES 64
#define POINTS_FILE "points_table.txt"

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len && (s[len-1] == '\n' || s[len-1] == '\r')) {
        s[len-1] = '\0';
        trim_newline(s);
    }
}

static int connect_to_server(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_ADDR, &server_addr.sin_addr);
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect"); close(sock); return -1;
    }
    return sock;
}

/* Line-buffered recv that does NOT use static state (thread-safe via sock-specific buf) */
typedef struct { char buf[MAX_LINE]; size_t len; } RecvBuf;

static int recv_line_buf(int sock, RecvBuf *rb, char *out, size_t outlen) {
    while (1) {
        for (size_t i = 0; i < rb->len; i++) {
            if (rb->buf[i] == '\n') {
                size_t copy = i < outlen - 1 ? i : outlen - 1;
                memcpy(out, rb->buf, copy);
                out[copy] = '\0';
                size_t remain = rb->len - (i + 1);
                memmove(rb->buf, rb->buf + i + 1, remain);
                rb->len = remain;
                return (int)copy;
            }
        }
        ssize_t n = recv(sock, rb->buf + rb->len, sizeof(rb->buf) - rb->len - 1, 0);
        if (n <= 0) return (int)n;
        rb->len += (size_t)n;
    }
}

/* ===== Match record (mirrored from server) ===== */
typedef struct {
    int  match_id;
    char stage[16];
    int  group;
    char p1[32];
    char p2[32];
    char status[16];
    char winner[32];
} MatchInfo;

static MatchInfo match_list[MAX_MATCHES];
static int match_list_count = 0;

/* ===== Points table entry ===== */
typedef struct {
    char name[32];
    int  matches_played;
    int  matches_won;
    int  games_won;
} PointsEntry;

static PointsEntry points[16];
static int points_count = 0;

/* ===== Read the full match list from server ===== */
static int fetch_match_list(int sock, RecvBuf *rb) {
    send(sock, "GET_MATCH_LIST\n", 15, 0);
    char line[MAX_LINE];
    int n = recv_line_buf(sock, rb, line, sizeof(line));
    if (n <= 0) return 0;
    int total = 0;
    sscanf(line, "MATCH_LIST_BEGIN %d", &total);
    match_list_count = 0;
    for (int i = 0; i < total && i < MAX_MATCHES; i++) {
        n = recv_line_buf(sock, rb, line, sizeof(line));
        if (n <= 0) break;
        MatchInfo *m = &match_list[match_list_count];
        if (sscanf(line, "MATCH_INFO %d %15s %d %31s %31s %15s %31s",
                   &m->match_id, m->stage, &m->group,
                   m->p1, m->p2, m->status, m->winner) == 7) {
            match_list_count++;
        }
    }
    /* consume END_MATCH_LIST */
    recv_line_buf(sock, rb, line, sizeof(line));
    return 1;
}

/* ===== Display match list ===== */
static void display_matches(void) {
    printf("\n=== Match Schedule ===\n");
    printf("%-4s %-6s %-5s %-12s %-12s %-12s %-12s\n",
           "ID","Stage","Grp","Player1","Player2","Status","Winner");
    printf("--------------------------------------------------------------------\n");
    for (int i = 0; i < match_list_count; i++) {
        MatchInfo *m = &match_list[i];
        const char *grp = (m->group == 0) ? "A" : (m->group == 1) ? "B" : "-";
        printf("%-4d %-6s %-5s %-12s %-12s %-12s %-12s\n",
               m->match_id, m->stage, grp,
               m->p1, m->p2, m->status,
               strcmp(m->winner,"NONE")==0 ? "-" : m->winner);
    }
    printf("\n");
}

/* ===== Load points table from file ===== */
static void load_points_table(void) {
    FILE *f = fopen(POINTS_FILE, "r");
    if (!f) { points_count = 0; return; }
    points_count = 0;
    char line[256];
    /* skip header line */
    fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f) && points_count < 16) {
        PointsEntry *e = &points[points_count];
        if (sscanf(line, "%31s %d %d %d", e->name, &e->matches_played, &e->matches_won, &e->games_won) == 4) {
            points_count++;
        }
    }
    fclose(f);
}

/* ===== Save points table to file (admin is the ONLY writer) ===== */
static void save_points_table(void) {
    FILE *f = fopen(POINTS_FILE, "w");
    if (!f) { perror("fopen points_table.txt"); return; }
    fprintf(f, "Player           Played  Won  GamesWon\n");
    fprintf(f, "----------------------------------------\n");
    for (int i = 0; i < points_count; i++) {
        fprintf(f, "%-16s %-7d %-4d %d\n",
                points[i].name,
                points[i].matches_played,
                points[i].matches_won,
                points[i].games_won);
    }
    fclose(f);
}

/* ===== Display points table ===== */
static void display_points_table(void) {
    load_points_table();
    if (points_count == 0) {
        printf("\n[Points table is empty or not yet created.]\n");
        return;
    }
    printf("\n=== Points Table ===\n");
    printf("%-16s %-7s %-4s %s\n", "Player", "Played", "Won", "GamesWon");
    printf("----------------------------------------\n");
    for (int i = 0; i < points_count; i++) {
        printf("%-16s %-7d %-4d %d\n",
               points[i].name,
               points[i].matches_played,
               points[i].matches_won,
               points[i].games_won);
    }
    printf("\n");
}

/* ===== Init points table with all registered players from match list ===== */
static void init_points_table_if_needed(void) {
    /* Collect unique player names from match_list */
    char seen[16][32];
    int ns = 0;
    for (int i = 0; i < match_list_count; i++) {
        int found1 = 0, found2 = 0;
        for (int j = 0; j < ns; j++) {
            if (strcmp(seen[j], match_list[i].p1) == 0) found1 = 1;
            if (strcmp(seen[j], match_list[i].p2) == 0) found2 = 1;
        }
        if (!found1 && ns < 16) strncpy(seen[ns++], match_list[i].p1, 31);
        if (!found2 && ns < 16) strncpy(seen[ns++], match_list[i].p2, 31);
    }
    /* Only init if file doesn't exist or is empty */
    FILE *f = fopen(POINTS_FILE, "r");
    if (f) { fclose(f); return; }  /* already exists, leave it */
    points_count = ns;
    for (int i = 0; i < ns; i++) {
        strncpy(points[i].name, seen[i], 31);
        points[i].matches_played = 0;
        points[i].matches_won = 0;
        points[i].games_won = 0;
    }
    save_points_table();
    printf("[Points table initialised with %d players.]\n", ns);
}

/* ===== Update points table after a match result ===== */
static void update_points_for_match(const char *p1, const char *p2, const char *winner, int is_bye) {
    load_points_table();
    /* Find or create entries */
    int ip1 = -1, ip2 = -1;
    for (int i = 0; i < points_count; i++) {
        if (strcmp(points[i].name, p1) == 0) ip1 = i;
        if (strcmp(points[i].name, p2) == 0) ip2 = i;
    }
    if (ip1 < 0 && points_count < 16) {
        ip1 = points_count;
        strncpy(points[ip1].name, p1, 31);
        points[ip1].matches_played = 0;
        points[ip1].matches_won = 0;
        points[ip1].games_won = 0;
        points_count++;
    }
    if (ip2 < 0 && points_count < 16) {
        ip2 = points_count;
        strncpy(points[ip2].name, p2, 31);
        points[ip2].matches_played = 0;
        points[ip2].matches_won = 0;
        points[ip2].games_won = 0;
        points_count++;
    }
    if (!is_bye) {
        if (ip1 >= 0) points[ip1].matches_played++;
        if (ip2 >= 0) points[ip2].matches_played++;
    }
    /* Increment winner's wins */
    for (int i = 0; i < points_count; i++) {
        if (strcmp(points[i].name, winner) == 0) {
            points[i].matches_won++;
            break;
        }
    }
    /* Prompt admin for games_won update */
    printf("\nMatch result: %s vs %s  → Winner: %s%s\n",
           p1, p2, winner, is_bye ? " (bye/walkover)" : "");
    if (!is_bye) {
        printf("Enter games won by %s in this match (from score file): ", winner);
        fflush(stdout);
        char inp[16];
        if (fgets(inp, sizeof(inp), stdin)) {
            trim_newline(inp);
            int gw = atoi(inp);
            for (int i = 0; i < points_count; i++) {
                if (strcmp(points[i].name, winner) == 0) {
                    points[i].games_won += gw;
                    break;
                }
            }
        }
    }
    save_points_table();
    printf("[Points table updated and saved.]\n");
}

/* ===== Handle MATCH_COMPLETE from server ===== */
static void handle_match_complete(int sock, RecvBuf *rb, const char *msg) {
    int mid = -1;
    char winner[32] = "";
    sscanf(msg, "MATCH_COMPLETE %d %31s", &mid, winner);
    printf("\n*** Match %d completed. Winner: %s ***\n", mid, winner);

    /* Find match in our list */
    fetch_match_list(sock, rb);
    for (int i = 0; i < match_list_count; i++) {
        if (match_list[i].match_id == mid) {
            int is_bye = strcmp(match_list[i].status, "BYE") == 0;
            update_points_for_match(match_list[i].p1, match_list[i].p2, winner, is_bye);
            break;
        }
    }

    /* Notify server that points table has been updated */
    send(sock, "POINTS_UPDATED\n", 15, 0);
    char resp[MAX_LINE];
    recv_line_buf(sock, rb, resp, sizeof(resp));

    display_points_table();
    display_matches();
}

int main(void) {
    printf("Admin client connecting to %s:%d...\n", SERVER_ADDR, SERVER_PORT);
    int sock = connect_to_server();
    if (sock < 0) return 1;

    int logged_in = 0;
    char logged_in_username[64] = "";
    RecvBuf rb;
    memset(&rb, 0, sizeof(rb));
    char recv_buffer[MAX_LINE];

    if (recv_line_buf(sock, &rb, recv_buffer, sizeof(recv_buffer)) > 0)
        printf("%s\n", recv_buffer);

    while (1) {
        if (logged_in) {
            printf("\nLogged in as %s\n", logged_in_username);
            printf("1) Start Tournament\n");
            printf("2) View Match Schedule\n");
            printf("3) Start Specific Match\n");
            printf("4) View Points Table\n");
            printf("5) Update Points Table (manual)\n");
            printf("6) Logout\n");
            printf("7) Quit\n");
            printf("Choice: ");
            fflush(stdout);

            fd_set set;
            FD_ZERO(&set);
            FD_SET(sock, &set);
            FD_SET(STDIN_FILENO, &set);
            int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
            int ready = select(maxfd + 1, &set, NULL, NULL, NULL);
            if (ready < 0) { perror("select"); break; }

            if (FD_ISSET(sock, &set)) {
                int n = recv_line_buf(sock, &rb, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;

                if (strncmp(recv_buffer, "APPROVE_REQUEST ", 16) == 0) {
                    char uname[32]; int ranking = 0;
                    if (sscanf(recv_buffer, "APPROVE_REQUEST %31s %d", uname, &ranking) == 2) {
                        printf("\nApproval request for %s (ranking %d). Approve? (y/n): ", uname, ranking);
                        fflush(stdout);
                        char ans[8];
                        if (fgets(ans, sizeof(ans), stdin)) {
                            trim_newline(ans);
                            if (ans[0] == 'y' || ans[0] == 'Y')
                                send(sock, "APPROVE\n", 8, 0);
                            else
                                send(sock, "DENY\n", 5, 0);
                        } else {
                            send(sock, "DENY\n", 5, 0);
                        }
                    }
                    continue;
                }

                if (strncmp(recv_buffer, "MATCH_COMPLETE ", 15) == 0) {
                    handle_match_complete(sock, &rb, recv_buffer);
                    continue;
                }

                if (strcmp(recv_buffer, "MATCH_LIST_UPDATE") == 0) {
                    /* silently refresh internal list */
                    fetch_match_list(sock, &rb);
                    continue;
                }

                if (strcmp(recv_buffer, "POINTS_UPDATE") == 0) {
                    /* Points table already updated by us; ignore */
                    continue;
                }

                printf("Server: %s\n", recv_buffer);
                if (strcmp(recv_buffer, "OK Logout successful") == 0) {
                    logged_in = 0;
                    logged_in_username[0] = '\0';
                }
                continue;
            }

            /* stdin ready */
            char choice[8];
            if (!fgets(choice, sizeof(choice), stdin)) break;
            trim_newline(choice);

            if (strcmp(choice, "1") == 0) {
                /* Start tournament */
                send(sock, "START_TOURNAMENT\n", 17, 0);
                int n = recv_line_buf(sock, &rb, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                printf("Server: %s\n", recv_buffer);
                if (strncmp(recv_buffer, "OK", 2) == 0) {
                    fetch_match_list(sock, &rb);
                    init_points_table_if_needed();
                    display_matches();
                }

            } else if (strcmp(choice, "2") == 0) {
                fetch_match_list(sock, &rb);
                display_matches();

            } else if (strcmp(choice, "3") == 0) {
                /* Manually start a specific pending match */
                fetch_match_list(sock, &rb);
                display_matches();
                printf("Enter Player1 username: ");
                char p1[32], p2[32];
                if (!fgets(p1, sizeof(p1), stdin)) break;
                trim_newline(p1);
                printf("Enter Player2 username: ");
                if (!fgets(p2, sizeof(p2), stdin)) break;
                trim_newline(p2);
                char cmd[MAX_LINE];
                snprintf(cmd, sizeof(cmd), "START_MATCH %s %s\n", p1, p2);
                send(sock, cmd, strlen(cmd), 0);
                int n = recv_line_buf(sock, &rb, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                printf("Server: %s\n", recv_buffer);

            } else if (strcmp(choice, "4") == 0) {
                display_points_table();

            } else if (strcmp(choice, "5") == 0) {
                /* Manual edit of points table */
                load_points_table();
                if (points_count == 0) {
                    printf("No entries yet.\n");
                    continue;
                }
                printf("\nCurrent entries:\n");
                for (int i = 0; i < points_count; i++)
                    printf("%d) %-16s Played=%d Won=%d GamesWon=%d\n",
                           i+1, points[i].name, points[i].matches_played,
                           points[i].matches_won, points[i].games_won);
                printf("Entry number to edit (0 to cancel): ");
                char idx_s[8];
                if (!fgets(idx_s, sizeof(idx_s), stdin)) continue;
                trim_newline(idx_s);
                int idx = atoi(idx_s) - 1;
                if (idx < 0 || idx >= points_count) { printf("Cancelled.\n"); continue; }
                printf("New Played: "); fflush(stdout);
                char tmp[16];
                if (!fgets(tmp, sizeof(tmp), stdin)) continue;
                trim_newline(tmp); if (tmp[0]) points[idx].matches_played = atoi(tmp);
                printf("New Won: "); fflush(stdout);
                if (!fgets(tmp, sizeof(tmp), stdin)) continue;
                trim_newline(tmp); if (tmp[0]) points[idx].matches_won = atoi(tmp);
                printf("New GamesWon: "); fflush(stdout);
                if (!fgets(tmp, sizeof(tmp), stdin)) continue;
                trim_newline(tmp); if (tmp[0]) points[idx].games_won = atoi(tmp);
                save_points_table();
                /* Notify everyone */
                send(sock, "POINTS_UPDATED\n", 15, 0);
                recv_line_buf(sock, &rb, recv_buffer, sizeof(recv_buffer));
                printf("[Points table saved and clients notified.]\n");
                display_points_table();

            } else if (strcmp(choice, "6") == 0) {
                char buf[MAX_LINE];
                snprintf(buf, sizeof(buf), "LOGOUT admin %s\n", logged_in_username);
                send(sock, buf, strlen(buf), 0);

            } else if (strcmp(choice, "7") == 0) {
                send(sock, "QUIT\n", 5, 0);
                break;
            } else {
                printf("Invalid choice.\n");
            }

        } else {
            /* Not logged in */
            printf("\nAdmin menu:\n");
            printf("1) Login\n");
            printf("2) Quit\n");
            printf("Choice: ");
            fflush(stdout);

            char choice[8];
            if (!fgets(choice, sizeof(choice), stdin)) break;
            trim_newline(choice);

            if (strcmp(choice, "1") == 0) {
                char username[64], password[64];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                if (strcmp(username, "admin") != 0) { printf("ERROR wrong username\n"); continue; }
                printf("Password: ");
                if (!fgets(password, sizeof(password), stdin)) break;
                trim_newline(password);
                char buf[MAX_LINE];
                snprintf(buf, sizeof(buf), "LOGIN admin %s %s\n", username, password);
                send(sock, buf, strlen(buf), 0);

                int n = recv_line_buf(sock, &rb, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                printf("Server: %s\n", recv_buffer);
                if (strcmp(recv_buffer, "OK Login successful") == 0) {
                    logged_in = 1;
                    strcpy(logged_in_username, "admin");
                }
            } else if (strcmp(choice, "2") == 0) {
                send(sock, "QUIT\n", 5, 0);
                break;
            } else {
                printf("Invalid choice.\n");
            }
        }
    }

    close(sock);
    return 0;
}
