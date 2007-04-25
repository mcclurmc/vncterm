
#ifndef QEMU

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libvnc.h"

#define	qemu_mallocz(s) calloc(1, (s))
#define	qemu_free(p) free((p))
#define	vnc_mallocz(s) calloc(1, (s))
#define	vnc_free(p) free((p))

#ifndef DEBUG
#define	bios_dir "/usr/share/xen/qemu"
#else
#define	bios_dir "."
#endif

#define	MIN(a,b) (((a)<(b))?(a):(b))

struct QEMUTimer {
};
typedef struct QEMUTimer QEMUTimer;

#else
#include "vl.h"
#define	vnc_mallocz(s) qemu_mallocz(s)
#define	vnc_free(p) qemu_free((p))
#endif

extern char vncpasswd[64];
extern unsigned char challenge[AUTHCHALLENGESIZE];
