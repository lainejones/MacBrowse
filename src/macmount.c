/* MacMount - mount a Mac HFS disk image as an AmigaDOS volume with L:MacHFS-Handler.
 *
 *   MacMount HFV0: MacDisks:Transfer.hfv          read-write
 *   MacMount HFV1: MacDisks:MacOS80.hfv RO        read-only
 *   MacMount HFV0: UNMOUNT
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include "mount.h"
#include "version.h"

const char vers[] = "$VER: MacMount " MB_VERSION " (" MB_DATE ")";

#define TEMPLATE "DEVICE/A,IMAGE,RO=READONLY/S,NODOUBLE/S,PART/K/N,HANDLER/K,LOG/K,STACK/K/N,UNMOUNT=DISMOUNT/S,QUIET/S"
enum { A_DEV, A_IMAGE, A_RO, A_NODOUBLE, A_PART, A_HANDLER, A_LOG, A_STACK, A_UNMOUNT, A_QUIET, A_COUNT };

int main(int argc, char **argv)
{
    LONG a[A_COUNT] = {0};
    struct RDArgs *rd;
    MountOpts m;
    int rc;
    (void)argv;
    if (argc == 0) return 20;
    rd = ReadArgs((STRPTR)TEMPLATE, a, NULL);
    if (!rd) {
        PrintFault(IoErr(), (STRPTR)"MacMount");
        printf("Usage: MacMount <device:> <image> [RO] [NODOUBLE] [PART n]   or   MacMount <device:> UNMOUNT\n");
        return 20;
    }
    if (a[A_UNMOUNT]) rc = hfv_unmount((char *)a[A_DEV], a[A_QUIET]);
    else if (!a[A_IMAGE]) {
        printf("MacMount: which image?\nUsage: MacMount <device:> <image> [RO] [NODOUBLE] [PART n]   or   MacMount <device:> UNMOUNT\n");
        rc = 10;
    } else {
        m.device = (char *)a[A_DEV];
        m.image = (char *)a[A_IMAGE];
        m.handler = (char *)a[A_HANDLER];
        m.log = (char *)a[A_LOG];
        m.ro = a[A_RO] != 0;
        m.nodouble = a[A_NODOUBLE] != 0;
        m.part = a[A_PART] ? (int)*(LONG *)a[A_PART] : -1;
        m.stack = a[A_STACK] ? (int)*(LONG *)a[A_STACK] : 0;
        m.quiet = a[A_QUIET] != 0;
        rc = hfv_mount(&m);
    }
    FreeArgs(rd);
    return rc;
}
