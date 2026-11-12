/* handler.c - MacHFS-Handler: mounts a Mac HFS disk image (ShapeShifter .hfv, Apple
 * partition map, DiskCopy 4.2) as an AmigaDOS volume, e.g. HFV0:.
 *
 * Mount with MacMount (or a DOSDriver/mountlist entry, see docs/HFV0):
 *   Handler = L:MacHFS-Handler   GlobVec = -1   Stacksize = 16384
 *   Startup = "<image> [RO] [NODOUBLE] [PART=n] [LOG=file]"
 *
 * - Directories and files map onto HFS folders and data forks.  The Finder type and
 *   creator appear as the file comment "TYPE/CRTR"; SetComment with that form sets them.
 * - A file with a resource fork is followed by a virtual AppleDouble file "._name"
 *   (resource fork + Finder info).  Writing "._name" sets the resource fork and the
 *   Finder info of "name" (creating "name" if needed); deleting it empties the
 *   resource fork.  NODOUBLE hides them.
 * - The image is held with an exclusive lock while mounted (ShapeShifter cannot open it
 *   at the same time).  A read-write mount is refused unless the volume is marked as
 *   cleanly unmounted.  The bitmap and MDB are written (and the volume marked clean)
 *   about 2 s after the last change, on ACTION_FLUSH, INHIBIT and DIE.
 *
 * Built with -nostartfiles (hstart.s, hlibc.c): see the Makefile.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <devices/timer.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "hfs.h"
#include "charset.h"
#include "version.h"

struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct Library *UtilityBase = NULL;    /* libgcc 32-bit multiply/divide call utility.library */

const char hversion[] = "$VER: MacHFS-Handler " MB_VERSION " (" MB_DATE ")";

#define ADBIT       0x80000000UL    /* lock key / fib_DiskKey bit: the "._name" view */
#define MAC_AMIGA   2335305600UL    /* seconds 1904-01-01 .. 1978-01-01 */
#define AD_EPOCH    3029529600UL    /* seconds 1904-01-01 .. 2000-01-01 */
#define ADCAP       8192            /* AppleDouble header bytes buffered while writing */
#define IDLE_SECS   2
#ifndef ID_BUSY
#define ID_BUSY     0x42555359UL    /* 'BUSY' */
#endif

typedef struct MLock {
    struct FileLock fl;             /* must be first: BPTRs point here */
    struct MLock *next;
    uint32_t id, parid;             /* CNID; parent folder */
    uint8_t name[32];               /* Mac name (files are found by parent + name) */
    int isdir, vad;
} MLock;

typedef struct OFork {              /* one per open fork, shared by its handles */
    struct OFork *next;
    int refs;
    HFork f;
} OFork;

typedef struct MFile {
    struct MFile *next;
    MLock *lock;
    OFork *of;
    uint32_t pos;
    int vad;                        /* AppleDouble view of the resource fork */
    int locked;                     /* Mac "locked" flag: no writes */
    /* AppleDouble being written (MODE_NEWFILE on "._name") */
    int adw, adparsed, adbad, created;
    uint8_t *adh;
    uint32_t adlen, adroff, adrlen;
} MFile;

typedef struct {
    HEnt e;
    int vad;
} Obj;

static HVol vol;
static struct Process *me;
static struct MsgPort *port;
static struct DeviceNode *devnode;
static struct DeviceList *volnode;
static MLock *locks;
static MFile *files;
static OFork *oforks;
static int readonly, nodouble, inhibited, dying, idle_pending, activity, part = -1;
static char imgpath[256], volname[40], logname[128];
static BPTR logfh;
static struct MsgPort *tport;
static struct timerequest *treq;
static ULONG tmask;

/* ------------------------------------------------------------------ helpers */

static int logser;          /* LOG=SERIAL: debug output straight to the serial port */

static void ser_out(const char *s, int n)
{
    volatile UWORD *serdatr = (volatile UWORD *)0xDFF018, *serdat = (volatile UWORD *)0xDFF030;
    while (n--) {
        int guard = 100000;
        while (!(*serdatr & 0x2000) && --guard) ;
        *serdat = 0x100 | (UBYTE)*s++;
    }
}

static void dbg(const char *fmt, ...)
{
    static char b[300];
    va_list ap;
    int n;
    if (!logfh && !logser) return;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 1) n = sizeof(b) - 1;
    if (logser) { ser_out(b, n); return; }
    Write(logfh, b, n);
}

static void bstr2c(BSTR b, char *out, int max)
{
    const UBYTE *s = BADDR(b);
    int n = 0;
    if (s) {
        n = s[0];
        if (n > max - 1) n = max - 1;
        memcpy(out, s + 1, n);
    }
    out[n] = 0;
}

static void c2bstr(UBYTE *b, const char *s, int max)
{
    int n = strlen(s);
    if (n > max - 1) n = max - 1;
    b[0] = (UBYTE)n;
    memcpy(b + 1, s, n);
}

static void mac2date(uint32_t t, struct DateStamp *ds)
{
    if (t < MAC_AMIGA) { ds->ds_Days = ds->ds_Minute = ds->ds_Tick = 0; return; }
    t -= MAC_AMIGA;
    ds->ds_Days = t / 86400UL;
    t %= 86400UL;
    ds->ds_Minute = t / 60;
    ds->ds_Tick = (t % 60) * TICKS_PER_SECOND;
}

static uint32_t date2mac(const struct DateStamp *ds)
{
    return MAC_AMIGA + (uint32_t)ds->ds_Days * 86400UL + (uint32_t)ds->ds_Minute * 60UL +
           (uint32_t)ds->ds_Tick / TICKS_PER_SECOND;
}

/* Error code for the last hfs.c failure. */
static LONG hfserr(LONG def)
{
    const char *e = vol.err;
    if (strstr(e, "full")) return ERROR_DISK_FULL;
    if (strstr(e, "already exists")) return ERROR_OBJECT_EXISTS;
    if (strstr(e, "not empty")) return ERROR_DIRECTORY_NOT_EMPTY;
    if (strstr(e, "into itself")) return ERROR_OBJECT_IN_USE;
    if (strstr(e, "name must be") || strstr(e, "cannot contain") || strstr(e, "at most 27")) return ERROR_INVALID_COMPONENT_NAME;
    if (strstr(e, "not found")) return ERROR_OBJECT_NOT_FOUND;
    if (strstr(e, "read-only")) return ERROR_DISK_WRITE_PROTECTED;
    if (strstr(e, "out of memory")) return ERROR_NO_FREE_STORE;
    return def;
}

static void touch(void) { activity = 1; }

/* Latin-1 AmigaDOS name -> Mac Roman Pascal string; 0 or an error code. */
static LONG mac_name(const char *s, uint8_t *pn)
{
    int n = strlen(s), i;
    if (!n || n > 31) return ERROR_INVALID_COMPONENT_NAME;
    pn[0] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        if (s[i] == ':' || s[i] == '/') return ERROR_INVALID_COMPONENT_NAME;
        pn[1 + i] = lat2mac((uint8_t)s[i]);
    }
    return 0;
}

static OFork *ofork_find(uint32_t id, int fork)
{
    OFork *o;
    for (o = oforks; o; o = o->next)
        if (o->f.id == id && o->f.fork == fork) return o;
    return NULL;
}

/* Current logical length of a fork: an open fork may be ahead of the catalog. */
static uint32_t fork_len(const HEnt *e, int fork)
{
    OFork *o = ofork_find(e->id, fork);
    if (o) return o->f.len;
    return fork == FORK_DATA ? e->dlen : e->rlen;
}

/* ------------------------------------------------------------------ AppleDouble */

