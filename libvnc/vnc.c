/*
 * QEMU VNC display driver
 * 
 * Copyright (C) 2006 Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2006 Fabrice Bellard
 * Copyright (C) 2006 Christian Limpach <Christian.Limpach@xensource.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "vnc.h"
#include "qemu_socket.h"
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

//#define DEBUG_VNC
#ifdef DEBUG_VNC
#define dprintf(s, ...) printf(s, ## __VA_ARGS__)
#else
#define dprintf(s, ...)
#endif

/* The refresh interval starts at BASE.  If we scan the buffer and
   find no change, we increase by INC, up to MAX.  If the mouse moves
   or we get a keypress, the interval is set back to BASE.  If we find
   an update, halve the interval.

   All times in milliseconds. */
#define VNC_REFRESH_INTERVAL_BASE 30
#define VNC_REFRESH_INTERVAL_INC  50
#define VNC_REFRESH_INTERVAL_MAX  2000

/* Wait at most one second between updates, so that we can detect a
   minimised vncviewer reasonably quickly. */
#define VNC_MAX_UPDATE_INTERVAL   5000

#include "vnc_keysym.h"
#include "keymaps.c"
#include "d3des.h"
#include "buffer.h"

typedef struct VncState VncState;
struct VncClientState;

typedef int VncReadEvent(struct VncClientState *vcs, uint8_t *data,
			 size_t len);

typedef void VncWritePixels(struct VncClientState *vcs, void *data, int size);

typedef void VncSendHextileTile(struct VncClientState *vcs,
                                uint8_t *data, int stride,
                                int w, int h,
                                void *last_bg, 
                                void *last_fg,
                                int *has_bg, int *has_fg);

struct vnc_pm_region_update {
    struct vnc_pm_region_update *next;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
};

struct vnc_pm_server_cut_text {
    char *text;
};

struct vnc_pending_messages {
    uint8_t vpm_resize;
    uint8_t vpm_bell;
    uint8_t vpm_null_update;
    uint8_t vpm_server_cut_text;
    uint8_t vpm_cursor_update;
    struct vnc_pm_region_update *vpm_region_updates;
    struct vnc_pm_region_update **vpm_region_updates_last;
};

struct VncClientState
{
    struct VncState *vs;
    int csock;
    int isvncviewer;
    Buffer output;
    Buffer input;

    int has_resize;
    int has_hextile;
    int has_pointer_type_change;
    int has_cursor_encoding;

    int absolute;
    int last_x;
    int last_y;

    /* current output mode information */
    VncWritePixels *write_pixels;
    VncSendHextileTile *send_hextile_tile;
    int pix_bpp, pix_big_endian;
    int red_shift, red_max, red_shift1, red_max1;
    int green_shift, green_max, green_shift1, green_max1;
    int blue_shift, blue_max, blue_shift1, blue_max1;

    VncReadEvent *read_handler;
    size_t read_handler_expect;

    struct vnc_pending_messages vpm;

    uint64_t *update_row;	/* outstanding updates */
};

#define VCS_INUSE(vcs) ((vcs) && (vcs)->csock != -1)
#define VCS_ACTIVE(vcs) ((vcs) && (vcs)->pix_bpp)

#define MAX_CLIENTS 8

struct VncState
{
    char *title;

    void *timer;
    int timer_interval;
    int64_t last_update_time;

    int lsock;

    DisplayState *ds;
    struct VncClientState *vcs[MAX_CLIENTS];

    int dirty_pixel_shift;
    int has_update;		/* there's outstanding updates in the
				 * visible area */

    int depth; /* internal VNC frame buffer byte per pixel */

    int visible_x;
    int visible_y;
    int visible_w;
    int visible_h;

    const char *display;

    char *kbd_layout_name;
    kbd_layout_t *kbd_layout;

    /* input */
    uint8_t modifiers_state[256];

    int send_resize;

    char *server_cut_text;
    char *client_cut_text;
    unsigned int client_cut_text_size;
};

#if 0
static VncState *vnc_state; /* needed for info vnc */
#endif

#define DIRTY_PIXEL_BITS 64
#define X2DP_DOWN(vs, x) ((x) >> (vs)->dirty_pixel_shift)
#define X2DP_UP(vs, x) \
  (((x) + (1ULL << (vs)->dirty_pixel_shift) - 1) >> (vs)->dirty_pixel_shift)
#define DP2X(vs, x) ((x) << (vs)->dirty_pixel_shift)

#if 0
void do_info_vnc(void)
{
    if (vnc_state == NULL)
	term_printf("VNC server disabled\n");
    else {
	term_printf("VNC server active on: ");
	term_print_filename(vnc_state->display);
	term_printf("\n");

	if (vnc_state->csock == -1)
	    term_printf("No client connected\n");
	else
	    term_printf("Client connected\n");
    }
}
#endif

/* TODO
   1) Get the queue working for IO.
   2) there is some weirdness when using the -S option (the screen is grey
      and not totally invalidated
*/

static inline void vnc_write_pending(struct VncClientState *vcs);
static inline void vnc_write_pending_all(struct VncState *vs);
static void vnc_write(struct VncClientState *vcs, const void *data,
		      size_t len);
static void vnc_write_u32(struct VncClientState *vcs, uint32_t value);
static void vnc_write_s32(struct VncClientState *vcs, int32_t value);
static void vnc_write_u16(struct VncClientState *vcs, uint16_t value);
static void vnc_write_u8(struct VncClientState *vcs, uint8_t value);
static void vnc_flush(struct VncClientState *vcs);
static void _vnc_update_client(void *opaque);
static void vnc_update_client(void *opaque);
static void vnc_client_read(void *opaque);
static void framebuffer_set_updated(VncState *vs, int x, int y, int w, int h);
static int make_challenge(unsigned char *random, int size);
static void set_seed(unsigned int *seedp);
static void get_random(int len, unsigned char *buf);

#if 0
static inline void vnc_set_bit(uint32_t *d, int k)
{
    d[k >> 5] |= 1 << (k & 0x1f);
}

static inline void vnc_clear_bit(uint32_t *d, int k)
{
    d[k >> 5] &= ~(1 << (k & 0x1f));
}

static inline void vnc_set_bits(uint32_t *d, int n, int nb_words)
{
    int j;

    j = 0;
    while (n >= 32) {
        d[j++] = -1;
        n -= 32;
    }
    if (n > 0) 
        d[j++] = (1 << n) - 1;
    while (j < nb_words)
        d[j++] = 0;
}

static inline int vnc_get_bit(const uint32_t *d, int k)
{
    return (d[k >> 5] >> (k & 0x1f)) & 1;
}

static inline int vnc_and_bits(const uint32_t *d1, const uint32_t *d2, 
                               int nb_words)
{
    int i;
    for(i = 0; i < nb_words; i++) {
        if ((d1[i] & d2[i]) != 0)
            return 1;
    }
    return 0;
}
#endif

static void set_bits_in_row(VncState *vs, uint64_t *row,
			    int x, int y, int w, int h)
{
    int x1, x2;
    uint64_t mask;

    if (w == 0)
	return;

    x1 = X2DP_DOWN(vs, x);
    x2 = X2DP_UP(vs, x + w);

    if (X2DP_UP(vs, w) != DIRTY_PIXEL_BITS)
	mask = ((1ULL << (x2 - x1)) - 1) << x1;
    else
	mask = ~(0ULL);

    h += y;
    if (h > vs->ds->height)
        h = vs->ds->height;
    for (; y < h; y++)
	row[y] |= mask;
}

static void vnc_dpy_update(DisplayState *ds, int x, int y, int w, int h)
{
    VncState *vs = ds->opaque;
    unsigned int i;

    for (i = 0; i < MAX_CLIENTS; i++)
	if (VCS_ACTIVE(vs->vcs[i]))
	    set_bits_in_row(vs, vs->vcs[i]->update_row, x, y, w, h);

    if (!vs->has_update && vs->timer) {
        vs->ds->set_timer(vs->timer, vs->ds->get_clock() + vs->timer_interval);
        vs->has_update = 1;
    }
}

