/* hfs.c - HFS (Mac OS Standard) volume access for MacBrowse.
 *
 * Reads bare HFS volume images (ShapeShifter file disks, MDB at byte 1024), images with an
 * Apple partition map (hardfiles, CD images) and DiskCopy 4.2 images.  Walks the catalog
 * B-tree and the extents-overflow B-tree, so fragmented files come out complete.
 * Write support: create files and folders, delete them; B-tree insertion with node
 * splits, allocation-bitmap and MDB updates.  Never grows the catalog/extents files.
 */
#include "hfs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void wb16(uint8_t *p, uint16_t x) { p[0] = x >> 8; p[1] = (uint8_t)x; }
void wb32(uint8_t *p, uint32_t x) { p[0] = x >> 24; p[1] = x >> 16; p[2] = x >> 8; p[3] = (uint8_t)x; }

static int fail(HVol *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->err, sizeof(v->err), fmt, ap);
    va_end(ap);
    return -1;
}

/* HFS catalog name ordering (Mac OS RelString, case-insensitive, diacritical-sensitive). */
static const uint8_t caseorder[256] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x22,0x23,0x28,0x29,0x2A,0x2B,0x2C,0x2F,0x30,0x31,0x32,0x33,0x34,0x35,0x36,
    0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x42,0x43,0x44,0x45,0x46,
    0x47,0x48,0x57,0x59,0x5D,0x5F,0x66,0x68,0x6A,0x6C,0x72,0x74,0x76,0x78,0x7A,0x7E,
    0x8C,0x8E,0x90,0x92,0x95,0x97,0x9E,0xA0,0xA2,0xA4,0xA7,0xA9,0xAA,0xAB,0xAC,0xAD,
    0x4E,0x48,0x57,0x59,0x5D,0x5F,0x66,0x68,0x6A,0x6C,0x72,0x74,0x76,0x78,0x7A,0x7E,
    0x8C,0x8E,0x90,0x92,0x95,0x97,0x9E,0xA0,0xA2,0xA4,0xA7,0xAF,0xB0,0xB1,0xB2,0xB3,
    0x4A,0x4C,0x5A,0x60,0x7B,0x7F,0x98,0x4F,0x49,0x51,0x4A,0x4B,0x4C,0x5A,0x60,0x63,
    0x64,0x65,0x6E,0x6F,0x70,0x71,0x7B,0x84,0x85,0x86,0x7F,0x80,0x9A,0x9B,0x9C,0x98,
    0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0x94,0xBB,0xBC,0xBD,0xBE,0xBF,0xC0,0x4D,0x81,
    0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0x55,0x8A,0xCC,0x4D,0x81,
    0xCD,0xCE,0xCF,0xD0,0xD1,0xD2,0xD3,0x26,0x27,0xD4,0x20,0x49,0x4B,0x80,0x82,0x82,
    0xD5,0xD6,0x24,0x25,0x2D,0x2E,0xD7,0xD8,0xA6,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

int hfs_relstring(const uint8_t *a, int alen, const uint8_t *b, int blen)
{
    int i, n = alen < blen ? alen : blen, d;
    for (i = 0; i < n; i++)
        if ((d = (int)caseorder[a[i]] - (int)caseorder[b[i]]) != 0) return d;
    return alen - blen;
}

static int catcmp(const uint8_t *k1, const uint8_t *k2)
{
    uint32_t p1 = be32(k1 + 2), p2 = be32(k2 + 2);
    int l1 = k1[6], l2 = k2[6];
    if (p1 != p2) return p1 < p2 ? -1 : 1;
    if (l1 > 31) l1 = 31;
    if (l2 > 31) l2 = 31;
    return hfs_relstring(k1 + 7, l1, k2 + 7, l2);
}

static int extcmp(const uint8_t *k1, const uint8_t *k2)
{
    uint32_t a, b;
    if (k1[1] != k2[1]) return k1[1] < k2[1] ? -1 : 1;
    a = be32(k1 + 2); b = be32(k2 + 2);
    if (a != b) return a < b ? -1 : 1;
    a = be16(k1 + 6); b = be16(k2 + 6);
    if (a != b) return a < b ? -1 : 1;
    return 0;
}

static int catkey(uint8_t *k, uint32_t par, const uint8_t *pname)
{
    int nl = pname ? pname[0] : 0;
    if (nl > 31) nl = 31;
    k[0] = (uint8_t)(6 + nl);
    k[1] = 0;
    wb32(k + 2, par);
    k[6] = (uint8_t)nl;
    if (nl) memcpy(k + 7, pname + 1, nl);
    return 7 + nl;
}

static int extkey(uint8_t *k, int fork, uint32_t id, uint16_t start)
{
    k[0] = 7; k[1] = (uint8_t)fork; wb32(k + 2, id); wb16(k + 6, start);
    return 8;
}

/* ------------------------------------------------------------------ partitions */

static uint32_t mdb_off_probe(IOF *f, uint32_t base)
{
    static uint8_t b[2];
    if (io_pread(f, base + 1024, b, 2)) return 0;
    return b[0] == 'B' && b[1] == 'D';
}

/* Reads the Apple partition map. Returns entry count, or 0 if none. */
static int read_pmap(IOF *f, HPart *parts, int max)
{
    static uint8_t b[512];
    uint32_t bs, step, cnt, i;
    int n = 0;
    if (io_pread(f, 0, b, 512) || b[0] != 'E' || b[1] != 'R') return 0;
    bs = be16(b + 2);
    if (bs < 512 || (bs & 511)) bs = 512;
    /* Entries normally sit every block_size bytes; some CDs use 512-byte spacing. */
    step = bs;
    if (io_pread(f, step, b, 512) || b[0] != 'P' || b[1] != 'M') {
        step = 512;
        if (io_pread(f, step, b, 512) || b[0] != 'P' || b[1] != 'M') return 0;
    }
    cnt = be32(b + 4);
    if (cnt > 64) cnt = 64;
    for (i = 1; i <= cnt && n < max; i++) {
        if (io_pread(f, i * step, b, 512) || b[0] != 'P' || b[1] != 'M') break;
        parts[n].start = be32(b + 8) * (step / 512);
        parts[n].size = be32(b + 12) * (step / 512);
        memcpy(parts[n].name, b + 16, 32); parts[n].name[32] = 0;
        memcpy(parts[n].type, b + 48, 32); parts[n].type[32] = 0;
        n++;
    }
    return n;
}

int hfs_list_parts(const char *path, HPart *parts, int max)
{
    IOF *f = io_open(path, IO_READ);
    int n;
    if (!f) return -1;
    n = read_pmap(f, parts, max);
    io_close(f);
    return n;
}

/* ------------------------------------------------------------------ forks */

static int ext_add(HExtList *x, uint16_t start, uint16_t count)
{
    if (!count) return 0;
    if (x->n == x->cap) {
        int nc = x->cap ? x->cap * 2 : 8;
        HExt *ne = xrealloc(x->e, nc * sizeof(HExt));
        if (!ne) return -1;
        x->e = ne; x->cap = nc;
    }
    x->e[x->n].start = start;
    x->e[x->n].count = count;
    x->n++;
    x->blocks += count;
    return 0;
}

void hfs_extlist_free(HExtList *x) { free(x->e); memset(x, 0, sizeof(*x)); }

static void ext_unpack(const uint8_t *p, HExt e[3])
{
    int i;
    for (i = 0; i < 3; i++) { e[i].start = be16(p + 4 * i); e[i].count = be16(p + 4 * i + 2); }
}

static void ext_pack(uint8_t *p, const HExt e[3])
{
    int i;
    for (i = 0; i < 3; i++) { wb16(p + 4 * i, e[i].start); wb16(p + 4 * i + 2, e[i].count); }
}

/* Map a fork byte offset to an image byte offset; *run = contiguous bytes available. */
static int fork_map(HVol *v, const HExtList *x, uint32_t off, uint32_t *img, uint32_t *run)
{
    uint32_t blk = off / v->alsize, within = off % v->alsize;
    int i;
    for (i = 0; i < x->n; i++) {
        if (blk < x->e[i].count) {
            *img = v->base + (uint32_t)v->alst * 512 + (x->e[i].start + blk) * v->alsize + within;
            *run = (x->e[i].count - blk) * v->alsize - within;
            return 0;
        }
        blk -= x->e[i].count;
    }
    return -1;
}

int hfs_fork_read(HVol *v, const HExtList *x, uint32_t off, void *buf, uint32_t len)
{
    uint8_t *p = buf;
    while (len) {
        uint32_t img, run, n;
        if (fork_map(v, x, off, &img, &run)) return fail(v, "read beyond fork extents");
        n = len < run ? len : run;
        if (io_pread(v->f, img, p, n)) return fail(v, "image read error at %lu", (unsigned long)img);
        p += n; off += n; len -= n;
    }
    return 0;
}

static int fork_write(HVol *v, const HExtList *x, uint32_t off, const void *buf, uint32_t len)
{
    const uint8_t *p = buf;
    while (len) {
        uint32_t img, run, n;
        if (fork_map(v, x, off, &img, &run)) return fail(v, "write beyond fork extents");
        n = len < run ? len : run;
        if (io_pwrite(v->f, img, p, n)) return fail(v, "image write error at %lu", (unsigned long)img);
        p += n; off += n; len -= n;
    }
    return 0;
}

/* ------------------------------------------------------------------ B-tree nodes */

static int nrecs(const uint8_t *nd) { return be16(nd + 10); }
static uint16_t roff(const uint8_t *nd, int i) { return be16(nd + HFS_NODESIZE - 2 * (i + 1)); }
static int rlen(const uint8_t *nd, int i) { return roff(nd, i + 1) - roff(nd, i); }
static int keytot(const uint8_t *rec) { return (1 + rec[0] + 1) & ~1; }  /* key incl. pad */

static int node_ok(const uint8_t *nd)
{
    int n = nrecs(nd), i;
    if (n * 2 + 2 > HFS_NODESIZE - 14) return 0;
    for (i = 0; i <= n; i++) {
        uint16_t o = roff(nd, i);
        if (o < 14 || o > HFS_NODESIZE - 2 * (n + 1)) return 0;
        if (i && o < roff(nd, i - 1)) return 0;
    }
    return 1;
}

/* Small direct-mapped cache of B-tree nodes (write-through).  The handler walks the
   catalog a lot; this saves most of the image reads. */
