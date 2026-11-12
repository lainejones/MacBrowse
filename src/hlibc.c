/* hlibc.c - the few C library functions MacHFS-Handler needs, without libnix's startup
 * code: memory from exec AllocVec, and a small vsnprintf for error messages and the log.
 * The handler is linked with -nostartfiles, so nothing here may depend on libnix
 * constructors (stdio, auto-opened libraries).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

extern struct ExecBase *SysBase;

/* 8-byte header keeps the size (for realloc) and the alignment of AllocVec. */
void *malloc(size_t n)
{
    ULONG *p = AllocVec(n + 8, MEMF_ANY);
    if (!p) return NULL;
    p[0] = n;
    return p + 2;
}

void *calloc(size_t n, size_t m)
{
    ULONG *p = AllocVec(n * m + 8, MEMF_ANY | MEMF_CLEAR);
    if (!p) return NULL;
    p[0] = n * m;
    return p + 2;
}

void free(void *q)
{
    if (q) FreeVec((ULONG *)q - 2);
}

void *realloc(void *q, size_t n)
{
    void *r;
    size_t old;
    if (!q) return malloc(n);
    old = ((ULONG *)q)[-2];
    if (n <= old) return q;
    r = malloc(n);
    if (!r) return NULL;
    memcpy(r, q, old);
    free(q);
    return r;
}

/* vsnprintf: %d %u %x %s %c %%, the l modifier, width, '-', '0' and precision
   (".*" or ".n" for strings). */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t o = 0;
#define PUT(c) do { if (o + 1 < size) buf[o] = (c); o++; } while (0)
    for (; *fmt; fmt++) {
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        char tmp[16];
        const char *s;
        int len, i;
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + *fmt++ - '0';
        }
        while (*fmt == 'l' || *fmt == 'h') { if (*fmt == 'l') lng = 1; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            unsigned long u;
            int neg = 0, base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
            if (*fmt == 'd' || *fmt == 'i') {
                long d = lng ? va_arg(ap, long) : va_arg(ap, int);
                if (d < 0) { neg = 1; u = (unsigned long)-d; } else u = (unsigned long)d;
            } else u = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            len = 0;
            do { tmp[len++] = "0123456789abcdef"[u % base]; u /= base; } while (u && len < 15);
            if (neg) tmp[len++] = '-';
            i = len;
            for (; !left && i < width; i++) PUT(zero ? '0' : ' ');
            i = len;
            while (len) PUT(tmp[--len]);
            for (; left && i < width; i++) PUT(' ');
            break;
        }
        case 'c':
            PUT((char)va_arg(ap, int));
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            for (len = 0; s[len] && (prec < 0 || len < prec); len++) ;
            for (i = len; !left && i < width; i++) PUT(' ');
            for (i = 0; i < len; i++) PUT(s[i]);
            for (i = len; left && i < width; i++) PUT(' ');
            break;
        case '%':
            PUT('%');
            break;
        default:
            PUT('%');
            if (*fmt) PUT(*fmt); else fmt--;
            break;
        }
    }
    if (size) buf[o < size ? o : size - 1] = 0;
    return (int)o;
#undef PUT
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
