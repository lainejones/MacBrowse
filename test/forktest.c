/* forktest.c - randomized test of hfs.c's open-fork API (streaming writes, size changes,
 * extents-overflow records, rename/move of files that are open) against an in-memory model.
 *
 *   forktest IMAGE [SEED [OPS [FILES [MAXLEN]]]]
 *
 * Use a COPY of an image: it creates the folder "ForkTest" (with a subfolder), runs the
 * operations on FILES files at once (interleaved, so their forks fragment), verifies every
 * read, runs CHECK, deletes everything again and checks that the free space and the
 * extents B-tree are back where they started.  Returns 0 on success.
 */
#include "../src/hfs.h"
#include "../src/charset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t hfs_prealloc;

static uint32_t seed = 1;
static uint32_t rnd(void) { seed = seed * 1103515245UL + 12345UL; return (seed >> 8) & 0xFFFFFF; }

typedef struct {
    HFork f;
    uint8_t *m;     /* model */
    uint32_t mlen;
    uint32_t dir;
    uint8_t name[32];
    int open, fork;
} TF;

static HVol v;
static TF *tf;
static int nf;
static uint32_t maxlen = 120000, maxop = 20000, dirs[2];
static unsigned long errors, maxext;
static uint8_t *buf;

static void pstr(uint8_t *pn, const char *s) { pn[0] = (uint8_t)strlen(s); memcpy(pn + 1, s, pn[0]); }

static int bad(const char *what, int i, unsigned long op)
{
    printf("FAIL op %lu file %d: %s (%s)\n", op, i, what, v.err);
    errors++;
    return -1;
}

static int reopen(TF *t, int i, unsigned long op)
{
    HEnt e;
    if (t->open) {
        if (hfs_fork_sync(&t->f, 1)) return bad("sync", i, op);
        hfs_fork_close(&t->f);
        t->open = 0;
    }
    if (hfs_lookup(&v, t->dir, t->name, &e)) return bad("lookup", i, op);
    if (hfs_fork_open(&v, &e, t->fork, &t->f)) return bad("open", i, op);
    t->open = 1;
    if (t->f.len != t->mlen) { printf("  len %lu want %lu\n", (unsigned long)t->f.len, (unsigned long)t->mlen); return bad("length after reopen", i, op); }
    return 0;
}