#define NC_SLOTS 64
typedef struct { const HTree *t; uint32_t n; uint8_t d[HFS_NODESIZE]; } NCache;
static NCache ncache[NC_SLOTS];

static void nc_reset(void)
{
    int i;
    for (i = 0; i < NC_SLOTS; i++) ncache[i].t = NULL;
}

static NCache *nc_slot(const HTree *t, uint32_t n) { return &ncache[(n ^ (t->cnid << 4)) & (NC_SLOTS - 1)]; }

static int node_read(HTree *t, uint32_t n, uint8_t *buf)
{
    NCache *c;
    if (n >= t->total) return fail(t->v, "B-tree node %lu out of range", (unsigned long)n);
    c = nc_slot(t, n);
    if (c->t == t && c->n == n) { memcpy(buf, c->d, HFS_NODESIZE); return 0; }
    if (hfs_fork_read(t->v, &t->ext, n * HFS_NODESIZE, buf, HFS_NODESIZE)) return -1;
    if (!node_ok(buf)) return fail(t->v, "corrupt B-tree node %lu (file %lu)", (unsigned long)n, (unsigned long)t->cnid);
    c->t = t; c->n = n;
    memcpy(c->d, buf, HFS_NODESIZE);
    return 0;
}

static int node_write(HTree *t, uint32_t n, const uint8_t *buf)
{
    NCache *c = nc_slot(t, n);
    if (fork_write(t->v, &t->ext, n * HFS_NODESIZE, buf, HFS_NODESIZE)) { c->t = NULL; return -1; }
    c->t = t; c->n = n;
    memcpy(c->d, buf, HFS_NODESIZE);
    return 0;
}

typedef struct { const uint8_t *p; int len; } NRec;

/* Rebuild a node from a descriptor (first 14 bytes of desc) and a record list. */
static int node_pack(uint8_t *out, const uint8_t *desc, const NRec *r, int n)
{
    int i, off = 14, sz = 14 + 2 * (n + 1);
    static uint8_t tmp[HFS_NODESIZE];
    for (i = 0; i < n; i++) sz += r[i].len;
    if (sz > HFS_NODESIZE) return -1;
    memset(tmp, 0, sizeof(tmp));
    memcpy(tmp, desc, 14);
    wb16(tmp + 10, (uint16_t)n);
    for (i = 0; i < n; i++) {
        wb16(tmp + HFS_NODESIZE - 2 * (i + 1), (uint16_t)off);
        memcpy(tmp + off, r[i].p, r[i].len);
        off += r[i].len;
    }
    wb16(tmp + HFS_NODESIZE - 2 * (n + 1), (uint16_t)off);
    memcpy(out, tmp, HFS_NODESIZE);
    return 0;
}

static void hdr_load(HTree *t)
{
    const uint8_t *h = t->hdr + 14;
    t->depth = be16(h);
    t->root = be32(h + 2);
    t->leafrecs = be32(h + 6);
    t->first = be32(h + 10);
    t->last = be32(h + 14);
    t->maxkey = be16(h + 20);
    t->total = be32(h + 22);
    t->freen = be32(h + 26);
}

static int hdr_write(HTree *t)
{
    uint8_t *h = t->hdr + 14;
    wb16(h, t->depth);
    wb32(h + 2, t->root);
    wb32(h + 6, t->leafrecs);
    wb32(h + 10, t->first);
    wb32(h + 14, t->last);
    wb32(h + 26, t->freen);
    return node_write(t, 0, t->hdr);
}

/* Node allocation map: op 0 = test, 1 = set, 2 = clear.  Returns bit, or -1. */
static int map_op(HTree *t, uint32_t node, int op)
{
    static uint8_t buf[HFS_NODESIZE];
    uint8_t *b = t->hdr;
    uint32_t base = 0, cur = 0;
    int mo = roff(t->hdr, 2), ml = rlen(t->hdr, 2), guard = 0;
    for (;;) {
        if (node < base + (uint32_t)ml * 8) {
            uint32_t bit = node - base;
            uint8_t m = 0x80 >> (bit & 7);
            int old = (b[mo + bit / 8] & m) != 0;
            if (op == 1) b[mo + bit / 8] |= m;
            else if (op == 2) b[mo + bit / 8] &= ~m;
            if (op && cur && node_write(t, cur, b)) return -1;
            return old;
        }
        base += (uint32_t)ml * 8;
        cur = be32(b);
        if (!cur || ++guard > 1000) return -1;
        if (node_read(t, cur, buf)) return -1;
        b = buf;
        mo = roff(b, 0);
        ml = rlen(b, 0);
    }
}

static uint32_t node_alloc(HTree *t)
{
    uint32_t i;
    static uint8_t z[HFS_NODESIZE];
    if (!t->freen) return 0;
    for (i = 1; i < t->total; i++) {
        int b = map_op(t, i, 0);
        if (b < 0) break;
        if (!b) {
            if (map_op(t, i, 1) < 0) return 0;
            t->freen--;
            memset(z, 0, sizeof(z));
            if (node_write(t, i, z)) return 0;
            return i;
        }
    }
    return 0;
}

static int node_free(HTree *t, uint32_t n)
{
    static uint8_t z[HFS_NODESIZE];
    memset(z, 0, sizeof(z));
    node_write(t, n, z);
    if (map_op(t, n, 2) < 0) return -1;
    t->freen++;
    return 0;
}

static int tree_open(HVol *v, HTree *t, uint32_t cnid, const uint8_t *extrec, uint32_t size)
{
    HExt first[3];
    memset(t, 0, sizeof(*t));
    t->v = v;
    t->cnid = cnid;
    t->size = size;
    t->cmp = cnid == HFS_CAT_ID ? catcmp : extcmp;
    ext_unpack(extrec, first);
    if (cnid == HFS_EXT_ID) {
        int i;
        for (i = 0; i < 3; i++) ext_add(&t->ext, first[i].start, first[i].count);
    } else if (hfs_fork_extents(v, cnid, FORK_DATA, first, size, &t->ext)) return -1;
    t->total = 1;
    if (hfs_fork_read(v, &t->ext, 0, t->hdr, HFS_NODESIZE)) return -1;
    if (t->hdr[8] != 1) return fail(v, "B-tree %lu: bad header node", (unsigned long)cnid);
    hdr_load(t);
    if (be16(t->hdr + 14 + 18) != HFS_NODESIZE)
        return fail(v, "B-tree %lu: unsupported node size %u", (unsigned long)cnid, be16(t->hdr + 14 + 18));
    if (t->total * HFS_NODESIZE > t->ext.blocks * v->alsize)
        return fail(v, "B-tree %lu: file shorter than its node count", (unsigned long)cnid);
    if (t->depth > HFS_MAXDEPTH) return fail(v, "B-tree %lu: depth %u too large", (unsigned long)cnid, t->depth);
    t->fixedidx = 1;
    return 0;
}

/* Descend to the leaf for key.  pn/pi get the node and record index per level
   (index levels: record followed; leaf: insertion position).  *exact set when found.
   Returns 0, or 1 for an empty tree, -1 on error. */
static int bt_search(HTree *t, const uint8_t *key, uint32_t *pn, int *pi, int *exact, uint8_t *leaf)
{
    static uint8_t nd[HFS_NODESIZE];
    uint32_t n = t->root;
    int lvl;
    *exact = 0;
    if (!t->root || !t->depth) return 1;
    for (lvl = 0; lvl < t->depth; lvl++) {
        int i, nr, c;
        if (node_read(t, n, nd)) return -1;
        nr = nrecs(nd);
        pn[lvl] = n;
        if (lvl == t->depth - 1) {
            if (nd[8] != 0xFF) return fail(t->v, "B-tree %lu: expected leaf at node %lu", (unsigned long)t->cnid, (unsigned long)n);
            for (i = 0; i < nr; i++) {
                c = t->cmp(nd + roff(nd, i), key);
                if (c >= 0) { *exact = c == 0; break; }
            }
            pi[lvl] = i;
            if (leaf) memcpy(leaf, nd, HFS_NODESIZE);
            return 0;
        }
        if (nd[8] != 0x00 || !nr) return fail(t->v, "B-tree %lu: expected index at node %lu", (unsigned long)t->cnid, (unsigned long)n);
        for (i = 1; i < nr; i++)
            if (t->cmp(nd + roff(nd, i), key) > 0) break;
        i--;
        pi[lvl] = i;
        {
            const uint8_t *r = nd + roff(nd, i);
            n = be32(r + keytot(r));
        }
    }
    return -1;
}

/* Find a record by exact key; copy it to rec (len to *rlenp). 0 found, 1 not found. */
static int bt_get(HTree *t, const uint8_t *key, uint8_t *rec, int *rlenp)
{
    uint32_t pn[HFS_MAXDEPTH];
    int pi[HFS_MAXDEPTH], ex, r;
    static uint8_t leaf[HFS_NODESIZE];
    r = bt_search(t, key, pn, pi, &ex, leaf);
    if (r) return r < 0 ? -1 : 1;
    if (!ex) return 1;
    {
        int i = pi[t->depth - 1], l = rlen(leaf, i);
        memcpy(rec, leaf + roff(leaf, i), l);
        if (rlenp) *rlenp = l;
    }
    return 0;
}

/* Build an index record pointing at child with the given key. */
static int mkidx(HTree *t, const uint8_t *key, uint32_t ptr, uint8_t *out)
{
    int kl = key[0], off;
    if (t->fixedidx && kl < t->maxkey) {
        memset(out, 0, 1 + t->maxkey + 1);
        memcpy(out, key, 1 + kl);
        out[0] = (uint8_t)t->maxkey;
        kl = t->maxkey;
    } else memcpy(out, key, 1 + kl);
    off = (1 + kl + 1) & ~1;
    if (off > 1 + kl) out[1 + kl] = 0;
    wb32(out + off, ptr);
    return off + 4;
}

/* For catalog index keys the stored name is padded; give back a key with the real length. */
static void leafkey_of(const uint8_t *rec, uint8_t *key)
{
    memcpy(key, rec, 1 + rec[0]);
}

