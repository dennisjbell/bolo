/*
  Copyright 2015 James Hunt <james@jameshunt.us>
  Copyright 2015  Dan Molik <dan@danmolik.com>

  This file is part of Bolo.

  Bolo is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.

  Bolo is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along
  with Bolo.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <vigor.h>

#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <bolo.h>

#define DEFAULT_AGENT_FILE       "/etc/dbolo.conf"
#define BOLO_EPOLL_MAXFD         8192
#define AGENT_TICK_MS              50
#define AGENT_SOCKET_LIFETIME_MS 7000

static struct {
	char *endpoint;
	char *commands;

	int   verbose;
	int   daemonize;

	float splay;

	char *pidfile;
	char *user;
	char *group;

	char     *beacon;
	int       reconnects;
	uint64_t  timeout;
} OPTIONS = { 0 };

typedef struct {
	void    *zmq;
	void    *zocket;
	char    *endpoint;

	int      lifetime;
	int64_t  expires;
} socket_t;

typedef struct {
	void     *zmq;
	void     *zocket;
	char     *endpoint;

	int       lifetime;
	int       reconnects;

	int       epfd;
	int       bfd;

	uint64_t  expires;
	hb_t     *hb;
	struct    epoll_event *ev;
} bsocket_t;

typedef struct {
	list_t     l;
	int        interval;
	char      *exec;

	uint64_t   next_run;
	pid_t      pid;
	int        fd;           /* hooked up to stdout */

	size_t     nread;
	char       buffer[8192];
} command_t;

static socket_t* s_socket(void *zmq, const char *endpoint, int lifetime)
{
	socket_t *s = vmalloc(sizeof(socket_t));
	s->zmq      = zmq;
	s->zocket   = NULL;
	s->endpoint = strdup(endpoint);
	s->lifetime = lifetime;
	s->expires  = 0; /* already expired */
	return s;
}

static int s_reconnect(socket_t *s)
{
	int64_t now = time_ms();
	if (s->expires > now)
		return 0;

	if (s->zocket)
		zmq_close(s->zocket);

	if (s->expires)
		logger(LOG_INFO, "connection expired; reconnecting to %s", s->endpoint);

	s->zocket = zmq_socket(s->zmq, ZMQ_PUSH);
	zmq_connect(s->zocket, s->endpoint);
	s->expires = now + s->lifetime;
	return 0;
}

/* BEACON Functions {{{ */

static bsocket_t* beacon_socket(void *zmq, const char *endpoint, int lifetime, int epfd, struct epoll_event *ev) /* {{{ */
{
	bsocket_t *s   = vmalloc(sizeof(bsocket_t));
	s->zmq         = zmq;
	s->endpoint    = strdup(endpoint);
	s->lifetime    = lifetime;
	s->expires     = time_ms();
	s->reconnects  = 0;
	s->hb          = vmalloc(sizeof(hb_t));
	s->epfd        = epfd;
	s->ev          = ev;
	return s;
} /* }}} */

static int beacon_connect(bsocket_t *s) /* {{{ */
{
	s->zocket = zmq_socket(s->zmq, ZMQ_SUB);
	if (zmq_connect(s->zocket, s->endpoint) != 0) {
		logger(LOG_ERR, "unable to connect to beacon endpoint (%s): %s", OPTIONS.beacon, strerror(errno));
		return 1;
	}
	if (zmq_setsockopt(s->zocket, ZMQ_SUBSCRIBE, NULL, 0)) {
		logger(LOG_ERR, "unable to set null beacon(%s) filter", OPTIONS.beacon);
		return 1;
	}
	int beacon_fd;
	size_t fd_len = sizeof(beacon_fd);
		if (zmq_getsockopt(s->zocket, ZMQ_FD, &beacon_fd, &fd_len) != 0) {
		logger(LOG_ERR, "unable to get beacon fd: %s", strerror(errno));
		return 1;
	}
	s->bfd = beacon_fd;
	s->ev->data.fd = beacon_fd;
	s->ev->events  = EPOLLIN | EPOLLET;
	if (epoll_ctl(s->epfd, EPOLL_CTL_ADD, beacon_fd, s->ev) != 0) {
		logger(LOG_ERR, "epoll_ctl beacon_fd failed: %s", strerror(errno));
		return 2;
	}
	return 0;
} /* }}} */

