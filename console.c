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
#include <stdlib.h>
#include <ctype.h>
#include <locale.h>
#include <wchar.h>
#include "debug.h"
#include "consmap.h"

#define DEFAULT_BACKSCROLL (512)
#define MAX_CONSOLES 12

#define QEMU_RGBA(r, g, b, a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define QEMU_RGB(r, g, b) QEMU_RGBA(r, g, b, 0xff)

#define	min(a,b) ((a) < (b) ? (a) : (b))
#define	max(a,b) ((a) > (b) ? (a) : (b))

/* fonts */
#define G0	0
#define G1	1

int insertmode = 0;

typedef struct TextAttributes {
    uint8_t fgcol:4;
    uint8_t bgcol:4;
    uint8_t bold:1;
    uint8_t uline:1;
    uint8_t blink:1;
    uint8_t invers:1;
    uint8_t unvisible:1;
    uint8_t used:1;
    uint8_t utf:1;
    uint8_t font:1;	/* 0 or 1 */
    uint8_t codec[2]; /* 0-3 translation table per font */
} TextAttributes;

typedef struct CellAttributes {
    uint8_t highlit:1;
    uint8_t wrapped:1;
    uint8_t columns:3;
    uint8_t spanned:1;
} CellAttributes;

typedef struct TextCell {
    uint8_t ch;
    TextAttributes t_attrib;
    CellAttributes c_attrib;
} TextCell;

#define MAX_ESC_PARAMS 16
#define MAX_PALETTE_PARAMS 7

enum TTYState {
    TTY_STATE_NORM,
    TTY_STATE_ESC,
    TTY_STATE_PERCENT,
    TTY_STATE_G0,
    TTY_STATE_G1,
    TTY_STATE_CSI,
    TTY_STATE_NONSTD,
    TTY_STATE_PALETTE,
/*
XXX to be done
    TTY_STATE_HASH,
    TTY_STATE_SQUARE,
    TTY_STATE_GETPARS,
    TTY_STATE_GOTPARS,
*/
    TTY_STATE_MAX = TTY_STATE_PALETTE
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

static void
write_or_chunk(struct chunked_stream *s, uint8_t *buf, int len)
{
    int done;
    struct stream_chunk *chunk;

    while (s->chunk) {
	chunk = s->chunk;
	done = write(s->fd, chunk->data + chunk->offset,
		     chunk->len - chunk->offset);
        if (done < 0) return; /* XXX error */
	chunk->offset += done;
	if (chunk->offset == chunk->len) {
	    s->chunk = chunk->next;
	    free(chunk);
	    if (s->chunk == NULL)
		s->chunk_tail = &s->chunk;
	} else
	    break;
    }
    done = 0;
    if (s->chunk == NULL) {
	done = write(s->fd, buf, len);
        if (done < 0) return; /* XXX error */
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

    /* width and height in pixels of "frame"/display */
    int g_width, g_height;

    /* width and height in char cells of "frame"/display*/
    int width, height;

    /* height, including history currently used,
       This should go beyond total_height-heigth */
    int backscroll;

    /* maximum possible height (backscroll)*/
    int total_height;

    /* current cursor position in screen coordinate
     * always 0 <= x < width and 0 <= y <= height
     */
    int x, y;

    /* saved cursor position */
    int saved_x, saved_y;

    /* boolean, selfexplanatory */
    char cursor_visible;

    /* screen's 1st line (the top line)*/
    int y_base;

    /* this is offset that is substracted from y_base
       and points to currently displayed screen */
    int y_scroll;

    /* scroll region in screen coordinate */
    int sr_top, sr_bottom;

    /* self explanatory */
    char autowrap;
    char wrapped;
    int insert_mode;
    int cursorkey_mode;

    /* display control chars */
    char display_ctrl;
    /* toggle high bit */
    char toggle_meta;

    /* orgin mode, not used currently - only set */
    char origin_mode;

    /* default text attributes */
    TextAttributes t_attrib_default;

    /* currently active text attributes */
    TextAttributes t_attrib;
    /* currently saved text attributes */
    TextAttributes saved_t_attrib;

    /* the actual content */
    TextCell *cells;

    /* default text attributes */
    CellAttributes c_attrib_default;

    enum TTYState state;
    int esc_params[MAX_ESC_PARAMS];
    int nb_esc_params;
    int has_esc_param;
    int has_qmark;

    struct chunked_stream input_stream;

    /* first one for current selection, second one for old */
    struct selection selections[2];
    int selecting;

    /* mouse position */
    int mouse_x, mouse_y;

    /* unicode bits (state of unicode input) */
    int unicodeIndex;
    char unicodeData[7];
    int unicodeLength;

    uint8_t palette_params[MAX_PALETTE_PARAMS];
    uint8_t nb_palette_params;
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
static void set_color_table(DisplayState *ds);

#define clip_y(s, v) {			\
	if ((s)->v >= (s)->height)	\
	    (s)->v = (s)->height - 1;	\
	if ((s)->v < 0)			\
	    (s)->v = 0;			\
    }

#define clip_x(s, v) {			\
	if ((s)->v >= (s)->width)	\
	    (s)->v = (s)->width - 1;	\
	if ((s)->v < 0)			\
	    (s)->v = 0;			\
    }

#define clip_xy(s,x,y) {clip_x(s,x);clip_y(s,y);}

/* convert a RGBA color to a color index usable in graphic primitives */
static unsigned int vga_get_color(DisplayState *ds, unsigned int rgba)
{
    unsigned int r, g, b, color;

    switch(ds->depth) {
    case 8:
        r = (rgba >> 16) & 0xff;
        g = (rgba >> 8) & 0xff;
        b = (rgba) & 0xff;
        color = ((r >> 5) << 5 | (g >> 5) << 2 | (b >> 6));
        break;
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
        QEMU_RGB(0xc0, 0x00, 0x00),  /* red */
        QEMU_RGB(0x00, 0xc0, 0x00),  /* green */
        QEMU_RGB(0xb2, 0x68, 0x18),  /* brown */
        QEMU_RGB(0x00, 0x00, 0xc0),  /* blue */
        QEMU_RGB(0xc0, 0x00, 0xc0),  /* magenta */
        QEMU_RGB(0x00, 0xc0, 0xc0),  /* cyan */
        QEMU_RGB(0xc0, 0xc0, 0xc0),  /* white */
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

#define UTFVAL(B) (consmap[curf][B]&0xffff)
/* simplest binary search */
static int get_glyphcode(TextConsole *s, int chart)
{
    int low = 0, high = 255, mid;
    int h=0, o='?';
    int curf = s->t_attrib.codec[s->t_attrib.font];

    /* there is no point in transcribing latin1 char */
    if (curf == MAPLAT1) {
	if (chart <= 0x7f)
	    return chart;
	else
	    curf = MAPGRAF;
    }

    if (chart > UTFVAL(high) || chart < UTFVAL(low))
        return o;
    while(low <= high) {
	h++;
	mid = (low + high) / 2;
	if (UTFVAL(mid) > chart)
	    high = mid - 1;
	else 
	if (UTFVAL(mid) < chart)
	    low = mid + 1;
	else {
	    o = (consmap[curf][mid]>>16)&0xff;
	    break;
        }
    }
    dprintf("utf8: %x to: %x, lookups: %d\n", chart, o, h);
    return o;
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

/* does a framebuffer scrolling by N lines */
static void vga_scroll(TextConsole *s, int n)
{
    int h;

    if (n>0) {	// up
        if (n > s->height) n = s->height;
        n *= FONT_HEIGHT;
        h = s->g_height - n;
	vga_bitblt(s->ds, 0, n, 0, 0, s->g_width, h);
	vga_fill_rect(s->ds, 0, h, s->g_width, n, s->t_attrib.bgcol);
    }
    else {	// down
        n = -n;
        if (n > s->height) n = s->height;
        n *= FONT_HEIGHT;
        h = s->g_height - n;
	vga_bitblt(s->ds, 0, 0, 0, n, s->g_width, h);
	vga_fill_rect(s->ds, 0, 0, s->g_width, n, s->t_attrib.bgcol);
    }
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

//    dprintf("x: %2i y: %2i", x, y);
    console_print_text_attributes(t_attrib, ch);
//    dprintf("font:%d\n", t_attrib->font);

    if (t_attrib->invers ^ c_attrib->highlit ^
	((s->cursor_visible && x == s->x && y == s->y && !s->y_scroll) ))
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

//    dprintf("vga_putcharxy: %d font:%d\n", ch, t_attrib->font );
    switch( t_attrib->font ) {
	case G0:
	    font_ptr = vgafont16 + FONT_HEIGHT * ch;
	break;
	case G1:
	default:
	    font_ptr = graphfont16 + FONT_HEIGHT * ch;
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

    s->sr_top = 0;
    s->sr_bottom = s->height - 1;

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
            c->c_attrib = s->c_attrib_default;
            c++;
        }
    }
    qemu_free(s->cells);
    s->cells = cells;
}

/*
  where virtual lines have to be used in
  loop, this macro should be used instead of y++
*/
#define next_line(A,Y) ((Y+1)%s->total_height)

/* projection onto 'real' screen 
   warning, this can return negative y or over s->height */
static int virtual_to_screen(TextConsole *s, int y) 
{
    y -= s->y_base-s->y_scroll;
    y %= s->total_height;

    if (y<0)
	y += s->total_height;
    return y;
}

/*
  this should be used whenever we want to access data from
  TextCells that reflect currently displayed screen frame 
*/
static int screen_to_virtual(TextConsole *s, int y)
{
    y += s->y_base-s->y_scroll;
    y %= s->total_height;

    if (y<0)
	y += s->total_height;

    return y;
}

/*
  paints character under X,Y - as visible on screen
  also sends VNC update for the character
*/
static void update_xy(TextConsole *s, int x, int y)
{
    TextCell *c;

    if (y<0 || x<0 || x>=s->width || y>=s->height || s != active_console)
	return;

    c = &s->cells[screen_to_virtual(s,y) * s->width + x];
    vga_putcharxy(s, x, y, c->ch,
                  &(c->t_attrib), &(c->c_attrib));
    s->ds->dpy_update(s->ds, x * FONT_WIDTH, y * FONT_HEIGHT,
		      FONT_WIDTH, FONT_HEIGHT);
}
/*
  update whole rectangle of characters, as visible on screen
  since this relies on update_x,y() - it also sends out VNC update
*/
static void update_rect(TextConsole *s, int x, int y, int w, int h)
{
    int i,j;
    for(i=0;i<h;i++) {
        if (i+y > s->height)
	    break;
	for(j=0;j<w;j++) {
	    if (j+x > s->width) {
		w=j;
		break;
	    }
	    update_xy(s, x+j, y+i);
	}
    }
}

static void set_cursor(TextConsole *s, int x, int y)
{
    s->y = y;
    s->wrapped = 0;
    s->x = x;
    clip_xy(s, x, y);
}

static void console_show_cursor(TextConsole *s, int show)
{
    s->cursor_visible = show;
    if (s == active_console && s->x < s->width) {
	update_xy(s, s->x, s->y);
    }
}

/* calculate 'dinstance' (number of lines) between two given
   lines on virtual console
*/
static int line_dist(TextConsole *s, int yf, int yt)
{
    if (yf <= yt )
	return yt-yf;

    yt += s->total_height;

    return yt-yf; 
}

#define swap_coords(TY, FY, TX, FX) {int tmp; tmp = FX; FX = TX; TX = tmp; \
					tmp = FY;FY = TY;TY = tmp;}

/* returns the selection as null terminated char 
   takes coordinates in virtual space already
*/
static char *
get_text(TextConsole *s, int from_x, int from_y, int to_x, int to_y)
{
    TextCell *c;
    char *buffer;
    int bufidx = 0;
    int sc_fy, sc_ty;

    sc_fy = virtual_to_screen(s, from_y);
    sc_ty = virtual_to_screen(s, to_y);

    /* swap if necessary */
    if ((sc_ty < sc_fy || (sc_ty == sc_fy && to_x < from_x)) && abs(sc_fy)-abs(sc_ty) < s->height) {
	swap_coords(to_y, from_y, to_x, from_x);
	sc_fy = sc_ty;
    }

    dprintf("get_text from %d/%d to %d/%d \n", from_y, from_x, to_y, to_x);

    buffer = malloc((line_dist(s, from_y, to_y) + 1) * (s->width + 1));
    if (buffer == NULL)
	return NULL;

    while(from_y != to_y || from_x != to_x) {
	c = &s->cells[from_y*s->width + from_x];
	if (c->t_attrib.used)
	    buffer[bufidx++] = c->ch;
	from_x++;
	if (from_x >= s->width) {
	    from_x = 0;
	    from_y = next_line(s, from_y);
	    if (!(c->t_attrib.used && c->c_attrib.wrapped))
		buffer[bufidx++] = '\n';
	}
    }
    buffer[bufidx] = 0;
    return buffer;
}

/*
    macros operating on selection structure 
*/
#define zero_selection(A,B) {memset( &A->selections[B],0,sizeof(struct selection) );}
#define is_selection_zero(A,B) ((A->selections[B].startx | \
				A->selections[B].starty | \
				A->selections[B].endx | \
				A->selections[B].endy ) == 0 )

