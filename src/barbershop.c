/*
Copyright (c) 2010 Nick Gerakines <nick at gerakines dot net>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <time.h>

#include "scores.h"
#include "bst.h"
#include "barbershop.h"
#include "stats.h"
#include <event.h>

static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
	char *s, *e;
	size_t ntokens = 0;
	for (s = e = command; ntokens < max_tokens - 1; ++e) {
		if (*e == ' ') {
			if (s != e) {
				tokens[ntokens].value = s;
				tokens[ntokens].length = e - s;
				ntokens++;
				*e = '\0';
			}
			s = e + 1;
		} else if (*e == '\0') {
			if (s != e) {
				tokens[ntokens].value = s;
				tokens[ntokens].length = e - s;
				ntokens++;
			}
			break;
		}
	}
	tokens[ntokens].value =  *e == '\0' ? NULL : e;
	tokens[ntokens].length = 0;
	ntokens++;
	return ntokens;
}

// TODO: Create an actual source file that decomposes input and
// determines execution.
void on_read(int fd, short ev, void *arg) {
	struct client *client = (struct client *)arg;
	// TODO: Find out what a reasonable size limit is on network input.
	char buf[8196];
	int len = read(fd, buf, sizeof(buf));
	if (len == 0) {
		printf("Client disconnected.\n");
		close(fd);
		event_del(&client->ev_read);
		free(client);
		return;
	} else if (len < 0) {
		printf("Socket failure, disconnecting client: %s", strerror(errno));
		close(fd);
		event_del(&client->ev_read);
		free(client);
		return;
	}
	// TOOD: Find a better way to do this {
	char* nl;
	nl = strrchr(buf, '\r');
	if (nl) { *nl = '\0'; }
	nl = strrchr(buf, '\n');
	if (nl) { *nl = '\0'; }
	// }
	token_t tokens[MAX_TOKENS];
	size_t ntokens = tokenize_command((char*)buf, tokens, MAX_TOKENS);
	// TODO: Add support for the 'quit' command.
	if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN].value, "update") == 0) {
		int item_id = atoi(tokens[KEY_TOKEN].value);
		int score = atoi(tokens[VALUE_TOKEN].value);

		Position lookup = Find( item_id, items );
		if (lookup == NULL) {
			items = Insert(item_id, score, items);
			scores = AddScoreToPool(scores, score, item_id);
			app_stats.items += 1;
			app_stats.items_gc += 1;
		} else {
			int old_score = lookup->score;
			lookup->score += score;
			scores = PurgeThenAddScoreToPool(scores, lookup->score, item_id, old_score);
		}
		app_stats.updates += 1;
		reply(fd, "OK\r\n");
	} else if (ntokens == 2 && strcmp(tokens[COMMAND_TOKEN].value, "next") == 0) {
		int next = GetNextItem(scores);
		if (next != -1) {
			app_stats.items_gc -= 1;
		}
		char msg[32];
		sprintf(msg, "%d\r\n", next);
		reply(fd, msg);
	} else if (ntokens == 2 && strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0) {
		// TODO: Find out of the stats output buffer can be reduced.
		char out[128];
		time_t current_time;
		time(&current_time);
		sprintf(out, "STAT uptime %d\r\n", (int)(current_time - app_stats.started_at)); reply(fd, out);
		sprintf(out, "STAT version %s\r\n", app_stats.version); reply(fd, out);
		sprintf(out, "STAT updates %d\r\n", app_stats.updates); reply(fd, out);
		sprintf(out, "STAT items %d\r\n", app_stats.items); reply(fd, out);
		sprintf(out, "STAT pools %d\r\n", app_stats.pools); reply(fd, out);
		sprintf(out, "STAT pools_gc %d\r\n", app_stats.pools_gc); reply(fd, out);
		sprintf(out, "STAT items_gc %d\r\n", app_stats.items_gc); reply(fd, out);
		reply(fd, "END\r\n");
		/*
		printf("Dumping items tree:\n");
		DumpItems(items);
		printf("Dumping score buckets:\n");
		DumpScores(scores);
		*/
	} else {
		reply(fd, "ERROR\r\n");
	}
}

void on_accept(int fd, short ev, void *arg) {
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct client *client;
    client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == -1) {
        warn("accept failed");
        return;
    }
    if (setnonblock(client_fd) < 0) {
        warn("failed to set client socket non-blocking");
    }
    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        err(1, "malloc failed");
    }
    event_set(&client->ev_read, client_fd, EV_READ|EV_PERSIST, on_read, client);
    event_add(&client->ev_read, NULL);
    printf("Accepted connection from %s\n", inet_ntoa(client_addr.sin_addr));
}

int main(int argc, char **argv) {
	items = MakeEmpty(NULL);
	scores = PrepScoreBucket(NULL);

	time(&app_stats.started_at);
	app_stats.version = "00.01.00";
	app_stats.updates = 0;
	app_stats.items = 0;
	app_stats.pools = 0;
	app_stats.items_gc = 0;
	app_stats.pools_gc = 0;

    int listen_fd;
    struct sockaddr_in listen_addr;
    int reuseaddr_on = 1;
    struct event ev_accept;
    event_init();
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { err(1, "listen failed"); }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_on, sizeof(reuseaddr_on)) == -1) { err(1, "setsockopt failed"); }
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(SERVER_PORT);
    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) { err(1, "bind failed"); }
    if (listen(listen_fd, 5) < 0) { err(1, "listen failed"); }
    if (setnonblock(listen_fd) < 0) { err(1, "failed to set server socket to non-blocking"); }
    event_set(&ev_accept, listen_fd, EV_READ|EV_PERSIST, on_accept, NULL);
    event_add(&ev_accept, NULL);
    event_dispatch();
    return 0;
}

int setnonblock(int fd) {
    int flags;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0) { return flags; }
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) { return -1; }
    return 0;
}

void reply(int fd, char *buffer) {
    int n = write(fd, buffer, strlen(buffer));
    if (n < 0 || n < strlen(buffer))
         printf("ERROR writing to socket");
}