/* AppleDouble v2 header: real name, dates, Finder info, then the resource fork. */
static int ad_build(uint8_t *b, const HEnt *e, uint32_t rlen)
{
    int off = 26 + 12 * 4, i = 0;
    memset(b, 0, off);
    wb32(b, 0x00051607UL);
    wb32(b + 4, 0x00020000UL);
    wb16(b + 24, 4);
    wb32(b + 26 + 12 * i, 3); wb32(b + 30 + 12 * i, off); wb32(b + 34 + 12 * i, e->name[0]); i++;
    memcpy(b + off, e->name + 1, e->name[0]);
    off += e->name[0];
    wb32(b + 26 + 12 * i, 8); wb32(b + 30 + 12 * i, off); wb32(b + 34 + 12 * i, 16); i++;
    wb32(b + off, e->crdat - AD_EPOCH);
    wb32(b + off + 4, e->mddat - AD_EPOCH);
    wb32(b + off + 8, e->bkdat ? e->bkdat - AD_EPOCH : 0x80000000UL);
    wb32(b + off + 12, e->mddat - AD_EPOCH);
    off += 16;
    wb32(b + 26 + 12 * i, 9); wb32(b + 30 + 12 * i, off); wb32(b + 34 + 12 * i, 32); i++;
    memcpy(b + off, e->finfo, 16);
    memcpy(b + off + 16, e->fxinfo, 16);
    off += 32;
    wb32(b + 26 + 12 * i, 2); wb32(b + 30 + 12 * i, off); wb32(b + 34 + 12 * i, rlen);
    return off;
}

static uint32_t ad_hdrlen(const HEnt *e) { return 26 + 12 * 4 + e->name[0] + 16 + 32; }

/* ------------------------------------------------------------------ objects & paths */

static LONG get_root(Obj *o)
{
    o->vad = 0;
    if (hfs_get_id(&vol, HFS_ROOT_ID, &o->e)) return ERROR_OBJECT_NOT_FOUND;
    return 0;
}

static LONG obj_from_lock(MLock *l, Obj *o)
{
    if (!l) return get_root(o);
    o->vad = l->vad;
    if (l->isdir) {
        if (hfs_get_id(&vol, l->id, &o->e)) return ERROR_OBJECT_NOT_FOUND;
    } else if (hfs_lookup(&vol, l->parid, l->name, &o->e) || o->e.id != l->id) return ERROR_OBJECT_NOT_FOUND;
    return 0;
}

typedef struct { const char *name; HEnt *out; int found; } FindCtx;

static int find_cb(HVol *v, const HEnt *e, void *ud)
{
    FindCtx *c = ud;
    char an[40];
    (void)v;
    amiga_name(an, e->name, 0);
    if (!lat_strcasecmp(an, c->name)) { *c->out = *e; c->found = 1; return 1; }
    return 0;
}

/* Find a (Latin-1) name in a folder.  "._name" gives the AppleDouble view of "name". */
static LONG lookup_child(uint32_t dirid, const char *name, Obj *o)
{
    uint8_t pn[32];
    int n = strlen(name);
    o->vad = 0;
    if (!mac_name(name, pn)) {
        int r = hfs_lookup(&vol, dirid, pn, &o->e);
        if (r == 0) return 0;
        if (r < 0) return hfserr(ERROR_OBJECT_NOT_FOUND);
    }
    /* names with characters that have no Latin-1 form are shown with a stand-in */
    if (n <= 31) {
        FindCtx c;
        c.name = name; c.out = &o->e; c.found = 0;
        hfs_readdir(&vol, dirid, find_cb, &c);
        if (c.found) return 0;
    }
    if (!nodouble && name[0] == '.' && name[1] == '_' && name[2]) {
        if (!lookup_child(dirid, name + 2, o) && o->e.type == CDR_FILE && !o->vad) { o->vad = 1; return 0; }
        o->vad = 0;
    }
    return ERROR_OBJECT_NOT_FOUND;
}

/* Resolve a path relative to a lock.  With leaf, the last component is not looked up
   but copied to leaf (o = its folder); leaf is "" if the path ends at a folder. */
static LONG walk(MLock *base, BSTR bname, Obj *o, char *leaf)
{
    static char path[256];
    char *p, *c, comp[108];
    LONG err;
    bstr2c(bname, path, sizeof(path));
    c = strchr(path, ':');
    if (c) { p = c + 1; err = get_root(o); }
    else { p = path; err = obj_from_lock(base, o); }
    if (err) return err;
    if (leaf) leaf[0] = 0;
    for (;;) {
        char *s = p;
        int n, slash, last;
        while (*p && *p != '/') p++;
        n = (int)(p - s);
        slash = *p == '/';
        if (slash) p++;
        last = !*p;
        if (!n) {
            if (!slash) break;
            /* empty component: parent */
            if (o->vad) { o->vad = 0; continue; }
            if (o->e.type == CDR_FILE) { if (hfs_get_id(&vol, o->e.parid, &o->e)) return ERROR_OBJECT_NOT_FOUND; continue; }
            if (o->e.id == HFS_ROOT_ID) return ERROR_OBJECT_NOT_FOUND;
            if (hfs_get_id(&vol, o->e.parid, &o->e)) return ERROR_OBJECT_NOT_FOUND;
            continue;
        }
        if (n > (int)sizeof(comp) - 1) return ERROR_INVALID_COMPONENT_NAME;
        memcpy(comp, s, n); comp[n] = 0;
        if (o->vad || o->e.type != CDR_DIR) return ERROR_OBJECT_WRONG_TYPE;
        if (leaf && last) { strcpy(leaf, comp); return 0; }
        err = lookup_child(o->e.id, comp, o);
        if (err) return last ? err : (err == ERROR_OBJECT_NOT_FOUND ? ERROR_DIR_NOT_FOUND : err);
        if (last) break;
    }
    return 0;
}

static uint32_t obj_key(const Obj *o) { return o->e.id | (o->vad ? ADBIT : 0); }

static LONG lock_check(uint32_t key, LONG access)
{
    MLock *l;
    for (l = locks; l; l = l->next)
        if ((uint32_t)l->fl.fl_Key == key && (access == EXCLUSIVE_LOCK || l->fl.fl_Access == EXCLUSIVE_LOCK))
            return ERROR_OBJECT_IN_USE;
    return 0;
}

static int in_use(uint32_t id)
{
    MLock *l;
    for (l = locks; l; l = l->next)
        if (l->id == id) return 1;
    return 0;
}

static MLock *mklock(const Obj *o, LONG access)
{
    MLock *l = calloc(1, sizeof(MLock));
    if (!l) return NULL;
    l->fl.fl_Key = (LONG)obj_key(o);
    l->fl.fl_Access = access;
    l->fl.fl_Task = port;
    l->fl.fl_Volume = MKBADDR(volnode);
    l->id = o->e.id;
    l->parid = o->e.parid;
    memcpy(l->name, o->e.name, 32);
    l->isdir = o->e.type == CDR_DIR;
    l->vad = o->vad;
    l->next = locks;
    locks = l;
    return l;
}

static void freelock(MLock *l)
{
    MLock **pp;
    for (pp = &locks; *pp; pp = &(*pp)->next)
        if (*pp == l) { *pp = l->next; free(l); return; }
}

static MLock *lockptr(BPTR b) { return (MLock *)BADDR(b); }

/* After a rename: keep locks and open forks pointing at the record. */
static void renamed(uint32_t id, uint32_t parid, const uint8_t *name)
{
    MLock *l;
    OFork *o;
    for (l = locks; l; l = l->next)
        if (l->id == id) { l->parid = parid; memcpy(l->name, name, 32); }
    for (o = oforks; o; o = o->next)
        if (o->f.id == id) { o->f.parid = parid; memcpy(o->f.name, name, 32); }
}

