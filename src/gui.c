/* gui.c - GadTools browser window for MacBrowse (Workbench or "MacBrowse <image>"). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/asl.h>
#include <proto/icon.h>
#include "hfs.h"
#include "charset.h"
#include "ops.h"
#include "version.h"

extern struct WBStartup *_WBenchMsg;
#include <graphics/gfxbase.h>

/* Opened here rather than by libnix's auto-open, so the CLI still starts where these
   libraries are missing (e.g. the vamos test runner). */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *GadToolsBase = NULL, *AslBase = NULL, *IconBase = NULL;

enum { G_IMAGE = 1, G_OPEN, G_PATH, G_LIST, G_PARENT, G_GET, G_PUT, G_MKDIR, G_DELETE, G_FORMAT, G_STATUS, G_COUNT };

typedef struct { struct Node n; HEnt e; char text[72]; } Item;

static struct Screen *scr;
static struct Window *win;
static APTR vi;
static struct Gadget *glist, *gad[G_COUNT];
static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct List items;
static Item *itemv;
static int nitems, sel = -1;
static HVol vol;
static int volopen;
static char imgpath[512], pathtext[256], status[160], wintitle[96];
static uint32_t curdir = HFS_ROOT_ID;
static char lastdrawer[256] = "RAM:", lastin[256] = "RAM:", lastimg[256] = "";
static int fmt;     /* cycle: 0 data, 1 MacBinary, 2 AppleDouble, 3 AppleSingle, 4 resource */
static const char *fmtlabels[] = { "Data fork", "MacBinary", "AppleDouble", "AppleSingle", "Resource fork", NULL };
static const int fmtmode[] = { M_DATA, M_MACBIN, M_DOUBLE, M_SINGLE, M_RSRC };

/* NewList() lives in amiga.lib, which this toolchain does not link: do it by hand. */
static void init_list(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    l->lh_Type = 0;
}

/* ------------------------------------------------------------------ helpers */

static int easy(const char *title, const char *text, const char *gads)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof(es);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)title;
    es.es_TextFormat = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)gads;
    return EasyRequest(win, &es, NULL, (ULONG)text);
}

static void req(const char *text) { easy("MacBrowse", text, "OK"); }

static void set_status(const char *s)
{
    if (s != status) {
        strncpy(status, s, sizeof(status) - 1);
        status[sizeof(status) - 1] = 0;
    }
    if (win) GT_SetGadgetAttrs(gad[G_STATUS], win, NULL, GTTX_Text, (ULONG)status, TAG_DONE);
}

static void busy(int on)
{
    if (!win || IntuitionBase->LibNode.lib_Version < 39) return;
    SetWindowPointer(win, WA_BusyPointer, (ULONG)on, TAG_DONE);
}

static void lat_name(char *dst, const uint8_t *pn)
{
    int i;
    mac2lat_str(dst, pn + 1, pn[0]);
    for (i = 0; dst[i]; i++) if ((unsigned char)dst[i] < 0x20 || dst[i] == 0x7F) dst[i] = '?';
}

static void fourcc(char *d, const uint8_t *p)
{
    int i;
    for (i = 0; i < 4; i++) { d[i] = (char)mac2lat(p[i]); if ((unsigned char)d[i] < 0x20) d[i] = '.'; }
    d[4] = 0;
}

/* ------------------------------------------------------------------ listing */

typedef struct { Item *v; int n, cap; } Coll;

static int coll_cb(HVol *v, const HEnt *e, void *ud)
{
    Coll *c = ud;
    (void)v;
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 64;
        Item *ni = xrealloc(c->v, nc * sizeof(Item));
        if (!ni) return -1;
        c->v = ni; c->cap = nc;
    }
    memset(&c->v[c->n], 0, sizeof(Item));
    c->v[c->n].e = *e;
    c->n++;
    return 0;
}

static void detach_list(void)
{
    if (win) GT_SetGadgetAttrs(gad[G_LIST], win, NULL, GTLV_Labels, ~0UL, TAG_DONE);
}

static void attach_list(void)
{
    if (win) GT_SetGadgetAttrs(gad[G_LIST], win, NULL, GTLV_Labels, (ULONG)&items,
                               GTLV_Selected, ~0UL, GTLV_Top, 0, TAG_DONE);
}

