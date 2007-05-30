extern int do_log;

#ifdef DEBUG_CONSOLE
#define dprintf printf
#else
#define dprintf(s, ...) {if (do_log) printf(s, ## __VA_ARGS__);}
#endif