/* ------------------------------------------------------------------ examine */

static void fill_fib(struct FileInfoBlock *fib, const Obj *o)
{
    const HEnt *e = &o->e;
    char nm[40];
    memset(fib, 0, sizeof(*fib));
    fib->fib_DiskKey = (LONG)obj_key(o);
    if (e->id == HFS_ROOT_ID && !o->vad) {
        fib->fib_DirEntryType = ST_ROOT;
        strcpy(nm, volname);
    } else {
        fib->fib_DirEntryType = e->type == CDR_DIR ? ST_USERDIR : ST_FILE;
        nm[0] = nm[1] = 0;
        if (o->vad) { nm[0] = '.'; nm[1] = '_'; }
        amiga_name(nm + (o->vad ? 2 : 0), e->name, 0);
    }
    fib->fib_EntryType = fib->fib_DirEntryType;
    c2bstr((UBYTE *)fib->fib_FileName, nm, sizeof(fib->fib_FileName));
    if (e->type == CDR_FILE) {
        uint32_t sz = o->vad ? ad_hdrlen(e) + fork_len(e, FORK_RSRC) : fork_len(e, FORK_DATA);
        fib->fib_Size = sz;
        fib->fib_NumBlocks = (sz + 511) / 512;
        if (e->flags & 0x01) fib->fib_Protection = FIBF_WRITE | FIBF_DELETE;
        if (!o->vad && (e->finfo[0] | e->finfo[1] | e->finfo[2] | e->finfo[3] |
                        e->finfo[4] | e->finfo[5] | e->finfo[6] | e->finfo[7])) {
            char cm[10];
            int i;
            for (i = 0; i < 4; i++) { cm[i] = e->finfo[i] ? (char)e->finfo[i] : ' '; cm[5 + i] = e->finfo[4 + i] ? (char)e->finfo[4 + i] : ' '; }
            cm[4] = '/';
            cm[9] = 0;
            c2bstr((UBYTE *)fib->fib_Comment, cm, sizeof(fib->fib_Comment));
        }
    }
    mac2date(e->mddat, &fib->fib_Date);
    /* resume point for ExNext: the Mac name of this entry */
    memcpy(fib->fib_Reserved, e->name, 32);
}

static LONG examine_next(MLock *l, struct FileInfoBlock *fib)
{
    uint32_t dirid = l ? l->id : HFS_ROOT_ID;
    static uint8_t prev[32];
    Obj o;
    int r, start;
    if (l && (!l->isdir || l->vad)) return ERROR_OBJECT_WRONG_TYPE;
    start = (uint32_t)fib->fib_DiskKey == dirid;
    memcpy(prev, fib->fib_Reserved, 32);
    if (prev[0] > 31) start = 1;
    o.vad = 0;
    if (!start && (fib->fib_DiskKey & ADBIT)) {
        /* the "._name" view went out last: now the file itself */
        if (!hfs_lookup(&vol, dirid, prev, &o.e)) { fill_fib(fib, &o); return 0; }
    }
    r = hfs_dir_next(&vol, dirid, start ? NULL : prev, &o.e);
    if (r < 0) return hfserr(ERROR_OBJECT_NOT_FOUND);
    if (r) return ERROR_NO_MORE_ENTRIES;
    o.vad = !nodouble && o.e.type == CDR_FILE && fork_len(&o.e, FORK_RSRC) > 0;
    fill_fib(fib, &o);
    return 0;
}

static void fill_info(struct InfoData *id)
{
    memset(id, 0, sizeof(*id));
    id->id_NumSoftErrors = 0;
    id->id_UnitNumber = 0;
    id->id_DiskState = readonly ? ID_WRITE_PROTECTED : ID_VALIDATED;
    id->id_NumBlocks = vol.nblocks;
    id->id_NumBlocksUsed = vol.nblocks - hfs_free_blocks(&vol);
    id->id_BytesPerBlock = vol.alsize;
    id->id_DiskType = inhibited ? ID_BUSY : ID_DOS_DISK;
    id->id_VolumeNode = MKBADDR(volnode);
    id->id_InUse = locks ? DOSTRUE : 0;
}

/* ------------------------------------------------------------------ open files */

static OFork *ofork_get(const HEnt *e, int fork, LONG *err)
{
    OFork *o = ofork_find(e->id, fork);
    if (o) { o->refs++; return o; }
    o = calloc(1, sizeof(OFork));
    if (!o) { *err = ERROR_NO_FREE_STORE; return NULL; }
    if (hfs_fork_open(&vol, e, fork, &o->f)) { *err = hfserr(ERROR_OBJECT_NOT_FOUND); free(o); return NULL; }
    o->refs = 1;
    o->next = oforks;
    oforks = o;
    return o;
}

static LONG ofork_put(OFork *o)
{
    OFork **pp;
    LONG err = 0;
    if (--o->refs > 0) {
        if (o->f.dirty && hfs_fork_sync(&o->f, 0)) err = hfserr(ERROR_SEEK_ERROR);
        return err;
    }
    if (hfs_fork_sync(&o->f, 1)) err = hfserr(ERROR_SEEK_ERROR), dbg("sync failed: %s\n", vol.err);
    hfs_fork_close(&o->f);
    for (pp = &oforks; *pp; pp = &(*pp)->next)
        if (*pp == o) { *pp = o->next; break; }
    free(o);
    return err;
}

