/* HDPart GadTools GUI (read-only display in Plan 3a). */
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <inline/alib.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include "gui.h"
#include "discover.h"
#include "device.h"
#include "rdb.h"

extern struct IntuitionBase *IntuitionBase;   /* opened in main.c */
struct Library *GadToolsBase = 0;
struct GfxBase *GfxBase = 0;

/* Gadget IDs */
enum { GID_DEVICE = 1, GID_RESCAN, GID_PARTS, GID_NEW, GID_DELETE, GID_EDIT,
       GID_INIT, GID_SAVE, GID_STATUS };

/* Module state for one GUI session. */
static struct Screen  *g_scr;        /* screen we render on (pub or own) */
static struct Screen  *g_pub;        /* locked pubscreen, or NULL */
static struct Window  *g_win;
static APTR            g_vi;          /* VisualInfo */
static struct Gadget  *g_glist;      /* gadtools context list */
static struct Gadget  *g_gad[16];    /* gadget pointers by a small index */
static struct TextAttr g_font = { (STRPTR)"topaz.font", 8, 0, 0 };

/* Discovery + current selection (static: keep off the stack). */
static DiscDisk g_disks[DISC_MAX];
static int      g_ndisks;
static const char *g_devlabels[DISC_MAX + 1];  /* for GTCY_Labels */
static char     g_devtext[DISC_MAX][48];
static RdbModel g_model;
static int      g_have_model;
static int      g_devmap[DISC_MAX];  /* cycle position -> g_disks index */
static WORD     g_topb, g_leftb;     /* window border offsets (title bar / left edge);
                                        gadget coords are relative to the window's
                                        outer top-left, so all gadgets shift by these */

/* Exec list of partition rows for the listview. */
static struct List g_partlist;
static struct Node g_partnodes[RDB_MAX_PARTS];
/* 80 bytes: worst legal row (31-char name + 3x10-digit fields + labels + NUL)
   is 67 bytes; 80 leaves headroom and prevents overflow for any legal RDB. */
static char        g_partrows[RDB_MAX_PARTS][80];
static char        g_statusbuf[80];

static int u2s(char *o, ULONG v)   /* write decimal, return length */
{
    char tmp[12]; int ti = 0, n = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
    while (ti > 0) o[n++] = tmp[--ti];
    return n;
}
static void s_cat(char *o, int *p, const char *s) { while (*s) o[(*p)++] = *s++; }
static void s_pad(char *o, int *p, int col) { while (*p < col) o[(*p)++] = ' '; }

/* Forward decls (implemented in later tasks). */
void gui_rescan(void);
void gui_select_device(int idx);
void gui_draw_bar(void);

static struct Gadget *build_gadgets(void)
{
    struct NewGadget ng;
    struct Gadget *g;
    int i;
    for (i = 0; i < 16; i++) g_gad[i] = 0;

    /* CreateContext's inline macro (LP1) declares the input register as
       'volatile t1' which in GCC 14+ triggers -Wincompatible-pointer-types
       (now an error).  Suppress around this one call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&g_glist);
#pragma GCC diagnostic pop
    if (!g) return 0;

    /* Device cycle gadget. All ng positions are offset by the window border
       (g_leftb,g_topb) since gadget coords are relative to the window origin. */
    ng.ng_TextAttr   = &g_font;
    ng.ng_VisualInfo = g_vi;
    ng.ng_LeftEdge = 70 + g_leftb;  ng.ng_TopEdge = 6 + g_topb;  ng.ng_Width = 300; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Disk:"; ng.ng_GadgetID = GID_DEVICE;
    ng.ng_Flags = 0;
    g_devlabels[0] = "(no disks)"; g_devlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_devlabels, TAG_END);
    g_gad[GID_DEVICE] = g;

    /* Rescan button */
    ng.ng_LeftEdge = 380 + g_leftb; ng.ng_TopEdge = 6 + g_topb; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Rescan"; ng.ng_GadgetID = GID_RESCAN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_RESCAN] = g;

    /* Partition listview */
    ng.ng_LeftEdge = 10 + g_leftb; ng.ng_TopEdge = 72 + g_topb; ng.ng_Width = 440; ng.ng_Height = 90;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_PARTS;
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, 0, GTLV_ReadOnly, TRUE, TAG_END);
    g_gad[GID_PARTS] = g;

    /* Read-only status text */
    ng.ng_LeftEdge = 70 + g_leftb; ng.ng_TopEdge = 168 + g_topb; ng.ng_Width = 380; ng.ng_Height = 12;
    ng.ng_GadgetText = (UBYTE *)"Status:"; ng.ng_GadgetID = GID_STATUS;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"no disk selected", TAG_END);
    g_gad[GID_STATUS] = g;

    /* Ghosted action buttons (enabled in Plan 3b) */
    {
        static const struct { int id; const char *txt; int x; } btn[] = {
            { GID_NEW, "New", 10 }, { GID_DELETE, "Delete", 70 }, { GID_EDIT, "Edit", 150 },
            { GID_INIT, "Init Disk", 280 }, { GID_SAVE, "Save", 390 }
        };
        int k;
        for (k = 0; k < 5; k++) {
            ng.ng_LeftEdge = btn[k].x + g_leftb; ng.ng_TopEdge = 186 + g_topb;
            ng.ng_Width = (btn[k].id == GID_INIT) ? 90 : 60; ng.ng_Height = 14;
            ng.ng_GadgetText = (UBYTE *)btn[k].txt; ng.ng_GadgetID = btn[k].id;
            g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, TAG_END);
            g_gad[btn[k].id] = g;
        }
    }
    return g;
}