/* Replace the key of index record pi[lvl-1] in node pn[lvl-1] with key; propagate if first. */
static int fixkey(HTree *t, uint32_t *pn, int *pi, int lvl, const uint8_t *key)
{
    static uint8_t nd[HFS_NODESIZE], nr[HFS_NODESIZE / 2];
    static NRec r[HFS_NODESIZE / 2];
    while (lvl > 0) {
        int p = lvl - 1, i, n, idx = pi[p];
        const uint8_t *old;
        if (node_read(t, pn[p], nd)) return -1;
        n = nrecs(nd);
        if (idx >= n) return fail(t->v, "fixkey: bad index");
        old = nd + roff(nd, idx);
        {
            int nl = mkidx(t, key, be32(old + keytot(old)), nr);
            for (i = 0; i < n; i++) { r[i].p = nd + roff(nd, i); r[i].len = rlen(nd, i); }
            r[idx].p = nr; r[idx].len = nl;
        }
        if (node_pack(nd, nd, r, n)) return fail(t->v, "fixkey: index node overflow");
        if (node_write(t, pn[p], nd)) return -1;
        if (idx != 0) break;
        lvl = p;
    }
    return 0;
}

static int bt_insert(HTree *t, const uint8_t *rec, int reclen)
{
    uint32_t pn[HFS_MAXDEPTH];
    int pi[HFS_MAXDEPTH], ex, lvl, pos, r, inslen;
    static uint8_t nd[HFS_NODESIZE], L[HFS_NODESIZE], R[HFS_NODESIZE], ins[HFS_NODESIZE], nextidx[HFS_NODESIZE];
    static NRec rr[HFS_NODESIZE / 2 + 2];

    if (t->freen < (uint32_t)t->depth + 1)
        return fail(t->v, "B-tree %lu is full (%lu free nodes); grow it on the Mac first",
                    (unsigned long)t->cnid, (unsigned long)t->freen);
    r = bt_search(t, rec, pn, pi, &ex, NULL);
    if (r < 0) return -1;
    if (r == 1) {   /* empty tree: new root leaf */
        uint32_t n = node_alloc(t);
        NRec one;
        static uint8_t desc[14];
        if (!n) return fail(t->v, "no free B-tree node");
        memset(desc, 0, 14); desc[8] = 0xFF; desc[9] = 1;
        one.p = rec; one.len = reclen;
        node_pack(nd, desc, &one, 1);
        if (node_write(t, n, nd)) return -1;
        t->root = t->first = t->last = n;
        t->depth = 1;
        t->leafrecs++;
        return hdr_write(t);
    }
    if (ex) return fail(t->v, "already exists");

    memcpy(ins, rec, reclen);
    inslen = reclen;
    lvl = t->depth - 1;
    pos = pi[lvl];
    for (;;) {
        int n, i, j, total, m, acc;
        uint32_t newn, fl;
        if (node_read(t, pn[lvl], nd)) return -1;
        n = nrecs(nd);
        for (i = 0, j = 0; i <= n; i++) {
            if (i == pos) { rr[j].p = ins; rr[j].len = inslen; j++; }
            if (i < n) { rr[j].p = nd + roff(nd, i); rr[j].len = rlen(nd, i); j++; }
        }
        n++;
        if (!node_pack(L, nd, rr, n)) {
            if (node_write(t, pn[lvl], L)) return -1;
            if (pos == 0 && lvl > 0) {
                static uint8_t k[64];
                leafkey_of(L + roff(L, 0), k);
                if (fixkey(t, pn, pi, lvl, k)) return -1;
            }
            break;
        }
        /* Split: left keeps the first half (by bytes), right gets the rest. */
        for (i = 0, total = 0; i < n; i++) total += rr[i].len + 2;
        for (m = 0, acc = 0; m < n - 1; m++) {
            if (acc + rr[m].len + 2 > total / 2 && m > 0) break;
            acc += rr[m].len + 2;
        }
        /* Appending at the end of the rightmost node (sequential inserts): start a new node
           with just the new record, so the old one stays full. */
        if (pos == n - 1 && !be32(nd)) m = n - 1;
        newn = node_alloc(t);
        if (!newn) return fail(t->v, "no free B-tree node");
        fl = be32(nd);
        {
            static uint8_t ld[14], rd[14];
            memcpy(ld, nd, 14); memcpy(rd, nd, 14);
            wb32(ld, newn);                 /* left.fLink = new */
            wb32(rd, fl);                   /* right.fLink = old fLink */
            wb32(rd + 4, pn[lvl]);          /* right.bLink = left */
            if (node_pack(L, ld, rr, m) || node_pack(R, rd, rr + m, n - m))
                return fail(t->v, "B-tree split failed");
        }
        if (fl) {
            static uint8_t nx[HFS_NODESIZE];
            if (node_read(t, fl, nx)) return -1;
            wb32(nx + 4, newn);
            if (node_write(t, fl, nx)) return -1;
        } else if (nd[8] == 0xFF) t->last = newn;
        if (node_write(t, pn[lvl], L) || node_write(t, newn, R)) return -1;
        if (pos == 0 && lvl > 0) {
            static uint8_t k[64];
            leafkey_of(L + roff(L, 0), k);
            if (fixkey(t, pn, pi, lvl, k)) return -1;
        }
        {
            static uint8_t k[64];
            leafkey_of(R + roff(R, 0), k);
            inslen = mkidx(t, k, newn, nextidx);
        }
        if (lvl == 0) {     /* root split: new root */
            static uint8_t desc[14], k[64], a[64];
            uint32_t nr = node_alloc(t);
            int al;
            static NRec two[2];
            if (!nr) return fail(t->v, "no free B-tree node");
            leafkey_of(L + roff(L, 0), k);
            al = mkidx(t, k, pn[0], a);
            two[0].p = a; two[0].len = al;
            two[1].p = nextidx; two[1].len = inslen;
            memset(desc, 0, 14);
            desc[8] = 0x00;
            desc[9] = (uint8_t)(nd[9] + 1);
            if (node_pack(nd, desc, two, 2)) return fail(t->v, "root build failed");
            if (node_write(t, nr, nd)) return -1;
            t->root = nr;
            t->depth++;
            break;
        }
        memcpy(ins, nextidx, inslen);
        lvl--;
        pos = pi[lvl] + 1;
    }
    t->leafrecs++;
    return hdr_write(t);
}

static int bt_delete(HTree *t, const uint8_t *key)
{
    uint32_t pn[HFS_MAXDEPTH];
    int pi[HFS_MAXDEPTH], ex, lvl, pos, r;
    static uint8_t nd[HFS_NODESIZE];
    static NRec rr[HFS_NODESIZE / 2];

    r = bt_search(t, key, pn, pi, &ex, NULL);
    if (r < 0) return -1;
    if (r == 1 || !ex) return fail(t->v, "record not found");
    lvl = t->depth - 1;
    pos = pi[lvl];
    for (;;) {
        int n, i, j;
        if (node_read(t, pn[lvl], nd)) return -1;
        n = nrecs(nd);
        for (i = 0, j = 0; i < n; i++)
            if (i != pos) { rr[j].p = nd + roff(nd, i); rr[j].len = rlen(nd, i); j++; }
        n--;
        if (n > 0) {
            static uint8_t out[HFS_NODESIZE];
            node_pack(out, nd, rr, n);
            if (node_write(t, pn[lvl], out)) return -1;
            if (pos == 0 && lvl > 0) {
                static uint8_t k[64];
                leafkey_of(out + roff(out, 0), k);
                if (fixkey(t, pn, pi, lvl, k)) return -1;
            }
            break;
        }
        /* node became empty: unlink and free it */
        {
            uint32_t fl = be32(nd), bl = be32(nd + 4);
            static uint8_t sib[HFS_NODESIZE];
            int leaf = nd[8] == 0xFF;
            if (bl) {
                if (node_read(t, bl, sib)) return -1;
                wb32(sib, fl);
                if (node_write(t, bl, sib)) return -1;
            } else if (leaf) t->first = fl;
            if (fl) {
                if (node_read(t, fl, sib)) return -1;
                wb32(sib + 4, bl);
                if (node_write(t, fl, sib)) return -1;
            } else if (leaf) t->last = bl;
            if (node_free(t, pn[lvl])) return -1;
        }
        if (lvl == 0) {     /* tree now empty */
            t->root = 0; t->depth = 0; t->first = t->last = 0;
            break;
        }
        lvl--;
        pos = pi[lvl];
    }
    t->leafrecs--;
    return hdr_write(t);
}

/* Overwrite the data part of an existing leaf record (same length). */
static int bt_update(HTree *t, const uint8_t *key, const uint8_t *rec, int reclen)
{
    uint32_t pn[HFS_MAXDEPTH];
    int pi[HFS_MAXDEPTH], ex, i;
    static uint8_t leaf[HFS_NODESIZE];
    if (bt_search(t, key, pn, pi, &ex, leaf) || !ex) return fail(t->v, "record not found");
    i = pi[t->depth - 1];
    if (rlen(leaf, i) != reclen) return fail(t->v, "record size mismatch");
    memcpy(leaf + roff(leaf, i), rec, reclen);
    return node_write(t, pn[t->depth - 1], leaf);
}

/* ------------------------------------------------------------------ extents */

int hfs_fork_extents(HVol *v, uint32_t cnid, int fork, const HExt first[3], uint32_t len, HExtList *out)
{
    uint32_t need = (len + v->alsize - 1) / v->alsize;
    int i, guard = 0;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < 3; i++)
        if (first[i].count && ext_add(out, first[i].start, first[i].count)) return fail(v, "out of memory");
    while (out->blocks < need) {
        static uint8_t key[8], rec[HFS_NODESIZE];
        HExt e[3];
        int r;
        if (cnid == HFS_EXT_ID || !v->ext.total) return fail(v, "file %lu needs overflow extents but none available", (unsigned long)cnid);
        extkey(key, fork, cnid, (uint16_t)out->blocks);
        r = bt_get(&v->ext, key, rec, NULL);
        if (r < 0) return -1;
        if (r) return fail(v, "file %lu: missing extents record at block %lu", (unsigned long)cnid, (unsigned long)out->blocks);
        ext_unpack(rec + 8, e);
        for (i = 0; i < 3; i++)
            if (e[i].count && ext_add(out, e[i].start, e[i].count)) return fail(v, "out of memory");
        if (++guard > 10000) return fail(v, "extents loop");
    }
    return 0;
}

/* ------------------------------------------------------------------ catalog */

