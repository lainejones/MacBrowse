/* ops.h - copy operations shared by the CLI (main.c) and the GUI (gui.c). */
#ifndef OPS_H
#define OPS_H
#include "hfs.h"

enum { M_DATA, M_RSRC, M_MACBIN, M_DOUBLE, M_SINGLE };

typedef struct {
    const char *image, *cmd, *to, *as, *type, *creator;
    const char *paths[64];
    int npaths, part, depth;
    int all, raw, force, replace, quiet, verbose, plain, mode, ro, nodouble;
} Opts;

extern Opts O;
extern int broken;          /* set by Ctrl-C (1) or a fatal "full" error (2) */

/* Error output: stderr for the CLI; collected in gui_errbuf when gui_mode is set. */
extern int gui_mode;
extern char gui_errbuf[512];
extern int gui_errcount;
void errf(const char *fmt, ...);

/* Copy a Mac file or folder (recursively) into the AmigaDOS directory destdir. */
int mb_get(HVol *v, const HEnt *e, const char *destdir);
/* Copy an AmigaDOS file or directory into Mac folder parid. */
int mb_put(HVol *v, const char *src, uint32_t parid);

#endif
