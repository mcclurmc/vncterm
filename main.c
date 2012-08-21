/*
    Copyright (c) Citrix Systems Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

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
#include <sched.h>

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
#include "libvnc/libtextterm.h"

#define LINES	24
#define COLS	80

#define FONTH	16
#define FONTW	8

#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif

char vncpasswd[64];
unsigned char challenge[AUTHCHALLENGESIZE];

DisplayState display_state;
TextDisplayState text_display_state;
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

enum privsep_opcode {
    privsep_op_statefile_completed
};

static void _write_port_to_xenstore(char *xenstore_path, char *type, int port);

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
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
	    err(1, "clock_gettime");
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
	if ((*c)->timeout >= timeout && n == timers_tail)
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
    TextDisplayState *tds;
    pid_t pid;
};

void
process_read(void *opaque)
{
    struct process *p = opaque;
    uint8_t buf[16];
    int count;

    count = read(p->fd, buf, sizeof(buf));
    if (count > 0)
    {
        p->console->chr_write(p->console, buf, count);
        if (p->tds)
            p->tds->chr_write(p->tds, buf, count);
    }
}

static void _configure_input_fd(CharDriverState *console,
                                TextDisplayState *tds,
                                int fd, void (*fd_read)(void *), void *opaque)
{
    fcntl(fd, F_SETFL, O_NONBLOCK);
    set_fd_handler(fd, NULL, fd_read, NULL, opaque);
    console_set_input(console, fd, opaque);
    if (tds)
        text_term_display_set_input(tds, fd, opaque);
}

/* Not safe after we've dropped privileges */
struct process *
run_process(CharDriverState *console, TextDisplayState *tds,
            const char *filename, char *const argv[], char *const envp[])
{
    struct process *p;
    struct winsize ws;

    p = calloc(1, sizeof(struct process));
    if (p == NULL)
	err(1, "malloc");

    p->console = console;
    p->tds = tds;

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

    _configure_input_fd(console, tds, p->fd, process_read, p);
    return p;
}

void
end_process(struct process *p)
{
    close(p->fd);
    free(p);
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
    TextDisplayState *tds;
};

void
pty_read(void *opaque)
{
    struct pty *pty = opaque;
    uint8_t buf[16];
    int count;

    count = read(pty->fd, buf, sizeof(buf));
    if (count > 0)
    {
    	pty->console->chr_write(pty->console, buf, count);
        if (pty->tds != NULL)
            pty->tds->chr_write(pty->tds, buf, count);
    }
}

static struct pty *
connect_pty(char *pty_path, CharDriverState *console, TextDisplayState *tds)
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
    pty->tds = tds;

    _configure_input_fd(console, tds, pty->fd, pty_read, pty);

    return pty;
}

struct vncterm
{
    CharDriverState *console;
    TextDisplayState *tds;
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