static unsigned char vnc_dpy_clients_connected(DisplayState *ds)
{
    VncState *vs = ds->opaque;
    unsigned char nrofclients = 0, i;

    for (i = 0; i < MAX_CLIENTS; i++)
	if (VCS_ACTIVE(vs->vcs[i]))
	    nrofclients++;

    return nrofclients;
}

static void vnc_framebuffer_update(struct VncClientState *vcs, int x, int y,
				   int w, int h, int32_t encoding)
{
    vnc_write_u16(vcs, x);
    vnc_write_u16(vcs, y);
    vnc_write_u16(vcs, w);
    vnc_write_u16(vcs, h);

    vnc_write_s32(vcs, encoding);
}

static void vnc_send_bell(DisplayState *ds)
{
    VncState *vs = ds->opaque;
    struct VncClientState *vcs;
    int i;

    dprintf("send bell\n");
    for (i = 0; i < MAX_CLIENTS; i++) {
	if (!VCS_ACTIVE(vs->vcs[i]))
	    continue;
	vcs = vs->vcs[i];

	vcs->vpm.vpm_bell++;
	vnc_write_pending(vcs);
    }
}

static void vnc_send_resize(DisplayState *ds)
{
    VncState *vs = ds->opaque;
    struct VncClientState *vcs;
    int i;

    if (!vs->send_resize)
	return;

    dprintf("send resize\n");
    for (i = 0; i < MAX_CLIENTS; i++) {
	if (!VCS_ACTIVE(vs->vcs[i]))
	    continue;
	vcs = vs->vcs[i];

	vcs->vpm.vpm_resize = 1;
	vnc_write_pending(vcs);
    }
}

static void vnc_flush_region_updates(struct vnc_pending_messages *vpm)
{
    struct vnc_pm_region_update *rup;
    
    while (vpm->vpm_region_updates) {
	rup = vpm->vpm_region_updates;
	vpm->vpm_region_updates = rup->next;
	free(rup);
    }
    vpm->vpm_region_updates_last = &vpm->vpm_region_updates;
}

static void vnc_reset_pending_messages(struct vnc_pending_messages *vpm)
{
    vpm->vpm_resize = 0;
    vpm->vpm_bell = 0;
    vpm->vpm_server_cut_text = 0;
    vnc_flush_region_updates(vpm);
}

static void vnc_dpy_resize(DisplayState *ds, int w, int h)
{
    VncState *vs = ds->opaque;
    int o, i;

    for (i = 0; i < MAX_CLIENTS; i++) {
	if (!VCS_ACTIVE(vs->vcs[i]))
	    continue;
	vnc_flush_region_updates(&vs->vcs[i]->vpm);
    }
    dprintf("dpy resize w %d->%d h %d->%d depth %d\n", ds->width, w,
	    ds->height, h, vs->depth);

    if (w != ds->width || h != ds->height || w * vs->depth != ds->linesize) {
	free(ds->data);
	ds->data = qemu_mallocz(w * h * vs->depth);

        for (i = 0; i < MAX_CLIENTS; i++) {
	    if (!vs->vcs[i])
                continue;
            free(vs->vcs[i]->update_row);
            vs->vcs[i]->update_row = qemu_mallocz(h * sizeof(vs->vcs[i]->update_row[0]));
            if (!vs->vcs[i]->update_row) {
	        fprintf(stderr, "vnc: memory allocation failed\n");
	        exit(1);
            }
        }
	if (ds->data == NULL) {
	    fprintf(stderr, "vnc: memory allocation failed\n");
	    exit(1);
	}
    }

    if (ds->depth != vs->depth * 8) {
        ds->depth = vs->depth * 8;
	if (ds->hw_refresh)
	    ds->hw_refresh(ds);
    }
    ds->width = w;
    ds->height = h;
    ds->linesize = w * vs->depth;
    vs->dirty_pixel_shift = 0;
    for (o = DIRTY_PIXEL_BITS; o < ds->width; o *= 2)
	vs->dirty_pixel_shift++;
    if (ds->width != w || ds->height != h)
	vnc_send_resize(ds);
    framebuffer_set_updated(vs, 0, 0, ds->width, ds->height);
}

/* fastest code */
static void vnc_write_pixels_copy(struct VncClientState *vcs, void *pixels,
				  int size)
{
    vnc_write(vcs, pixels, size);
}

/* slowest but generic code. */
static void vnc_convert_pixel(struct VncClientState *vcs, uint8_t *buf,
                  uint32_t v)
{
    uint32_t r, g, b;
    r = ((v >> vcs->red_shift1) & vcs->red_max1) * (vcs->red_max + 1) / (vcs->red_max1 + 1);
    g = ((v >> vcs->green_shift1) & vcs->green_max1) * (vcs->green_max + 1) / (vcs->green_max1 + 1);
    b = ((v >> vcs->blue_shift1) & vcs->blue_max1) * (vcs->blue_max + 1) / (vcs->blue_max1 + 1);
    v = (r << vcs->red_shift) | (g << vcs->green_shift) | (b << vcs->blue_shift);
    switch(vcs->pix_bpp) {
    case 1:
        buf[0] = (uint8_t) v;
        break;
    case 2:
        if (vcs->pix_big_endian) {
            buf[0] = v >> 8;
            buf[1] = v;
        } else {
            buf[1] = v >> 8;
            buf[0] = v;
        }
        break;
    default:
    case 4:
        if (vcs->pix_big_endian) {
            buf[0] = v >> 24;
            buf[1] = v >> 16;
            buf[2] = v >> 8;
            buf[3] = v;
        } else {
            buf[3] = v >> 24;
            buf[2] = v >> 16;
            buf[1] = v >> 8;
            buf[0] = v;
        }
        break;
    }
}

static void vnc_write_pixels_generic(struct VncClientState *vcs,
                     void *pixels1, int size)
{
    uint8_t buf[4];

    if (vcs->vs->depth == 4) {
        uint32_t *pixels = pixels1;
        int n, i;
        n = size >> 2;
        for(i = 0; i < n; i++) {
            vnc_convert_pixel(vcs, buf, pixels[i]);
            vnc_write(vcs, buf, vcs->pix_bpp);
        }
    } else if (vcs->vs->depth == 2) {
        uint16_t *pixels = pixels1;
        int n, i;
        n = size >> 1;
        for(i = 0; i < n; i++) {
            vnc_convert_pixel(vcs, buf, pixels[i]);
            vnc_write(vcs, buf, vcs->pix_bpp);
        }
    } else if (vcs->vs->depth == 1) {
        uint8_t *pixels = pixels1;
        int n, i;
        n = size;
        for(i = 0; i < n; i++) {
            vnc_convert_pixel(vcs, buf, pixels[i]);
            vnc_write(vcs, buf, vcs->pix_bpp);
        }
    } else {
        fprintf(stderr, "vnc_write_pixels_generic: VncState color depth not supported\n");
    }
}

unsigned char cursorbmsk[16] = {
	0xff, /* 11111111 */
	0x3c, /* 00111100 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x18, /* 00011000 */
	0x3c, /* 00111100 */
	0xff, /* 11111111 */
};

static void writeGrey(struct VncClientState *vcs, unsigned char **cur)
{
    uint8_t r, g, b;
    r = (0xc0) * (vcs->red_max + 1) / (256);
    g = (0xc0) * (vcs->green_max + 1) / (256);
    b = (0xc0) * (vcs->blue_max + 1) / (256);
    switch(vcs->pix_bpp) {
    case 1:
        **cur = (r << vcs->red_shift) | (g << vcs->green_shift) | (b << vcs->blue_shift);
        *cur = *cur + 1;
        break;
    case 2:
    {
        uint16_t *p = (uint16_t *) *cur;
        *p = (r << vcs->red_shift) | (g << vcs->green_shift) | (b << vcs->blue_shift);
        if (vcs->pix_big_endian) {
            *p = htons(*p);
        }
        *cur = *cur + 2;
    }
        break;
    default:
    case 4:
        if (vcs->pix_big_endian) {
            (*cur)[0] = 255;
            (*cur)[1] = r;
            (*cur)[2] = g;
            (*cur)[3] = b;
        } else {
            (*cur)[0] = b;
            (*cur)[1] = g;
            (*cur)[2] = r;
            (*cur)[3] = 255;
        }
        *cur = *cur + 4;
        break;
    }
}