static int verify(TF *t, int i, unsigned long op, uint32_t off, uint32_t len)
{
    if (!len) return 0;
    if (hfs_fork_pread(&t->f, off, buf, len)) return bad("read", i, op);
    if (memcmp(buf, t->m + off, len)) {
        uint32_t k;
        for (k = 0; k < len && buf[k] == t->m[off + k]; k++) ;
        printf("  mismatch at %lu: got %02x want %02x\n", (unsigned long)(off + k), buf[k], t->m[off + k]);
        return bad("data mismatch", i, op);
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long ops = 3000, op, extrecs0;
    uint32_t free0;
    uint8_t pn[32];
    int i, rc;
    HEnt root;

    if (argc < 2) { printf("usage: forktest IMAGE [SEED [OPS [FILES [MAXLEN]]]]\n"); return 20; }
    if (argc > 2) seed = (uint32_t)strtoul(argv[2], NULL, 0);
    if (argc > 3) ops = strtoul(argv[3], NULL, 0);
    nf = argc > 4 ? atoi(argv[4]) : 4;
    if (argc > 5) maxlen = (uint32_t)strtoul(argv[5], NULL, 0);
    if (nf < 1 || nf > 16) nf = 4;
    hfs_prealloc = 0;       /* no reservation beyond the clump: fragment as much as possible */
    if (hfs_open(&v, argv[1], 2, -1)) { printf("open: %s\n", v.err); return 20; }
    free0 = hfs_free_blocks(&v);
    extrecs0 = v.ext.leafrecs;
    printf("forktest: seed %lu, %lu ops, %d files up to %lu bytes, %lu-byte blocks, %lu free\n",
           (unsigned long)seed, ops, nf, (unsigned long)maxlen, (unsigned long)v.alsize, (unsigned long)free0);
    tf = calloc(nf, sizeof(TF));
    buf = malloc(maxlen + maxop);
    if (!tf || !buf) { printf("out of memory\n"); return 20; }
    hfs_get_id(&v, HFS_ROOT_ID, &root);
    pstr(pn, "ForkTest");
    if (hfs_mkdir(&v, HFS_ROOT_ID, pn, &dirs[0])) { printf("mkdir: %s\n", v.err); return 20; }
    pstr(pn, "Sub");
    if (hfs_mkdir(&v, dirs[0], pn, &dirs[1])) { printf("mkdir: %s\n", v.err); return 20; }
    for (i = 0; i < nf; i++) {
        TF *t = &tf[i];
        char nm[32];
        sprintf(nm, "file %02d", i);
        pstr(t->name, nm);
        t->dir = dirs[0];
        t->fork = (i & 1) ? FORK_RSRC : FORK_DATA;
        t->m = calloc(maxlen + 1, 1);
        if (!t->m) { printf("out of memory\n"); return 20; }
        if (hfs_create(&v, t->dir, t->name, NULL, NULL, 0, 0, NULL)) { printf("create: %s\n", v.err); return 20; }
        if (reopen(t, i, 0)) return 20;
    }

    for (op = 1; op <= ops && errors == 0; op++) {
        int i = rnd() % nf;
        TF *t = &tf[i];
        uint32_t r = rnd() % 100;
        if (r < 45) {                                   /* write */
            uint32_t off = t->mlen ? rnd() % (t->mlen + 1) : 0, len = 1 + rnd() % maxop, k;
            if (r < 4) off = t->mlen + rnd() % 3000;    /* past the end: zero-filled gap */
            if (off >= maxlen) off = maxlen - 1;
            if (off + len > maxlen) len = maxlen - off;
            for (k = 0; k < len; k++) buf[k] = (uint8_t)(op + k * 7);
            if (hfs_fork_pwrite(&t->f, off, buf, len)) { bad("write", i, op); break; }
            if (off > t->mlen) memset(t->m + t->mlen, 0, off - t->mlen);
            memcpy(t->m + off, buf, len);
            if (off + len > t->mlen) t->mlen = off + len;
        } else if (r < 58) {                            /* set size */
            uint32_t nl = rnd() % maxlen;
            if (hfs_fork_setlen(&t->f, nl)) { bad("setlen", i, op); break; }
            if (nl > t->mlen) memset(t->m + t->mlen, 0, nl - t->mlen);
            t->mlen = nl;
        } else if (r < 85) {                            /* read */
            uint32_t off = t->mlen ? rnd() % t->mlen : 0, len = rnd() % maxop;
            if (off + len > t->mlen) len = t->mlen - off;
            if (verify(t, i, op, off, len)) break;
        } else if (r < 92) {                            /* sync */
            if (hfs_fork_sync(&t->f, rnd() & 1)) { bad("sync", i, op); break; }
        } else if (r < 97) {                            /* close + reopen */
            if (reopen(t, i, op)) break;
        } else {                                        /* rename / move while open */
            uint32_t nd = dirs[rnd() & 1];
            char nm[32];
            uint8_t nn[32];
            sprintf(nm, "file %02d.%lu", i, op);
            pstr(nn, nm);
            if (hfs_rename(&v, t->dir, t->name, nd, nn)) { bad("rename", i, op); break; }
            t->dir = nd;
            memcpy(t->name, nn, 32);
            t->f.parid = nd;
            memcpy(t->f.name, nn, 32);
        }
        if ((unsigned long)t->f.x.n > maxext) maxext = t->f.x.n;
        if (t->f.len != t->mlen) { bad("length", i, op); break; }
        if (op % 1000 == 0) {
            int k;
            for (k = 0; k < nf; k++) if (hfs_fork_sync(&tf[k].f, 0)) bad("sync", k, op);
            printf("  op %lu: max extents %lu, overflow records %lu, free %lu\n", op, maxext,
                   (unsigned long)v.ext.leafrecs, (unsigned long)hfs_free_blocks(&v));
            if (hfs_check(&v, 0)) { printf("FAIL: CHECK at op %lu\n", op); errors++; }
        }
    }
    /* final: everything synced, reopened and compared in full */
    for (i = 0; i < nf && !errors; i++)
        if (!reopen(&tf[i], i, op)) verify(&tf[i], i, op, 0, tf[i].mlen);
    printf("  end: max extents %lu, overflow records %lu\n", maxext, (unsigned long)v.ext.leafrecs);
    if (!errors && hfs_check(&v, 0)) { printf("FAIL: CHECK at the end\n"); errors++; }
    for (i = 0; i < nf; i++) {
        if (tf[i].open) hfs_fork_close(&tf[i].f);
        if (hfs_delete(&v, tf[i].dir, tf[i].name)) { printf("delete: %s\n", v.err); errors++; }
    }
    pstr(pn, "Sub");
    if (hfs_delete(&v, dirs[0], pn)) { printf("delete Sub: %s\n", v.err); errors++; }
    pstr(pn, "ForkTest");
    if (hfs_delete(&v, HFS_ROOT_ID, pn)) { printf("delete ForkTest: %s\n", v.err); errors++; }
    if (hfs_check(&v, 0)) { printf("FAIL: CHECK after cleanup\n"); errors++; }
    if (hfs_free_blocks(&v) != free0) { printf("FAIL: free blocks %lu, was %lu\n", (unsigned long)hfs_free_blocks(&v), (unsigned long)free0); errors++; }
    if (v.ext.leafrecs != extrecs0) { printf("FAIL: %lu overflow records left, was %lu\n", (unsigned long)v.ext.leafrecs, extrecs0); errors++; }
    rc = hfs_close(&v);
    if (rc) { printf("close: %s\n", v.err); errors++; }
    printf("RESULT: %s\n", errors ? "FAILED" : "OK");
    return errors ? 10 : 0;
}