static void unlink_file(MFile *f)
{
    MFile **pp;
    for (pp = &files; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
}

/* Open o (a file, or the "._name" view) on fh.  trunc: MODE_NEWFILE. */
static LONG open_obj(struct FileHandle *fh, const Obj *o, LONG access, int trunc, int created)
{
    MFile *f;
    LONG err = 0;
    if (o->e.type != CDR_FILE) return ERROR_OBJECT_WRONG_TYPE;
    if ((err = lock_check(obj_key(o), access))) return err;
    if (trunc && (o->e.flags & 0x01)) return ERROR_WRITE_PROTECTED;
    f = calloc(1, sizeof(MFile));
    if (!f) return ERROR_NO_FREE_STORE;
    f->vad = o->vad;
    f->locked = o->e.flags & 0x01;
    f->created = created;
    f->of = ofork_get(&o->e, o->vad ? FORK_RSRC : FORK_DATA, &err);
    if (!f->of) { free(f); return err; }
    f->lock = mklock(o, access);
    if (!f->lock) { ofork_put(f->of); free(f); return ERROR_NO_FREE_STORE; }
    if (trunc) {
        if (f->of->f.len && hfs_fork_setlen(&f->of->f, 0)) err = hfserr(ERROR_DISK_FULL);
        if (o->vad) {
            f->adw = 1;
            f->adh = malloc(ADCAP);
            if (!f->adh && !err) err = ERROR_NO_FREE_STORE;
        }
        touch();
    }
    if (err) {
        ofork_put(f->of); freelock(f->lock); free(f->adh); free(f);
        return err;
    }
    f->next = files;
    files = f;
    fh->fh_Arg1 = (LONG)f;
    return 0;
}

/* AppleDouble writer: parse the header once enough of it is there. */
static void ad_parse(MFile *f)
{
    uint32_t n, i;
    if (f->adparsed || f->adbad || f->adlen < 26) return;
    if (be32(f->adh) != 0x00051607UL && be32(f->adh) != 0x00051600UL) { f->adbad = 1; return; }
    n = be16(f->adh + 24);
    if (n > 200) { f->adbad = 1; return; }
    if (f->adlen < 26 + 12 * n) return;
    f->adroff = 0xFFFFFFFFUL;
    f->adrlen = 0;
    for (i = 0; i < n; i++) {
        uint8_t *d = f->adh + 26 + 12 * i;
        if (be32(d) == 2) { f->adroff = be32(d + 4); f->adrlen = be32(d + 8); }
    }
    f->adparsed = 1;
}

/* Bytes of the AppleDouble stream at [pos, pos+len) that belong to the resource fork. */
static LONG ad_rsrc(MFile *f, uint32_t pos, const uint8_t *buf, uint32_t len)
{
    uint32_t a = pos, b = pos + len;
    if (a < f->adroff) a = f->adroff;
    if (f->adroff + f->adrlen < b) b = f->adroff + f->adrlen;
    if (a >= b) return 0;
    if (hfs_fork_pwrite(&f->of->f, a - f->adroff, buf + (a - pos), b - a)) return hfserr(ERROR_DISK_FULL);
    return 0;
}

static LONG ad_write(MFile *f, const uint8_t *buf, uint32_t len)
{
    uint32_t pos = f->pos, old = f->adlen;
    LONG err;
    if (f->adbad) return ERROR_OBJECT_WRONG_TYPE;
    if (pos < ADCAP) {
        uint32_t n = len < ADCAP - pos ? len : ADCAP - pos;
        memcpy(f->adh + pos, buf, n);
        if (pos + n > f->adlen) f->adlen = pos + n;
    }
    if (!f->adparsed) {
        ad_parse(f);
        if (f->adbad) return ERROR_OBJECT_WRONG_TYPE;
        if (!f->adparsed) return pos + len > ADCAP ? ERROR_OBJECT_WRONG_TYPE : 0;
        /* resource fork bytes that came in before the header was complete */
        if (old > pos) old = pos;
        if ((err = ad_rsrc(f, 0, f->adh, old))) return err;
    }
    return ad_rsrc(f, pos, buf, len);
}

/* End of an AppleDouble write: Finder info and dates into the catalog. */
static void ad_finish(MFile *f)
{
    uint32_t n, i;
    uint8_t *fi = NULL;
    uint32_t cr = 0, md = 0;
    if (!f->adparsed) return;
    n = be16(f->adh + 24);
    for (i = 0; i < n; i++) {
        uint8_t *d = f->adh + 26 + 12 * i;
        uint32_t id = be32(d), off = be32(d + 4), len = be32(d + 8);
        if (off + len > f->adlen || off + len < off) continue;
        if (id == 9 && len >= 32) fi = f->adh + off;
        if (id == 8 && len >= 8) { cr = be32(f->adh + off) + AD_EPOCH; md = be32(f->adh + off + 4) + AD_EPOCH; }
    }
    if (hfs_fork_sync(&f->of->f, 0)) dbg("ad sync: %s\n", vol.err);
    f->of->f.touched = 0;
    if (hfs_set_info(&vol, f->of->f.parid, f->of->f.name, fi, fi ? fi + 16 : NULL, cr, md))
        dbg("ad set_info: %s\n", vol.err);
}

static LONG end_file(MFile *f)
{
    LONG err = 0;
    uint32_t id = f->of->f.id, parid = f->of->f.parid;
    uint8_t name[32];
    int created = f->created && (f->adbad || !f->adparsed);
    memcpy(name, f->of->f.name, 32);
    if (f->adw && f->adparsed) ad_finish(f);
    err = ofork_put(f->of);
    freelock(f->lock);
    unlink_file(f);
    free(f->adh);
    free(f);
    if (created && !in_use(id)) {
        /* a "._name" that was not AppleDouble created "name" for nothing */
        HEnt e;
        if (!hfs_lookup(&vol, parid, name, &e) && !e.dlen && !e.rlen) hfs_delete(&vol, parid, name);
    }
    return err;
}

static LONG do_read(MFile *f, uint8_t *buf, LONG len, LONG *res)
{
    uint32_t flen = f->of->f.len, got = 0;
    *res = -1;
    if (len < 0) return ERROR_BAD_NUMBER;
    if (f->vad) {
        uint8_t hdr[200];
        HEnt e;
        uint32_t hl;
        if (hfs_lookup(&vol, f->of->f.parid, f->of->f.name, &e)) return ERROR_OBJECT_NOT_FOUND;
        hl = ad_build(hdr, &e, flen);
        while (len && f->pos < hl) {
            buf[got++] = hdr[f->pos++];
            len--;
        }
        if (len && f->pos < hl + flen) {
            uint32_t n = hl + flen - f->pos;
            if ((uint32_t)len < n) n = len;
            if (hfs_fork_pread(&f->of->f, f->pos - hl, buf + got, n)) return hfserr(ERROR_SEEK_ERROR);
            got += n;
            f->pos += n;
        }
        *res = got;
        return 0;
    }
    if (f->pos < flen) {
        uint32_t n = flen - f->pos;
        if ((uint32_t)len < n) n = len;
        if (hfs_fork_pread(&f->of->f, f->pos, buf, n)) return hfserr(ERROR_SEEK_ERROR);
        f->pos += n;
        got = n;
    }
    *res = got;
    return 0;
}

static uint32_t file_size(MFile *f)
{
    if (f->vad) {
        HEnt e;
        if (f->adw) return f->adlen > f->pos ? f->adlen : f->pos;
        if (hfs_lookup(&vol, f->of->f.parid, f->of->f.name, &e)) return f->of->f.len;
        return ad_hdrlen(&e) + f->of->f.len;
    }
    return f->of->f.len;
}

static LONG do_write(MFile *f, const uint8_t *buf, LONG len, LONG *res)
{
    LONG err;
    *res = -1;
    if (readonly) return ERROR_DISK_WRITE_PROTECTED;
    if (f->locked) return ERROR_WRITE_PROTECTED;
    if (len < 0) return ERROR_BAD_NUMBER;
    touch();
    if (f->vad) {
        if (!f->adw) {
            /* MODE_OLDFILE/READWRITE on "._name": only the resource fork part is writable */
            HEnt e;
            uint32_t hl;
            if (hfs_lookup(&vol, f->of->f.parid, f->of->f.name, &e)) return ERROR_OBJECT_NOT_FOUND;
            hl = ad_hdrlen(&e);
            if (f->pos < hl) return ERROR_WRITE_PROTECTED;
            if (hfs_fork_pwrite(&f->of->f, f->pos - hl, buf, len)) return hfserr(ERROR_DISK_FULL);
        } else if ((err = ad_write(f, buf, len))) return err;
        f->pos += len;
        *res = len;
        return 0;
    }
    if (hfs_fork_pwrite(&f->of->f, f->pos, buf, len)) return hfserr(ERROR_DISK_FULL);
    f->pos += len;
    *res = len;
    return 0;
}

static LONG do_seek(MFile *f, LONG off, LONG mode, LONG *res)
{
    uint32_t size = file_size(f);
    long np;
    *res = -1;
    if (mode == OFFSET_BEGINNING) np = off;
    else if (mode == OFFSET_CURRENT) np = (long)f->pos + off;
    else if (mode == OFFSET_END) np = (long)size + off;
    else return ERROR_SEEK_ERROR;
    if (np < 0 || (uint32_t)np > size) return ERROR_SEEK_ERROR;
    *res = f->pos;
    f->pos = (uint32_t)np;
    return 0;
}

static LONG do_setsize(MFile *f, LONG off, LONG mode, LONG *res)
{
    long ns;
    MFile *g;
    *res = -1;
    if (readonly) return ERROR_DISK_WRITE_PROTECTED;
    if (f->locked) return ERROR_WRITE_PROTECTED;
    if (f->vad) return ERROR_ACTION_NOT_KNOWN;
    if (mode == OFFSET_BEGINNING) ns = off;
    else if (mode == OFFSET_CURRENT) ns = (long)f->pos + off;
    else if (mode == OFFSET_END) ns = (long)f->of->f.len + off;
    else return ERROR_SEEK_ERROR;
    if (ns < 0) return ERROR_SEEK_ERROR;
    touch();
    if (hfs_fork_setlen(&f->of->f, (uint32_t)ns)) return hfserr(ERROR_DISK_FULL);
    for (g = files; g; g = g->next)
        if (g->of == f->of && g->pos > (uint32_t)ns) g->pos = (uint32_t)ns;
    *res = ns;
    return 0;
}

/* ------------------------------------------------------------------ flushing */

static void do_flush(void)
{
    OFork *o;
    for (o = oforks; o; o = o->next)
        if (o->f.dirty && hfs_fork_sync(&o->f, 0)) dbg("flush: sync %lu: %s\n", (unsigned long)o->f.id, vol.err);
    if (vol.dirty) {
        if (hfs_flush(&vol)) dbg("flush: %s\n", vol.err);
        else dbg("flushed: volume marked clean\n");
    }
    activity = 0;
}

static void arm_timer(void)
{
    if (!treq || idle_pending) return;
    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs = IDLE_SECS;
    treq->tr_time.tv_micro = 0;
    SendIO(&treq->tr_node);
    idle_pending = 1;
}

static void diskchange(int inserted)
{
    struct MsgPort *ip = CreateMsgPort();
    struct IOStdReq *io;
    struct InputEvent ie;
    if (!ip) return;
    io = (struct IOStdReq *)CreateIORequest(ip, sizeof(struct IOStdReq));
    if (io && !OpenDevice("input.device", 0, (struct IORequest *)io, 0)) {
        memset(&ie, 0, sizeof(ie));
        ie.ie_Class = inserted ? IECLASS_DISKINSERTED : IECLASS_DISKREMOVED;
        io->io_Command = IND_WRITEEVENT;
        io->io_Data = &ie;
        io->io_Length = sizeof(ie);
        DoIO((struct IORequest *)io);
        CloseDevice((struct IORequest *)io);
    }
    if (io) DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(ip);
}

/* ------------------------------------------------------------------ packets */

#define WRITEOP(t) ((t) == ACTION_DELETE_OBJECT || (t) == ACTION_RENAME_OBJECT || (t) == ACTION_CREATE_DIR || \
                    (t) == ACTION_SET_COMMENT || (t) == ACTION_SET_DATE || (t) == ACTION_RENAME_DISK || \
                    (t) == ACTION_FINDOUTPUT || (t) == ACTION_SET_PROTECT)