static void writeZero(struct VncClientState *vcs, unsigned char **cur)
{
    memset(*cur, 0x00, vcs->pix_bpp);
    *cur = *cur + vcs->pix_bpp;
}

static void vnc_send_custom_cursor(struct VncClientState *vcs)
{
    unsigned char *cursorcur, *cur;
    unsigned int size, i, j;
     
    if (vcs->has_cursor_encoding != 1)
    return;

    dprintf("sending custom cursor %d with bpp %d\n", vcs->csock,
        vcs->pix_bpp);
    size = sizeof(cursorbmsk) * 8 * vcs->pix_bpp;
    cursorcur = malloc(size);
    if (cursorcur == NULL)
    return;

    cur = (unsigned char *) cursorcur;

    for (i = 0; i < sizeof(cursorbmsk); i++) {
        for (j = 0; j < 8; j++) {
            if (cursorbmsk[i] & (1 << (8 - j)))
                writeGrey(vcs, &cur);
            else
                writeZero(vcs, &cur);
        }
    }

    vnc_write_u16(vcs, 0);
    vnc_write_u16(vcs, 1); /* number of rects */

    /* width 8, height - number of bytes in mask, hotspot in the middle */
    vnc_framebuffer_update(vcs, 8 / 2, sizeof(cursorbmsk) / 2, 8,
               sizeof(cursorbmsk), -239);
    vnc_write_pixels_copy(vcs, cursorcur, size);
    vnc_write(vcs, cursorbmsk, sizeof(cursorbmsk));

    free(cursorcur);
}

static void vnc_dpy_copy_rect(DisplayState *ds, int xf, int yf, int xt, int yt, int w, int h)
{
    struct VncState *vs = ds->opaque;
    struct VncClientState *vcs;
    int i;

    dprintf("sending copy rect. %d,%d->%d,%d [%d,%d]\n", xf, yf, xt, yt, w, h);

    for (i = 0; i < MAX_CLIENTS; i++) {
	if (!VCS_ACTIVE(vs->vcs[i]))
	    continue;
	
	vcs = vs->vcs[i];

	vnc_write_u16(vcs, 0);
	vnc_write_u16(vcs, 1); /* number of rects */
	
	/* width 8, height - number of bytes in mask, hotspot in the middle */
	vnc_framebuffer_update(vcs, xt, yt, w, h, 1);
	vnc_write_u16(vcs, xf); /* src X */
	vnc_write_u16(vcs, yf); /* src Y */
    }

}

static void hextile_enc_cord(uint8_t *ptr, int x, int y, int w, int h)
{
    ptr[0] = ((x & 0x0F) << 4) | (y & 0x0F);
    ptr[1] = (((w - 1) & 0x0F) << 4) | ((h - 1) & 0x0F);
}

#define BPP 8
#include "vnchextile.h"
#undef BPP

#define BPP 16
#include "vnchextile.h"
#undef BPP

#define BPP 32
#include "vnchextile.h"
#undef BPP

#define GENERIC
#define BPP 8
#include "vnchextile.h"
#undef BPP
#undef GENERIC

#define GENERIC
#define BPP 16
#include "vnchextile.h"
#undef BPP
#undef GENERIC

#define GENERIC
#define BPP 32
#include "vnchextile.h"
#undef BPP
#undef GENERIC

static void send_framebuffer_update(struct VncClientState *vc, int x, int y, int w, int h)
{
    struct vnc_pm_region_update *rup;

    rup = malloc(sizeof(struct vnc_pm_region_update));
    if (rup == NULL)
        return;			/* XXX */

    rup->next = NULL;
    rup->x = x;
    rup->y = y;
    rup->w = w;
    rup->h = h;

    *vc->vpm.vpm_region_updates_last = rup;
    vc->vpm.vpm_region_updates_last = &rup->next;
    dprintf("created rup %d %p %d %d %d %d %d %d\n", vc->csock,
	    rup, x, y, w, h, vc->pix_bpp, vc->vs->depth);
}

static void send_framebuffer_update_all(VncState *vs, int x, int y, int w, int h)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
	if (!VCS_ACTIVE(vs->vcs[i]))
	    continue;
	send_framebuffer_update(vs->vcs[i], x, y, w, h);
    }
}

 
static inline int find_update_height(VncState *vs, uint64_t *row,
				     int y, int maxy, uint64_t mask)
{
    int h = 1;

    while (y + h < maxy && row[y + h] & mask) {
	row[y + h] &= ~mask;
	h++;
    }

    return h;
}

static void _vnc_update_client(void *opaque)
{
    VncState *vs = opaque;
    int64_t now;

    now = vs->ds->get_clock();

    if (!vs->has_update || vs->visible_y >= vs->ds->height 
	|| vs->visible_x >= vs->ds->width)
	goto backoff;

    /* mark no updated so updater can schedule another timer when needed */
    vs->has_update = 0;
    vnc_send_resize(vs->ds);

    vnc_write_pending_all(vs);

    vs->last_update_time = now;

    vs->timer_interval /= 2;
    if (vs->timer_interval < VNC_REFRESH_INTERVAL_BASE)
	vs->timer_interval = VNC_REFRESH_INTERVAL_BASE;

    return;

 backoff:
    /* No update -> back off a bit */
    vs->timer_interval += VNC_REFRESH_INTERVAL_INC;
    if (vs->timer_interval > VNC_REFRESH_INTERVAL_MAX) {
	vs->timer_interval = VNC_REFRESH_INTERVAL_MAX;
	if (now - vs->last_update_time >= VNC_MAX_UPDATE_INTERVAL) {
	    /* Send a null update.  If the client is no longer
	       interested (e.g. minimised) it'll ignore this, and we
	       can stop scanning the buffer until it sends another
	       update request. */
	    /* It turns out that there's a bug in realvncviewer 4.1.2
	       which means that if you send a proper null update (with
	       no update rectangles), it gets a bit out of sync and
	       never sends any further requests, regardless of whether
	       it needs one or not.  Fix this by sending a single 1x1
	       update rectangle instead. */
            vnc_send_resize(vs->ds);
            dprintf("send null update\n");
	    send_framebuffer_update_all(vs, 0, 0, 1, 1);
	    vnc_write_pending_all(vs);
	    vs->last_update_time = now;
	    return;
	}
    }
    vs->ds->set_timer(vs->timer, now + vs->timer_interval);
    return;
}

static void vnc_set_server_text(DisplayState *ds, char *text)
{
    VncState *vs = ds->opaque;
    int i;

    if (!text) return;
    if (vs->server_cut_text)
	free(vs->server_cut_text);
    vs->server_cut_text = text;
    for (i = 0; i < MAX_CLIENTS; i++)
	if (VCS_ACTIVE(vs->vcs[i]))
	    vs->vcs[i]->vpm.vpm_server_cut_text = 1;
    vnc_write_pending_all(vs);
    dprintf("set server text %s\n", vs->server_cut_text);
}

static void vnc_update_client(void *opaque)
{
    VncState *vs = opaque;

    vs->ds->dpy_refresh(vs->ds);
    _vnc_update_client(vs);
}

static void vnc_timer_init(VncState *vs)
{
    if (vs->timer == NULL) {
	vs->timer = vs->ds->init_timer(vnc_update_client, vs);
	vs->timer_interval = VNC_REFRESH_INTERVAL_BASE;
    }
}

static void vnc_dpy_refresh(DisplayState *ds)
{
    if (ds->hw_update)
	ds->hw_update(ds->hw_opaque);
}

#if 0
static int vnc_listen_poll(void *opaque)
{
    return 1;
}
#endif

