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
static int      g_dirty;            /* unsaved edits in g_model */
static DeviceInfo g_geo;            /* geometry of the selected device */
static char     g_cur_driver[40];   /* selected device driver/unit (for Save) */
static uint32_t g_cur_unit;
static int      g_sel_part = -1;    /* selected partition index, or -1 */
static int      g_devmap[DISC_MAX]; /* cycle position -> g_disks index */
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

/* Forward decls. */
void gui_rescan(void);
void gui_select_device(int idx);
void gui_draw_bar(void);
static void gui_set_selection(int idx);   /* select partition `idx` (or -1) + redraw */
static int  gui_part_at_x(int mx, int my);/* partition under a window-relative point, or -1 */

static void gui_update_buttons(void)
{
    int hasModel = g_have_model;
    int hasGeo   = g_geo.has_media;
    int hasSel   = (g_sel_part >= 0 && g_sel_part < g_model.num_parts);
    if (!g_win) return;
    GT_SetGadgetAttrs(g_gad[GID_SAVE],   g_win, 0, GA_Disabled, (ULONG)!(hasModel && g_dirty), TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_NEW],    g_win, 0, GA_Disabled, (ULONG)!hasModel, TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_INIT],   g_win, 0, GA_Disabled, (ULONG)!hasGeo,   TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_DELETE], g_win, 0, GA_Disabled, (ULONG)!hasSel,   TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_EDIT],   g_win, 0, GA_Disabled, (ULONG)!hasSel,   TAG_END);
}

/* Rebuild the partition listview + status text + bar from g_model. */
static void gui_refresh_parts(void)
{
    int i;
    NewList(&g_partlist);
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
        { int p = 0; s_cat(g_statusbuf, &p, g_dirty ? "MODIFIED  " : "RDB OK  ");
          p += u2s(g_statusbuf + p, (ULONG)g_model.num_parts);
          s_cat(g_statusbuf, &p, " partitions  ");
          p += u2s(g_statusbuf + p, g_model.cylinders); s_cat(g_statusbuf, &p, " cyl");
          g_statusbuf[p] = 0; }
    } else {
        int p = 0; s_cat(g_statusbuf, &p, "no valid RDB on this disk"); g_statusbuf[p] = 0;
    }
    if (g_win && g_gad[GID_PARTS])
        GT_SetGadgetAttrs(g_gad[GID_PARTS], g_win, 0,
                          GTLV_Labels, (ULONG)&g_partlist,
                          GTLV_Selected, (ULONG)(g_sel_part >= 0 ? (ULONG)g_sel_part : ~0UL),
                          TAG_END);
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
    gui_draw_bar();
}

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
    /* GTLV_ShowSelected (NULL) makes this a SELECTION listview: the clicked row
       stays highlighted, rather than a scroll-only list with momentary highlight. */
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, 0, GTLV_ShowSelected, 0, TAG_END);
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

/* Draw a multi-line ('\n'-separated) message into a window's RastPort. */
static void gui_draw_text(struct Window *w, const char *body, int x, int y)
{
    struct RastPort *rp = w->RPort;
    const char *line = body, *p = body;
    int ly = y + 7;                 /* first baseline */
    SetAPen(rp, 1);
    for (;;) {
        if (*p == '\n' || *p == 0) {
            Move(rp, x, ly);
            Text(rp, (CONST_STRPTR)line, (LONG)(p - line));
            ly += 10;
            if (*p == 0) break;
            line = p + 1;
        }
        p++;
    }
}

/* Modal requester centered over the main window. twoButtons=1 -> Proceed/Cancel
   (returns 1 for Proceed, 0 for Cancel/close); twoButtons=0 -> single OK. */
