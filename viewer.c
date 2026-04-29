#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_PORT 9001
#define SERVER_ADDR "127.0.0.1"
#define MAX_LINE 512
#define MAX_MATCHES 64
#define SCORE_FILE_TEMPLATE "/tmp/match_score_%d.txt"
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

/* ===== Per-connection recv buffer ===== */
typedef struct { char buf[MAX_LINE]; size_t len; } RecvBuf;

static void show_points_table(void);
static void handle_score_update(void);
static int recv_line_from_buffer(RecvBuf *rb, char *out, size_t outlen);
static int buffer_has_line(RecvBuf *rb);
static int recv_line_skip_notifications(int sock, RecvBuf *rb, char *out, size_t outlen, int expect_match_list);
static int recv_line_until_match_list(int sock, RecvBuf *rb, char *out, size_t outlen);
static int watching_match_id = -1;

static int recv_line_rb(int sock, RecvBuf *rb, char *out, size_t outlen) {
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

static int recv_line_from_buffer(RecvBuf *rb, char *out, size_t outlen) {
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
    return 0;
}

static int buffer_has_line(RecvBuf *rb) {
    return memchr(rb->buf, '\n', rb->len) != NULL;
}

static int recv_line_skip_notifications(int sock, RecvBuf *rb, char *out, size_t outlen, int expect_match_list) {
    while (1) {
        int n = recv_line_rb(sock, rb, out, outlen);
        if (n <= 0) return n;
        if (strcmp(out, "SCORE_UPDATE") == 0) {
            if (watching_match_id >= 0) handle_score_update();
            continue;
        }
        if (strcmp(out, "MATCH_LIST_UPDATE") == 0) {
            printf("\n[Tournament update: new matches available.]\n");
            continue;
        }
        if (strcmp(out, "POINTS_UPDATE") == 0) {
            printf("\n[Points table has been updated.]\n");
            show_points_table();
            continue;
        }
        if (!expect_match_list &&
            (strncmp(out, "MATCH_LIST_BEGIN", 16) == 0 ||
             strncmp(out, "MATCH_INFO", 10) == 0 ||
             strcmp(out, "END_MATCH_LIST") == 0)) {
            continue;
        }
        return n;
    }
}

static int recv_line_until_match_list(int sock, RecvBuf *rb, char *out, size_t outlen) {
    while (1) {
        int n = recv_line_rb(sock, rb, out, outlen);
        if (n <= 0) return n;
        if (strcmp(out, "SCORE_UPDATE") == 0) {
            if (watching_match_id >= 0) handle_score_update();
            continue;
        }
        if (strcmp(out, "MATCH_LIST_UPDATE") == 0) {
            printf("\n[Tournament update: new matches available.]\n");
            continue;
        }
        if (strcmp(out, "POINTS_UPDATE") == 0) {
            printf("\n[Points table has been updated.]\n");
            show_points_table();
            continue;
        }
        if (strncmp(out, "MATCH_LIST_BEGIN", 16) == 0) {
            return n;
        }
        if (strncmp(out, "MATCH_INFO", 10) == 0 || strcmp(out, "END_MATCH_LIST") == 0) {
            continue;
        }
        return n;
    }
}

/* ===== Match list ===== */
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

static int fetch_match_list(int sock, RecvBuf *rb) {
    send(sock, "GET_MATCH_LIST\n", 15, 0);
    char line[MAX_LINE];
    int n = recv_line_until_match_list(sock, rb, line, sizeof(line));
    if (n <= 0) return 0;
    int total = 0;
    if (sscanf(line, "MATCH_LIST_BEGIN %d", &total) != 1) return 0;
    match_list_count = 0;
    for (int i = 0; i < total && i < MAX_MATCHES; i++) {
        n = recv_line_rb(sock, rb, line, sizeof(line));
        if (n <= 0) break;
        MatchInfo *m = &match_list[match_list_count];
        if (sscanf(line, "MATCH_INFO %d %15s %d %31s %31s %15s %31s",
                   &m->match_id, m->stage, &m->group,
                   m->p1, m->p2, m->status, m->winner) == 7) {
            match_list_count++;
        }
    }
    recv_line_rb(sock, rb, line, sizeof(line)); /* consume END_MATCH_LIST */
    return 1;
}

static void display_match_list(void) {
    printf("\n=== Tournament Match Schedule ===\n");
    printf("%-4s %-6s %-5s %-14s %-14s %-12s %-14s\n",
           "ID","Stage","Grp","Player1","Player2","Status","Winner");
    printf("------------------------------------------------------------------------\n");
    for (int i = 0; i < match_list_count; i++) {
        MatchInfo *m = &match_list[i];
        const char *grp = (m->group == 0) ? "A" : (m->group == 1) ? "B" : "-";
        printf("%-4d %-6s %-5s %-14s %-14s %-12s %-14s\n",
               m->match_id, m->stage, grp,
               m->p1, m->p2, m->status,
               strcmp(m->winner,"NONE") == 0 ? "-" : m->winner);
    }
    printf("\n");
}

/* ===== Points table display ===== */
static void show_points_table(void) {
    FILE *f = fopen(POINTS_FILE, "r");
    if (!f) { printf("\n[Points table not yet available.]\n"); return; }
    printf("\n=== Points Table ===\n");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        printf("%s\n", line);
    }
    fclose(f);
    printf("\n");
}