static int parse_ent(const uint8_t *rec, int len, HEnt *e)
{
    const uint8_t *d;
    int nl = rec[6];
    memset(e, 0, sizeof(*e));
    if (nl > 31) nl = 31;
    e->parid = be32(rec + 2);
    e->name[0] = (uint8_t)nl;
    memcpy(e->name + 1, rec + 7, nl);
    d = rec + keytot(rec);
    len -= keytot(rec);
    e->type = d[0];
    if (e->type == CDR_DIR && len >= 70) {
        e->flags = d[2];
        e->valence = be16(d + 4);
        e->id = be32(d + 6);
        e->crdat = be32(d + 10);
        e->mddat = be32(d + 14);
        e->bkdat = be32(d + 18);
        memcpy(e->finfo, d + 22, 16);
        memcpy(e->fxinfo, d + 38, 16);
        memcpy(e->raw, d, 70);
        return 0;
    }
    if (e->type == CDR_FILE && len >= 102) {
        e->flags = d[2];
        memcpy(e->finfo, d + 4, 16);
        e->id = be32(d + 20);
        e->dlen = be32(d + 26);
        e->dpyl = be32(d + 30);
        e->rlen = be32(d + 36);
        e->rpyl = be32(d + 40);
        e->crdat = be32(d + 44);
        e->mddat = be32(d + 48);
        e->bkdat = be32(d + 52);
        memcpy(e->fxinfo, d + 56, 16);
        ext_unpack(d + 74, e->dext);
        ext_unpack(d + 86, e->rext);
        memcpy(e->raw, d, 102);
        return 0;
    }
    return -1;
}

int hfs_lookup(HVol *v, uint32_t parid, const uint8_t *pname, HEnt *e)
{
    static uint8_t key[40], rec[HFS_NODESIZE];
    int l, r;
    catkey(key, parid, pname);
    r = bt_get(&v->cat, key, rec, &l);
    if (r) return r;
    return parse_ent(rec, l, e) ? 1 : 0;
}

int hfs_get_id(HVol *v, uint32_t id, HEnt *e)
{
    static uint8_t key[40], rec[HFS_NODESIZE], nm[32];
    const uint8_t *d;
    int l, r;
    catkey(key, id, NULL);
    r = bt_get(&v->cat, key, rec, &l);
    if (r) return r;
    d = rec + keytot(rec);
    if (d[0] != CDR_DTHREAD && d[0] != CDR_FTHREAD) return 1;
    memcpy(nm, d + 14, 32);
    if (nm[0] > 31) nm[0] = 31;
    return hfs_lookup(v, be32(d + 10), nm, e);
}

int hfs_resolve(HVol *v, const char *path, HEnt *e)
{
    uint32_t cur = HFS_ROOT_ID;
    const char *p = path;
    int first = 1;
    if (hfs_get_id(v, HFS_ROOT_ID, e)) return fail(v, "root folder not found");
    while (*p == ':') p++;
    while (*p) {
        static uint8_t nm[32];
        const char *s = p;
        int n = 0, r;
        while (*p && *p != ':') p++;
        n = (int)(p - s);
        while (*p == ':') p++;
        if (n == 0) continue;
        if (n > 31) return fail(v, "name too long: %.*s", n, s);
        nm[0] = (uint8_t)n;
        memcpy(nm + 1, s, n);
        if (e->type != CDR_DIR) return fail(v, "not a folder: %.*s", (int)e->name[0], e->name + 1);
        r = hfs_lookup(v, cur, nm, e);
        if (r < 0) return -1;
        if (r) {
            if (first && !hfs_relstring(nm + 1, nm[0], v->name + 1, v->name[0])) {
                hfs_get_id(v, HFS_ROOT_ID, e);
                first = 0;
                continue;
            }
            return fail(v, "not found: %.*s", n, s);
        }
        first = 0;
        cur = e->id;
    }
    return 0;
}

int hfs_readdir(HVol *v, uint32_t dirid, hfs_dircb cb, void *ud)
{
    uint32_t pn[HFS_MAXDEPTH];
    int pi[HFS_MAXDEPTH], ex, i, r, guard = 0;
    uint8_t key[40], *nd;
    HEnt *e;
    HTree *t = &v->cat;
    /* callbacks may recurse into hfs_readdir: keep per-call buffers off the stack */
    nd = malloc(HFS_NODESIZE + sizeof(HEnt));
    if (!nd) return fail(v, "out of memory");
    e = (HEnt *)(nd + HFS_NODESIZE);
    catkey(key, dirid, NULL);
    r = bt_search(t, key, pn, pi, &ex, nd);
    if (r) { free(nd); return r < 0 ? -1 : 0; }
    i = pi[t->depth - 1];
    for (;;) {
        for (; i < nrecs(nd); i++) {
            const uint8_t *rec = nd + roff(nd, i);
            if (be32(rec + 2) != dirid) { free(nd); return 0; }
            if (!parse_ent(rec, rlen(nd, i), e) && (r = cb(v, e, ud))) { free(nd); return r; }
        }
        if (!be32(nd) || ++guard > 1000000) { free(nd); return 0; }
        if (node_read(t, be32(nd), nd)) { free(nd); return -1; }
        i = 0;
    }
}

int hfs_walk_all(HVol *v, hfs_dircb cb, void *ud)
{
    static uint8_t nd[HFS_NODESIZE];
    uint32_t n = v->cat.first;
    int i, r;
    uint32_t guard = 0;
    while (n) {
        if (node_read(&v->cat, n, nd)) return -1;
        for (i = 0; i < nrecs(nd); i++) {
            HEnt e;
            if (!parse_ent(nd + roff(nd, i), rlen(nd, i), &e) && (r = cb(v, &e, ud))) return r;
        }
        n = be32(nd);
        if (++guard > v->cat.total) return fail(v, "catalog leaf chain loops");
    }
    return 0;
}

/* ------------------------------------------------------------------ open/close */

static int find_volume(HVol *v, int part)
{
    static HPart parts[64];
    int n = read_pmap(v->f, parts, 64), i, k = 0;
    long sz = io_size(v->f);
    if (n > 0) {
        for (i = 0; i < n; i++) {
            if (strcmp(parts[i].type, "Apple_HFS")) continue;
            if (part >= 0 ? k++ == part : mdb_off_probe(v->f, parts[i].start * 512)) {
                v->base = parts[i].start * 512;
                v->secs = parts[i].size;
                if (!mdb_off_probe(v->f, v->base)) return fail(v, "partition %d is not HFS", i);
                return 0;
            }
        }
        return fail(v, "no HFS partition found in the partition map");
    }
    if (mdb_off_probe(v->f, 0)) v->base = 0;
    else if (mdb_off_probe(v->f, 84)) v->base = 84;    /* DiskCopy 4.2 */
    else return fail(v, "not an HFS volume image (no MDB at offset 1024)");
    v->secs = (uint32_t)((sz - (long)v->base) / 512);
    return 0;
}

int hfs_open(HVol *v, const char *path, int rw, int part)
{
    uint16_t atrb;
    int excl = rw & HFS_EXCL;
    rw &= 3;
    memset(v, 0, sizeof(*v));
    nc_reset();
    v->rw = rw;
    v->f = io_open(path, rw ? IO_UPDATE : excl ? IO_READX : IO_READ);
    if (!v->f) {
        if (rw || excl) return fail(v, "cannot open %s %s(%s) - is it in use by ShapeShifter?", path, rw ? "for writing " : "", io_errstr());
        return fail(v, "cannot open %s (%s)", path, io_errstr());
    }
    if (find_volume(v, part)) goto bad;
    if (io_pread(v->f, v->base + 1024, v->mdb, 512)) { fail(v, "cannot read MDB"); goto bad; }
    v->alsize = be32(v->mdb + 20);
    v->alst = be16(v->mdb + 28);
    v->nblocks = be16(v->mdb + 18);
    v->vbmst = be16(v->mdb + 14);
    if (!v->alsize || (v->alsize & 511)) { fail(v, "bad allocation block size %lu", (unsigned long)v->alsize); goto bad; }
    memcpy(v->name, v->mdb + 36, 28);
    if (v->name[0] > 27) v->name[0] = 27;
    v->name[v->name[0] + 1] = 0;
    if (be16(v->mdb + 124) == 0x482B && rw) { fail(v, "HFS+ volume (HFS wrapper): writing not supported"); goto bad; }
    if (tree_open(v, &v->ext, HFS_EXT_ID, v->mdb + 134, be32(v->mdb + 130))) goto bad;
    if (tree_open(v, &v->cat, HFS_CAT_ID, v->mdb + 150, be32(v->mdb + 146))) goto bad;
    /* Detect whether catalog index keys are padded to maxKeyLen (Apple HFS always does). */
    if (v->cat.depth > 1) {
        static uint8_t nd[HFS_NODESIZE];
        if (!node_read(&v->cat, v->cat.root, nd) && nrecs(nd))
            v->cat.fixedidx = (nd + roff(nd, 0))[0] == v->cat.maxkey;
    }
    if (rw) {
        uint32_t bytes = ((uint32_t)v->nblocks + 4095) / 4096 * 512;
        atrb = be16(v->mdb + 10);
        if (atrb & 0x8080) { fail(v, "volume is locked"); goto bad; }
        if (!(atrb & 0x0100) && rw < 2) {
            fail(v, "volume is marked as mounted (not cleanly unmounted):\nShapeShifter may be using it");
            goto bad;
        }
        v->bitmap = malloc(bytes);
        if (!v->bitmap || io_pread(v->f, v->base + (uint32_t)v->vbmst * 512, v->bitmap, bytes)) {
            fail(v, "cannot read the allocation bitmap"); goto bad;
        }
    }
    return 0;
bad:
    if (v->f) io_close(v->f);
    v->f = NULL;
    hfs_extlist_free(&v->ext.ext);
    hfs_extlist_free(&v->cat.ext);
    free(v->bitmap); v->bitmap = NULL;
    return -1;
}

static int mdb_write(HVol *v)
{
    static uint8_t alt[512];
    uint32_t altoff = v->base + (v->secs - 2) * 512;
    if (io_pwrite(v->f, v->base + 1024, v->mdb, 512)) return fail(v, "cannot write MDB");
    if (v->secs > 4 && !io_pread(v->f, altoff, alt, 512) && alt[0] == 'B' && alt[1] == 'D')
        if (io_pwrite(v->f, altoff, v->mdb, 512)) return fail(v, "cannot write alternate MDB");
    return 0;
}

