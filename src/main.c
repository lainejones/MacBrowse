/* MacBrowse - browse and copy files on Mac HFS disk images from AmigaOS.
 *
 * MacBrowse <image> <command> [args] [options]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "hfs.h"
#include "charset.h"
#include "macfmt.h"
#include "version.h"
#include "ops.h"
#include "sync.h"
#include <stdarg.h>

#if defined(AMIGA) || defined(__amigaos__)
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
const char vers[] = "$VER: MacBrowse " MB_VERSION " (" MB_DATE ")";
#define AMIGA_HOST 1
#include "mount.h"
#define BREAK_CHECK() (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
#else
#define BREAK_CHECK() 0
#endif

extern int gui_main(const char *image);

/* ------------------------------------------------------------------ options */

Opts O;
int broken;
int gui_mode;
char gui_errbuf[512];
int gui_errcount;

void errf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (gui_mode) {
        /* keep the first few messages for the GUI's requester */
        size_t l = strlen(gui_errbuf);
        if (gui_errcount < 6 && l < sizeof(gui_errbuf) - 2) {
            vsnprintf(gui_errbuf + l, sizeof(gui_errbuf) - l, fmt, ap);
            if (!strncmp(gui_errbuf + l, "MacBrowse: ", 11))
                memmove(gui_errbuf + l, gui_errbuf + l + 11, strlen(gui_errbuf + l + 11) + 1);
        }
        gui_errcount++;
    } else vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void outn(const uint8_t *p, int n) { fwrite(p, 1, n, stdout); }

/* Print a Pascal Mac Roman name, converted for the console unless RAW. */
static void pname_out(const uint8_t *pn)
{
    char b[64];
    int i;
    if (O.raw) { outn(pn + 1, pn[0]); return; }
    mac2lat_str(b, pn + 1, pn[0]);
    for (i = 0; b[i]; i++) if ((unsigned char)b[i] < 0x20 || b[i] == 0x7F) b[i] = '?';
    fputs(b, stdout);
}

static int pname_len(const uint8_t *pn) { return pn[0]; }

static void fourcc_out(const uint8_t *p)
{
    int i;
    for (i = 0; i < 4; i++) {
        uint8_t c = p[i];
        if (!O.raw) { c = mac2lat(c); if (c < 0x20) c = '.'; }
        putchar(c);
    }
}

