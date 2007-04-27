
#include <err.h>
#include <errno.h>
#include <netinet/in.h>
#if !defined(__APPLE__)
#include <pty.h>
#else
#include <util.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#if !defined(__APPLE__)
#define USE_POLL
#endif

#ifndef USE_POLL
#include <sys/select.h>
#endif

#include "console.h"
#include "libvnc/libvnc.h"

char vncpasswd[64];
unsigned char challenge[AUTHCHALLENGESIZE];

DisplayState display_state;

struct iohandler {
    int fd;
    void (*fd_read)(void *);
    void (*fd_write)(void *);
    void *opaque;
    int enabled;
#ifdef USE_POLL
    struct pollfd *pollfd;
#endif
    struct iohandler *next;
};

struct iohandler *iohandlers = NULL;
static int nr_handlers = 0;
static int handlers_updated = 1;

int
set_fd_handler(int fd, int (*fd_read_poll)(void *), void (*fd_read)(void *),
	       void (*fd_write)(void *), void *opaque)
{
    struct iohandler **pioh = &iohandlers, *ioh;

    while (*pioh) {
	if ((*pioh)->fd == fd)
	    break;
	pioh = &(*pioh)->next;
    }
    if (fd_read || fd_write) {
	if (*pioh == NULL) {
	    *pioh = calloc(1, sizeof(struct iohandler));
	    if (*pioh == NULL)
		return -1;
	    (*pioh)->fd = fd;
	    nr_handlers++;
	}
	(*pioh)->fd_read = fd_read;
	(*pioh)->fd_write = fd_write;
	(*pioh)->opaque = opaque;
	(*pioh)->enabled = 1;
	handlers_updated = 1;
    } else if (*pioh) {
	nr_handlers--;
	ioh = *pioh;
	*pioh = (*pioh)->next;
	free(ioh);
	handlers_updated = 1;
    }
    return 0;
}

struct timer {
    void (*callback)(void *);
    void *opaque;
    uint64_t timeout;
    struct timer *next;
};

static struct timer *timers = NULL;
static struct timer **timers_tail = &timers;