/* ===== Live score display state ===== */
static int last_set1 = -1, last_set2 = -1;
static char last_game_points[32] = "";
static char last_p1_name[32] = "";
static char last_p2_name[32] = "";
static char last_serving_name[32] = "";
static char last_toss_summary[256] = "";

static void reset_watch_state(void) {
    last_set1 = -1; last_set2 = -1;
    last_game_points[0] = '\0';
    last_p1_name[0] = '\0'; last_p2_name[0] = '\0';
    last_serving_name[0] = '\0'; last_toss_summary[0] = '\0';
}

static int split_score(const char *gp, char *left, char *right, size_t len) {
    const char *sep = strchr(gp, '-');
    if (!sep) return 0;
    size_t ll = sep - gp, rl = strlen(sep+1);
    if (ll >= len || rl >= len) return 0;
    memcpy(left, gp, ll); left[ll] = '\0';
    strncpy(right, sep+1, len-1); right[len-1] = '\0';
    return 1;
}

static int read_score_file(int match_id,
                           int *set1, int *set2,
                           char *p1_name, char *p2_name,
                           char *game_points, size_t gplen,
                           char *serving_name, size_t svlen,
                           char *toss_summary, size_t tslen) {
    char fname[128];
    snprintf(fname, sizeof(fname), SCORE_FILE_TEMPLATE, match_id);
    int fd = open(fname, O_RDONLY);
    if (fd < 0) return 0;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        close(fd);
        return 0;
    }
    char data[1024];
    ssize_t n = read(fd, data, sizeof(data)-1);
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    close(fd);
    if (n <= 0) return 0;
    data[n] = '\0';
    *set1 = -1; *set2 = -1;
    p1_name[0] = '\0'; p2_name[0] = '\0';
    game_points[0] = '\0'; serving_name[0] = '\0'; toss_summary[0] = '\0';
    char *line = strtok(data, "\n");
    while (line) {
        if (strncmp(line, "Set Games:", 10) == 0)
            sscanf(line, "Set Games: %31s %d - %d %31s", p1_name, set1, set2, p2_name);
        else if (strncmp(line, "Current Game Points:", 20) == 0) {
            const char *p = line + 20;
            while (*p == ' ') p++;
            strncpy(game_points, p, gplen-1); game_points[gplen-1] = '\0';
        } else if (strncmp(line, "Serving:", 8) == 0) {
            const char *p = line + 8;
            while (*p == ' ') p++;
            strncpy(serving_name, p, svlen-1); serving_name[svlen-1] = '\0';
        } else if (strncmp(line, "Toss:", 5) == 0) {
            const char *p = line + 5;
            while (*p == ' ') p++;
            strncpy(toss_summary, p, tslen-1); toss_summary[tslen-1] = '\0';
        }
        line = strtok(NULL, "\n");
    }
    return (*set1 >= 0 && *set2 >= 0 && p1_name[0] && game_points[0]);
}

static void print_score_header(const char *ln, int ls, int rs, const char *rn,
                               const char *gp, const char *ts) {
    char lsc[32], rsc[32];
    printf("\n============= LIVE MATCH SCORE =============\n");
    printf("Set Games: %s %d - %d %s\n", ln, ls, rs, rn);
    if (split_score(gp, lsc, rsc, sizeof(lsc)))
        printf("Current Game Points: %s %s - %s %s\n", ln, lsc, rsc, rn);
    else
        printf("Current Game Points: %s\n", gp);
    if (ts[0]) printf("Toss: %s\n", ts);
}

