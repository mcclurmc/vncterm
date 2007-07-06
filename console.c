/*
 * QEMU graphical console
 * 
 * Copyright (c) 2004 Fabrice Bellard
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
#include "console.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "debug.h"

#define DEFAULT_BACKSCROLL 512
#define MAX_CONSOLES 12

#define QEMU_RGBA(r, g, b, a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define QEMU_RGB(r, g, b) QEMU_RGBA(r, g, b, 0xff)

#define	min(a,b) ((a) < (b) ? (a) : (b))
#define	max(a,b) ((a) > (b) ? (a) : (b))

/* fonts */
#define G0	0
#define G1	1

static int utf8map[256];

typedef struct TextAttributes {
    uint8_t fgcol:4;
    uint8_t bgcol:4;
    uint8_t bold:1;
    uint8_t uline:1;
    uint8_t blink:1;
    uint8_t invers:1;
    uint8_t unvisible:1;
    uint8_t used:1;
    uint8_t font:2;
    uint8_t codec:1; /* utf8 or not */
} TextAttributes;

typedef struct CellAttributes {
    uint8_t highlit:1;
} CellAttributes;

typedef struct TextCell {
    uint8_t ch;
    TextAttributes t_attrib;
    CellAttributes c_attrib;

} TextCell;

#define MAX_ESC_PARAMS 3

enum TTYState {
    TTY_STATE_NORM,
    TTY_STATE_ESC,
    TTY_STATE_CSI
};

struct stream_chunk
{
    int offset;
    int len;
    struct stream_chunk *next;
    char data[];
};

struct chunked_stream {
    int fd;
    void *opaque;
    struct stream_chunk *chunk;
    struct stream_chunk **chunk_tail;
};

void
write_or_chunk(struct chunked_stream *s, uint8_t *buf, int len)
{
    int done;
    struct stream_chunk *chunk;

    while (s->chunk) {
	done = write(s->fd, s->chunk->data + s->chunk->offset,
		     s->chunk->len - s->chunk->offset);
	s->chunk->offset += done;
	if (s->chunk->offset == s->chunk->len) {
	    s->chunk = s->chunk->next;
	    if (s->chunk == NULL)
		s->chunk_tail = &s->chunk;
	} else
	    break;
    }
    if (s->chunk == NULL) {
	done = write(s->fd, buf, len);
	if (done == len)
	    return;
    }
    chunk = malloc(sizeof(struct stream_chunk) + len - done);
    if (chunk == NULL)
	return;			/* XXX raise error */
    chunk->next = NULL;
    chunk->offset = 0;
    chunk->len = len - done;
    memcpy(chunk->data, buf + done, len - done);
    *s->chunk_tail = chunk;
    s->chunk_tail = &chunk->next;
}

struct selection{
    int startx, starty;
    int endx, endy;
};

/* ??? This is mis-named.
   It is used for both text and graphical consoles.  */
struct TextConsole {
    int text_console; /* true if text console */
    DisplayState *ds;
    /* Graphic console state.  */

    int g_width, g_height;
    int width;
    int height;
    int total_height;
    int backscroll_height;
    int x, y;
    int saved_x, saved_y;
    int cursor_visible;
    int y_displayed;
    int y_base;
    int autowrap;
    int wrapped;
    int origin_mode;
    int insert_mode;
    int cursorkey_mode;
    int ansi_mode;
    int top_marg;
    int bot_marg;
    TextAttributes t_attrib_default; /* default text attributes */
    TextAttributes t_attrib; /* currently active text attributes */
    TextAttributes saved_t_attrib; /* currently saved text attributes */
    TextCell *cells;
    CellAttributes c_attrib_default; /* default text attributes */

    enum TTYState state;
    int esc_params[MAX_ESC_PARAMS];
    int nb_esc_params;
    int has_esc_param;
    int has_qmark;

    struct chunked_stream input_stream;

/* first one for current selection, second one for old */
    struct selection selections[2];
    int selecting;

    int mouse_x;
    int mouse_y;

/* unicode bits */
    int unicodeIndex;
    int unicodeData[4];
    int unicodeLength;

#if 0
    /* kbd read handler */
    IOCanRWHandler *fd_can_read; 
    IOReadHandler *fd_read;
    void *fd_opaque;
    /* fifo for key pressed */
    QEMUFIFO out_fifo;
    uint8_t out_fifo_buf[16];
    QEMUTimer *kbd_timer;
#endif
};
typedef struct TextConsole TextConsole;

static TextConsole *active_console;
static TextConsole *consoles[MAX_CONSOLES];
static int nb_consoles = 0;

#define clip_y(s, v) {			\
	if ((s)->v < 0)			\
	    (s)->v = 0;			\
	if ((s)->v >= (s)->height)	\
	    (s)->v = (s)->height - 1;	\
    }

#define clip_x(s, v) {			\
	if ((s)->v < 0)			\
	    (s)->v = 0;			\
	if ((s)->v >= (s)->width)	\
	    (s)->v = (s)->width - 1;	\
    }

#define cy(y) ((s->y_displayed + (y)) % s->total_height)

/* convert a RGBA color to a color index usable in graphic primitives */
static unsigned int vga_get_color(DisplayState *ds, unsigned int rgba)
{
    unsigned int r, g, b, color;

    switch(ds->depth) {
#if 0
    case 8:
        r = (rgba >> 16) & 0xff;
        g = (rgba >> 8) & 0xff;
        b = (rgba) & 0xff;
        color = (rgb_to_index[r] * 6 * 6) + 
            (rgb_to_index[g] * 6) + 
            (rgb_to_index[b]);
        break;
#endif
    case 15:
        r = (rgba >> 16) & 0xff;
        g = (rgba >> 8) & 0xff;
        b = (rgba) & 0xff;
        color = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        break;
    case 16:
        r = (rgba >> 16) & 0xff;
        g = (rgba >> 8) & 0xff;
        b = (rgba) & 0xff;
        color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        break;
    case 32:
    default:
        color = rgba;
        break;
    }
    return color;
}

static void vga_fill_rect (DisplayState *ds, 
                           int posx, int posy, int width, int height, uint32_t color)
{
    uint8_t *d, *d1;
    int x, y, bpp;

    bpp = (ds->depth + 7) >> 3;

    d1 = ds->data + 
        ds->linesize * posy + bpp * posx;
    for (y = 0; y < height; y++) {
        d = d1;
        switch(bpp) {
        case 1:
            memset(d,color,width);
            d+=width;
            break;

        case 2:
            for (x = 0; x < width; x++) {
                *((uint16_t *)d) = color;
                d += bpp;
            }
            break;
        case 4:
            for (x = 0; x < width; x++) {
                *((uint32_t *)d) = color;
                d += bpp;
            }
            break;
        }
        d1 += ds->linesize;
    }
}

/* copy from (xs, ys) to (xd, yd) a rectangle of size (w, h) */
static void vga_bitblt(DisplayState *ds, int xs, int ys, int xd, int yd, int w, int h)
{
    const uint8_t *s;
    uint8_t *d;
    int wb, y, bpp;

    bpp = (ds->depth + 7) >> 3;
    wb = w * bpp;
    if (yd <= ys) {
        s = ds->data + 
            ds->linesize * ys + bpp * xs;
        d = ds->data + 
            ds->linesize * yd + bpp * xd;
        for (y = 0; y < h; y++) {
            memmove(d, s, wb);
            d += ds->linesize;
            s += ds->linesize;
        }
    } else {
        s = ds->data + 
            ds->linesize * (ys + h - 1) + bpp * xs;
        d = ds->data + 
            ds->linesize * (yd + h - 1) + bpp * xd;
       for (y = 0; y < h; y++) {
            memmove(d, s, wb);
            d -= ds->linesize;
            s -= ds->linesize;
        }
    }
}

/***********************************************************/
/* basic char display */

