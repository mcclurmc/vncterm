
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvnc/libvnc.h"

#define	qemu_malloc(s) malloc((s))
#define	qemu_mallocz(s) calloc(1, (s))
#define	qemu_free(p) free((p))

struct CharDriverState {
    void *opaque;
    int (*chr_write)(struct CharDriverState *s, const uint8_t *buf, int len);
};
typedef struct CharDriverState CharDriverState;

CharDriverState *text_console_init(DisplayState *);
void kbd_put_keysym(int keysym);
void console_select(unsigned int index);
void console_set_input(CharDriverState *s, int fd, void *opaque);

int mouse_is_absolute(void *);
void mouse_event(int dx, int dy, int dz, int buttons_state, void *opaque);