/*
    highlight the selected text visualy
    this operates on 'virtual' coordinates
*/
static void
highlight(TextConsole *s, int from_x, int from_y, int to_x, int to_y, int highlight)
{
    TextCell *c;
    int sc_fy, sc_ty;
    int x;
    int last_c = 0;

    if (from_y == to_y && to_x == from_x)
	return;

    sc_fy = virtual_to_screen(s, from_y);
    sc_ty = virtual_to_screen(s, to_y);

    /* swap if necessary */
    if ((sc_ty < sc_fy || (sc_ty == sc_fy && to_x < from_x)) && abs(sc_fy)-abs(sc_ty) < s->height) {
	swap_coords(to_y, from_y, to_x, from_x);
	sc_fy = sc_ty;
    }

    dprintf("highlight from %d/%d to %d/%d - %d \n", from_y, from_x, to_y, to_x, highlight);

    if (to_y != from_y) x = s->width - 1;
    else x = to_x - 1;
    while(x >= from_x) {        
	c = &s->cells[from_y * s->width + x];
	if (c->c_attrib.highlit != highlight) {
	    if (c->t_attrib.used || from_y != to_y || last_c) {
		c->c_attrib.highlit = highlight;
		update_xy(s, x, sc_fy);
                last_c = 1;
	    }
	}

	c--;
	x--;
        if (x < from_x && from_y != to_y) {
            from_y = next_line(s, from_y);
            if (from_y != to_y) x = s->width - 1;
            else x = to_x - 1;
    
	    sc_fy = virtual_to_screen(s, from_y);
            last_c = 0;
            from_x = 0;
	}
    }
}

int
mouse_is_absolute(void *opaque)
{
    return 1;
}

static void console_refresh(TextConsole *s)
{
    TextCell *c;
    int x, y;

    if (s != active_console) 
        return;

    vga_fill_rect(s->ds, 0, 0, s->g_width, s->g_height, s->t_attrib.bgcol);

    for(y = 0; y < s->height; y++) {
        c = &s->cells[screen_to_virtual(s,y) * s->width];
        for(x = 0; x < s->width; x++) {
            vga_putcharxy(s, x, y, c->ch, &(c->t_attrib), &(c->c_attrib));
            c++;
        }
    }
    s->ds->dpy_update(s->ds, 0, 0, s->ds->width, s->ds->height);
    console_show_cursor(s, 1);
}


