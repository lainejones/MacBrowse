/* mount.c - mount / unmount an HFS image as an AmigaDOS device with MacHFS-Handler.
 * Shared by MacMount and "MacBrowse <image> MOUNT <device>".  Amiga only.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "hfs.h"
#include "charset.h"
#include "mount.h"

/* BSTR in public memory: it must outlive the program that mounts. */
static BSTR mkbstr(const char *s)
{
    int n = strlen(s);
    UBYTE *b;
    if (n > 255) return 0;
    b = AllocVec(n + 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!b) return 0;
    b[0] = (UBYTE)n;
    memcpy(b + 1, s, n);
    return MKBADDR(b);
}

static int ncase_eq(const char *a, const char *b, int n)
{
    while (n--) {
        int x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 0x41 && x <= 0x5A) x += 32;
        if (y >= 0x41 && y <= 0x5A) y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static void devname(char *out, const char *dev)
{
    int n;
    strncpy(out, dev, 31);
    out[31] = 0;
    n = strlen(out);
    if (n && out[n - 1] == ':') out[n - 1] = 0;
}

static struct DosList *find_dev(const char *name, ULONG flags)
{
    struct DosList *dl = LockDosList(LDF_DEVICES | flags);
    dl = FindDosEntry(dl, (STRPTR)name, LDF_DEVICES);
    if (!dl) UnLockDosList(LDF_DEVICES | flags);
    return dl;          /* still locked when found */
}

int hfv_mount(const MountOpts *m)
{
    static char name[32], abspath[256], startup[400], full[40];
    static HVol v;
    struct DosList *dl;
    struct MsgPort *mp;
    BPTR lk;
    BSTR hb, sb;
    char vname[40];
    int n;

    devname(name, m->device);
    if (!name[0] || strchr(name, ':') || strchr(name, '/')) { printf("Bad device name %s\n", m->device); return 10; }
    if ((dl = find_dev(name, LDF_READ))) {
        UnLockDosList(LDF_DEVICES | LDF_READ);
        printf("%s: is already mounted\n", name);
        return 10;
    }
    if (!(lk = Lock((STRPTR)(m->handler ? m->handler : HFV_HANDLER), SHARED_LOCK))) {
        printf("Cannot find %s\n", m->handler ? m->handler : HFV_HANDLER);
        return 20;
    }
    UnLock(lk);
    /* the handler has no current directory: give it the full path */
    if (!(lk = Lock((STRPTR)m->image, SHARED_LOCK))) {
        LONG e = IoErr();
        if (e == ERROR_OBJECT_IN_USE) printf("%s: in use (mounted already, or open in ShapeShifter)\n", m->image);
        else PrintFault(e, (STRPTR)m->image);
        return 20;
    }
    n = NameFromLock(lk, (STRPTR)abspath, sizeof(abspath));
    UnLock(lk);
    if (!n) { printf("%s: path too long\n", m->image); return 20; }
    if (strchr(abspath, '"')) { printf("%s: the path cannot contain quotes\n", abspath); return 20; }
    /* Not every file system enforces the handler's exclusive lock (host-folder
       drives in emulators may not): refuse an image another MacHFS device has. */
    {
        struct DosList *d = LockDosList(LDF_DEVICES | LDF_READ);
        char other[40];
        other[0] = 0;
        while ((d = NextDosEntry(d, LDF_DEVICES))) {
            UBYTE *st = BADDR(d->dol_misc.dol_handler.dol_Startup);
            int al = strlen(abspath);
            if (!d->dol_Task || !st || (ULONG)st < 0x400 || st[0] < al + 2) continue;
            if (st[1] == '"' && ncase_eq((char *)st + 2, abspath, al) && st[2 + al] == '"') {
                UBYTE *nm = BADDR(d->dol_Name);
                int k = nm[0] < 38 ? nm[0] : 38;
                memcpy(other, nm + 1, k);
                other[k] = 0;
                break;
            }
        }
        UnLockDosList(LDF_DEVICES | LDF_READ);
        if (other[0]) { printf("%s: already mounted as %s:\n", abspath, other); return 20; }
    }
    /* check it now, for a clear message (the handler checks again) */
    if (hfs_open(&v, abspath, m->ro ? HFS_EXCL : 1, m->part)) {
        char *nl;
        while ((nl = strchr(v.err, '\n'))) *nl = ' ';
        printf("%s: %s\n", m->image, v.err);
        if (strstr(v.err, "mounted")) printf("Quit ShapeShifter first, or mount it read-only (RO).\n");
        return 20;
    }
    amiga_name(vname, v.name, 0);
    hfs_close(&v);

    /* always an option after the quoted path: the handler strips the quotes of a
       startup line that is one quoted string (what Mount passes) */
    n = snprintf(startup, sizeof(startup), "\"%s\" %s%s", abspath, m->ro ? "RO" : "RW", m->nodouble ? " NODOUBLE" : "");
    if (m->part >= 0) n += snprintf(startup + n, sizeof(startup) - n, " PART=%d", m->part);
    if (m->log) n += snprintf(startup + n, sizeof(startup) - n, " LOG=%s", m->log);
    if (n > 255) { printf("Image path too long\n"); return 20; }
    hb = mkbstr(m->handler ? m->handler : HFV_HANDLER);
    sb = mkbstr(startup);
    dl = MakeDosEntry((STRPTR)name, DLT_DEVICE);
    if (!hb || !sb || !dl) { printf("Out of memory\n"); return 20; }
    dl->dol_misc.dol_handler.dol_Handler = hb;
    dl->dol_misc.dol_handler.dol_StackSize = m->stack > 4096 ? m->stack : 16384;
    dl->dol_misc.dol_handler.dol_Priority = 5;
    dl->dol_misc.dol_handler.dol_Startup = sb;
    dl->dol_misc.dol_handler.dol_SegList = 0;
    dl->dol_misc.dol_handler.dol_GlobVec = -1;
    LockDosList(LDF_DEVICES | LDF_WRITE);
    n = AddDosEntry(dl);
    UnLockDosList(LDF_DEVICES | LDF_WRITE);
    if (!n) { printf("%s: cannot add the device\n", name); FreeDosEntry(dl); return 20; }

    /* start the handler now, so errors show here */
    snprintf(full, sizeof(full), "%s:", name);
    mp = DeviceProc((STRPTR)full);
    if (!mp) {
        char msg[80];
        LONG e = IoErr();
        Fault(e, (STRPTR)"", (STRPTR)msg, sizeof(msg));
        printf("%s: the handler did not start%s\n", name, msg);
        if ((dl = find_dev(name, LDF_WRITE))) {
            RemDosEntry(dl);
            UnLockDosList(LDF_DEVICES | LDF_WRITE);
        }
        return 20;
    }
    if (!m->quiet) printf("%s mounted: %s as volume \"%s\"%s\n", full, abspath, vname, m->ro ? " (read-only)" : "");
    return 0;
}

int hfv_unmount(const char *device, int quiet)
{
    static char name[32];
    struct DosList *dl;
    struct MsgPort *mp;
    BPTR seg;

    devname(name, device);
    if (!(dl = find_dev(name, LDF_READ))) { printf("%s: is not mounted\n", name); return 10; }
    mp = dl->dol_Task;
    UnLockDosList(LDF_DEVICES | LDF_READ);
    if (mp) {
        if (!DoPkt(mp, ACTION_DIE, 0, 0, 0, 0, 0)) {
            LONG e = IoErr();
            if (e == ERROR_OBJECT_IN_USE) printf("%s: files or drawers on it are still in use\n", name);
            else PrintFault(e, (STRPTR)name);
            return 10;
        }
    }
    if ((dl = find_dev(name, LDF_WRITE))) {
        if (dl->dol_Task) {     /* started again meanwhile */
            UnLockDosList(LDF_DEVICES | LDF_WRITE);
            printf("%s: is busy again\n", name);
            return 10;
        }
        seg = dl->dol_misc.dol_handler.dol_SegList;
        RemDosEntry(dl);
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
        /* the handler replied to ACTION_DIE inside Forbid() and then ended: its code is free */
        if (seg) UnLoadSeg(seg);
    }
    if (!quiet) printf("%s: unmounted\n", name);
    return 0;
}