static void build_path(void)
{
    uint32_t chain[32], id = curdir;
    int n = 0, i;
    HEnt e;
    char nm[64];
    pathtext[0] = 0;
    if (!volopen) return;
    while (id != HFS_ROOT_ID && n < 32 && !hfs_get_id(&vol, id, &e)) { chain[n++] = id; id = e.parid; }
    lat_name(pathtext, vol.name);
    strcat(pathtext, ":");
    for (i = n - 1; i >= 0; i--) {
        if (hfs_get_id(&vol, chain[i], &e)) break;
        lat_name(nm, e.name);
        if (strlen(pathtext) + strlen(nm) + 2 >= sizeof(pathtext)) break;
        strcat(pathtext, nm);
        strcat(pathtext, ":");
    }
}

static void list_dir(void)
{
    Coll c = { NULL, 0, 0 };
    int i, k = 0, pass;
    char b[80];
    detach_list();
    init_list(&items);
    free(itemv);
    itemv = NULL;
    nitems = 0;
    sel = -1;
    if (volopen) {
        HEnt d;
        if (hfs_get_id(&vol, curdir, &d) || d.type != CDR_DIR) curdir = HFS_ROOT_ID;
        if (hfs_readdir(&vol, curdir, coll_cb, &c) < 0) set_status(vol.err);
    }
    /* folders first, then files; catalog (alphabetical) order inside each group */
    itemv = calloc(c.n ? c.n : 1, sizeof(Item));
    if (itemv) {
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < c.n; i++) {
                Item *it;
                char nm[64], ty[8], cr[8];
                if ((c.v[i].e.type == CDR_DIR) != (pass == 0)) continue;
                it = &itemv[k++];
                it->e = c.v[i].e;
                lat_name(nm, it->e.name);
                if (it->e.type == CDR_DIR)
                    sprintf(it->text, "%-31.31s  <folder>  %5u item%s", nm, it->e.valence, it->e.valence == 1 ? "" : "s");
                else {
                    unsigned long sz = it->e.dlen + it->e.rlen;
                    fourcc(ty, it->e.finfo); fourcc(cr, it->e.finfo + 4);
                    if (sz >= 10UL * 1024 * 1024) sprintf(b, "%5luM", sz >> 20);
                    else if (sz >= 100000UL) sprintf(b, "%5luK", sz >> 10);
                    else sprintf(b, "%6lu", sz);
                    sprintf(it->text, "%-31.31s  %s/%s  %s", nm, ty, cr, b);
                }
                it->n.ln_Name = it->text;
                AddTail(&items, &it->n);
            }
        nitems = k;
    }
    free(c.v);
    build_path();
    if (win) GT_SetGadgetAttrs(gad[G_PATH], win, NULL, GTTX_Text, (ULONG)pathtext, TAG_DONE);
    attach_list();
    if (volopen) {
        sprintf(b, "%d item%s", nitems, nitems == 1 ? "" : "s");
        set_status(b);
    }
}

static void set_title(void)
{
    if (volopen) {
        char nm[40];
        unsigned long kfree = (unsigned long)be16(vol.mdb + 34) * (vol.alsize / 512) / 2;
        lat_name(nm, vol.name);
        sprintf(wintitle, "MacBrowse - %s (%lu KB free)", nm, kfree);
    } else strcpy(wintitle, "MacBrowse " MB_VERSION);
    if (win) SetWindowTitles(win, (UBYTE *)wintitle, (UBYTE *)~0);
}

static int open_image(const char *path)
{
    if (volopen) { hfs_close(&vol); volopen = 0; }
    if (path != imgpath) strncpy(imgpath, path, sizeof(imgpath) - 1);
    if (win) GT_SetGadgetAttrs(gad[G_IMAGE], win, NULL, GTST_String, (ULONG)imgpath, TAG_DONE);
    if (!*imgpath) { list_dir(); set_title(); return -1; }
    busy(1);
    if (hfs_open(&vol, imgpath, 0, -1)) {
        char e[200];
        busy(0);
        sprintf(e, "%.190s", vol.err);
        list_dir(); set_title();
        set_status(e);
        return -1;
    }
    volopen = 1;
    curdir = HFS_ROOT_ID;
    list_dir();
    set_title();
    busy(0);
    return 0;
}

/* Reopen the image for writing; afterwards end_write() goes back to read-only.
   Writing is refused if the image is in use (exclusive lock) or not cleanly unmounted. */