static int gui_request(const char *title, const char *body, int twoButtons)
{
    struct Window *dw;
    struct Gadget *glist = 0, *g;
    struct NewGadget ng;
    int dt = g_topb, dl = g_leftb;
    int nlines = 1, maxw = 0, cur = 0;
    int textW, dwW, dwH, dwL, dwT, btnY;
    int done = 0, result = 0;
    const char *p;

    for (p = body; ; p++) {                 /* measure: lines + longest line */
        if (*p == '\n' || *p == 0) {
            if (cur > maxw) maxw = cur;
            cur = 0;
            if (*p == '\n') nlines++; else break;
        } else cur++;
    }
    textW = maxw * 8; if (textW < 180) textW = 180;
    btnY  = dt + 8 + nlines * 10 + 8;
    dwW   = dl + textW + 24 + g_scr->WBorRight;
    dwH   = btnY + 14 + 8 + g_scr->WBorBottom;
    dwL   = g_win->LeftEdge + (g_win->Width  - dwW) / 2; if (dwL < 0) dwL = 0;
    dwT   = g_win->TopEdge  + (g_win->Height - dwH) / 2; if (dwT < 0) dwT = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;
    ng.ng_TopEdge = btnY; ng.ng_Width = 90; ng.ng_Height = 14;
    if (twoButtons) {
        ng.ng_LeftEdge = dl + 12;  ng.ng_GadgetText = (UBYTE *)"Proceed"; ng.ng_GadgetID = 1;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
        ng.ng_LeftEdge = dl + textW + 12 - 90; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 0;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    } else {
        ng.ng_LeftEdge = dl + (textW + 24 - 90) / 2; ng.ng_GadgetText = (UBYTE *)"OK"; ng.ng_GadgetID = 1;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (!g) { FreeGadgets(glist); return 0; }

    dw = OpenWindowTags(0,
        WA_Left, dwL, WA_Top, dwT, WA_Width, dwW, WA_Height, dwH,
        WA_Title, (ULONG)title, WA_Gadgets, (ULONG)glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
    if (!dw) { FreeGadgets(glist); return 0; }
    GT_RefreshWindow(dw, 0);
    gui_draw_text(dw, body, dl + 12, dt + 6);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { result = 0; done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(dw); gui_draw_text(dw, body, dl + 12, dt + 6); GT_EndRefresh(dw, TRUE);
            } else if (cl == IDCMP_GADGETUP) { result = (ig->GadgetID == 1) ? 1 : 0; done = 1; }
        }
    }
    CloseWindow(dw);
    FreeGadgets(glist);
    return result;
}

static int  gui_confirm(const char *title, const char *body) { return gui_request(title, body, 1); }
static void gui_msg(const char *title, const char *body)      { (void)gui_request(title, body, 0); }

static char g_msgbuf[120];

static int gui_save(void)
{
    DeviceHandle *h;
    static RdbModel chk;          /* static: keep off the stack */
    int ok = 0, p = 0;

    if (!g_have_model) return 0;
    if (rdb_validate(&g_model) != RDB_OK) { gui_msg("Save", "Partition layout is invalid."); return 0; }

    /* Build a confirm message naming the device (no % in the string). */
    s_cat(g_msgbuf, &p, "Write the partition table to\n");
    { int k; for (k = 0; g_cur_driver[k] && p < 100; k++) g_msgbuf[p++] = g_cur_driver[k]; }
    s_cat(g_msgbuf, &p, " unit ");
    p += u2s(g_msgbuf + p, g_cur_unit);
    s_cat(g_msgbuf, &p, " ?\nThis overwrites the disk's RDB.");
    g_msgbuf[p] = 0;
    if (!gui_confirm("Save", g_msgbuf)) return 0;

    h = dev_open(g_cur_driver, g_cur_unit);
    if (!h) { gui_msg("Save", "Could not open the device."); return 0; }
    if (rdb_serialize(&g_model, dev_block_io, h) == RDB_OK) {
        /* read back and verify the partition count + first/last cylinders */
        if (rdb_parse(&chk, dev_block_io, h) == RDB_OK &&
            chk.num_parts == g_model.num_parts) {
            int i; ok = 1;
            for (i = 0; i < chk.num_parts; i++)
                if (chk.parts[i].low_cyl  != g_model.parts[i].low_cyl ||
                    chk.parts[i].high_cyl != g_model.parts[i].high_cyl) { ok = 0; break; }
        }
    }
    dev_close(h);

    if (ok) { g_dirty = 0; gui_msg("Save", "Saved and verified."); }
    else      gui_msg("Save", "WRITE FAILED or verify mismatch.\nThe disk RDB may be inconsistent.");
    gui_refresh_parts();
    gui_update_buttons();
    return ok;
}

static void gui_init_disk(void)
{
    if (!g_geo.has_media || g_geo.cylinders == 0) {
        gui_msg("Init Disk", "No media / geometry on this device."); return;
    }
    if (!gui_confirm("Init Disk",
        "Replace the in-memory partition table with a\nfresh empty one for this disk?\n"
        "(Nothing is written until you press Save.)"))
        return;
    rdb_init_model(&g_model, g_geo.cylinders, g_geo.heads, g_geo.sectors);
    g_have_model = 1;
    g_dirty = 1;
    g_sel_part = -1;
    gui_refresh_parts();
    gui_update_buttons();
}

static void gui_delete(void)
{
    if (g_sel_part < 0 || g_sel_part >= g_model.num_parts) return;
    if (!gui_confirm("Delete Partition",
        "Remove the selected partition from the\nin-memory table? (Save to apply.)"))
        return;
    rdb_delete_partition(&g_model, g_sel_part);
    g_sel_part = -1;
    g_dirty = 1;
    gui_refresh_parts();
    gui_update_buttons();
}