#define FONT_HEIGHT 16
#define FONT_WIDTH 8

#include "vgafont.h"
#include "graphfont.h"

#define cbswap_32(__x) \
((uint32_t)( \
		(((uint32_t)(__x) & (uint32_t)0x000000ffUL) << 24) | \
		(((uint32_t)(__x) & (uint32_t)0x0000ff00UL) <<  8) | \
		(((uint32_t)(__x) & (uint32_t)0x00ff0000UL) >>  8) | \
		(((uint32_t)(__x) & (uint32_t)0xff000000UL) >> 24) ))

#ifdef WORDS_BIGENDIAN
#define PAT(x) x
#else
#define PAT(x) cbswap_32(x)
#endif

static const uint32_t dmask16[16] = {
    PAT(0x00000000),
    PAT(0x000000ff),
    PAT(0x0000ff00),
    PAT(0x0000ffff),
    PAT(0x00ff0000),
    PAT(0x00ff00ff),
    PAT(0x00ffff00),
    PAT(0x00ffffff),
    PAT(0xff000000),
    PAT(0xff0000ff),
    PAT(0xff00ff00),
    PAT(0xff00ffff),
    PAT(0xffff0000),
    PAT(0xffff00ff),
    PAT(0xffffff00),
    PAT(0xffffffff),
};

static const uint32_t dmask4[4] = {
    PAT(0x00000000),
    PAT(0x0000ffff),
    PAT(0xffff0000),
    PAT(0xffffffff),
};

static uint32_t color_table[2][8];

enum color_names {
    COLOR_BLACK   = 0,
    COLOR_RED     = 1,
    COLOR_GREEN   = 2,
    COLOR_BROWN  = 3,
    COLOR_BLUE    = 4,
    COLOR_MAGENTA = 5,
    COLOR_CYAN    = 6,
    COLOR_WHITE   = 7
};

static const uint32_t color_table_rgb[2][8] = {
    {   /* dark */
        QEMU_RGB(0x00, 0x00, 0x00),  /* black */
        QEMU_RGB(0xaa, 0x00, 0x00),  /* red */
        QEMU_RGB(0x00, 0xaa, 0x00),  /* green */
        QEMU_RGB(0xb2, 0x68, 0x18),  /* brown */
        QEMU_RGB(0x00, 0x00, 0xaa),  /* blue */
        QEMU_RGB(0xaa, 0x00, 0xaa),  /* magenta */
        QEMU_RGB(0x00, 0xaa, 0xaa),  /* cyan */
        QEMU_RGB(0xaa, 0xaa, 0xaa),  /* white */
    },
    {   /* bright */
        QEMU_RGB(0x00, 0x00, 0x00),  /* black */
        QEMU_RGB(0xff, 0x00, 0x00),  /* red */
        QEMU_RGB(0x00, 0xff, 0x00),  /* green */
        QEMU_RGB(0xb2, 0x68, 0x18),  /* brown */
        QEMU_RGB(0x00, 0x00, 0xff),  /* blue */
        QEMU_RGB(0xff, 0x00, 0xff),  /* magenta */
        QEMU_RGB(0x00, 0xff, 0xff),  /* cyan */
        QEMU_RGB(0xff, 0xff, 0xff),  /* white */
    }
};

#define UTFVAL(A) (utf8map[A]&0xffff)
/* simplest binary search */
static int get_glyphcode( int utf) {
    int low = 0, high = 255, mid;
    int h=0;

    while(low <= high) {
	h++;
	mid = (low + high) / 2;
	if (UTFVAL(mid) > utf)
	    high = mid - 1;
	else 
	if (UTFVAL(mid) < utf)
	    low = mid + 1;
	else {
	    dprintf("binsearch lookups: %d\n", h);
	    return (utf8map[mid]>>16)&0xff;
        }
    }
    return 0;
}

static inline unsigned int col_expand(DisplayState *ds, unsigned int col)
{
    switch(ds->depth) {
    case 8:
        col |= col << 8;
        col |= col << 16;
        break;
    case 15:
    case 16:
        col |= col << 16;
        break;
    default:
        break;
    }

    return col;
}

static void console_print_text_attributes(TextAttributes *t_attrib, char ch)
{
    if (!do_log)
	return;

    if (t_attrib->bold) {
        dprintf("b");
    } else {
        dprintf(" ");
    }
    if (t_attrib->uline) {
        dprintf("u");
    } else {
        dprintf(" ");
    }
    if (t_attrib->blink) {
        dprintf("l");
    } else {
        dprintf(" ");
    }
    if (t_attrib->invers) {
        dprintf("i");
    } else {
        dprintf(" ");
    }
    if (t_attrib->unvisible) {
        dprintf("n");
    } else {
        dprintf(" ");
    }

    dprintf(" fg: %d bg: %d ch:'%2X' '%c'\n", t_attrib->fgcol, t_attrib->bgcol, ch, ch);
}

static void vga_putcharxy(TextConsole *s, int x, int y, int ch, 
                          TextAttributes *t_attrib, CellAttributes *c_attrib)
{
    uint8_t *d;
    const uint8_t *font_ptr;
    unsigned int font_data, linesize, xorcol, bpp;
    int i;
    unsigned int fgcol, bgcol;
    DisplayState *ds = s->ds;

    dprintf("x: %2i y: %2i", x, y);
    console_print_text_attributes(t_attrib, ch);
    dprintf("font:%d\n", t_attrib->font);

    if (t_attrib->invers ^ c_attrib->highlit ^
	((s->cursor_visible && x == s->x && y == s->y) ))
    {
        bgcol = color_table[0][t_attrib->fgcol];
        fgcol = color_table[t_attrib->bold][t_attrib->bgcol];
    } else {
        fgcol = color_table[t_attrib->bold][t_attrib->fgcol];
        bgcol = color_table[0][t_attrib->bgcol];
    }

    bpp = (ds->depth + 7) >> 3;
    d = ds->data + 
        ds->linesize * y * FONT_HEIGHT + bpp * x * FONT_WIDTH;
    linesize = ds->linesize;

    switch( t_attrib->font ) {
	case G1:
	    font_ptr = graphfont16 + FONT_HEIGHT * ch;
	break;
	case G0:
	default:
	    font_ptr = vgafont16 + FONT_HEIGHT * ch;
	break;
    }

    xorcol = bgcol ^ fgcol;
    switch(ds->depth) {
    case 8:
        for(i = 0; i < FONT_HEIGHT; i++) {
            font_data = *font_ptr++;
            if (t_attrib->uline
                && ((i == FONT_HEIGHT - 2) || (i == FONT_HEIGHT - 3))) {
                font_data = 0xFFFF;
            }
            ((uint32_t *)d)[0] = (dmask16[(font_data >> 4)] & xorcol) ^ bgcol;
            ((uint32_t *)d)[1] = (dmask16[(font_data >> 0) & 0xf] & xorcol) ^ bgcol;
            d += linesize;
        }
        break;
    case 16:
    case 15:
        for(i = 0; i < FONT_HEIGHT; i++) {
            font_data = *font_ptr++;
            if (t_attrib->uline
                && ((i == FONT_HEIGHT - 2) || (i == FONT_HEIGHT - 3))) {
                font_data = 0xFFFF;
            }
            ((uint32_t *)d)[0] = (dmask4[(font_data >> 6)] & xorcol) ^ bgcol;
            ((uint32_t *)d)[1] = (dmask4[(font_data >> 4) & 3] & xorcol) ^ bgcol;
            ((uint32_t *)d)[2] = (dmask4[(font_data >> 2) & 3] & xorcol) ^ bgcol;
            ((uint32_t *)d)[3] = (dmask4[(font_data >> 0) & 3] & xorcol) ^ bgcol;
            d += linesize;
        }
        break;
    case 32:
        for(i = 0; i < FONT_HEIGHT; i++) {
            font_data = *font_ptr++;
            if (t_attrib->uline && ((i == FONT_HEIGHT - 2) || (i == FONT_HEIGHT - 3))) {
                font_data = 0xFFFF;
            }
            ((uint32_t *)d)[0] = (-((font_data >> 7)) & xorcol) ^ bgcol;
            ((uint32_t *)d)[1] = (-((font_data >> 6) & 1) & xorcol) ^ bgcol;
            ((uint32_t *)d)[2] = (-((font_data >> 5) & 1) & xorcol) ^ bgcol;
            ((uint32_t *)d)[3] = (-((font_data >> 4) & 1) & xorcol) ^ bgcol;
            ((uint32_t *)d)[4] = (-((font_data >> 3) & 1) & xorcol) ^ bgcol;
            ((uint32_t *)d)[5] = (-((font_data >> 2) & 1) & xorcol) ^ bgcol;
            ((uint32_t *)d)[6] = (-((font_data >> 1) & 1) & xorcol) ^ bgcol;
            ((uint32_t *)d)[7] = (-((font_data >> 0) & 1) & xorcol) ^ bgcol;
            d += linesize;
        }
        break;
    }
}