static int vnc_client_io_error(struct VncClientState *vcs, int ret,
			       int last_errno)
{
    struct VncState *vs = vcs->vs;

    if (ret > 0)
	return ret;

    if (ret == -1 && (last_errno == EINTR || last_errno == EAGAIN))
	return 0;

    dprintf("vnc_client_io_error on %d\n", vcs->csock);
    vs->ds->set_fd_handler(vcs->csock, NULL, NULL, NULL, NULL);
    closesocket(vcs->csock);
    vcs->csock = -1;
    buffer_reset(&vcs->input);
    buffer_reset(&vcs->output);
    vnc_reset_pending_messages(&vcs->vpm);
    vcs->pix_bpp = 0;
    return 0;
}

static void vnc_client_error(void *opaque)
{
    struct VncClientState *vcs = opaque;

    vnc_client_io_error(vcs, -1, EINVAL);
}

static int vnc_process_messages(struct VncClientState *vcs)
{
    struct vnc_pending_messages *vpm;
    struct VncState *vs = vcs->vs;
    int maxx, maxy, y;

    vpm = &vcs->vpm;
    if (vpm == NULL)
	return 0;

    dprintf("processing messages\n");
    if (vpm->vpm_resize) {
	dprintf("++ resize\n");
	vnc_write_u8(vcs, 0);  /* msg id */
	vnc_write_u8(vcs, 0);
	vnc_write_u16(vcs, 1); /* number of rects */
	vnc_framebuffer_update(vcs, 0, 0, vs->ds->width, vs->ds->height, -223);
	vpm->vpm_resize = 0;
    }
    while (vpm->vpm_bell) {
	dprintf("++ bell\n");
	vnc_write_u8(vcs, 2);  /* msg id */
	vpm->vpm_bell--;
    }
    if (vpm->vpm_server_cut_text) {
	char pad[3] = { 0, 0, 0 };
	dprintf("++ cut text\n");
	vnc_write_u8(vcs, 3);	/* ServerCutText */
	vnc_write(vcs, pad, 3);	/* padding */
	vnc_write_u32(vcs, strlen(vs->server_cut_text));	/* length */
	vnc_write(vcs, vs->server_cut_text,
		  strlen(vs->server_cut_text)); /* text */
	vpm->vpm_server_cut_text = 0;
    }
    if (vpm->vpm_cursor_update) {
	vnc_send_custom_cursor(vcs);
	vpm->vpm_cursor_update = 0;
    }

    maxy = vs->visible_y + vs->visible_h;
    if (maxy > vs->ds->height)
	maxy = vs->ds->height;
    maxx = vs->visible_x + vs->visible_w;
    if (maxx > vs->ds->width)
	maxx = vs->ds->width;

    for (y = vs->visible_y; y < maxy; y++) {
	int x, h;
	for (x = X2DP_DOWN(vs, vs->visible_x);
	     x < X2DP_UP(vs, maxx); x++) {
	    uint64_t mask = 1ULL << x;
	    if (vcs->update_row[y] & mask) {
		h = find_update_height(vs, vcs->update_row, y, maxy, mask);
		if (h != 0) {
		    send_framebuffer_update(vcs, DP2X(vs, x), y,
					    DP2X(vs, 1), h);
		}
	    }
	}
	vcs->update_row[y] = 0;
    }

    if (vpm->vpm_region_updates) {
	uint16_t n_rects;
	struct vnc_pm_region_update *rup;

	/* Count rectangles */
	n_rects = 0;
	for (rup = vpm->vpm_region_updates; rup; rup = rup->next)
	    n_rects++;
	dprintf("sending %d rups\n", n_rects);

	vnc_write_u8(vcs, 0);  /* msg id */
	vnc_write_u8(vcs, 0);
	vnc_write_u16(vcs, n_rects);
	while (vpm->vpm_region_updates) {
	    int i, j, stride;
	    uint8_t *row;
	    rup = vpm->vpm_region_updates;
	    vpm->vpm_region_updates = rup->next;
	    vnc_framebuffer_update(vcs, rup->x, rup->y, rup->w, rup->h,
				   vcs->has_hextile ? 5 : 0);
	    row = vs->ds->data + rup->y * vs->ds->linesize +
		rup->x * vs->depth;
	    stride = vs->ds->linesize;
        if (vcs->has_hextile) {
            int has_fg, has_bg;
            void *last_fg, *last_bg;
            last_fg = (void *) malloc(vcs->vs->depth);
            last_bg = (void *) malloc(vcs->vs->depth);
            has_fg = has_bg = 0;
            for (j = 0; j < rup->h; j += 16) {
                for (i = 0; i < rup->w; i += 16) {
                    vcs->send_hextile_tile(vcs, row + i * vs->depth,
                               stride,
                               MIN(16, rup->w - i),
                               MIN(16, rup->h - j),
                               last_bg, last_fg,
                               &has_bg, &has_fg);
                }
                row += 16 * stride;
            }
            free(last_fg);
            free(last_bg);
        } else {
            for (i = 0; i < rup->h; i++) {
            vcs->write_pixels(vcs, row, rup->w * vs->depth);
            row += stride;
            }
        }
	    dprintf("-- sent rup %p %d %d %d %d\n", rup, rup->x, rup->y,
		    rup->w, rup->h);
	    free(rup);
	}
	vpm->vpm_region_updates_last = &vpm->vpm_region_updates;
    }
    return vcs->output.offset;
}

static void vnc_client_write(void *opaque)
{
    long ret;
    struct VncClientState *vcs = opaque;
    struct VncState *vs = vcs->vs;

    while (1) {
	if (vcs->output.offset == 0 && vnc_process_messages(vcs) == 0) {
	    dprintf("disable write\n");
	    vs->ds->set_fd_handler(vcs->csock, NULL, vnc_client_read, NULL,
				   vcs);
	    break;
	}

	dprintf("write %d\n", vcs->output.offset);
	ret = send(vcs->csock, vcs->output.buffer, vcs->output.offset, 0);
	ret = vnc_client_io_error(vcs, ret, socket_error());
	if (!ret) {
	    dprintf("write error %d with %d\n", errno, vcs->output.offset);
	    return;
	}

	memmove(vcs->output.buffer, vcs->output.buffer + ret,
		vcs->output.offset - ret);
	vcs->output.offset -= ret;

	if (vcs->output.offset)
	    break;
    }
}

static void vnc_read_when(struct VncClientState *vcs, VncReadEvent *func,
			  size_t expecting)
{
    vcs->read_handler = func;
    vcs->read_handler_expect = expecting;
}

static void vnc_client_read(void *opaque)
{
    struct VncClientState *vcs = opaque;
    long ret;

    buffer_reserve(&vcs->input, 4096);

    ret = recv(vcs->csock, buffer_end(&vcs->input), 4096, 0);
    ret = vnc_client_io_error(vcs, ret, socket_error());
    if (!ret)
	return;

    vcs->input.offset += ret;

    while (vcs->read_handler &&
	   vcs->input.offset >= vcs->read_handler_expect) {
	size_t len = vcs->read_handler_expect;
	int ret;

	ret = vcs->read_handler(vcs, vcs->input.buffer, len);
	if (vcs->csock == -1)
	    return;

	if (!ret) {
	    memmove(vcs->input.buffer, vcs->input.buffer + len,
		    vcs->input.offset - len);
	    vcs->input.offset -= len;
	} else {
	    assert(ret > vcs->read_handler_expect);
	    vcs->read_handler_expect = ret;
	}
    }
}

static inline void vnc_write_pending(struct VncClientState *vcs)
{
    struct VncState *vs = vcs->vs;

    if (buffer_empty(&vcs->output)) {
	dprintf("enable write\n");
	vs->ds->set_fd_handler(vcs->csock, NULL, vnc_client_read,
			       vnc_client_write, vcs);
    }
}

static inline void vnc_write_pending_all(struct VncState *vs)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
	if (VCS_ACTIVE(vs->vcs[i]))
	    vnc_write_pending(vs->vcs[i]);
}

