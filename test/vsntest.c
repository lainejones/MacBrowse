/* vsntest.c - check hlibc.c's vsnprintf (built for m68k, run under vamos). */
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);

static void out(const char *s) { Write(Output(), (APTR)s, strlen(s)); Write(Output(), "\n", 1); }

int main(void)
{
    char b[200];
    snprintf(b, sizeof(b), "MacHFS-Handler %s: image \"%s\"%s%s part %d", "1.1", "Test:mac.hfv", " RO", "", -1); out(b);
    snprintf(b, sizeof(b), "%lu blocks of %lu, %u free, [%5d] [%-5d] [%05d] %x", 3526UL, 512UL, 51, 42, 42, 42, 255); out(b);
    snprintf(b, sizeof(b), "name %.*s end %c %%", 3, "abcdef", 'Z'); out(b);
    snprintf(b, 8, "truncated %s", "xxxxxxxxxx"); out(b);
    return 0;
}
