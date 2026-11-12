/* hfsio.c - file I/O layer.  AmigaDOS (dos.library) build and a POSIX host build. */
#include "hfsio.h"
#include <string.h>
#include <stdlib.h>

/* Seconds between 1904-01-01 and 1978-01-01 (Amiga epoch): 27029 days. */
#define MAC_AMIGA_DELTA 2335305600UL
/* Seconds between 1904-01-01 and 1970-01-01 (Unix epoch). */
#define MAC_UNIX_DELTA  2082844800UL

void *xrealloc(void *p, unsigned long n) { return p ? realloc(p, n) : malloc(n); }

#if defined(AMIGA) || defined(__amigaos__)
/* ------------------------------------------------------------------ AmigaDOS */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct IOF { BPTR fh; long pos; };

static char errbuf[80];

IOF *io_open(const char *name, int mode)
{
    IOF *f = malloc(sizeof(IOF));
    if (!f) return NULL;
    f->pos = 0;
    if (mode == IO_UPDATE || mode == IO_READX) {
        /* Take an exclusive lock first: this fails if anyone else (e.g. ShapeShifter)
           has the file open, and keeps them out while we write. */
        BPTR lk = Lock((STRPTR)name, EXCLUSIVE_LOCK);
        if (!lk) { free(f); return NULL; }
        f->fh = OpenFromLock(lk);
        if (!f->fh) {
            /* No OpenFromLock (e.g. the vamos test runner): the exclusive lock has proved
               nobody else has the file open, so open it normally (MODE_OLDFILE is read/write). */
            UnLock(lk);
            f->fh = Open((STRPTR)name, MODE_OLDFILE);
        }
    } else if (mode == IO_CREATE) {
        f->fh = Open((STRPTR)name, MODE_NEWFILE);
    } else {
        f->fh = Open((STRPTR)name, MODE_OLDFILE);
    }
    if (!f->fh) { free(f); return NULL; }
    return f;
}

void io_close(IOF *f) { if (f) { Close(f->fh); free(f); } }

static int seekto(IOF *f, uint32_t off)
{
    if ((long)off == f->pos) return 0;
    if (Seek(f->fh, (LONG)off, OFFSET_BEGINNING) < 0) return -1;
    f->pos = (long)off;
    return 0;
}

int io_pread(IOF *f, uint32_t off, void *buf, uint32_t len)
{
    LONG r;
    if (seekto(f, off)) return -1;
    r = Read(f->fh, buf, (LONG)len);
    if (r < 0) { f->pos = -1; return -1; }
    f->pos += r;
    return (uint32_t)r == len ? 0 : -1;
}

int io_pwrite(IOF *f, uint32_t off, const void *buf, uint32_t len)
{
    LONG r;
    if (seekto(f, off)) return -1;
    r = Write(f->fh, (APTR)buf, (LONG)len);
    if (r < 0) { f->pos = -1; return -1; }
    f->pos += r;
    return (uint32_t)r == len ? 0 : -1;
}

int io_write(IOF *f, const void *buf, uint32_t len)
{
    LONG r = Write(f->fh, (APTR)buf, (LONG)len);
    if (r > 0) f->pos += r;
    return (uint32_t)r == len ? 0 : -1;
}

long io_size(IOF *f)
{
    long cur = Seek(f->fh, 0, OFFSET_END), sz;
    if (cur < 0) return -1;
    sz = Seek(f->fh, cur, OFFSET_BEGINNING);
    f->pos = cur;
    return sz;
}

int io_exists(const char *name)
{
    BPTR lk = Lock((STRPTR)name, SHARED_LOCK);
    if (!lk) return 0;
    UnLock(lk);
    return 1;
}

int io_isdir(const char *name)
{
    struct FileInfoBlock *fib;
    int r = 0;
    BPTR lk = Lock((STRPTR)name, SHARED_LOCK);
    if (!lk) return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lk, fib)) r = fib->fib_DirEntryType > 0;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lk);
    return r;
}

int io_mkdir(const char *name)
{
    BPTR lk;
    if (io_isdir(name)) return 0;
    lk = CreateDir((STRPTR)name);
    if (!lk) return -1;
    UnLock(lk);
    return 0;
}

const char *io_errstr(void)
{
    LONG e = IoErr();
    if (!Fault(e, NULL, errbuf, sizeof(errbuf))) {
        strcpy(errbuf, "error ");
    }
    return errbuf;
}