/* Filesystem cycle choices (phase 1: FFS Intl only; dos types parallel). */
static const char *const kFsLabels[] = { "FFS Intl (DOS\\3)", 0 };
static const uint32_t     kFsTypes[]  = { RDB_DOSTYPE_FFS_INTL };

/* Edit partition `index` of g_model via a modal dialog. Returns 1 if applied. */
static int gui_edit_dialog(int index)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g;
    struct Gadget *gName = 0, *gSize = 0;
    struct NewGadget ng;
    RdbPartition *pt = &g_model.parts[index];
    static char nameBuf[32];
    static char sizeMaxLabel[24];
    uint32_t startCyl = pt->low_cyl;
    uint32_t maxEndExclusive;   /* first cylinder not available to this part */
    uint32_t maxCyls, maxMB, curMB;
    int done = 0, applied = 0;
    int dt = g_topb, dl = g_leftb;
    int i, k;

    /* compute the space available to this partition (start..next part start-1 or hi_cyl) */
    maxEndExclusive = g_model.hi_cyl + 1;
    for (i = 0; i < g_model.num_parts; i++) {
        if (i == index) continue;
        if (g_model.parts[i].low_cyl >= startCyl &&
            g_model.parts[i].low_cyl < maxEndExclusive)
            maxEndExclusive = g_model.parts[i].low_cyl;
    }
    maxCyls = (maxEndExclusive > startCyl) ? (maxEndExclusive - startCyl) : 1;
    maxMB   = rdb_cyls_to_mb(maxCyls, g_model.cyl_blocks, g_model.block_bytes);
    if (maxMB < 1) maxMB = 1;
    curMB   = rdb_cyls_to_mb(pt->high_cyl - pt->low_cyl + 1, g_model.cyl_blocks, g_model.block_bytes);
    if (curMB < 1) curMB = 1;
    if (curMB > maxMB) curMB = maxMB;

    for (k = 0; k < 31 && pt->name[k]; k++) nameBuf[k] = pt->name[k];
    nameBuf[k] = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&dglist);
