#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 9001
#define SERVER_ADDR "127.0.0.1"
#define MAX_LINE 256
#define SCORE_FILE "match_score.txt"

enum {
    SIDE_RAMANUJAN = 0,
    SIDE_LILAVATI = 1
};

enum {
    BALL_LILA_RIGHT = 0,
    BALL_LILA_LEFT = 1,
    BALL_RAM_RIGHT = 2,
    BALL_RAM_LEFT = 3
};

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

static int timed_menu_choice(int seconds, int min_choice, int max_choice, int *choice_out, double *elapsed_out) {
    struct timespec start_ts;
    struct timespec end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    int ret = select(STDIN_FILENO + 1, &set, NULL, NULL, &tv);
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    if (elapsed_out) {
        double sec = (double)(end_ts.tv_sec - start_ts.tv_sec);
        double nsec = (double)(end_ts.tv_nsec - start_ts.tv_nsec) / 1000000000.0;
        *elapsed_out = sec + nsec;
    }
    if (ret <= 0) return 0;
    char line[32];
    if (!fgets(line, sizeof(line), stdin)) return 0;
    trim_newline(line);
    int val = atoi(line);
    if (val < min_choice || val > max_choice) return -1;
    *choice_out = val;
    return 1;
}

static int blocking_menu_choice(int min_choice, int max_choice, int *choice_out) {
    while (1) {
        char line[32];
        if (!fgets(line, sizeof(line), stdin)) return 0;
        trim_newline(line);
        int val = atoi(line);
        if (val >= min_choice && val <= max_choice) {
            *choice_out = val;
            return 1;
        }
        printf("Invalid input. Enter a number between %d and %d: ", min_choice, max_choice);
        fflush(stdout);
    }
}

static int fifo_read_line(int fd, char *buf, size_t buflen) {
    size_t used = 0;
    while (used < buflen - 1) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) return (int)n;
        if (ch == '\n') {
            buf[used] = '\0';
            return (int)used;
        }
        buf[used++] = ch;
    }
    buf[buflen - 1] = '\0';
    return (int)(buflen - 1);
}

static int fifo_send_line(int fd, const char *msg) {
    size_t len = strlen(msg);
    ssize_t n = write(fd, msg, len);
    return (n == (ssize_t)len) ? 1 : 0;
}

static int next_ball_state(int hitter_side, int current_state, int shot_choice) {
    int current_is_right = (current_state == BALL_LILA_RIGHT || current_state == BALL_RAM_RIGHT);
    int target_is_right = (shot_choice == 1) ? current_is_right : !current_is_right;
    int target_side = (hitter_side == SIDE_LILAVATI) ? SIDE_RAMANUJAN : SIDE_LILAVATI;
    if (target_side == SIDE_LILAVATI) return target_is_right ? BALL_LILA_RIGHT : BALL_LILA_LEFT;
    return target_is_right ? BALL_RAM_RIGHT : BALL_RAM_LEFT;
}

static int ball_side(int state) {
    if (state == BALL_LILA_RIGHT || state == BALL_LILA_LEFT) return SIDE_LILAVATI;
    return SIDE_RAMANUJAN;
}

static const char *raw_point_label(int p) {
    if (p <= 0) return "0";
    if (p == 1) return "15";
    if (p == 2) return "30";
    return "40";
}

static void format_game_points(int server_points, int receiver_points, char *out, size_t outlen) {
    if (server_points >= 3 && receiver_points >= 3) {
        if (server_points == receiver_points) {
            snprintf(out, outlen, "40-40");
        } else if (server_points == receiver_points + 1) {
            snprintf(out, outlen, "AD-40");
        } else if (receiver_points == server_points + 1) {
            snprintf(out, outlen, "40-AD");
        } else {
            snprintf(out, outlen, "%s-%s", raw_point_label(server_points), raw_point_label(receiver_points));
        }
        return;
    }
    snprintf(out, outlen, "%s-%s", raw_point_label(server_points), raw_point_label(receiver_points));
}

static void write_score_snapshot(const char *p1, const char *p2, int g1, int g2, int gp1, int gp2) {
    int fd = open(SCORE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        close(fd);
        return;
    }
    char game_points[32];
    format_game_points(gp1, gp2, game_points, sizeof(game_points));
    char out[512];
    snprintf(out, sizeof(out),
             "Match Score\n"
             "Set Games: %s %d - %d %s\n"
             "Current Game Points: %s\n",
             p1, g1, g2, p2, game_points);
    write(fd, out, strlen(out));
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    close(fd);
}