int io_scandir(const char *dir, io_dircb cb, void *ud)
{
    struct FileInfoBlock *fib;
    BPTR lk = Lock((STRPTR)dir, SHARED_LOCK);
    int r = 0;
    if (!lk) return -1;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lk); return -1; }
    if (Examine(lk, fib) && fib->fib_DirEntryType > 0) {
        while (ExNext(lk, fib)) {
            r = cb(fib->fib_FileName, fib->fib_DirEntryType > 0, ud);
            if (r) break;
        }
    } else r = -1;
    FreeDosObject(DOS_FIB, fib);
    UnLock(lk);
    return r;
}

uint32_t io_mactime(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return MAC_AMIGA_DELTA + (uint32_t)ds.ds_Days * 86400UL + ds.ds_Minute * 60UL + ds.ds_Tick / 50;
}

uint32_t io_filemactime(const char *name)
{
    struct FileInfoBlock *fib;
    uint32_t t = 0;
    BPTR lk = Lock((STRPTR)name, SHARED_LOCK);
    if (!lk) return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lk, fib))
            t = MAC_AMIGA_DELTA + (uint32_t)fib->fib_Date.ds_Days * 86400UL +
                fib->fib_Date.ds_Minute * 60UL + fib->fib_Date.ds_Tick / 50;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lk);
    return t;
}

void io_setfiletime(const char *name, uint32_t mt)
{
    struct DateStamp ds;
    if (mt < MAC_AMIGA_DELTA) return;
    mt -= MAC_AMIGA_DELTA;
    ds.ds_Days = mt / 86400UL;
    mt %= 86400UL;
    ds.ds_Minute = mt / 60;
    ds.ds_Tick = (mt % 60) * 50;
    SetFileDate((STRPTR)name, &ds);
}

void io_setcomment(const char *name, const char *c) { SetComment((STRPTR)name, (STRPTR)c); }

#else
/* ------------------------------------------------------------------ POSIX host */
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include <fcntl.h>
#include <unistd.h>

struct IOF { int fd; };

IOF *io_open(const char *name, int mode)
{
    IOF *f = malloc(sizeof(IOF));
    int fl = mode == IO_UPDATE ? O_RDWR : mode == IO_READX ? O_RDONLY : mode == IO_CREATE ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
    if (!f) return NULL;
    f->fd = open(name, fl, 0644);
    if (f->fd < 0) { free(f); return NULL; }
    return f;
}
void io_close(IOF *f) { if (f) { close(f->fd); free(f); } }
int io_pread(IOF *f, uint32_t off, void *buf, uint32_t len)
{ return pread(f->fd, buf, len, off) == (ssize_t)len ? 0 : -1; }
int io_pwrite(IOF *f, uint32_t off, const void *buf, uint32_t len)
{ return pwrite(f->fd, buf, len, off) == (ssize_t)len ? 0 : -1; }
int io_write(IOF *f, const void *buf, uint32_t len)
{ return write(f->fd, buf, len) == (ssize_t)len ? 0 : -1; }
long io_size(IOF *f) { struct stat st; return fstat(f->fd, &st) ? -1 : (long)st.st_size; }
int io_exists(const char *n) { struct stat st; return stat(n, &st) == 0; }
int io_isdir(const char *n) { struct stat st; return stat(n, &st) == 0 && S_ISDIR(st.st_mode); }
int io_mkdir(const char *n) { if (io_isdir(n)) return 0; return mkdir(n, 0755); }
const char *io_errstr(void) { return strerror(errno); }
int io_scandir(const char *dir, io_dircb cb, void *ud)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[1024];
    int r = 0;
    if (!d) return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if ((r = cb(e->d_name, io_isdir(p), ud))) break;
    }
    closedir(d);
    return r;
}
static long tzoff(time_t t) { struct tm lt = *localtime(&t), gt = *gmtime(&t); return (long)(mktime(&lt) - mktime(&gt)); }
uint32_t io_mactime(void) { time_t t = time(NULL); return (uint32_t)(t + tzoff(t)) + MAC_UNIX_DELTA; }
uint32_t io_filemactime(const char *n)
{ struct stat st; if (stat(n, &st)) return 0; return (uint32_t)(st.st_mtime + tzoff(st.st_mtime)) + MAC_UNIX_DELTA; }
void io_setfiletime(const char *n, uint32_t mt)
{
    struct utimbuf ub; time_t t;
    if (mt < MAC_UNIX_DELTA) return;
    t = (time_t)(mt - MAC_UNIX_DELTA);
    t -= tzoff(t);
    ub.actime = ub.modtime = t;
    utime(n, &ub);
}
void io_setcomment(const char *n, const char *c) { (void)n; (void)c; }
#endif
