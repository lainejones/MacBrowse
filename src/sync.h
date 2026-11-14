/* sync.h - synced drawer commands (sync.c) and what they use from main.c and hfsio.c */
#ifndef SYNC_H
#define SYNC_H
#include <stdint.h>
int cmd_create(void);
int cmd_syncin(void);
int cmd_syncout(void);
int hfs_mkimage(const char *path, uint32_t sectors, const char *latname);
/* main.c */
void mb_to_pname(uint8_t *pn, const char *lat);     /* Latin-1 name -> Pascal Mac Roman */
extern uint8_t mb_last_pn[32];                      /* the Mac name the last PUT gave its file */
#endif