static int begin_write(void)
{
    char e[220];
    if (!volopen) return -1;
    hfs_close(&vol);
    volopen = 0;
    if (hfs_open(&vol, imgpath, 1, -1)) {
        sprintf(e, "This image cannot be written to:\n%.180s", vol.err);
        if (!hfs_open(&vol, imgpath, 0, -1)) volopen = 1;
        req(e);
        return -1;
    }
    volopen = 1;
    return 0;
}

static void end_write(void)
{
    if (hfs_close(&vol)) req(vol.err);
    volopen = !hfs_open(&vol, imgpath, 0, -1);
    list_dir();
    set_title();
}

static void begin_op(void)
{
    gui_errbuf[0] = 0;
    gui_errcount = 0;
    broken = 0;
    busy(1);
}

static void end_op(const char *done)
{
    char b[600];
    busy(0);
    if (gui_errcount) {
        snprintf(b, sizeof(b), "%d problem%s:\n%s%s", gui_errcount, gui_errcount == 1 ? "" : "s",
                 gui_errbuf, gui_errcount > 6 ? "..." : "");
        req(b);
        set_status("Finished with errors");
    } else set_status(done);
}

/* ------------------------------------------------------------------ actions */

static int pick_file(const char *title, char *drawer, char *out, int outlen, int dirsonly, int save)
{
    struct FileRequester *fr = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, (ULONG)win, ASLFR_TitleText, (ULONG)title,
        ASLFR_InitialDrawer, (ULONG)drawer, ASLFR_DrawersOnly, (ULONG)dirsonly,
        ASLFR_DoSaveMode, (ULONG)save, TAG_DONE);
    int ok = 0;
    if (!fr) return 0;
    if (AslRequest(fr, NULL)) {
        strncpy(drawer, (char *)fr->fr_Drawer, 255);
        strncpy(out, (char *)fr->fr_Drawer, outlen - 1);
        out[outlen - 1] = 0;
        if (!dirsonly && !AddPart((STRPTR)out, fr->fr_File, outlen)) out[0] = 0;
        ok = out[0] != 0 || dirsonly;
    }
    FreeAslRequest(fr);
    return ok;
}

static void do_open(void)
{
    char p[512];
    if (pick_file("Open Mac disk image", lastimg, p, sizeof(p), 0, 0)) open_image(p);
}

static void do_get(void)
{
    char dir[512], b[120];
    if (!volopen || sel < 0 || sel >= nitems) { set_status("Select a file or folder first"); return; }
    if (!pick_file("Copy to which drawer?", lastdrawer, dir, sizeof(dir), 1, 1)) return;
    begin_op();
    O.mode = fmtmode[fmt];
    mb_get(&vol, &itemv[sel].e, dir);
    sprintf(b, "Copied to %.100s", dir);
    end_op(b);
}

