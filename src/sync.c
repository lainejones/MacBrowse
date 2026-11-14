/* sync.c - keep an HFS image and an AmigaDOS drawer the same ("synced drawer" of ShapeShifter).

   MacBrowse <image> CREATE <MB> [NAME name]     write a new, empty image
   MacBrowse <image> SYNCIN <drawer> [FREE mb] [NAME name]
        before the Mac starts: bring over what a Mac session left behind (as SYNCOUT), then
        make the image match the drawer; the image is created, or made anew, when needed
   MacBrowse <image> SYNCOUT <drawer>
        after the Mac has shut down: copy new and changed Mac files to the drawer and
        delete from the drawer what was deleted on the Mac

   What both sides looked like after the last sync is kept in "<image>.sync".  A file that
   changed on one side is copied to the other; one that changed on both is kept twice (the
   Mac's as "name (Mac)"); a file deleted on one side is deleted on the other only when it
   was not changed there.  Icons (.info), AppleDouble sidecars (._name) and the Mac's
   invisible items are not treated as items of their own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hfs.h"
#include "ops.h"
#include "charset.h"
#include "sync.h"

extern int put_path(HVol *v, const char *src, uint32_t parid, const char *asname);
extern int get_entry(HVol *v, const HEnt *e, const char *dest);

typedef struct {
    int      type;              /* 'F' or 'D' */
    int      parent;            /* index of the folder item, -1 = top */
    char    *aname;             /* AmigaDOS name (one path part) */
    uint8_t  mname[32];         /* Mac name, Pascal */
    uint32_t asize, amtime;     /* the Amiga file as it was */
    uint32_t mmd, mdlen, mrlen; /* the Mac file as it was */
    uint32_t macid;             /* folders: CNID, looked up when needed */
    int      seen, gone;
} Item;

static Item *items;
static int nitems, capitems;
static int n_in, n_out, n_del, n_conf, n_err;
static int full;
static uint32_t list_volcr;     /* creation date of the volume the list belongs to (0: none noted) */
static int no_delete;           /* this run must not delete anything in the drawer */
static char list_drawer[300];   /* the drawer the list was made for ("" when not noted) */

static void say(const char *what, const char *path)
{
    if (!O.quiet) printf("  %-10s %s\n", what, path);
}

static int add_item(int type, int parent, const char *aname, const uint8_t *mname)
{
    Item *it;
    if (nitems == capitems) {
        capitems = capitems ? capitems * 2 : 256;
        items = xrealloc(items, (unsigned long)capitems * sizeof(Item));
        if (!items) { errf("MacBrowse: out of memory\n"); exit(20); }
    }
    it = &items[nitems];
    memset(it, 0, sizeof(*it));
    it->type = type; it->parent = parent;
    it->aname = strdup(aname);
    memcpy(it->mname, mname, 32);
    return nitems++;
}

static int find_a(int parent, const char *aname)
{
    int i;
    for (i = 0; i < nitems; i++)
        if (items[i].parent == parent && !items[i].gone && !lat_strcasecmp(items[i].aname, aname)) return i;
    return -1;
}

static int find_m(int parent, const uint8_t *mname)
{
    int i;
    for (i = 0; i < nitems; i++)
        if (items[i].parent == parent && !items[i].gone &&
            !hfs_relstring(items[i].mname + 1, items[i].mname[0], mname + 1, mname[0])) return i;
    return -1;
}

/* AmigaDOS path of an item below the drawer */
static void apath(char *out, size_t sz, const char *drawer, int idx)
{
    int chain[HFS_MAXDEPTH * 4], n = 0, i;
    size_t l;
    for (i = idx; i >= 0 && n < (int)(sizeof(chain) / sizeof(chain[0])); i = items[i].parent) chain[n++] = i;
    snprintf(out, sz, "%s", drawer);
    for (i = n - 1; i >= 0; i--) {
        l = strlen(out);
        if (l && out[l - 1] != ':' && out[l - 1] != '/') snprintf(out + l, sz - l, "/");
        l = strlen(out);
        snprintf(out + l, sz - l, "%s", items[chain[i]].aname);
    }
}

static void join2(char *out, size_t sz, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l && dir[l - 1] != ':' && dir[l - 1] != '/') snprintf(out, sz, "%s/%s", dir, name);
    else snprintf(out, sz, "%s%s", dir, name);
}

