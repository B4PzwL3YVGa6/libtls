/*
 * Copyright (c) 2018 Bob Beck <beck@obtuse.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * A relatively simple buffering echo server that uses poll(2),
 * for instructional purposes.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tls.h>
#include <unistd.h>

#define MAX_CONNECTIONS 256
#define BUFLEN 4096

static int debug = 0;

static void usage()
{
	extern char * __progname;
	fprintf(stderr, "usage: %s host portnumber\n", __progname);
	exit(1);
}

#define STATE_READING 0
#define STATE_WRITING 1

struct client {
	int state;
	unsigned char *readptr, *writeptr, *nextptr;
	unsigned char buf[BUFLEN];
	struct tls *ctx;
};

static struct client clients[MAX_CONNECTIONS];
static struct pollfd pollfds[MAX_CONNECTIONS];
static int throttle = 0;

static void
client_init(struct client *client, struct tls *ctx)
{
	client->readptr = client->writeptr = client->nextptr = client->buf;
	client->state = STATE_READING;
	client->ctx = ctx;
}

static ssize_t
client_consume(struct client *client, size_t len)
{
	size_t n = 0;

	while (n < len) {
		if (client->readptr == client->nextptr)
			break;
		client->readptr++;
		n++;
	}

	if (debug && n > 0)
                fprintf(stderr, "client_consume: %ld bytes from buffer\n", n);

        return ((ssize_t)n);
}

static ssize_t
client_get(struct client * client, unsigned char *outbuf, size_t outlen)
{
        unsigned char *nextptr = client->readptr;
        size_t n = 0;

        while (n < outlen) {
                if (nextptr == client->writeptr)
                        break;
                *outbuf++ = *nextptr++;
                if ((size_t)(nextptr - client->buf) >= sizeof(client->buf))
                        nextptr = client->buf;
                client->nextptr = nextptr;
                n++;
        }

        if (debug && n > 0)
                fprintf(stderr, "client_get: got %ld bytes from buffer\n", n);

        return ((ssize_t)n);
}

static ssize_t
client_put(struct client *client, const unsigned char *inbuf, size_t inlen)
{
        unsigned char *nextptr = client->writeptr;
        unsigned char *prevptr;
        size_t n = 0;

        while (n < inlen) {
                prevptr = nextptr++;
                if ((size_t)(nextptr - client->buf) >= sizeof(client->buf))
                        nextptr = client->buf;
                if (nextptr == client->readptr)
                        break;
                *prevptr = *inbuf++;
                client->writeptr = nextptr;
                n++;
        }

        if (debug && n > 0)
                fprintf(stderr, "client_put: put %ld bytes into buffer\n", n);

        return ((ssize_t)n);
}

static void
closeconn (struct pollfd *pfd, struct client *client)
{
	int i;

	do {
		i = tls_close(client->ctx);
	} while (i == TLS_WANT_POLLIN || i == TLS_WANT_POLLOUT);
	tls_free(client->ctx);

	close(pfd->fd);
	pfd->fd = -1;
	pfd->revents = 0;
	throttle = 0;
}

static void
newconn(struct pollfd *pfd, int newfd) {
	int sflags;
	if ((sflags = fcntl(newfd, F_GETFL)) < 0)
		err(1, "fcntl failed");
	sflags |= O_NONBLOCK;
	if (fcntl(newfd, F_SETFL, sflags) < 0)
		err(1, "fcntl failed");
	pfd->fd = newfd;
	pfd->events = POLLIN | POLLHUP;
	pfd->revents = 0;
}

static void
handle_client(struct pollfd *pfd, struct client *client)
{
	if ((pfd->revents & (POLLERR | POLLNVAL)))
		errx(1, "bad fd %d", pfd->fd);
	if (pfd->revents & POLLHUP) {
		closeconn(pfd, client);
	}
	else if (pfd->revents & pfd->events) {
		char buf[BUFLEN];
		ssize_t len = 0;
		if (client->state == STATE_READING) {
			len = tls_read(client->ctx, buf, sizeof(buf));
			if (len == TLS_WANT_POLLIN)
				pfd->events = POLLIN | POLLHUP;
			else if (len == TLS_WANT_POLLOUT)
				pfd->events = POLLOUT | POLLHUP;
			else if (len < 0)
				warn("tls_read: %s", tls_error(client->ctx));
			else if (len == 0)
				closeconn(pfd, client);
			else {
				if (client_put(client, buf, len) != len) {
					warnx("client buffer failed");
					closeconn(pfd, client);
				} else {
					client->state=STATE_WRITING;
					pfd->events = POLLOUT | POLLHUP;
				}
			}
		} else if (client->state == STATE_WRITING) {
			ssize_t ret = 0;
			len = client_get(client, buf, sizeof(buf));
			if (len) {
				ret = tls_write(client->ctx, buf, len);
				if (ret == TLS_WANT_POLLIN)
					pfd->events = POLLIN | POLLHUP;
				else if (ret == TLS_WANT_POLLOUT)
					pfd->events = POLLOUT | POLLHUP;
				else if (ret < 0)
					warn("tls_write: %s", tls_error(client->ctx));
				else
					client_consume(client, ret);
			}
			if (ret == len) {
				client->state = STATE_READING;
				pfd->events = POLLIN | POLLHUP;
			}
		}
	}
}

int main(int argc, char **argv) {
        struct tls_config *tls_cfg = NULL;
        struct tls *tls_ctx = NULL;
        struct tls *tls_cctx = NULL;
	struct addrinfo hints, *res;
	int i, listenfd, error;


	if (argc != 3)
		usage();

        /* now set up TLS */

        if ((tls_cfg = tls_config_new()) == NULL)
                errx(1, "unable to allocate TLS config");
        if (tls_config_set_ca_file(tls_cfg, "../CA/root.pem") == -1)
                errx(1, "unable to set root CA filet");
        if (tls_config_set_cert_file(tls_cfg, "../CA/server.crt") == -1)
                errx(1, "unable to set TLS certificate file");
        if (tls_config_set_key_file(tls_cfg, "../CA/server.key") == -1)
                errx(1, "unable to set TLS key file");
        if ((tls_ctx = tls_server()) == NULL)
                errx(1, "tls server creation failed");
        if (tls_configure(tls_ctx, tls_cfg) == -1)
                errx(1, "tls configuration failed (%s)", tls_error(tls_ctx));

	bzero(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if ((error = getaddrinfo(argv[1], argv[2], &hints, &res))) {
		fprintf(stderr, "%s\n", gai_strerror(error));
		usage();
	}

	for (i = 0; i < MAX_CONNECTIONS; i++)  {
		pollfds[i].fd = -1;
		pollfds[i].events = POLLIN | POLLHUP;
		pollfds[i].revents = 0;
	}

	if ((listenfd = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol)) < 0)
		err(1, "Couldn't get listen socket");

	if (bind(listenfd, res->ai_addr, res->ai_addrlen) == -1)
		err(1, "bind failed");

	if (listen(listenfd, MAX_CONNECTIONS) == -1)
		err(1, "listen failed");

	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(int)) == -1)
		err(1, "SO_REUSEADDR setsockopt failed");

	newconn(&pollfds[0], listenfd);

	while(1) {
		if (!throttle)
			pollfds[0].events = POLLIN | POLLHUP;
		else
			pollfds[0].events = 0;

		if (poll(pollfds, MAX_CONNECTIONS, -1) == -1)
			err(1, "poll failed");
		if (pollfds[0].revents) {
			struct sockaddr csaddr;
			socklen_t cssize;
			int fd;

			fd = accept(pollfds[0].fd, &csaddr, &cssize);
			throttle = 1;
			for (i = 1; fd >= 0 && i < MAX_CONNECTIONS; i++)  {
				if (pollfds[i].fd == -1) {
					throttle = 0;
					if (tls_accept_socket(tls_ctx,
						&tls_cctx, fd) == -1) {
						warnx("tls accept failed (%s)",
						    tls_error(tls_ctx));
						close(fd);
						break;
					}
					newconn(&pollfds[i], fd);
					client_init(&clients[i], tls_cctx);
					break;
				}
			}
		}
		for (i = 1; i < MAX_CONNECTIONS; i++)
			handle_client(&pollfds[i], &clients[i]);
	}

	freeaddrinfo(res);
	return 0;
}