int gui_run(void)
{
    BOOL done = FALSE;

    GadToolsBase = OpenLibrary("gadtools.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    if (!GadToolsBase || !GfxBase) {
        if (GadToolsBase) CloseLibrary(GadToolsBase);
        if (GfxBase) CloseLibrary((struct Library *)GfxBase);
        return 20;
    }

    g_pub = LockPubScreen(0);
    if (g_pub) g_scr = g_pub;
    else {
        g_scr = OpenScreenTags(0, SA_Depth, 2, SA_Title, (ULONG)"HDPart",
                               SA_Type, CUSTOMSCREEN, TAG_END);
    }
    if (!g_scr) goto cleanup_libs;

    /* Window border offsets: gadget coords are relative to the window's outer
       top-left (which includes the title bar), so shift all gadgets down/right
       by these and size the window to enclose the content. */
    g_topb  = g_scr->WBorTop + g_scr->Font->ta_YSize + 1;
    g_leftb = g_scr->WBorLeft;

    g_vi = GetVisualInfo(g_scr, TAG_END);
    if (!g_vi) goto cleanup_scr;

    if (!build_gadgets()) goto cleanup_gad;  /* FreeGadgets handles a partial/NULL chain */

    g_win = OpenWindowTags(0,
        WA_Left, 40, WA_Top, 16,
        WA_Width,  g_leftb + 460 + g_scr->WBorRight,
        WA_Height, g_topb + 206 + g_scr->WBorBottom,
        WA_Title, (ULONG)"HDPart 0.1",
        WA_Gadgets, (ULONG)g_glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | CYCLEIDCMP | BUTTONIDCMP | LISTVIEWIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                  WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
    if (!g_win) goto cleanup_gad;

    GT_RefreshWindow(g_win, 0);
    gui_rescan();          /* populate device list (Task 3a.2.1) */
    gui_draw_bar();        /* initial bar (Task 3a.4.1) */

    while (!done) {
        struct IntuiMessage *imsg;
        WaitPort(g_win->UserPort);
        while ((imsg = GT_GetIMsg(g_win->UserPort)) != 0) {
            ULONG cls = imsg->Class;
            struct Gadget *gad = (struct Gadget *)imsg->IAddress;
            UWORD code = imsg->Code;
            GT_ReplyIMsg(imsg);
            switch (cls) {
                case IDCMP_CLOSEWINDOW:
                    done = TRUE;
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(g_win);
                    gui_draw_bar();
                    GT_EndRefresh(g_win, TRUE);
                    break;
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GID_DEVICE) gui_select_device((int)code);
                    else if (gad->GadgetID == GID_RESCAN) gui_rescan();
                    break;
            }
        }
    }

    CloseWindow(g_win); g_win = 0;
cleanup_gad:
    FreeGadgets(g_glist); g_glist = 0;
    FreeVisualInfo(g_vi); g_vi = 0;   /* reached by fall-through from cleanup_gad */
cleanup_scr:
    if (!g_pub && g_scr) CloseScreen(g_scr);
    if (g_pub) UnlockPubScreen(0, g_pub);
    g_scr = 0; g_pub = 0;
cleanup_libs:
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary(GadToolsBase);
    return 0;
}

void gui_rescan(void)
{
    int i, n = 0;
    g_ndisks = discover_disks(g_disks, DISC_MAX);

    /* Build cycle labels from partitionable disks only. */
    for (i = 0; i < g_ndisks && n < DISC_MAX; i++) {
        DiscDisk *d = &g_disks[i];
        char *t = g_devtext[n];
        int p = 0, k;
        if (!d->partitionable) continue;
        g_devmap[n] = i;
        for (k = 0; d->driver[k] && p < 30; k++) t[p++] = d->driver[k];
        t[p++] = ' '; t[p++] = 'u';
        /* unit as decimal (tmp[12]: a uint32 is up to 10 digits) */
        { ULONG u = d->unit; char tmp[12]; int ti = 0;
          if (u == 0) tmp[ti++] = '0';
          while (u) { tmp[ti++] = (char)('0' + (u % 10)); u /= 10; }
          while (ti > 0 && p < 44) t[p++] = tmp[--ti]; }
        t[p++] = ' ';
        /* size MB */
        { ULONG s = d->size_mb; char tmp[12]; int ti = 0;
          if (s == 0) tmp[ti++] = '0';
          while (s) { tmp[ti++] = (char)('0' + (s % 10)); s /= 10; }
          while (ti > 0 && p < 46) t[p++] = tmp[--ti];
          t[p++] = 'M'; }
        t[p] = 0;
        g_devlabels[n] = t;
        n++;
    }
    if (n == 0) { g_devlabels[0] = "(no disks found)"; g_devlabels[1] = 0; }
    else g_devlabels[n] = 0;

    if (g_win && g_gad[GID_DEVICE])
        GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0,
                          GTCY_Labels, (ULONG)g_devlabels,
                          GTCY_Active, 0, TAG_END);

    if (n > 0) gui_select_device(0);
}