static void text_console_resize(TextConsole *s)
{
    TextCell *cells, *c, *c1;
    int w1, x, y, last_width;

    dprintf("text console resize %p\n", s->cells);
    last_width = s->width;
    s->width = s->g_width / FONT_WIDTH;
    s->height = s->g_height / FONT_HEIGHT;
    s->bot_marg = s->height - 1;
    if (s->top_marg >= s->bot_marg)
	s->top_marg = s->bot_marg;

    w1 = last_width;
    if (s->width < w1)
        w1 = s->width;

    cells = qemu_malloc(s->width * s->total_height * sizeof(TextCell));
    memset(cells,0,s->width * s->total_height * sizeof(TextCell));
    for(y = 0; y < s->total_height; y++) {
        c = &cells[y * s->width];
        if (w1 > 0) {
            c1 = &s->cells[y * last_width];
            for(x = 0; x < w1; x++) {
                *c++ = *c1++;
            }
        }
        for(x = w1; x < s->width; x++) {
            c->ch = ' ';
            c->t_attrib = s->t_attrib_default;
            c++;
        }
    }
    qemu_free(s->cells);
    s->cells = cells;
}

static void update_xy(TextConsole *s, int x, int y)
{
    TextCell *c;
    int y1, y2;

    if (s == active_console) {
        y1 = cy(y);
        y2 = y1 - s->y_displayed;
        if (y2 < 0)
            y2 += s->total_height;
        if (y2 < s->height) {
            c = &s->cells[y1 * s->width + x];
            vga_putcharxy(s, x, y2, c->ch, 
                          &(c->t_attrib), &(c->c_attrib));
            s->ds->dpy_update(s->ds, x * FONT_WIDTH, y2 * FONT_HEIGHT, 
			      FONT_WIDTH, FONT_HEIGHT);
        }
    }
}

static void set_cursor(TextConsole *s, int y, int x)
{
    s->y = y;
    s->wrapped = 0;
    s->x = x;
}

static void console_show_cursor(TextConsole *s, int show)
{
    // TextAttributes t_attrib;
    TextCell *c;
    int y, y1;

    s->cursor_visible = show;
    if (s == active_console && s->x < s->width) {
        y1 = cy(s->y);
        y = y1 - s->y_displayed;
        if (y < 0)
            y += s->total_height;
        if (y < s->height) {
            c = &s->cells[y1 * s->width + s->x];
#if 0
            if (show) {
                t_attrib = s->t_attrib_default;
                t_attrib.highlit = !(t_attrib.highlit); /* invert fg and bg */
                vga_putcharxy(s, s->x, y, c->ch, &t_attrib);
            } else {
#endif
                vga_putcharxy(s, s->x, y, c->ch, &(c->t_attrib), &(c->c_attrib));
//            }
            s->ds->dpy_update(s->ds, s->x * FONT_WIDTH, y * FONT_HEIGHT, 
			      FONT_WIDTH, FONT_HEIGHT);
        }
    }
}

static char *
get_text(TextConsole *s, int from_y, int from_x, int to_y, int to_x)
{
    TextCell *c;
    TextAttributes t_attrib;
    int end;
    char *buffer;
    int bufidx = 0;

    if (to_y < from_y || (to_y == from_y && to_x < from_x)) {
	end = from_y * s->width + from_x;
	from_x = to_x;
	from_y = to_y;
    } else
	end = to_y * s->width + to_x;
    dprintf("get_text from %d/%d to %d/%d size %d\n", from_y, from_x, to_y,
	   to_x, (from_y * s->width + from_x) - end);
    buffer = malloc(end - (from_y * s->width + from_x) + s->height + 1);
    if (buffer == NULL)
	return NULL;
    c = &s->cells[cy(from_y) * s->width + from_x];
    while (from_y * s->width + from_x < end) {
	t_attrib = c->t_attrib;
	if (t_attrib.used)
	    buffer[bufidx++] = c->ch;
	c++;
	from_x++;
	if (from_x >= s->width) {
	    from_x = 0;
	    from_y++;
	    buffer[bufidx++] = '\n';
	}
    }
    buffer[bufidx] = 0;
    return buffer;
}

static void
highlight(TextConsole *s, int from_y, int from_x, int to_y, int to_x,
	  int highlight, int used, int persist)
{
    TextCell *c;
    CellAttributes c_attrib;
    int end;

    if (to_y < from_y || (to_y == from_y && to_x < from_x)) {
	end = from_y * s->width + from_x;
	from_x = to_x;
	from_y = to_y;
    } else
	end = to_y * s->width + to_x;

    c = &s->cells[cy(from_y) * s->width + from_x];
    while (from_y * s->width + from_x < end) {
	if ((!used || c->t_attrib.used) && persist)
	    c->c_attrib.highlit = !(c->c_attrib.highlit);
	c_attrib = c->c_attrib;
	if (/* (from_x == s->x && from_y == s->y) || */
	    (highlight && (!used || c->t_attrib.used)))
	    c_attrib.highlit = !(c_attrib.highlit);
	vga_putcharxy(s, from_x, from_y, c->ch, &(c->t_attrib), &c_attrib);
	s->ds->dpy_update(s->ds, from_x * FONT_WIDTH, from_y * FONT_HEIGHT,
			  FONT_WIDTH, FONT_HEIGHT);
	c++;
	from_x++;
	if (from_x >= s->width) {
	    from_x = 0;
	    from_y++;
	}
    }
}
/*
static void
refresh(TextConsole *s, int y, int x)
{
    TextCell *c;

    if (x < 0 || y < 0 || x >= s->width || y >= s->height)
	return;

    c = &s->cells[cy(y) * s->width + x];
    vga_putcharxy(s, x, y, c->ch, &(c->t_attrib), &(c->c_attrib));
    s->ds->dpy_update(s->ds, x * FONT_WIDTH, y * FONT_HEIGHT,
		      FONT_WIDTH, FONT_HEIGHT);
}
*/
int
mouse_is_absolute(void *opaque)
{
    return 1;
}

static void console_refresh(TextConsole *s)
{
    TextCell *c;
    int x, y, y1;

    if (s != active_console) 
        return;

    vga_fill_rect(s->ds, 0, 0, s->ds->width, s->ds->height,
                  color_table[0][COLOR_BLACK]);
    y1 = s->y_displayed;
    for(y = 0; y < s->height; y++) {
        c = &s->cells[y1 * s->width];
        for(x = 0; x < s->width; x++) {
            vga_putcharxy(s, x, y, c->ch, &(c->t_attrib), &(c->c_attrib));
            c++;
        }
        if (++y1 == s->total_height)
            y1 = 0;
    }
    s->ds->dpy_update(s->ds, 0, 0, s->ds->width, s->ds->height);
    console_show_cursor(s, 1);
}

static void console_scroll(int ydelta)
{
    TextConsole *s;
    int i, y1;

    s = active_console;
    if (!s || !s->text_console)
        return;

    if (ydelta > 0) {
        for(i = 0; i < ydelta; i++) {
            if (s->y_displayed == s->y_base)
                break;
            if (++s->y_displayed == s->total_height)
                s->y_displayed = 0;
        }
    } else {
        ydelta = -ydelta;
        i = s->backscroll_height;
        if (i > s->total_height - s->height)
            i = s->total_height - s->height;
        y1 = s->y_base - i;
        if (y1 < 0)
            y1 += s->total_height;
        for(i = 0; i < ydelta; i++) {
            if (s->y_displayed == y1)
                break;
            if (--s->y_displayed < 0)
                s->y_displayed = s->total_height - 1;
        }
    }
    console_refresh(s);
}

static void clear(TextConsole *s, int from_y, int from_x, int to_y, int to_x)
{
    TextCell *c;
    int end;

    end = to_y * s->width + to_x;
    c = &s->cells[cy(from_y) * s->width + from_x];
    while (from_y * s->width + from_x < end) {
	c->ch = ' ';
	c->t_attrib = s->t_attrib_default;
	c->t_attrib.fgcol = s->t_attrib.fgcol;
	c->t_attrib.bgcol = s->t_attrib.bgcol;
	update_xy(s, from_x, from_y);
	c++;
	from_x++;
	if (from_x >= s->width) {
	    from_x = 0;
	    from_y++;
	}
    }
}

#define zero_selection(A,B) {memset( &A->selections[B],0,sizeof(struct selection) );}
#define is_selection_zero(A,B) ((A->selections[B].startx | \
				A->selections[B].starty | \
				A->selections[B].endx | \
				A->selections[B].endy ) == 0 )