static void vnc_write(struct VncClientState *vcs, const void *data, size_t len)
{
    buffer_reserve(&vcs->output, len);

    vnc_write_pending(vcs);

    buffer_append(&vcs->output, data, len);
}

static void vnc_write_s32(struct VncClientState *vcs, int32_t value)
{
    vnc_write_u32(vcs, *(uint32_t *)&value);
}

static void vnc_write_u32(struct VncClientState *vcs, uint32_t value)
{
    uint8_t buf[4];

    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >>  8) & 0xFF;
    buf[3] = value & 0xFF;

    vnc_write(vcs, buf, 4);
}

static void vnc_write_u16(struct VncClientState *vcs, uint16_t value)
{
    uint8_t buf[2];

    buf[0] = (value >> 8) & 0xFF;
    buf[1] = value & 0xFF;

    vnc_write(vcs, buf, 2);
}

static void vnc_write_u8(struct VncClientState *vcs, uint8_t value)
{
    vnc_write(vcs, &value, 1);
}

static void vnc_flush(struct VncClientState *vcs)
{
    if (vcs->output.offset)
	vnc_client_write(vcs);
}

static uint8_t read_u8(uint8_t *data, size_t offset)
{
    return data[offset];
}

static uint16_t read_u16(uint8_t *data, size_t offset)
{
    return ntohs(*(uint16_t *)(data + offset));
}

static uint32_t read_u32(uint8_t *data, size_t offset)
{
    return ntohl(*(uint32_t *)(data + offset));
}

static int32_t read_s32(uint8_t *data, size_t offset)
{
    return (int32_t)read_u32(data, offset);
}

static void client_cut_text_update(VncState *vs, size_t len, char *text)
{
    dprintf("paste clipboard update:\"%s\"\n", text);

    vs->client_cut_text = realloc(vs->client_cut_text, len);
    /*
      KLL 19/8/2011. We need to set the length regardless of whether
      the buffer ptr is null, because this is used instead of a
      nullity check. Eg in client_cut_text...
    */
    vs->client_cut_text_size = len;

    if (vs->client_cut_text == NULL)
	return;

    memcpy(vs->client_cut_text, text, len);
}

static void client_cut_text(VncState *vs)
{
    unsigned int i;

    // KLL otherwise this test will pass on a null pointer...

    for(i=0;i<vs->client_cut_text_size;i++)
	vs->ds->kbd_put_keysym(vs->client_cut_text[i]);
}

static void check_pointer_type_change(struct VncClientState *vcs, int absolute)
{
    struct VncState *vs = vcs->vs;
    if (vcs->has_pointer_type_change && vcs->absolute != absolute) {
	dprintf("pointer type change\n");
	vnc_write_u8(vcs, 0);
	vnc_write_u8(vcs, 0);
	vnc_write_u16(vcs, 1);
	vnc_framebuffer_update(vcs, absolute, 0,
			       vs->ds->width, vs->ds->height, -257);
	vnc_flush(vcs);
    }
    vcs->absolute = absolute;
}

static void pointer_event(struct VncClientState *vcs, int button_mask,
			  int x, int y)
{
    struct VncState *vs = vcs->vs;
    int buttons = 0;
    int dz = 0;

    if (button_mask & 0x01)
	buttons |= MOUSE_EVENT_LBUTTON;
    if (button_mask & 0x02)
	buttons |= MOUSE_EVENT_MBUTTON;
    if (button_mask & 0x04)
	buttons |= MOUSE_EVENT_RBUTTON;
    /* scrolling wheel events */
    if (button_mask & 0x08)
	dz = -1;
    if (button_mask & 0x10)
	dz = 1;

    if (buttons == MOUSE_EVENT_MBUTTON && !dz) {
	client_cut_text(vs);
	return;
    }

    if (vcs->absolute) {
	vs->ds->mouse_event(x * 0x7FFF / vs->ds->width,
			    y * 0x7FFF / vs->ds->height,
			    dz, buttons, vs->ds->mouse_opaque);
    } else if (vcs->has_pointer_type_change) {
	x -= 0x7FFF;
	y -= 0x7FFF;

	vs->ds->mouse_event(x, y, dz, buttons, vs->ds->mouse_opaque);
    } else {
	if (vcs->last_x != -1)
	    vs->ds->mouse_event(x - vcs->last_x,
				y - vcs->last_y,
				dz, buttons,
				vs->ds->mouse_opaque);
	vcs->last_x = x;
	vcs->last_y = y;
    }

    check_pointer_type_change(vcs,
			      vs->ds->mouse_is_absolute(vs->ds->mouse_opaque));
}

static void reset_keys(VncState *vs)
{
    int i;
    for(i = 0; i < 256; i++) {
        if (vs->modifiers_state[i]) {
            if (i & 0x80)
                vs->ds->kbd_put_keycode(0xe0);
            vs->ds->kbd_put_keycode(i | 0x80);
            vs->modifiers_state[i] = 0;
        }
    }
}

static void press_key(VncState *vs, int keysym)
{
    vs->ds->kbd_put_keycode(keysym2scancode(vs->kbd_layout, keysym) & 0x7f);
    vs->ds->kbd_put_keycode(keysym2scancode(vs->kbd_layout, keysym) | 0x80);
}

static void do_key_event(VncState *vs, int down, uint32_t sym)
{
    int keycode;

    keycode = keysym2scancode(vs->kbd_layout, sym & 0xFFFF);

    /* QEMU console switch */
    switch(keycode) {
    case 0x2a:                          /* Left Shift */
    case 0x36:                          /* Right Shift */
    case 0x1d:                          /* Left CTRL */
    case 0x9d:                          /* Right CTRL */
    case 0x38:                          /* Left ALT */
    case 0xb8:                          /* Right ALT */
        if (down)
            vs->modifiers_state[keycode] = 1;
        else
            vs->modifiers_state[keycode] = 0;
        break;
    case 0x02 ... 0x0a: /* '1' to '9' keys */ 
        if (down && vs->modifiers_state[0x1d] && vs->modifiers_state[0x38]) {
            /* Reset the modifiers sent to the current console */
            reset_keys(vs);
            // console_select(keycode - 0x02);
            return;
        }
        break;
    case 0x45:			/* NumLock */
	if (!down)
	    vs->modifiers_state[keycode] ^= 1;
	break;
    }

    if (keycodeIsKeypad(vs->kbd_layout, keycode)) {
        /* If the numlock state needs to change then simulate an additional
           keypress before sending this one.  This will happen if the user
           toggles numlock away from the VNC window.
        */
        if (keysymIsNumlock(vs->kbd_layout, sym & 0xFFFF)) {
	    if (!vs->modifiers_state[0x45]) {
		vs->modifiers_state[0x45] = 1;
		press_key(vs, 0xff7f);
	    }
	} else {
	    if (vs->modifiers_state[0x45]) {
		vs->modifiers_state[0x45] = 0;
		press_key(vs, 0xff7f);
	    }
        }
    }

    if (vs->ds->graphic_mode) {
        if (keycode & 0x80)
            vs->ds->kbd_put_keycode(0xe0);
        if (down)
            vs->ds->kbd_put_keycode(keycode & 0x7f);
        else
            vs->ds->kbd_put_keycode(keycode | 0x80);
    } else {
        /* QEMU console emulation */
        if (down) {
	    int mod = 0;
	    if (vs->modifiers_state[0x1d] || vs->modifiers_state[0x9d])
		mod += QEMU_KEY_MOD_CTRL;
	    if (vs->modifiers_state[0x36] || vs->modifiers_state[0x2a])
		 mod += QEMU_KEY_MOD_SHIFT;

            switch (keycode) {
            case 0x2a:                          /* Left Shift */
            case 0x36:                          /* Right Shift */
            case 0x1d:                          /* Left CTRL */
            case 0x9d:                          /* Right CTRL */
            case 0x38:                          /* Left ALT */
            case 0xb8:                          /* Right ALT */
                return;
            }

            /* When ALT is held down, send an ESC first */
            if (vs->modifiers_state[0x38] || vs->modifiers_state[0xb8])
                vs->ds->kbd_put_keysym('\033');
            switch (keycode) {
            case 0xc8:
                vs->ds->kbd_put_keysym(QEMU_KEY_UP + mod);
                break;
            case 0xd0:
                vs->ds->kbd_put_keysym(QEMU_KEY_DOWN + mod);
                break;
            case 0xcb:
                vs->ds->kbd_put_keysym(QEMU_KEY_LEFT + mod);
                break;
            case 0xcd:
                vs->ds->kbd_put_keysym(QEMU_KEY_RIGHT + mod);
                break;
            case 0xd3:
                vs->ds->kbd_put_keysym(QEMU_KEY_DELETE + mod);
                break;
            case 0xc7:
                vs->ds->kbd_put_keysym(QEMU_KEY_HOME + mod);
                break;
            case 0xcf:
                vs->ds->kbd_put_keysym(QEMU_KEY_END + mod);
                break;
            case 0xc9:
                vs->ds->kbd_put_keysym(QEMU_KEY_PAGEUP + mod);
                break;
            case 0xd1:
                vs->ds->kbd_put_keysym(QEMU_KEY_PAGEDOWN + mod);
                break;
            default:
		if (vs->modifiers_state[0x1d] || vs->modifiers_state[0x9d])
		    sym &= 0x1f;
                vs->ds->kbd_put_keysym(sym);
                break;
            }
        }
    }
}

