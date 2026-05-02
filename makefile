CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L

all: server player viewer admin

server: server.c
	$(CC) $(CFLAGS) -o server server.c -lpthread -lrt

player: player.c
	$(CC) $(CFLAGS) -o player player.c -lrt

viewer: viewer.c
	$(CC) $(CFLAGS) -o viewer viewer.c

admin: admin.c
	$(CC) $(CFLAGS) -o admin admin.c

clean:
	rm -f server player viewer admin

.PHONY: all clean