void
mouse_event(int dx, int dy, int dz, int buttons_state, void *opaque)
{
    CharDriverState *chr = opaque;
    TextConsole *s = chr->opaque;
    char *text;

    dprintf("mouse event %03x:%03x:%x:%x\n", dx, dy, dz, buttons_state);
    dx = dx * s->width / 0x7FFF;
    dy = dy * s->height / 0x7FFF;

/* boundry check & fix */
    if (dy>s->height-1)
	dy=s->height-1;

    if (dx>s->width-1)
	dx=s->width-1;

    if (dy<0) 
	dy=0;

    if (dx<0)
	dx=0;

    if (dz == -1)
	console_scroll(-1);
    if (dz == 1)
	console_scroll(1);

    s->mouse_x = dx;
    s->mouse_y = dy;

    /* button not pressed */
    if (buttons_state == 0) {

        /* if button was pressed before, means we have to grab selected text
           end send it to peer's clipboard */
	if (s->selecting) {
	    text = get_text(s, s->selections[0].starty, s->selections[0].startx,
			   s->selections[0].endy, s->selections[0].endx);

	    if ( strlen(text) )
		s->ds->dpy_set_server_text(s->ds, text);

	    /* set flag, copy current selection to old one */
	    s->selecting = 0;
	    memcpy( &s->selections[1], &s->selections[0], sizeof(struct selection) );
	}
    } else if (buttons_state == 1) {
        /* button pressed, no selection made - we have to initialze selection */
	if (s->selecting == 0 ){
	    /* if previous highlight is still displayed, 
               we have to cancel it */
	    if ( !is_selection_zero(s, 1) )
		highlight(s, s->selections[1].starty, s->selections[1].startx,
			   s->selections[1].endy, s->selections[1].endx, 0, 1, 1);
	    zero_selection(s, 1);
	    /* initialize current coordinates */
	    s->selections[0].startx = dx;
	    s->selections[0].starty = dy;
	    s->selections[0].endx = dx;
	    s->selections[0].endy = dy;
	    s->selecting=1;
	    /* highlite current character */
	    highlight(s, dy, dx, dy, dx, 0, 1, 1);
	}
	else {
	    /* in this case, we just have to update selection */
	    highlight(s, s->selections[0].endy, s->selections[0].endx, dy, dx, 0, 1, 1);
	    s->selections[0].endx = dx;
	    s->selections[0].endy = dy;
	}
    }
}

static void scroll_up(TextConsole *s, int top, int bot, int n)
{
    int y1;

    dprintf("scroll up %d->%d by %d\n", top, bot, n);
    bot++;
    if (top == 0 && bot == s->height) {
	if (s->y_displayed == s->y_base) {
	    s->y_displayed += n;
	    if (s->y_displayed >= s->total_height)
		s->y_displayed -= s->total_height;
	}
	s->y_base += n;
	if (s->y_base >= s->total_height)
	    s->y_base -= s->total_height;
	if (s->backscroll_height < s->total_height) {
	    s->backscroll_height += n;
	    if (s->backscroll_height >= s->total_height)
		s->backscroll_height = s->total_height;
	}
	dprintf("fake scroll disp %d base %d cells %p-%p\n", s->y_displayed,
	       s->y_base, &s->cells[cy(top) * s->width], &s->cells[cy(bot) * s->width]);
    } else {
	for (y1 = top + n; y1 < bot; y1++)
	    memmove(&s->cells[cy(y1 - n) * s->width],
		    &s->cells[cy(y1) * s->width],
		    s->width * sizeof(TextCell));
    }
    if (s == active_console && s->y_displayed == s->y_base)
	vga_bitblt(s->ds, 0, (top + n) * FONT_HEIGHT,
		   0, top * FONT_HEIGHT, 
		   s->width * FONT_WIDTH, 
		   (bot - top - n) * FONT_HEIGHT);
    clear(s, bot - n, 0, bot - 1, s->width);
    if (s == active_console && s->y_displayed == s->y_base) {
	vga_fill_rect(s->ds, 0, (bot - n) * FONT_HEIGHT,
		      s->width * FONT_WIDTH, n * FONT_HEIGHT, 
		      color_table[0][s->t_attrib_default.bgcol]);
	s->ds->dpy_update(s->ds, 0, top * FONT_HEIGHT, 
			  s->width * FONT_WIDTH,
			  bot * FONT_HEIGHT);
    }
}

static void scroll_down(TextConsole *s, int top, int bot, int n)
{
    int y1;
    int n_orig;

    bot++;
    n_orig = n;
 again:
    if (top == 0 && bot == s->height && s->backscroll_height) {
	if (s->backscroll_height < n)
	    n = s->backscroll_height;
	if (s->y_displayed == s->y_base) {
	    s->y_displayed -= n;
	    if (s->y_displayed < 0)
		s->y_displayed = s->total_height - s->y_displayed;
	}
	s->y_base -= n;
	if (s->y_base < 0)
	    s->y_base = s->total_height - s->y_base;
	s->backscroll_height -= n;
    } else {
	for (y1 = bot - 1; y1 >= top + n; y1--)
	    memmove(&s->cells[cy(y1) * s->width],
		    &s->cells[cy(y1 - n) * s->width],
		    s->width * sizeof(TextCell));
    }
    if (s == active_console && s->y_displayed == s->y_base)
	vga_bitblt(s->ds, 0, top * FONT_HEIGHT,
		   0, (top + n) * FONT_HEIGHT, 
		   s->width * FONT_WIDTH, 
		   (bot - top - n) * FONT_HEIGHT);
    clear(s, top, 0, top + n - 1, s->width);
    if (s == active_console && s->y_displayed == s->y_base) {
	vga_fill_rect(s->ds, 0, top * FONT_HEIGHT,
		      s->width * FONT_WIDTH, n * FONT_HEIGHT, 
		      color_table[0][s->t_attrib_default.bgcol]);
	s->ds->dpy_update(s->ds, 0, top * FONT_HEIGHT, 
			  s->width * FONT_WIDTH,
			  bot * FONT_HEIGHT);
    }
    if (n != n_orig) {
	n = n_orig - n;
	goto again;
    }
}