static void key_event(VncState *vs, int down, uint32_t sym)
{
    if (sym >= 'A' && sym <= 'Z' && vs->ds->graphic_mode)
	sym = sym - 'A' + 'a';
    do_key_event(vs, down, sym);
}

static void scan_event(VncState *vs, int down, uint32_t code)
{

    /* Prefix with 0xe0 if high bit set, except for NumLock key. */
    if (code & 0x80 && code != 0xc5)
	vs->ds->kbd_put_keycode(0xe0);
    if (down)
	vs->ds->kbd_put_keycode(code & 0x7f);
    else
	vs->ds->kbd_put_keycode(code | 0x80);
}

static void framebuffer_set_updated(VncState *vs, int x, int y, int w, int h)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++)
	if (VCS_ACTIVE(vs->vcs[i]))
	    set_bits_in_row(vs, vs->vcs[i]->update_row, x, y, w, h);

    if (!vs->has_update && vs->timer) {
        vs->ds->set_timer(vs->timer, vs->ds->get_clock() + vs->timer_interval);
        vs->has_update = 1;
    }
}

static void framebuffer_update_request(struct VncClientState *vcs,
				       int incremental,
				       int x_position, int y_position,
				       int w, int h)
{
    struct VncState *vs = vcs->vs;
    if (!incremental)
	framebuffer_set_updated(vs, x_position, y_position, w, h);
    /* XXX */
    vs->visible_x = 0;
    vs->visible_y = 0;
    vs->visible_w = vs->ds->width;
    vs->visible_h = vs->ds->height;

    vs->ds->set_timer(vs->timer, vs->ds->get_clock());
}

static void set_encodings(struct VncClientState *vcs, int32_t *encodings,
			  size_t n_encodings)
{
    struct VncState *vs = vcs->vs;
    int i;

    vcs->has_hextile = 0;
    vcs->has_resize = 0;
    vcs->has_pointer_type_change = 0;
    vcs->has_cursor_encoding = 0;
    vcs->absolute = -1;

    for (i = n_encodings - 1; i >= 0; i--) {
	switch (encodings[i]) {
	case 0: /* Raw */
	    vcs->has_hextile = 0;
	    break;
	case 5: /* Hextile */
	    vcs->has_hextile = 1;
	    break;
	case -223: /* DesktopResize */
	    vcs->has_resize = 1;
	    break;
	case -239: /* Cursor Pseud-Encoding */
	    vcs->has_cursor_encoding = 1;
	    break;
        case -254: /* xencenter */
            break;
        case -255: /* vncviewer client */
            vcs->isvncviewer = 1;
            break;
	case -257:
	    vcs->has_pointer_type_change = 1;
	    break;
	default:
	    break;
	}
    }

    check_pointer_type_change(vcs,
			      vs->ds->mouse_is_absolute(vs->ds->mouse_opaque));
}

static void set_pixel_format(struct VncClientState *vcs,
			     int bits_per_pixel, int depth,
			     int big_endian_flag, int true_color_flag,
			     int red_max, int green_max, int blue_max,
			     int red_shift, int green_shift, int blue_shift)
{
    struct VncState *vs = vcs->vs;
    int host_big_endian_flag;

#ifdef WORDS_BIGENDIAN
    host_big_endian_flag = 1;
#else
    host_big_endian_flag = 0;
#endif
    if (!true_color_flag) {
    fail:
	vnc_client_error(vcs);
        return;
    }
    dprintf("pixel format bpp %d depth %d vs depth %d\n", bits_per_pixel,
	    depth, vs->depth);
    if (bits_per_pixel == 32 && 
        host_big_endian_flag == big_endian_flag &&
        red_max == 0xff && green_max == 0xff && blue_max == 0xff &&
        red_shift == 16 && green_shift == 8 && blue_shift == 0 &&
        bits_per_pixel == vs->depth * 8) {
        vcs->write_pixels = vnc_write_pixels_copy;
        vcs->send_hextile_tile = send_hextile_tile_32;
        dprintf("set pixel format bpp %d depth %d copy\n", bits_per_pixel,
		vs->depth);
    } else 
    if (bits_per_pixel == 16 && 
        host_big_endian_flag == big_endian_flag &&
        red_max == 31 && green_max == 63 && blue_max == 31 &&
        red_shift == 11 && green_shift == 5 && blue_shift == 0 &&
        bits_per_pixel == vs->depth * 8) {
        vcs->write_pixels = vnc_write_pixels_copy;
        vcs->send_hextile_tile = send_hextile_tile_16;
        dprintf("set pixel format bpp %d depth %d copy\n", bits_per_pixel,
		vs->depth);
    } else 
    if (bits_per_pixel == 8 && 
        red_max == 7 && green_max == 7 && blue_max == 3 &&
        red_shift == 5 && green_shift == 2 && blue_shift == 0 &&
        bits_per_pixel == vs->depth * 8) {
        vcs->write_pixels = vnc_write_pixels_copy;
        vcs->send_hextile_tile = send_hextile_tile_8;
        dprintf("set pixel format bpp %d depth %d copy\n", bits_per_pixel,
		vs->depth);
    } else 
    {
        /* generic and slower case */
        if (bits_per_pixel != 8 &&
            bits_per_pixel != 16 &&
            bits_per_pixel != 32)
            goto fail;
        if (vcs->vs->depth == 4) {
            vcs->red_shift1 = 16;
            vcs->green_shift1 = 8;
            vcs->blue_shift1 = 0;
            vcs->send_hextile_tile = send_hextile_tile_generic_32;
        } else if (vcs->vs->depth == 2) {
            vcs->red_shift1 = 11;
            vcs->green_shift1 = 5;
            vcs->blue_shift1 = 0;
            vcs->send_hextile_tile = send_hextile_tile_generic_16;
        } else {
            vcs->red_shift1 = 5;
            vcs->green_shift1 = 2;
            vcs->blue_shift1 = 0;
            vcs->send_hextile_tile = send_hextile_tile_generic_8;
        }
        vcs->pix_big_endian = big_endian_flag;
        vcs->write_pixels = vnc_write_pixels_generic;
        dprintf("set pixel format bpp %d depth %d generic\n", bits_per_pixel,
                vs->depth);
    }
    vcs->red_shift = red_shift;
    vcs->red_max = red_max;
    vcs->green_shift = green_shift;
    vcs->green_max = green_max;
    vcs->blue_shift = blue_shift;
    vcs->blue_max = blue_max;
    vcs->pix_bpp = bits_per_pixel / 8;
    vnc_dpy_resize(vs->ds, vs->ds->width, vs->ds->height);

    dprintf("sending cursor %d for pixel format change\n", vcs->csock);
    vcs->vpm.vpm_cursor_update = 1;
    vnc_write_pending(vcs);

    if (vs->ds->hw_invalidate)
	vs->ds->hw_invalidate(vs->ds->hw_opaque);
    if (vs->ds->hw_update)
	vs->ds->hw_update(vs->ds->hw_opaque);
}