static void beacon_close(bsocket_t *s) /* {{{ */
{
	if (s->zocket)
		zmq_close(s->zocket);
	s->ev->data.fd = s->bfd;
	s->ev->events  = EPOLLIN | EPOLLET;
	if (epoll_ctl(s->epfd, EPOLL_CTL_DEL, s->bfd, s->ev) != 0)
		logger(LOG_ERR, "epoll_ctl beacon_fd failed: %s", strerror(errno));
} /* }}} */

static void beacon_timer(bsocket_t *s, socket_t *ps) /* {{{ */
{
	int64_t now = time_ms();
	if (now > s->expires) {
		s->expires += s->hb->interval;
		if (hb_miss(s->hb) == 0) {
			if ( s->reconnects < OPTIONS.reconnects) {
				s->reconnects += 2;
				s_reconnect(ps);
				int rc = 0;
				int tries = 3;
				do {
					beacon_close(s);
					rc = beacon_connect(s);
					tries--;
				} while (rc > 0 && tries);
			} else {
				/* preform some sort of backoff */
				sleep(15 *60);
				s->reconnects = 0;
			}
		}
	}
} /* }}} */

static void beacon_ping(bsocket_t *s) /* {{{ */
{
	unsigned int zmq_events;
	size_t       zmq_events_size = sizeof(zmq_events);
	uint64_t     now             = time_ms();

	zmq_getsockopt(s->zocket, ZMQ_EVENTS, &zmq_events, &zmq_events_size);
	while (zmq_events & ZMQ_POLLIN) {
		logger(LOG_DEBUG, "recieved beacon from %s", OPTIONS.beacon, now);
		pdu_t *pdu = pdu_recv(s->zocket);
		if(strcmp(pdu_type(pdu), "BEACON") == 0) {
			char *st;
			st = pdu_string(pdu, 1); uint64_t       ts = strtoul(st, NULL, 10); free(st);
			st = pdu_string(pdu, 2); uint32_t interval = strtol (st, NULL, 10); free(st);
			/* latency modeled as:
			 *   Now + Travel Time (Difference) / Now
			 *   Now + ( Now - Sendtime ) / Now
			 *   2 - (Sendtime / Now)
			 */
			float latency   = 2 - ts / now ;
			s->hb->interval = interval;
			s->hb->backoff  = latency;
			s->expires      = latency * (interval + time_ms());
			if (s->reconnects > 0)
				s->reconnects--;
			hb_ping(s->hb);
		} else {
			logger(LOG_ERR, "unsupported pdu type (%s) recieved from beacon (%s)", pdu_type(pdu), OPTIONS.beacon);
		}
		pdu_free(pdu);
		zmq_getsockopt(s->zocket, ZMQ_EVENTS, &zmq_events, &zmq_events_size);
	}
} /* }}} */

/* }}} */