void gui_select_device(int idx)
{
    DeviceHandle *h;
    DiscDisk *d;
    int i, di;

    g_have_model = 0;
    NewList(&g_partlist);

    if (g_ndisks == 0 || idx < 0) {
        { int p = 0; s_cat(g_statusbuf, &p, "no disk selected"); g_statusbuf[p] = 0; }
        if (g_win && g_gad[GID_PARTS])
            GT_SetGadgetAttrs(g_gad[GID_PARTS], g_win, 0, GTLV_Labels, (ULONG)&g_partlist, TAG_END);
        if (g_win && g_gad[GID_STATUS])
            GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
        gui_draw_bar();
        return;
    }
    di = g_devmap[idx];
    d  = &g_disks[di];

    h = dev_open(d->driver, d->unit);
    if (h) {
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) g_have_model = 1;
        dev_close(h);
    }

    if (g_have_model) {
        for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
            RdbPartition *pt = &g_model.parts[i];
            char *row = g_partrows[i];
            int p = 0;
            s_cat(row, &p, pt->name);                 s_pad(row, &p, 8);
            s_cat(row, &p, "FFS");                     s_pad(row, &p, 14);
            p += u2s(row + p, pt->low_cyl);            s_pad(row, &p, 22);
            p += u2s(row + p, pt->high_cyl);           s_pad(row, &p, 30);
            { ULONG cyls = pt->high_cyl - pt->low_cyl + 1;
              ULONG mb = disc_blocks_to_mb(cyls * g_model.cyl_blocks, g_model.block_bytes);
              p += u2s(row + p, mb); s_cat(row, &p, "MB"); }
            row[p] = 0;
            g_partnodes[i].ln_Name = row;
            AddTail(&g_partlist, &g_partnodes[i]);
        }
        { int p = 0; s_cat(g_statusbuf, &p, "RDB OK  ");
          p += u2s(g_statusbuf + p, (ULONG)g_model.num_parts);
          s_cat(g_statusbuf, &p, " partitions  ");
          p += u2s(g_statusbuf + p, g_model.cylinders); s_cat(g_statusbuf, &p, " cyl");
          g_statusbuf[p] = 0; }
    } else {
        const char *m = "no valid RDB on this disk";
        int p = 0; s_cat(g_statusbuf, &p, m); g_statusbuf[p] = 0;
    }

    if (g_win && g_gad[GID_PARTS])
        GT_SetGadgetAttrs(g_gad[GID_PARTS], g_win, 0, GTLV_Labels, (ULONG)&g_partlist, TAG_END);
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
    gui_draw_bar();
}

void gui_draw_bar(void)
{
    struct RastPort *rp;
    int bx = 10 + g_leftb, by = 26 + g_topb, bw = 440, bh = 16;   /* bar rectangle in the window */
    int i;
    if (!g_win) return;
    rp = g_win->RPort;

    /* frame + unused background (pen 2 = black border, pen 0 = bg) */
    SetAPen(rp, 1); RectFill(rp, bx, by, bx + bw - 1, by + bh - 1);    /* fill light */
    SetAPen(rp, 2); Move(rp, bx, by); Draw(rp, bx + bw - 1, by);
    Draw(rp, bx + bw - 1, by + bh - 1); Draw(rp, bx, by + bh - 1); Draw(rp, bx, by);

    if (!g_have_model || g_model.cylinders == 0) return;

    /* each partition drawn proportional to its cylinder span over hi_cyl */
    for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
        RdbPartition *pt = &g_model.parts[i];
        int x0 = bx + (int)((pt->low_cyl  * (ULONG)bw) / g_model.cylinders);
        int x1 = bx + (int)(((pt->high_cyl + 1) * (ULONG)bw) / g_model.cylinders);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > bx + bw - 1) x1 = bx + bw - 1;
        SetAPen(rp, (UBYTE)(3 - (i & 1)));   /* alternate pens 3/2 */
        RectFill(rp, x0, by + 1, x1 - 1, by + bh - 2);
    }
}