static void clear_line(TextConsole *s, int line, int from_x, int to_x)
{
    TextCell *c;
    int m_fy, i;

    if (to_x > s->width)
	to_x = s->width;

    if (0 > from_x || from_x >= to_x)
	return;

    m_fy = screen_to_virtual(s, line);
    c = &s->cells[(m_fy * s->width)+from_x];

    for (i = from_x; i < to_x; i++) {
	c->ch = ' ';
	c->t_attrib = s->t_attrib_default;
	c->t_attrib.fgcol = s->t_attrib.fgcol;
	c->t_attrib.bgcol = s->t_attrib.bgcol;
        c->c_attrib.wrapped = s->c_attrib_default.wrapped;
        c->c_attrib.columns = 1;
        c->c_attrib.spanned = 0;
	c++;
   }

   update_rect(s, from_x, line, to_x - from_x, 1);
}

static void clear(TextConsole *s, int from_x, int start_y, int to_x, int height)
{
    int i;
    dprintf("clear(%d, %d, %d, %d)\n", from_x, start_y, to_x,
	    start_y + height);
    for (i = 0; i < height; i++)
	clear_line(s, start_y + i,
		   (i == 0) ? from_x : 0, (i == height - 1) ? to_x : s->width);
}

/* this just scrolls view */
static void console_scroll(TextConsole *s, int ydelta)
{
    int y_scroll;

    if (!s || !s->text_console)
        return;

    y_scroll = s->y_scroll - ydelta;

    if (y_scroll > s->backscroll)
	y_scroll = s->backscroll;

    if (y_scroll < 0)
	y_scroll = 0;

    ydelta = s->y_scroll - y_scroll;
    if (ydelta == 0)
	return;
    s->y_scroll = y_scroll;

    if (abs(ydelta) < s->height) {
	vga_scroll(s, ydelta);
	
	if (ydelta>0)
	    update_rect(s, 0, s->height-ydelta, s->width, ydelta );
	else
	    update_rect(s, 0, 0, s->width, -ydelta );

	/* update whole region, because dpy_copy_rect is currently not used */
	s->ds->dpy_update(s->ds, 0, 0, s->g_width, s->g_height);
    }
    else {
	update_rect(s, 0, 0, s->width, s->height );
    }
}

static void scroll_text_cells(TextConsole* s, int f, int t, int by)
{
    TextCell* fc, *tc;
    int m_fy, m_ty, direction;

    /* prevent zero division*/
    if (by == 0) return;

    direction = by/abs(by);
    by = abs(by);

    while(by--) {
	m_fy = screen_to_virtual(s, f);
	m_ty = screen_to_virtual(s, t);

	fc = &s->cells[(m_fy * s->width)];
	tc = &s->cells[(m_ty * s->width)];

	memmove(tc, fc, s->width*sizeof(TextCell));

	t += direction;
	f += direction;
    };
}

static void scroll_to_base(TextConsole*s) 
{
    if (s->y_scroll)
	console_scroll(s, s->y_scroll);
}

/* scrolls down, moves whole view to the +n point */
static void scroll_down(TextConsole* s, int n)
{
    if (!s || !s->text_console)
        return;
    
    if ( s->sr_top != 0 || s->sr_bottom != s->height-1 ) {
        if ( n > s->sr_bottom-s->sr_top ) {
            n = s->sr_bottom-s->sr_top;
        }
        scroll_text_cells(s, s->sr_bottom-n, s->sr_bottom, n - s->sr_bottom + s->sr_top - 1);
        update_rect(s, 0, s->sr_top + n, s->width, s->sr_bottom - s->sr_top - n + 1);
        clear(s, 0, s->sr_top, s->width, n);
        
        return;
    }
       
    s->backscroll -= n;
    if (s->backscroll < 0)
        s->backscroll = 0;
    
    s->y_base -= n;
    if (s->y_base < 0)
        s->y_base += s->total_height;

    vga_scroll(s, -n);
    clear(s, 0, s->sr_top, s->width, n);
    s->ds->dpy_update(s->ds, 0, 0, s->g_width, s->g_height);
}

/* scrolls up, moves whole view to the -n point */
static void scroll_up(TextConsole* s, int n)
{
    if (!s || !s->text_console)
        return;
    
    if ( s->sr_top != 0 || s->sr_bottom != s->height-1 ) {
        if ( n > s->sr_bottom-s->sr_top ) {
            n = s->sr_bottom-s->sr_top;
        }
        scroll_text_cells(s, s->sr_top+n, s->sr_top, s->sr_bottom-s->sr_top-n+1);
        update_rect(s, 0, s->sr_top, s->width, s->sr_bottom - s->sr_top - n + 1);
        clear(s, 0, s->sr_bottom - n + 1, s->width, n);
        
        return;
    }
    
    s->backscroll += n;
    if (s->backscroll > (s->total_height-s->height) )
        s->backscroll = s->total_height-s->height;

    s->y_base = s->y_base + n;  
    if (s->y_base > s->total_height )
        s->y_base -= s->total_height;
    
    vga_scroll(s, n);
    clear(s, 0, s->sr_bottom - n + 1, s->width, n);
    s->ds->dpy_update(s->ds, 0, 0, s->g_width, s->g_height);
}

void
mouse_event(int dx, int dy, int dz, int buttons_state, void *opaque)
{
    static int odx = 0;
    int ndx;
    CharDriverState *chr = opaque;
    TextConsole *s = chr->opaque;
    char *text;

    dprintf("mouse event %03x:%03x:%x:%x\n", dx, dy, dz, buttons_state);
    ndx = dx;
    dx = dx * s->width / 0x7FFF;
    dy = dy * s->height / 0x7FFF;

/* boundry check & fix */
    if (dy >= s->height)
	dy = s->height-1;

    if (dx >= s->width)
	dx = s->width-1;

    if (dy < 0) 
	dy = 0;

    if (dx < 0)
	dx = 0;

    if (dz == -1)
	console_scroll(s, -1);
    if (dz == 1)
	console_scroll(s, 1);

    s->mouse_x = dx;
    s->mouse_y = dy;

    /* button not pressed */
    if (buttons_state == 0) {

        /* if button was pressed before, means we have to grab selected text
           end send it to peer's clipboard */
	if (s->selecting) {
	    text = get_text(s, s->selections[0].startx, s->selections[0].starty,
			   s->selections[0].endx, s->selections[0].endy);

	    if (text != NULL) {
		if (strlen(text))
		    s->ds->dpy_set_server_text(s->ds, text);
		else
		    free(text);
	    }

	    /* set flag, copy current selection to old one */
	    s->selecting = 0;
	    memcpy( &s->selections[1], &s->selections[0], sizeof(struct selection) );
	    zero_selection(s, 0);
	}
    } else if (buttons_state == 1) {
        /* button pressed, no selection made - we have to initialze selection */
	if (s->selecting == 0 ){
	    /* if previous highlight is still displayed, 
               we have to cancel it */
	    if ( !is_selection_zero(s, 1) )
		highlight(s, s->selections[1].startx, s->selections[1].starty,
			s->selections[1].endx, s->selections[1].endy, 0);
	    zero_selection(s, 1);

	    /* initialize current coordinates */
	    s->selections[0].startx = dx;
	    s->selections[0].starty = screen_to_virtual(s, dy);
	    s->selections[0].endx = dx;
	    s->selections[0].endy = screen_to_virtual(s, dy);
	    s->selecting=1;
	    /* highlite current character */
	    highlight(s, dx, screen_to_virtual(s, dy), dx, screen_to_virtual(s, dy), 1);
	}
	else {
	if ( !is_selection_zero(s, 0) )
	    /* in this case, we just have to update selection */
	    /* zero the highlight first */
	    highlight(s, s->selections[0].startx, s->selections[0].starty,
		s->selections[0].endx, s->selections[0].endy, 0);
            if (dx == s->selections[0].endx) {
                if (ndx - odx > 10) dx++;
            } else if (dx == s->selections[0].endx - 1) {
                if (odx - ndx < 10) dx++;
            }
            if (dx >= s->width) dx = s->width - 1;

	    /* update coords */
	    s->selections[0].endx = dx;
	    s->selections[0].endy = screen_to_virtual(s, dy);
	    /* highlight new region */
	    highlight(s, s->selections[0].startx, s->selections[0].starty,
		s->selections[0].endx, s->selections[0].endy, 1);
	}
    }
    odx = ndx;
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
    scroll_to_base(s);

    if (s->y + 1 > s->sr_bottom) {
	scroll_up(s, 1);
        set_cursor(s, s->x, s->sr_bottom);
    } else {
        set_cursor(s, s->x, s->y + 1);
    }
}