/* First modification: mark the volume as mounted/dirty so an interrupted write is noticed. */
static int begin_write(HVol *v)
{
    if (!v->rw) return fail(v, "volume opened read-only");
    if (!v->dirty) {
        v->dirty = 1;
        wb16(v->mdb + 10, be16(v->mdb + 10) & ~0x0100);
        if (mdb_write(v)) return -1;
    }
    return 0;
}

/* Write the bitmap and both MDB copies and mark the volume cleanly unmounted again.
   The next modification clears the flag again (begin_write). */
int hfs_flush(HVol *v)
{
    int r = 0;
    if (!v->f || !v->rw || !v->dirty) return 0;
    {
        uint32_t bytes = ((uint32_t)v->nblocks + 4095) / 4096 * 512;
        if (v->bmdirty) {
            if (io_pwrite(v->f, v->base + (uint32_t)v->vbmst * 512, v->bitmap, bytes))
                r = fail(v, "cannot write the allocation bitmap");
            else v->bmdirty = 0;
        }
    }
    wb32(v->mdb + 6, io_mactime());
    wb32(v->mdb + 70, be32(v->mdb + 70) + 1);
    if (!r) wb16(v->mdb + 10, be16(v->mdb + 10) | 0x0100);
    if (mdb_write(v)) r = -1;
    if (!r) v->dirty = 0;
    return r;
}

int hfs_close(HVol *v)
{
    int r = hfs_flush(v);
    if (v->f) io_close(v->f);
    v->f = NULL;
    hfs_extlist_free(&v->ext.ext);
    hfs_extlist_free(&v->cat.ext);
    free(v->bitmap);
    v->bitmap = NULL;
    nc_reset();
    return r;
}

/* ------------------------------------------------------------------ allocation */

static int bm_get(HVol *v, uint32_t b) { return (v->bitmap[b >> 3] >> (7 - (b & 7))) & 1; }
static void bm_put(HVol *v, uint32_t b, int on)
{
    if (on) v->bitmap[b >> 3] |= 0x80 >> (b & 7);
    else v->bitmap[b >> 3] &= ~(0x80 >> (b & 7));
    v->bmdirty = 1;
}

static void blocks_mark(HVol *v, const HExt *e, int n, int on)
{
    int i;
    uint32_t b, delta = 0;
    for (i = 0; i < n; i++)
        for (b = e[i].start; b < (uint32_t)e[i].start + e[i].count && b < v->nblocks; b++)
            if (bm_get(v, b) != on) { bm_put(v, b, on); delta++; }
    {
        uint16_t fr = be16(v->mdb + 34);
        wb16(v->mdb + 34, (uint16_t)(on ? fr - delta : fr + delta));
    }
}

uint32_t hfs_free_blocks(HVol *v) { return be16(v->mdb + 34); }

/* Allocate one run of free blocks (1..want).  Prefers the run starting at 'near' (so a
   fork grows contiguously), then the first run of at least 'want' blocks from the
   allocation pointer, then the largest run on the volume. */
static int alloc_run(HVol *v, uint32_t near, uint32_t want, HExt *out)
{
    uint32_t b, s, best = 0, bestlen = 0, ptr, pass;
    if (want > 65535) want = 65535;
    if (!want) return fail(v, "alloc_run: nothing wanted");
    if (near < v->nblocks && !bm_get(v, near)) {
        for (b = near; b < v->nblocks && b - near < want && !bm_get(v, b); b++) ;
        out->start = (uint16_t)near; out->count = (uint16_t)(b - near);
        goto take;
    }
    ptr = be16(v->mdb + 16);
    if (ptr >= v->nblocks) ptr = 0;
    for (pass = 0; pass < 2; pass++) {
        uint32_t from = pass ? 0 : ptr, to = pass ? ptr : v->nblocks;
        for (b = from; b < to;) {
            if (v->bitmap[b >> 3] == 0xFF && !(b & 7)) { b += 8; continue; }
            if (bm_get(v, b)) { b++; continue; }
            s = b;
            while (b < to && b - s < want && !bm_get(v, b)) b++;
            if (b - s >= want) { out->start = (uint16_t)s; out->count = (uint16_t)want; goto take; }
            if (b - s > bestlen) { best = s; bestlen = b - s; }
        }
    }
    if (!bestlen) return fail(v, "disk full");
    out->start = (uint16_t)best; out->count = (uint16_t)bestlen;
take:
    blocks_mark(v, out, 1, 1);
    wb16(v->mdb + 16, (uint16_t)(out->start + out->count));
    return 0;
}

/* ------------------------------------------------------------------ open forks */

/* Remove every extents-overflow record of a fork. */
static int ext_purge(HVol *v, uint32_t id, int fork)
{
    int guard = 0;
    if (!v->ext.root) return 0;
    for (;;) {
        uint32_t pn[HFS_MAXDEPTH];
        int pi[HFS_MAXDEPTH], ex, r;
        static uint8_t key[8], leaf[HFS_NODESIZE], k2[8];
        const uint8_t *rec;
        extkey(key, fork, id, 0);
        r = bt_search(&v->ext, key, pn, pi, &ex, leaf);
        if (r < 0) return -1;
        if (r) break;
        if (pi[v->ext.depth - 1] >= nrecs(leaf)) {
            uint32_t nx = be32(leaf);
            if (!nx || node_read(&v->ext, nx, leaf) || !nrecs(leaf)) break;
            rec = leaf + roff(leaf, 0);
        } else rec = leaf + roff(leaf, pi[v->ext.depth - 1]);
        if (rec[1] != fork || be32(rec + 2) != id) break;
        memcpy(k2, rec, 8);
        if (bt_delete(&v->ext, k2)) return -1;
        if (++guard > 10000) break;
    }
    return 0;
}

int hfs_fork_open(HVol *v, const HEnt *e, int fork, HFork *f)
{
    memset(f, 0, sizeof(*f));
    if (e->type != CDR_FILE) return fail(v, "not a file");
    f->v = v;
    f->id = e->id;
    f->parid = e->parid;
    memcpy(f->name, e->name, 32);
    f->fork = fork;
    f->len = fork == FORK_DATA ? e->dlen : e->rlen;
    if (hfs_fork_extents(v, e->id, fork, fork == FORK_DATA ? e->dext : e->rext,
                         fork == FORK_DATA ? e->dpyl : e->rpyl, &f->x)) return -1;
    f->hadover = f->x.n >= 3;
    return 0;
}

void hfs_fork_close(HFork *f) { hfs_extlist_free(&f->x); }

/* Minimum reservation when a fork grows by writing (0 = none; forktest uses that). */
uint32_t hfs_prealloc = 65536;

/* Make the fork cover at least 'need' allocation blocks.  With 'extra' it reserves
   room for further growth (a clump, or a quarter of the size so far), which keeps a
   file written in small pieces in few extents; hfs_fork_sync(trim) gives it back. */
static int fork_grow(HFork *f, uint32_t need, int extra)
{
    HVol *v = f->v;
    uint32_t want = need, have = f->x.blocks;
    if (have >= need) return 0;
    if (need - have > hfs_free_blocks(v))
        return fail(v, "disk full (%lu blocks needed, %lu free)", (unsigned long)(need - have), (unsigned long)hfs_free_blocks(v));
    if (extra && hfs_prealloc) {
        uint32_t g = have / 4, clump = be32(v->mdb + 24) / v->alsize, m = hfs_prealloc / v->alsize;
        if (g < clump) g = clump;
        if (g < m) g = m;
        if (have + g > want) want = have + g;
        if (want - have > hfs_free_blocks(v)) want = need;
    }
    while (f->x.blocks < want) {
        HExt r;
        uint32_t near = 0xFFFFFFFFUL;
        HExt *last = f->x.n ? &f->x.e[f->x.n - 1] : NULL;
        if (last) near = (uint32_t)last->start + last->count;
        if (alloc_run(v, near, want - f->x.blocks, &r)) {
            if (f->x.blocks >= need) break;
            return -1;
        }
        if (last && near == r.start && (uint32_t)last->count + r.count <= 65535) {
            last->count += r.count;
            f->x.blocks += r.count;
        } else if (ext_add(&f->x, r.start, r.count)) {
            blocks_mark(v, &r, 1, 0);
            return fail(v, "out of memory");
        }
        f->extchanged = f->dirty = 1;
    }
    return 0;
}

/* Give back allocation blocks beyond 'keep'. */
static void fork_shrink(HFork *f, uint32_t keep)
{
    while (f->x.blocks > keep && f->x.n) {
        HExt *l = &f->x.e[f->x.n - 1], fr;
        uint32_t cut = f->x.blocks - keep;
        if (cut > l->count) cut = l->count;
        fr.start = (uint16_t)(l->start + l->count - cut);
        fr.count = (uint16_t)cut;
        blocks_mark(f->v, &fr, 1, 0);
        l->count -= cut;
        f->x.blocks -= cut;
        if (!l->count) f->x.n--;
        f->extchanged = f->dirty = 1;
    }
}

static int fork_zero(HFork *f, uint32_t off, uint32_t len)
{
    static uint8_t z[2048];
    while (len) {
        uint32_t n = len < sizeof(z) ? len : sizeof(z);
        if (fork_write(f->v, &f->x, off, z, n)) return -1;
        off += n; len -= n;
    }
    return 0;
}

int hfs_fork_pread(HFork *f, uint32_t off, void *buf, uint32_t len)
{
    if (off > f->len || len > f->len - off) return fail(f->v, "read beyond end of fork");
    return hfs_fork_read(f->v, &f->x, off, buf, len);
}

int hfs_fork_pwrite(HFork *f, uint32_t off, const void *buf, uint32_t len)
{
    HVol *v = f->v;
    uint32_t end = off + len;
    if (end < off) return fail(v, "file too large");
    if (begin_write(v)) return -1;
    if (fork_grow(f, (end + v->alsize - 1) / v->alsize, 1)) return -1;
    if (off > f->len && fork_zero(f, f->len, off - f->len)) return -1;
    if (len && fork_write(v, &f->x, off, buf, len)) return -1;
    if (end > f->len) f->len = end;
    f->dirty = f->touched = 1;
    return 0;
}