static int apply_point_result(int winner_p1, int *gp1, int *gp2) {
    if (winner_p1) {
        if (*gp1 >= 3 && *gp2 >= 3) {
            if (*gp1 == *gp2) (*gp1)++;
            else if (*gp1 > *gp2) return 1;
            else (*gp2)--;
        } else {
            (*gp1)++;
            if (*gp1 >= 4 && (*gp1 - *gp2) >= 2) return 1;
        }
    } else {
        if (*gp1 >= 3 && *gp2 >= 3) {
            if (*gp1 == *gp2) (*gp2)++;
            else if (*gp2 > *gp1) return 2;
            else (*gp1)--;
        } else {
            (*gp2)++;
            if (*gp2 >= 4 && (*gp2 - *gp1) >= 2) return 2;
        }
    }
    return 0;
}

static int run_match(int sock, const char *my_username, int my_player_no, const char *opponent_username,
                     const char *write_fifo_path, const char *read_fifo_path) {
    printf("\n=== Match Started: %s vs %s ===\n\n", my_username, opponent_username);
    mkfifo(write_fifo_path, 0666);
    mkfifo(read_fifo_path, 0666);

    int write_fd = open(write_fifo_path, O_RDWR);
    int read_fd = open(read_fifo_path, O_RDWR);
    if (write_fd < 0 || read_fd < 0) {
        perror("fifo open");
        if (write_fd >= 0) close(write_fd);
        if (read_fd >= 0) close(read_fd);
        return -1;
    }

    srand((unsigned int)(time(NULL) ^ getpid()));

    int my_is_p1 = (my_player_no == 1);
    const char *p1_name = my_is_p1 ? my_username : opponent_username;
    const char *p2_name = my_is_p1 ? opponent_username : my_username;
    int side_p1 = SIDE_RAMANUJAN;
    int initial_server_p1 = 1;
    char line[MAX_LINE];
    if (my_player_no == 1) {
        printf("Toss time.\n");
        printf("Enter your toss call:\n");
        printf("1) Heads\n");
        printf("2) Tails\n");
        printf("Enter choice (1 or 2): ");
        fflush(stdout);
        int toss_call = 1;
        if (!blocking_menu_choice(1, 2, &toss_call)) goto walkover_win;
        int toss_call_bit = (toss_call == 1) ? 0 : 1;
        int toss_result = rand() % 2;
        int winner = (toss_call_bit == toss_result) ? 1 : 2;
        snprintf(line, sizeof(line), "TOSS %d %d\n", toss_result, winner);
        write(write_fd, line, strlen(line));
        printf("Toss result is %s. Toss winner is %s.\n", toss_result == 0 ? "Heads" : "Tails", winner == 1 ? p1_name : p2_name);
    } else {
        if (fifo_read_line(read_fd, line, sizeof(line)) <= 0) goto walkover_win;
        printf("Waiting for Player1 toss call...\n");
    }

    int toss_result = 0, toss_winner = 1;
    if (sscanf(line, "TOSS %d %d", &toss_result, &toss_winner) != 2) {
        if (my_player_no == 1) sscanf(line, "TOSS %d %d", &toss_result, &toss_winner);
    }

    int i_am_winner = (my_player_no == toss_winner);
    if (i_am_winner) {
        printf("You won the toss.\n");
        printf("Choose what you want:\n");
        printf("1) Choose to serve first\n");
        printf("2) Choose your side\n");
        printf("Enter choice (1 or 2): ");
        fflush(stdout);
        int ch = 1;
        if (!blocking_menu_choice(1, 2, &ch)) goto walkover_win;
        if (ch == 1) {
            snprintf(line, sizeof(line), "WCHOICE SERVE\n");
            write(write_fd, line, strlen(line));
            printf("You chose to serve first.\n");
            printf("Waiting for opponent to choose side...\n");
            while (1) {
                if (fifo_read_line(read_fd, line, sizeof(line)) <= 0) goto walkover_win;
                int loser_side;
                if (sscanf(line, "LRESP SIDE %d", &loser_side) == 1) {
                    if (toss_winner == 1) side_p1 = (loser_side == SIDE_RAMANUJAN) ? SIDE_LILAVATI : SIDE_RAMANUJAN;
                    else side_p1 = loser_side;
                    initial_server_p1 = (toss_winner == 1);
                    break;
                }
            }
        } else {
            printf("Choose your side:\n");
            printf("1) Ramanujan side\n");
            printf("2) Lilavati side\n");
            printf("Enter choice (1 or 2): ");
            fflush(stdout);
            int side_choice = 1;
            if (!blocking_menu_choice(1, 2, &side_choice)) goto walkover_win;
            int winner_side = (side_choice == 1) ? SIDE_RAMANUJAN : SIDE_LILAVATI;
            snprintf(line, sizeof(line), "WCHOICE SIDE %d\n", winner_side);
            write(write_fd, line, strlen(line));
            printf("You chose side. Waiting for opponent to choose serve/receive...\n");
            while (1) {
                if (fifo_read_line(read_fd, line, sizeof(line)) <= 0) goto walkover_win;
                int loser_decision;
                if (sscanf(line, "LRESP SR %d", &loser_decision) == 1) {
                    if (toss_winner == 1) side_p1 = winner_side;
                    else side_p1 = (winner_side == SIDE_RAMANUJAN) ? SIDE_LILAVATI : SIDE_RAMANUJAN;
                    initial_server_p1 = (loser_decision == 1) ? (toss_winner != 1) : (toss_winner == 1);
                    break;
                }
            }
        }
    } else {
        while (1) {
            if (fifo_read_line(read_fd, line, sizeof(line)) <= 0) goto walkover_win;
            int winner_side;
            if (strstr(line, "WCHOICE SERVE") == line) {
                printf("You lost toss. Toss winner chose to serve first.\n");
                printf("Choose your side:\n");
                printf("1) Ramanujan side\n");
                printf("2) Lilavati side\n");
                printf("Enter choice (1 or 2): ");
                fflush(stdout);
                int side_choice = 1;
                if (!blocking_menu_choice(1, 2, &side_choice)) goto walkover_win;
                int my_side = (side_choice == 1) ? SIDE_RAMANUJAN : SIDE_LILAVATI;
                snprintf(line, sizeof(line), "LRESP SIDE %d\n", my_side);
                write(write_fd, line, strlen(line));
                if (toss_winner == 1) side_p1 = (my_side == SIDE_RAMANUJAN) ? SIDE_LILAVATI : SIDE_RAMANUJAN;
                else side_p1 = my_side;
                initial_server_p1 = (toss_winner == 1);
                break;
            }
            if (sscanf(line, "WCHOICE SIDE %d", &winner_side) == 1) {
                printf("You lost toss. Toss winner chose side.\n");
                printf("Choose your option:\n");
                printf("1) Serve first\n");
                printf("2) Receive first\n");
                printf("Enter choice (1 or 2): ");
                fflush(stdout);
                int sr = 1;
                if (!blocking_menu_choice(1, 2, &sr)) goto walkover_win;
                snprintf(line, sizeof(line), "LRESP SR %d\n", sr);
                write(write_fd, line, strlen(line));
                if (toss_winner == 1) side_p1 = winner_side;
                else side_p1 = (winner_side == SIDE_RAMANUJAN) ? SIDE_LILAVATI : SIDE_RAMANUJAN;
                initial_server_p1 = (sr == 1) ? (toss_winner != 1) : (toss_winner == 1);
                break;
            }
        }
    }

    int games_p1 = 0, games_p2 = 0;
    int gp1 = 0, gp2 = 0;
    printf("\n========================================\n");
    printf("Match play begins now.\n");
    while (games_p1 < 6 && games_p2 < 6) {
        int game_index = games_p1 + games_p2;
        int server_is_p1 = (game_index % 2 == 0) ? initial_server_p1 : !initial_server_p1;
        int i_serve_this_game = (server_is_p1 == my_is_p1);
        int my_side = my_is_p1 ? side_p1 : (1 - side_p1);
        int server_side = server_is_p1 ? side_p1 : (1 - side_p1);
        int receiver_side = (server_side == SIDE_RAMANUJAN) ? SIDE_LILAVATI : SIDE_RAMANUJAN;
        const char *server_name = server_is_p1 ? p1_name : p2_name;
        const char *receiver_name = server_is_p1 ? p2_name : p1_name;

        printf("========================================\n");
        printf("Game %d\n", game_index + 1);
        printf("Set score: %s %d - %d %s\n", server_name,
               server_is_p1 ? games_p1 : games_p2, server_is_p1 ? games_p2 : games_p1, receiver_name);
        printf("Current side: %s\n", my_side == SIDE_RAMANUJAN ? "Ramanujan" : "Lilavati");
        printf("========================================\n");

        while (1) {
            int current_state = (receiver_side == SIDE_RAMANUJAN) ? BALL_RAM_RIGHT : BALL_LILA_RIGHT;
            int i_turn = 0;

            if (i_serve_this_game) {
                printf("You are serving this point.\n");
                printf("Enter 1 to serve now: ");
                fflush(stdout);
                int serve_cmd = 1;
                if (!blocking_menu_choice(1, 1, &serve_cmd)) goto walkover_win;
                char serve_msg[MAX_LINE];
                snprintf(serve_msg, sizeof(serve_msg), "SERVE %d\n", current_state);
                if (!fifo_send_line(write_fd, serve_msg)) goto walkover_win;
                i_turn = 0;
                printf("Serve delivered. Rally has started.\n");
            } else {
                printf("Opponent is serving... waiting for serve.\n");
                if (fifo_read_line(read_fd, line, sizeof(line)) <= 0) goto walkover_win;
                if (sscanf(line, "SERVE %d", &current_state) != 1) {
                    goto walkover_win;
                }
                i_turn = 1;
                printf("Serve received. Your turn.\n");
            }

            int point_winner_p1 = -1;
            while (point_winner_p1 == -1) {
                printf("\n----------------------------------------\n");
                if (i_turn) {
                    if (ball_side(current_state) != my_side) {
                        point_winner_p1 = my_is_p1 ? 0 : 1;
                        break;
                    }
                    printf("Enter shot choice:\n");
                    printf("1) Down the line\n");
                    printf("2) Cross court\n");
                    printf("Enter choice (1 or 2): ");
                    fflush(stdout);
                    int shot = 1;
                    double elapsed = 0.0;
                    int status = timed_menu_choice(10, 1, 2, &shot, &elapsed);
                    int limit_ok = (status == 1);
                    if (limit_ok && elapsed > 10.0) limit_ok = 0;
                    if (!limit_ok) {
                        printf("\n");
                        if (!fifo_send_line(write_fd, "MISS\n")) goto walkover_win;
                        point_winner_p1 = my_is_p1 ? 0 : 1;
                        break;
                    }
                    current_state = next_ball_state(my_side, current_state, shot);
                    char shot_msg[MAX_LINE];
                    snprintf(shot_msg, sizeof(shot_msg), "SHOT %d\n", current_state);
                    if (!fifo_send_line(write_fd, shot_msg)) goto walkover_win;
                    i_turn = 0;
                } else {
                    printf("Waiting for opponent shot...\n");
                    if (fifo_read_line(read_fd, line, sizeof(line)) <= 0) goto walkover_win;
                    if (strncmp(line, "MISS", 4) == 0) {
                        point_winner_p1 = my_is_p1 ? 1 : 0;
                    } else if (sscanf(line, "SHOT %d", &current_state) == 1) {
                        i_turn = 1;
                    } else {
                        point_winner_p1 = my_is_p1 ? 1 : 0;
                    }
                }
            }

            int i_won_point = (point_winner_p1 == 1 && my_is_p1) || (point_winner_p1 == 0 && !my_is_p1);
            int game_winner = 0;
            if (i_won_point) {
                game_winner = apply_point_result(point_winner_p1 == 1, &gp1, &gp2);
                if (game_winner == 1) {
                    games_p1++;
                    gp1 = 0;
                    gp2 = 0;
                } else if (game_winner == 2) {
                    games_p2++;
                    gp1 = 0;
                    gp2 = 0;
                }
                write_score_snapshot(my_is_p1 ? my_username : opponent_username,
                                     my_is_p1 ? opponent_username : my_username,
                                     games_p1, games_p2, gp1, gp2);
                if (sock >= 0) {
                    send(sock, "SCORE_UPDATE\n", 13, 0);
                }
                char point_msg[MAX_LINE];
                snprintf(point_msg, sizeof(point_msg), "POINT %d %d %d %d %d %d\n",
                         point_winner_p1, games_p1, games_p2, gp1, gp2, game_winner);
                if (!fifo_send_line(write_fd, point_msg)) goto walkover_win;
            } else {
                if (fifo_read_line(read_fd, line, sizeof(line)) <= 0) goto walkover_win;
                int winner_msg, g1m, g2m, gp1m, gp2m, gwm;
                if (sscanf(line, "POINT %d %d %d %d %d %d", &winner_msg, &g1m, &g2m, &gp1m, &gp2m, &gwm) == 6) {
                    games_p1 = g1m;
                    games_p2 = g2m;
                    gp1 = gp1m;
                    gp2 = gp2m;
                    game_winner = gwm;
                    (void)winner_msg;
                }
            }
            char game_points_display[32];
            int server_points = server_is_p1 ? gp1 : gp2;
            int receiver_points = server_is_p1 ? gp2 : gp1;
            format_game_points(server_points, receiver_points, game_points_display, sizeof(game_points_display));
            const char *point_winner_name = (point_winner_p1 == 1) ? p1_name : p2_name;
            printf("\n----------------------------------------\n");
            printf("**Point won by: %s**\n", point_winner_name);
            printf("**Game points: %s**\n", game_points_display);
            printf("----------------------------------------\n");

            if (game_winner == 1 || game_winner == 2) {
                const char *game_winner_name = (game_winner == 1) ? p1_name : p2_name;
                printf("\n========================================\n");
                printf("Game won by: %s\n", game_winner_name);
                printf("Set score: %s %d - %d %s\n", server_name,
                       server_is_p1 ? games_p1 : games_p2,
                       server_is_p1 ? games_p2 : games_p1,
                       receiver_name);
                printf("========================================\n");
                break;
            }
        }

        if (((games_p1 + games_p2) % 2) == 1) {
            side_p1 = (side_p1 == SIDE_RAMANUJAN) ? SIDE_LILAVATI : SIDE_RAMANUJAN;
            printf("Sides switched.\n");
        }
    }

    int p1_won = games_p1 >= 6;
    const char *set_winner = p1_won ? p1_name : p2_name;
    printf("\n========================================\n");
    printf("MATCH COMPLETE\n");
    printf("Winner: %s\n", set_winner);
    printf("Final set score: %s %d - %d %s\n", p1_name, games_p1, games_p2, p2_name);
    printf("========================================\n");
    close(write_fd);
    close(read_fd);
    return ((my_player_no == 1) ? p1_won : !p1_won) ? 1 : 0;

walkover_win:
    printf("\n========================================\n");
    printf("Match ended by disconnection.\n");
    printf("Winner by walkover: %s\n", my_username);
    printf("========================================\n");
    close(write_fd);
    close(read_fd);
    return 1;
}