static void console_put_cr(TextConsole *s)
{
    set_cursor(s, 0, s->y);
}

static void console_put_ri(TextConsole *s)
{
    if (s->y - 1 < s->sr_top) {
	scroll_down(s, 1);
	set_cursor(s, s->x, s->sr_top);
    } else {
        set_cursor(s, s->x, s->y - 1);
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

    dprintf("handle ESC CSI M %d\n", s->nb_esc_params);
    if (s->nb_esc_params == 0) { /* ESC[m sets all attributes to default */
        s->t_attrib = s->t_attrib_default;
        return;
    }
    for (i=0; i<s->nb_esc_params; i++) {
	dprintf("\tparam %d\n", s->esc_params[i]);
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
            case 10:
                s->t_attrib.font = 0;
                s->display_ctrl = 0;
                s->toggle_meta = 0;
                break;
            case 11:
                s->t_attrib.codec[s->t_attrib.font] = MAPGRAF;
                s->display_ctrl = 1;
                s->toggle_meta = 0;
                break;
            case 12:
                s->t_attrib.codec[s->t_attrib.font] = MAPIBMPC;
                s->display_ctrl = 1;
                s->toggle_meta = 1;
                break;
/*
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
int normidx = 0, norm_x = 0, norm_y = 0;
static void print_norm(void)
{
    if (normidx) {
	normbuf[normidx] = 0;
	dprintf("norm %d:%d >%s<\n", norm_x, norm_y, normbuf);
	normidx = 0;
    }
}
static void put_norm(TextConsole *s, char ch)
{
    if (normidx == 0) {
	norm_x = s->x;
	norm_y = s->y;
    }
    normbuf[normidx++] = ch;
    if (normidx == 1024)
	print_norm();
}

static void do_putchar_utf(TextConsole *s, wchar_t ch, char glyph)
{
    TextCell *c;
    int nc, i;

    scroll_to_base(s);

    if (s->wrapped) {
        c = &s->cells[screen_to_virtual(s, s->y) * s->width + s->x];
        c->c_attrib.wrapped=1;
        set_cursor(s, 0, s->y);
        console_put_lf(s);
    }

    nc = wcwidth(ch);
    dprintf("utf-8: %d columns char\n", nc);
    if (nc < 0) nc = 1;
    /* assure we have enough space to put our character, do no split in two lines */
    if (s->x + nc > s->width) {
	set_cursor(s, 0, s->y);
	console_put_lf(s);
    }
    for (i = 0; i < nc; i++) {
        put_norm(s, glyph);
        c = &s->cells[screen_to_virtual(s, s->y) * s->width + s->x + i];
        c->ch = glyph;
        c->t_attrib = s->t_attrib;
        c->t_attrib.used = 1;
        c->c_attrib = s->c_attrib_default;
        c->c_attrib.columns = nc;
        c->c_attrib.spanned = i ? 1 : 0;
        update_xy(s, s->x + i, s->y);
    }

    if (s->x + nc < s->width)
        set_cursor(s, s->x + nc, s->y);
    else
        if (s->autowrap)
            s->wrapped = 1;
}

static void do_putchar(TextConsole *s, int ch)
{
    TextCell *c;

    scroll_to_base(s);

    put_norm(s, ch);
    if (s->wrapped) {
	c = &s->cells[screen_to_virtual(s, s->y) * s->width + s->x];
	c->c_attrib.wrapped=1;
	set_cursor(s, 0, s->y);
	console_put_lf(s);
    }
    c = &s->cells[screen_to_virtual(s, s->y) * s->width + s->x];
    c->ch = ch;
    c->t_attrib = s->t_attrib;
    c->t_attrib.used = 1;
    c->c_attrib = s->c_attrib_default;
    update_xy(s, s->x, s->y);
    if (s->x + 1 < s->width)
	set_cursor(s, s->x + 1, s->y);
    else
	if (s->autowrap)
	    s->wrapped = 1;

}

static int handle_params(TextConsole *s, int ch)
{
    int i;

    dprintf("putchar csi %02x '%c'\n", ch, ch > 0x1f ? ch : ' ');
    if (ch >= '0' && ch <= '9') {
	if (s->nb_esc_params < MAX_ESC_PARAMS && (s->esc_params[s->nb_esc_params] < 10000)) {
	    s->esc_params[s->nb_esc_params] = 
		s->esc_params[s->nb_esc_params] * 10 + ch - '0';
	}
	s->has_esc_param = 1;
	return 0;
    } else {
	if (s->has_esc_param && s->nb_esc_params < MAX_ESC_PARAMS)
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
	    dprintf(" 0x%02x/%d", s->esc_params[i], s->esc_params[i]);
	dprintf("\n");
    }
    return 1;
}

static void reset_params(TextConsole *s)
{
    int i;

    for(i=0;i<MAX_ESC_PARAMS;i++)
	s->esc_params[i] = 0;
    s->has_esc_param = 0;
    s->nb_esc_params = 0;
    s->has_qmark = 0;
}

static void console_dch(TextConsole *s)
{
    TextCell *row;
    int x, nc;

    nc = s->esc_params[0];
    if (nc == 0)
	nc = 1;

    row = &s->cells[screen_to_virtual(s,s->y) * s->width];

    /* move to first column of current character */
    for (x = s->x; x > 0 && row[x].c_attrib.spanned; --x)
        continue;

    /* skip nc characters */
    for (; nc > 0 && x < s->width; --nc)
        x += row[x].c_attrib.columns;

    /* compute as many columns we skipped */
    if (x > s->width)
        x = s->width - 1;
    nc = x - s->x;

    for (x = s->x; x + nc < s->width; ++x) {
        row[x].ch = row[x + nc].ch;
	row[x].t_attrib = row[x + nc].t_attrib;
	update_xy(s, x, s->y);
    }
    for (; x < s->width; x++) {
        row[x].ch = ' ';
        row[x].t_attrib = s->t_attrib_default;
        row[x].t_attrib.fgcol = s->t_attrib.fgcol;
        row[x].t_attrib.bgcol = s->t_attrib.bgcol;
        row[x].c_attrib.wrapped = s->c_attrib_default.wrapped;
        update_xy(s, x, s->y);
    }
}

static void console_putchar(TextConsole *s, int ch)
{
    TextCell *c, *d;
    int i, x, y, x1, y1, a;

    dprintf("putchar %02x '%c' state:%d\n", ch, ch > 0x1f ? ch : ' ', s->state);
    if (s->unicodeIndex > 0 && (ch & 0xc0) == 0x80) goto unicode;

    switch(s->state) {
    case TTY_STATE_NORM:
	dprintf("putchar norm %02x '%c'\n", ch, ch > 0x1f ? ch : ' ');
        if (s->display_ctrl && (ch == 127 || !((0x0800f501 >> ch) & 1))) goto unicode;
        switch(ch) {
        case NUL:
        case STX:
        case SOH:
            break;
        case BEL:
	    dprintf("bell\n");
	    s->ds->dpy_bell(s->ds);
            break;
        case BS:
	    dprintf("BS\n");
            set_cursor(s, s->x - 1, s->y);
            break;
        case HT:
	    dprintf("HT\n");
	    x = s->x + (8 - (s->x % 8));
            if (x > s->width) {
                set_cursor(s, 0, s->y);
                console_put_lf(s);
            } else {
                set_cursor(s, x, s->y);
            }
            break;
        case LF:
        case VT:
        case FF:
	    dprintf("LF\n");
            console_put_lf(s);
            break;
        case CR:
	    dprintf("CR\n");
            set_cursor(s, 0, s->y);
            break;
        case SO:
	    dprintf("SO G1 switch\n");
	    s->t_attrib.font = G1;
            s->display_ctrl = 1;
            break;
        case SI:
            dprintf("SI G0 switch\n");
	    s->t_attrib.font = G0;
            s->display_ctrl = 0;
            break;
        case CAN:
	case ESN:
            dprintf("not implemented CAN\n");
            break;
        case ESC:
            dprintf("ESC state\n");
            print_norm();
	    reset_params(s);
            s->state = TTY_STATE_ESC;
            break;
        case DEL: /* according to term=linux 'standard' should be ignored.*/
            break;
        case CSI:
            dprintf("CSI state\n");
            print_norm();
	    reset_params(s);
            s->state = TTY_STATE_CSI;
            break;

        default:
        unicode:
/* utf 8 bit */
	    if (s->t_attrib.utf && !s->display_ctrl) {
		if (s->unicodeIndex > 0) {
                    wchar_t wc;
		    if ((ch & 0xc0) != 0x80) {
			dprintf("bogus unicode data %u\n", ch);
			s->unicodeIndex = 0;
			do_putchar(s, '?');
			return;
		    }
		    s->unicodeData[s->unicodeIndex++] = ch;
		    if (s->unicodeIndex < s->unicodeLength) {
			return;
		    }
		    mbrtowc(&wc, s->unicodeData, s->unicodeLength, NULL);
                    switch (s->unicodeLength) {
                        case 2 ... 6:
                            ch = (s->unicodeData[0] & (0x7f >> s->unicodeLength));
                            break;
                        default:
                            dprintf("bogus unicode length %u\n", s->unicodeLength);
                            s->unicodeIndex = 0;
                            return;
                        break;
                    }
                    for (i = 1; i < s->unicodeLength; i++) {
                        ch = (ch << 6) | (s->unicodeData[i] & 0x3f);
                    } 
                    s->unicodeIndex = 0;
                    ch = get_glyphcode(s, ch);
                    do_putchar_utf(s, wc, ch);
                    return;
		}
                /* multibyte sequence */
		else if (ch > 0x7f) {
                    memset(s->unicodeData, '\0', 7);
                    s->unicodeData[0] = ch;
                    s->unicodeIndex = 1;
                    if ((ch & 0xe0) == 0xc0) {
			s->unicodeLength = 2;
			return;
		    } 
		    else
		    if ((ch & 0xf0) == 0xe0) {
			s->unicodeLength = 3;
			return;
		    } 
		    else
		    if ((ch & 0xf8) == 0xf0) {
			s->unicodeLength = 4;
			return;
		    }
                    else
                    if ((ch & 0xfc) == 0xf8) {
			s->unicodeLength = 5;
                        return;
                    }
                    else
                    if ((ch & 0xfe) == 0xfc) {
			s->unicodeLength = 6;
                        return;
                    } else {
                        dprintf("Invalid unicode sequence start %x\n", ch);
                        s->unicodeIndex = 0;
                        do_putchar(s, '?');
                        return;
                    }
		} else {
		    /* single ASCII char */
		    do_putchar(s, ch);
		}
	    /* end of utf 8 bit */
	    } else {
	        do_putchar(s, s->toggle_meta ? (ch|0x80) : ch);
	        return;
	    }
            break;
        }
        break;
    case TTY_STATE_ESC: /* check if it is a terminal escape sequence */
	if (ch != '[')
	    dprintf("putchar esc %02x '%c'\n", ch > 0x1f ? ch : ' ', ch);
	s->state = TTY_STATE_NORM;
	switch (ch) {
	case ']': /* Operating system command */
            s->state = TTY_STATE_NONSTD;
	    break;
	case '>': /* Set numeric keypad mode */
	    break;
	case '=': /* Set application keypad mode */
	    break;
	case '#': /* boo */
	    dprintf("DECTEST: this should print E's on screen\n");
	    break;
	case 'c': /* reset */
	    dprintf("RESET\n");
	    set_cursor(s, 0, 0);
	    s->display_ctrl = 0;
	    s->toggle_meta = 0;
	    s->has_esc_param = 0;
	    s->nb_esc_params = 0;
            s->t_attrib = s->t_attrib_default;
	    /* reset any highlighted area */
	    if ( !is_selection_zero(s, 1) )
		highlight(s, s->selections[1].startx, s->selections[1].starty,
			s->selections[1].endx, s->selections[1].endy, 0);
	    zero_selection(s,1);
	    clear(s, s->x, s->y, s->width, s->height);
	    break;
	case 'D': /* linefeed */
	    dprintf("ESC_LF\n");
	    console_put_lf(s);
	    break;
	case 'H': /* Set tab stop at current column.*/
	    dprintf("TAB stop - unimplemented\n");
	    break;
	case 'Z': /* DEC private identification */
	    dprintf("DEC INDENT\n");
	    va_write(s, "\033[?6c");
	    break;
	/* charset selection */
	case '%':
	    dprintf("ESC PERCENT\n");
	    s->state = TTY_STATE_PERCENT;
	    break;
	case '(': /* G0 charset */
	    dprintf("ESC (\n");
	    s->state = TTY_STATE_G0;
	    break;
	case ')': /* G1 charset */
	    dprintf("ESC )\n");
	    s->state = TTY_STATE_G1;
	    break;

	case '[': /* CSI */
	    reset_params(s);
            s->state = TTY_STATE_CSI;
	    break;
	case 'E': /* new line */
	    dprintf("ESC LF CR\n");
	    console_put_lf(s);
	    console_put_cr(s);
	    break;
	case 'M': /* reverse linefeed */
	    dprintf("ESC RLF\n");
	    console_put_ri(s);
	    break;
	case '7': /* save current state */
	    dprintf("ESC SAVE STATE\n");
	    s->saved_x = s->x;
	    s->saved_y = s->y;
	    s->saved_t_attrib = s->t_attrib;
	    break;
	case '8': /* restore current state */
	    dprintf("ESC RESTORE STATE\n");
	    set_cursor(s, s->saved_x, s->saved_y);
	    s->t_attrib = s->saved_t_attrib;
	    break;
        case 'P':
	case 'R':
	default:
	    dprintf("unknown STATE_ESC command %d\n", ch);
	    break;
	 }
        break;
    case TTY_STATE_CSI: /* handle escape sequence parameters */
	if (handle_params(s, ch)) {
	    s->state = TTY_STATE_NORM;
            switch(ch) {
	    case '@': /* ins del characters */
		y1 = screen_to_virtual(s, s->y);
		c = &s->cells[y1 * s->width + s->width - 1];
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		if (a > s->width - 1)
		    a = s->width - 1;
		d = &s->cells[y1 * s->width + s->width - 1 - a];
		for (x = s->width - 1; x >= s->x + a; x--) {
		    c->ch = d->ch;
		    c->t_attrib = d->t_attrib;
		    c--;
		    d--;
		    update_xy(s, x, s->y);
		}
		clear_line(s, s->y, s->x, s->x + a);
                break;
	    case 'A': /* cursor up */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		dprintf("cursor up %d\n", a);
		if (a > s->y)
		    a = s->y;
		set_cursor(s, s->x, s->y - a);
		if (s->y < s->sr_top)
		    set_cursor(s, s->x, s->sr_top);
		break;
	    case 'B': /* cursor down */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		dprintf("cursor down %d\n", a);
		set_cursor(s, s->x, s->y + a);
		if (s->y > s->sr_bottom)
		    set_cursor(s, s->x, s->sr_bottom);
		break;
	    case 'a':
            case 'C': /* cursor right */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		dprintf("cursor right %d\n", a);
		set_cursor(s, s->x + a, s->y);
                break;
            case 'D': /* cursor left */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		dprintf("cursor left %d\n", a);
		set_cursor(s, s->x - a, s->y);
                break;
	    case 'E': /* cursor down and to first column */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		dprintf("cursor down %d and to first column\n", a);
		set_cursor(s, 0, s->y + a);
		if (s->y > s->sr_bottom)
		    set_cursor(s, 0, s->sr_bottom);
		break;
	    case 'F': /* cursor up and to first column */
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->nb_esc_params ? s->esc_params[0] : 1;
		dprintf("cursor up %d and to first column\n", a);
		set_cursor(s, 0, s->y - a);;
		if (s->y < s->sr_top)
		    set_cursor(s, 0, s->sr_top);
		break;
	    case '`': /* fallthrough */
	    case 'G':
		if (s->nb_esc_params == 1) {
		    if (s->esc_params[0] == 0)
			s->esc_params[0] = 1;
		    dprintf("set cursor x %d\n", s->esc_params[0] - 1);
		    set_cursor(s, s->esc_params[0] - 1, s->y);
		}
		break;
	    case 'f':
	    case 'H': /* cursor position */
		x = s->esc_params[1];
		if (x == 0)
		    x = 1;
		--x;
		y = s->esc_params[0];
		if (y == 0)
		    y = 1;
		--y;
		set_cursor(s, x,  (s->origin_mode ? s->sr_top : 0) + y);
		dprintf("cursor pos %d:%d\n", s->y, s->x);
		break;
	    case 'J': /* eraseInDisplay */
		if (s->nb_esc_params == 0)
		    s->esc_params[0] = 0;
		switch(s->esc_params[0]) {
		    case 0: /* erase from cursor to end of display */
			clear(s, s->x, s->y, s->width,
			      s->sr_bottom - s->y + 1);
			break;
		    case 1: /* erase from start to cursor */
			clear(s, 0, s->sr_top, s->x + 1, s->y - s->sr_top + 1);
			break;
		    case 2: /* erase whole display */
			clear(s, 0, s->sr_top, s->width,
			      s->sr_bottom - s->sr_top + 1);
			break;
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
		    dprintf("clear line %d %d->%d\n", s->y, x, x1);
		    clear(s, x, s->y, x1, 1);
                }
                break;
	    case 'L':
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
                scroll_down(s, s->esc_params[0]);
		break;
	    case 'M':
		a = s->esc_params[0];
		if (a == 0)
		    a = 1;
		if (a > s->height)
		    a = s->height;
                scroll_text_cells(s, s->y + a, s->y, s->sr_bottom - s->y - a + 1);
                update_rect(s, 0, s->y, s->width, s->sr_bottom - s->y - a + 1);
                clear(s, 0, s->sr_bottom - a + 1, s->width, a);
		break;
	    case 'P':		/* DCH - delete character */
		console_dch(s);
		break;
            case 'X':		/* ECH - erase character */
		c = &s->cells[screen_to_virtual(s,s->y) * s->width];
		if (s->esc_params[0] == 0)
		    s->esc_params[0] = 1;
		a = s->esc_params[0];
		for (x = s->x; x > 0 && c[x].c_attrib.spanned; --x)
		    continue;
		for (; a > 0 && x < s->width; --a)
		    x += c[x].c_attrib.columns;
		/* does not test if x >= s->width as clear already clip x values*/
		clear(s, s->x, s->y, x, 1);
                break;
	    case 'c': /* device attributes */
		if (s->nb_esc_params == 0 )
                    va_write(s, "\033[?6c"); // I'm a VT102
		/* if there are any params, just return */ 
		break;
	    case 'd':
		if (s->nb_esc_params == 1) {
		    if (s->esc_params[0] == 0)
			s->esc_params[0] = 1;
		    set_cursor(s, s->x, s->esc_params[0] - 1);
		}
		break;
	    case 'e':
		if (s->nb_esc_params == 1) {
		    if (s->esc_params[0] == 0)
			s->esc_params[0] = 1;
		    set_cursor(s, s->x, s->y + s->esc_params[0]);
		    if (s->y > s->sr_bottom)
			set_cursor(s, s->x, s->sr_bottom);
		}
		break;
	    case 'm':
#if !defined(__APPLE__)
		    console_handle_escape(s);
#endif
		break;
	    case 'l': /* reset mode */
            case 'h': /* set mode */
		a = (ch == 'h') ? 1 : 0;
		if (s->has_qmark) {
		    for (i = 0; i < s->nb_esc_params; i++) {
			switch (s->esc_params[i]) {
			case 1:
			    s->cursorkey_mode = a;
			    break;
			case 2:
			    s->t_attrib.utf = ~a;
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
			    s->cursor_visible = a;
			    break;
			case 1000:
			    // s->mousereporting_mode = a;
			    break;
			}
		    }
		} else if (s->nb_esc_params >= 1) {
		    switch (s->esc_params[0]) {
		    case 3:
			s->display_ctrl = a;
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
		    case 5:     /* DSR */
			va_write(s, "%c[0n", 0x1b);
			break;
		    case 6:	/* CPR */
			va_write(s, "%c[%d;%dR", 0x1b, s->y + 1, s->x + 1);
			break;
		    }
		}
		break;
	    case 'r':
		if (s->nb_esc_params == 0) {
		    s->sr_top = 0;
		    s->sr_bottom = s->height - 1;
		} else if (s->nb_esc_params == 2) {
		    if (s->esc_params[0] == 0)
			s->esc_params[0] = 1;
		    if (s->esc_params[1] == 0)
			s->esc_params[1] = 1;
		    s->sr_top = s->esc_params[0] - 1;
		    s->sr_bottom = s->esc_params[1] - 1;
		    clip_xy(s, sr_top, sr_bottom);
		}
		set_cursor(s, 0, s->sr_top);
		break;
	    case 's':
		s->saved_x = s->x;
		s->saved_y = s->y;
		break;
	    case 'u':
		set_cursor(s, s->saved_x, s->saved_y);
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
	    case ']':
		dprintf("setterm(%d) NOT IMPLEMENTED\n", s->esc_params[0]);
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
	    case TTY_STATE_G0:
	    case TTY_STATE_G1:
		i = (s->state == TTY_STATE_G1) ? G0:G1;
		dprintf("TTY_STATE_G%01d %d\n", i, ch);
		switch(ch) {
		    case '0':
			s->t_attrib.codec[i] = MAPGRAF;
		    break;
		    case 'B':
			s->t_attrib.codec[i] = MAPLAT1;
		    break;
		    case 'U':
			s->t_attrib.codec[i] = MAPIBMPC;
		    break;
		    case 'K':
			s->t_attrib.codec[i] = MAPUSER;
		    break;
		}
		s->state = TTY_STATE_NORM;
	    break;

	    case TTY_STATE_PERCENT:
		dprintf("TTY_STATE_PERCENT %d\n", ch);
		switch (ch) {
		    case '@':
			s->t_attrib.utf = 0;
			s->t_attrib_default.utf = 0;
			break;
		    case 'G':
		    case '8': 
			s->t_attrib.utf = 1;
			s->t_attrib_default.utf = 1;
			break;
		}
		s->state = TTY_STATE_NORM;
		break;
        }
        break;
    case TTY_STATE_NONSTD:
        dprintf("TTY_STATE_NONSTD %c\n", ch);
        switch (ch) {
            case 'P':
                s->nb_palette_params = 0;
                memset(s->palette_params, 0x00, sizeof(uint8_t) * MAX_PALETTE_PARAMS);
                s->state = TTY_STATE_PALETTE;
                break;
            case 'R':
                set_color_table(s->ds);
                s->state = TTY_STATE_NORM;
                break;
            default:
                s->state = TTY_STATE_NORM;
                break;
        }
        break;
    case TTY_STATE_PALETTE:
        if ( (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f') ) {
            s->palette_params[s->nb_palette_params++] = (ch > '9' ? (ch & 0xDF) - 'A' + 10 : ch - '0');
            if (s->nb_palette_params == 7) {
                uint8_t r, g, b, j = 1;
                r = 16 * s->palette_params[j++];
                r += s->palette_params[j++];
                g = 16 * s->palette_params[j++];
                g += s->palette_params[j++];
                b = 16 * s->palette_params[j++];
                b += s->palette_params[j];
		if (s->palette_params[0] < 8)
	                color_table[0][s->palette_params[0]] = col_expand(s->ds, vga_get_color(s->ds, QEMU_RGB(r, g, b)));
                s->state = TTY_STATE_NORM; 
            }
        } else
            s->state = TTY_STATE_NORM;
        break;
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

static int cmputfents(const void *p1, const void *p2)
{
    int a,b;

    a=*(int*)p1;
    b=*(int*)p2;

    return (a&0xffff)-(b&0xffff);
}

static void prepare_console_maps()
{
    unsigned int i,j;

    for(i=0;i<3;i++)
	for(j=0;j<256;j++) {
 	    consmap[i][j] |= j<<16;
	}
    /*
	now we have to sort it, in order to prepare for binary search
    */

    for(i=0;i<3;i++)
	qsort( consmap[i], 256, sizeof(unsigned int), cmputfents );
}

/* Not safe after we drop privileges */
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

    fwrite(&(s->g_width), sizeof(int), 1, f);
    fwrite(&(s->g_height), sizeof(int), 1, f);
    fwrite(&(s->total_height), sizeof(int), 1, f);
    fwrite(&(s->sr_bottom), sizeof(int), 1, f);
    fwrite(&(s->sr_top), sizeof(int), 1, f);
    fwrite(&(s->y_base), sizeof(int), 1, f);
    fwrite(&(s->y_scroll), sizeof(int), 1, f);
    fwrite(&(s->wrapped), sizeof(char), 1, f);
    fwrite(&(s->x), sizeof(int), 1, f);
    fwrite(&(s->y), sizeof(int), 1, f);
    fwrite(&(s->saved_x), sizeof(int), 1, f);
    fwrite(&(s->saved_y), sizeof(int), 1, f);
    fwrite(&(s->backscroll), sizeof(int), 1, f);
    fwrite(&(s->total_height), sizeof(int), 1, f);
    fwrite(&(s->cursor_visible), sizeof(char), 1, f);
    fwrite(&(s->autowrap), sizeof(char), 1, f);
    fwrite(&(s->wrapped), sizeof(char), 1, f);
    fwrite(&(s->insert_mode), sizeof(int), 1, f);
    fwrite(&(s->cursorkey_mode), sizeof(int), 1, f);
    fwrite(&(s->display_ctrl), sizeof(char), 1, f);
    fwrite(&(s->toggle_meta), sizeof(char), 1, f);
    fwrite(&(s->t_attrib_default), sizeof(TextAttributes), 1, f);
    fwrite(&(s->t_attrib), sizeof(TextAttributes), 1, f);
    fwrite(&(s->saved_t_attrib), sizeof(TextAttributes), 1, f);
    fwrite(s->cells, sizeof(TextCell), s->width * s->total_height, f);
    fwrite(&(s->state), sizeof(int), 1, f);
    fwrite((s->esc_params), sizeof(int), MAX_ESC_PARAMS, f);
    fwrite(&(s->nb_esc_params), sizeof(int), 1, f);
    fwrite(&(s->has_esc_param), sizeof(int), 1, f);
    fwrite(&(s->has_qmark), sizeof(int), 1, f);
    fwrite((s->selections), sizeof(struct selection), 2, f);
    fwrite(&(s->selecting), sizeof(int), 1, f);
    fwrite(&(s->mouse_x), sizeof(int), 1, f);
    fwrite(&(s->mouse_y), sizeof(int), 1, f);
    fwrite(&(s->unicodeIndex), sizeof(int), 1, f);
    fwrite((s->unicodeData), sizeof(char), 7, f);
    fwrite(&(s->unicodeLength), sizeof(int), 1, f);
    fclose(f);
}

static int clip_to(int value, int from, int to)
{
    if (value < from)
        value = from;
    if (value > to)
        value = to;
    return value;
}

#define clip_to(value, a, b) do { (value) = clip_to((value), a, b); } while(0)

void load_console_from_file(CharDriverState *chr, char *fn)
{
    FILE* f;
    TextConsole *s = chr->opaque;

    if (s == NULL)
        return;

    if (s->cells == NULL)
        return;

    f=fopen(fn, "rb");
    if (!f)
        return;

    fread(&(s->g_width), sizeof(int), 1, f);
    fread(&(s->g_height), sizeof(int), 1, f);
    fread(&(s->total_height), sizeof(int), 1, f);

    clip_to(s->g_width, FONT_WIDTH*2, FONT_WIDTH*1600);
    clip_to(s->g_height, FONT_HEIGHT*2, FONT_HEIGHT*500);
    clip_to(s->total_height, s->g_height / FONT_HEIGHT, 8192);
    
    text_console_resize(s);
    
    fread(&(s->sr_bottom), sizeof(int), 1, f);
    fread(&(s->sr_top), sizeof(int), 1, f);
    fread(&(s->y_base), sizeof(int), 1, f);
    fread(&(s->y_scroll), sizeof(int), 1, f);
    fread(&(s->wrapped), sizeof(char), 1, f);
    fread(&(s->x), sizeof(int), 1, f);
    fread(&(s->y), sizeof(int), 1, f);
    fread(&(s->saved_x), sizeof(int), 1, f);
    fread(&(s->saved_y), sizeof(int), 1, f);
    fread(&(s->backscroll), sizeof(int), 1, f);
    fread(&(s->total_height), sizeof(int), 1, f);
    fread(&(s->cursor_visible), sizeof(char), 1, f);
    fread(&(s->autowrap), sizeof(char), 1, f);
    fread(&(s->wrapped), sizeof(char), 1, f);
    fread(&(s->insert_mode), sizeof(int), 1, f);
    fread(&(s->cursorkey_mode), sizeof(int), 1, f);
    fread(&(s->display_ctrl), sizeof(char), 1, f);
    fread(&(s->toggle_meta), sizeof(char), 1, f);
    fread(&(s->t_attrib_default), sizeof(TextAttributes), 1, f);
    fread(&(s->t_attrib), sizeof(TextAttributes), 1, f);
    fread(&(s->saved_t_attrib), sizeof(TextAttributes), 1, f);
    fread(s->cells, sizeof(TextCell), s->width * s->total_height, f);
    fread(&(s->state), sizeof(int), 1, f);
    fread(s->esc_params, sizeof(int), MAX_ESC_PARAMS, f);
    fread(&(s->nb_esc_params), sizeof(int), 1, f);
    fread(&(s->has_esc_param), sizeof(int), 1, f);
    fread(&(s->has_qmark), sizeof(int), 1, f);
    fread(s->selections, sizeof(struct selection), 2, f);
    fread(&(s->selecting), sizeof(int), 1, f);
    fread(&(s->mouse_x), sizeof(int), 1, f);
    fread(&(s->mouse_y), sizeof(int), 1, f);
    fread(&(s->unicodeIndex), sizeof(int), 1, f);
    fread(s->unicodeData, sizeof(char), 7, f);
    fread(&(s->unicodeLength), sizeof(int), 1, f);
    fclose(f);

    /* sanitize values */
    clip_to(s->unicodeLength, 0, sizeof(s->unicodeData));
    clip_to(s->unicodeIndex, 0, s->unicodeLength);
    clip_to(s->sr_bottom, 0, s->height - 1);
    clip_to(s->sr_top, 0, s->height - 1);
    clip_to(s->y_base, 0, s->total_height);
    clip_to(s->backscroll, 0, s->total_height - s->height);
    clip_to(s->y_scroll, 0, s->backscroll);
    clip_to(s->x, 0, s->width - 1);
    clip_to(s->y, 0, s->height - 1);
    clip_to(s->saved_x, 0, s->width - 1);
    clip_to(s->saved_y, 0, s->height - 1);
    clip_to(s->mouse_x, -1, s->width - 1);
    clip_to(s->mouse_y, -1, s->height - 1);
    clip_to(s->cursor_visible, 0, 1);
    clip_to(s->autowrap, 0, 1);
    clip_to(s->insert_mode, 0, 1);
    clip_to(s->nb_esc_params, 0, MAX_ESC_PARAMS);
    clip_to(s->has_esc_param, 0, 1);
    clip_to(s->has_qmark, 0, 1);
    clip_to(s->state, 0, TTY_STATE_MAX);
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
        console_scroll(s, -1);
        break;
    case QEMU_KEY_CTRL_DOWN:
        console_scroll(s, 1);
        break;
    case QEMU_KEY_SHIFT_PAGEUP:
        console_scroll(s, -10);
        break;
    case QEMU_KEY_SHIFT_PAGEDOWN:
        console_scroll(s, 10);
        break;
    default:
        /* convert the QEMU keysym to VT100 key string */
        q = buf;
	switch (keysym) {
        case QEMU_KEY_BACKSPACE:
            /* following closely the linux term "standard" */
            *q++ = 0x7f;
            break;
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
        case 0xffb0 ... 0xffb9: /* keypad numbers from 0 to 9 */
            *q++ = (keysym & 0x00ff) - 0xb0 + 0x30;
            break;
        case 0xffbe ... 0xffc2: /* F1 to F5 */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '[';
            *q++ = 'A' + (keysym & 0xff) - 0xbe;
            break;
        case 0xffc3 ... 0xffc5: /* F6 to F8 */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '1';
            *q++ = '7' + (keysym & 0xff) - 0xc3;
            *q++ = '~';
            break;
        case 0xffc6: /* F9 */
        case 0xffc7: /* F10 */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '2';
            *q++ = '0' + (keysym & 0xff) - 0xc6;
            *q++ = '~';
            break;
        case 0xffc8 ... 0xffcb: /* F11 to F14 */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '2';
            *q++ = '3' + (keysym & 0xff) - 0xc8;
            *q++ = '~';
            break;
        case 0xff95: /* KP_Home */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '1';
            *q++ = '~';
            break;
        case 0xff96: /* KP_Left */
            *q++ = '\033';
            *q++ = '[';
            *q++ = 'D';
            break;
        case 0xff97: /* KP_Up */
            *q++ = '\033';
            *q++ = '[';
            *q++ = 'A';
            break;
        case 0xff98: /* KP_Right */
            *q++ = '\033';
            *q++ = '[';
            *q++ = 'C';
            break;
        case 0xff99: /* KP_Down */
            *q++ = '\033';
            *q++ = '[';
            *q++ = 'B';
            break;
        case 0xff9c: /* KP_End */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '4';
            *q++ = '~';
            break;
        case 0xff9b: /* KP_Next (PgDown) */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '6';
            *q++ = '~';
            break;
        case 0xff9d: /* Ignore KP_Begin (alternative to 5) */
            break;
        case 0xff7f: /* Ignore Num_Lock */
            break;
        case 0xffae: /* KP_Decimal */
            *q++ = '.';
            break;
        case 0xff9e: /* KP_Insert */
        case 0xff63: /* Insert */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '4';
            if (!insertmode) {
                *q++ = 'h';
                insertmode = 1;
            } else {
                *q++ = 'l';
                insertmode = 0;
            }
            break;
        case 0xff9f: /* KP_Delete */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '3';
            *q++ = '~';
            break;
        case 0xff8d: /* KP_Enter */
            *q++ = 0x0d;
            break;
        case 0xffab: /* KP_Add */
            *q++ = '+';
            break;
        case 0xff9a: /* KP_Prior (PgUp) */
            *q++ = '\033';
            *q++ = '[';
            *q++ = '5';
            *q++ = '~';
            break;
        case 0xffaf: /* KP_Divide */
            *q++ = '/';
            break;
        case 0xffaa: /* KP_Multiply */
            *q++ = '*';
            break;
        case 0xffad: /* KP_Subtract */
            *q++ = '-';
            break;    
	default:
	    *q++ = keysym;
        }

	for (c = 0; c < q - buf; c++)
	    dprintf("fchar %c %x\n", buf[c] > 0x1f ? buf[c] : ' ', buf[c]);

	dprintf("write_or_chunk(%d, %ld)\n", s->input_stream.fd, (long int)(q-buf));

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
    memset(s, 0, sizeof(TextConsole));

    if (!active_console || (active_console->text_console && !text))
        active_console = s;
    s->ds = ds;
    s->cells = NULL;
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

static void set_color_table(DisplayState *ds)
{
    int i, j;
    for(j = 0; j < 2; j++) {
	for(i = 0; i < 8; i++) {
	    color_table[j][i] =
		col_expand(ds, vga_get_color(ds, color_table_rgb[j][i]));
	}
    }
}

unsigned char nrof_clients_connected(CharDriverState *chr)
{
    TextConsole *s = chr->opaque;

    return s->ds->dpy_clients_connected(s->ds);
}

CharDriverState *text_console_init(DisplayState *ds)
{
    CharDriverState *chr;
    TextConsole *s;
    static int color_inited;

/* init unicode maps */
//    parse_unicode_map("/usr/share/xen/qemu/cp437_to_uni.trans");
    prepare_console_maps();

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

    s->y_base = DEFAULT_BACKSCROLL/3;
    s->total_height = DEFAULT_BACKSCROLL;

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
    /* by default we love utf */
    s->t_attrib_default.utf = 1;
    s->t_attrib_default.codec[0] = MAPLAT1;
    s->t_attrib_default.codec[1] = MAPGRAF;
    s->t_attrib_default.font = G0;
    s->c_attrib_default.highlit = 0;
    s->c_attrib_default.wrapped = 0;
    s->c_attrib_default.columns = 1;
    s->c_attrib_default.spanned = 0;
    s->unicodeIndex = 0;
    s->unicodeLength = 0;

    /* set current text attributes to default */
    s->t_attrib = s->t_attrib_default;

    text_console_resize(s);
    set_cursor(s, 0, 0);

    return chr;
}

void
console_set_input(CharDriverState *chr, int fd, void *opaque)
{
    TextConsole *s = chr->opaque;
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

