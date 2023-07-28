#include "chat.h"
#include "chat_server.h"
#include "partial_message_queue.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <stdbool.h>

struct chat_peer {
	/** Client's socket. To read/write messages. */
	int socket;
	/// Outgoing message queue
	struct partial_message_queue outgoing;
	/// Incoming message queue
	struct partial_message_queue incoming;

	struct chat_peer *prev, *next;
};

struct chat_peer *chat_peer_new(int socket, struct chat_peer *next) {
	struct chat_peer *ret = malloc(sizeof *ret);
	if (!ret)
		abort();
	ret->socket = socket;
	pmq_init(&ret->outgoing, 16);
	pmq_init(&ret->incoming, 16);
	ret->prev = NULL;
	ret->next = next;
	next ? next->prev = ret : 0;
	return ret;
}

struct chat_peer *chat_peer_delete(struct chat_peer *peer) {
	(void)close(peer->socket);
	pmq_destroy(&peer->outgoing);
	pmq_destroy(&peer->incoming);
	struct chat_peer *prev = peer->prev, *next = peer->next;
	if (prev)
		prev->next = next;
	if (next)
		next->prev = prev;
	free(peer);
	return next;
}

struct chat_server {
	/** Listening socket. To accept new clients. */
	int socket;
	/// epoll descriptor
	int epoll_fd;
	/// Peers array
	struct chat_peer *peers;

	/// Number of peers that have something to send
	size_t pending_output_peers;

	/// Queue of received messages
	struct partial_message_queue received;
};

struct chat_server *
chat_server_new(void)
{
	struct chat_server *server = calloc(1, sizeof(*server));
	server->socket = -1;
	server->epoll_fd = -1;
	server->peers = NULL;

	server->pending_output_peers = 0;

	pmq_init(&server->received, 16);

	return server;
}

void
chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
		close(server->socket);
	if (server->epoll_fd >= 0)
		close(server->epoll_fd);
	pmq_destroy(&server->received);

	while (server->peers)
		server->peers = chat_peer_delete(server->peers);

	free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (server->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	/*
	 * 1) Create a server socket (function socket()).
	 * 2) Bind the server socket to addr (function bind()).
	 * 3) Listen the server socket (function listen()).
	 * 4) Create epoll/kqueue if needed.
	 */

	server->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (0 > server->socket) {
		return CHAT_ERR_SYS;
	}
	if (0 > fcntl(server->socket, F_SETFL, O_NONBLOCK)) {
	    (void)close(server->socket);
	    server->socket = -1;
	    return CHAT_ERR_SYS;
	}
	(void)setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof (int)); /* If fails, ok */

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_port = htons(port);
	/* Listen on all IPs of this machine. */
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (0 > bind(server->socket, (struct sockaddr *)&addr, sizeof addr)) {
		int save_errno = errno;
		(void)close(server->socket);
		server->socket = -1;
		errno = save_errno;
		if (errno == EADDRINUSE)
			return CHAT_ERR_PORT_BUSY;
	}
	if (0 > listen(server->socket, 100) ||
			0 > (server->epoll_fd = epoll_create(321))) {
		int save_errno = errno;
		(void)close(server->socket);
		server->socket = -1;
		errno = save_errno;
		return CHAT_ERR_SYS;
	}

	if (0 > epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->socket,
				&(struct epoll_event){.events = EPOLLIN, .data.ptr = NULL})) {
		int save_errno = errno;
		(void)close(server->epoll_fd);
		(void)close(server->socket);
		server->socket = server->epoll_fd = -1;
		errno = save_errno;
		return CHAT_ERR_SYS;
	}

	return 0;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */
#endif
	(void)server;
	(void)msg;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
}