static void va_write(TextConsole *s, char *f, ...)
{
    va_list ap;
    char *str;

    va_start(ap, f);

    vasprintf(&str, f, ap);
    if (str)
	write_or_chunk(&s->input_stream, (uint8_t *)str, strlen(str));
    free(str);

    va_end(ap);
}

static void console_put_lf(TextConsole *s)
{
    set_cursor(s, s->y + 1, s->x);
    if (s->y > s->bot_marg) {
        set_cursor(s, s->bot_marg, s->x);
	scroll_up(s, s->top_marg, s->bot_marg, 1);
    }
}

static void console_put_cr(TextConsole *s)
{
    set_cursor(s, s->y, 0);
}

static void console_put_ri(TextConsole *s)
{
    set_cursor(s, s->y - 1, 0);
    if (s->y < s->top_marg) {
	set_cursor(s, s->top_marg, s->x);;
	scroll_down(s, s->top_marg, s->bot_marg, 1);
    }
}

#if !defined(__APPLE__)
/* Set console attributes depending on the current escape codes.
 * NOTE: I know this code is not very efficient (checking every color for it
 * self) but it is more readable and better maintainable.
 */
static void console_handle_escape(TextConsole *s)
{
    int i;

    dprintf("handle escape %d\n", s->nb_esc_params);
    if (s->nb_esc_params == 0) { /* ESC[m sets all attributes to default */
        s->t_attrib = s->t_attrib_default;
        return;
    }
    for (i=0; i<s->nb_esc_params; i++) {
	dprintf("putchar escape param %d\n", s->esc_params[i]);
        switch (s->esc_params[i]) {
            case 0: /* reset all console attributes to default */
                s->t_attrib = s->t_attrib_default;
                break;
            case 1:
                s->t_attrib.bold = 1;
                break;
            case 4:
                s->t_attrib.uline = 1;
                break;
            case 5:
                s->t_attrib.blink = 1;
                break;
            case 7:
                s->t_attrib.invers = 1;
                break;
            case 8:
                s->t_attrib.unvisible = 1;
                break;
/*
10  reset selected mapping, display control flag,
  and toggle meta flag.
  11  select null mapping, set display control flag,
  reset toggle meta flag.
  12  select null mapping, set display control flag,
  set toggle meta flag. (The toggle meta flag
  causes the high bit of a byte to be toggled
  before the mapping table translation is done.)
  21  set normal intensity (this is not compatible with ECMA-48)
*/
            case 22:
                s->t_attrib.bold = 0;
                break;
            case 24:
                s->t_attrib.uline = 0;
                break;
            case 25:
                s->t_attrib.blink = 0;
                break;
            case 27:
                s->t_attrib.invers = 0;
                break;
            case 28:
                s->t_attrib.unvisible = 0;
                break;
            /* set foreground color */
            case 30:
                s->t_attrib.fgcol=COLOR_BLACK;
                break;
            case 31:
                s->t_attrib.fgcol=COLOR_RED;
                break;
            case 32:
                s->t_attrib.fgcol=COLOR_GREEN;
                break;
            case 33:
                s->t_attrib.fgcol=COLOR_BROWN;
                break;
            case 34:
                s->t_attrib.fgcol=COLOR_BLUE;
                break;
            case 35:
                s->t_attrib.fgcol=COLOR_MAGENTA;
                break;
            case 36:
                s->t_attrib.fgcol=COLOR_CYAN;
                break;
            case 37:
                s->t_attrib.fgcol=COLOR_WHITE;
                break;
	    case 38:
		/* set to default foreground, underscore on */
		s->t_attrib.fgcol=s->t_attrib_default.fgcol;
		s->t_attrib.uline = 1;
		break;
	    case 39:
		/* set to default foreground, underscore off */
		s->t_attrib.fgcol=s->t_attrib_default.fgcol;
		s->t_attrib.uline = 0;
		break;
            /* set background color */
            case 40:
                s->t_attrib.bgcol=COLOR_BLACK;
                break;
            case 41:
                s->t_attrib.bgcol=COLOR_RED;
                break;
            case 42:
                s->t_attrib.bgcol=COLOR_GREEN;
                break;
            case 43:
                s->t_attrib.bgcol=COLOR_BROWN;
                break;
            case 44:
                s->t_attrib.bgcol=COLOR_BLUE;
                break;
            case 45:
                s->t_attrib.bgcol=COLOR_MAGENTA;
                break;
            case 46:
                s->t_attrib.bgcol=COLOR_CYAN;
                break;
            case 47:
                s->t_attrib.bgcol=COLOR_WHITE;
                break;
	    case 48:
		/* TODO: implement - set collors acording to x,y cursor position */
		break;
	    case 49:
		/* set to default */
		s->t_attrib.bgcol=s->t_attrib_default.bgcol;
		break;
        }
    }
}
#endif

char normbuf[1024];
int normidx = 0;
static void print_norm(void)
{
    if (normidx) {
	normbuf[normidx] = 0;
	dprintf("norm %s\n", normbuf);
	normidx = 0;
    }
}
static void put_norm(char ch)
{
    normbuf[normidx++] = ch;
    if (normidx == 1024)
	print_norm();
}

static void do_putchar(TextConsole *s, int ch)
{
    TextCell *c;

    put_norm(ch);
    if (s->wrapped) {
	set_cursor(s, s->y, 0);
	console_put_lf(s);
    }
    c = &s->cells[cy(s->y) * s->width + s->x];
    c->ch = ch;
    c->t_attrib = s->t_attrib;
    c->t_attrib.used = 1;
    update_xy(s, s->x, s->y);
    if (s->x + 1 < s->width)
	set_cursor(s, s->y, s->x + 1);
    else 
	if (s->autowrap)
	    s->wrapped = 1;

}

static int handle_params(TextConsole *s, int ch)
{
    int i;

    dprintf("putchar csi %c %02x\n", ch > 0x1f ? ch : ' ', ch);
    if (ch >= '0' && ch <= '9') {
	if (s->nb_esc_params < MAX_ESC_PARAMS) {
	    s->esc_params[s->nb_esc_params] = 
		s->esc_params[s->nb_esc_params] * 10 + ch - '0';
	}
	s->has_esc_param = 1;
	return 0;
    } else {
	if (s->has_esc_param)
	    s->nb_esc_params++;
	s->has_esc_param = 0;
	if (ch == '?') {
	    s->has_qmark = 1;
	    return 0;
	}
	if (ch == ';')
	    return 0;
	dprintf("csi %x[%c] with args", ch,
		ch > 0x1f ? ch : ' ');
	if (s->has_qmark)
	    dprintf(" ?");
	for (i = 0; i < s->nb_esc_params; i++)
	    dprintf(" 0x%0x/%d", s->esc_params[i], s->esc_params[i]);
	dprintf("\n");
    }
    return 1;
}

static void reset_params(TextConsole *s)
{
    int i;

    for(i=0;i<MAX_ESC_PARAMS;i++)
	s->esc_params[i] = 0;
    s->nb_esc_params = 0;
    s->has_qmark = 0;
}