int hfs_fork_setlen(HFork *f, uint32_t len)
{
    HVol *v = f->v;
    uint32_t need = (len + v->alsize - 1) / v->alsize;
    if (begin_write(v)) return -1;
    if (len > f->len) {
        if (fork_grow(f, need, 0)) return -1;
        if (fork_zero(f, f->len, len - f->len)) return -1;
    } else fork_shrink(f, need);
    f->len = len;
    f->dirty = f->touched = 1;
    return 0;
}

/* Store the fork's length and extents in its catalog record (and the extents-overflow
   B-tree).  'trim' first gives back blocks reserved beyond the logical end. */
int hfs_fork_sync(HFork *f, int trim)
{
    HVol *v = f->v;
    static uint8_t key[40], rec[HFS_NODESIZE];
    HExt first[3];
    uint8_t *d;
    int l, i, o = f->fork == FORK_DATA ? 0 : 10;
    if (trim) {
        uint32_t keep = (f->len + v->alsize - 1) / v->alsize;
        if (f->x.blocks > keep) {
            if (begin_write(v)) return -1;
            fork_shrink(f, keep);
        }
    }
    if (!f->dirty) return 0;
    if (begin_write(v)) return -1;
    if (f->extchanged && (f->hadover || f->x.n > 3)) {
        uint32_t start = 0;
        if (ext_purge(v, f->id, f->fork)) return -1;
        for (i = 0; i < 3 && i < f->x.n; i++) start += f->x.e[i].count;
        for (i = 3; i < f->x.n; i += 3) {
            static uint8_t r[20];
            HExt g[3];
            int j;
            memset(g, 0, sizeof(g));
            extkey(r, f->fork, f->id, (uint16_t)start);
            for (j = 0; j < 3 && i + j < f->x.n; j++) { g[j] = f->x.e[i + j]; start += g[j].count; }
            ext_pack(r + 8, g);
            if (bt_insert(&v->ext, r, 20)) return fail(v, "cannot add extents record: %s", v->err);
        }
        f->hadover = f->x.n >= 3;
    }
    f->extchanged = 0;
    catkey(key, f->parid, f->name);
    if (bt_get(&v->cat, key, rec, &l)) return fail(v, "file record for %lu not found", (unsigned long)f->id);
    d = rec + keytot(rec);
    if (d[0] != CDR_FILE || be32(d + 20) != f->id) return fail(v, "file record for %lu changed", (unsigned long)f->id);
    memset(first, 0, sizeof(first));
    for (i = 0; i < 3 && i < f->x.n; i++) first[i] = f->x.e[i];
    wb16(d + 24 + o, first[0].count ? first[0].start : 0);
    wb32(d + 26 + o, f->len);
    wb32(d + 30 + o, f->x.blocks * v->alsize);
    ext_pack(d + 74 + 12 * (f->fork != FORK_DATA), first);
    if (f->touched) wb32(d + 48, io_mactime());
    if (bt_update(&v->cat, key, rec, l)) return -1;
    f->dirty = f->touched = 0;
    return 0;
}

/* ------------------------------------------------------------------ write ops */

static int touch_parent(HVol *v, uint32_t parid, int delta)
{
    static uint8_t key[40], rec[HFS_NODESIZE], tk[40];
    int l, r;
    const uint8_t *d;
    /* thread -> name of the parent */
    catkey(tk, parid, NULL);
    r = bt_get(&v->cat, tk, rec, &l);
    if (r) return fail(v, "parent folder %lu has no thread record", (unsigned long)parid);
    d = rec + keytot(rec);
    if (d[0] != CDR_DTHREAD) return fail(v, "parent %lu is not a folder", (unsigned long)parid);
    {
        static uint8_t nm[32];
        memcpy(nm, d + 14, 32);
        if (nm[0] > 31) nm[0] = 31;
        catkey(key, be32(d + 10), nm);
    }
    r = bt_get(&v->cat, key, rec, &l);
    if (r) return fail(v, "parent folder record missing");
    d = rec + keytot(rec);
    wb16((uint8_t *)d + 4, (uint16_t)(be16(d + 4) + delta));
    wb32((uint8_t *)d + 14, io_mactime());
    return bt_update(&v->cat, key, rec, l);
}

static int check_name(HVol *v, const uint8_t *pname)
{
    int i;
    if (!pname[0] || pname[0] > 31) return fail(v, "name must be 1..31 characters");
    for (i = 1; i <= pname[0]; i++)
        if (pname[i] == ':') return fail(v, "names cannot contain ':'");
    return 0;
}

static int check_parent(HVol *v, uint32_t parid, const uint8_t *pname)
{
    HEnt pe, e;
    int r;
    if (check_name(v, pname)) return -1;
    if (hfs_get_id(v, parid, &pe) || pe.type != CDR_DIR) return fail(v, "target folder not found");
    r = hfs_lookup(v, parid, pname, &e);
    if (r < 0) return -1;
    if (r == 0) return fail(v, "%.*s already exists", pname[0], pname + 1);
    return 0;
}

static void mdb_rootcount(HVol *v, uint32_t parid, int isdir, int delta)
{
    if (parid == HFS_ROOT_ID) {
        int o = isdir ? 82 : 12;
        wb16(v->mdb + o, (uint16_t)(be16(v->mdb + o) + delta));
    }
}

static void mdb_count(HVol *v, uint32_t parid, int isdir, int delta)
{
    int o = isdir ? 88 : 84;
    mdb_rootcount(v, parid, isdir, delta);
    wb32(v->mdb + o, be32(v->mdb + o) + delta);
}

int hfs_create(HVol *v, uint32_t parid, const uint8_t *pname, const uint8_t finfo[16],
               const uint8_t fxinfo[16], uint32_t crdat, uint32_t mddat, uint32_t *newid)
{
    static uint8_t rec[HFS_NODESIZE], *d;
    uint32_t id;
    int kl;
    if (begin_write(v) || check_parent(v, parid, pname)) return -1;
    if (v->cat.freen < (uint32_t)v->cat.depth + 1)
        return fail(v, "catalog is full (%lu free nodes); make room on the Mac first", (unsigned long)v->cat.freen);
    id = be32(v->mdb + 30);
    kl = catkey(rec, parid, pname);
    kl = (kl + 1) & ~1;
    if (kl > 7 + pname[0]) rec[kl - 1] = 0;
    d = rec + kl;
    memset(d, 0, 102);
    d[0] = CDR_FILE;
    if (finfo) memcpy(d + 4, finfo, 16);
    wb32(d + 20, id);
    wb32(d + 44, crdat);
    wb32(d + 48, mddat);
    if (fxinfo) memcpy(d + 56, fxinfo, 16);
    if (bt_insert(&v->cat, rec, kl + 102)) return -1;
    wb32(v->mdb + 30, id + 1);
    if (touch_parent(v, parid, 1)) return -1;
    mdb_count(v, parid, 0, 1);
    if (newid) *newid = id;
    return 0;
}

static int fill_fork(HVol *v, HFork *f, const HForkSrc *src)
{
    uint8_t *buf;
    uint32_t off = 0;
    int r = 0;
    if (!src || !src->len) return 0;
    /* reserve the whole fork first, so it lands in as few extents as possible */
    if (begin_write(v) || fork_grow(f, (src->len + v->alsize - 1) / v->alsize, 0)) return -1;
    buf = malloc(16384);
    if (!buf) return fail(v, "out of memory");
    while (off < src->len && !r) {
        uint32_t n = src->len - off < 16384 ? src->len - off : 16384;
        if (src->read(src->ud, off, buf, n)) r = fail(v, "error reading source data");
        else r = hfs_fork_pwrite(f, off, buf, n);
        off += n;
    }
    free(buf);
    return r;
}

int hfs_create_file(HVol *v, uint32_t parid, const uint8_t *pname, const uint8_t finfo[16],
                    const uint8_t fxinfo[16], uint32_t crdat, uint32_t mddat,
                    const HForkSrc *data, const HForkSrc *rsrc, uint32_t *newid)
{
    uint32_t dl = data ? data->len : 0, rl = rsrc ? rsrc->len : 0, id;
    uint32_t db = (dl + v->alsize - 1) / v->alsize, rb = (rl + v->alsize - 1) / v->alsize;
    HEnt e;
    HFork f;
    int fk, r = 0;

    if (db + rb > hfs_free_blocks(v))
        return fail(v, "disk full: need %lu blocks, %lu free", (unsigned long)(db + rb), (unsigned long)hfs_free_blocks(v));
    if (hfs_create(v, parid, pname, finfo, fxinfo, crdat, mddat, &id)) return -1;
    for (fk = 0; fk < 2 && !r; fk++) {
        const HForkSrc *s = fk ? rsrc : data;
        if (!s || !s->len) continue;
        if (hfs_lookup(v, parid, pname, &e)) { r = fail(v, "new file vanished"); break; }
        if (hfs_fork_open(v, &e, fk ? FORK_RSRC : FORK_DATA, &f)) { r = -1; break; }
        f.touched = 0;
        r = fill_fork(v, &f, s);
        if (!r) { f.touched = 0; r = hfs_fork_sync(&f, 1); }
        else {      /* keep the extents in the record so the delete below frees them */
            static char keep[sizeof(v->err)];
            memcpy(keep, v->err, sizeof(keep));
            hfs_fork_sync(&f, 0);
            memcpy(v->err, keep, sizeof(keep));
        }
        hfs_fork_close(&f);
    }
    if (r) {
        static char keep[sizeof(v->err)];
        memcpy(keep, v->err, sizeof(keep));
        hfs_delete(v, parid, pname);
        memcpy(v->err, keep, sizeof(keep));
        return -1;
    }
    if (newid) *newid = id;
    return 0;
}