int
chat_server_update(struct chat_server *server, double timeout)
{
	if (server->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	/*
	 * 1) Wait on epoll/kqueue/poll for update on any socket.
	 * 2) Handle the update.
	 * 2.1) If the update was on listen-socket, then you probably need to
	 *     call accept() on it - a new client wants to join.
	 * 2.2) If the update was on a client-socket, then you might want to
	 *     read/write on it.
	 */

	const size_t events_cnt = 10;
	struct epoll_event events[events_cnt];
	int res = epoll_wait(server->epoll_fd, events, events_cnt, timeout * 1000);
	if (0 > res)
		return CHAT_ERR_SYS;
	else if (0 == res)
		return CHAT_ERR_TIMEOUT;
	else {
		for (int i = 0; i < res; ++i) {
			if (!events[i].data.ptr) {
				// Server passive socket
				int sock;
				while (0 < (sock = accept(server->socket, NULL, NULL))) {
				    if (0 > fcntl(sock, F_SETFL, O_NONBLOCK)) {
					    (void)close(sock);
					    return CHAT_ERR_SYS;
				    }
				    server->peers = chat_peer_new(sock, server->peers);
				    if (0 > epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, sock,
						      &(struct epoll_event){.events = EPOLLIN, .data.ptr = server->peers})) {
					    // Failed to add to epoll...
					    int save_errno = errno;
					    server->peers = chat_peer_delete(server->peers);
					    (void)close(sock);
					    errno = save_errno;
					    return CHAT_ERR_SYS;
				    }
				}
				if (sock < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
					return CHAT_ERR_SYS;
			} else {
				// Peer
				struct chat_peer *peer = events[i].data.ptr;
				if (events[i].events & EPOLLIN) {
					const size_t bufsz = 1024;
					char buf[bufsz];

					ssize_t got;
					while ((got = recv(peer->socket, buf, bufsz, 0)) > 0) {
						pmq_put(&peer->incoming, buf, got);
					}
					if (got == 0) {
						// Disconnected...

						if (*peer->outgoing.read)
							--server->pending_output_peers;

						int err = epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, peer->socket, NULL);
						if (err)
							return CHAT_ERR_SYS;

						if (server->peers == peer)
							server->peers = chat_peer_delete(peer);
						else
							chat_peer_delete(peer);

						continue;
					} else if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
						return CHAT_ERR_SYS;
					} else {
						// Successful `recv`. Process the received data
						char *msg;
						while ((msg = pmq_next_message(&peer->incoming))) {
							size_t len = strlen(msg);
							msg[len++] = '\n';  // '\0' -> '\n'
							pmq_put(&server->received, msg, len);
							for (struct chat_peer *other = server->peers; other; other = other->next) {
								if (other == peer)
									continue;

								if (!*other->outgoing.read) {
									++server->pending_output_peers;
									int err = epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD,
										other->socket, &(struct epoll_event){
											.events = EPOLLIN | EPOLLOUT,
											.data.ptr = other
										});
									if (err)
									    return CHAT_ERR_SYS;
								}
#ifdef NEED_AUTHOR
								pmq_put(&other->outgoing, peer->author, author_len);
#endif
								pmq_put(&other->outgoing, msg, len);
							}
						}
					}
				}
				if (events[i].events & EPOLLOUT) {
					// Abusing partial_message_queue implementation
					ssize_t sent = 1;
					while (*peer->outgoing.read && sent > 0) {
						sent = send(peer->socket, peer->outgoing.read, strlen(peer->outgoing.read), 0);
						peer->outgoing.read += sent;
					}
					if (!*peer->outgoing.read) {
						--server->pending_output_peers;
						int err = epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD,
							peer->socket, &(struct epoll_event){
								.events = EPOLLIN,
								.data.ptr = peer
							});
						if (err)
						    return CHAT_ERR_SYS;
					}
					if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
						return CHAT_ERR_SYS;
					}
				}
			}
		}
	}
	return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	char *msg = pmq_next_message(&server->received);
	if (!msg)
		return NULL;
	struct chat_message *ret = NULL;
	if (!(ret = malloc(sizeof *ret)) || !(ret->data = strdup(msg))) {
		free(ret);
		abort();
	}
	return ret;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */

	/*
	 * Server has multiple sockets - own and from connected clients. Hence
	 * you can't return a socket here. But if you are using epoll/kqueue,
	 * then you can return their descriptor. These descriptors can be polled
	 * just like sockets and will return an event when any of their owned
	 * descriptors has any events.
	 *
	 * For example, assume you created an epoll descriptor and added to
	 * there a listen-socket and a few client-sockets. Now if you will call
	 * poll() on the epoll's descriptor, then on return from poll() you can
	 * be sure epoll_wait() can return something useful for some of those
	 * sockets.
	 */
#else
	(void)server;
	return -1;
#endif
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
    	if (server->socket < 0)
	    return 0;
	if (server->pending_output_peers)
		return CHAT_EVENT_INPUT | CHAT_EVENT_OUTPUT;
	return CHAT_EVENT_INPUT;
}