static void console_putchar(TextConsole *s, int ch)
{
    TextCell *c, *d;
    int y1, i, x, x1, a;
    int x_, y_, och;

    dprintf("putchar %d '%c' state:%d \n", ch, ch, s->state);

    switch(s->state) {
    case TTY_STATE_NORM:
	dprintf("putchar norm %c %02x\n", ch > 0x1f ? ch : ' ', ch);
        switch(ch) {
        case BEL:
	    s->ds->dpy_bell(s->ds);
            break;
        case BS:
            if (s->x > 0) 
                set_cursor(s, s->y, s->x - 1);
            break;
        case HT:
            if (s->x + (8 - (s->x % 8)) > s->width) {
                set_cursor(s, s->y, 0);
                console_put_lf(s);
            } else {
                set_cursor(s, s->y, s->x + (8 - (s->x % 8)));
            }
            break;
        case LF:
        case VT:
        case FF:
            console_put_lf(s);
            break;
        case CR:
            set_cursor(s, s->y, 0);
            break;
        case SO:
	    dprintf("SO G1 switch");
	    s->t_attrib.font = G1;
            break;
        case SI:
            dprintf("SI G0 switch");
	    s->t_attrib.font = G0;
            break;
        case CAN:
	case ESN:
            dprintf("not implemented CAN");
            break;
        case ESC:
            print_norm();
	    reset_params(s);
            s->state = TTY_STATE_ESC;
            break;
        case DEL: /* according to term=linux 'standard' should be ignored.*/
            break;
        case CSI:
            print_norm();
	    reset_params(s);
            s->state = TTY_STATE_CSI;
            break;

        default:
/* utf 8 bit */
	    if (s->t_attrib.codec) {
		if (s->unicodeIndex > 0) {
		    if ((ch & 0xc0) != 0x80) {
			dprintf("bogus unicode data %u\n", ch);
			/* even tho we think it might be bogus, still print it */
			do_putchar(s, ch);
		    }
		    s->unicodeData[s->unicodeIndex++] = ch;
		    if (s->unicodeIndex < s->unicodeLength) {
			return;
		    }
		    switch (s->unicodeLength) {
			case 2:
			    ch = ((s->unicodeData[0] & 0x1f) << 6) |
				(s->unicodeData[1] & 0x3f);
			    break;
			case 3:
			    ch = ((s->unicodeData[0] & 0x0f) << 12) |
				((s->unicodeData[1] & 0x3f) << 6) |
				(s->unicodeData[2] & 0x3f); 
			    break;
			case 4:
 			    ch = ((s->unicodeData[0] & 0x07) << 18) |
				((s->unicodeData[1] & 0x3f) << 12) |
				((s->unicodeData[2] & 0x3f) << 6) |
				(s->unicodeData[3] & 0x3f);
			    break;
			default:
			    dprintf("bogus unicode length %u\n", s->unicodeLength);
			break;
		    }
		    /* get it from lookup table, cp437_to_uni.trans */
		    och=ch;
		    ch = get_glyphcode(ch);
		    s->unicodeIndex = 0;
		    dprintf("utf8: %x to: %x\n", och, ch);	
		} 
		else {
		    if ((ch & 0xe0) == 0xc0) {
			s->unicodeData[0] = ch;
			s->unicodeIndex = 1;
			s->unicodeLength = 2;
			return;
		    } 
		    else
		    if ((ch & 0xf0) == 0xe0) {
			s->unicodeData[0] = ch;
			s->unicodeIndex = 1;
			s->unicodeLength = 3;
			return;
		    } 
		    else
		    if ((ch & 0xf8) == 0xf0) {
			s->unicodeData[0] = ch;
			s->unicodeIndex = 1;
			s->unicodeLength = 4;
			return;
		    }
		}
	    }

/* end of utf 8 bit */
	    do_putchar(s, ch);
            break;
        }
        break;
    case TTY_STATE_ESC: /* check if it is a terminal escape sequence */
	if (ch != '[')
	    dprintf("putchar esc %c %02x\n", ch > 0x1f ? ch : ' ', ch);
	s->state = TTY_STATE_NORM;
	switch (ch) {
	case ']': /* Operating system command */
	    break;
	case '>': /* Set numeric keypad mode */
	    break;
	case '=': /* Set application keypad mode */
	    break;
	case 'c': /* reset */
	    set_cursor(s, 0, 0);
	    s->nb_esc_params = 0;
            s->t_attrib = s->t_attrib_default;
	    clear(s, s->y, s->x, s->height - 1, s->width);
	    break;
	case 'D': /* linefeed */
	    console_put_lf(s);
	    break;
	case 'H': /* Set tab stop at current column.*/

	    break;
	case 'Z': /* DEC private identification */
	    va_write(s, "\033[?1;2C");
	    break;
	/* charset selection */
	case '%':
	    if ( s->nb_esc_params < 1 ) {
		break;
	    }
	    switch(s->esc_params[0]) {
		/* set default G0 */
		case '@':
		    s->t_attrib.codec = 0;
		    break;
		/* utf8 */
		case 'G':
		case '8':
		    s->t_attrib.codec = 1;
		    break;
	    }
	    break;
	case '(': /* G0 charset */
	    s->t_attrib.font = G0;
	    break;
	case ')': /* G1 charset */
	    s->t_attrib.font = G1;
	    break;
/*
	case '+':
	case '*':
	case '$':
	    break;
*/
	case '[': /* CSI */
	    reset_params(s);
            s->state = TTY_STATE_CSI;
	    break;
	case 'E': /* new line */
	    console_put_lf(s);
	    console_put_cr(s);
	    break;
	case 'M': /* reverse linefeed */
	    console_put_ri(s);
	    break;
	case '7': /* save current state */
	    s->saved_x = s->x;
	    s->saved_y = s->y;
	    s->saved_t_attrib = s->t_attrib;
	    break;
	case '8': /* restore current state */
	    set_cursor(s, s->saved_y, s->saved_x);
	    s->t_attrib = s->saved_t_attrib;
	    break;
        }
        break;
    case TTY_STATE_CSI: /* handle escape sequence parameters */
	if (handle_params(s, ch)) {
           s->state = TTY_STATE_NORM;
            switch(ch) {
	    case '@': /* ins del characters */
		y1 = cy(s->y);
		c = &s->cells[y1 * s->width + s->width - 1];
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		d = &s->cells[y1 * s->width + s->width - 1 - a];
                for(x = s->width - 1; x >= s->x + a; x--) {
                    c->ch = d->ch;
                    c->t_attrib = d->t_attrib;
                    c--;
		    d--;
                    update_xy(s, x, s->y);
                }
                break;
	    case 'A': /* cursor up */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		set_cursor(s, s->y - a, s->x);
		if (s->y < s->top_marg)
		    set_cursor(s, s->top_marg, s->x);
		break;
	    case 'B': /* cursor down */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		set_cursor(s, s->y + a, s->x);
		if (s->y > s->bot_marg)
		    set_cursor(s, s->bot_marg, s->x);
		break;
	    case 'a':
            case 'C': /* cursor right */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		set_cursor(s, s->y, s->x + a);
		if (s->x >= s->width)
		    set_cursor(s, s->y, s->width - 1);
                break;
            case 'D': /* cursor left */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		set_cursor(s, s->y, s->x - a);
		if (s->x < 0)
		    set_cursor(s, s->y, 0);
                break;
	    case 'E': /* cursor down and to first column */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		set_cursor(s, s->y + a, 0);
		if (s->y > s->bot_marg)
		    set_cursor(s, s->bot_marg, 0);
		break;
	    case 'F': /* cursor up and to first column */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		set_cursor(s, s->y - a, 0);;
		if (s->y < s->top_marg)
		    set_cursor(s, s->top_marg, 0);
		break;
	    case '`': /* fallthrough */
	    case 'G':
		if (s->nb_esc_params == 1) {
		    set_cursor(s, s->y, max(s->esc_params[0] - 1, 0));
		}
		break;
	    case 'f':
	    case 'H': /* cursor possition */
		x_ = y_ = 0;
		switch(s->nb_esc_params) {
			case 2 ... 99: /* two or more */
				y_ = s->esc_params[1] ? s->esc_params[1]-1 : 0;
			case 1:
				x_ = s->esc_params[0] ? s->esc_params[0]-1 : 0;
			break;
		}
		set_cursor(s, x_ + (s->origin_mode ? s->top_marg : 0), y_);
		clip_y(s, y);
		clip_x(s, x);
		dprintf("cursor pos %d:%d\n", s->y, s->x);
		break;
	    case 'J': /* eraseInDisplay */
		if (s->nb_esc_params == 1 && s->esc_params[0] == 2) {
		    set_cursor(s, 0, 0);
		    s->nb_esc_params = 0;
		}
		if (s->nb_esc_params == 0 ||
		    (s->nb_esc_params == 1 && s->esc_params[0] == 0)) {
		    clear(s, s->y, s->x, s->height - 1, s->width);
		} else if (s->nb_esc_params == 1 && s->esc_params[0] == 1) {
		    clear(s, 0, 0, s->y, s->x);
		}
		break;
            case 'K':
		if (s->nb_esc_params == 0) {
		    s->esc_params[0] = 0;
		    s->nb_esc_params = 1;
		}
		if (s->nb_esc_params == 1) {
		    x = 0;
		    x1 = s->width;
		    if (s->esc_params[0] == 0)
			x = s->x;
		    else if (s->esc_params[0] == 1)
			x1 = s->x + 1;
		    dprintf("clear line %d %d->%d\n", s->y, x, x1 - 1);
		    clear(s, s->y, x, s->y, x1);
                }
                break;
	    case 'L':
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		scroll_down(s, s->y, s->bot_marg, s->esc_params[0]);
		break;
	    case 'M':
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		scroll_up(s, s->y, s->bot_marg, s->esc_params[0]);
		break;
	    case 'P':		/* DCH */
		y1 = cy(s->y);
		c = &s->cells[y1 * s->width + s->x];
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		d = &s->cells[y1 * s->width + s->x + a];
                for(x = s->x; x <= s->width - a; x++) {
                    c->ch = d->ch;
                    c->t_attrib = d->t_attrib;
                    c++;
		    d++;
                    update_xy(s, x, s->y);
                }
                break;
            case 'X':
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		clear(s, s->y, s->x, s->y, s->x + s->esc_params[0]);
                break;
	    case 'c': /* device attributes */
		if (s->nb_esc_params == 0 ||
		    (s->nb_esc_params == 1 && s->esc_params[0] == 0)) {
		    if ( !s->has_qmark ) {
			if (s->ansi_mode)
			    va_write(s, "\033[?1;2c"); // I'm a VT100 ansi
			else
			    va_write(s, "\033[?62;1;2c"); // I'm a VT220
		    }
		}
		break;
	    case 'd':
		if (s->nb_esc_params == 1) {
		    set_cursor(s, s->esc_params[0] - 1, s->x);
		}
		break;
	    case 'e':
		if (s->nb_esc_params == 1) {
		    if (s->esc_params[0] == 0)
			s->esc_params[0] = 1;
		    set_cursor(s, s->y + s->esc_params[0] - 1, s->x);
		    if (s->y > s->bot_marg)
			set_cursor(s, s->bot_marg, s->x);
		}
		break;
	    case 'm':
#if !defined(__APPLE__)
		    console_handle_escape(s);
#endif
		break;
	    case 'h': /* reset mode */
	    case 'l': /* set mode */
		a = (ch == 'h') ? 1 : 0;
		if (s->has_qmark) {
		    for (i = 0; i < s->nb_esc_params; i++) {
			switch (s->esc_params[i]) {
			case 1:
			    s->cursorkey_mode = a;
			    break;
			case 2:
			    s->ansi_mode = a;
			    break;
			case 3: // I
			    // s->column_mode = a;
			    break;
			case 4:
			    // s->scrolling_mode = a;
			    break;
			case 5:
			    // s->screen_mode = a;
			    break;
			case 6:
			    s->origin_mode = a;
			    break;
			case 7:
			    s->autowrap = a;
			    break;
			case 8:
			    // s->autorepeat_mode = a;
			    break;
			case 9:
			    // s->interlace_mode = a;
			    break;

			case 20: // I
			    // s->line_mode = a;
			    break;

			case 25:
			    // s->cursorvisible_mode = a;
			    break;
			case 1000:
			    // s->mousereporting_mode = a;
			    break;
			}
		    }
		} else if (s->nb_esc_params >= 1) {
		    switch (s->esc_params[0]) {
		    case 3:
			// s->displaycontrol = a;
			break;
		    case 4:
			s->insert_mode = a;
			break;
		    case 20:
			// s->line_mode = a;
			break;
		    }
		}
		break;
	    case 'n':
		if (s->nb_esc_params == 1) {
		    switch (s->esc_params[0]) {
		    case 6:	/* CPR */
			va_write(s, "%c[%d;%dR", 0x1b, s->y + 1, s->x + 1);
			break;
		    }
		}
	    case 'r':
		if (s->nb_esc_params == 0) {
		    s->top_marg = 0;
		    s->bot_marg = s->height - 1;
		} else if (s->nb_esc_params == 2) {
		    s->top_marg = s->esc_params[0] - 1;
		    clip_y(s, top_marg);
		    s->bot_marg = s->esc_params[1] - 1;
		    clip_y(s, bot_marg);
		}
		set_cursor(s, s->top_marg, 0);
		break;
	    case 's':
		s->saved_x = s->x;
		s->saved_y = s->y;
		break;
	    case 'u':
		set_cursor(s, s->saved_y, s->saved_x);
		break;
	    case 'q':
		dprintf("led toggle\n");
		break;
	    case 'x':
		/* request terminal parametrs */
		/*	report
			no parity set
			8 bits per character
			19200 transmit
			19200 receive
			bit rate multiplier is 16
			switch values are all 0 */
		    va_write(s, "\033[2;1;1;120;120;1;0x");
		break;
            default:
		dprintf("unknown command %x[%c] with args", ch,
		       ch > 0x1f ? ch : ' ');
		for (i = 0; i < s->nb_esc_params; i++)
		    dprintf(" %0x/%d", s->esc_params[i], s->esc_params[i]);
		dprintf("\n");
                break;
            }
            break;
        }
    }
}