static void side_path(char *out, size_t sz, const char *path)
{
    const char *a = strrchr(path, '/'), *b = strrchr(path, ':');
    size_t dl;
    if (b > a) a = b;
    a = a ? a + 1 : path;
    dl = (size_t)(a - path);
    if (dl >= sz) dl = sz - 1;
    memcpy(out, path, dl);
    snprintf(out + dl, sz - dl, "._%s", a);
}

static uint32_t file_size(const char *path)
{
    IOF *f = io_open(path, IO_READ);
    long s;
    if (!f) return 0xFFFFFFFFUL;
    s = io_size(f);
    io_close(f);
    return s < 0 ? 0xFFFFFFFFUL : (uint32_t)s;
}

/* ------------------------------------------------------------------ the list file */

static void list_name(char *out, size_t sz, const char *image) { snprintf(out, sz, "%s.sync", image); }

static void hexname(char *out, const uint8_t *pn)
{
    int i;
    for (i = 0; i <= pn[0]; i++) sprintf(out + 2 * i, "%02x", pn[i]);
}

static void load_list(const char *image)
{
    char nm[600], line[400];
    FILE *f;
    nitems = 0;
    list_volcr = 0;
    list_drawer[0] = 0;
    list_name(nm, sizeof(nm), image);
    if (!(f = fopen(nm, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        char type, hex[80], *an;
        if (line[0] == 'V') { list_volcr = strtoul(line + 1, NULL, 10); continue; }
        if (line[0] == 'W' && line[1] == ' ') {
            snprintf(list_drawer, sizeof(list_drawer), "%s", line + 2);
            list_drawer[strcspn(list_drawer, "\r\n")] = 0;
            continue;
        }
        long parent;
        unsigned long as, am, mm, md, mr;
        uint8_t pn[32];
        int i, used = 0;
        if (sscanf(line, "%c %ld %lu %lu %lu %lu %lu %79s %n", &type, &parent, &as, &am, &mm, &md, &mr, hex, &used) < 8) continue;
        an = line + used;
        an[strcspn(an, "\r\n")] = 0;
        if (!*an || (type != 'F' && type != 'D') || parent >= nitems) continue;
        memset(pn, 0, sizeof(pn));
        for (i = 0; i < 32 && hex[2 * i] && hex[2 * i + 1]; i++) {
            unsigned x;
            sscanf(hex + 2 * i, "%2x", &x);
            pn[i] = (uint8_t)x;
        }
        i = add_item(type, (int)parent, an, pn);
        items[i].asize = (uint32_t)as; items[i].amtime = (uint32_t)am;
        items[i].mmd = (uint32_t)mm; items[i].mdlen = (uint32_t)md; items[i].mrlen = (uint32_t)mr;
    }
    fclose(f);
}

/* Items that are gone are left out; the others keep their order, so a folder always comes
   before what is in it and the parent numbers can be mapped. */
static int save_list(const char *image)
{
    char nm[600], hex[80];
    FILE *f;
    int i, *map, n = 0;
    list_name(nm, sizeof(nm), image);
    if (!(f = fopen(nm, "w"))) { errf("MacBrowse: cannot write %s\n", nm); return -1; }
    fprintf(f, "V %lu\n", (unsigned long)list_volcr);
    if (list_drawer[0]) fprintf(f, "W %s\n", list_drawer);
    map = malloc(sizeof(int) * (nitems + 1));
    for (i = 0; i < nitems; i++) {
        Item *it = &items[i];
        if (it->gone || (it->parent >= 0 && map[it->parent] < 0)) { map[i] = -1; continue; }
        map[i] = n++;
        hexname(hex, it->mname);
        fprintf(f, "%c %d %lu %lu %lu %lu %lu %s %s\n", it->type, it->parent < 0 ? -1 : map[it->parent],
                (unsigned long)it->asize, (unsigned long)it->amtime, (unsigned long)it->mmd,
                (unsigned long)it->mdlen, (unsigned long)it->mrlen, hex, it->aname);
    }
    free(map);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ Mac side helpers */

static uint32_t mac_dir_id(HVol *v, int idx)
{
    HEnt e;
    uint32_t par;
    if (idx < 0) return HFS_ROOT_ID;
    if (items[idx].macid) return items[idx].macid;
    par = mac_dir_id(v, items[idx].parent);
    if (!par || hfs_lookup(v, par, items[idx].mname, &e) || e.type != CDR_DIR) return 0;
    return items[idx].macid = e.id;
}

static int invisible(const HEnt *e)
{
    static const char *skip[] = { "Desktop DB", "Desktop DF", "Desktop", "Trash", "Desktop Folder",
        "Temporary Items", "TheVolumeSettingsFolder", "TheFindByContentFolder", "Network Trash Folder",
        "Cleanup At Startup", "Shutdown Check", "VM Storage", "AppleShare PDS", "DesktopPrinters DB",
        "OpenFolderListDF\r", "Icon\r", NULL };
    int i;
    if (be16(e->finfo + 8) & 0x4000) return 1;
    for (i = 0; skip[i]; i++)
        if (e->name[0] == strlen(skip[i]) && !memcmp(e->name + 1, skip[i], e->name[0])) return 1;
    return 0;
}

typedef struct { HEnt *e; int n, cap; } EList;
static int collect_cb(HVol *v, const HEnt *e, void *ud)
{
    EList *l = ud;
    (void)v;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->e = xrealloc(l->e, (unsigned long)l->cap * sizeof(HEnt));
        if (!l->e) return 1;
    }
    l->e[l->n++] = *e;
    return 0;
}

/* Does the Mac file need a sidecar on the Amiga side to come back unchanged? */
static int wants_side(const HEnt *e, const char *aname)
{
    uint8_t fi[16];
    if (e->rlen) return 1;
    memset(fi, 0, sizeof(fi));
    type_from_ext(aname, fi);
    return memcmp(fi, e->finfo, 8) != 0 && memcmp(e->finfo, "\0\0\0\0\0\0\0\0", 8) != 0;
}

/* ------------------------------------------------------------------ Mac -> drawer */

static int mac_to_amiga(HVol *v, const HEnt *e, const char *path, int idx)
{
    char side[600];
    int ws = wants_side(e, items[idx].aname), r;
    O.mode = ws ? M_DOUBLE : M_DATA;
    { int q = O.quiet; O.quiet = 1; r = get_entry(v, e, path); O.quiet = q; }
    if (r) { n_err++; return -1; }
    if (!ws) { side_path(side, sizeof(side), path); if (io_exists(side)) io_delete(side); }
    items[idx].asize = file_size(path);
    items[idx].amtime = io_filemactime(path);
    items[idx].mmd = e->mddat; items[idx].mdlen = e->dlen; items[idx].mrlen = e->rlen;
    n_out++;
    return 0;
}

static void out_dir(HVol *v, uint32_t dirid, int parent, const char *adir)
{
    EList l;
    int i;
    char *path = malloc(600), an[64];
    memset(&l, 0, sizeof(l));
    if (!path || hfs_readdir(v, dirid, collect_cb, &l) < 0) { errf("MacBrowse: %s\n", v->err); n_err++; free(path); free(l.e); return; }
    for (i = 0; i < l.n && !broken; i++) {
        HEnt *e = &l.e[i];
        int idx;
        if (invisible(e)) continue;
        idx = find_m(parent, e->name);
        if (e->type == CDR_DIR) {
            if (idx < 0) {
                amiga_name(an, e->name, 0);
                idx = add_item('D', parent, an, e->name);
                apath(path, 600, adir, -1); join2(path, 600, adir, an);
                if (io_mkdir(path)) { errf("MacBrowse: cannot create drawer %s: %s\n", path, io_errstr()); n_err++; items[idx].gone = 1; continue; }
                say("new drawer", path);
            } else join2(path, 600, adir, items[idx].aname);
            items[idx].seen = 1;
            items[idx].macid = e->id;
            {
                char *sub = strdup(path);
                out_dir(v, e->id, idx, sub);
                free(sub);
            }
            continue;
        }
        if (idx < 0) {                          /* new on the Mac */
            amiga_name(an, e->name, 0);
            join2(path, 600, adir, an);
            if (io_exists(path)) {              /* the drawer got one of that name meanwhile */
                char an2[80];
                snprintf(an2, sizeof(an2), "%.50s (Mac)", an);
                strcpy(an, an2);
                join2(path, 600, adir, an);
                n_conf++;
            }
            idx = add_item('F', parent, an, e->name);
            items[idx].seen = 1;
            if (!mac_to_amiga(v, e, path, idx)) say("from Mac", path); else items[idx].gone = 1;
            continue;
        }
        items[idx].seen = 1;
        if (items[idx].type != 'F') continue;
        if (e->mddat == items[idx].mmd && e->dlen == items[idx].mdlen && e->rlen == items[idx].mrlen) continue;
        join2(path, 600, adir, items[idx].aname);
        if (io_exists(path) && (file_size(path) != items[idx].asize || io_filemactime(path) != items[idx].amtime)) {
            /* changed on both sides: keep both */
            char an2[80];
            int j;
            snprintf(an2, sizeof(an2), "%.50s (Mac)", items[idx].aname);
            join2(path, 600, adir, an2);
            j = add_item('F', parent, an2, e->name);
            items[j].seen = 1;
            items[idx].gone = 1;            /* the Amiga one is put on the Mac anew by SYNCIN */
            if (!mac_to_amiga(v, e, path, j)) say("both changed", path); else items[j].gone = 1;
            n_conf++;
            continue;
        }
        if (!mac_to_amiga(v, e, path, idx)) say("changed", path);
    }
    free(l.e);
    free(path);
}

/* What the list has and the Mac no longer has was deleted on the Mac.  Not when the Mac
   disk is not the one the list was made for (erased on the Mac, or another image), and not
   when most of the drawer would go: then nothing is deleted here, and the next SYNCIN
   puts the files on the Mac again. */
static void out_deleted(const char *drawer)
{
    int i, files = 0, miss = 0;
    char path[600], side[600];
    for (i = 0; i < nitems; i++) {
        if (items[i].gone || items[i].type != 'F') continue;
        files++;
        if (!items[i].seen) miss++;
    }
    if (!O.force && (no_delete || (miss > 10 && miss * 2 > files))) {
        if (miss) {
            errf("MacBrowse: %d of %d files are missing on the Mac disk%s.\n"
                 "Nothing was deleted in %s.\n", miss, files,
                 no_delete ? ", which is not the disk of the last sync" : "", drawer);
            n_conf++;
        }
        for (i = 0; i < nitems; i++) if (!items[i].seen) items[i].gone = 1;
        return;
    }
    for (i = nitems - 1; i >= 0; i--) {         /* contents before their folder */
        Item *it = &items[i];
        if (it->gone || it->seen) continue;
        if (it->parent >= 0 && !items[it->parent].seen && !items[it->parent].gone) { /* handled with the parent */ }
        apath(path, sizeof(path), drawer, i);
        it->gone = 1;
        if (!io_exists(path)) continue;
        if (it->type == 'D') {
            if (!io_delete(path)) { say("deleted", path); n_del++; }
            continue;                           /* not empty: something new is in it, it stays */
        }
        if (file_size(path) != it->asize || io_filemactime(path) != it->amtime) {
            say("kept", path);                  /* changed here since: it stays and goes to the Mac again */
            continue;
        }
        if (io_delete(path)) { errf("MacBrowse: cannot delete %s: %s\n", path, io_errstr()); n_err++; continue; }
        side_path(side, sizeof(side), path);
        if (io_exists(side)) io_delete(side);
        say("deleted", path);
        n_del++;
    }
}

static int sync_out(HVol *v, const char *drawer)
{
    int i;
    uint32_t cr = be32(v->mdb + 2);             /* drCrDate */
    no_delete = list_volcr && list_volcr != cr;
    list_volcr = cr;
    for (i = 0; i < nitems; i++) items[i].seen = 0;
    out_dir(v, HFS_ROOT_ID, -1, drawer);
    if (broken || n_err) return -1;             /* an incomplete walk must not delete anything */
    out_deleted(drawer);
    return 0;
}

/* ------------------------------------------------------------------ drawer -> Mac */

static int skip_amiga(const char *name)
{
    size_t l = strlen(name);
    if (!strncmp(name, "._", 2) || !strcmp(name, ".DS_Store")) return 1;
    if (l > 5 && !lat_strcasecmp(name + l - 5, ".info")) return 1;
    return 0;
}

typedef struct { char **n; int *d; int cnt, cap; } NList;
static int names_cb(const char *name, int isdir, void *ud)
{
    NList *l = ud;
    if (l->cnt == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->n = xrealloc(l->n, (unsigned long)l->cap * sizeof(char *));
        l->d = xrealloc(l->d, (unsigned long)l->cap * sizeof(int));
        if (!l->n || !l->d) return 1;
    }
    l->n[l->cnt] = strdup(name); l->d[l->cnt] = isdir; l->cnt++;
    return 0;
}

static void in_dir(HVol *v, const char *adir, int parent)
{
    NList l;
    int i;
    char *path = malloc(600);
    uint32_t parid = mac_dir_id(v, parent);
    memset(&l, 0, sizeof(l));
    if (!path || !parid || io_scandir(adir, names_cb, &l)) {
        errf("MacBrowse: cannot read %s\n", adir); n_err++; free(path); return;
    }
    for (i = 0; i < l.cnt && !broken && !full; i++) {
        const char *name = l.n[i];
        int idx;
        HEnt e;
        if (skip_amiga(name)) continue;
        if (strlen(name) > 31) { errf("MacBrowse: %s: name longer than 31 characters, left out\n", name); continue; }
        join2(path, 600, adir, name);
        idx = find_a(parent, name);
        if (l.d[i]) {
            if (idx < 0 || items[idx].type != 'D' || !mac_dir_id(v, idx)) {
                uint8_t pn[32];
                uint32_t id;
                mb_to_pname(pn, name);
                if (hfs_lookup(v, parid, pn, &e) == 0 && e.type == CDR_DIR) id = e.id;
                else if (hfs_mkdir(v, parid, pn, &id)) {
                    errf("MacBrowse: %s: %s\n", path, v->err); n_err++;
                    if (strstr(v->err, "full")) full = 1;
                    continue;
                } else say("new folder", path);
                if (idx >= 0) items[idx].gone = 1;
                idx = add_item('D', parent, name, pn);
                items[idx].macid = id;
            }
            items[idx].seen = 1;
            {
                char *sub = strdup(path);
                in_dir(v, sub, idx);
                free(sub);
            }
            continue;
        }
        {
            uint32_t sz = file_size(path), mt = io_filemactime(path);
            if (idx >= 0 && items[idx].type == 'F' && items[idx].asize == sz && items[idx].amtime == mt &&
                hfs_lookup(v, parid, items[idx].mname, &e) == 0) { items[idx].seen = 1; continue; }
            if (idx >= 0 && hfs_lookup(v, parid, items[idx].mname, &e) == 0 && e.type == CDR_FILE)
                hfs_delete(v, parid, items[idx].mname);         /* the name on the Mac may differ */
            O.replace = 1;
            { int q = O.quiet, pr; O.quiet = 1; pr = put_path(v, path, parid, NULL); O.quiet = q;
            if (pr) {
                n_err++;
                if (broken == 2) { full = 1; broken = 0; }
                continue;
            } }
            if (idx < 0) idx = add_item('F', parent, name, mb_last_pn);
            else memcpy(items[idx].mname, mb_last_pn, 32);
            items[idx].type = 'F';
            items[idx].seen = 1;
            items[idx].asize = sz; items[idx].amtime = mt;
            if (hfs_lookup(v, parid, mb_last_pn, &e) == 0) { items[idx].mmd = e.mddat; items[idx].mdlen = e.dlen; items[idx].mrlen = e.rlen; }
            say("to Mac", path);
            n_in++;
        }
    }
    for (i = 0; i < l.cnt; i++) free(l.n[i]);
    free(l.n); free(l.d);
    free(path);
}

/* What the list has and the drawer no longer has was deleted on the Amiga. */
static void in_deleted(HVol *v)
{
    int i;
    for (i = nitems - 1; i >= 0; i--) {
        Item *it = &items[i];
        HEnt e;
        uint32_t par;
        if (it->gone || it->seen) continue;
        it->gone = 1;
        par = mac_dir_id(v, it->parent);
        if (!par || hfs_lookup(v, par, it->mname, &e)) continue;
        if (it->type == 'F' && (e.mddat != it->mmd || e.dlen != it->mdlen || e.rlen != it->mrlen)) continue; /* changed on the Mac */
        if (!hfs_delete(v, par, it->mname)) { n_del++; if (!O.quiet) printf("  %-10s %s\n", "deleted", it->aname); }
    }
}

/* ------------------------------------------------------------------ a new image */

static void p16(uint8_t *p, uint16_t x) { wb16(p, x); }
static void p32(uint8_t *p, uint32_t x) { wb32(p, x); }

static void bt_header(uint8_t *n, uint32_t nodes, int depth, uint32_t root, uint32_t recs, uint32_t first,
                      uint32_t last, int keylen, int used)
{
    int i;
    memset(n, 0, 512);
    n[8] = 1; p16(n + 10, 3);
    p16(n + 14, (uint16_t)depth); p32(n + 16, root); p32(n + 20, recs); p32(n + 24, first); p32(n + 28, last);
    p16(n + 32, 512); p16(n + 34, (uint16_t)keylen); p32(n + 36, nodes); p32(n + 40, nodes - used);
    for (i = 0; i < used; i++) n[0xf8 + (i >> 3)] |= 0x80 >> (i & 7);
    p16(n + 504, 0x1f8); p16(n + 506, 0xf8); p16(n + 508, 0x78); p16(n + 510, 0x0e);
}

/* The same layout as ShapeShifter's "Create..." (and tools/mkhfs.py there). */
int hfs_mkimage(const char *path, uint32_t total, const char *latname)
{
    uint32_t spb = total / 65536 + 1, bm, alst, nblk, absz, bt, btblk, nodes, now = io_mactime(), s, xt, ct;
    static uint8_t sec[1024], zero[8192];
    uint8_t name[32], *bits;
    IOF *f;
    int nl, kl, o, o2, o3, i;
    if (total < 2048 || total > 4194303UL) { errf("MacBrowse: an image has 1 MB to 2047 MB\n"); return -1; }
    bm = (total / spb + 4095) / 4096;
    alst = 3 + bm;
    nblk = (total - alst - 2) / spb;
    absz = spb * 512;
    bt = total * 8 < 1048576UL ? total * 8 : 1048576UL;
    bt = bt / absz * absz;
    if (bt < absz) bt = absz;
    btblk = bt / absz; nodes = bt / 512;
    nl = (int)strlen(latname); if (nl > 27) nl = 27;
    if (!nl) { latname = "Amiga"; nl = 5; }
    for (i = 0; i < nl; i++) { uint8_t c = lat2mac((uint8_t)latname[i]); name[i] = c == ':' ? '-' : c; }
    if (!(f = io_open(path, IO_CREATE))) { errf("MacBrowse: cannot create %s: %s\n", path, io_errstr()); return -1; }
    for (s = 0; s < total; s += 16) {
        uint32_t n = total - s < 16 ? total - s : 16;
        if (io_write(f, zero, n * 512)) { errf("MacBrowse: %s: %s\n", path, io_errstr()); io_close(f); return -1; }
    }
    memset(sec, 0, 512);
    p16(sec, 0x4244); p32(sec + 2, now); p32(sec + 6, now); p16(sec + 10, 0x0100); p16(sec + 14, 3);
    p16(sec + 18, (uint16_t)nblk); p32(sec + 20, absz); p32(sec + 24, 4 * absz); p16(sec + 28, (uint16_t)alst);
    p32(sec + 30, 16); p16(sec + 34, (uint16_t)(nblk - 2 * btblk));
    sec[36] = (uint8_t)nl; memcpy(sec + 37, name, nl);
    p32(sec + 70, 1); p32(sec + 74, bt); p32(sec + 78, bt);
    p32(sec + 130, bt); p16(sec + 134, 0); p16(sec + 136, (uint16_t)btblk);
    p32(sec + 146, bt); p16(sec + 150, (uint16_t)btblk); p16(sec + 152, (uint16_t)btblk);
    if (io_pwrite(f, 2 * 512, sec, 512) || io_pwrite(f, (total - 2) * 512, sec, 512)) goto werr;
    if (!(bits = calloc(bm, 512))) { io_close(f); return -1; }
    for (i = 0; i < (int)(2 * btblk); i++) bits[i >> 3] |= 0x80 >> (i & 7);
    if (io_pwrite(f, 3 * 512, bits, bm * 512)) { free(bits); goto werr; }
    free(bits);
    xt = alst; ct = alst + btblk * spb;
    bt_header(sec, nodes, 0, 0, 0, 0, 0, 7, 1);
    if (io_pwrite(f, xt * 512, sec, 512)) goto werr;
    bt_header(sec, nodes, 1, 1, 2, 1, 1, 37, 2);
    if (io_pwrite(f, ct * 512, sec, 512)) goto werr;
    memset(sec, 0, 512);                        /* the catalog's one leaf: root folder and its thread */
    sec[8] = 0xff; sec[9] = 1; p16(sec + 10, 2);
    o = 14;
    kl = 6 + nl; if (!(kl & 1)) kl++;
    sec[o] = (uint8_t)kl; p32(sec + o + 2, 1); sec[o + 6] = (uint8_t)nl; memcpy(sec + o + 7, name, nl);
    o2 = o + 1 + kl;
    sec[o2] = 1; p32(sec + o2 + 6, 2); p32(sec + o2 + 10, now); p32(sec + o2 + 14, now);
    o2 += 70;
    sec[o2] = 7; p32(sec + o2 + 2, 2);
    sec[o2 + 8] = 3; p32(sec + o2 + 18, 1); sec[o2 + 22] = (uint8_t)nl; memcpy(sec + o2 + 23, name, nl);
    o3 = o2 + 8 + 46;
    p16(sec + 506, (uint16_t)o3); p16(sec + 508, (uint16_t)o2); p16(sec + 510, (uint16_t)o);
    if (io_pwrite(f, (ct + 1) * 512, sec, 512)) goto werr;
    io_close(f);
    return 0;
werr:
    errf("MacBrowse: %s: %s\n", path, io_errstr());
    io_close(f);
    return -1;
}

/* How large an image the drawer needs: its files, each rounded up, and room to spare. */
typedef struct { uint32_t kb; uint32_t files; } Need;
static void need_dir(const char *adir, Need *nd)
{
    NList l;
    int i;
    char *path = malloc(600);
    memset(&l, 0, sizeof(l));
    if (!path || io_scandir(adir, names_cb, &l)) { free(path); return; }
    for (i = 0; i < l.cnt; i++) {
        join2(path, 600, adir, l.n[i]);
        if (l.d[i]) { char *sub = strdup(path); need_dir(sub, nd); free(sub); nd->files++; }
        else {
            uint32_t sz = file_size(path);
            if (sz != 0xFFFFFFFFUL) nd->kb += (sz + 1023) / 1024 + 32;   /* up to one allocation block lost */
            nd->files++;
        }
        free(l.n[i]);
    }
    free(l.n); free(l.d); free(path);
}

static uint32_t image_sectors(const char *drawer, int freemb)
{
    Need nd;
    uint32_t kb;
    nd.kb = 0; nd.files = 0;
    need_dir(drawer, &nd);
    kb = nd.kb + nd.files + (uint32_t)(freemb > 0 ? freemb : 32) * 1024 + 2048;
    kb += kb / 8;
    if (kb < 4096) kb = 4096;
    return kb > 2097151UL ? 0 : kb * 2;
}

/* ------------------------------------------------------------------ commands */

static const char *base_name(const char *p)
{
    static char b[64];
    size_t l = strlen(p);
    const char *a, *c;
    snprintf(b, sizeof(b), "%s", p);
    while (l > 1 && (b[l - 1] == '/')) b[--l] = 0;
    if (l && b[l - 1] == ':') b[--l] = 0;
    a = strrchr(b, '/'); c = strrchr(b, ':');
    if (c > a) a = c;
    return a ? a + 1 : b;
}

static void report(const char *what)
{
    if (!O.quiet || n_conf || n_err)
        printf("%s: %d to the Mac, %d from the Mac, %d deleted%s%s\n", what, n_in, n_out, n_del,
               n_conf ? "; look at the messages above and at files named \"(Mac)\"" : "", n_err ? ", WITH ERRORS" : "");
}

int cmd_create(void)
{
    long mb = O.npaths ? atol(O.paths[0]) : 0;
    if (mb < 1 || mb > 2047) { errf("MacBrowse: CREATE <megabytes> (1 to 2047) [NAME name]\n"); return 10; }
    if (io_exists(O.image) && !O.force) { errf("MacBrowse: %s exists (FORCE writes over it)\n", O.image); return 10; }
    if (hfs_mkimage(O.image, (uint32_t)mb * 2048, O.name ? O.name : base_name(O.image))) return 20;
    if (!O.quiet) printf("%s: empty Mac disk of %ld MB\n", O.image, mb);
    return 0;
}

int cmd_syncout(void)
{
    static HVol v;
    const char *drawer = O.npaths ? O.paths[0] : NULL;
    int r;
    if (!drawer || !io_isdir(drawer)) { errf("MacBrowse: SYNCOUT <drawer>\n"); return 10; }
    if (hfs_open(&v, O.image, HFS_EXCL, O.part)) { errf("MacBrowse: %s\n", v.err); return 20; }
    load_list(O.image);
    if (list_drawer[0] && lat_strcasecmp(list_drawer, drawer) && !O.force) {
        errf("MacBrowse: %s was synced with the drawer %s, not with %s (FORCE: do it all the same)\n", O.image, list_drawer, drawer);
        hfs_close(&v);
        return 10;
    }
    snprintf(list_drawer, sizeof(list_drawer), "%s", drawer);
    r = sync_out(&v, drawer);
    hfs_close(&v);
    if (!r || nitems) save_list(O.image);
    report("Synced drawer");
    return r || n_err ? 10 : 0;
}

int cmd_syncin(void)
{
    static HVol v;
    const char *drawer = O.npaths ? O.paths[0] : NULL;
    int made = 0, i;
    if (!drawer || !io_isdir(drawer)) { errf("MacBrowse: SYNCIN <drawer> [FREE megabytes] [NAME name]\n"); return 10; }
    load_list(O.image);
    if (nitems && list_drawer[0] && lat_strcasecmp(list_drawer, drawer)) {
        if (!O.quiet) printf("%s belonged to the drawer %s: it is made anew\n", O.image, list_drawer);
        for (i = 0; i < nitems; i++) free(items[i].aname);
        nitems = 0;
        made = 1;
    }
    snprintf(list_drawer, sizeof(list_drawer), "%s", drawer);
    if (io_exists(O.image) && made != 1) {
        /* what a Mac session left behind comes over first, whatever state the image is in */
        if (nitems) {
            if (hfs_open(&v, O.image, HFS_EXCL, O.part)) { errf("MacBrowse: %s\n", v.err); return 20; }
            if (sync_out(&v, drawer)) { hfs_close(&v); save_list(O.image); report("Synced drawer"); return 10; }
            hfs_close(&v);
            save_list(O.image);
            load_list(O.image);
        }
    } else nitems = 0;
again:
    if (!io_exists(O.image) || made == 1 || hfs_open(&v, O.image, 1, O.part)) {
        /* none yet, one that was not shut down cleanly, or one that is too small: a new one */
        uint32_t secs = image_sectors(drawer, O.freemb);
        if (made == 2) { errf("MacBrowse: the drawer does not fit a synced disk\n"); return 20; }
        if (!secs) { errf("MacBrowse: %s is too large for a synced disk (2 GB at most)\n", drawer); return 20; }
        if (!O.quiet) printf("New Mac disk %s, %lu MB\n", O.image, (unsigned long)(secs / 2048));
        if (hfs_mkimage(O.image, secs, O.name ? O.name : base_name(drawer))) return 20;
        for (i = 0; i < nitems; i++) free(items[i].aname);
        nitems = 0;
        made = 2;
        if (hfs_open(&v, O.image, 1, O.part)) { errf("MacBrowse: %s\n", v.err); return 20; }
    }
    for (i = 0; i < nitems; i++) { items[i].seen = 0; items[i].macid = 0; }
    full = 0;
    in_dir(&v, drawer, -1);
    if (full && made != 2) {
        hfs_close(&v);
        n_in = n_err = 0;
        made = 1;
        goto again;
    }
    if (!broken && !n_err) in_deleted(&v);
    list_volcr = be32(v.mdb + 2);
    if (hfs_close(&v)) { errf("MacBrowse: %s\n", v.err); n_err++; }
    save_list(O.image);
    report("Synced drawer");
    return n_err || broken ? 10 : 0;
}
