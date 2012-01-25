#include "vnc.h"
#include "qemu_socket.h"
#include <assert.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "buffer.h"
#include "libtextterm.h"

//#define DEBUG_TEXTTERM
#ifdef DEBUG_TEXTTERM
#define dprintf(s, ...) printf(s, ## __VA_ARGS__)
#else
#define dprintf(s, ...)
#endif

#define MAX_CLIENTS 8

typedef struct TextTermState TextTermState;

struct TextTermClientState
{
    struct TextTermState *ts;
    int csock;
    Buffer output;
    Buffer input;
};

struct TextTermState
{
    char *title;

    int lsock;

    TextDisplayState *ds;
    int ssock;
    struct TextTermClientState *tcs[MAX_CLIENTS];
};

#define TCS_INUSE(tcs) ((tcs) && (tcs)->csock != -1)
#define TCS_ACTIVE(tcs) ((tcs))

static void text_term_client_read(void *opaque);
static inline void text_term_write_pending(struct TextTermClientState *tcs);
static void text_term_write(struct TextTermClientState *tcs, const void *data,
                            size_t len);

static void reset_tcs(struct TextTermClientState *tcs)
{
    if (tcs->csock != -1) {
        tcs->ts->ds->set_fd_handler(tcs->csock, NULL, NULL, NULL, NULL);
        closesocket(tcs->csock);
    }
    tcs->csock = -1;
    buffer_reset(&tcs->input);
    buffer_reset(&tcs->output);
}

static int text_term_client_io_error(struct TextTermClientState *tcs, int ret,
                                     int last_errno)
{
    if (ret != 0 && ret != -1)
	return ret;

    if (ret == -1 && (last_errno == EINTR || last_errno == EAGAIN))
	return 0;

    dprintf("text_term_client_io_error on %d\n", tcs->csock);
    reset_tcs(tcs);
    return ret;
}

static void text_term_client_error(void *opaque)
{
    struct TextTermClientState *tcs = opaque;

    text_term_client_io_error(tcs, -1, EINVAL);
}

static void text_term_listen_read(void *opaque)
{
    TextTermState *ts = opaque;
    struct TextTermClientState *tcs;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int new_sock;
    int i;

    new_sock = accept(ts->lsock, (struct sockaddr *)&addr, &addrlen);
    if (new_sock == -1)
    	return;

    for (i = 0; i < MAX_CLIENTS; i++)
	if (!TCS_INUSE(ts->tcs[i]))
	    break;

    if (i == MAX_CLIENTS)
    	goto fail;

    if (ts->tcs[i] == NULL) {
    	ts->tcs[i] = calloc(1, sizeof(struct TextTermClientState));
    	if (ts->tcs[i] == NULL)
            goto fail;
    }
    else {
        reset_tcs(ts->tcs[i]);
    }

    tcs = ts->tcs[i];
    tcs->ts = ts;
    tcs->csock = new_sock;
    socket_set_nonblock(tcs->csock);

    ts->ds->set_fd_handler(tcs->csock, NULL, text_term_client_read, NULL, tcs);
    ts->ds->set_fd_error_handler(tcs->csock, text_term_client_error);

    return;

fail:
    closesocket(new_sock);
}

static void text_term_client_read(void *opaque)
{
    struct TextTermClientState *tcs = opaque;
    long ret;

    buffer_reserve(&tcs->input, 4096);

    ret = recv(tcs->csock, buffer_end(&tcs->input), 4096, 0);
    ret = text_term_client_io_error(tcs, ret, socket_error());
    if (ret <= 0)
    	return;

    tcs->input.offset += ret;

    while (tcs->input.offset > 0) {
        ssize_t ret = write(tcs->ts->ssock, tcs->input.buffer,
                            tcs->input.offset);
        if (ret > 0) {
            if (ret != tcs->input.offset) {
                memmove(tcs->input.buffer, tcs->input.buffer + ret,
                        tcs->input.offset - ret);
            }
            tcs->input.offset -= ret;
    	}
        else {
            text_term_client_io_error(tcs, ret, socket_error());
            return;
        }
    }
}

static void text_term_client_write(void *opaque)
{
    long ret;
    struct TextTermClientState *tcs = opaque;
    struct TextTermState *ts = tcs->ts;

    while (1) {
    	if (tcs->output.offset == 0) {
            dprintf("disable write\n");
            ts->ds->set_fd_handler(tcs->csock, NULL, text_term_client_read,
                                   NULL, tcs);
            break;
    	}

    	dprintf("write %d\n", tcs->output.offset);
    	ret = send(tcs->csock, tcs->output.buffer, tcs->output.offset, 0);
    	ret = text_term_client_io_error(tcs, ret, socket_error());
    	if (ret <= 0) {
            dprintf("write error %d with %d\n", errno, tcs->output.offset);
            return;
    	}

        if (tcs->output.offset != ret)
            memmove(tcs->output.buffer, tcs->output.buffer + ret,
                    tcs->output.offset - ret);
    	tcs->output.offset -= ret;

    	if (tcs->output.offset)
            break;
    }
}