static LONG act_open(struct DosPacket *pkt, int type)
{
    struct FileHandle *fh = BADDR(pkt->dp_Arg1);
    MLock *base = lockptr(pkt->dp_Arg2);
    char leaf[108];
    Obj dir, o;
    LONG err;
    int created = 0;
    if ((err = walk(base, pkt->dp_Arg3, &dir, leaf))) return err;
    if (!leaf[0]) return dir.e.type == CDR_DIR ? ERROR_OBJECT_WRONG_TYPE : ERROR_OBJECT_NOT_FOUND;
    err = lookup_child(dir.e.id, leaf, &o);
    if (!err && o.vad && type == ACTION_FINDINPUT && !fork_len(&o.e, FORK_RSRC)) err = ERROR_OBJECT_NOT_FOUND;
    if (err == ERROR_OBJECT_NOT_FOUND && type != ACTION_FINDINPUT) {
        /* create it; "._name" creates "name" and writes its AppleDouble view */
        uint8_t pn[32], fi[16];
        int ad = !nodouble && leaf[0] == '.' && leaf[1] == '_' && leaf[2];
        const char *nm = ad ? leaf + 2 : leaf;
        uint32_t now = io_mactime();
        if (readonly) return ERROR_DISK_WRITE_PROTECTED;
        if ((err = mac_name(nm, pn))) return err;
        memset(fi, 0, sizeof(fi));
        type_from_ext(nm, fi);
        if (hfs_create(&vol, dir.e.id, pn, fi, NULL, now, now, NULL)) return hfserr(ERROR_DISK_FULL);
        touch();
        if (hfs_lookup(&vol, dir.e.id, pn, &o.e)) return ERROR_OBJECT_NOT_FOUND;
        o.vad = ad;
        created = ad;
        err = 0;
        type = ACTION_FINDOUTPUT;
    }
    if (err) return err;
    if (o.e.type == CDR_DIR) return ERROR_OBJECT_WRONG_TYPE;
    if (type == ACTION_FINDOUTPUT) {
        if (readonly) return ERROR_DISK_WRITE_PROTECTED;
        return open_obj(fh, &o, EXCLUSIVE_LOCK, 1, created);
    }
    /* "._name" with MODE_READWRITE on a file without resource fork: write it from scratch */
    if (type == ACTION_FINDUPDATE && o.vad && !fork_len(&o.e, FORK_RSRC) && !readonly)
        return open_obj(fh, &o, SHARED_LOCK, 1, 0);
    return open_obj(fh, &o, SHARED_LOCK, 0, 0);
}

static LONG act_delete(struct DosPacket *pkt)
{
    char leaf[108];
    Obj dir, o;
    LONG err;
    if ((err = walk(lockptr(pkt->dp_Arg1), pkt->dp_Arg2, &dir, leaf))) return err;
    if (!leaf[0]) return ERROR_OBJECT_WRONG_TYPE;
    err = lookup_child(dir.e.id, leaf, &o);
    if (err == ERROR_OBJECT_NOT_FOUND && !nodouble && leaf[0] == '.' && leaf[1] == '_')
        return 0;   /* "._name" whose file is already gone */
    if (err) return err;
    if (o.vad) {
        HFork f;
        if (lock_check(obj_key(&o), EXCLUSIVE_LOCK)) return ERROR_OBJECT_IN_USE;
        if (!o.e.rlen) return 0;
        if (o.e.flags & 0x01) return ERROR_DELETE_PROTECTED;
        if (hfs_fork_open(&vol, &o.e, FORK_RSRC, &f)) return hfserr(ERROR_OBJECT_NOT_FOUND);
        err = hfs_fork_setlen(&f, 0) || hfs_fork_sync(&f, 1) ? hfserr(ERROR_DISK_FULL) : 0;
        hfs_fork_close(&f);
        touch();
        return err;
    }
    if (o.e.id == HFS_ROOT_ID) return ERROR_OBJECT_WRONG_TYPE;
    if (in_use(o.e.id)) return ERROR_OBJECT_IN_USE;
    if (o.e.flags & 0x01) return ERROR_DELETE_PROTECTED;
    touch();
    if (hfs_delete(&vol, o.e.parid, o.e.name)) return hfserr(ERROR_OBJECT_IN_USE);
    return 0;
}