static void do_put(void)
{
    struct FileRequester *fr;
    int i, n = 0;
    char b[600];
    if (!volopen) return;
    fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_Window, (ULONG)win,
        ASLFR_TitleText, (ULONG)"Copy files into the Mac folder", ASLFR_InitialDrawer, (ULONG)lastin,
        ASLFR_DoMultiSelect, TRUE, TAG_DONE);
    if (!fr) return;
    if (!AslRequest(fr, NULL)) { FreeAslRequest(fr); return; }
    strncpy(lastin, (char *)fr->fr_Drawer, sizeof(lastin) - 1);
    if (begin_write()) { FreeAslRequest(fr); return; }
    begin_op();
    if (fr->fr_NumArgs == 0 && fr->fr_File && fr->fr_File[0]) {
        strncpy(b, (char *)fr->fr_Drawer, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
        AddPart((STRPTR)b, fr->fr_File, sizeof(b));
        if (!mb_put(&vol, b, curdir)) n++;
    }
    for (i = 0; i < fr->fr_NumArgs && !broken; i++) {
        strncpy(b, (char *)fr->fr_Drawer, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
        AddPart((STRPTR)b, fr->fr_ArgList[i].wa_Name, sizeof(b));
        if (!mb_put(&vol, b, curdir)) n++;
    }
    FreeAslRequest(fr);
    end_write();
    sprintf(b, "Copied %d item%s in", n, n == 1 ? "" : "s");
    end_op(b);
}

/* Small modal window with one string gadget. */
static int ask_string(const char *title, char *buf, int len)
{
    struct NewGadget ng;
    struct Gadget *gl = NULL, *g, *sg;
    struct Window *w;
    int fh = scr->Font->ta_YSize, done = 0, ok = 0, ww = 300;
    int top = scr->WBorTop + fh + 1;
    g = CreateContext(&gl);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = 10; ng.ng_TopEdge = top + 6; ng.ng_Width = ww - 20; ng.ng_Height = fh + 6;
    ng.ng_GadgetID = 1;
    sg = g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)buf, GTST_MaxChars, (ULONG)(len - 1), TAG_DONE);
    ng.ng_TopEdge += fh + 12; ng.ng_Width = 80; ng.ng_GadgetText = (UBYTE *)"OK"; ng.ng_GadgetID = 2;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    ng.ng_LeftEdge = ww - 90; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 3;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    if (!g) { FreeGadgets(gl); return 0; }
    w = OpenWindowTags(NULL, WA_Left, (ULONG)(win->LeftEdge + 40), WA_Top, (ULONG)(win->TopEdge + 40),
        WA_Width, (ULONG)ww, WA_Height, (ULONG)(ng.ng_TopEdge + fh + 12 + scr->WBorBottom),
        WA_Title, (ULONG)title, WA_Gadgets, (ULONG)gl, WA_PubScreen, (ULONG)scr,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
        WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | STRINGIDCMP | BUTTONIDCMP, TAG_DONE);
    if (!w) { FreeGadgets(gl); return 0; }
    GT_RefreshWindow(w, NULL);
    ActivateGadget(sg, w, NULL);
    while (!done) {
        struct IntuiMessage *m;
        WaitPort(w->UserPort);
        while ((m = GT_GetIMsg(w->UserPort))) {
            ULONG cl = m->Class;
            struct Gadget *gg = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cl == IDCMP_CLOSEWINDOW) done = 1;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
            else if (cl == IDCMP_GADGETUP) {
                if (gg->GadgetID == 3) done = 1;
                else { ok = 1; done = 1; }
            }
        }
    }
    if (ok) {
        strncpy(buf, (char *)((struct StringInfo *)sg->SpecialInfo)->Buffer, len - 1);
        buf[len - 1] = 0;
    }
    CloseWindow(w);
    FreeGadgets(gl);
    return ok && buf[0];
}

static void do_mkdir(void)
{
    char nm[64] = "";
    uint8_t pn[32];
    int i, n;
    if (!volopen || !ask_string("New Mac folder name", nm, 32)) return;
    n = (int)strlen(nm);
    if (n > 31) n = 31;
    pn[0] = (uint8_t)n;
    for (i = 0; i < n; i++) pn[1 + i] = lat2mac((uint8_t)nm[i]);
    if (begin_write()) return;
    begin_op();
    if (hfs_mkdir(&vol, curdir, pn, NULL)) errf("%s\n", vol.err);
    end_write();
    end_op("Folder created");
}

static void do_delete(void)
{
    char nm[64], b[200];
    HEnt e;
    if (!volopen || sel < 0 || sel >= nitems) { set_status("Select a file or folder first"); return; }
    e = itemv[sel].e;
    lat_name(nm, e.name);
    sprintf(b, "Delete %s \"%s\" from the Mac disk?\nThis cannot be undone.",
            e.type == CDR_DIR ? "the empty folder" : "the file", nm);
    if (!easy("MacBrowse", b, "Delete|Cancel")) return;
    if (begin_write()) return;
    begin_op();
    if (hfs_delete(&vol, e.parid, e.name)) errf("%s\n", vol.err);
    end_write();
    end_op("Deleted");
}

static void enter(int i)
{
    if (i < 0 || i >= nitems) return;
    if (itemv[i].e.type == CDR_DIR) {
        curdir = itemv[i].e.id;
        list_dir();
    } else {
        char b[160], ty[8], cr[8];
        const HEnt *e = &itemv[i].e;
        fourcc(ty, e->finfo); fourcc(cr, e->finfo + 4);
        sprintf(b, "%s/%s  data fork %lu, resource fork %lu bytes", ty, cr, (unsigned long)e->dlen, (unsigned long)e->rlen);
        set_status(b);
    }
}

static void parent(void)
{
    HEnt e;
    if (!volopen || curdir == HFS_ROOT_ID) return;
    if (!hfs_get_id(&vol, curdir, &e)) curdir = e.parid;
    if (curdir < HFS_ROOT_ID) curdir = HFS_ROOT_ID;
    list_dir();
}

/* ------------------------------------------------------------------ window */