void text_term_chr_write(struct TextDisplayState *ds, const uint8_t *data,
                         int len)
{
    TextTermState *ts = ds->opaque;
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
    	if (TCS_ACTIVE(ts->tcs[i])) {
            text_term_write(ts->tcs[i], data, len);
    	}
    }
}

static void text_term_write(struct TextTermClientState *tcs, const void *data,
                            size_t len)
{
    buffer_reserve(&tcs->output, len);

    text_term_write_pending(tcs);

    buffer_append(&tcs->output, data, len);
}

static inline void text_term_write_pending(struct TextTermClientState *tcs)
{
    struct TextTermState *ts = tcs->ts;

    if (buffer_empty(&tcs->output)) {
	dprintf("enable write\n");
	ts->ds->set_fd_handler(tcs->csock, NULL, text_term_client_read,
			       text_term_client_write, tcs);
    }
}

/* returns the server port number to listen to; or 0 for AN_UNIX family*/
int text_term_display_init(TextDisplayState *ds, struct sockaddr *addr,
                           int find_unused, char *title)
{
    struct sockaddr_in *iaddr = NULL;
    int reuse_addr, ret;
    socklen_t addrlen;
    TextTermState *ts;
    in_port_t port = 0;

    ts = qemu_mallocz(sizeof(TextTermState));
    if (!ts)
	exit(1);

    memset(ts, 0, sizeof(TextTermState));
    ds->opaque = ts;
    ds->chr_write  = text_term_chr_write;

    ts->lsock = -1;
    ts->ds = ds;

    memset(ts->tcs, 0, sizeof(ts->tcs));

    ts->title = strdup(title ?: "");
    ts->ds->data = NULL;

#ifndef _WIN32
    if (addr->sa_family == AF_UNIX) {
    	addrlen = sizeof(struct sockaddr_un);

    	ts->lsock = socket(PF_UNIX, SOCK_STREAM, 0);
    	if (ts->lsock == -1) {
            fprintf(stderr, "Could not create socket\n");
            exit(1);
    	}
    } else
#endif
 again:
    if (addr->sa_family == AF_INET) {
        iaddr = (struct sockaddr_in *)addr;
        addrlen = sizeof(struct sockaddr_in);

	if (!port) {
	    port = ntohs(iaddr->sin_port) + 9500;
	}

        ts->lsock = socket(PF_INET, SOCK_STREAM, 0);
        if (ts->lsock == -1) {
            fprintf(stderr, "Could not create socket\n");
            exit(1);
        }

        reuse_addr = 1;
        ret = setsockopt(ts->lsock, SOL_SOCKET, SO_REUSEADDR,
                         (const char *)&reuse_addr, sizeof(reuse_addr));
        if (ret == -1) {
            fprintf(stderr, "setsockopt() failed\n");
            exit(1);
        }
    } else {
        fprintf(stderr, "Invalid socket family %x\n", addr->sa_family);
        exit(1);
    }


      ret = fcntl(ts->lsock, F_GETFD, NULL);
      fcntl(ts->lsock, F_SETFD, ret | FD_CLOEXEC);

    do {
	iaddr->sin_port = htons(port);
	ret = bind(ts->lsock, addr, addrlen);
	if (ret == -1) {
	    if (errno == EADDRINUSE && find_unused && addr->sa_family == AF_INET) {
		port++;
	    }
	    else {
	      break;
	    }
	}
    } while (ret == -1);
    if (ret == -1) {
	fprintf(stderr, "bind() failed\n");
	exit(1);
    }

    if (listen(ts->lsock, 1) == -1) {
	if (errno == EADDRINUSE && find_unused && addr->sa_family == AF_INET) {
	    close(ts->lsock);
	    port++;
	    goto again;
	}
    	fprintf(stderr, "listen() failed\n");
    	exit(1);
    }

    ret = ts->ds->set_fd_handler(ts->lsock, NULL, text_term_listen_read, NULL,
                                 ts);
    if (ret == -1) {
    	exit(1);
    }

    if (addr->sa_family == AF_INET)
    	return ntohs(iaddr->sin_port);
    else
    	return 0;
}


void text_term_display_set_input(TextDisplayState *ds, int fd, void *opaque)
{
    TextTermState *ts = ds->opaque;
    ts->ssock = fd;
}