uint64_t
get_clock(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
	err(1, "gettimeofday");
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void *
init_timer(void (*callback)(void *), void *opaque)
{
    struct timer *t;

    t = calloc(1, sizeof(struct timer));
    if (t == NULL)
	return NULL;
    t->callback = callback;
    t->opaque = opaque;
    t->timeout = UINT64_MAX;
    *timers_tail = t;
    timers_tail = &t->next;

    return t;
}

int
set_timer(void *_t, uint64_t timeout)
{
    struct timer *t = _t;
    struct timer **o = NULL;
    struct timer **n = timers_tail;
    struct timer **c = &timers;

    t->timeout = timeout;
    while (*c && (o == NULL || n == NULL)) {
	if ((*c)->timeout >= timeout)
	    n = c;
	if (*c == t)
	    o = c;
	c = &(*c)->next;
    }
    if (n != o) {
	*o = t->next;
	if (*o == NULL)
	    timers_tail = o;
	t->next = *n;
	*n = t;
	if (t->next == NULL)
	    timers_tail = &t->next;
    }
    return 0;
}

void kbd_put_keycode(int keycode)
{
}

void
hw_update(void *s)
{
    // CharDriverState *console = s;

    // console_select(0);
}

void
hw_invalidate(void *s)
{
    // CharDriverState *console = s;

    console_select(0);
}

struct process {
    int fd;
    CharDriverState *console;
};

void
stdin_to_process(void *opaque)
{
    struct process *p = opaque;
    uint8_t buf[16];
    int count;

    count = read(0, buf, 16);
    if (count > 0)
	write(p->fd, buf, count);
}

void
process_read(void *opaque)
{
    struct process *p = opaque;
    uint8_t buf[16];
    int count;

    count = read(p->fd, buf, 16);
    if (count > 0)
	p->console->chr_write(p->console, buf, count);
}

struct process *
run_process(CharDriverState *console, const char *filename,
	    char *const argv[], char *const envp[])
{
    pid_t pid;
    struct process *p;
    struct winsize ws;

    p = calloc(1, sizeof(struct process));
    if (p == NULL)
	err(1, "malloc");

    p->console = console;

    ws.ws_row = 25;
    ws.ws_col = 80;
    ws.ws_xpixel = ws.ws_col * 8;
    ws.ws_ypixel = ws.ws_row * 16;

    pid = forkpty(&p->fd, NULL, NULL, &ws);
    if (pid < 0)
	err(1, "fork %s\n", filename);
    if (pid == 0) {
	execve(filename, argv, envp);
	perror("execve");
	_exit(1);
    }

    set_fd_handler(p->fd, NULL, process_read, NULL, p);

    console_set_input(console, p->fd, p);

    return p;
}

int
main(int argc, char **argv, char **envp)
{
    DisplayState *ds;
    CharDriverState *console;
    struct process *p;
    struct sockaddr_in sa;
    struct iohandler *ioh, *next;
    struct timer *t;
    char **nenvp;
    int nenv;
    uint64_t now;
    short revents;
    int ret, timeout;
    int nfds = 0;

#ifdef USE_POLL
    struct pollfd *pollfds = NULL;
    int max_pollfds = 0;
#else
    fd_set rdset, wrset, exset, rdset_m, wrset_m, exset_m;
    struct timeval timeout_tv;
#endif

    memset(&sa.sin_addr, 0, sizeof(sa.sin_addr));

    ds = &display_state;
    memset(ds, 0, sizeof(display_state));
    ds->set_fd_handler = set_fd_handler;
    ds->init_timer = init_timer;
    ds->get_clock = get_clock;
    ds->set_timer = set_timer;
    ds->kbd_put_keycode = kbd_put_keycode;
    ds->kbd_put_keysym = kbd_put_keysym;

    vnc_display_init(ds, 0, 1, &sa, "test", NULL);
    console = text_console_init(ds);

#if 0
    {
	char *msg = "Hello World\n\r";
	console->chr_write(console, (uint8_t *)msg, strlen(msg));
    }
#endif

    ds->mouse_opaque = console;
    ds->mouse_is_absolute = mouse_is_absolute;
    ds->mouse_event = mouse_event;

    ds->hw_opaque = console;
    ds->hw_update = hw_update;
    ds->hw_invalidate = hw_invalidate;

    for (nenv = 0; envp[nenv]; nenv++)
	;
    nenvp = malloc(nenv * sizeof(char *));
    if (nenvp == NULL)
	err(1, "malloc");
    for (nenv = 0; envp[nenv]; nenv++)
	if (strncmp(envp[nenv], "TERM=", 5))
	    nenvp[nenv] = envp[nenv];
	else
	    nenvp[nenv] = "TERM=vt100";
    nenvp[nenv] = NULL;

    if (argc == 1) {
	argv = calloc(2, sizeof(char *));
	argv[0] = "/bin/bash";
    } else
	argv++;

    p = run_process(console, argv[0], argv, nenvp);
    // set_fd_handler(0, NULL, stdin_to_process, NULL, p);

    for (;;) {
	if (handlers_updated) {
#ifdef USE_POLL
	    if (nr_handlers > max_pollfds) {
		free(pollfds);
		pollfds = malloc(nr_handlers * sizeof(struct pollfd));
		if (pollfds == NULL)
		    err(1, "malloc");
		max_pollfds = nr_handlers;
	    }
	    nfds = 0;
	    for (ioh = iohandlers; ioh != NULL; ioh = ioh->next) {
		if (!ioh->enabled)
		    continue;
		pollfds[nfds].fd = ioh->fd;
		pollfds[nfds].events = 0;
		if (ioh->fd_read)
		    pollfds[nfds].events |= POLLIN;
		if (ioh->fd_write)
		    pollfds[nfds].events |= POLLOUT;
		ioh->pollfd = &pollfds[nfds];
		nfds++;
	    }
#else
	    FD_ZERO(&rdset_m);
	    FD_ZERO(&wrset_m);
	    FD_ZERO(&exset_m);
	    nfds = 0;
	    for (ioh = iohandlers; ioh != NULL; ioh = ioh->next) {
		if (!ioh->enabled)
		    continue;
		if (ioh->fd_read || ioh->fd_write) {
		    FD_SET(ioh->fd, &exset_m);
		    if (nfds <= ioh->fd)
			nfds = ioh->fd + 1;
		    if (ioh->fd_read)
			FD_SET(ioh->fd, &rdset_m);
		    if (ioh->fd_write)
			FD_SET(ioh->fd, &wrset_m);
		}
	    }
#endif
	    handlers_updated = 0;
	}
	if (timers && timers->timeout != UINT64_MAX) {
	    now = get_clock();
	    if (timers->timeout < now)
		timeout = 0;
	    else if (timers->timeout - now > 60000)
		timeout = 60000;
	    else
		timeout = timers->timeout - now;
	} else
	    timeout = 60000;
	if (timeout) {
#ifdef USE_POLL
	    ret = poll(pollfds, nfds, timeout);
#else
	    FD_COPY(&rdset_m, &rdset);
	    FD_COPY(&wrset_m, &wrset);
	    FD_COPY(&exset_m, &exset);
	    timeout_tv.tv_sec = timeout / 1000;
	    timeout_tv.tv_usec = (timeout % 1000) * 1000;
	    ret = select(nfds, &rdset, &wrset, &exset, &timeout_tv);
#endif
	} else
	    ret = 0;
	if (ret == -1 && errno != EINTR) {
#ifdef USE_POLL
	    err(1, "poll failed");
#else
	    err(1, "select failed");
#endif
	}
	if (ret == 0) {
	    now = get_clock();
	    while (timers && timers->timeout < now) {
		t = timers;
		timers = t->next;
		if (timers == NULL)
		    timers_tail = &timers;
		t->timeout = UINT64_MAX;
		t->next = NULL;
		*timers_tail = t;
		timers_tail = &t->next;
		t->callback(t->opaque);
	    }
	}
	if (ret > 0) {
	    ioh = iohandlers;
	    for (ioh = iohandlers; ioh != NULL; ioh = next) {
		next = ioh->next;
#ifdef USE_POLL
		if (ioh->pollfd == NULL)
		    continue;
		revents = ioh->pollfd->revents;
#else
		revents = 0;
		if (FD_ISSET(ioh->fd, &rdset))
		    revents |= POLLIN;
		if (FD_ISSET(ioh->fd, &wrset))
		    revents |= POLLOUT;
		if (FD_ISSET(ioh->fd, &exset))
		    revents |= POLLERR;
#endif
		if (revents == 0)
		    continue;
		if (revents & (POLLERR|POLLHUP|POLLNVAL)) {
		    ioh->enabled = 0;
		    handlers_updated = 1;
		}
		if (revents & POLLOUT)
		    ioh->fd_write(ioh->opaque);
		if (revents & POLLIN)
		    ioh->fd_read(ioh->opaque);
	    }
	}
    }

    return 0;
}