static int make_gadgets(int ww, int wh)
{
    struct NewGadget ng;
    struct Gadget *g;
    int fh = scr->Font->ta_YSize, top = scr->WBorTop + fh + 1, left = scr->WBorLeft + 6;
    int iw = ww - scr->WBorLeft - scr->WBorRight - 12, bh = fh + 6, y, lvh, bw, i;
    static const char *bl[] = { "Parent", "Copy out...", "Copy in...", "New folder...", "Delete" };
    static const int bid[] = { G_PARENT, G_GET, G_PUT, G_MKDIR, G_DELETE };
    int labw = TextLength(&scr->RastPort, (STRPTR)"Image", 5) + 8;
    g = CreateContext(&glist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;
    y = top + 4;
    ng.ng_LeftEdge = left + labw; ng.ng_TopEdge = y; ng.ng_Width = iw - labw - 90; ng.ng_Height = bh;
    ng.ng_GadgetText = (UBYTE *)"Image"; ng.ng_GadgetID = G_IMAGE; ng.ng_Flags = PLACETEXT_LEFT;
    g = gad[G_IMAGE] = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)imgpath, GTST_MaxChars, 500, TAG_DONE);
    ng.ng_LeftEdge = left + iw - 84; ng.ng_Width = 84; ng.ng_GadgetText = (UBYTE *)"Open..."; ng.ng_GadgetID = G_OPEN;
    ng.ng_Flags = PLACETEXT_IN;
    g = gad[G_OPEN] = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    y += bh + 4;
    ng.ng_LeftEdge = left; ng.ng_TopEdge = y; ng.ng_Width = iw; ng.ng_Height = fh + 4;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_PATH;
    g = gad[G_PATH] = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)pathtext, GTTX_Border, TRUE, GTTX_Clipped, TRUE, TAG_DONE);
    y += fh + 8;
    lvh = wh - y - scr->WBorBottom - 2 * (bh + 4) - (fh + 4) - 10;
    ng.ng_TopEdge = y; ng.ng_Height = lvh; ng.ng_GadgetID = G_LIST; ng.ng_TextAttr = &topaz8;
    g = gad[G_LIST] = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&items, GTLV_ShowSelected, 0, TAG_DONE);
    ng.ng_TextAttr = scr->Font;
    y += lvh + 4;
    bw = (iw - 4 * 4) / 5;
    for (i = 0; i < 5; i++) {
        ng.ng_LeftEdge = left + i * (bw + 4); ng.ng_TopEdge = y; ng.ng_Width = bw; ng.ng_Height = bh;
        ng.ng_GadgetText = (UBYTE *)bl[i]; ng.ng_GadgetID = bid[i];
        g = gad[bid[i]] = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    y += bh + 4;
    labw = TextLength(&scr->RastPort, (STRPTR)"Copy out as", 11) + 8;
    ng.ng_LeftEdge = left + labw; ng.ng_TopEdge = y; ng.ng_Width = 160; ng.ng_Height = bh;
    ng.ng_GadgetText = (UBYTE *)"Copy out as"; ng.ng_GadgetID = G_FORMAT; ng.ng_Flags = PLACETEXT_LEFT;
    g = gad[G_FORMAT] = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)fmtlabels, GTCY_Active, (ULONG)fmt, TAG_DONE);
    y += bh + 4;
    ng.ng_LeftEdge = left; ng.ng_TopEdge = y; ng.ng_Width = iw; ng.ng_Height = fh + 4;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_STATUS; ng.ng_Flags = 0;
    g = gad[G_STATUS] = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)status, GTTX_Border, TRUE, GTTX_Clipped, TRUE, TAG_DONE);
    return g != NULL;
}

static void wb_image(char *out, int len)
{
    /* an image icon shift-clicked together with MacBrowse, or the IMAGE= tooltype */
    struct WBStartup *wb = _WBenchMsg;
    out[0] = 0;
    if (!wb) return;
    if (wb->sm_NumArgs > 1 && wb->sm_ArgList[1].wa_Lock) {
        if (NameFromLock(wb->sm_ArgList[1].wa_Lock, (STRPTR)out, len))
            AddPart((STRPTR)out, wb->sm_ArgList[1].wa_Name, len);
        else out[0] = 0;
        return;
    }
    {
        struct DiskObject *dob;
        BPTR old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
        dob = GetDiskObject((STRPTR)wb->sm_ArgList[0].wa_Name);
        if (dob) {
            STRPTR s = (STRPTR)FindToolType((CONST_STRPTR *)dob->do_ToolTypes, (CONST_STRPTR)"IMAGE");
            if (s) { strncpy(out, (char *)s, len - 1); out[len - 1] = 0; }
            FreeDiskObject(dob);
        }
        CurrentDir(old);
    }
}

