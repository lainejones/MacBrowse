/* mount.h - mount an HFS image with MacHFS-Handler (Amiga only). */
#ifndef MOUNT_H
#define MOUNT_H

#define HFV_HANDLER "L:MacHFS-Handler"

typedef struct {
    const char *device, *image, *handler, *log;
    int ro, nodouble, part, stack, quiet;
} MountOpts;

int hfv_mount(const MountOpts *m);
int hfv_unmount(const char *device, int quiet);

#endif