static void handle_score_update(void) {
    if (watching_match_id < 0) return;
    int set1, set2;
    char p1[32], p2[32], gp[32], sv[32], ts[256];
    if (!read_score_file(watching_match_id, &set1, &set2,
                         p1, p2, gp, sizeof(gp), sv, sizeof(sv), ts, sizeof(ts))) {
        return;
    }
    const char *ln = sv[0] ? sv : p1;
    const char *rn = (strcmp(ln, p1) == 0) ? p2 : p1;
    int ls = (strcmp(ln, p1) == 0) ? set1 : set2;
    int rs = (strcmp(ln, p1) == 0) ? set2 : set1;

    if (last_set1 < 0) {
        /* First update */
        last_set1 = set1; last_set2 = set2;
        strncpy(last_p1_name, p1, 31); strncpy(last_p2_name, p2, 31);
        strncpy(last_serving_name, sv, 31); strncpy(last_toss_summary, ts, 255);
        strncpy(last_game_points, gp, 31);
        print_score_header(ln, ls, rs, rn, gp, ts);
    } else if (set1 != last_set1 || set2 != last_set2 ||
               strcmp(sv, last_serving_name) != 0) {
        char lsc[32], rsc[32];
        printf("\nSet Games: %s %d - %d %s\n", ln, ls, rs, rn);
        if (split_score(gp, lsc, rsc, sizeof(lsc)))
            printf("Current Game Points: %s %s - %s %s\n", ln, lsc, rsc, rn);
        else
            printf("Current Game Points: %s\n", gp);
        last_set1 = set1; last_set2 = set2;
        strncpy(last_serving_name, sv, 31);
        strncpy(last_game_points, gp, 31);
    } else if (strcmp(gp, last_game_points) != 0) {
        char lsc[32], rsc[32];
        if (split_score(gp, lsc, rsc, sizeof(lsc)))
            printf("\nCurrent Game Points: %s %s - %s %s\n", ln, lsc, rsc, rn);
        else
            printf("\nCurrent Game Points: %s\n", gp);
        strncpy(last_game_points, gp, 31);
    }
}

/* ===== Pick a match to watch ===== */
static int pick_match_to_watch(int sock, RecvBuf *rb) {
    fetch_match_list(sock, rb);
    display_match_list();

    if (match_list_count == 0) {
        printf("[No matches available yet.]\n");
        return -1;
    }

    printf("Enter Match ID to watch (or 0 to skip): ");
    fflush(stdout);
    char inp[16];
    if (!fgets(inp, sizeof(inp), stdin)) return -1;
    trim_newline(inp);
    int mid = atoi(inp);
    if (mid <= 0) return -1;

    /* Ask server for watch info */
    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "WATCH_MATCH %d\n", mid);
    send(sock, cmd, strlen(cmd), 0);
    char resp[MAX_LINE];
    if (recv_line_skip_notifications(sock, rb, resp, sizeof(resp), 0) <= 0) return -1;
    if (strncmp(resp, "ERROR", 5) == 0) {
        printf("Server: %s\n", resp);
        return -1;
    }
    /* WATCH_INFO <id> <p1> <p2> <LIVE|OTHER> */
    int rid; char rp1[32], rp2[32], rstatus[16];
    sscanf(resp, "WATCH_INFO %d %31s %31s %15s", &rid, rp1, rp2, rstatus);
    printf("Watching match %d: %s vs %s [%s]\n", rid, rp1, rp2, rstatus);
    reset_watch_state();
    return rid;
}