static int gui_run(const char *image)
{
    int ww = 600, wh, done = 0;
    ULONG lastsec = 0, lastmic = 0;
    int lastsel = -1;
    static char first[512];

    gui_mode = 1;
    O.quiet = 1;
    O.mode = M_DATA;
    O.part = -1;
    init_list(&items);
    if (image) { strncpy(first, image, sizeof(first) - 1); first[sizeof(first) - 1] = 0; }
    else wb_image(first, sizeof(first));

    if (!(scr = LockPubScreen(NULL))) return 20;
    if (!(vi = GetVisualInfo(scr, TAG_DONE))) { UnlockPubScreen(NULL, scr); return 20; }
    if (ww > scr->Width) ww = scr->Width;
    wh = scr->Height - scr->BarHeight - 20;
    if (wh > 420) wh = 420;
    if (!make_gadgets(ww, wh)) {
        FreeGadgets(glist); FreeVisualInfo(vi); UnlockPubScreen(NULL, scr);
        return 20;
    }
    strcpy(status, "Open a Mac disk image (.hfv file disk, hardfile or CD image)");
    win = OpenWindowTags(NULL, WA_Left, (ULONG)((scr->Width - ww) / 2), WA_Top, (ULONG)(scr->BarHeight + 4),
        WA_Width, (ULONG)ww, WA_Height, (ULONG)wh, WA_Title, (ULONG)"MacBrowse",
        WA_Gadgets, (ULONG)glist, WA_PubScreen, (ULONG)scr,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_SmartRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP | IDCMP_VANILLAKEY |
                  LISTVIEWIDCMP | BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP, TAG_DONE);
    if (!win) { FreeGadgets(glist); FreeVisualInfo(vi); UnlockPubScreen(NULL, scr); return 20; }
    GT_RefreshWindow(win, NULL);
    set_title();
    if (first[0]) open_image(first);
    else set_status(status);

    while (!done) {
        struct IntuiMessage *m;
        WaitPort(win->UserPort);
        while ((m = GT_GetIMsg(win->UserPort))) {
            ULONG cl = m->Class, secs = m->Seconds, mics = m->Micros;
            UWORD code = m->Code;
            struct Gadget *g = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            switch (cl) {
            case IDCMP_CLOSEWINDOW: done = 1; break;
            case IDCMP_REFRESHWINDOW: GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
            case IDCMP_VANILLAKEY:
                if (code == 8) parent();                        /* backspace */
                else if (code == 13 && sel >= 0) enter(sel);
                else if (code == 27) done = 1;                  /* escape */
                break;
            case IDCMP_GADGETUP:
                switch (g->GadgetID) {
                case G_IMAGE:
                    open_image((char *)((struct StringInfo *)g->SpecialInfo)->Buffer);
                    break;
                case G_OPEN: do_open(); break;
                case G_LIST:
                    sel = code;
                    if (sel == lastsel && DoubleClick(lastsec, lastmic, secs, mics)) { enter(sel); lastsel = -1; }
                    else {
                        lastsel = sel; lastsec = secs; lastmic = mics;
                        if (sel < nitems && itemv[sel].e.type != CDR_DIR) enter(sel);
                    }
                    break;
                case G_PARENT: parent(); break;
                case G_GET: do_get(); break;
                case G_PUT: do_put(); break;
                case G_MKDIR: do_mkdir(); break;
                case G_DELETE: do_delete(); break;
                case G_FORMAT: fmt = code; break;
                }
                break;
            }
        }
    }
    if (volopen) hfs_close(&vol);
    CloseWindow(win);
    win = NULL;
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    UnlockPubScreen(NULL, scr);
    free(itemv);
    return 0;
}

int gui_main(const char *image)
{
    int rc = 20;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37);
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
    AslBase = OpenLibrary((STRPTR)"asl.library", 37);
    IconBase = OpenLibrary((STRPTR)"icon.library", 37);
    if (IntuitionBase && GfxBase && GadToolsBase && AslBase && IconBase) rc = gui_run(image);
    else {
        BPTR out = Output();
        if (out) FPuts(out, (STRPTR)"MacBrowse: the window needs AmigaOS 2.04+ (intuition, gadtools, asl, icon)\n");
    }
    if (IconBase) CloseLibrary(IconBase);
    if (AslBase) CloseLibrary(AslBase);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