static LONG act_rename(struct DosPacket *pkt)
{
    MLock *l2 = lockptr(pkt->dp_Arg3);
    char oleaf[108], leaf[108];
    Obj odir, src, dir, t;
    uint8_t pn[32];
    LONG err;
    if (l2 && l2->fl.fl_Task != port) return ERROR_RENAME_ACROSS_DEVICES;
    if ((err = walk(lockptr(pkt->dp_Arg1), pkt->dp_Arg2, &odir, oleaf))) return err;
    if ((err = walk(l2, pkt->dp_Arg4, &dir, leaf))) return err;
    if (!oleaf[0] || !leaf[0]) return ERROR_OBJECT_WRONG_TYPE;
    err = lookup_child(odir.e.id, oleaf, &src);
    if (err == ERROR_OBJECT_NOT_FOUND && !nodouble && oleaf[0] == '.' && oleaf[1] == '_' &&
        leaf[0] == '.' && leaf[1] == '_' && !lookup_child(dir.e.id, leaf + 2, &t))
        return 0;   /* "._name" already moved along with its file */
    if (err) return err;
    if (src.vad) {
        /* the view moves with its file; renaming it on its own is not possible */
        if (!lookup_child(dir.e.id, leaf, &t) && t.vad && t.e.id == src.e.id) return 0;
        return ERROR_OBJECT_WRONG_TYPE;
    }
    if (src.e.id == HFS_ROOT_ID) return ERROR_OBJECT_WRONG_TYPE;
    if (!nodouble && leaf[0] == '.' && leaf[1] == '_') return ERROR_INVALID_COMPONENT_NAME;
    if ((err = mac_name(leaf, pn))) return err;
    if (!lookup_child(dir.e.id, leaf, &t) && t.e.id != src.e.id) return ERROR_OBJECT_EXISTS;
    touch();
    if (hfs_rename(&vol, src.e.parid, src.e.name, dir.e.id, pn)) return hfserr(ERROR_OBJECT_EXISTS);
    renamed(src.e.id, dir.e.id, pn);
    return 0;
}

static LONG act_rename_disk(struct DosPacket *pkt)
{
    char nm[64];
    uint8_t pn[32];
    UBYTE *b;
    int i, n;
    bstr2c(pkt->dp_Arg1, nm, sizeof(nm));
    n = strlen(nm);
    if (!n || n > 27) return ERROR_INVALID_COMPONENT_NAME;
    for (i = 0; i < n; i++) if (nm[i] == ':' || nm[i] == '/') return ERROR_INVALID_COMPONENT_NAME;
    mac_name(nm, pn);
    touch();
    if (hfs_rename(&vol, HFS_ROOT_PAR, vol.name, HFS_ROOT_PAR, pn)) return hfserr(ERROR_INVALID_COMPONENT_NAME);
    renamed(HFS_ROOT_ID, HFS_ROOT_PAR, pn);
    /* new BSTR for the volume node */
    b = AllocVec(32, MEMF_PUBLIC | MEMF_CLEAR);
    if (b) {
        amiga_name(volname, vol.name, 0);
        c2bstr(b, volname, 32);
        Forbid();
        volnode->dl_Name = MKBADDR(b);   /* the old name is left allocated: someone may hold it */
        Permit();
    }
    return 0;
}

static LONG act_set(struct DosPacket *pkt)
{
    char leaf[108];
    Obj dir, o;
    LONG err;
    if ((err = walk(lockptr(pkt->dp_Arg2), pkt->dp_Arg3, &dir, leaf))) return err;
    if (leaf[0]) { if ((err = lookup_child(dir.e.id, leaf, &o))) return err; }
    else o = dir;
    if (pkt->dp_Type == ACTION_SET_PROTECT) return 0;
    if (o.vad || o.e.id == HFS_ROOT_ID) return 0;
    if (pkt->dp_Type == ACTION_SET_DATE) {
        OFork *of = ofork_find(o.e.id, FORK_DATA);
        uint32_t t = date2mac((struct DateStamp *)pkt->dp_Arg4);
        if (of) {           /* an open fork must not put "now" back on close */
            if (of->f.dirty && hfs_fork_sync(&of->f, 0)) return hfserr(ERROR_DISK_FULL);
            of->f.touched = 0;
        }
        touch();
        if (hfs_set_info(&vol, o.e.parid, o.e.name, NULL, NULL, 0, t)) return hfserr(ERROR_OBJECT_NOT_FOUND);
        return 0;
    }
    /* SET_COMMENT: "TYPE/CRTR" sets the Finder type and creator; other comments are ignored */
    {
        char cm[84];
        bstr2c(pkt->dp_Arg4, cm, sizeof(cm));
        if (o.e.type == CDR_FILE && strlen(cm) == 9 && cm[4] == '/') {
            uint8_t fi[16];
            int i;
            memcpy(fi, o.e.finfo, 16);
            for (i = 0; i < 4; i++) { fi[i] = (uint8_t)cm[i]; fi[4 + i] = (uint8_t)cm[5 + i]; }
            touch();
            if (hfs_set_info(&vol, o.e.parid, o.e.name, fi, NULL, 0, 0)) return hfserr(ERROR_OBJECT_NOT_FOUND);
        }
    }
    return 0;
}

static LONG act_parent(MLock *l, LONG *res)
{
    Obj o;
    LONG err;
    MLock *n;
    *res = 0;
    if (!l) return 0;
    if (l->vad || !l->isdir) {
        if ((err = obj_from_lock(l, &o))) return err;
        if (hfs_get_id(&vol, o.e.parid, &o.e)) return ERROR_OBJECT_NOT_FOUND;
    } else {
        if (l->id == HFS_ROOT_ID) return 0;
        if ((err = obj_from_lock(l, &o))) return err;
        if (hfs_get_id(&vol, o.e.parid, &o.e)) return ERROR_OBJECT_NOT_FOUND;
    }
    o.vad = 0;
    if ((err = lock_check(obj_key(&o), SHARED_LOCK))) return err;
    if (!(n = mklock(&o, SHARED_LOCK))) return ERROR_NO_FREE_STORE;
    *res = MKBADDR(n);
    return 0;
}

static LONG dup_lock(MLock *l, LONG *res, int force)
{
    Obj o;
    MLock *n;
    LONG err;
    *res = 0;
    if (l && !force && l->fl.fl_Access == EXCLUSIVE_LOCK) return ERROR_OBJECT_IN_USE;
    if ((err = obj_from_lock(l, &o))) return err;
    if (!(n = mklock(&o, SHARED_LOCK))) return ERROR_NO_FREE_STORE;
    *res = MKBADDR(n);
    return 0;
}

