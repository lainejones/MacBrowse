/* macfmt.c - MacBinary II, AppleSingle/AppleDouble v2 and BinHex 4.0. */
#include "macfmt.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Seconds between 1904-01-01 and 2000-01-01 (AppleDouble date base). */
#define AD_EPOCH 3029529600UL

uint16_t crc_ccitt(uint16_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        int i;
        crc ^= (uint16_t)(*p++) << 8;
        for (i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ------------------------------------------------------------------ MacBinary */

void mb_header(uint8_t h[128], const HEnt *e)
{
    memset(h, 0, 128);
    h[1] = e->name[0];
    memcpy(h + 2, e->name + 1, e->name[0]);
    memcpy(h + 65, e->finfo, 8);            /* type, creator */
    h[73] = e->finfo[8];                    /* Finder flags, high byte */
    memcpy(h + 75, e->finfo + 10, 4);       /* location */
    memcpy(h + 79, e->finfo + 14, 2);       /* folder */
    wb32(h + 83, e->dlen);
    wb32(h + 87, e->rlen);
    wb32(h + 91, e->crdat);
    wb32(h + 95, e->mddat);
    h[101] = e->finfo[9];                   /* Finder flags, low byte */
    h[122] = 129;
    h[123] = 129;
    wb16(h + 124, crc_ccitt(0, h, 124));
}

int mb_parse(const uint8_t h[128], long size, MacMeta *m)
{
    uint32_t dl = be32(h + 83), rl = be32(h + 87), need;
    int i, mb2;
    if (h[0] != 0 || h[74] != 0 || h[82] != 0 || h[1] < 1 || h[1] > 63) return -1;
    if (dl > 0x7FFFFFFUL || rl > 0x7FFFFFFUL) return -1;
    need = 128 + ((dl + 127) & ~127UL) + rl;
    if ((long)need > size || (long)(need + 128 + 127) < size - 512) return -1;
    mb2 = crc_ccitt(0, h, 124) == be16(h + 124);
    if (!mb2) {     /* MacBinary I: bytes 99..125 must be zero */
        for (i = 99; i < 126; i++) if (h[i]) return -1;
    }
    memset(m, 0, sizeof(*m));
    m->name[0] = h[1] > 31 ? 31 : h[1];
    memcpy(m->name + 1, h + 2, m->name[0]);
    memcpy(m->finfo, h + 65, 8);
    m->finfo[8] = h[73];
    m->finfo[9] = mb2 ? h[101] : 0;
    memcpy(m->finfo + 10, h + 75, 4);
    memcpy(m->finfo + 14, h + 79, 2);
    m->finfo[8] &= ~0x01;                   /* clear "on desk" */
    m->have_finfo = 1;
    m->crdat = be32(h + 91);
    m->mddat = be32(h + 95);
    m->doff = 128 + (mb2 ? ((be16(h + 120) + 127) & ~127) : 0);
    m->dlen = dl;
    m->roff = m->doff + ((dl + 127) & ~127UL);
    m->rlen = rl;
    m->has_data = 1;
    return 0;
}

/* ------------------------------------------------------------------ AppleSingle/Double */

int ad_header(uint8_t *b, int bufsize, const HEnt *e, int single, uint32_t *rsrcoff, uint32_t *dataoff)
{
    int n = single ? 5 : 4, off, i = 0;
    uint32_t hl;
    if (bufsize < 26 + 12 * 5 + 32 + 16 + 32) return -1;
    memset(b, 0, 26 + 12 * n);
    wb32(b, single ? 0x00051600UL : 0x00051607UL);
    wb32(b + 4, 0x00020000UL);
    wb16(b + 24, (uint16_t)n);
    off = 26 + 12 * n;
    /* 3: real name */
    wb32(b + 26 + 12 * i, 3); wb32(b + 30 + 12 * i, off); wb32(b + 34 + 12 * i, e->name[0]); i++;
    memcpy(b + off, e->name + 1, e->name[0]); off += e->name[0];
    /* 8: file dates (create, modify, backup, access) relative to 2000 */
    wb32(b + 26 + 12 * i, 8); wb32(b + 30 + 12 * i, off); wb32(b + 34 + 12 * i, 16); i++;
    wb32(b + off, e->crdat - AD_EPOCH);
    wb32(b + off + 4, e->mddat - AD_EPOCH);
    wb32(b + off + 8, e->bkdat ? e->bkdat - AD_EPOCH : 0x80000000UL);
    wb32(b + off + 12, e->mddat - AD_EPOCH);
    off += 16;
    /* 9: Finder info */
    wb32(b + 26 + 12 * i, 9); wb32(b + 30 + 12 * i, off); wb32(b + 34 + 12 * i, 32); i++;
    memcpy(b + off, e->finfo, 16);
    memcpy(b + off + 16, e->fxinfo, 16);
    off += 32;
    hl = (uint32_t)off;
    /* 2: resource fork */
    wb32(b + 26 + 12 * i, 2); wb32(b + 30 + 12 * i, hl); wb32(b + 34 + 12 * i, e->rlen); i++;
    *rsrcoff = hl;
    if (single) {
        /* 1: data fork, after the resource fork */
        wb32(b + 26 + 12 * i, 1); wb32(b + 30 + 12 * i, hl + e->rlen); wb32(b + 34 + 12 * i, e->dlen);
        if (dataoff) *dataoff = hl + e->rlen;
    }
    return (int)hl;
}

int ad_parse(IOF *f, long size, MacMeta *m)
{
    uint8_t h[26 + 12 * 32], buf[64];
    uint32_t magic;
    int n, i;
    memset(m, 0, sizeof(*m));
    if (size < 26 || io_pread(f, 0, h, 26)) return -1;
    magic = be32(h);
    if (magic != 0x00051600UL && magic != 0x00051607UL) return -1;
    n = be16(h + 24);
    if (n > 32 || 26 + 12 * n > size || io_pread(f, 26, h + 26, 12 * n)) return -1;
    m->has_data = magic == 0x00051600UL;
    for (i = 0; i < n; i++) {
        uint32_t id = be32(h + 26 + 12 * i), off = be32(h + 30 + 12 * i), len = be32(h + 34 + 12 * i);
        if ((long)(off + len) > size) return -1;
        switch (id) {
        case 1: m->doff = off; m->dlen = len; break;
        case 2: m->roff = off; m->rlen = len; break;
        case 3:
            if (len > 31) len = 31;
            if (!io_pread(f, off, m->name + 1, len)) m->name[0] = (uint8_t)len;
            break;
        case 8:
            if (len >= 8 && !io_pread(f, off, buf, 8)) {
                m->crdat = be32(buf) + AD_EPOCH;
                m->mddat = be32(buf + 4) + AD_EPOCH;
            }
            break;
        case 9:
            if (len >= 16 && !io_pread(f, off, buf, len >= 32 ? 32 : 16)) {
                memcpy(m->finfo, buf, 16);
                if (len >= 32) memcpy(m->fxinfo, buf + 16, 16);
                m->have_finfo = 1;
            }
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ BinHex 4.0 */

static const char hqxchars[] = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
static const char hqxsig[] = "(This file must be converted with BinHex";

int hqx_detect(const uint8_t *p, int n)
{
    int i, l = (int)strlen(hqxsig);
    for (i = 0; i + l <= n; i++)
        if (!memcmp(p + i, hqxsig, l)) return 1;
    return 0;
}

typedef struct { uint8_t *b; uint32_t n, cap; } Buf;
static int bput(Buf *b, uint8_t c)
{
    if (b->n == b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 65536;
        uint8_t *nb = xrealloc(b->b, nc);
        if (!nb) return -1;
        b->b = nb; b->cap = nc;
    }
    b->b[b->n++] = c;
    return 0;
}

int hqx_decode(IOF *f, long size, MacMeta *m, uint8_t **out, char *msg, int msglen)
{
    uint8_t *src, dec[256];
    Buf o = {0, 0, 0};
    long i;
    int started = 0, bits = 0, rle = 0, havelast = 0;
    uint32_t acc = 0, p, nl, dl, rl;
    uint8_t last = 0;
    memset(m, 0, sizeof(*m));
    *out = NULL;
    src = malloc(size + 1);
    if (!src) { snprintf(msg, msglen, "out of memory"); return -1; }
    if (io_pread(f, 0, src, (uint32_t)size)) { free(src); snprintf(msg, msglen, "read error"); return -1; }
    src[size] = 0;
    memset(dec, 0xFF, sizeof(dec));
    for (i = 0; i < 64; i++) dec[(uint8_t)hqxchars[i]] = (uint8_t)i;
    /* find the signature line, then the opening ':' at the start of a line */
    {
        long s = -1, l = (long)strlen(hqxsig);
        for (i = 0; i + l <= size; i++) if (!memcmp(src + i, hqxsig, l)) { s = i; break; }
        if (s < 0) { free(src); snprintf(msg, msglen, "no BinHex signature"); return -1; }
        for (i = s; i < size; i++)
            if (src[i] == ':' && (src[i - 1] == '\n' || src[i - 1] == '\r')) break;
    }
    for (i++; i < size; i++) {
        uint8_t c = src[i], v;
        if (c == ':') { started = 2; break; }
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        v = dec[c];
        if (v == 0xFF) { free(src); free(o.b); snprintf(msg, msglen, "bad BinHex character"); return -1; }
        started = 1;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            uint8_t byte = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
            if (rle) {
                rle = 0;
                if (byte == 0) { if (bput(&o, 0x90)) goto oom; last = 0x90; havelast = 1; }
                else if (havelast) { while (--byte) if (bput(&o, last)) goto oom; }
            } else if (byte == 0x90) rle = 1;
            else { if (bput(&o, byte)) goto oom; last = byte; havelast = 1; }
        }
    }
    free(src);
    src = NULL;
    if (started != 2) { free(o.b); snprintf(msg, msglen, "BinHex data truncated"); return -1; }
    /* header */
    if (o.n < 22) goto trunc;
    nl = o.b[0];
    if (nl < 1 || nl > 63 || o.n < 22 + nl) goto trunc;
    p = 1 + nl + 1;
    memcpy(m->finfo, o.b + p, 10);          /* type, creator, flags */
    m->have_finfo = 1;
    dl = be32(o.b + p + 10);
    rl = be32(o.b + p + 14);
    if (crc_ccitt(0, o.b, p + 18) != be16(o.b + p + 18)) {
        free(o.b); snprintf(msg, msglen, "BinHex header CRC mismatch"); return -1;
    }
    p += 20;
    if ((uint64_t)p + dl + 2 + rl + 2 > o.n) goto trunc;
    if (crc_ccitt(0, o.b + p, dl) != be16(o.b + p + dl)) { free(o.b); snprintf(msg, msglen, "BinHex data fork CRC mismatch"); return -1; }
    if (crc_ccitt(0, o.b + p + dl + 2, rl) != be16(o.b + p + dl + 2 + rl)) { free(o.b); snprintf(msg, msglen, "BinHex resource fork CRC mismatch"); return -1; }
    m->name[0] = nl > 31 ? 31 : (uint8_t)nl;
    memcpy(m->name + 1, o.b + 1, m->name[0]);
    m->doff = p; m->dlen = dl;
    m->roff = p + dl + 2; m->rlen = rl;
    m->has_data = 1;
    *out = o.b;
    return 0;
trunc:
    free(o.b);
    snprintf(msg, msglen, "BinHex data truncated");
    return -1;
oom:
    free(src); free(o.b);
    snprintf(msg, msglen, "out of memory");
    return -1;
}
