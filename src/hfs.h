/* hfs.h - minimal HFS (Mac OS Standard) volume access: read and simple write. */
#ifndef HFS_H
#define HFS_H

#include <stdint.h>
#include "hfsio.h"

#define HFS_NODESIZE   512
#define HFS_MAXDEPTH   10
#define HFS_ROOT_PAR   1        /* parent ID of the root folder */
#define HFS_ROOT_ID    2        /* CNID of the root folder */
#define HFS_EXT_ID     3
#define HFS_CAT_ID     4

#define CDR_DIR     1
#define CDR_FILE    2
#define CDR_DTHREAD 3
#define CDR_FTHREAD 4

#define FORK_DATA 0x00
#define FORK_RSRC 0xFF

typedef struct { uint16_t start, count; } HExt;

typedef struct {
    HExt    *e;
    int      n, cap;
    uint32_t blocks;        /* total allocation blocks covered */
} HExtList;

struct HVol;

typedef struct {
    struct HVol *v;
    uint32_t cnid;
    HExtList ext;
    uint32_t size;          /* logical fork length */
    int      (*cmp)(const uint8_t *k1, const uint8_t *k2);
    uint8_t  hdr[HFS_NODESIZE];   /* node 0 */
    uint16_t depth, maxkey;
    uint32_t root, leafrecs, first, last, total, freen;
    int      fixedidx;      /* index keys padded to maxkey (catalog) */
} HTree;

typedef struct HVol {
    IOF     *f;
    uint32_t base;          /* byte offset of the HFS volume in the image */
    uint32_t secs;          /* volume size in 512-byte sectors */
    int      rw, dirty;
    uint8_t  mdb[512];
    uint32_t alsize;        /* allocation block size */
    uint16_t alst, nblocks, vbmst;
    uint8_t  name[32];      /* Pascal string, Mac Roman */
    HTree    ext, cat;
    uint8_t *bitmap;        /* loaded when writable */
    int      bmdirty;
    char     err[160];
} HVol;

/* A catalog entry (directory or file). */
typedef struct {
    uint8_t  type;          /* CDR_DIR / CDR_FILE */
    uint32_t parid, id;
    uint8_t  name[32];      /* Pascal string, Mac Roman */
    uint32_t crdat, mddat, bkdat;
    uint16_t valence;       /* dirs */
    uint8_t  finfo[16], fxinfo[16];   /* FInfo/FXInfo (files) or DInfo/DXInfo (dirs) */
    uint32_t dlen, rlen, dpyl, rpyl;  /* files */
    HExt     dext[3], rext[3];
    uint8_t  flags;
    uint8_t  raw[102];
} HEnt;

/* Partition description for vol_scan_parts. */
typedef struct { uint32_t start, size; char name[33], type[33]; } HPart;

#define HFS_EXCL 4      /* hfs_open rw flag: hold an exclusive lock even when read-only */
int  hfs_open(HVol *v, const char *path, int rw, int part);  /* rw 0/1/2(=force); part -1 = auto */
int  hfs_flush(HVol *v);                                     /* bitmap + MDB, marks clean */
int  hfs_close(HVol *v);                                     /* flushes when rw */
int  hfs_list_parts(const char *path, HPart *parts, int max); /* Apple partition map */

/* Catalog */
int  hfs_lookup(HVol *v, uint32_t parid, const uint8_t *pname, HEnt *e);   /* 0 = found */
int  hfs_get_id(HVol *v, uint32_t id, HEnt *e);                           /* via thread record */
int  hfs_resolve(HVol *v, const char *path, HEnt *e);   /* Mac path "a:b:c" (raw Mac Roman) */
typedef int (*hfs_dircb)(HVol *v, const HEnt *e, void *ud);
int  hfs_readdir(HVol *v, uint32_t dirid, hfs_dircb cb, void *ud);
int  hfs_walk_all(HVol *v, hfs_dircb cb, void *ud);   /* every catalog file/dir leaf record, in order */

/* Forks */
int  hfs_fork_extents(HVol *v, uint32_t cnid, int fork, const HExt first[3], uint32_t len, HExtList *out);
int  hfs_fork_read(HVol *v, const HExtList *x, uint32_t off, void *buf, uint32_t len);
void hfs_extlist_free(HExtList *x);

/* Write support */
typedef int (*hfs_srcfn)(void *ud, uint32_t off, void *buf, uint32_t len);
typedef struct {
    uint32_t len;
    hfs_srcfn read; void *ud;   /* read(ud, off, buf, len) supplies fork bytes */
} HForkSrc;
int  hfs_create_file(HVol *v, uint32_t parid, const uint8_t *pname, const uint8_t finfo[16],
                     const uint8_t fxinfo[16], uint32_t crdat, uint32_t mddat,
                     const HForkSrc *data, const HForkSrc *rsrc, uint32_t *newid);
int  hfs_mkdir(HVol *v, uint32_t parid, const uint8_t *pname, uint32_t *newid);
int  hfs_delete(HVol *v, uint32_t parid, const uint8_t *pname);
int  hfs_create(HVol *v, uint32_t parid, const uint8_t *pname, const uint8_t finfo[16],
                const uint8_t fxinfo[16], uint32_t crdat, uint32_t mddat, uint32_t *newid);  /* empty file */
int  hfs_rename(HVol *v, uint32_t oldpar, const uint8_t *oldname, uint32_t newpar, const uint8_t *newname);
int  hfs_set_info(HVol *v, uint32_t parid, const uint8_t *pname, const uint8_t *finfo,
                  const uint8_t *fxinfo, uint32_t crdat, uint32_t mddat);
int  hfs_dir_next(HVol *v, uint32_t dirid, const uint8_t *after, HEnt *e);  /* 0 found, 1 end */
uint32_t hfs_free_blocks(HVol *v);

/* Open forks: streaming read/write.  Writes grow the fork a clump at a time and add
   extents-overflow records past three extents; hfs_fork_sync stores the result in the
   catalog.  parid/name locate the catalog record: update them if the file is renamed. */
typedef struct {
    HVol    *v;
    uint32_t id, parid;
    uint8_t  name[32];
    int      fork;
    HExtList x;
    uint32_t len;
    int      dirty, extchanged, hadover, touched;
} HFork;
int  hfs_fork_open(HVol *v, const HEnt *e, int fork, HFork *f);
int  hfs_fork_pread(HFork *f, uint32_t off, void *buf, uint32_t len);
int  hfs_fork_pwrite(HFork *f, uint32_t off, const void *buf, uint32_t len);
int  hfs_fork_setlen(HFork *f, uint32_t len);
int  hfs_fork_sync(HFork *f, int trim);
void hfs_fork_close(HFork *f);

int  hfs_check(HVol *v, int verbose);   /* consistency check; returns number of problems */

/* Helpers */
int  hfs_relstring(const uint8_t *a, int alen, const uint8_t *b, int blen);
uint16_t be16(const uint8_t *p);
uint32_t be32(const uint8_t *p);
void wb16(uint8_t *p, uint16_t x);
void wb32(uint8_t *p, uint32_t x);

#endif
