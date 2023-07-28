#include "chat.h"
#include "chat_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

static int
port_from_str(const char *str, uint16_t *port)
{
	errno = 0;
	char *end = NULL;
	long res = strtol(str, &end, 10);
	if (res == 0 && errno != 0)
		return -1;
	if (*end != 0)
		return -1;
	if (res > UINT16_MAX || res < 0)
		return -1;
	*port = (uint16_t)res;
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("Expected a port to listen on\n");
		return -1;
	}
	uint16_t port = 0;
	int rc = port_from_str(argv[1], &port);
	if (rc != 0) {
		printf("Invalid port\n");
		return -1;
	}
	struct chat_server *serv = chat_server_new();
	rc = chat_server_listen(serv, port);
	if (rc != 0) {
		printf("Couldn't listen: %d\n", rc);
		chat_server_delete(serv);
		return -1;
	}
#if NEED_SERVER_FEED
	struct pollfd poll_fds[2] = {[0] = {.fd = STDIN_FILENO, .events = POLLIN}};

	struct pollfd *poll_stdin = &poll_fds[0], *poll_server_queue = &poll_fds[1];
	poll_server_queue->fd = chat_server_get_descriptor(serv);
#endif
	/*
	 * The basic implementation without server messages. Just serving
	 * clients.
	 */
	while (true) {
#if NEED_SERVER_FEED
		poll_server_queue->events = chat_events_to_poll_events(chat_server_get_events(serv));

		int rc = poll(poll_fds, 2, -1);
		if (rc < 0) {
			printf("poll failed: %d\n", rc);
			break;
		}

		if (poll_stdin->revents) {
			const size_t bufsiz = 1024;
			char buf[bufsiz];
			ssize_t got = read(STDIN_FILENO, buf, bufsiz);
			if (got < 0) {
				printf("Failed reading from stdin: %s\n", strerror(errno));
				break;
			} else if (got == 0) {
				puts("EOF. Exiting");
				break;
			} else {
				int err = chat_server_feed(serv, buf, got);
				if (err) {
					printf("chat_server_feed failed: %d\n", err);
				}
			}
		}

		if (poll_server_queue->revents) {
			// Let the server handle it
#endif

		int rc = chat_server_update(serv, -1);
		if (rc != 0) {
			printf("Update error: %d\n", rc);
			break;
		}
		/* Flush all the pending messages to the standard output. */
		struct chat_message *msg;
		while ((msg = chat_server_pop_next(serv)) != NULL) {
#if NEED_AUTHOR
			printf("%s: %s\n", msg->author, msg->data);
#else
			printf("%s\n", msg->data);
#endif
			chat_message_delete(msg);
		}

#if NEED_SERVER_FEED
		}
#endif
	}
	chat_server_delete(serv);
	return 0;
}