#pragma GCC diagnostic pop
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    ng.ng_LeftEdge = dl + 90; ng.ng_TopEdge = dt + 6; ng.ng_Width = 180; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Name"; ng.ng_GadgetID = 1;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)nameBuf, GTST_MaxChars, 31, TAG_END);
    gName = g;

    /* "max <N> MB" hint shown beside the size field */
    { int p = 0; sizeMaxLabel[p++]='m'; sizeMaxLabel[p++]='a'; sizeMaxLabel[p++]='x'; sizeMaxLabel[p++]=' ';
      p += u2s(sizeMaxLabel + p, maxMB); sizeMaxLabel[p++]=' '; sizeMaxLabel[p++]='M'; sizeMaxLabel[p++]='B';
      sizeMaxLabel[p] = 0; }

    ng.ng_LeftEdge = dl + 90; ng.ng_TopEdge = dt + 26; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Size (MB)"; ng.ng_GadgetID = 2;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, curMB, GTIN_MaxChars, 7, TAG_END);
    gSize = g;

    ng.ng_LeftEdge = dl + 180; ng.ng_Width = 110; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)sizeMaxLabel, TAG_END);

    ng.ng_LeftEdge = dl + 90; ng.ng_TopEdge = dt + 48; ng.ng_Width = 180; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"FS"; ng.ng_GadgetID = 4;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)kFsLabels, GTCY_Active, 0, TAG_END);

    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 70; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 210; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return 0; }

    {   /* center the dialog over the main window */
        int dwW = dl + 300 + g_scr->WBorRight;
        int dwH = dt + 92 + g_scr->WBorBottom;
        int dwL = g_win->LeftEdge + (g_win->Width  - dwW) / 2;
        int dwT = g_win->TopEdge  + (g_win->Height - dwH) / 2;
        if (dwL < 0) dwL = 0;
        if (dwT < 0) dwT = 0;
        dw = OpenWindowTags(0,
            WA_Left, dwL, WA_Top, dwT, WA_Width, dwW, WA_Height, dwH,
            WA_Title, (ULONG)"Edit Partition",
            WA_Gadgets, (ULONG)dglist,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP | INTEGERIDCMP,
            WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
            g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
            TAG_END);
    }
    if (!dw) { FreeGadgets(dglist); return 0; }
    GT_RefreshWindow(dw, 0);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) {
                if (ig == gSize) {
                    /* clamp the typed size to [1, maxMB] */
                    LONG v = ((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    if (v < 1) v = 1;
                    if ((ULONG)v > maxMB) v = (LONG)maxMB;
                    GT_SetGadgetAttrs(gSize, dw, 0, GTIN_Number, (ULONG)v, TAG_END);
                } else if (ig->GadgetID == 10) {                                /* Ok */
                    LONG mb = ((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    char *nm = (char *)((struct StringInfo *)gName->SpecialInfo)->Buffer;
                    int r;
                    if (mb < 1) mb = 1;
                    if ((ULONG)mb > maxMB) mb = (LONG)maxMB;
                    /* If the MB field is unchanged from what the dialog opened
                       with, keep the EXACT cylinder range (so a gap-filling
                       partition isn't silently shrunk by MB re-rounding); only
                       resize when the user actually changed the size. */
                    if ((uint32_t)mb == curMB)
                        r = rdb_rename_partition(&g_model, index, nm, kFsTypes[0]);
                    else
                        r = rdb_set_partition(&g_model, index, nm, (uint32_t)mb, kFsTypes[0]);
                    if (r == RDB_OK) {
                        applied = 1; done = 1;
                    } else {
                        gui_msg("Edit", "Invalid name or size (overlaps / out of range).");
                    }
                } else if (cl == IDCMP_GADGETUP && ig->GadgetID == 11) { done = 1; } /* Cancel */
            }
        }
    }

    CloseWindow(dw);
    FreeGadgets(dglist);
    if (applied) g_dirty = 1;
    return applied;
}

/* Pick the lowest unused "DH<n>" name into out[32]. */
static void gui_auto_name(char *out)
{
    int n, i, used, p;
    for (n = 0; n < 100; n++) {
        used = 0;
        for (i = 0; i < g_model.num_parts; i++) {
            const char *nm = g_model.parts[i].name;
            /* compare nm == "DH<n>" */
            char cand[8]; int cp = 0;
            cand[cp++]='D'; cand[cp++]='H'; cp += u2s(cand + cp, (ULONG)n); cand[cp]=0;
            { int j=0; while (cand[j] && nm[j] && cand[j]==nm[j]) j++;
              if (cand[j]==0 && nm[j]==0) { used = 1; break; } }
        }
        if (!used) break;
    }
    p = 0; out[p++]='D'; out[p++]='H'; p += u2s(out + p, (ULONG)n); out[p]=0;
}

static void gui_new(void)
{
    uint32_t gs = 0, ge = 0;
    char name[32];
    int idx;
    if (!g_have_model) return;
    if (!rdb_largest_free_gap(&g_model, &gs, &ge)) {
        gui_msg("New Partition", "No free space on this disk."); return;
    }
    gui_auto_name(name);
    /* Fill the whole free gap exactly (by cylinder) — converting to MB and back
       would floor away the fractional cylinders and waste space. */
    idx = rdb_add_partition_cyl(&g_model, name, gs, ge, RDB_DOSTYPE_FFS_INTL);
    if (idx < 0) { gui_msg("New Partition", "Could not add the partition."); return; }
    g_sel_part = idx;
    g_dirty = 1;
    gui_refresh_parts();
    gui_edit_dialog(idx);            /* let the user adjust size/name immediately */
    gui_refresh_parts();
    gui_update_buttons();
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
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS | CYCLEIDCMP | BUTTONIDCMP | LISTVIEWIDCMP,
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
            WORD  mx = imsg->MouseX, my = imsg->MouseY;
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
                case IDCMP_MOUSEBUTTONS:
                    if (code == SELECTDOWN) {           /* click on the disk-map bar selects */
                        int idx = gui_part_at_x((int)mx, (int)my);
                        if (idx >= 0) gui_set_selection(idx);
                    }
                    break;
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GID_DEVICE) gui_select_device((int)code);
                    else if (gad->GadgetID == GID_RESCAN) gui_rescan();
                    else if (gad->GadgetID == GID_PARTS) gui_set_selection((int)code);
                    else if (gad->GadgetID == GID_SAVE) gui_save();
                    else if (gad->GadgetID == GID_INIT) gui_init_disk();
                    else if (gad->GadgetID == GID_DELETE) gui_delete();
                    else if (gad->GadgetID == GID_NEW) gui_new();
                    else if (gad->GadgetID == GID_EDIT) {
                        if (g_sel_part >= 0 && g_sel_part < g_model.num_parts) {
                            gui_edit_dialog(g_sel_part);
                            gui_refresh_parts();
                            gui_update_buttons();
                        }
                    }
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
    /* Discovery probes many devices/units and can take a moment; show progress
       so the window doesn't look hung while the dropdown is empty. */
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0,
                          GTTX_Text, (ULONG)"Scanning for devices...", TAG_END);
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

    /* gui_select_device(idx) fully resets edit state (model/dirty/sel/geo) and
       updates the buttons; pass -1 when there are no partitionable disks so we
       never leave stale selection/geometry enabling Delete/Edit/Init. */
    gui_select_device(n > 0 ? 0 : -1);
}