int main(void) {
    printf("Viewer client connecting to %s:%d...\n", SERVER_ADDR, SERVER_PORT);
    int sock = connect_to_server();
    if (sock < 0) return 1;

    RecvBuf rb;
    memset(&rb, 0, sizeof(rb));
    int logged_in = 0;
    char logged_in_username[64] = "";
    char recv_buffer[MAX_LINE];

    if (recv_line_skip_notifications(sock, &rb, recv_buffer, sizeof(recv_buffer), 0) > 0)
        printf("%s\n", recv_buffer);

    while (1) {
        if (!logged_in) {
            printf("\nViewer menu:\n1) Register\n2) Login\n3) Quit\nChoice: ");
            fflush(stdout);
            char choice[8];
            if (!fgets(choice, sizeof(choice), stdin)) break;
            trim_newline(choice);

            if (strcmp(choice, "1") == 0) {
                char username[64], password[64];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                char buf[MAX_LINE];
                snprintf(buf, sizeof(buf), "CHECK viewer %s\n", username);
                send(sock, buf, strlen(buf), 0);
                int n = recv_line_skip_notifications(sock, &rb, recv_buffer, sizeof(recv_buffer), 0);
                if (n <= 0) break;
                if (strncmp(recv_buffer, "OK AVAILABLE", 12) != 0) {
                    printf("Server: %s\n", recv_buffer);
                    continue;
                }
                printf("Password: ");
                if (!fgets(password, sizeof(password), stdin)) break;
                trim_newline(password);
                snprintf(buf, sizeof(buf), "REGISTER viewer %s %s\n", username, password);
                send(sock, buf, strlen(buf), 0);
                n = recv_line_skip_notifications(sock, &rb, recv_buffer, sizeof(recv_buffer), 0);
                if (n <= 0) break;
                printf("Server: %s\n", recv_buffer);
            } else if (strcmp(choice, "2") == 0) {
                char username[64], password[64];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                char buf[MAX_LINE];
                snprintf(buf, sizeof(buf), "CHECK viewer %s\n", username);
                send(sock, buf, strlen(buf), 0);
                int n = recv_line_skip_notifications(sock, &rb, recv_buffer, sizeof(recv_buffer), 0);
                if (n <= 0) break;
                if (strncmp(recv_buffer, "OK EXISTS", 9) != 0) {
                    printf("Server: ERROR No account found\n"); continue;
                }
                printf("Password: ");
                if (!fgets(password, sizeof(password), stdin)) break;
                trim_newline(password);
                snprintf(buf, sizeof(buf), "LOGIN viewer %s %s\n", username, password);
                send(sock, buf, strlen(buf), 0);
                strcpy(logged_in_username, username);
                n = recv_line_skip_notifications(sock, &rb, recv_buffer, sizeof(recv_buffer), 0);
                if (n <= 0) break;
                printf("Server: %s\n", recv_buffer);
                if (strcmp(recv_buffer, "OK Login successful") == 0) logged_in = 1;
            } else if (strcmp(choice, "3") == 0) {
                send(sock, "QUIT\n", 5, 0); break;
            } else {
                printf("Invalid choice.\n");
            }
            continue;
        }

        /* ===== Logged in ===== */

        if (watching_match_id < 0) {
            /* Not watching a match: show menu */
            printf("\nLogged in as %s\n", logged_in_username);
            printf("1) View all matches & select one to watch\n");
            printf("2) View points table\n");
            printf("3) Logout\n");
            printf("4) Quit\n");
            printf("Choice: ");
            fflush(stdout);

            fd_set wait_set;
            FD_ZERO(&wait_set);
            FD_SET(STDIN_FILENO, &wait_set);
            FD_SET(sock, &wait_set);
            int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
            int ready = select(maxfd + 1, &wait_set, NULL, NULL, NULL);
            if (ready < 0) { perror("select"); break; }

            if (FD_ISSET(sock, &wait_set)) {
                int n = recv_line_rb(sock, &rb, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;
                if (strcmp(recv_buffer, "SCORE_UPDATE") == 0) {
                    /* No match selected – ignore score updates */
                } else if (strcmp(recv_buffer, "MATCH_LIST_UPDATE") == 0) {
                    printf("\n[Tournament update: new matches available.]\n");
                } else if (strcmp(recv_buffer, "POINTS_UPDATE") == 0) {
                    printf("\n[Points table has been updated.]\n");
                    show_points_table();
                } else if (strncmp(recv_buffer, "MATCH_LIST_BEGIN", 16) == 0 ||
                           strncmp(recv_buffer, "MATCH_INFO", 10) == 0 ||
                           strcmp(recv_buffer, "END_MATCH_LIST") == 0) {
                    /* stray match-list response from earlier fetch; ignore */
                } else {
                    printf("Server: %s\n", recv_buffer);
                    if (strcmp(recv_buffer, "OK Logout successful") == 0) {
                        logged_in = 0; logged_in_username[0] = '\0'; watching_match_id = -1;
                    }
                }
                while (buffer_has_line(&rb)) {
                    n = recv_line_from_buffer(&rb, recv_buffer, sizeof(recv_buffer));
                    if (n <= 0) break;
                    if (strcmp(recv_buffer, "SCORE_UPDATE") == 0) {
                        /* No match selected – ignore score updates */
                    } else if (strcmp(recv_buffer, "MATCH_LIST_UPDATE") == 0) {
                        printf("\n[Tournament update: new matches available.]\n");
                    } else if (strcmp(recv_buffer, "POINTS_UPDATE") == 0) {
                        printf("\n[Points table has been updated.]\n");
                        show_points_table();
                    } else if (strncmp(recv_buffer, "MATCH_LIST_BEGIN", 16) == 0 ||
                               strncmp(recv_buffer, "MATCH_INFO", 10) == 0 ||
                               strcmp(recv_buffer, "END_MATCH_LIST") == 0) {
                        /* stray match-list response from earlier fetch; ignore */
                    } else {
                        printf("Server: %s\n", recv_buffer);
                        if (strcmp(recv_buffer, "OK Logout successful") == 0) {
                            logged_in = 0; logged_in_username[0] = '\0'; watching_match_id = -1;
                        }
                    }
                }
                continue;
            }

            char choice[8];
            if (!fgets(choice, sizeof(choice), stdin)) break;
            trim_newline(choice);

            if (strcmp(choice, "1") == 0) {
                watching_match_id = pick_match_to_watch(sock, &rb);
                if (watching_match_id < 0)
                    printf("Not watching any match.\n");
            } else if (strcmp(choice, "2") == 0) {
                show_points_table();
            } else if (strcmp(choice, "3") == 0) {
                char buf[MAX_LINE];
                snprintf(buf, sizeof(buf), "LOGOUT viewer %s\n", logged_in_username);
                send(sock, buf, strlen(buf), 0);
                int n = recv_line_skip_notifications(sock, &rb, recv_buffer, sizeof(recv_buffer), 0);
                if (n <= 0) break;
                printf("Server: %s\n", recv_buffer);
                if (strcmp(recv_buffer, "OK Logout successful") == 0) {
                    logged_in = 0; logged_in_username[0] = '\0'; watching_match_id = -1;
                }
            } else if (strcmp(choice, "4") == 0) {
                send(sock, "QUIT\n", 5, 0); break;
            } else {
                printf("Invalid choice.\n");
            }

        } else {
            /* ===== Watching a match: wait for server events ===== */
            fd_set wait_set;
            FD_ZERO(&wait_set);
            FD_SET(sock, &wait_set);
            int ready = select(sock + 1, &wait_set, NULL, NULL, NULL);
            if (ready < 0) { perror("select"); break; }

            if (FD_ISSET(sock, &wait_set)) {
                int n = recv_line_rb(sock, &rb, recv_buffer, sizeof(recv_buffer));
                if (n <= 0) break;

                if (strcmp(recv_buffer, "SCORE_UPDATE") == 0) {
                    handle_score_update();

                } else if (strcmp(recv_buffer, "MATCH_LIST_UPDATE") == 0) {
                    /* A match completed – check if it was the one we were watching */
                    fetch_match_list(sock, &rb);
                    int match_done = 0;
                    for (int i = 0; i < match_list_count; i++) {
                        if (match_list[i].match_id == watching_match_id &&
                            (strcmp(match_list[i].status, "DONE") == 0 ||
                             strcmp(match_list[i].status, "BYE") == 0)) {
                            printf("\n========================================\n");
                            printf("Match %d COMPLETED. Winner: %s\n",
                                   watching_match_id,
                                   strcmp(match_list[i].winner,"NONE")==0 ? "?" : match_list[i].winner);
                            printf("========================================\n");
                            match_done = 1;
                            break;
                        }
                    }
                    if (match_done) {
                        watching_match_id = -1;
                        reset_watch_state();
                        /* Show points table */
                        show_points_table();
                        /* Show all matches, let viewer pick another */
                        printf("Select another match to watch:\n");
                        watching_match_id = pick_match_to_watch(sock, &rb);
                        if (watching_match_id < 0)
                            printf("Returning to main menu.\n");
                    }

                } else if (strcmp(recv_buffer, "POINTS_UPDATE") == 0) {
                    printf("\n[Points table updated.]\n");
                    show_points_table();

                } else if (strncmp(recv_buffer, "MATCH_LIST_BEGIN", 16) == 0 ||
                           strncmp(recv_buffer, "MATCH_INFO", 10) == 0 ||
                           strcmp(recv_buffer, "END_MATCH_LIST") == 0) {
                    continue;
                } else {
                    printf("Server: %s\n", recv_buffer);
                    if (strcmp(recv_buffer, "OK Logout successful") == 0) {
                        logged_in = 0; logged_in_username[0] = '\0'; watching_match_id = -1;
                    }
                }
            }
        }
    }

    close(sock);
    return 0;
}