static int protocol_client_msg(struct VncClientState *vcs, uint8_t *data,
			       size_t len)
{
    struct VncState *vs = vcs->vs;
    int i;
    uint16_t limit;

    switch (data[0]) {
    case 0:
	if (len == 1)
	    return 20;

	set_pixel_format(vcs, read_u8(data, 4), read_u8(data, 5),
			 read_u8(data, 6), read_u8(data, 7),
			 read_u16(data, 8), read_u16(data, 10),
			 read_u16(data, 12), read_u8(data, 14),
			 read_u8(data, 15), read_u8(data, 16));
	break;
    case 2:
	if (len == 1)
	    return 4;

	if (len == 4) {
	    uint16_t v;
	    v = read_u16(data, 2);
	    if (v)
		return 4 + v * 4;
	}

	limit = read_u16(data, 2);
	for (i = 0; i < limit; i++) {
	    int32_t val = read_s32(data, 4 + (i * 4));
	    memcpy(data + 4 + (i * 4), &val, sizeof(val));
	}

	set_encodings(vcs, (int32_t *)(data + 4), limit);

	/* encodings available, immiedately update the cursor - if supported */
	if (VCS_ACTIVE(vcs)) {
	    dprintf("sending cursor %d for encodings change\n",
		    vcs->csock);
	    vcs->vpm.vpm_cursor_update = 1;
	    vnc_write_pending(vcs);
	}

	break;
    case 3:
	if (len == 1)
	    return 10;

	framebuffer_update_request(vcs, read_u8(data, 1), read_u16(data, 2),
				   read_u16(data, 4), read_u16(data, 6),
				   read_u16(data, 8));
	break;
    case 4:
	if (len == 1)
	    return 8;

	vs->timer_interval = VNC_REFRESH_INTERVAL_BASE;
	vs->ds->set_timer(vs->timer, vs->ds->get_clock() + vs->timer_interval);
	key_event(vs, read_u8(data, 1), read_u32(data, 4));
	break;
    case 5:
	if (len == 1)
	    return 6;

	vs->timer_interval = VNC_REFRESH_INTERVAL_BASE;
	vs->ds->set_timer(vs->timer, vs->ds->get_clock() + vs->timer_interval);
	pointer_event(vcs, read_u8(data, 1), read_u16(data, 2),
		      read_u16(data, 4));
	break;
    case 6:
	if (len == 1)
	    return 8;

	if (len == 8) {
	    uint32_t v;
	    v = read_u32(data, 4);
	    if (v)
		return 8 + v;
	}

	client_cut_text_update(vs, read_u32(data, 4), (char *)(data + 8));
	break;
    case 254: // Special case, sending keyboard scan codes
	if (len == 1)
	    return 8;

	vs->timer_interval = VNC_REFRESH_INTERVAL_BASE;
	vs->ds->set_timer(vs->timer, vs->ds->get_clock() + vs->timer_interval);
	scan_event(vs, read_u8(data, 1), read_u32(data, 4));
	break;
    default:
	dprintf("Msg: %d\n", data[0]);
	vnc_client_error(vcs);
	break;
    }

    vnc_read_when(vcs, protocol_client_msg, 1);
    return 0;
}

static int protocol_client_init(struct VncClientState *vcs, uint8_t *data,
				size_t len)
{
    struct VncState *vs = vcs->vs;
    size_t l;
    char pad[3] = { 0, 0, 0 };

    if (vs->ds->hw_update)
	vs->ds->hw_update(vs->ds->hw_opaque);

    dprintf("client init\n");
    vnc_write_u16(vcs, vs->ds->width);
    vnc_write_u16(vcs, vs->ds->height);

    vnc_write_u8(vcs, vs->depth * 8); /* bits-per-pixel */
    vnc_write_u8(vcs, vs->depth * 8); /* depth */
#ifdef WORDS_BIGENDIAN
    vnc_write_u8(vcs, 1);             /* big-endian-flag */
#else
    vnc_write_u8(vcs, 0);             /* big-endian-flag */
#endif
    vnc_write_u8(vcs, 1);             /* true-color-flag */
    if (vs->depth == 4) {
	vnc_write_u16(vcs, 0xFF);     /* red-max */
	vnc_write_u16(vcs, 0xFF);     /* green-max */
	vnc_write_u16(vcs, 0xFF);     /* blue-max */
	vnc_write_u8(vcs, 16);        /* red-shift */
	vnc_write_u8(vcs, 8);         /* green-shift */
	vnc_write_u8(vcs, 0);         /* blue-shift */
        vcs->send_hextile_tile = send_hextile_tile_32;
    } else if (vs->depth == 2) {
	vnc_write_u16(vcs, 31);       /* red-max */
	vnc_write_u16(vcs, 63);       /* green-max */
	vnc_write_u16(vcs, 31);       /* blue-max */
	vnc_write_u8(vcs, 11);        /* red-shift */
	vnc_write_u8(vcs, 5);         /* green-shift */
	vnc_write_u8(vcs, 0);         /* blue-shift */
        vcs->send_hextile_tile = send_hextile_tile_16;
    } else if (vs->depth == 1) {
        /* XXX: change QEMU pixel 8 bit pixel format to match the VNC one ? */
	vnc_write_u16(vcs, 7);        /* red-max */
	vnc_write_u16(vcs, 7);        /* green-max */
	vnc_write_u16(vcs, 3);        /* blue-max */
	vnc_write_u8(vcs, 5);         /* red-shift */
	vnc_write_u8(vcs, 2);         /* green-shift */
	vnc_write_u8(vcs, 0);         /* blue-shift */
        vcs->send_hextile_tile = send_hextile_tile_8;
    }
    vcs->write_pixels = vnc_write_pixels_copy;

    vnc_write(vcs, pad, 3);           /* padding */

    l = strlen(vs->title); 
    vnc_write_u32(vcs, l);
    vnc_write(vcs, vs->title, l);

    vnc_flush(vcs);

    vnc_read_when(vcs, protocol_client_msg, 1);

    return 0;
}

static int protocol_response(struct VncClientState *vcs,
			     uint8_t *client_response, size_t len)
{
    unsigned char cryptchallenge[AUTHCHALLENGESIZE];
    unsigned char key[8];
    int passwdlen, i, j;

    memcpy(cryptchallenge, challenge, AUTHCHALLENGESIZE);

    /* Calculate the sent challenge */
    passwdlen = strlen(vncpasswd);
    for (i=0; i<8; i++)
	key[i] = i<passwdlen ? vncpasswd[i] : 0;
    deskey(key, EN0);
    for (j = 0; j < AUTHCHALLENGESIZE; j += 8)
	des(cryptchallenge+j, cryptchallenge+j);

    /* Check the actual response */
    if (memcmp(cryptchallenge, client_response, AUTHCHALLENGESIZE) != 0) {
	/* password error */
	vnc_write_u32(vcs, 1);
	vnc_write_u32(vcs, 22);
	vnc_write(vcs, "Authentication failure", 22);
	vnc_flush(vcs);
	fprintf(stderr, "VNC Password error.\n");
	vnc_client_error(vcs);
	return 0;
    }

    dprintf("protocol response\n");
    vnc_write_u32(vcs, 0);
    vnc_flush(vcs);

    vnc_read_when(vcs, protocol_client_init, 1);

    return 0;
}