void console_select(unsigned int index)
{
    TextConsole *s;

    if (index >= MAX_CONSOLES)
        return;
    s = consoles[index];
    if (s) {
        active_console = s;
        if (s->text_console) {
            if (s->g_width != s->ds->width ||
                s->g_height != s->ds->height) {
                s->g_width = s->ds->width;
                s->g_height = s->ds->height;
                text_console_resize(s);
            }
            console_refresh(s);
        } else {
            s->ds->hw_invalidate(s->ds->hw_opaque);
        }
    }
}

static int console_puts(CharDriverState *chr, const uint8_t *buf, int len)
{
    TextConsole *s = chr->opaque;
    int i;

    console_show_cursor(s, 0);
    for(i = 0; i < len; i++) {
        console_putchar(s, buf[i]);
    }
    console_show_cursor(s, 1);
    return len;
}

#if 0
static void console_chr_add_read_handler(CharDriverState *chr, 
                                         IOCanRWHandler *fd_can_read, 
                                         IOReadHandler *fd_read, void *opaque)
{
    TextConsole *s = chr->opaque;
    s->fd_can_read = fd_can_read;
    s->fd_read = fd_read;
    s->fd_opaque = opaque;
}

static void console_send_event(CharDriverState *chr, int event)
{
    TextConsole *s = chr->opaque;
    int i;

    if (event == CHR_EVENT_FOCUS) {
        for(i = 0; i < nb_consoles; i++) {
            if (consoles[i] == s) {
                console_select(i);
                break;
            }
        }
    }
}

static void kbd_send_chars(void *opaque)
{
    TextConsole *s = opaque;
    int len;
    uint8_t buf[16];
    
    len = s->fd_can_read(s->fd_opaque);
    if (len > s->out_fifo.count)
        len = s->out_fifo.count;
    if (len > 0) {
        if (len > sizeof(buf))
            len = sizeof(buf);
        qemu_fifo_read(&s->out_fifo, buf, len);
        s->fd_read(s->fd_opaque, buf, len);
    }
    /* characters are pending: we send them a bit later (XXX:
       horrible, should change char device API) */
    if (s->out_fifo.count > 0) {
        qemu_mod_timer(s->kbd_timer, qemu_get_clock(rt_clock) + 1);
    }
}
#endif