int hfs_mkdir(HVol *v, uint32_t parid, const uint8_t *pname, uint32_t *newid)
{
    static uint8_t rec[HFS_NODESIZE], *d;
    uint32_t id, now = io_mactime();
    int kl;
    if (begin_write(v) || check_parent(v, parid, pname)) return -1;
    if (v->cat.freen < 2u * ((uint32_t)v->cat.depth + 1) + 1)
        return fail(v, "catalog is full (%lu free nodes)", (unsigned long)v->cat.freen);
    id = be32(v->mdb + 30);
    wb32(v->mdb + 30, id + 1);
    /* folder record */
    kl = catkey(rec, parid, pname);
    if (kl & 1) rec[kl++] = 0;
    d = rec + kl;
    memset(d, 0, 70);
    d[0] = CDR_DIR;
    wb32(d + 6, id);
    wb32(d + 10, now);
    wb32(d + 14, now);
    if (bt_insert(&v->cat, rec, kl + 70)) return -1;
    /* thread record */
    kl = catkey(rec, id, NULL);
    rec[kl++] = 0;
    d = rec + kl;
    memset(d, 0, 46);
    d[0] = CDR_DTHREAD;
    wb32(d + 10, parid);
    memcpy(d + 14, pname, 1 + pname[0]);
    if (bt_insert(&v->cat, rec, kl + 46)) return -1;
    if (touch_parent(v, parid, 1)) return -1;
    mdb_count(v, parid, 1, 1);
    if (newid) *newid = id;
    return 0;
}

static int free_fork(HVol *v, const HEnt *e, int fork)
{
    HExtList x;
    uint32_t pyl = fork == FORK_DATA ? e->dpyl : e->rpyl;
    if (hfs_fork_extents(v, e->id, fork, fork == FORK_DATA ? e->dext : e->rext, pyl, &x)) return -1;
    blocks_mark(v, x.e, x.n, 0);
    hfs_extlist_free(&x);
    return ext_purge(v, e->id, fork);
}

int hfs_delete(HVol *v, uint32_t parid, const uint8_t *pname)
{
    HEnt e;
    static uint8_t key[40];
    int r;
    if (begin_write(v)) return -1;
    r = hfs_lookup(v, parid, pname, &e);
    if (r) return r < 0 ? -1 : fail(v, "not found: %.*s", pname[0], pname + 1);
    if (e.type == CDR_DIR) {
        if (e.valence) return fail(v, "folder %.*s is not empty", pname[0], pname + 1);
        catkey(key, e.id, NULL);
        if (bt_delete(&v->cat, key)) return -1;
    } else {
        if (free_fork(v, &e, FORK_DATA) || free_fork(v, &e, FORK_RSRC)) return -1;
        if (e.flags & 0x02) {   /* file thread record exists */
            static uint8_t rec[HFS_NODESIZE];
            catkey(key, e.id, NULL);
            if (!bt_get(&v->cat, key, rec, NULL) && bt_delete(&v->cat, key)) return -1;
        }
    }
    catkey(key, parid, pname);
    if (bt_delete(&v->cat, key)) return -1;
    if (touch_parent(v, parid, -1)) return -1;
    mdb_count(v, parid, e.type == CDR_DIR, -1);
    return 0;
}

/* Rename and/or move a file or folder.  Renaming the root folder renames the volume. */
int hfs_rename(HVol *v, uint32_t oldpar, const uint8_t *oldname, uint32_t newpar, const uint8_t *newname)
{
    static uint8_t key[40], nkey[40], rec[HFS_NODESIZE], nrec[HFS_NODESIZE], trec[HFS_NODESIZE];
    HEnt e, t;
    int l, kl, dl, r, same;
    const uint8_t *d;
    if (begin_write(v) || check_name(v, newname)) return -1;
    catkey(key, oldpar, oldname);
    r = bt_get(&v->cat, key, rec, &l);
    if (r) return r < 0 ? -1 : fail(v, "not found: %.*s", oldname[0], oldname + 1);
    if (parse_ent(rec, l, &e)) return fail(v, "not a file or folder");
    if (e.id == HFS_ROOT_ID) {
        if (newpar != oldpar) return fail(v, "cannot move the root folder");
        if (newname[0] > 27) return fail(v, "volume names are at most 27 characters");
    }
    same = oldpar == newpar && !hfs_relstring(oldname + 1, oldname[0], newname + 1, newname[0]);
    if (!same) {
        if (newpar != oldpar) {
            uint32_t p = newpar;
            int guard = 0;
            if (hfs_get_id(v, newpar, &t) || t.type != CDR_DIR) return fail(v, "target folder not found");
            /* a folder cannot go into itself or below itself */
            while (e.type == CDR_DIR && p >= HFS_ROOT_ID) {
                if (p == e.id) return fail(v, "cannot move a folder into itself");
                if (p == HFS_ROOT_ID || hfs_get_id(v, p, &t) || ++guard > 1000) break;
                p = t.parid;
            }
        }
        r = hfs_lookup(v, newpar, newname, &t);
        if (r < 0) return -1;
        if (r == 0) return fail(v, "%.*s already exists", newname[0], newname + 1);
    }
    if (v->cat.freen < (uint32_t)v->cat.depth + 2)
        return fail(v, "catalog is full (%lu free nodes)", (unsigned long)v->cat.freen);
    /* new record = new key + the old data */
    d = rec + keytot(rec);
    dl = l - keytot(rec);
    kl = catkey(nrec, newpar, newname);
    if (kl & 1) nrec[kl++] = 0;
    memcpy(nrec + kl, d, dl);
    if (bt_delete(&v->cat, key)) return -1;
    if (bt_insert(&v->cat, nrec, kl + dl)) {
        static char keep[sizeof(v->err)];
        memcpy(keep, v->err, sizeof(keep));
        bt_insert(&v->cat, rec, l);       /* put the old one back */
        memcpy(v->err, keep, sizeof(keep));
        return -1;
    }
    /* thread record: folders always have one, files only when flagged */
    if (e.type == CDR_DIR || (e.flags & 0x02)) {
        catkey(nkey, e.id, NULL);
        if (!bt_get(&v->cat, nkey, trec, &l)) {
            uint8_t *td = trec + keytot(trec);
            wb32(td + 10, newpar);
            memset(td + 14, 0, 32);
            memcpy(td + 14, newname, 1 + newname[0]);
            if (bt_update(&v->cat, nkey, trec, l)) return -1;
        } else if (e.type == CDR_DIR) return fail(v, "folder %lu has no thread record", (unsigned long)e.id);
    }
    if (e.id == HFS_ROOT_ID) {
        memset(v->mdb + 36, 0, 28);
        memcpy(v->mdb + 36, newname, 1 + newname[0]);
        memcpy(v->name, newname, 1 + newname[0]);
        v->name[newname[0] + 1] = 0;
        return 0;
    }
    if (oldpar != newpar) {
        if (touch_parent(v, oldpar, -1) || touch_parent(v, newpar, 1)) return -1;
        mdb_rootcount(v, oldpar, e.type == CDR_DIR, -1);
        mdb_rootcount(v, newpar, e.type == CDR_DIR, 1);
    } else if (touch_parent(v, oldpar, 0)) return -1;
    return 0;
}

/* Change Finder info and/or dates of an entry.  NULL / 0 leaves a field alone. */
int hfs_set_info(HVol *v, uint32_t parid, const uint8_t *pname, const uint8_t *finfo,
                 const uint8_t *fxinfo, uint32_t crdat, uint32_t mddat)
{
    static uint8_t key[40], rec[HFS_NODESIZE];
    uint8_t *d;
    int l, r, dir;
    if (begin_write(v)) return -1;
    catkey(key, parid, pname);
    r = bt_get(&v->cat, key, rec, &l);
    if (r) return r < 0 ? -1 : fail(v, "not found: %.*s", pname[0], pname + 1);
    d = rec + keytot(rec);
    if (d[0] != CDR_DIR && d[0] != CDR_FILE) return fail(v, "not a file or folder");
    dir = d[0] == CDR_DIR;
    if (finfo) memcpy(d + (dir ? 22 : 4), finfo, 16);
    if (fxinfo) memcpy(d + (dir ? 38 : 56), fxinfo, 16);
    if (crdat) wb32(d + (dir ? 10 : 44), crdat);
    if (mddat) wb32(d + (dir ? 14 : 48), mddat);
    return bt_update(&v->cat, key, rec, l);
}

/* Next entry of a folder after the one named 'after' (NULL: the first one).  Works
   even when 'after' was deleted meanwhile.  0 = found, 1 = no more, -1 = error. */
int hfs_dir_next(HVol *v, uint32_t dirid, const uint8_t *after, HEnt *e)
{
    uint32_t pn[HFS_MAXDEPTH];
    int pi[HFS_MAXDEPTH], ex, i, r, guard = 0;
    static uint8_t key[40], nd[HFS_NODESIZE];
    HTree *t = &v->cat;
    catkey(key, dirid, after);
    r = bt_search(t, key, pn, pi, &ex, nd);
    if (r) return r < 0 ? -1 : 1;
    i = pi[t->depth - 1];
    if (ex && after) i++;
    for (;;) {
        for (; i < nrecs(nd); i++) {
            const uint8_t *rec = nd + roff(nd, i);
            if (be32(rec + 2) != dirid) return 1;
            if (!parse_ent(rec, rlen(nd, i), e)) return 0;
        }
        if (!be32(nd) || ++guard > 1000000) return 1;
        if (node_read(t, be32(nd), nd)) return -1;
        i = 0;
    }
}

#ifndef HFS_NOCHECK
/* ------------------------------------------------------------------ check */

typedef struct { HVol *v; int verbose, bad; uint8_t *used; } Chk;