static int s_parse(list_t *list, const char *file)
{
	/* line format:
	   # comment
	   @60s /command/to/run every 60s
	   @1h  /path/to/hourly/command
	 */

	FILE *io = fopen(file, "r");
	if (!io) {
		logger(LOG_ERR, "failed to open commands file %s for reading: %s",
			file, strerror(errno));
		return 1;
	}

	int n = 0;
	char line[8192];
	while (fgets(line, 8192, io) != NULL) {
		n++;
		char *nl = strrchr(line, '\n');
		if (!nl) {
			logger(LOG_WARNING, "%s:%i too long; skipping", file, n);
			continue;
		}

		*nl = '\0';

		char *a = line;
		while (isspace(*a)) a++;

		if (!*a || *a == '#')
			continue;

		if (*a == '@') {
			a++;
			int x = 0;
			while (isdigit(*a)) {
				x = x * 10 + (*a - '0');
				a++;
			}

			if (x == 0) {
				logger(LOG_WARNING, "%s:%i has illegal time spec; skipping", file, n);
				continue;
			}

			switch (*a) {
			case 'h': x *= 3600; break;
			case 'm': x *= 60;   break;
			case 's':            break;
			default:
				logger(LOG_WARNING, "%s:%i has illegal time unit '%c'; skipping", file, n, *a);
				continue;
			}
			a++;

			if (x < 0) {
				logger(LOG_WARNING, "%s:%i time overflow detected; skipping", file, n);
				continue;
			}

			if (!isspace(*a)) {
				logger(LOG_WARNING, "%s:%i malformed; skipping", file, n);
				continue;
			}
			while (isspace(*a)) a++;

			int offset = rand() * OPTIONS.splay * x / RAND_MAX;
			command_t *cmd = vmalloc(sizeof(command_t));
			cmd->next_run =  time_s() + offset;
			cmd->pid      = -1;
			cmd->fd       = -1;
			cmd->nread    =  0;

			cmd->interval = x * 1000;
			cmd->exec = strdup(a);
			list_push(list, &cmd->l);

			logger(LOG_DEBUG, "check scheduled @%i (+%is / splay %0.2f) to run every %is: `%s`",
				cmd->next_run, offset, OPTIONS.splay, cmd->interval / 1000, cmd->exec);
			continue;
		}

		logger(LOG_WARNING, "%s:%i malformed; skipping", file, n);
	}

	fclose(io);
	return 0;
}

static void s_sink(socket_t *sock, command_t *cmd)
{
	ssize_t nread;
	while ((nread = read(cmd->fd, cmd->buffer + cmd->nread, 8191 - cmd->nread)) > 0) {
		logger(LOG_INFO, "read %i bytes", nread);
		cmd->nread += nread;
		cmd->buffer[cmd->nread] = '\0';

		for (;;) {
			char *nl = strchr(cmd->buffer, '\n');
			if (!nl) break;

			*nl++ = '\0';
			pdu_t *pdu = stream_pdu(cmd->buffer);
			if (pdu) {
				logger(LOG_INFO, "sending [%s] to %s", cmd->buffer, sock->endpoint);
				pdu_send_and_free(pdu, sock->zocket);
			}

			memmove(cmd->buffer, nl, 8192 - (nl - cmd->buffer));
			cmd->nread -= (nl - cmd->buffer);
		}
	}
	if (nread == -1 && errno != EAGAIN) {
		logger(LOG_WARNING, "read failed [child %i]: %s", cmd->pid, strerror(errno));
	}
}

