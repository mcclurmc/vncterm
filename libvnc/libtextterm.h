#ifndef _LIBTEXTTERM_H
#define _LIBTEXTTERM_H

struct TextDisplayState {
    uint8_t *data;
    void *opaque;

    void *(*init_timer)(void (*)(void *), void *);
    uint64_t (*get_clock)(void);
    int (*set_timer)(void *, uint64_t);

    int (*set_fd_handler)(int, int (*)(void *), void (*)(void *),
			  void (*)(void *), void *);
    int (*set_fd_error_handler)(int, void (*)(void *));

    void (*chr_write)(struct TextDisplayState *s, const uint8_t *buf, int len);

};
typedef struct TextDisplayState TextDisplayState;

struct sockaddr;

int text_term_display_init(TextDisplayState *ds, struct sockaddr *sa,
                           int find_unused, char *title);
void text_term_display_set_input(TextDisplayState *ds, int fd, void *opaque);

#endif /* _LIBTEXTTERM_H */