static int handle(struct DosPacket *pkt)
{
    LONG res1 = DOSTRUE, res2 = 0, t = pkt->dp_Type;
    Obj o;

    if (inhibited && t != ACTION_INHIBIT && t != ACTION_DIE && t != ACTION_DISK_INFO &&
        t != ACTION_IS_FILESYSTEM && t != ACTION_FREE_LOCK && t != ACTION_END && t != ACTION_FLUSH) {
        res1 = DOSFALSE; res2 = ERROR_NOT_A_DOS_DISK;
        goto reply;
    }
    if (readonly && WRITEOP(t) && t != ACTION_SET_PROTECT) {
        res1 = DOSFALSE; res2 = ERROR_DISK_WRITE_PROTECTED;
        goto reply;
    }
    switch (t) {
    case ACTION_LOCATE_OBJECT: {
        LONG acc = pkt->dp_Arg3 == EXCLUSIVE_LOCK ? EXCLUSIVE_LOCK : SHARED_LOCK;
        MLock *l;
        res1 = 0;
        if ((res2 = walk(lockptr(pkt->dp_Arg1), pkt->dp_Arg2, &o, NULL))) break;
        if (o.vad && !fork_len(&o.e, FORK_RSRC)) { res2 = ERROR_OBJECT_NOT_FOUND; break; }
        if ((res2 = lock_check(obj_key(&o), acc))) break;
        if (!(l = mklock(&o, acc))) { res2 = ERROR_NO_FREE_STORE; break; }
        res1 = MKBADDR(l);
        break;
    }
    case ACTION_FREE_LOCK:
        if (pkt->dp_Arg1) freelock(lockptr(pkt->dp_Arg1));
        break;
    case ACTION_COPY_DIR:
        res2 = dup_lock(lockptr(pkt->dp_Arg1), &res1, 0);
        break;
    case ACTION_COPY_DIR_FH:
        res2 = dup_lock(((MFile *)pkt->dp_Arg1)->lock, &res1, 1);
        break;
    case ACTION_PARENT:
        res2 = act_parent(lockptr(pkt->dp_Arg1), &res1);
        break;
    case ACTION_PARENT_FH:
        res2 = act_parent(((MFile *)pkt->dp_Arg1)->lock, &res1);
        break;
    case ACTION_SAME_LOCK: {
        MLock *a = lockptr(pkt->dp_Arg1), *b = lockptr(pkt->dp_Arg2);
        uint32_t ka = a ? (uint32_t)a->fl.fl_Key : HFS_ROOT_ID, kb = b ? (uint32_t)b->fl.fl_Key : HFS_ROOT_ID;
        res1 = ka == kb ? DOSTRUE : DOSFALSE;
        break;
    }
    case ACTION_EXAMINE_OBJECT:
        if ((res2 = obj_from_lock(lockptr(pkt->dp_Arg1), &o))) { res1 = DOSFALSE; break; }
        fill_fib(BADDR(pkt->dp_Arg2), &o);
        break;
    case ACTION_EXAMINE_FH:
        if ((res2 = obj_from_lock(((MFile *)pkt->dp_Arg1)->lock, &o))) { res1 = DOSFALSE; break; }
        fill_fib(BADDR(pkt->dp_Arg2), &o);
        break;
    case ACTION_EXAMINE_NEXT:
        res2 = examine_next(lockptr(pkt->dp_Arg1), BADDR(pkt->dp_Arg2));
        if (res2) res1 = DOSFALSE;
        break;
    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE:
        res2 = act_open(pkt, t);
        if (res2) res1 = DOSFALSE;
        break;
    case ACTION_FH_FROM_LOCK: {
        MLock *l = lockptr(pkt->dp_Arg2);
        LONG err = 0;
        MFile *f;
        if (!l || l->isdir) { res1 = DOSFALSE; res2 = ERROR_OBJECT_WRONG_TYPE; break; }
        if ((res2 = obj_from_lock(l, &o))) { res1 = DOSFALSE; break; }
        f = calloc(1, sizeof(MFile));
        if (!f) { res1 = DOSFALSE; res2 = ERROR_NO_FREE_STORE; break; }
        f->vad = o.vad;
        f->locked = o.e.flags & 0x01;
        f->of = ofork_get(&o.e, o.vad ? FORK_RSRC : FORK_DATA, &err);
        if (!f->of) { free(f); res1 = DOSFALSE; res2 = err; break; }
        f->lock = l;        /* the lock now belongs to the file handle */
        f->next = files;
        files = f;
        ((struct FileHandle *)BADDR(pkt->dp_Arg1))->fh_Arg1 = (LONG)f;
        break;
    }
    case ACTION_READ:
        res2 = do_read((MFile *)pkt->dp_Arg1, (uint8_t *)pkt->dp_Arg2, pkt->dp_Arg3, &res1);
        break;
    case ACTION_WRITE:
        res2 = do_write((MFile *)pkt->dp_Arg1, (const uint8_t *)pkt->dp_Arg2, pkt->dp_Arg3, &res1);
        break;
    case ACTION_SEEK:
        res2 = do_seek((MFile *)pkt->dp_Arg1, pkt->dp_Arg2, pkt->dp_Arg3, &res1);
        break;
    case ACTION_SET_FILE_SIZE:
        res2 = do_setsize((MFile *)pkt->dp_Arg1, pkt->dp_Arg2, pkt->dp_Arg3, &res1);
        break;
    case ACTION_END:
        res2 = end_file((MFile *)pkt->dp_Arg1);
        if (res2) res1 = DOSFALSE;
        break;
    case ACTION_DELETE_OBJECT:
        if ((res2 = act_delete(pkt))) res1 = DOSFALSE;
        break;
    case ACTION_RENAME_OBJECT:
        if ((res2 = act_rename(pkt))) res1 = DOSFALSE;
        break;
    case ACTION_RENAME_DISK:
        if ((res2 = act_rename_disk(pkt))) res1 = DOSFALSE;
        break;
    case ACTION_CREATE_DIR: {
        char leaf[108];
        uint8_t pn[32];
        uint32_t id;
        MLock *l;
        res1 = 0;
        if ((res2 = walk(lockptr(pkt->dp_Arg1), pkt->dp_Arg2, &o, leaf))) break;
        if (!leaf[0]) { res2 = ERROR_OBJECT_EXISTS; break; }
        {
            Obj t;
            if (!lookup_child(o.e.id, leaf, &t)) { res2 = ERROR_OBJECT_EXISTS; break; }
        }
        if ((res2 = mac_name(leaf, pn))) break;
        touch();
        if (hfs_mkdir(&vol, o.e.id, pn, &id)) { res2 = hfserr(ERROR_DISK_FULL); break; }
        if (hfs_get_id(&vol, id, &o.e)) { res2 = ERROR_OBJECT_NOT_FOUND; break; }
        o.vad = 0;
        if (!(l = mklock(&o, EXCLUSIVE_LOCK))) { res2 = ERROR_NO_FREE_STORE; break; }
        res1 = MKBADDR(l);
        break;
    }
    case ACTION_SET_DATE:
    case ACTION_SET_COMMENT:
    case ACTION_SET_PROTECT:
        if ((res2 = act_set(pkt))) res1 = DOSFALSE;
        break;
    case ACTION_INFO:
        fill_info(BADDR(pkt->dp_Arg2));
        break;
    case ACTION_DISK_INFO:
        fill_info(BADDR(pkt->dp_Arg1));
        break;
    case ACTION_CURRENT_VOLUME:
        res1 = MKBADDR(volnode);
        break;
    case ACTION_IS_FILESYSTEM:
        break;
    case ACTION_FLUSH:
        do_flush();
        break;
    case ACTION_INHIBIT:
        if (pkt->dp_Arg1) { do_flush(); inhibited++; }
        else if (inhibited) inhibited--;
        break;
    case ACTION_CHANGE_MODE: {
        LONG nm = pkt->dp_Arg3 == EXCLUSIVE_LOCK || pkt->dp_Arg3 == MODE_NEWFILE ? EXCLUSIVE_LOCK : SHARED_LOCK;
        MLock *l, *x;
        if (pkt->dp_Arg1 == CHANGE_FH) l = ((MFile *)((struct FileHandle *)BADDR(pkt->dp_Arg2))->fh_Arg1)->lock;
        else l = lockptr(pkt->dp_Arg2);
        if (!l) { res1 = DOSFALSE; res2 = ERROR_OBJECT_WRONG_TYPE; break; }
        for (x = locks; x; x = x->next)
            if (x != l && x->fl.fl_Key == l->fl.fl_Key && (nm == EXCLUSIVE_LOCK || x->fl.fl_Access == EXCLUSIVE_LOCK)) break;
        if (x) { res1 = DOSFALSE; res2 = ERROR_OBJECT_IN_USE; break; }
        l->fl.fl_Access = nm;
        break;
    }
    case ACTION_DIE:
        if (locks || files) { res1 = DOSFALSE; res2 = ERROR_OBJECT_IN_USE; break; }
        dying = 1;
        break;
    default:
        res1 = DOSFALSE;
        res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
reply:
    dbg("pkt %ld %lx %lx %lx -> %ld/%ld\n", (long)t, (long)pkt->dp_Arg1, (long)pkt->dp_Arg2,
        (long)pkt->dp_Arg3, (long)res1, (long)res2);
    if (dying) return 1;
    ReplyPkt(pkt, res1, res2);
    if (activity && !vol.dirty) activity = 0;   /* nothing was really written */
    if (activity) arm_timer();
    return 0;
}

/* ------------------------------------------------------------------ startup */

/* Startup string: <image> [RO] [NODOUBLE] [PART=n] [LOG=file].  It arrives as a BSTR
   (MacMount, Mount with Startup = "...") and may still carry the quotes. */
static void parse_startup(BPTR arg)
{
    char s[400], tok[260];
    const char *p = s;
    int first = 1;
    UBYTE *b = BADDR(arg);
    int n;
    s[0] = 0;
    if (!b) return;
    n = b[0];
    if (n > 0 && n < (int)sizeof(s) && b[1] >= 0x20) { memcpy(s, b + 1, n); s[n] = 0; }
    else { strncpy(s, (char *)b, sizeof(s) - 1); s[sizeof(s) - 1] = 0; }   /* plain C string */
    /* Mount keeps the quotes of Startup = "image RO": one quoted string with no quote
       inside is the whole startup line (a path with spaces: Startup = "'a b' RO") */
    n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"' && !memchr(s + 1, '"', n - 2)) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = 0;
    }
    for (;;) {
        int k = 0;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {     /* "a b" or 'a b' (Mount cannot nest ") */
            char q = *p++;
            while (*p && *p != q && k < (int)sizeof(tok) - 1) tok[k++] = *p++;
            if (*p == q) p++;
        } else while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && k < (int)sizeof(tok) - 1) {
            if (*p == '"') { p++; continue; }
            tok[k++] = *p++;
        }
        tok[k] = 0;
        if (first) { strcpy(imgpath, tok); first = 0; continue; }
        if (!lat_strcasecmp(tok, "RO") || !lat_strcasecmp(tok, "READONLY")) readonly = 1;
        else if (!lat_strcasecmp(tok, "NODOUBLE")) nodouble = 1;
        else if (!strncmp(tok, "PART=", 5) || !strncmp(tok, "part=", 5)) part = atoi(tok + 5);
        else if (!strncmp(tok, "LOG=", 4) || !strncmp(tok, "log=", 4)) strcpy(logname, tok + 4);
    }
}

int atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
    return neg ? -v : v;
}

static int make_volume(void)
{
    struct DateStamp ds;
    amiga_name(volname, vol.name, 0);
    volnode = (struct DeviceList *)MakeDosEntry((STRPTR)volname, DLT_VOLUME);
    if (!volnode) return -1;
    volnode->dl_Task = port;
    volnode->dl_DiskType = ID_DOS_DISK;
    mac2date(be32(vol.mdb + 2), &ds);
    volnode->dl_VolumeDate = ds;
    volnode->dl_LockList = 0;
    LockDosList(LDF_VOLUMES | LDF_WRITE);
    AddDosEntry((struct DosList *)volnode);
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    return 0;
}

static void cleanup(void)
{
    if (treq) {
        if (idle_pending) { AbortIO(&treq->tr_node); WaitIO(&treq->tr_node); }
        CloseDevice(&treq->tr_node);
        DeleteIORequest((struct IORequest *)treq);
        treq = NULL;
    }
    if (tport) { DeleteMsgPort(tport); tport = NULL; }
    if (vol.f) hfs_close(&vol);
    if (logfh) { dbg("handler stopped\n"); Close(logfh); logfh = 0; }
}

void handler_main(void)
{
    struct Message *msg;
    struct DosPacket *pkt;
    LONG err = 0;

    SysBase = *(struct ExecBase **)4;
    me = (struct Process *)FindTask(NULL);
    port = &me->pr_MsgPort;
    WaitPort(port);
    msg = GetMsg(port);
    pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
    if (!UtilityBase && DOSBase) { CloseLibrary((struct Library *)DOSBase); DOSBase = NULL; }
    if (!DOSBase) {
        /* OS 2.04 or later needed; reply by hand */
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_INVALID_RESIDENT_LIBRARY;
        msg = pkt->dp_Link;
        msg->mn_Node.ln_Name = (char *)pkt;
        PutMsg(pkt->dp_Port, msg);
        return;
    }
    devnode = BADDR(pkt->dp_Arg3);
    me->pr_WindowPtr = (APTR)-1;    /* no "insert volume" requesters from the image's device */
    parse_startup(pkt->dp_Arg2);
    if (logname[0]) {
        if (!lat_strcasecmp(logname, "SERIAL")) logser = 1;
        else logfh = Open((STRPTR)logname, MODE_NEWFILE);
        dbg("MacHFS-Handler %s: image \"%s\"%s%s part %d\n", MB_VERSION, imgpath,
            readonly ? " RO" : "", nodouble ? " NODOUBLE" : "", part);
    }
    if (!imgpath[0]) err = ERROR_REQUIRED_ARG_MISSING;
    else if (hfs_open(&vol, imgpath, readonly ? HFS_EXCL : 1, part)) {
        LONG ioe = IoErr();
        dbg("open failed: %s\n", vol.err);
        err = !strncmp(vol.err, "cannot open", 11) ? (ioe ? ioe : ERROR_OBJECT_NOT_FOUND) :
              strstr(vol.err, "mounted") ? ERROR_DISK_NOT_VALIDATED : ERROR_NOT_A_DOS_DISK;
    }
    if (!err) {
        tport = CreateMsgPort();
        if (tport) treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
        if (!treq || OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, &treq->tr_node, 0)) {
            if (treq) DeleteIORequest((struct IORequest *)treq);
            treq = NULL;
            err = ERROR_NO_FREE_STORE;
        } else tmask = 1UL << tport->mp_SigBit;
    }
    dbg("image open: %s\n", err ? vol.err : "ok");
    if (!err && make_volume()) err = ERROR_NO_FREE_STORE;
    dbg("volume node: %s\n", volname);
    if (err) {
        cleanup();
        ReplyPkt(pkt, DOSFALSE, err);
        CloseLibrary(UtilityBase);
        CloseLibrary((struct Library *)DOSBase);
        return;
    }
    devnode->dn_Task = port;
    dbg("mounted \"%s\" (%s), %lu blocks of %lu, %lu free\n", volname, readonly ? "read-only" : "read-write",
        (unsigned long)vol.nblocks, (unsigned long)vol.alsize, (unsigned long)hfs_free_blocks(&vol));
    ReplyPkt(pkt, DOSTRUE, 0);
    diskchange(1);

    for (;;) {
        ULONG sig = Wait((1UL << port->mp_SigBit) | tmask);
        if ((sig & tmask) && idle_pending && GetMsg(tport)) {
            idle_pending = 0;
            if (activity) { activity = 0; arm_timer(); }
            else do_flush();
        }
        while ((msg = GetMsg(port))) {
            pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
            if (handle(pkt)) goto die;
        }
    }
die:
    do_flush();
    LockDosList(LDF_VOLUMES | LDF_DEVICES | LDF_WRITE);
    RemDosEntry((struct DosList *)volnode);
    devnode->dn_Task = NULL;
    UnLockDosList(LDF_VOLUMES | LDF_DEVICES | LDF_WRITE);
    FreeDosEntry((struct DosList *)volnode);
    cleanup();
    diskchange(0);
    Forbid();
    ReplyPkt(pkt, DOSTRUE, 0);
    /* anything that arrived meanwhile */
    while ((msg = GetMsg(port))) {
        struct DosPacket *p = (struct DosPacket *)msg->mn_Node.ln_Name;
        ReplyPkt(p, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED);
    }
    CloseLibrary(UtilityBase);
    CloseLibrary((struct Library *)DOSBase);
    /* returning ends the process; Forbid() keeps the code alive until then */
}