static void chk_bad(Chk *c, const char *fmt, ...)
{
    va_list ap;
    c->bad++;
    if (c->verbose || c->bad <= 20) {
        va_start(ap, fmt);
        printf("  problem: ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
}

static void chk_mark(Chk *c, const HExtList *x, const char *what, uint32_t id)
{
    int i;
    uint32_t b;
    for (i = 0; i < x->n; i++)
        for (b = x->e[i].start; b < (uint32_t)x->e[i].start + x->e[i].count; b++) {
            if (b >= c->v->nblocks) { chk_bad(c, "%s %lu: block %lu beyond volume", what, (unsigned long)id, (unsigned long)b); return; }
            if (c->used[b >> 3] & (0x80 >> (b & 7))) { chk_bad(c, "%s %lu: block %lu doubly allocated", what, (unsigned long)id, (unsigned long)b); return; }
            c->used[b >> 3] |= 0x80 >> (b & 7);
        }
}

/* Walk a tree: leaf order, links, key order, index keys, node map. */
static void chk_tree(Chk *c, HTree *t, const char *nm)
{
    static uint8_t nd[HFS_NODESIZE], prev[64];
    uint32_t n = t->first, bl = 0, cnt = 0, nodes = 0, i, guard = 0;
    int havep = 0, j;
    uint8_t *seen = calloc((t->total + 7) / 8 + 1, 1);
    if (!seen) return;
    seen[0] |= 0x80;
    while (n && guard++ <= t->total) {
        if (node_read(t, n, nd)) { chk_bad(c, "%s: %s", nm, c->v->err); break; }
        if (nd[8] != 0xFF || nd[9] != 1) chk_bad(c, "%s: node %lu in leaf chain is not a leaf", nm, (unsigned long)n);
        if (be32(nd + 4) != bl) chk_bad(c, "%s: node %lu bad back link", nm, (unsigned long)n);
        for (j = 0; j < nrecs(nd); j++) {
            const uint8_t *r = nd + roff(nd, j);
            if (havep && t->cmp(prev, r) >= 0) chk_bad(c, "%s: keys out of order in node %lu record %d", nm, (unsigned long)n, j);
            memcpy(prev, r, 1 + r[0] < 64 ? 1 + r[0] : 64);
            havep = 1;
            cnt++;
        }
        seen[n >> 3] |= 0x80 >> (n & 7);
        nodes++;
        bl = n;
        n = be32(nd);
    }
    if (bl != t->last) chk_bad(c, "%s: lastLeaf %lu but chain ends at %lu", nm, (unsigned long)t->last, (unsigned long)bl);
    if (cnt != t->leafrecs) chk_bad(c, "%s: header says %lu leaf records, found %lu", nm, (unsigned long)t->leafrecs, (unsigned long)cnt);
    /* index levels: breadth walk from root */
    if (t->root) {
        uint32_t *level = malloc(2 * 4096 * sizeof(uint32_t)), *next, nl = 1, lv;
        if (!level) { free(seen); return; }
        next = level + 4096;
        level[0] = t->root;
        for (lv = t->depth; lv > 1; lv--) {
            uint32_t nn = 0;
            for (i = 0; i < nl; i++) {
                if (node_read(t, level[i], nd)) { chk_bad(c, "%s: %s", nm, c->v->err); continue; }
                if (nd[8] != 0x00 || nd[9] != lv) chk_bad(c, "%s: index node %lu has kind %d height %d (want %lu)", nm, (unsigned long)level[i], nd[8], nd[9], (unsigned long)lv);
                seen[level[i] >> 3] |= 0x80 >> (level[i] & 7);
                nodes++;
                for (j = 0; j < nrecs(nd); j++) {
                    const uint8_t *r = nd + roff(nd, j);
                    uint32_t ch = be32(r + keytot(r));
                    static uint8_t cn[HFS_NODESIZE];
                    if (j && t->cmp(nd + roff(nd, j - 1), r) >= 0) chk_bad(c, "%s: index node %lu keys out of order", nm, (unsigned long)level[i]);
                    if (!node_read(t, ch, cn)) {
                        if (!nrecs(cn)) chk_bad(c, "%s: empty node %lu", nm, (unsigned long)ch);
                        else if (t->cmp(r, cn + roff(cn, 0)) != 0)
                            chk_bad(c, "%s: index key in node %lu does not match first key of child %lu", nm, (unsigned long)level[i], (unsigned long)ch);
                    }
                    if (nn < 4096) next[nn++] = ch;
                }
            }
            memcpy(level, next, nn * sizeof(uint32_t));
            nl = nn;
        }
        free(level);
    }
    /* map nodes */
    {
        uint32_t m = be32(t->hdr);
        while (m && m < t->total) {
            seen[m >> 3] |= 0x80 >> (m & 7);
            nodes++;
            if (node_read(t, m, nd)) break;
            m = be32(nd);
        }
    }
    {
        uint32_t used = 0;
        for (i = 0; i < t->total; i++) {
            int b = map_op(t, i, 0), s = (seen[i >> 3] >> (7 - (i & 7))) & 1;
            if (b) used++;
            if (s && b != 1) chk_bad(c, "%s: node %lu in use but free in map", nm, (unsigned long)i);
        }
        if (t->total - used != t->freen) chk_bad(c, "%s: header free nodes %lu, map says %lu", nm, (unsigned long)t->freen, (unsigned long)(t->total - used));
        if (used != nodes + 1) chk_bad(c, "%s: map has %lu nodes allocated, %lu reachable", nm, (unsigned long)used, (unsigned long)(nodes + 1));
    }
    free(seen);
}

typedef struct { Chk *c; uint32_t files, dirs, rootf, rootd, maxid; uint32_t *val; uint32_t *vid; int nv, cap; } ChkCat;

static int chk_ent(HVol *v, const HEnt *e, void *ud)
{
    ChkCat *k = ud;
    if (e->id > k->maxid) k->maxid = e->id;
    if (e->type == CDR_FILE) {
        HExtList x;
        k->files++;
        if (e->parid == HFS_ROOT_ID) k->rootf++;
        if (e->dlen > e->dpyl || e->rlen > e->rpyl) chk_bad(k->c, "file %lu: logical length beyond physical", (unsigned long)e->id);
        if (!hfs_fork_extents(v, e->id, FORK_DATA, e->dext, e->dpyl, &x)) {
            if (x.blocks * v->alsize < e->dpyl) chk_bad(k->c, "file %lu: data extents short", (unsigned long)e->id);
            chk_mark(k->c, &x, "file", e->id); hfs_extlist_free(&x);
        } else chk_bad(k->c, "file %lu: %s", (unsigned long)e->id, v->err);
        if (!hfs_fork_extents(v, e->id, FORK_RSRC, e->rext, e->rpyl, &x)) {
            if (x.blocks * v->alsize < e->rpyl) chk_bad(k->c, "file %lu: rsrc extents short", (unsigned long)e->id);
            chk_mark(k->c, &x, "file", e->id); hfs_extlist_free(&x);
        } else chk_bad(k->c, "file %lu: %s", (unsigned long)e->id, v->err);
    } else {
        HEnt t;
        if (e->id != HFS_ROOT_ID) { k->dirs++; if (e->parid == HFS_ROOT_ID) k->rootd++; }
        if (hfs_get_id(v, e->id, &t) || t.id != e->id) chk_bad(k->c, "folder %lu: missing or wrong thread record", (unsigned long)e->id);
    }
    /* count children per parent */
    {
        int i;
        for (i = 0; i < k->nv; i++) if (k->vid[i] == e->parid) { k->val[i]++; return 0; }
        if (k->nv == k->cap) {
            k->cap = k->cap ? k->cap * 2 : 256;
            k->vid = xrealloc(k->vid, k->cap * sizeof(uint32_t));
            k->val = xrealloc(k->val, k->cap * sizeof(uint32_t));
            if (!k->vid || !k->val) return -1;
        }
        k->vid[k->nv] = e->parid; k->val[k->nv] = 1; k->nv++;
    }
    return 0;
}

typedef struct { ChkCat *k; } ChkVal;
static int chk_val(HVol *v, const HEnt *e, void *ud)
{
    ChkCat *k = ud;
    int i;
    uint32_t n = 0;
    (void)v;
    if (e->type != CDR_DIR) return 0;
    for (i = 0; i < k->nv; i++) if (k->vid[i] == e->id) { n = k->val[i]; break; }
    if (n != e->valence) chk_bad(k->c, "folder %lu (%.*s): valence %u but %lu entries", (unsigned long)e->id, e->name[0], e->name + 1, e->valence, (unsigned long)n);
    return 0;
}

int hfs_check(HVol *v, int verbose)
{
    Chk c;
    ChkCat k;
    uint32_t b, freeb = 0;
    uint8_t *bm = v->bitmap;
    uint32_t bytes = ((uint32_t)v->nblocks + 4095) / 4096 * 512;
    memset(&c, 0, sizeof(c));
    memset(&k, 0, sizeof(k));
    c.v = v; c.verbose = verbose;
    c.used = calloc(bytes, 1);
    if (!bm) {
        bm = malloc(bytes);
        if (!bm || io_pread(v->f, v->base + (uint32_t)v->vbmst * 512, bm, bytes)) { printf("cannot read bitmap\n"); free(bm); free(c.used); return 1; }
    }
    chk_tree(&c, &v->ext, "extents B-tree");
    chk_tree(&c, &v->cat, "catalog B-tree");
    chk_mark(&c, &v->ext.ext, "extents file", 3);
    chk_mark(&c, &v->cat.ext, "catalog file", 4);
    k.c = &c;
    if (hfs_walk_all(v, chk_ent, &k)) chk_bad(&c, "catalog walk: %s", v->err);
    hfs_walk_all(v, chk_val, &k);
    if (k.files != be32(v->mdb + 84)) chk_bad(&c, "MDB file count %lu, catalog has %lu", (unsigned long)be32(v->mdb + 84), (unsigned long)k.files);
    if (k.dirs != be32(v->mdb + 88)) chk_bad(&c, "MDB folder count %lu, catalog has %lu", (unsigned long)be32(v->mdb + 88), (unsigned long)k.dirs);
    if (k.rootf != be16(v->mdb + 12)) chk_bad(&c, "MDB root file count %u, catalog has %lu", be16(v->mdb + 12), (unsigned long)k.rootf);
    if (k.rootd != be16(v->mdb + 82)) chk_bad(&c, "MDB root folder count %u, catalog has %lu", be16(v->mdb + 82), (unsigned long)k.rootd);
    if (k.maxid >= be32(v->mdb + 30)) chk_bad(&c, "next CNID %lu not above highest ID %lu", (unsigned long)be32(v->mdb + 30), (unsigned long)k.maxid);
    for (b = 0; b < v->nblocks; b++) {
        int u = (c.used[b >> 3] >> (7 - (b & 7))) & 1, m = (bm[b >> 3] >> (7 - (b & 7))) & 1;
        if (!m) freeb++;
        if (u && !m) chk_bad(&c, "block %lu in use but marked free", (unsigned long)b);
        if (!u && m && verbose) printf("  note: block %lu marked used but not owned by any file\n", (unsigned long)b);
    }
    if (freeb != be16(v->mdb + 34)) chk_bad(&c, "MDB free blocks %u, bitmap has %lu", be16(v->mdb + 34), (unsigned long)freeb);
    if (bm != v->bitmap) free(bm);
    free(c.used); free(k.vid); free(k.val);
    return c.bad;
}
#endif /* HFS_NOCHECK */