static void date_str(char *b, uint32_t t)
{
    /* Mac time: seconds since 1904-01-01 local */
    uint32_t days = t / 86400, s = t % 86400;
    int y = 1904, m = 0;
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!t) { strcpy(b, "----------  --:--"); return; }
    for (;;) {
        int yl = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < (uint32_t)yl) break;
        days -= yl; y++;
    }
    for (m = 0; m < 12; m++) {
        int l = md[m] + (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
        if (days < (uint32_t)l) break;
        days -= l;
    }
    sprintf(b, "%04d-%02d-%02d %02lu:%02lu", y, m + 1, (int)days + 1, (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

static int kw(const char *a, const char *k) { return !strcasecmp(a, k); }

static void usage(void)
{
    puts("MacBrowse " MB_VERSION " - Mac HFS disk image browser\n"
         "Usage: MacBrowse <image> <command> [args] [options]\n"
         "Commands:\n"
         "  INFO                     volume information\n"
         "  PARTS                    list the Apple partition map\n"
         "  LIST [path] [ALL]        list a folder (ALL = recursive)\n"
         "  LS [DEPTH n]             flat listing of all files (hfsls.py format)\n"
         "  GET <path..> [TO dir|file] [MODE DATA|RSRC|MACBIN|DOUBLE|SINGLE]\n"
         "                           copy files/folders out (default: data fork)\n"
         "  PUT <file|dir..> [TO folder] [AS name] [TYPE t] [CREATOR c] [PLAIN] [REPLACE]\n"
         "                           copy in; MacBinary, AppleSingle, BinHex and ._name\n"
         "                           AppleDouble sidecars are decoded unless PLAIN\n"
         "  MKDIR <path>             create a folder\n"
         "  DELETE <path..>          delete files / empty folders\n"
         "  RENAME <path> <newpath>  rename or move (into newpath if it is a folder)\n"
         "  CHECK                    verify B-trees, bitmap and counts\n"
         "  CREATE <MB> [NAME name]  write a new, empty image (FORCE: over an old one)\n"
         "  SYNCIN <drawer> [FREE mb] [NAME name]\n"
         "                           make the image match an AmigaDOS drawer (creates it)\n"
         "  SYNCOUT <drawer>         bring what changed on the Mac back to the drawer\n"
         "  MOUNT <device:> [RO] [NODOUBLE]   mount as a volume with L:MacHFS-Handler\n"
         "  UNMOUNT <device:>        unmount it again (the image argument is ignored)\n"
         "Options: PART n  RAW (Mac Roman names)  FORCE  QUIET  VERBOSE   -- ends options\n"
         "Mac paths use ':' (\"Folder:Sub:File\"); the volume name may be given first.\n"
         "Wildcards * ? #? are allowed in the last path component.");
}

static int setmode(const char *m)
{
    if (kw(m, "DATA")) O.mode = M_DATA;
    else if (kw(m, "RSRC") || kw(m, "RESOURCE")) O.mode = M_RSRC;
    else if (kw(m, "MACBIN") || kw(m, "MACBINARY")) O.mode = M_MACBIN;
    else if (kw(m, "DOUBLE") || kw(m, "APPLEDOUBLE")) O.mode = M_DOUBLE;
    else if (kw(m, "SINGLE") || kw(m, "APPLESINGLE")) O.mode = M_SINGLE;
    else { errf("MODE must be DATA, RSRC, MACBIN, DOUBLE or SINGLE\n"); return -1; }
    return 0;
}

static int iskey(const char *k)
{
    return kw(k, "TO") || kw(k, "AS") || kw(k, "TYPE") || kw(k, "CREATOR") || kw(k, "PART") ||
           kw(k, "NAME") || kw(k, "FREE") ||
           kw(k, "DEPTH") || kw(k, "MODE");
}

static int parse_args(int argc, char **argv)
{
    int i, nomore = 0;
    memset(&O, 0, sizeof(O));
    O.part = -1;
    O.depth = 99;
    O.mode = M_DATA;
    for (i = 1; i < argc; i++) {
        char *a = argv[i], *eq = strchr(a, '='), key[16];
        const char *val = NULL;
        if (!nomore && !strcmp(a, "--")) { nomore = 1; continue; }
        if (nomore) goto positional;
        if (eq && eq - a < (int)sizeof(key) && eq > a) {
            memcpy(key, a, eq - a); key[eq - a] = 0;
            if (iskey(key)) { a = key; val = eq + 1; }
        }
        if (iskey(a)) {
            if (!val) {
                if (i + 1 >= argc) { errf("%s needs a value\n", a); return -1; }
                val = argv[++i];
            }
            if (kw(a, "TO")) O.to = val;
            else if (kw(a, "AS")) O.as = val;
            else if (kw(a, "TYPE")) O.type = val;
            else if (kw(a, "CREATOR")) O.creator = val;
            else if (kw(a, "PART")) O.part = atoi(val);
            else if (kw(a, "MODE")) { if (setmode(val)) return -1; }
            else if (kw(a, "NAME")) O.name = val;
            else if (kw(a, "FREE")) O.freemb = atoi(val);
            else O.depth = atoi(val);
            continue;
        }
        if (kw(a, "ALL")) { O.all = 1; continue; }
        if (kw(a, "RAW")) { O.raw = 1; continue; }
        if (kw(a, "FORCE")) { O.force = 1; continue; }
        if (kw(a, "REPLACE")) { O.replace = 1; continue; }
        if (kw(a, "QUIET")) { O.quiet = 1; continue; }
        if (kw(a, "VERBOSE")) { O.verbose = 1; continue; }
        if (kw(a, "PLAIN")) { O.plain = 1; continue; }
        if (kw(a, "RO") || kw(a, "READONLY")) { O.ro = 1; continue; }
        if (kw(a, "NODOUBLE")) { O.nodouble = 1; continue; }
        if (kw(a, "MACBINARY") || kw(a, "APPLEDOUBLE") || kw(a, "APPLESINGLE")) { setmode(a); continue; }
    positional:
        if (!O.image) O.image = a;
        else if (!O.cmd) O.cmd = a;
        else if (O.npaths < 64) O.paths[O.npaths++] = a;
    }
    return O.image && O.cmd ? 0 : -1;
}

/* ------------------------------------------------------------------ paths & matching */

static int haswild(const char *s) { return strchr(s, '*') || strchr(s, '?'); }

/* AmigaDOS-ish pattern: * and #? = any sequence, ? = one char.  Case-insensitive Latin-1. */
static int wmatch(const char *p, const char *s)
{
    for (;;) {
        if (p[0] == '#' && p[1] == '?') { p++; goto star; }
        switch (*p) {
        case 0: return *s == 0;
        case '*':
        star:
            p++;
            for (;;) { if (wmatch(p, s)) return 1; if (!*s) return 0; s++; }
        case '?':
            if (!*s) return 0;
            break;
        default:
            if (tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
        }
        p++; s++;
    }
}

uint8_t mb_last_pn[32];
static void to_pname(uint8_t *pn, const char *lat, int n);
void mb_to_pname(uint8_t *pn, const char *lat)
{
    int i;
    memset(pn, 0, 32);
    to_pname(pn, lat, (int)strlen(lat));
    for (i = 1; i <= pn[0]; i++) if (pn[i] == ':') pn[i] = '-';
}

static void to_pname(uint8_t *pn, const char *lat, int n)
{
    int i;
    if (n > 31) n = 31;
    pn[0] = (uint8_t)n;
    for (i = 0; i < n; i++) pn[1 + i] = O.raw ? (uint8_t)lat[i] : lat2mac((uint8_t)lat[i]);
}

/* Look a single (Latin-1) name up in a folder: exact HFS lookup, then a scan that compares
   the Latin-1 rendering (so "Settlers II f" finds "Settlers II <florin>"). */
typedef struct { const char *pat; HEnt *out; int n, cap, first; } Scan;

static int scan_add(Scan *s, const HEnt *e)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        HEnt *no = xrealloc(s->out, nc * sizeof(HEnt));
        if (!no) return -1;
        s->out = no; s->cap = nc;
    }
    s->out[s->n++] = *e;
    return 0;
}

static int scan_cb(HVol *v, const HEnt *e, void *ud)
{
    Scan *s = ud;
    char nm[64];
    (void)v;
    if (O.raw) { memcpy(nm, e->name + 1, e->name[0]); nm[e->name[0]] = 0; }
    else mac2lat_str(nm, e->name + 1, e->name[0]);
    if (wmatch(s->pat, nm)) {
        if (scan_add(s, e)) return -1;
        if (s->first) return 1;
    }
    return 0;
}

/* Matches for one (Latin-1) name in a folder go into s (exact HFS lookup first, then a
   scan that compares the Latin-1 rendering, so "Settlers II f" finds "Settlers II <florin>"). */
static void find_in(HVol *v, uint32_t dir, const char *name, int n, Scan *s, int first)
{
    uint8_t pn[32];
    char pat[64];
    static HEnt e;
    if (n > 63) n = 63;
    memcpy(pat, name, n); pat[n] = 0;
    if (!haswild(pat)) {
        to_pname(pn, name, n);
        if (hfs_lookup(v, dir, pn, &e) == 0) { scan_add(s, &e); return; }
    }
    s->pat = pat;
    s->first = first;
    hfs_readdir(v, dir, scan_cb, s);
}

/* Resolve a Latin-1 Mac path.  Wildcards are allowed in every component (the first match
   is followed for folders; the last component returns all matches).  *list is malloc'd
   (free it); returns the number of matches, 0 if not found. */
static int resolve(HVol *v, const char *path, HEnt **list)
{
    HEnt cur;
    const char *p = path;
    int first = 1;
    Scan s;
    memset(&s, 0, sizeof(s));
    *list = NULL;
    if (hfs_get_id(v, HFS_ROOT_ID, &cur)) return 0;
    while (*p == ':' || *p == '/') p++;
    for (;;) {
        const char *c = p;
        int n, last;
        while (*p && *p != ':') p++;
        n = (int)(p - c);
        while (*p == ':') p++;
        last = !*p;
        if (!n) {
            if (last) { scan_add(&s, &cur); *list = s.out; return s.n; }
            continue;
        }
        if (cur.type != CDR_DIR) break;
        s.n = 0;
        find_in(v, cur.id, c, n, &s, !last);
        if (s.n) {
            if (last) { *list = s.out; return s.n; }
            cur = s.out[0];
            first = 0;
            continue;
        }
        /* the first component may be the volume name */
        if (first) {
            char vn[32];
            mac2lat_str(vn, v->name + 1, v->name[0]);
            first = 0;
            if ((int)strlen(vn) == n && !strncasecmp(vn, c, n)) {
                if (last) { scan_add(&s, &cur); *list = s.out; return s.n; }
                continue;
            }
        }
        break;
    }
    free(s.out);
    return 0;
}

/* Resolve to exactly one entry. */
static int resolve1(HVol *v, const char *path, HEnt *e)
{
    HEnt *l;
    int n = resolve(v, path, &l);
    if (n >= 1) *e = l[0];
    free(l);
    return n == 1 ? 0 : -1;
}

/* Split "a:b:c" into folder "a:b" (resolved) and leaf name "c". */
static int resolve_parent(HVol *v, const char *path, HEnt *dir, const char **leaf)
{
    char buf[512];
    const char *c = strrchr(path, ':');
    if (!c) { *leaf = path; return hfs_get_id(v, HFS_ROOT_ID, dir) ? -1 : 0; }
    *leaf = c + 1;
    if ((size_t)(c - path) >= sizeof(buf)) return -1;
    memcpy(buf, path, c - path); buf[c - path] = 0;
    if (resolve1(v, buf, dir) || dir->type != CDR_DIR) return -1;
    return 0;
}

/* ------------------------------------------------------------------ INFO / PARTS / CHECK */

static void print_pstr(const char *label, const uint8_t *pn) { printf("%s", label); pname_out(pn); putchar('\n'); }

static int cmd_info(HVol *v)
{
    char d[32];
    const uint8_t *m = v->mdb;
    uint32_t freeb = be16(m + 34);
    print_pstr("Volume name:      ", v->name);
    printf("Volume offset:    %lu\n", (unsigned long)v->base);
    date_str(d, be32(m + 2));  printf("Created:          %s\n", d);
    date_str(d, be32(m + 6));  printf("Modified:         %s\n", d);
    date_str(d, be32(m + 64)); printf("Backed up:        %s\n", d);
    printf("Attributes:       $%04x%s%s\n", be16(m + 10),
           (be16(m + 10) & 0x0100) ? " (cleanly unmounted)" : " (MOUNTED or not cleanly unmounted)",
           (be16(m + 10) & 0x8080) ? " (locked)" : "");
    printf("Allocation block: %lu bytes, %u blocks, %lu free (%lu KB free of %lu KB)\n",
           (unsigned long)v->alsize, v->nblocks, (unsigned long)freeb,
           (unsigned long)(freeb * (v->alsize / 512) / 2), (unsigned long)((uint32_t)v->nblocks * (v->alsize / 512) / 2));
    printf("Files / folders:  %lu / %lu (root: %u / %u)\n", (unsigned long)be32(m + 84),
           (unsigned long)be32(m + 88), be16(m + 12), be16(m + 82));
    printf("Next CNID:        %lu\n", (unsigned long)be32(m + 30));
    printf("Write count:      %lu\n", (unsigned long)be32(m + 70));
    printf("Catalog B-tree:   %lu bytes, %lu nodes, %lu free, depth %u, %lu records%s\n",
           (unsigned long)v->cat.size, (unsigned long)v->cat.total, (unsigned long)v->cat.freen,
           v->cat.depth, (unsigned long)v->cat.leafrecs, v->cat.ext.n > 3 ? " (fragmented)" : "");
    printf("Extents B-tree:   %lu bytes, %lu nodes, %lu free, depth %u, %lu records\n",
           (unsigned long)v->ext.size, (unsigned long)v->ext.total, (unsigned long)v->ext.freen,
           v->ext.depth, (unsigned long)v->ext.leafrecs);
    if (be16(m + 124) == 0x482B) printf("Note: this is an HFS wrapper around an HFS+ volume (not supported).\n");
    return 0;
}

static int cmd_parts(void)
{
    static HPart p[64];
    int n = hfs_list_parts(O.image, p, 64), i;
    if (n < 0) { errf("cannot open %s\n", O.image); return 20; }
    if (!n) { printf("No Apple partition map (bare volume image).\n"); return 0; }
    printf(" #  %-10s %-10s %-24s %s\n", "start", "sectors", "type", "name");
    for (i = 0; i < n; i++)
        printf("%2d  %-10lu %-10lu %-24s %s\n", i, (unsigned long)p[i].start, (unsigned long)p[i].size, p[i].type, p[i].name);
    return 0;
}

/* ------------------------------------------------------------------ LIST */

typedef struct { HVol *v; char path[512]; int depth; int nf, nd; } LCtx;

static int list_cb(HVol *v, const HEnt *e, void *ud);

static void list_line(const char *prefix, const HEnt *e)
{
    char d[32];
    int n = (int)strlen(prefix) + pname_len(e->name) + (e->type == CDR_DIR);
    fputs(prefix, stdout);
    pname_out(e->name);
    if (e->type == CDR_DIR) putchar(':');
    while (n++ < 32) putchar(' ');
    putchar(' ');
    date_str(d, e->mddat);
    if (e->type == CDR_DIR) {
        printf("(folder)   %5u item%s          %s\n", e->valence, e->valence == 1 ? " " : "s", d);
    } else {
        fourcc_out(e->finfo); putchar(' '); fourcc_out(e->finfo + 4);
        printf(" %9lu %9lu  %s\n", (unsigned long)e->dlen, (unsigned long)e->rlen, d);
    }
}

static int list_cb(HVol *v, const HEnt *e, void *ud)
{
    LCtx *c = ud;
    if (broken || BREAK_CHECK()) { if (!broken) broken = 1; return 1; }
    list_line(O.all ? c->path : "", e);
    if (e->type == CDR_DIR) c->nd++; else c->nf++;
    if (O.all && e->type == CDR_DIR) {
        size_t l = strlen(c->path);
        char nm[64];
        int r;
        if (O.raw) { memcpy(nm, e->name + 1, e->name[0]); nm[e->name[0]] = 0; }
        else mac2lat_str(nm, e->name + 1, e->name[0]);
        if (l + strlen(nm) + 2 >= sizeof(c->path)) return 0;
        strcat(c->path, nm);
        strcat(c->path, ":");
        r = hfs_readdir(v, e->id, list_cb, c);
        c->path[l] = 0;
        return r;
    }
    return 0;
}

static int cmd_list(HVol *v)
{
    HEnt *e;
    int n, i, j;
    LCtx c;
    const char *path = O.npaths ? O.paths[0] : ":";
    n = resolve(v, path, &e);
    if (!n) { errf("MacBrowse: %s: not found\n", path); free(e); return 10; }
    memset(&c, 0, sizeof(c));
    c.v = v;
    for (j = 0; j < n; j++) {
        if (e[j].type != CDR_DIR) { list_line("", &e[j]); c.nf++; continue; }
        if (!O.quiet) {
            printf("Folder \"");
            if (e[j].id == HFS_ROOT_ID) pname_out(v->name);
            else pname_out(e[j].name);
            printf("\"  (%u items)\n", e[j].valence);
            printf("%-32s Type Crtr      Data      Rsrc  Modified\n", "Name");
        }
        c.path[0] = 0;
        i = hfs_readdir(v, e[j].id, list_cb, &c);
        if (i < 0) { errf("MacBrowse: %s\n", v->err); free(e); return 20; }
    }
    free(e);
    if (!O.quiet) printf("%d file%s, %d folder%s\n", c.nf, c.nf == 1 ? "" : "s", c.nd, c.nd == 1 ? "" : "s");
    return broken ? 5 : 0;
}

/* ------------------------------------------------------------------ LS (hfsls.py format) */

typedef struct { uint32_t id, par; uint8_t name[32]; } DirRec;
typedef struct { DirRec *d; int n, cap; } DirTab;

static int ls_dirs(HVol *v, const HEnt *e, void *ud)
{
    DirTab *t = ud;
    (void)v;
    if (e->type != CDR_DIR) return 0;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 512;
        t->d = xrealloc(t->d, t->cap * sizeof(DirRec));
        if (!t->d) return -1;
    }
    t->d[t->n].id = e->id;
    t->d[t->n].par = e->parid;
    memcpy(t->d[t->n].name, e->name, 32);
    t->n++;
    return 0;
}

static int dcmp(const void *a, const void *b)
{
    uint32_t x = ((const DirRec *)a)->id, y = ((const DirRec *)b)->id;
    return x < y ? -1 : x > y;
}

static DirTab LT;

static const DirRec *dfind(uint32_t id)
{
    DirRec k;
    k.id = id;
    return bsearch(&k, LT.d, LT.n, sizeof(DirRec), dcmp);
}

/* Build "Vol:A:B" for folder id; returns ':' count. */
static int dpath(uint32_t id, uint8_t *out, int *len)
{
    const DirRec *chain[64];
    int n = 0, i, l = 0, colons = 0;
    const DirRec *d;
    while (id != HFS_ROOT_PAR && (d = dfind(id)) && n < 64) { chain[n++] = d; id = d->par; }
    for (i = n - 1; i >= 0; i--) {
        if (l + chain[i]->name[0] + 1 > 500) break;
        memcpy(out + l, chain[i]->name + 1, chain[i]->name[0]);
        l += chain[i]->name[0];
        if (i) { out[l++] = ':'; colons++; }
    }
    *len = l;
    return colons;
}

static int ls_files(HVol *v, const HEnt *e, void *ud)
{
    static uint8_t line[600];
    int l, i;
    (void)v; (void)ud;
    if (e->type != CDR_FILE) return 0;
    if (dpath(e->parid, line, &l) >= O.depth) return 0;
    line[l++] = ':';
    memcpy(line + l, e->name + 1, e->name[0]);
    l += e->name[0];
    if (l > 60) l = 60;
    if (!O.raw) for (i = 0; i < l; i++) line[i] = mac2lat(line[i]);
    outn(line, l);
    for (i = l; i < 60; i++) putchar(' ');
    putchar(' ');
    fourcc_out(e->finfo); putchar('/'); fourcc_out(e->finfo + 4);
    printf(" data=%lu rsrc=%lu\n", (unsigned long)e->dlen, (unsigned long)e->rlen);
    return 0;
}

static int cmd_ls(HVol *v)
{
    memset(&LT, 0, sizeof(LT));
    if (hfs_walk_all(v, ls_dirs, &LT)) { errf("MacBrowse: %s\n", v->err); return 20; }
    qsort(LT.d, LT.n, sizeof(DirRec), dcmp);
    if (hfs_walk_all(v, ls_files, NULL)) { errf("MacBrowse: %s\n", v->err); return 20; }
    free(LT.d);
    return 0;
}

/* ------------------------------------------------------------------ GET */

static uint8_t *copybuf;
#define COPYBUF 32768

static int copy_fork(HVol *v, const HEnt *e, int fork, IOF *out, uint32_t *written)
{
    HExtList x;
    uint32_t len = fork == FORK_DATA ? e->dlen : e->rlen, off = 0;
    if (hfs_fork_extents(v, e->id, fork, fork == FORK_DATA ? e->dext : e->rext, len, &x)) return -1;
    while (off < len) {
        uint32_t n = len - off < COPYBUF ? len - off : COPYBUF;
        if (hfs_fork_read(v, &x, off, copybuf, n)) { hfs_extlist_free(&x); return -1; }
        if (io_write(out, copybuf, n)) { hfs_extlist_free(&x); snprintf(v->err, sizeof(v->err), "write error: %s", io_errstr()); return -1; }
        off += n;
    }
    hfs_extlist_free(&x);
    if (written) *written += len;
    return 0;
}

static int pad_out(IOF *out, uint32_t len)
{
    static const uint8_t z[128];
    uint32_t p = (128 - (len & 127)) & 127;
    return p ? io_write(out, z, p) : 0;
}

static void join(char *dst, size_t sz, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (!l) { snprintf(dst, sz, "%s", name); return; }
    if (dir[l - 1] == ':' || dir[l - 1] == '/') snprintf(dst, sz, "%s%s", dir, name);
    else snprintf(dst, sz, "%s/%s", dir, name);
}

static void set_meta(const char *path, const HEnt *e)
{
    char c[16];
    int i;
    io_setfiletime(path, e->mddat);
    if (e->type == CDR_FILE) {
        for (i = 0; i < 4; i++) { c[i] = (char)mac2lat(e->finfo[i]); c[5 + i] = (char)mac2lat(e->finfo[4 + i]); }
        c[4] = '/'; c[9] = 0;
        for (i = 0; i < 9; i++) if ((unsigned char)c[i] < 0x20) c[i] = '.';
        io_setcomment(path, c);
    }
}

static int get_file(HVol *v, const HEnt *e, const char *dest)
{
    IOF *out;
    static char path[600], side[600];
    uint32_t w = 0;
    int r = 0;
    strcpy(path, dest);
    if (O.mode == M_MACBIN) strcat(path, ".bin");
    out = io_open(path, IO_CREATE);
    if (!out) { errf("MacBrowse: cannot create %s: %s\n", path, io_errstr()); return -1; }
    switch (O.mode) {
    case M_DATA: r = copy_fork(v, e, FORK_DATA, out, &w); break;
    case M_RSRC: r = copy_fork(v, e, FORK_RSRC, out, &w); break;
    case M_MACBIN: {
        uint8_t h[128];
        mb_header(h, e);
        r = io_write(out, h, 128);
        if (!r) r = copy_fork(v, e, FORK_DATA, out, &w);
        if (!r) r = pad_out(out, e->dlen);
        if (!r) r = copy_fork(v, e, FORK_RSRC, out, &w);
        if (!r) r = pad_out(out, e->rlen);
        break; }
    case M_SINGLE: {
        uint8_t h[256];
        uint32_t ro, dof;
        int hl = ad_header(h, sizeof(h), e, 1, &ro, &dof);
        r = io_write(out, h, hl);
        if (!r) r = copy_fork(v, e, FORK_RSRC, out, &w);
        if (!r) r = copy_fork(v, e, FORK_DATA, out, &w);
        break; }
    case M_DOUBLE:
        r = copy_fork(v, e, FORK_DATA, out, &w);
        break;
    }
    io_close(out);
    if (r) { errf("MacBrowse: %s: %s\n", path, v->err); return -1; }
    set_meta(path, e);
    if (O.mode == M_DOUBLE) {
        /* sidecar ._name next to the data file */
        const char *base = strrchr(dest, '/');
        const char *colon = strrchr(dest, ':');
        size_t dl;
        uint8_t h[256];
        uint32_t ro;
        int hl;
        if (colon > base) base = colon;
        base = base ? base + 1 : dest;
        dl = (size_t)(base - dest);
        memcpy(side, dest, dl);
        snprintf(side + dl, sizeof(side) - dl, "._%s", base);
        out = io_open(side, IO_CREATE);
        if (!out) { errf("MacBrowse: cannot create %s: %s\n", side, io_errstr()); return -1; }
        hl = ad_header(h, sizeof(h), e, 0, &ro, NULL);
        r = io_write(out, h, hl);
        if (!r) r = copy_fork(v, e, FORK_RSRC, out, &w);
        io_close(out);
        if (r) { errf("MacBrowse: %s: %s\n", side, v->err); return -1; }
        io_setfiletime(side, e->mddat);
    }
    if (!O.quiet) printf("  %s (%lu bytes)\n", path, (unsigned long)w);
    return 0;
}

typedef struct { const char *dir; int err; } GCtx;
int get_entry(HVol *v, const HEnt *e, const char *dest);

static int get_child(HVol *v, const HEnt *e, void *ud)
{
    GCtx *g = ud;
    char nm[64], *p;
    if (broken || BREAK_CHECK()) { if (!broken) broken = 1; return 1; }
    if (!(p = malloc(600))) return 1;     /* recursion: keep paths off the stack */
    amiga_name(nm, e->name, O.raw);
    join(p, 600, g->dir, nm);
    if (get_entry(v, e, p)) g->err++;
    free(p);
    return 0;
}

int get_entry(HVol *v, const HEnt *e, const char *dest)
{
    if (e->type == CDR_FILE) return get_file(v, e, dest);
    if (io_mkdir(dest)) { errf("MacBrowse: cannot create folder %s: %s\n", dest, io_errstr()); return -1; }
    if (!O.quiet) printf("%s/\n", dest);
    {
        GCtx g;
        g.dir = dest; g.err = 0;
        if (hfs_readdir(v, e->id, get_child, &g) < 0) { errf("MacBrowse: %s\n", v->err); return -1; }
        io_setfiletime(dest, e->mddat);
        return g.err ? -1 : 0;
    }
}

int mb_get(HVol *v, const HEnt *e, const char *destdir)
{
    char nm[64];
    static char dest[600];
    if (!copybuf && !(copybuf = malloc(COPYBUF))) return -1;
    if (e->id == HFS_ROOT_ID) amiga_name(nm, v->name, O.raw);
    else amiga_name(nm, e->name, O.raw);
    join(dest, sizeof(dest), destdir, nm);
    return get_entry(v, e, dest);
}

static int cmd_get(HVol *v)
{
    int i, j, n, errs = 0, total = 0;
    HEnt *e = NULL;
    if (!O.npaths) { errf("GET: which file?\n"); free(e); return 10; }
    for (i = 0; i < O.npaths && !broken; i++) {
        free(e);
        n = resolve(v, O.paths[i], &e);
        if (!n) { errf("MacBrowse: %s: not found\n", O.paths[i]); errs++; continue; }
        total += n;
        for (j = 0; j < n && !broken; j++) {
            char nm[64];
            static char dest[600];
            const HEnt *x = &e[j];
            if (x->id == HFS_ROOT_ID) amiga_name(nm, v->name, O.raw);
            else amiga_name(nm, x->name, O.raw);
            if (!O.to) strcpy(dest, nm);
            else if (io_isdir(O.to) || n > 1 || O.npaths > 1 || O.to[strlen(O.to) - 1] == '/' || O.to[strlen(O.to) - 1] == ':')
                join(dest, sizeof(dest), O.to, nm);
            else snprintf(dest, sizeof(dest), "%s", O.to);
            if (get_entry(v, x, dest)) errs++;
        }
    }
    free(e);
    if (broken) { errf("***Break\n"); return 5; }
    return errs ? 10 : 0;
}

/* ------------------------------------------------------------------ PUT */

typedef struct { IOF *f; uint32_t base; const uint8_t *mem; } Src;

static int src_read(void *ud, uint32_t off, void *buf, uint32_t len)
{
    Src *s = ud;
    if (s->mem) { memcpy(buf, s->mem + s->base + off, len); return 0; }
    return io_pread(s->f, s->base + off, buf, len);
}

static void set_fourcc(uint8_t *dst, const char *s)
{
    int i;
    for (i = 0; i < 4; i++) dst[i] = *s ? lat2mac((uint8_t)*s++) : ' ';
}

static const char *basename_of(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, ':');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

static int put_file(HVol *v, const char *src, uint32_t parid, const char *asname)
{
    IOF *f = io_open(src, IO_READ), *side = NULL;
    long size;
    static uint8_t h[2048];
    uint8_t pn[32], *mem = NULL;
    MacMeta m;
    Src sd, sr;
    HForkSrc fd, fr;
    int kind = 0, r, hn;
    const char *kname[] = {"", " [MacBinary]", " [AppleSingle/Double]", " [BinHex]", " [+AppleDouble]"};
    uint32_t id;

    if (!f) { errf("MacBrowse: cannot open %s: %s\n", src, io_errstr()); return -1; }
    size = io_size(f);
    memset(&m, 0, sizeof(m));
    hn = size < (long)sizeof(h) ? (int)size : (int)sizeof(h);
    memset(h, 0, sizeof(h));
    if (hn > 0) io_pread(f, 0, h, hn);
    if (!O.plain && !O.sync) {
        if (size >= 128 && !mb_parse(h, size, &m)) kind = 1;
        else if (!ad_parse(f, size, &m) && m.has_data) kind = 2;
        else if (hqx_detect(h, hn)) {
            char msg[80];
            if (hqx_decode(f, size, &m, &mem, msg, sizeof(msg))) {
                errf("MacBrowse: %s: %s\n", src, msg);
                io_close(f);
                return -1;
            }
            kind = 3;
        }
    }
    if (!kind) {
        static char sp[600];
        const char *b = basename_of(src);
        size_t dl = (size_t)(b - src);
        memset(&m, 0, sizeof(m));
        m.doff = 0; m.dlen = (uint32_t)size; m.has_data = 1;
        type_from_ext(b, m.finfo);
        m.mddat = io_filemactime(src);
        m.crdat = m.mddat;
        if (!O.plain && dl + 3 + strlen(b) < sizeof(sp)) {
            MacMeta am;
            memcpy(sp, src, dl);
            sprintf(sp + dl, "._%s", b);
            if (io_exists(sp) && (side = io_open(sp, IO_READ))) {
                if (!ad_parse(side, io_size(side), &am)) {
                    if (am.have_finfo) { memcpy(m.finfo, am.finfo, 16); memcpy(m.fxinfo, am.fxinfo, 16); }
                    if (am.crdat) m.crdat = am.crdat;
                    if (am.mddat) m.mddat = am.mddat;
                    if (am.name[0]) memcpy(m.name, am.name, 32);
                    m.roff = am.roff; m.rlen = am.rlen;
                    kind = 4;
                } else { io_close(side); side = NULL; }
            }
        }
    }
    if (O.type) set_fourcc(m.finfo, O.type);
    if (O.creator) set_fourcc(m.finfo + 4, O.creator);
    if (!m.crdat) m.crdat = io_mactime();
    if (!m.mddat) m.mddat = m.crdat;

    if (asname) to_pname(pn, asname, (int)strlen(asname));
    else if (m.name[0] && (kind == 1 || kind == 2 || kind == 3 || kind == 4)) memcpy(pn, m.name, 32);
    else {
        const char *b = basename_of(src);
        if (strlen(b) > 31) errf("MacBrowse: warning: %s truncated to 31 characters\n", b);
        to_pname(pn, b, (int)strlen(b));
    }
    {   /* ':' is the Mac path separator */
        int i;
        for (i = 1; i <= pn[0]; i++) if (pn[i] == ':') pn[i] = '-';
    }

    sd.f = f; sd.base = m.doff; sd.mem = mem;
    sr.f = side ? side : f; sr.base = m.roff; sr.mem = side ? NULL : mem;
    fd.len = m.dlen; fd.read = src_read; fd.ud = &sd;
    fr.len = m.rlen; fr.read = src_read; fr.ud = &sr;

    if (O.replace) {
        HEnt old;
        if (hfs_lookup(v, parid, pn, &old) == 0) {
            if (old.type == CDR_DIR) { errf("MacBrowse: a folder of that name exists\n"); r = -1; goto out; }
            if (hfs_delete(v, parid, pn)) { errf("MacBrowse: cannot replace: %s\n", v->err); r = -1; goto out; }
        }
    }
    memset(mb_last_pn, 0, 32);
    memcpy(mb_last_pn, pn, pn[0] + 1);
    r = hfs_create_file(v, parid, pn, m.finfo, m.fxinfo, m.crdat, m.mddat, &fd, &fr, &id);
    if (r) {
        errf("MacBrowse: %s: %s\n", src, v->err);
        if (strstr(v->err, "full")) broken = 2;     /* no point trying the rest */
    }
    else if (!O.quiet) {
        printf("  ");
        pname_out(pn);
        printf(" (");
        fourcc_out(m.finfo); putchar('/'); fourcc_out(m.finfo + 4);
        printf(", data %lu, rsrc %lu)%s\n", (unsigned long)m.dlen, (unsigned long)m.rlen, kname[kind]);
    }
out:
    io_close(f);
    if (side) io_close(side);
    free(mem);
    return r;
}

int put_path(HVol *v, const char *src, uint32_t parid, const char *asname);

typedef struct { HVol *v; const char *dir; uint32_t parid; int err; } PCtx;

static int put_child(const char *name, int isdir, void *ud)
{
    PCtx *p = ud;
    char *path;
    (void)isdir;
    if (broken || BREAK_CHECK()) { if (!broken) broken = 1; return 1; }
    if (!strncmp(name, "._", 2) || !strcmp(name, ".DS_Store")) return 0;
    if (!(path = malloc(600))) return 1;
    join(path, 600, p->dir, name);
    if (put_path(p->v, path, p->parid, NULL)) p->err++;
    free(path);
    return 0;
}

int put_path(HVol *v, const char *src, uint32_t parid, const char *asname)
{
    if (io_isdir(src)) {
        uint8_t pn[32];
        uint32_t id;
        static HEnt ex;
        PCtx p;
        char *clean;
        size_t l;
        const char *nm;
        int r;
        if (!(clean = malloc(600))) return -1;
        snprintf(clean, 600, "%s", src);
        l = strlen(clean);
        while (l > 1 && clean[l - 1] == '/') clean[--l] = 0;
        nm = asname ? asname : basename_of(clean);
        if (!*nm) { errf("MacBrowse: give the folder a name with AS\n"); free(clean); return -1; }
        to_pname(pn, nm, (int)strlen(nm));
        if (hfs_lookup(v, parid, pn, &ex) == 0) {
            if (ex.type != CDR_DIR) { errf("MacBrowse: %s exists and is not a folder\n", nm); free(clean); return -1; }
            id = ex.id;
        } else if (hfs_mkdir(v, parid, pn, &id)) {
            errf("MacBrowse: mkdir %s: %s\n", nm, v->err);
            free(clean);
            return -1;
        } else if (!O.quiet) { printf("  "); pname_out(pn); printf(":\n"); }
        p.v = v; p.dir = clean; p.parid = id; p.err = 0;
        r = io_scandir(clean, put_child, &p);
        if (r < 0) errf("MacBrowse: cannot read %s\n", clean);
        free(clean);
        return (r < 0 || p.err) ? -1 : 0;
    }
    return put_file(v, src, parid, asname);
}

int mb_put(HVol *v, const char *src, uint32_t parid)
{
    return put_path(v, src, parid, NULL);
}

static int cmd_put(HVol *v)
{
    HEnt dir;
    int i, errs = 0;
    if (!O.npaths) { errf("PUT: which file?\n"); return 10; }
    if (O.to) {
        if (resolve1(v, O.to, &dir) || dir.type != CDR_DIR) { errf("MacBrowse: folder %s not found\n", O.to); return 10; }
    } else hfs_get_id(v, HFS_ROOT_ID, &dir);
    for (i = 0; i < O.npaths && !broken; i++)
        if (put_path(v, O.paths[i], dir.id, O.npaths == 1 ? O.as : NULL)) errs++;
    if (broken == 1) errf("***Break\n");
    return errs ? 10 : 0;
}

static int cmd_mkdir(HVol *v)
{
    HEnt dir;
    const char *leaf;
    uint8_t pn[32];
    int i, errs = 0;
    for (i = 0; i < O.npaths; i++) {
        if (resolve_parent(v, O.paths[i], &dir, &leaf)) { errf("MacBrowse: parent of %s not found\n", O.paths[i]); errs++; continue; }
        to_pname(pn, leaf, (int)strlen(leaf));
        if (hfs_mkdir(v, dir.id, pn, NULL)) { errf("MacBrowse: %s: %s\n", O.paths[i], v->err); errs++; }
        else if (!O.quiet) printf("  created %s\n", O.paths[i]);
    }
    return errs ? 10 : 0;
}

static int cmd_delete(HVol *v)
{
    HEnt *e = NULL;
    int i, j, n, errs = 0;
    for (i = 0; i < O.npaths; i++) {
        free(e);
        n = resolve(v, O.paths[i], &e);
        if (!n) { errf("MacBrowse: %s: not found\n", O.paths[i]); errs++; continue; }
        for (j = 0; j < n; j++) {
            if (e[j].id == HFS_ROOT_ID) { errf("MacBrowse: cannot delete the root folder\n"); errs++; continue; }
            if (hfs_delete(v, e[j].parid, e[j].name)) { char nm[64]; mac2lat_str(nm, e[j].name + 1, e[j].name[0]); errf("MacBrowse: %s: %s\n", nm, v->err); errs++; }
            else if (!O.quiet) { printf("  deleted "); pname_out(e[j].name); putchar('\n'); }
        }
    }
    return errs ? 10 : 0;
}

/* RENAME <path> <newpath>: rename and/or move.  If newpath is an existing folder the
   item moves into it under its own name; "" or ":" as newpath means the root folder. */
static int cmd_rename(HVol *v)
{
    HEnt e, dir;
    const char *leaf;
    uint8_t pn[32];
    if (O.npaths != 2) { errf("RENAME: give the old and the new path\n"); return 10; }
    if (resolve1(v, O.paths[0], &e)) { errf("MacBrowse: %s: not found\n", O.paths[0]); return 10; }
    if (!resolve1(v, O.paths[1], &dir) && dir.type == CDR_DIR && dir.id != e.id) {
        memcpy(pn, e.name, 32);
    } else {
        if (resolve_parent(v, O.paths[1], &dir, &leaf)) { errf("MacBrowse: parent of %s not found\n", O.paths[1]); return 10; }
        to_pname(pn, leaf, (int)strlen(leaf));
        if (e.id == HFS_ROOT_ID) dir.id = HFS_ROOT_PAR;
    }
    if (hfs_rename(v, e.parid, e.name, dir.id, pn)) { errf("MacBrowse: %s: %s\n", O.paths[0], v->err); return 10; }
    if (!O.quiet) printf("  renamed %s\n", O.paths[0]);
    return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    static HVol v;
    int rw, rc = 0;
    const char *c;

#ifdef AMIGA_HOST
    if (argc == 0) return gui_main(NULL);   /* started from Workbench */
#endif
    if (argc < 3 || parse_args(argc, argv)) {
#ifdef AMIGA_HOST
        if (argc == 2 && strcmp(argv[1], "?")) return gui_main(argv[1]);
#endif
        usage();
#ifdef AMIGA_HOST
        if (argc < 2) puts("(MacBrowse <image> alone, or a double-click on its icon, opens the window.)");
#endif
        return argc < 2 ? 0 : 10;
    }
    c = O.cmd;
    if (kw(c, "PARTS")) return cmd_parts();
    if (kw(c, "CREATE")) return cmd_create();
    if (kw(c, "SYNCIN") || kw(c, "SYNCOUT")) {
        if (!(copybuf = malloc(COPYBUF))) return 20;
        O.sync = 1;
        rc = kw(c, "SYNCIN") ? cmd_syncin() : cmd_syncout();
        free(copybuf);
        return broken == 1 ? 5 : rc;
    }
#ifdef AMIGA_HOST
    if (kw(c, "MOUNT")) {
        MountOpts m;
        if (O.npaths != 1) { errf("MOUNT: which device (e.g. HFV0:)?\n"); return 10; }
        memset(&m, 0, sizeof(m));
        m.device = O.paths[0];
        m.image = O.image;
        m.ro = O.ro;
        m.nodouble = O.nodouble;
        m.part = O.part;
        m.quiet = O.quiet;
        return hfv_mount(&m);
    }
    if (kw(c, "UNMOUNT")) return hfv_unmount(O.npaths ? O.paths[0] : O.image, O.quiet);
#endif
    rw = kw(c, "PUT") || kw(c, "MKDIR") || kw(c, "DELETE") || kw(c, "RENAME") || kw(c, "MOVE");
    if (hfs_open(&v, O.image, rw ? (O.force ? 2 : 1) : 0, O.part)) {
        char *nl;
        while ((nl = strchr(v.err, '\n'))) *nl = ' ';
        errf("MacBrowse: %s\n", v.err);
        if (strstr(v.err, "mounted")) errf("Quit ShapeShifter first; FORCE overrides this check.\n");
        return 20;
    }
    copybuf = malloc(COPYBUF);
    if (!copybuf) { hfs_close(&v); return 20; }
    if (kw(c, "INFO")) rc = cmd_info(&v);
    else if (kw(c, "LIST") || kw(c, "DIR")) rc = cmd_list(&v);
    else if (kw(c, "LS")) rc = cmd_ls(&v);
    else if (kw(c, "GET")) rc = cmd_get(&v);
    else if (kw(c, "PUT")) rc = cmd_put(&v);
    else if (kw(c, "MKDIR")) rc = cmd_mkdir(&v);
    else if (kw(c, "DELETE")) rc = cmd_delete(&v);
    else if (kw(c, "RENAME") || kw(c, "MOVE")) rc = cmd_rename(&v);
    else if (kw(c, "CHECK")) {
        int bad = hfs_check(&v, O.verbose);
        if (bad) printf("%d problem%s found\n", bad, bad == 1 ? "" : "s");
        else printf("Volume OK: B-trees, bitmap and counts consistent\n");
        rc = bad ? 10 : 0;
    } else { errf("MacBrowse: unknown command %s\n", c); usage(); rc = 10; }
    if (hfs_close(&v)) { errf("MacBrowse: %s\n", v.err); rc = 20; }
    free(copybuf);
    return rc;
}
