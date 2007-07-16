#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#if !defined(__APPLE__)
#include <pty.h>
#else
#include <util.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifndef NXENSTORE
#include <xs.h>
static struct xs_handle *xs;
#endif

#if !defined(__APPLE__)
#define USE_POLL
#endif

#ifndef USE_POLL
#include <sys/select.h>
#endif

#include "console.h"
#include "libvnc/libvnc.h"

#define LINES	24
#define COLS	80

#define FONTH	16
#define FONTW	8

char vncpasswd[64];
unsigned char challenge[AUTHCHALLENGESIZE];

DisplayState display_state;
int do_log;

static int dump_cells = 0;

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
    struct iohandler **pioh = &iohandlers;

    while (*pioh) {
	if ((*pioh)->fd == fd)
	    break;
	pioh = &(*pioh)->next;
    }
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
    (*pioh)->enabled = (fd_read || fd_write);
    handlers_updated = 1;
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
    pid_t pid;
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
    struct process *p;
    struct winsize ws;

    p = calloc(1, sizeof(struct process));
    if (p == NULL)
	err(1, "malloc");

    p->console = console;

    ws.ws_row = LINES;
    ws.ws_col = COLS;
    ws.ws_xpixel = ws.ws_col * FONTW;
    ws.ws_ypixel = ws.ws_row * FONTH;

    p->pid = forkpty(&p->fd, NULL, NULL, &ws);
    if (p->pid < 0)
	err(1, "fork %s\n", filename);
    if (p->pid == 0) {
	execve(filename, argv, envp);
	perror("execve");
	_exit(1);
    }

    set_fd_handler(p->fd, NULL, process_read, NULL, p);

    console_set_input(console, p->fd, p);

    return p;
}

void
end_process(struct process *p)
{
    close(p->fd);
}

static void
handle_sigchld(int signo)
{
    wait(NULL);
    signal(SIGCHLD, handle_sigchld);
}

static void
handle_sigusr1(int signo)
{
    dump_cells = 1;
}

static void
handle_sigusr2(int signo)
{
    do_log = ~do_log;
}

struct pty {
    int fd;
    CharDriverState *console;
};

void
pty_read(void *opaque)
{
    struct pty *pty = opaque;
    uint8_t buf[16];
    int count;

    count = read(pty->fd, buf, 16);
    if (count > 0)
	pty->console->chr_write(pty->console, buf, count);
}

static struct pty *
connect_pty(char *pty_path, CharDriverState *console)
{
    struct pty *pty;

    pty = malloc(sizeof(struct pty));
    if (pty == NULL)
	err(1, "malloc");
    pty->fd = open(pty_path, O_RDWR | O_NOCTTY);
    if (pty->fd == -1)
	err(1, "open");
    pty->console = console;
    set_fd_handler(pty->fd, NULL, pty_read, NULL, pty);
    console_set_input(pty->console, pty->fd, pty);

    return pty;
}

struct vncterm
{
    CharDriverState *console;
    struct process *process;
    struct pty *pty;
    char *xenstore_path;
};

#ifndef NXENSTORE
void
read_xs_watch(void *opaque)
{
    struct vncterm *vncterm = opaque;
    char **vec, *pty_path = NULL;
    unsigned int num;

    vec = xs_read_watch(xs, &num);
    if (vec == NULL)
	return;

    if (strcmp(vncterm->xenstore_path, vec[XS_WATCH_PATH]))
	goto out;

    pty_path = xs_read(xs, XBT_NULL, vncterm->xenstore_path, NULL);
    if (pty_path == NULL)
	goto out;

    vncterm->pty = connect_pty(pty_path, vncterm->console);

    xs_unwatch(xs, vncterm->xenstore_path, "tty");

 out:
    free(pty_path);
    free(vec);
}
#endif