int main(int argc, char **argv)
{
	OPTIONS.verbose    = 0;
	OPTIONS.endpoint   = strdup("tcp://127.0.0.1:2999");
	OPTIONS.commands   = strdup(DEFAULT_AGENT_FILE);
	OPTIONS.splay      = 0.5;
	OPTIONS.daemonize  = 1;
	OPTIONS.pidfile    = strdup("/var/run/dbolo.pid");
	OPTIONS.user       = strdup("root");
	OPTIONS.group      = strdup("root");
	OPTIONS.beacon     = NULL;
	OPTIONS.timeout    = 300000;
	OPTIONS.reconnects = 8;

	char *a; /* for splay */

	struct option long_opts[] = {
		{ "help",             no_argument, NULL, 'h' },
		{ "verbose",          no_argument, NULL, 'v' },
		{ "quiet",            no_argument, NULL, 'q' },
		{ "endpoint",   required_argument, NULL, 'e' },
		{ "foreground",       no_argument, NULL, 'F' },
		{ "commands",   required_argument, NULL, 'c' },
		{ "splay",      required_argument, NULL, 's' },
		{ "pidfile",    required_argument, NULL, 'p' },
		{ "user",       required_argument, NULL, 'u' },
		{ "group",      required_argument, NULL, 'g' },
		{ "beacon",     required_argument, NULL, 'b' },
		{ "reconnects", required_argument, NULL, 'r' },
		{ "timeout",    required_argument, NULL, 't' },
		{ 0, 0, 0, 0 },
	};
	for (;;) {
		int idx = 1;
		int c = getopt_long(argc, argv, "h?v+qe:Fc:s:p:u:g:b:r:t:", long_opts, &idx);
		if (c == -1) break;

		switch (c) {
		case 'h':
		case '?':
			printf("%s v%s\n", argv[0], BOLO_VERSION);
			printf("Usage: %s [-h?FVv] [-e tcp://host:port]\n"
			       "          [-s splay] [-c /path/to/commands]\n"
			       "          [-b tcp://host:port] [-r reconnects] [-t timeout]\n"
			       "          [-u user] [-g group] [-p /path/to/pidfile]\n\n",
			         argv[0]);

			printf("Options:\n");
			printf("  -?, -h               show this help screen\n");
			printf("  -F, --foreground     don't daemonize, stay in the foreground\n");
			printf("  -q, --quiet          \n");
			printf("  -v, --verbose        turn on debugging, to standard error\n");
			printf("  -e, --endpoint       bolo listener endpoint to connect to\n");
			printf("  -c, --commands       file path containing the commands to run\n");
			printf("  -s, --splay          randomization factor for scheduling commands\n");

			printf("  -b, --beacon         the beacon endpoint to subscribe to\n");
			printf("  -r, --reconnects     the maximum number of beacon beacons \n");
			printf("  -t, --timeout        the maximum wait time between beacons\n");

			printf("  -u, --user           user to run as (if daemonized)\n");
			printf("  -g, --group          group to run as (if daemonized)\n");
			printf("  -p, --pidfile        where to store the pidfile (if daemonized)\n");
			exit(0);

		case 'v':
			OPTIONS.verbose++;
			break;

		case 'q':
			OPTIONS.verbose = 0;
			break;

		case 'e':
			free(OPTIONS.endpoint);
			OPTIONS.endpoint = strdup(optarg);
			break;

		case 'F':
			OPTIONS.daemonize = 0;
			break;

		case 'c':
			free(OPTIONS.commands);
			OPTIONS.commands = strdup(optarg);
			break;

		case 's':
			OPTIONS.splay = strtof(optarg, &a);
			if (a && *a) {
				fprintf(stderr, "Invalid --splay value (%s)\n", optarg);
				exit(1);
			}
			break;

		case 'p':
			free(OPTIONS.pidfile);
			OPTIONS.pidfile = strdup(optarg);
			break;

		case 'u':
			free(OPTIONS.user);
			OPTIONS.user = strdup(optarg);
			break;

		case 'g':
			free(OPTIONS.group);
			OPTIONS.group = strdup(optarg);
			break;

		case 'b':
			free(OPTIONS.beacon);
			OPTIONS.beacon = strdup(optarg);
			break;

		case 'r':
			OPTIONS.reconnects = 2 * atoi(optarg);
			break;

		case 't':
			OPTIONS.timeout = atoi(optarg);
			break;

		default:
			fprintf(stderr, "unhandled option flag %#02x\n", c);
			return 1;
		}
	}

	seed_randomness();

	if (OPTIONS.daemonize) {
		log_open("dbolo", "daemon");
		log_level(LOG_ERR + OPTIONS.verbose, NULL);

		mode_t um = umask(0);
		if (daemonize(OPTIONS.pidfile, OPTIONS.user, OPTIONS.group) != 0) {
			fprintf(stderr, "daemonization failed: (%i) %s\n", errno, strerror(errno));
			return 3;
		}
		umask(um);
	} else {
		log_open("dbolo", "console");
		log_level(LOG_ERR + OPTIONS.verbose, NULL);
		if (!freopen("/dev/null", "r", stdin))
			logger(LOG_WARNING, "failed to reopen stdin </dev/null: %s", strerror(errno));
	}
	logger(LOG_NOTICE, "starting up");

	logger(LOG_DEBUG, "allocating 0MQ context");
	void *zmq = zmq_ctx_new();
	if (!zmq) {
		logger(LOG_ERR, "failed to initialize 0MQ context");
		return 3;
	}
	socket_t *sock = s_socket(zmq, OPTIONS.endpoint, AGENT_SOCKET_LIFETIME_MS);
	s_reconnect(sock);

	LIST(COMMANDS);
	if (s_parse(&COMMANDS, OPTIONS.commands) != 0)
		return 1;

	int epfd = epoll_create(42);
	if (epfd < 0) {
		logger(LOG_ERR, "failed to get an epoll fd");
		return 1;
	}

	struct epoll_event ev, events[BOLO_EPOLL_MAXFD];
	memset(&ev, 0, sizeof(ev));

	bsocket_t *bsock = vmalloc(sizeof(bsocket_t));
	if (OPTIONS.beacon) {
		bsock = beacon_socket(zmq, OPTIONS.beacon, 60000, epfd, &ev);
		if (hb_init(bsock->hb, OPTIONS.timeout / 5, 5, 1.1, OPTIONS.timeout))
			logger(LOG_INFO, "initializing beacon(%s) hb_t", OPTIONS.beacon);
		beacon_connect(bsock);
	} else
		logger(LOG_INFO, "disabling beaconing, no beacon endpoint given");

	int i, n = 0;
	for (;;) {
		uint64_t now = time_ms();
		now = now - now % 1000;

		command_t *cmd;
		pid_t pid;
		int status;

		if (OPTIONS.beacon)
			beacon_timer(bsock, sock);

		for (i = 0; i < n; i++) {
			if (events[i].events & EPOLLIN) {
				if (OPTIONS.beacon && events[i].data.fd == bsock->bfd) {
					beacon_ping(bsock);
					continue;
				}
				for_each_object(cmd, &COMMANDS, l) {
					if (events[i].data.fd != cmd->fd)
						continue;

					logger(LOG_INFO, "checking child %u (fd=%x)", cmd->pid, cmd->fd);
					s_sink(sock, cmd);
					break;
				}
			}
		}

		for_each_object(cmd, &COMMANDS, l) {
			if (cmd->pid > 0) {
				if ((pid = waitpid(cmd->pid, &status, WNOHANG)) == cmd->pid) {
					logger(LOG_INFO, "child %i exited %02x, next run at %lu", cmd->pid, status, cmd->next_run);

					epoll_ctl(epfd, EPOLL_CTL_DEL, cmd->fd, &ev);
					s_sink(sock, cmd);

					close(cmd->fd);
					cmd->pid = -1;
					cmd->fd  = -1;
				}

			} else if (cmd->next_run < now) {
				int pfd[2];
				if (pipe(pfd) != 0) {
					logger(LOG_ERR, "failed to run collector; pipe(): %s", strerror(errno));
					continue;
				}

				pid = fork();
				if (pid == 0) {
					dup2(pfd[1], 1);
					close(pfd[0]); /* stdin */
					execl("/bin/sh", "sh", "-c", cmd->exec, NULL);
					return 99;
				}

				close(pfd[1]); /* 'stdout' */
				cmd->fd = pfd[0];

				int fl = fcntl(cmd->fd, F_GETFL);
				fcntl(cmd->fd, F_SETFL, fl|O_NONBLOCK);

				ev.data.fd = cmd->fd;
				ev.events = EPOLLIN | EPOLLET;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, cmd->fd, &ev) != 0) {
					logger(LOG_ERR, "epoll_ctl failed: %s", strerror(errno));
				}

				cmd->nread = 0;
				cmd->pid = pid;
				cmd->next_run = now + cmd->interval;
				logger(LOG_INFO, "child %i executing `%s`", cmd->pid, cmd->exec);
			}
		}
		n = epoll_wait(epfd, events, BOLO_EPOLL_MAXFD, AGENT_TICK_MS);
	}
	return 0;
}