static int protocol_version(struct VncClientState *vcs, uint8_t *version,
			    size_t len)
{
    extern char vncpasswd[64];
    extern unsigned char challenge[AUTHCHALLENGESIZE];
    char local[13];
    int  support, maj, min;

    memcpy(local, version, 12);
    local[12] = 0;

    /* protocol version check */
    if (sscanf(local, "RFB %03d.%03d\n", &maj, &min) != 2) {
	fprintf(stderr, "Protocol version error.\n");
	vnc_client_error(vcs);
	return 0;
    }

    support = 0;
    if (maj == 3) {
	if (min == 3 || min ==4) {
	    support = 1;
	}
    }

    if (! support) {
	fprintf(stderr, "Client uses unsupported protocol version %d.%d.\n",
		maj, min);
	vnc_client_error(vcs);
	return 0;
    }

    dprintf("authentication\n");
    if (*vncpasswd == '\0') {
	/* AuthType is None */
	vnc_write_u32(vcs, 1);
	vnc_flush(vcs);
	vnc_read_when(vcs, protocol_client_init, 1);
    } else {
	/* AuthType is VncAuth */
	vnc_write_u32(vcs, 2);

	/* Challenge-Responce authentication */
	/* Send Challenge */
	make_challenge(challenge, AUTHCHALLENGESIZE);
	vnc_write(vcs, challenge, AUTHCHALLENGESIZE);
	vnc_flush(vcs);
	vnc_read_when(vcs, protocol_response, AUTHCHALLENGESIZE);
    }

    return 0;
}

static void vnc_listen_read(void *opaque)
{
    VncState *vs = opaque;
    struct VncClientState *vcs;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int new_sock;
    int i;

    new_sock = accept(vs->lsock, (struct sockaddr *)&addr, &addrlen);
    if (new_sock == -1)
	return;

    for (i = 0; i < MAX_CLIENTS; i++)
	if (!VCS_INUSE(vs->vcs[i]))
	    break;

    if (i == MAX_CLIENTS)
	goto fail;

    if (vs->vcs[i] == NULL) {
	vs->vcs[i] = calloc(1, sizeof(struct VncClientState));
	if (vs->vcs[i] == NULL)
	    goto fail;
    }

    free(vs->vcs[i]->update_row);
    vs->vcs[i]->update_row = qemu_mallocz(vs->ds->height * sizeof(vs->vcs[i]->update_row[0]));
    if (!vs->vcs[i]->update_row) {
        free(vs->vcs[i]);
        vs->vcs[i] = NULL;
        goto fail;
    }

    vcs = vs->vcs[i];
    vcs->vs = vs;
    vcs->vpm.vpm_region_updates_last = &vcs->vpm.vpm_region_updates;
    vcs->csock = new_sock;
    vcs->isvncviewer = 0;
    socket_set_nonblock(vcs->csock);
    vs->ds->set_fd_handler(vcs->csock, NULL, vnc_client_read, NULL, vcs);
    vs->ds->set_fd_error_handler(vcs->csock, vnc_client_error);
    dprintf("rfb greeting\n");
    vnc_write(vcs, "RFB 003.003\n", 12);
    vnc_flush(vcs);
    vnc_read_when(vcs, protocol_version, 12);
    vcs->has_resize = 0;
    vcs->has_hextile = 0;
    vcs->last_x = -1;
    vcs->last_y = -1;
    if (vs->depth == 1) {
        vcs->red_max1 = 7;
        vcs->green_max1 = 7;
        vcs->blue_max1 = 3;
    } else if (vs->depth == 2) {
        vcs->red_max1 = 31;
        vcs->green_max1 = 63;
        vcs->blue_max1 = 31;
    } else {
        vcs->red_max1 = 255;
        vcs->green_max1 = 255;
        vcs->blue_max1 = 255;
    }
    framebuffer_set_updated(vs, 0, 0, vs->ds->width, vs->ds->height);
    vnc_timer_init(vs);		/* XXX */
    return;

 fail:
    closesocket(new_sock);
    return;
}

static void vnc_dpy_close_vncviewer_connections(DisplayState *ds)
{
    int i;
    VncState *vs = ds->opaque;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!VCS_INUSE(vs->vcs[i])) continue;
        else if(vs->vcs[i]->isvncviewer == 1) {
            vnc_client_io_error(vs->vcs[i], -1, EINVAL);
        }
    }
}

int vnc_display_init(DisplayState *ds, struct sockaddr *addr,
		     int find_unused, char *title, char *keyboard_layout,
		     unsigned int width, unsigned int height)
{
    struct sockaddr_in *iaddr = NULL;
    int reuse_addr, ret;
    socklen_t addrlen;
    VncState *vs;
    in_port_t port = 0;

    vs = qemu_mallocz(sizeof(VncState));
    if (!vs)
	exit(1);

    memset(vs, 0, sizeof(VncState));

    ds->opaque = vs;

#if 0
    vnc_state = vs;
    vs->display = arg;
#endif

    vs->lsock = -1;
    ds->depth = 8;
    vs->depth = 1;

    vs->ds = ds;

    memset(vs->vcs, 0, sizeof(vs->vcs));

    if (!keyboard_layout)
	keyboard_layout = "en-us";

    vs->kbd_layout_name = strdup(keyboard_layout);
    vs->kbd_layout = init_keyboard_layout(keyboard_layout);
    if (!vs->kbd_layout)
	exit(1);
    vs->modifiers_state[0x45] = 1; /* NumLock on - on boot */

    vs->title = strdup(title ?: "");

    vs->ds->data = NULL;
    vs->ds->dpy_update = vnc_dpy_update;
    vs->ds->dpy_resize = vnc_dpy_resize;
    vs->ds->dpy_refresh = vnc_dpy_refresh;
    vs->ds->dpy_set_server_text = vnc_set_server_text;
    vs->ds->dpy_bell = vnc_send_bell;
    vs->ds->dpy_copy_rect = vnc_dpy_copy_rect;
    vs->ds->dpy_clients_connected = vnc_dpy_clients_connected;
    vs->ds->dpy_close_vncviewer_connections = vnc_dpy_close_vncviewer_connections;

    vnc_dpy_resize(vs->ds, width, height);

#ifndef _WIN32
    if (addr->sa_family == AF_UNIX) {
	addrlen = sizeof(struct sockaddr_un);

	vs->lsock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (vs->lsock == -1) {
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
	    port = ntohs(iaddr->sin_port) + 5900;
	}

	vs->lsock = socket(PF_INET, SOCK_STREAM, 0);
	if (vs->lsock == -1) {
	    fprintf(stderr, "Could not create socket\n");
	    exit(1);
	}

	reuse_addr = 1;
	ret = setsockopt(vs->lsock, SOL_SOCKET, SO_REUSEADDR,
			 (const char *)&reuse_addr, sizeof(reuse_addr));
	if (ret == -1) {
	    fprintf(stderr, "setsockopt() failed\n");
	    exit(1);
	}
    } else {
	fprintf(stderr, "Invalid socket family %x\n", addr->sa_family);
	exit(1);
    }

      ret = fcntl(vs->lsock, F_GETFD, NULL);
      fcntl(vs->lsock, F_SETFD, ret | FD_CLOEXEC);

    do {
	iaddr->sin_port = htons(port);
	ret = bind(vs->lsock, addr, addrlen);
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

    if (listen(vs->lsock, 1) == -1) {
	if (errno == EADDRINUSE && find_unused && addr->sa_family == AF_INET) {
	    close(vs->lsock);
	    port++;
	    goto again;
	}
	fprintf(stderr, "listen() failed\n");
	exit(1);
    }

    ret = vs->ds->set_fd_handler(vs->lsock, NULL, vnc_listen_read, NULL, vs);
    if (ret == -1) {
	exit(1);
    }

    if (addr->sa_family == AF_INET)
	return ntohs(iaddr->sin_port);
    else
	return 0;
}

unsigned int seed;

static int make_challenge(unsigned char *random, int size)
{
 
    set_seed(&seed);
    get_random(size, random);

    return 0;
}

static void set_seed(unsigned int *seedp)
{
    *seedp += (unsigned int)(time(NULL)+getpid()+getpid()*987654+rand());
    srand(*seedp);

    return;
}

static void get_random(int len, unsigned char *buf)
{
    int i;

    for (i=0; i<len; i++)
	buf[i] = (int) (256.0*rand()/(RAND_MAX+1.0));

    return;
}
