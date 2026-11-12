/* macfmt.h - MacBinary, AppleSingle/AppleDouble and BinHex 4.0 support. */
#ifndef MACFMT_H
#define MACFMT_H
#include <stdint.h>
#include "hfs.h"

typedef struct {
    uint8_t  name[32];          /* Pascal, Mac Roman; name[0]==0 if none */
    uint8_t  finfo[16], fxinfo[16];
    int      have_finfo;
    uint32_t crdat, mddat;      /* Mac time, 0 if unknown */
    uint32_t doff, dlen;        /* data fork position in the source (file or buffer) */
    uint32_t roff, rlen;        /* resource fork */
    int      has_data;          /* AppleDouble carries no data fork */
} MacMeta;

uint16_t crc_ccitt(uint16_t crc, const uint8_t *p, uint32_t n);

void mb_header(uint8_t h[128], const HEnt *e);
int  mb_parse(const uint8_t h[128], long filesize, MacMeta *m);    /* 0 = valid MacBinary */

/* AppleDouble v2 header for a file; resource fork follows at *rsrcoff.  Returns header length. */
int  ad_header(uint8_t *buf, int bufsize, const HEnt *e, int single, uint32_t *rsrcoff, uint32_t *dataoff);
/* Parse AppleSingle/AppleDouble; returns 0 if valid. */
int  ad_parse(IOF *f, long size, MacMeta *m);

/* BinHex 4.0: returns 1 if the buffer looks like BinHex. */
int  hqx_detect(const uint8_t *p, int n);
/* Decode a whole BinHex file into a new buffer (*out, free() it); forks at m->doff/m->roff.
   Returns 0 ok, -1 error (msg set). */
int  hqx_decode(IOF *f, long size, MacMeta *m, uint8_t **out, char *msg, int msglen);

#endif
