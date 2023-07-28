#include "chat.h"
#include "chat_client.h"
#include "partial_message_queue.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <assert.h>

struct chat_client {
	/** Socket connected to the server */
	int socket;
	/** Incoming messages queue */
	struct partial_message_queue incoming;
	/** Outgoing messages queue */
	struct partial_message_queue outgoing;

#if NEED_AUTHOR
	char *name;
#endif
};

struct chat_client *
chat_client_new(const char *name)
{
	struct chat_client *client = calloc(1, sizeof(*client));
	client->socket = -1;

	pmq_init(&client->incoming, 16);
	pmq_init(&client->outgoing, 16);

#if NEED_AUTHOR
	assert(!strchr(name, '\n'));  // Client name with `'\n'`s are not allowed
	client->name = strdup(name);
	if (!client->name)
		abort();
#else
	(void)name;
#endif

	return client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (client->socket >= 0)
		(void)close(client->socket);

	pmq_destroy(&client->incoming);
	pmq_destroy(&client->outgoing);
#if NEED_AUTHOR
	free(client->name);
#endif

	free(client);
}

int
chat_client_connect(struct chat_client *client, const char *addr)
{
	if (client->socket > 0)
		return CHAT_ERR_ALREADY_STARTED;

	char *addr_dup = strdup(addr);
	assert(addr_dup);
	char *colon = strchr(addr_dup, ':');
	assert(colon);
	*colon = '\0';

	/*
	 * 1) Use getaddrinfo() to resolve addr to struct sockaddr_in.
	 * 2) Create a client socket (function socket()).
	 * 3) Connect it by the found address (function connect()).
	 */

	struct addrinfo *result, hints = {.ai_family = AF_INET};
	int err = getaddrinfo(addr_dup, colon + 1, &hints, &result);
	if (err != 0) {
		return CHAT_ERR_NO_ADDR;
	}

	int sockfd;
	struct addrinfo *aip;
	for (aip = result; aip != NULL; aip = aip->ai_next) {
		sockfd = socket(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
		if (sockfd < 0) {
			if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT || errno == EPROTOTYPE) {
				continue;
			}
			return CHAT_ERR_SYS;
		}
		if (0 == connect(sockfd, aip->ai_addr, aip->ai_addrlen))
			break;
		(void)close(sockfd);
	}
	freeaddrinfo(result);
	free(addr_dup);

	if (aip == NULL) {
		return CHAT_ERR_NO_ADDR;
	}
	if (sockfd < 0)
		return CHAT_ERR_SYS;
	if (0 > fcntl(sockfd, F_SETFL, O_NONBLOCK)) {
		close(sockfd);
		return CHAT_ERR_SYS;
	}
	client->socket = sockfd;

#if NEED_AUTHOR
	chat_client_feed(client, client->name, strlen(client->name));
	chat_client_feed(client, "\n", 1);
#endif

	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	struct chat_message *ret = malloc(sizeof *ret);
	if (!ret)
		abort();

#if NEED_AUTHOR
	const char *author = pmq_next_message(&client->incoming), *data = pmq_next_message(&client->incoming);
	if (!author)
		return NULL;
	assert(data);
	ret->author = strdup(author);
	if (!ret->author)
		abort();
#else
	const char *data = pmq_next_message(&client->incoming);
	if (!data)
		return NULL;
#endif
	ret->data = strdup(data);
	if (!ret->data)
		abort();
	return ret;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;
	pmq_put(&client->outgoing, msg, msg_size);
	return 0;
}

int
chat_client_get_events(const struct chat_client *client)
{
	if (client->socket < 0)
		return 0;

	// Abusing partial_message_queue implementation

	if (client->outgoing.read[0]) {
		// There is data to send
		return CHAT_EVENT_INPUT | CHAT_EVENT_OUTPUT;
	}
	return CHAT_EVENT_INPUT;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	struct pollfd fd = {.fd = client->socket,
		.events = POLLIN | (chat_client_get_events(client) & CHAT_EVENT_OUTPUT ? POLLOUT : 0)
	};
	int res = poll(&fd, 1, timeout * 1000);
	if (res < 0)
		return CHAT_ERR_SYS;
	else if (res == 0)
		return CHAT_ERR_TIMEOUT;
	else {
		// Note: the input processing should preceed output to avoid SIGPIPE

		if (fd.revents & POLLIN) {
			const size_t bufsz = 1024;
			char buf[bufsz];
			ssize_t got;
			while ((got = recv(client->socket, buf, bufsz, 0)) > 0) {
				pmq_put(&client->incoming, buf, got);
			}
			if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
					return CHAT_ERR_SYS;
			}
		}

		if (fd.revents & POLLOUT) {
			// Abusing partial_message_queue implementation

			ssize_t sent = 1;
			while (*client->outgoing.read && sent > 0) {
				sent = send(client->socket, client->outgoing.read, strlen(client->outgoing.read), 0);
				if (sent < 0)
					return CHAT_ERR_SYS;
				client->outgoing.read += sent;
			}
			if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				return CHAT_ERR_SYS;
			}
		}
	}
	return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}
