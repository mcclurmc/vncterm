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
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include <locale.h>

#ifndef NXENSTORE
#include <xs.h>
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
    void (*fd_error)(void *);
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
    if (!(*pioh)->enabled) {
	(*pioh)->pollfd = NULL;
	(*pioh)->fd_error = NULL;
    }
    handlers_updated = 1;
    return 0;
}

int
set_fd_error_handler(int fd, void (*fd_error)(void *))
{
    struct iohandler **pioh = &iohandlers;

    while (*pioh) {
	if ((*pioh)->fd == fd)
	    break;
	pioh = &(*pioh)->next;
    }
    if (*pioh == NULL)
	return 1;
    (*pioh)->fd_error = fd_error;
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

/* Not safe after we've dropped privileges */
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
    /* Only called at start of day, so doesn't need privsep */
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
read_xs_watch(struct xs_handle *xs, struct vncterm *vncterm)
{
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
vnc_start_viewer(char** opts)
{
    int pid, i, open_max;

    switch (pid = fork()) {
    case -1:
	fprintf(stderr, "vncviewer failed fork\n");
	exit(1);

    case 0:	/* child */
	open_max = sysconf(_SC_OPEN_MAX);
	for (i = 0; i < open_max; i++)
	    if (i != STDIN_FILENO &&
		i != STDOUT_FILENO &&
		i != STDERR_FILENO)
		close(i);
        execvp("/usr/bin/vncviewer", opts);
	err(1, "vncviewer execlp failed\n");

    default:
	return pid;
    }
}

static char root_directory[64];
static int privsep_fd;
static int child_pid;

static void clean_exit(int ret)
{
    if (strcmp(root_directory, "/var/empty"))
        rmdir(root_directory);
    exit(ret);
}

/* Read data with the assertion that it all must come through, or
 * else abort the process.  Based on atomicio() from openssh. */
static void
must_read(int fd, void *buf, size_t n)
{
    char *s = buf;
    ssize_t res, pos = 0;

    while (n > pos) {
        res = read(fd, s + pos, n - pos);
        switch (res) {
            case -1:
                if (errno == EINTR || errno == EAGAIN)
                    continue;
            case 0:
                return;
            default:
                pos += res;
        }
    }
}

/* Write data with the assertion that it all has to be written, or
 * else abort the process.  Based on atomicio() from openssh. */
static void
must_write(int fd, const void *buf, size_t n)
{
    const char *s = buf;
    ssize_t res, pos = 0;

    while (n > pos) {
        res = write(fd, s + pos, n - pos);
        switch (res) {
            case -1:
                if (errno == EINTR || errno == EAGAIN)
                    continue;
            case 0:
                exit(0);
            default:
                pos += res;
        }
    }
}

static void handle_sigsegv(int num)
{
    char buf[] = "sigsegv";
    must_write(privsep_fd, &buf, 8);
    while (access("/", W_OK) != 0) {
        sleep(1);
    }
}

static void parent_handle_sigchld(int num)
{
    int status, pid;
    pid = wait(&status);
    if (pid == child_pid) {
        if (!WCOREDUMP(status))
            clean_exit(0);
        else
            exit(0);
    } else
        signal(SIGCHLD, parent_handle_sigchld);
}

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
    char *statefile = NULL;
#ifndef NXENSTORE
    char *xenstore_path = NULL;
#endif
    char *vnclisten = NULL;
    char *vncvieweroptions = NULL;
    int exit_on_eof = 1;
    int restart = 0;
    int restart_needed = 1;
    int cmd_mode = 0;
    int exit_when_all_disconnect = 0;
    int stay_root = 0;
    int vncviewer = 0;

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
	    {"vnclisten", 1, 0, 'v'},
        {"stay-root", 0, 0, 'S'},
        {"vncviewer", 2, 0, 'V'},
            {"loadstate", 1, 0, 'l'},
	    {0, 0, 0, 0}
	};

	c = getopt_long(argc, argv, "+cp:rst:x:v:SV::l:", long_options, NULL);
	if (c == -1)
	    break;

	switch (c) {
        case 'l':
            statefile = strdup(optarg);
            break;
	case 'c':
	    cmd_mode = 1;
            /* We sometimes re-exec ourselves when run in cmd mode,
               and expect to have root when we come back.  We
               therefore can't drop privileges in command mode. */
            stay_root = 1;
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
        case 'S':
            stay_root = 1;
            break;
	case 'x':
#ifndef NXENSTORE
	    xenstore_path = strdup(optarg);
#endif
	    break;
	case 'v':
	    vnclisten = strdup(optarg);
	    break;
	case 'V':
	    vncviewer = 1;
        if (optarg != NULL)
            vncvieweroptions = strdup(optarg);
        break;
	}
    }

    setlocale(LC_ALL, "en_US.UTF-8");
    ds = &display_state;
    memset(ds, 0, sizeof(display_state));
    ds->set_fd_handler = set_fd_handler;
    ds->set_fd_error_handler = set_fd_error_handler;
    ds->init_timer = init_timer;
    ds->get_clock = get_clock;
    ds->set_timer = set_timer;
    ds->kbd_put_keycode = kbd_put_keycode;
    ds->kbd_put_keysym = kbd_put_keysym;

    memset(&sa.sin_addr, 0, sizeof(sa.sin_addr));
    if (vnclisten != NULL)
    {
        char *c;
        c = strchr(vnclisten, ':');
        if (c != NULL) {
            int port;
            char *r;
            *c = '\0';
            c++;
            if (strlen(vnclisten) > 1)
                if (!inet_aton(vnclisten, &(sa.sin_addr))) err(1, "inet_aton");
            port = strtol(c, (char **)&r, 10);
            if (r[0] != '\0' && c[0] != '\0') {
                fprintf(stderr, "incorrect port number\n");
                exit(1);
            }
            sa.sin_port = htons(port);
        } else {
            if (!inet_aton(vnclisten, &(sa.sin_addr))) err(1, "inet_aton");
            sa.sin_port = htons(0);
        }
    } else {
        sa.sin_port = htons(0);
    }
    ((struct sockaddr *)&sa)->sa_family = AF_INET;

    display = vnc_display_init(ds, (struct sockaddr *)&sa, 1, title, NULL, 
		COLS * FONTW, LINES * FONTH );
    vncterm->console = text_console_init(ds);
    
    if (statefile != NULL) {
        load_console_from_file(vncterm->console, statefile);
    }
    
    if (vncviewer == 1) {
        int i, l = 0;
        int count = 0;
        char **opts;
        char* vmuuid;
        char name[50];
        char port[10];

        if (vncvieweroptions != NULL) {
            l = strlen(vncvieweroptions);
            for (i = 0; i < l; i++) {
                if (vncvieweroptions[i] == ';') {
                    count++;
                }
            }
            count++;
        }
        count = count + 5;
        opts = (char **) malloc (count * sizeof(char*));
        opts[0] = "vncviewer";
        count = 1;
        if (vncvieweroptions != NULL) {
            opts[count] = vncvieweroptions;
            count++; 
            for (i = 0; i < l; i++) {
                if (vncvieweroptions[i] == ';') {
                    vncvieweroptions[i] = '\0';
                    opts[count] = (char *) (vncvieweroptions + i + 1);
                    count++;
                }
            }
        }
        sprintf(port, ":%d", display);
        opts[count] = port;
        count++;
        opts[count] = "-name";
        count++;
        vmuuid = getenv("VMUUID");
        strcpy(name, "vncterm-");
        strncat(name, vmuuid, 37);
        opts[count] = name;
        count++;
        opts[count] = NULL;
        vnc_start_viewer(opts);
        free(opts);
    }
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
        struct xs_handle *xs;

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

            while (vncterm->pty == NULL)
                read_xs_watch(xs, vncterm);
	}

        xs_daemon_close(xs);
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

        stay_root = 1;
    }

    if (pty_path)
	vncterm->pty = connect_pty(pty_path, vncterm->console);

    if (stay_root) {
        /* warnx("not dropping root privileges"); */
    } else {
        int socks[2];
        gid_t vncterm_gid;
        uid_t vncterm_uid;
        struct passwd *pw;
        pw = getpwnam("vncterm_base");
        if (!pw)
            err(1, "getting uid/gid for vncterm_base");
        vncterm_gid = pw->pw_gid + (unsigned short)display;
        vncterm_uid = pw->pw_uid + (unsigned short)display;

        snprintf(root_directory, 64, "/var/xen/vncterm/%d", getpid());
        if (mkdir(root_directory, 00755) < 0) {
            fprintf(stderr, "cannot create vncterm scratch directory");
            strcpy(root_directory, "/var/empty");
        }

        if (socketpair(AF_LOCAL, SOCK_STREAM, PF_UNSPEC, socks) == -1)
            err(1, "socketpair() failed");

        child_pid = fork();
        if (child_pid < 0) err(1, "fork() failed");
        else if (child_pid > 0) {
            char buf[8];
            close(socks[0]);
            signal(SIGCHLD, parent_handle_sigchld);
            must_read(socks[1], &buf, 8);
            if (!strncmp(buf, "sigsegv", 8) && strcmp(root_directory, "/var/empty")) {
                chown(root_directory, vncterm_uid, vncterm_gid);
                sleep(7);
            }
            /* If we are still alive it means we didn't receive a SIGCHLD */
            kill(child_pid, SIGQUIT);
            sleep(3);
            if (!access("/usr/bin/pkill", X_OK)) {
                char *pkill;
                asprintf(&pkill, "pkill -SIGKILL -U %d -G %d", vncterm_uid, vncterm_gid);
                system(pkill);
            }
            clean_exit(0);
        } else {
            close(socks[1]);
            privsep_fd = socks[0];

            chdir(root_directory);
            chroot(root_directory);

            setgid(vncterm_gid);
            setuid(vncterm_uid);

            /* qemu core dumps are often useful; make sure they're allowed. */
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

            /* handling SIGSEGV */
            signal (SIGSEGV, handle_sigsegv);
        }
    }

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
	    dump_console_to_file(vncterm->console, "vncterm.statefile");
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
                ds->dpy_close_vncviewer_connections(ds);
			if (restart)
			    restart_needed = 1;
			else if (exit_on_eof)
			    exit_when_all_disconnect=1;
		    }
		    if (ioh->fd_error)
			ioh->fd_error(ioh->opaque);
		    ioh->enabled = 0;
		    ioh->pollfd = NULL;
		    handlers_updated = 1;
		    continue;
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