    vncterm->pty = connect_pty(pty_path, vncterm->console, vncterm->tds);

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
static int parent_fd;
static int child_pid;
static gid_t vncterm_gid;
static uid_t vncterm_uid;
#ifndef NXENSTORE
struct xs_handle *xs = NULL;
char *xenstore_path = NULL;
#endif

static void clean_exit(int ret)
{
    if (strcmp(root_directory, "/var/empty")) {
        char name[80];
        struct stat buf;
        snprintf(name, 80, "%s/core.%d", root_directory, child_pid);
        if (!stat(name, &buf) && !buf.st_size)
            unlink(name);
        rmdir(root_directory);
    }
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
                clean_exit(0);
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

static void xenstore_write_statefile(const char *filepath)
{
    int ret;
    char *path = NULL;

    ret = asprintf(&path, "%s/statefile", xenstore_path);
    if (ret < 0)
        err(1, "asprintf");
    ret = xs_write(xs, XBT_NULL, path, filepath, strlen(filepath));
    if (!ret)
        err(1, "xs_write");

    free(path);
}

static void privsep_xenstore_statefile()
{
    uint32_t l;
    char filepath[256+1];

    must_read(parent_fd, &l, sizeof(l));
    if (l == 0 || l > 256) {
        errno = EINVAL;
        return;
    }
    must_read(parent_fd, filepath, l);
    filepath[l] = 0;

    xenstore_write_statefile(filepath);
}

static void privsep_statefile_completed(const char *name)
{
    enum privsep_opcode cmd;
    uint32_t l;

    if (privsep_fd <= 0) {
        xenstore_write_statefile(name);
        return;
    }
    cmd = privsep_op_statefile_completed;
    must_write(privsep_fd, &cmd, sizeof(cmd));
    l = strlen(name);
    must_write(privsep_fd, &l, sizeof(l));
    must_write(privsep_fd, name, l);
}

static void sigxfsz_handler(int num)
{
    struct rlimit rlim;

    getrlimit(RLIMIT_FSIZE, &rlim);
    rlim.rlim_cur = rlim.rlim_max;
    setrlimit(RLIMIT_FSIZE, &rlim);

    write(2, "SIGXFSZ received: exiting\n", 26);

    exit(1);
}

static void parent_handle_sigusr1(int num)
{
    if (strcmp(root_directory, "/var/empty")) {
        int f;
        char name[80];
        snprintf(name, 80, "%s/vncterm.statefile", root_directory);
        f = creat(name, 0644);
        if (f > 0) {
            close(f);
            chown(name, vncterm_uid, vncterm_gid);
        }
    }
    kill(child_pid, SIGUSR1);
    signal(SIGUSR1, parent_handle_sigusr1);
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

static void parent_handle_sigterm(int num)
{
    kill(child_pid, SIGTERM);
    signal(SIGTERM, parent_handle_sigterm);
}

int
main(int argc, char **argv, char **envp)
{
    DisplayState *ds;
    TextDisplayState *tds;
    struct vncterm *vncterm;
    struct sockaddr_in sa, sat;
    int display;
    int text_display;
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
    char *vnclisten = NULL;
    char *vncvieweroptions = NULL;
    int exit_on_eof = 1;
    int restart = 0;
    int restart_needed = 1;
    int cmd_mode = 0;
    int exit_when_all_disconnect = 0;
    int stay_root = 0;
    int vncviewer = 0;
    int enable_textterm = 0;

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
            {"text", 0, 0, 'T'},
	    {0, 0, 0, 0}
	};

	c = getopt_long(argc, argv, "+cp:rst:x:v:SV::l:T", long_options, NULL);
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
        case 'T':
            enable_textterm = 1;
            break;
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

    tds = &text_display_state;
    memset(tds, 0, sizeof(text_display_state));
    if (enable_textterm) {
        tds->set_fd_handler = set_fd_handler;
        tds->set_fd_error_handler = set_fd_error_handler;
        tds->init_timer = init_timer;
        tds->get_clock = get_clock;
        tds->set_timer = set_timer;
    }

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

    /* make a copy of the listen port for text console*/
    memset(&sat.sin_addr, 0, sizeof(sat.sin_addr));
    sat.sin_family = sa.sin_family;
    sat.sin_addr = sa.sin_addr;
    sat.sin_port = sa.sin_port;

    display = vnc_display_init(ds, (struct sockaddr *)&sa, 1, title, NULL, 
		COLS * FONTW, LINES * FONTH );
    vncterm->console = text_console_init(ds);

    if (enable_textterm) {
        text_display = text_term_display_init(tds, (struct sockaddr *)&sat, 1,
                                              title);
        vncterm->tds = tds;
    }
    else {
        text_display = -1;
        vncterm->tds = NULL;
    }

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
	xs = xs_daemon_open();
	if (xs == NULL)
	    err(1, "xs_daemon_open");

        _write_port_to_xenstore(xenstore_path, "vnc", display);
        if (enable_textterm)
            _write_port_to_xenstore(xenstore_path, "tc", text_display);

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
	vncterm->pty = connect_pty(pty_path, vncterm->console, vncterm->tds);

    if (stay_root) {
        /* warnx("not dropping root privileges"); */
    } else {
        int socks[2];
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
            enum privsep_opcode opcode;
            close(socks[0]);
            parent_fd = socks[1];
            signal(SIGUSR1, parent_handle_sigusr1);
            signal(SIGCHLD, parent_handle_sigchld);
            signal(SIGTERM, parent_handle_sigterm);

            while (1) {
                must_read(parent_fd, &opcode, sizeof(opcode));
                switch (opcode) {
                case privsep_op_statefile_completed:
                    privsep_xenstore_statefile();
                    break;
                default:
                    clean_exit(0);
                }
            }
        } else {
            int f;
            char name[64];
            struct rlimit rlim;

            close(socks[1]);
            privsep_fd = socks[0];
            xs_daemon_close(xs);

            if (!cmd_mode)
                unshare(CLONE_NEWNET);

            rlim.rlim_cur = 64 * 1024 * 1024;
            rlim.rlim_max = 64 * 1024 * 1024 + 64;
            setrlimit(RLIMIT_FSIZE, &rlim);

            /* limit memory ro 32MB */
            rlim.rlim_cur = 32 * 1024 * 1024;
            rlim.rlim_max = 32 * 1024 * 1024;
            setrlimit(RLIMIT_AS, &rlim);

            rlim.rlim_cur = 256;
            rlim.rlim_max = 256;
            setrlimit(RLIMIT_NOFILE, &rlim);

            chdir(root_directory);
            chroot(root_directory);

            snprintf(name, 64, "core.%d", getpid());
            f = creat(name, 0644);
            if (f > 0) {
                close(f);
                chown(name, vncterm_uid, vncterm_gid);
            }

            setgid(vncterm_gid);
            setuid(vncterm_uid);

            /* vncterm core dumps are often useful; make sure they're allowed. */
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

            /* handling SIGXFSZ */
            signal(SIGXFSZ, sigxfsz_handler);
        }
    }

    signal(SIGUSR1, handle_sigusr1);
    signal(SIGUSR2, handle_sigusr2);
    signal(SIGCHLD, handle_sigchld);

    for (;;) {
	if (restart_needed && cmd_mode) {
	    if (vncterm->process)
		end_process(vncterm->process);
	    vncterm->process = run_process(vncterm->console, vncterm->tds,
                                           argv[0], argv, newenvp);
	    restart_needed = 0;
	}

	if (exit_when_all_disconnect && !nrof_clients_connected(vncterm->console))
	    exit(0);

        if (dump_cells) {
            char *filepath;
            int ret;

	    dump_cells = 0;
            if (strlen(root_directory))
                ret = asprintf(&filepath, "vncterm.statefile");
            else
                ret = asprintf(&filepath, "/tmp/vncterm.statefile.%d", getpid());
            if (ret < 0)
                err(1, "asprintf");
	    dump_console_to_file(vncterm->console, filepath);

#ifndef NXENSTORE
            if (xenstore_path) {
                char *fullfilepath;
                if (filepath[0] != '/') {
                    ret = asprintf(&fullfilepath, "%s/vncterm.statefile", root_directory);
                    if (ret < 0)
                        err(1, "asprintf");
                } else {
                    fullfilepath = malloc(strlen(filepath) + 1);
                    if (!fullfilepath)
                        err(1, "malloc");
                    memcpy (fullfilepath, filepath, strlen(filepath) + 1);
                }
                privsep_statefile_completed(fullfilepath);
                free(fullfilepath);
            }
#endif
            free(filepath);
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

        /* prevent DoS */
        alarm(20);

        /* Test for timers. Test even if not timeout to avoid situations where we have always data */
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

        alarm(0);
    }

    return 0;
}

static void _write_port_to_xenstore(char *xenstore_path, char *type, int no)
{
    char *path, *port;
    int ret;
    
    ret = asprintf(&path, "%s/%s-port", xenstore_path, type);
    if (ret < 0)
        err(1, "asprintf");

    ret = asprintf(&port, "%d", no);
    if (ret < 0)
        err(1, "asprintf");

    ret = xs_write(xs, XBT_NULL, path, port, strlen(port));
    if (!ret)
        err(1, "xs_write");

    free(port);
    free(path);
}