static 
int cmputfents(const void *p1, const void *p2)
{
    short a,b;

    a=*(short*)p1;
    b=*(short*)p2;

    return a-b;
}

static void parse_unicode_map( char* filename )
{
    FILE* f;
    int ch, n[2], number, position, entry;

    memset(utf8map, 0, sizeof(int)*256);

    f=fopen(filename, "rb");
    if (f==NULL)
	return;

    /* the file structure is simple: 
	X\tY.*
	.....
	we only care about X and Y
    */

    n[0]=n[1]=position=number=entry=0;

    do{
	ch=fgetc(f);
	if (ch==EOF || ch=='\n' || ch=='\r') {
		utf8map[entry++]=(n[1]&0xffff)|(n[0]<<16);
		dprintf("utf8 map: %x %x\n", n[0], n[1] );
		n[0]=n[1]=position=number=0;
	}
	else {
		if (ch=='\t') {
		    number++;
		    position=0;
		}
		else {
		    if (number<2) {
			if (ch-'0' > 9 ) {
			    ch=tolower(ch);
			    ch=ch-'a'+10;
			}
			else {
			    ch=ch-'0';
			}
			n[number]=(n[number]*16)+ch;
		    }
		}
		
	}
    }while(ch!=EOF && entry<256);

    /*
	now we have to sort it, in order to prepare for binary search 
    */
    qsort( utf8map, 256, sizeof(int), cmputfents );

}

void dump_console_to_file(CharDriverState *chr, char *fn)
{
    FILE* f;
    TextConsole *s = chr->opaque;

    if (s == NULL)
	return;

    if (s->cells == NULL)
	return;

    f=fopen(fn, "wb");
    if (!f)
	return;

    fwrite(s->cells, s->width * s->total_height, sizeof(TextCell), f);
    fclose(f);
}

/* called when an ascii key is pressed */
void kbd_put_keysym(int keysym)
{
    TextConsole *s;
    uint8_t buf[16], *q;
    int c;

    dprintf("kbd_put_keysym 0x%x\n", keysym );
    
    s = active_console;
    if (!s || !s->text_console)
        return;

    switch(keysym) {
    case QEMU_KEY_CTRL_UP:
        console_scroll(-1);
        break;
    case QEMU_KEY_CTRL_DOWN:
        console_scroll(1);
        break;
    case QEMU_KEY_SHIFT_PAGEUP:
        console_scroll(-10);
        break;
    case QEMU_KEY_SHIFT_PAGEDOWN:
        console_scroll(10);
        break;
    default:
        /* convert the QEMU keysym to VT100 key string */
        q = buf;
	switch (keysym) {
	case 0xe100 ... 0xe11f:
            *q++ = '\033';
            *q++ = '[';
            c = keysym - 0xe100;
            if (c >= 10)
                *q++ = '0' + (c / 10);
            *q++ = '0' + (c % 10);
            *q++ = '~';
	    break;
	case 0xe141 ... 0xe144:
	    *q++ = '\033';
	    dprintf("cm %d , %c\n", s->cursorkey_mode, keysym&0xff );
	    *q++ = s->cursorkey_mode ? 'O' : '[';
	    *q++ = keysym & 0xff;
	    break;
	case 0xe120 ... 0xe140:
	case 0xe145 ... 0xe17f:
            *q++ = '\033';
            *q++ = '[';
            *q++ = keysym & 0xff;
	    break;
	default:
	    *q++ = keysym;
        }
	for (c = 0; c < q - buf; c++)
	    dprintf("fchar %c %x\n", buf[c] > 0x1f ? buf[c] : ' ', buf[c]);
	if (s->input_stream.fd != -1)
	    write_or_chunk(&s->input_stream, buf, q - buf);
        break;
    }
}

static TextConsole *new_console(DisplayState *ds, int text)
{
    TextConsole *s;
    int i;

    if (nb_consoles >= MAX_CONSOLES)
        return NULL;
    s = qemu_mallocz(sizeof(TextConsole));
    if (!s) {
        return NULL;
    }
    if (!active_console || (active_console->text_console && !text))
        active_console = s;
    s->ds = ds;
    s->cells = 0;
    s->text_console = text;
    ds->graphic_mode = text ? 0 : 1;
    if (text) {
        consoles[nb_consoles++] = s;
    } else {
        /* HACK: Put graphical consoles before text consoles.  */
        for (i = nb_consoles; i > 0; i--) {
            if (!consoles[i - 1]->text_console)
                break;
            consoles[i] = consoles[i - 1];
        }
        consoles[i] = s;
    }
    s->input_stream.fd = -1;
    s->autowrap = 1;
    return s;
}

TextConsole *graphic_console_init(DisplayState *ds)
{
    TextConsole *s;

    s = new_console(ds, 0);
    if (!s)
      return NULL;
    return s;
}

int is_graphic_console(void)
{
    return !active_console->text_console;
}

void set_color_table(DisplayState *ds) 
{
    int i, j;
    for(j = 0; j < 2; j++) {
	for(i = 0; i < 8; i++) {
	    color_table[j][i] =
		col_expand(ds, vga_get_color(ds, color_table_rgb[j][i]));
	}
    }
}

CharDriverState *text_console_init(DisplayState *ds)
{
    CharDriverState *chr;
    TextConsole *s;
    static int color_inited;

    parse_unicode_map("/usr/share/xen/qemu/cp437_to_uni.trans");

    chr = qemu_mallocz(sizeof(CharDriverState));
    if (!chr)
        return NULL;
    s = new_console(ds, 1);
    if (!s) {
        free(chr);
        return NULL;
    }
    chr->opaque = s;
    chr->chr_write = console_puts;
#if 0
    chr->chr_add_read_handler = console_chr_add_read_handler;
    chr->chr_send_event = console_send_event;
#endif

#if 0
    s->out_fifo.buf = s->out_fifo_buf;
    s->out_fifo.buf_size = sizeof(s->out_fifo_buf);
    s->kbd_timer = qemu_new_timer(rt_clock, kbd_send_chars, s);
#endif
    
    if (!color_inited) {
        color_inited = 1;
        set_color_table(ds);
    }

    s->y_displayed = 0;
    s->y_base = 0;
    s->total_height = DEFAULT_BACKSCROLL;
    set_cursor(s, 0, 0);

    zero_selection(s, 1);

    s->mouse_x = -1;
    s->mouse_y = -1;
    s->g_width = s->ds->width;
    s->g_height = s->ds->height;

    /* Set text attribute defaults */
    s->t_attrib_default.bold = 0;
    s->t_attrib_default.uline = 0;
    s->t_attrib_default.blink = 0;
    s->t_attrib_default.invers = 0;
    s->t_attrib_default.unvisible = 0;
    s->t_attrib_default.fgcol = COLOR_WHITE;
    s->t_attrib_default.bgcol = COLOR_BLACK;
    s->t_attrib_default.used = 0;
    s->t_attrib_default.font = G0;
    s->t_attrib_default.codec = 1;
    s->c_attrib_default.highlit = 0;
    s->unicodeIndex = 0;
    s->unicodeLength = 0;

    /* set current text attributes to default */
    s->t_attrib = s->t_attrib_default;

    text_console_resize(s);

    return chr;
}

void
console_set_input(CharDriverState *chr, int fd, void *opaque)
{
    TextConsole *s = chr->opaque;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    s->input_stream.fd = fd;
    s->input_stream.opaque = opaque;
    s->input_stream.chunk = NULL;
    s->input_stream.chunk_tail = &s->input_stream.chunk;
}

int
console_input_fd(CharDriverState *chr)
{
    TextConsole *s = chr->opaque;

    return s->input_stream.fd;
}