static int handle_server_line(int sock, int logged_in, const char *logged_in_username, const char *line) {
    if (strncmp(line, "MATCH_START", 11) == 0 && logged_in) {
        char role[8], opponent[64], write_fifo[128], read_fifo[128];
        if (sscanf(line, "MATCH_START %7s %63s %127s %127s", role, opponent, write_fifo, read_fifo) == 4) {
            int my_no = (strcmp(role, "P1") == 0) ? 1 : 2;
            run_match(sock, logged_in_username, my_no, opponent, write_fifo, read_fifo);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    printf("Player client connecting to %s:%d...\n", SERVER_ADDR, SERVER_PORT);
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
                if (handle_server_line(sock, logged_in, logged_in_username, recv_buffer)) continue;
                printf("\nServer: %s\n", recv_buffer);
                if (strcmp(recv_buffer, "OK Logout successful") == 0) {
                    logged_in = 0;
                    logged_in_username[0] = '\0';
                }
                continue;
            }
        } else {
            printf("\nPlayer menu:\n");
            printf("1) Register\n");
            printf("2) Login\n");
            printf("3) Quit\n");
            printf("Choice: ");
        }
        fflush(stdout);

        char choice[8];
        if (!fgets(choice, sizeof(choice), stdin)) break;
        trim_newline(choice);
        if (logged_in) {
            if (strcmp(choice, "1") == 0 || strcasecmp(choice, "logout") == 0) {
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "LOGOUT player %s\n", logged_in_username);
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
                char ranking[16];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "CHECK player %s\n", username);
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
                printf("AITA ranking: ");
                if (!fgets(ranking, sizeof(ranking), stdin)) break;
                trim_newline(ranking);
                snprintf(send_buffer, sizeof(send_buffer), "REGISTER player %s %s %s\n", username, password, ranking);
                send(sock, send_buffer, strlen(send_buffer), 0);
            } else if (strcmp(choice, "2") == 0 || strcasecmp(choice, "login") == 0) {
                char username[64];
                char password[64];
                printf("Username: ");
                if (!fgets(username, sizeof(username), stdin)) break;
                trim_newline(username);
                char send_buffer[MAX_LINE];
                snprintf(send_buffer, sizeof(send_buffer), "CHECK player %s\n", username);
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
                snprintf(send_buffer, sizeof(send_buffer), "LOGIN player %s %s\n", username, password);
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

        n = recv_line(sock, recv_buffer, sizeof(recv_buffer));
        if (n <= 0) break;
        if (handle_server_line(sock, logged_in, logged_in_username, recv_buffer)) continue;
        printf("Server: %s\n", recv_buffer);
        if (strcmp(recv_buffer, "OK Login successful") == 0) {
            logged_in = 1;
            // logged_in_username is set in the login block above
        } else if (strcmp(recv_buffer, "OK Logout successful") == 0) {
            logged_in = 0;
            logged_in_username[0] = '\0';
        }
    }

    close(sock);
    return 0;
}
