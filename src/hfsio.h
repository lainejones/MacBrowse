/* hfsio.h - tiny file I/O layer: AmigaDOS on the Amiga, stdio on the host (for tests). */
#ifndef HFSIO_H
#define HFSIO_H

#include <stdint.h>

typedef struct IOF IOF;

#define IO_READ   0
#define IO_UPDATE 1     /* read/write an existing file, exclusively */
#define IO_CREATE 2     /* create/truncate for writing */
#define IO_READX  3     /* read-only, but hold an exclusive lock */

IOF  *io_open(const char *name, int mode);
void  io_close(IOF *f);
/* Positioned read/write; return 0 on success, -1 on error/short transfer. */
int   io_pread(IOF *f, uint32_t off, void *buf, uint32_t len);
int   io_pwrite(IOF *f, uint32_t off, const void *buf, uint32_t len);
/* Sequential write at the current position. */
int   io_write(IOF *f, const void *buf, uint32_t len);
long  io_size(IOF *f);                 /* file size or -1 */
int   io_exists(const char *name);     /* 1 if a file/dir of that name exists */
int   io_isdir(const char *name);      /* 1 if it is a directory */
int   io_mkdir(const char *name);      /* 0 ok (also if it already exists as a dir) */
const char *io_errstr(void);           /* text for the last OS error */

/* Directory scan: calls cb for each entry name (not recursive). */
typedef int (*io_dircb)(const char *name, int isdir, void *ud);
int   io_scandir(const char *dir, io_dircb cb, void *ud);

/* Current local time as seconds since 1904-01-01 (Mac epoch). */
uint32_t io_mactime(void);
/* File modification time as Mac time (0 if unknown). */
uint32_t io_filemactime(const char *name);
/* Set a file's modification time from Mac time (best effort). */
void  io_setfiletime(const char *name, uint32_t mactime);
void  io_setcomment(const char *name, const char *comment);

/* realloc() that accepts NULL (libnix's does not). */
void *xrealloc(void *p, unsigned long n);

#endif