int
main(int argc, char **argv, char **envp)
{
    DisplayState *ds;
    struct vncterm *vncterm;
    struct sockaddr_in sa;
    int display;
    struct iohandler *ioh, *next;
    struct timer *t;
    char **newenvp = NULL;	/* sigh gcc */
    int nenv;
    uint64_t now;
    short revents;
    int ret, timeout;
    int nfds = 0;
    char *pty_path = NULL;
    char *title = "XenServer Virtual Terminal";
#ifndef NXENSTORE
    char *xenstore_path = NULL;
#endif
    int exit_on_eof = 1;
    int restart = 0;
    int restart_needed = 1;
    int cmd_mode = 0;
    int exit_when_all_disconnect = 0;

#ifdef USE_POLL
    struct pollfd *pollfds = NULL;
    int max_pollfds = 0;
#else
    fd_set rdset, wrset, exset, rdset_m, wrset_m, exset_m;
    struct timeval timeout_tv;
#endif

    vncterm = calloc(1, sizeof(struct vncterm));
    if (vncterm == NULL)
	err(1, "malloc");

    while (1) {
	int c;
	static struct option long_options[] = {
	    {"cmd", 0, 0, 'c'},
	    {"pty", 1, 0, 'p'},
	    {"restart", 0, 0, 'r'},
	    {"stay", 0, 0, 's'},
	    {"title", 1, 0, 't'},
	    {"xenstore", 1, 0, 'x'},
	    {0, 0, 0, 0}
	};

	c = getopt_long(argc, argv, "+cp:rst:x:", long_options, NULL);
	if (c == -1)
	    break;

	switch (c) {
	case 'c':
	    cmd_mode = 1;
	    break;
	case 'p':
	    pty_path = strdup(optarg);
	    break;
	case 'r':
	    restart = 1;
	    break;
	case 's':
	    exit_on_eof = 0;
	    break;
	case 't':
	    title = strdup(optarg);
	    break;
	case 'x':
#ifndef NXENSTORE
	    xenstore_path = strdup(optarg);
#endif
	    break;
	}
    }

    ds = &display_state;
    memset(ds, 0, sizeof(display_state));
    ds->set_fd_handler = set_fd_handler;
    ds->init_timer = init_timer;
    ds->get_clock = get_clock;
    ds->set_timer = set_timer;
    ds->kbd_put_keycode = kbd_put_keycode;
    ds->kbd_put_keysym = kbd_put_keysym;

    memset(&sa.sin_addr, 0, sizeof(sa.sin_addr));
    sa.sin_port = htons(0);
    ((struct sockaddr *)&sa)->sa_family = AF_INET;

    display = vnc_display_init(ds, (struct sockaddr *)&sa, 1, title, NULL, 
		COLS * FONTW, LINES * FONTH );
    vncterm->console = text_console_init(ds);

#if 0
    {
	char *msg = "Hello World\n\r";
	vncterm->console->chr_write(vncterm->console, (uint8_t *)msg,
				    strlen(msg));
    }
#endif

    ds->mouse_opaque = vncterm->console;
    ds->mouse_is_absolute = mouse_is_absolute;
    ds->mouse_event = mouse_event;

    ds->hw_opaque = vncterm->console;
    ds->hw_update = hw_update;
    ds->hw_invalidate = hw_invalidate;

#ifndef NXENSTORE
    if (xenstore_path && access("/proc/xen", F_OK))
	xenstore_path = NULL;

    if (xenstore_path) {
	char *path, *port;

	xs = xs_daemon_open();
	if (xs == NULL)
	    err(1, "xs_daemon_open");

	ret = asprintf(&path, "%s/vnc-port", xenstore_path);
	if (ret < 0)
	    err(1, "asprintf");

	ret = asprintf(&port, "%d", display);
	if (ret < 0)
	    err(1, "asprintf");

	ret = xs_write(xs, XBT_NULL, path, port, strlen(port));
	if (!ret)
	    err(1, "xs_write");

	if (!cmd_mode) {
	    ret = asprintf(&vncterm->xenstore_path, "%s/tty", xenstore_path);
	    if (ret < 0)
		err(1, "asprintf");

	    ret = xs_watch(xs, vncterm->xenstore_path, "tty");
	    if (!ret)
		err(1, "xs_watch");
	    set_fd_handler(xs_fileno(xs), NULL, read_xs_watch, NULL, vncterm);
	}
    }
    else /* fallthrough */
#endif
    if (!pty_path)
	cmd_mode = 1;

    if (cmd_mode) {
	/* count env variables */
	for (nenv = 0; envp[nenv]; nenv++);

	newenvp = malloc(++nenv * sizeof(char *));
	if (newenvp == NULL)
		err(1, "malloc");

	for (nenv = 0; envp[nenv]; nenv++) {
		if (!strncmp(envp[nenv], "TERM=", 5)) 
			newenvp[nenv] = "TERM=linux";
		else
			newenvp[nenv] = envp[nenv];
	}
	newenvp[nenv] = NULL;

	if (argc == optind) {
	    argv = calloc(2, sizeof(char *));
	    argv[0] = "/bin/bash";
	    argc = 1;
	} else {
	    argv += optind;
	    argc -= optind;
	}
    }

    if (pty_path)
	vncterm->pty = connect_pty(pty_path, vncterm->console);

    signal(SIGUSR1, handle_sigusr1);
    signal(SIGUSR2, handle_sigusr2);
    signal(SIGCHLD, handle_sigchld);

    for (;;) {
	if (restart_needed && cmd_mode) {
	    if (vncterm->process)
		end_process(vncterm->process);
	    vncterm->process = run_process(vncterm->console, argv[0],
					   argv, newenvp);
	    restart_needed = 0;
	}

	if (exit_when_all_disconnect && !nrof_clients_connected(vncterm->console))
	    exit(0);

        if (dump_cells) {
	    dump_cells = 0;
	    dump_console_to_file(vncterm->console, "/tmp/console.cells");
	}

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
		    if (ioh->fd == console_input_fd(vncterm->console)) {
			if (restart)
			    restart_needed = 1;
			else if (exit_on_eof)
			    exit_when_all_disconnect=1;
		    }
		    ioh->enabled = 0;
		    handlers_updated = 1;
		}
		if (revents & POLLOUT && ioh->fd_write)
		    ioh->fd_write(ioh->opaque);
		if (revents & POLLIN && ioh->fd_read)
		    ioh->fd_read(ioh->opaque);
	    }
	}
    }

    return 0;
}