void gui_select_device(int idx)
{
    DeviceHandle *h;
    DiscDisk *d;
    int di, n;

    g_have_model = 0;
    g_dirty = 0;
    g_sel_part = -1;
    g_geo.has_media = 0;

    if (g_ndisks == 0 || idx < 0) {
        gui_refresh_parts();
        gui_update_buttons();
        return;
    }
    di = g_devmap[idx];
    d  = &g_disks[di];
    for (n = 0; n < 40 && d->driver[n]; n++) g_cur_driver[n] = d->driver[n];
    g_cur_driver[n] = 0;
    g_cur_unit = d->unit;

    h = dev_open(d->driver, d->unit);
    if (h) {
        dev_geometry(h, &g_geo);                       /* for Init + display */
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) g_have_model = 1;
        dev_close(h);
    }
    gui_refresh_parts();
    gui_update_buttons();
}

/* The disk-map bar rectangle (window-relative). Shared by draw + hit-test. */
static void gui_bar_rect(int *bx, int *by, int *bw, int *bh)
{
    *bx = 10 + g_leftb; *by = 26 + g_topb; *bw = 440; *bh = 16;
}

/* Return the partition index whose bar segment contains (mx,my), or -1. */
static int gui_part_at_x(int mx, int my)
{
    int bx, by, bw, bh, i;
    if (!g_have_model || g_model.cylinders == 0) return -1;
    gui_bar_rect(&bx, &by, &bw, &bh);
    if (mx < bx || mx >= bx + bw || my < by || my >= by + bh) return -1;
    for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
        RdbPartition *pt = &g_model.parts[i];
        int x0 = bx + (int)((pt->low_cyl      * (ULONG)bw) / g_model.cylinders);
        int x1 = bx + (int)(((pt->high_cyl+1) * (ULONG)bw) / g_model.cylinders);
        if (x1 <= x0) x1 = x0 + 1;
        if (mx >= x0 && mx < x1) return i;
    }
    return -1;
}

/* Select partition `idx` (or -1 = none): syncs the listview highlight, redraws
   the bar (with a selection outline), and updates the action buttons. */
static void gui_set_selection(int idx)
{
    g_sel_part = idx;
    if (g_win && g_gad[GID_PARTS])
        GT_SetGadgetAttrs(g_gad[GID_PARTS], g_win, 0,
                          GTLV_Selected, (ULONG)(idx >= 0 ? (ULONG)idx : ~0UL), TAG_END);
    gui_draw_bar();
    gui_update_buttons();
}

void gui_draw_bar(void)
{
    struct RastPort *rp;
    int bx, by, bw, bh, i;
    if (!g_win) return;
    rp = g_win->RPort;
    gui_bar_rect(&bx, &by, &bw, &bh);

    /* frame + unused background */
    SetAPen(rp, 1); RectFill(rp, bx, by, bx + bw - 1, by + bh - 1);
    SetAPen(rp, 2); Move(rp, bx, by); Draw(rp, bx + bw - 1, by);
    Draw(rp, bx + bw - 1, by + bh - 1); Draw(rp, bx, by + bh - 1); Draw(rp, bx, by);

    if (!g_have_model || g_model.cylinders == 0) return;

    /* each partition drawn proportional to its cylinder span; the selected one
       gets a contrasting outline so selection is clearly visible on the map. */
    for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
        RdbPartition *pt = &g_model.parts[i];
        int x0 = bx + (int)((pt->low_cyl  * (ULONG)bw) / g_model.cylinders);
        int x1 = bx + (int)(((pt->high_cyl + 1) * (ULONG)bw) / g_model.cylinders);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > bx + bw - 1) x1 = bx + bw - 1;
        SetAPen(rp, (UBYTE)(3 - (i & 1)));   /* alternate pens 3/2 */
        RectFill(rp, x0, by + 1, x1 - 1, by + bh - 2);
        if (i == g_sel_part) {
            SetAPen(rp, 1);                  /* black selection outline */
            Move(rp, x0, by + 1); Draw(rp, x1 - 1, by + 1);
            Draw(rp, x1 - 1, by + bh - 2); Draw(rp, x0, by + bh - 2); Draw(rp, x0, by + 1);
        }
    }
}
