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
#include <graphics/text.h>
#include <libraries/asl.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include "gui.h"
#include "discover.h"
#include "device.h"
#include "driver.h"
#include "rdb.h"

extern struct IntuitionBase *IntuitionBase;   /* opened in main.c */
struct Library *GadToolsBase = 0;
struct GfxBase *GfxBase = 0;
struct Library *AslBase = 0;
extern struct DosLibrary *DOSBase;   /* opened in startup.c (for AddPart) */

/* Gadget IDs */
enum { GID_DEVICE = 1, GID_SCAN, GID_DRIVER, GID_PARTS, GID_NEW, GID_DELETE,
       GID_EDIT, GID_INIT, GID_SAVE, GID_STATUS, GID_UNIT };
/* GID_DEVICE is the Driver cycle; GID_UNIT is the Unit cycle. */

/* Module state for one GUI session. */
static struct Screen  *g_scr;        /* screen we render on (pub or own) */
static struct Screen  *g_pub;        /* locked pubscreen, or NULL */
static struct Window  *g_win;
static APTR            g_vi;          /* VisualInfo */
static struct Gadget  *g_glist;      /* gadtools context list */
static struct Gadget  *g_gad[16];    /* gadget pointers by a small index */
static struct TextAttr g_font = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct TextAttr g_font_bold = { (STRPTR)"topaz.font", 8, FSF_BOLD, 0 };

/* Discovery + current selection (static: keep off the stack). */
static DiscDisk g_disks[DISC_MAX];
static int      g_ndisks;
static const char *g_drvlabels[DISC_MAX + 2];   /* Driver cycle: prompt + names */
static const char *g_unitlabels[DISC_MAX + 2];  /* Unit cycle: placeholder or u<n> size */
static char     g_unittext[DISC_MAX][24];        /* storage for unit-cycle labels */
static RdbModel g_model;
static int      g_have_model;
static int      g_dirty;            /* unsaved edits in g_model */
static DeviceInfo g_geo;            /* geometry of the selected device */
static char     g_cur_driver[40];   /* selected device driver/unit (for Save) */
static uint32_t g_cur_unit;
static int      g_sel_part = -1;    /* selected partition index, or -1 */
static int      g_unitmap[DISC_MAX + 1]; /* Unit cycle index -> g_disks index (-1 = none) */
static int      g_nunits;           /* number of real disk entries in the Unit cycle */
static char     g_target_driver[40];/* driver the Scan button will probe ("" = none) */
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
static void gui_load_driver(void);
static void gui_init_picker(void);            /* no-probe startup picker */
static void gui_select_driver(int drvIdx);    /* Driver cycle: pick a scan target (resets) */
static void gui_select_unit(int unitIdx);     /* Unit cycle: show that disk's partitions */
static void gui_scan_selected(void);          /* probe the target driver (Scan button) */
void gui_draw_bar(void);
static void gui_draw_partheader(void);    /* column headings above the listview */
static void gui_set_selection(int idx);   /* select partition `idx` (or -1) + redraw */
static int  gui_part_at_x(int mx, int my);/* partition under a window-relative point, or -1 */

/* Tiny string compare (freestanding: no libc strcmp). */
static int streq(const char *a, const char *b)
{ int i; for (i = 0; a[i] && a[i] == b[i]; i++) ; return a[i] == 0 && b[i] == 0; }

/* Set the status-line text. */
static void gui_status(const char *s)
{
    int p = 0;
    while (s[p] && p < (int)sizeof(g_statusbuf) - 1) { g_statusbuf[p] = s[p]; p++; }
    g_statusbuf[p] = 0;
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
}

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
    GT_SetGadgetAttrs(g_gad[GID_SCAN],   g_win, 0, GA_Disabled, (ULONG)(g_target_driver[0] == 0), TAG_END);
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
            int k;
            /* col 0 is a selection marker ('>' = selected) so the choice STICKS
               in the list on every OS version — GadTools GTLV_Selected highlight
               is V39+ only, a no-op on the V37 baseline. Fixed char columns so
               the heading (gui_draw_partheader) lines up:
               mark@0 #@2 Name@6 Type@18 Start@24 End@32 Size@40. */
            row[p++] = (i == g_sel_part) ? '>' : ' ';
            row[p++] = ' ';
            p += u2s(row + p, (ULONG)(i + 1));         s_pad(row, &p, 6);
            for (k = 0; pt->name[k] && k < 11; k++) row[p++] = pt->name[k];
            s_pad(row, &p, 18);
            s_cat(row, &p, "FFS");                     s_pad(row, &p, 24);
            p += u2s(row + p, pt->low_cyl);            s_pad(row, &p, 32);
            p += u2s(row + p, pt->high_cyl);           s_pad(row, &p, 40);
            { ULONG cyls = pt->high_cyl - pt->low_cyl + 1;
              ULONG mb = disc_blocks_to_mb(cyls * g_model.cyl_blocks, g_model.block_bytes);
              p += u2s(row + p, mb); s_cat(row, &p, "MB"); }
            row[p] = 0;
            g_partnodes[i].ln_Name = row;
            AddTail(&g_partlist, &g_partnodes[i]);
        }
        { int p = 0; s_cat(g_statusbuf, &p, g_dirty ? "Modified  " : "RDB OK  ");
          p += u2s(g_statusbuf + p, (ULONG)g_model.num_parts);
          s_cat(g_statusbuf, &p, " parts  ");
          p += u2s(g_statusbuf + p, g_model.cylinders); s_cat(g_statusbuf, &p, " cyl");
          g_statusbuf[p] = 0; }
    } else {
        int p = 0; s_cat(g_statusbuf, &p, "No RDB on this disk"); g_statusbuf[p] = 0;
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

    /* Driver cycle gadget (row 1). All ng positions are offset by the window
       border (g_leftb,g_topb) since gadget coords are relative to the origin. */
    ng.ng_TextAttr   = &g_font;
    ng.ng_VisualInfo = g_vi;
    ng.ng_LeftEdge = 70 + g_leftb;  ng.ng_TopEdge = 6 + g_topb;  ng.ng_Width = 236; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Driver:"; ng.ng_GadgetID = GID_DEVICE;
    ng.ng_Flags = 0;
    g_drvlabels[0] = "Select or load a driver"; g_drvlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_drvlabels, TAG_END);
    g_gad[GID_DEVICE] = g;

    /* Driver... button (load a .device from file via ASL). "..." signals it
       opens a file requester; widened to fit the 9-char ASCII label (Topaz
       cannot render a UTF-8 ellipsis). */
    ng.ng_LeftEdge = 312 + g_leftb; ng.ng_TopEdge = 6 + g_topb; ng.ng_Width = 84; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Driver..."; ng.ng_GadgetID = GID_DRIVER;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, (ULONG)(AslBase == 0), TAG_END);
    g_gad[GID_DRIVER] = g;

    /* Scan button (bold: it is the primary action — query the chosen driver). */
    ng.ng_TextAttr = &g_font_bold;
    ng.ng_LeftEdge = 400 + g_leftb; ng.ng_TopEdge = 6 + g_topb; ng.ng_Width = 56; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Scan"; ng.ng_GadgetID = GID_SCAN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_SCAN] = g;
    ng.ng_TextAttr = &g_font;            /* restore default for later gadgets */

    /* Unit cycle gadget (row 2): the disks found on the selected driver after a
       Scan. Starts disabled with a placeholder until a Scan populates it. */
    ng.ng_LeftEdge = 70 + g_leftb; ng.ng_TopEdge = 26 + g_topb; ng.ng_Width = 236; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Unit:"; ng.ng_GadgetID = GID_UNIT;
    g_unitlabels[0] = "(press Scan)"; g_unitlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_unitlabels,
                     GA_Disabled, TRUE, TAG_END);
    g_gad[GID_UNIT] = g;

    /* Partition listview (shifted down for the unit row; ~2 rows shorter so the
       window keeps its height). */
    ng.ng_LeftEdge = 10 + g_leftb; ng.ng_TopEdge = 74 + g_topb; ng.ng_Width = 440; ng.ng_Height = 66;
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
    AslBase = OpenLibrary("asl.library", 37);   /* optional: Driver... disabled if absent */

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
    gui_init_picker();     /* no-probe picker: prompt + driver names */
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
                    /* Guard against losing in-memory edits that were never saved. */
                    if (!g_dirty ||
                        gui_confirm("Quit HDPart", "Discard unsaved changes and quit?"))
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
                    if (gad->GadgetID == GID_DEVICE) gui_select_driver((int)code);
                    else if (gad->GadgetID == GID_UNIT) gui_select_unit((int)code);
                    else if (gad->GadgetID == GID_SCAN) gui_scan_selected();
                    else if (gad->GadgetID == GID_DRIVER) gui_load_driver();
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
    if (AslBase) { CloseLibrary(AslBase); AslBase = 0; }
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary(GadToolsBase);
    return 0;
}

/* Build the Driver cycle labels: prompt at 0, then the candidate driver names
   (curated + loaded). Returns the entry count. Labels point at static storage. */
static int gui_build_drivers(void)
{
    const char *cand[DISC_MAX];
    int nc = disc_candidate_drivers(cand, DISC_MAX), n = 0, j;
    g_drvlabels[n++] = "Select or load a driver";
    for (j = 0; j < nc && n < DISC_MAX + 1; j++) g_drvlabels[n++] = cand[j];
    g_drvlabels[n] = 0;
    return n;
}

/* Set the Unit cycle to a single placeholder with no real units (used on every
   driver change so a stale unit/size can never linger). */
static void gui_clear_units(const char *text)
{
    g_nunits = 0;
    g_unitlabels[0] = text; g_unitmap[0] = -1; g_unitlabels[1] = 0;
}

/* Build the Unit cycle from the disks in g_disks[] that belong to the target
   driver. Returns the number of real units; sets a placeholder when none. */
static int gui_build_units(const char *emptyText)
{
    int i, n = 0;
    for (i = 0; i < g_ndisks && n < DISC_MAX; i++) {
        DiscDisk *d = &g_disks[i];
        char *t = g_unittext[n];
        int p = 0;
        if (!d->partitionable || !streq(d->driver, g_target_driver)) continue;
        /* "Unit <n> (<size> MB)" */
        s_cat(t, &p, "Unit ");
        p += u2s(t + p, (ULONG)d->unit);
        s_cat(t, &p, " (");
        p += u2s(t + p, (ULONG)d->size_mb);
        s_cat(t, &p, " MB)");
        t[p] = 0;
        g_unitmap[n] = i;
        g_unitlabels[n] = t;
        n++;
    }
    g_nunits = n;
    if (n == 0) { g_unitlabels[0] = emptyText; g_unitmap[0] = -1; g_unitlabels[1] = 0; }
    else g_unitlabels[n] = 0;
    return n;
}

/* Push the Unit cycle labels to the gadget; disabled when there are no real
   units (only a placeholder). */
static void gui_set_unit_cycle(int active)
{
    if (g_win && g_gad[GID_UNIT])
        GT_SetGadgetAttrs(g_gad[GID_UNIT], g_win, 0,
                          GTCY_Labels, (ULONG)g_unitlabels,
                          GTCY_Active, (ULONG)active,
                          GA_Disabled, (ULONG)(g_nunits == 0),
                          TAG_END);
}

/* Driver cycle handler. ANY driver change fully resets state — model, edit
   flags, partition selection, current disk, AND the Unit cycle (back to its
   placeholder at index 0) — so nothing from the previous driver lingers. */
static void gui_select_driver(int idx)
{
    g_have_model = 0; g_dirty = 0; g_sel_part = -1; g_geo.has_media = 0;
    g_cur_driver[0] = 0; g_cur_unit = 0;

    if (idx <= 0) {                               /* the prompt */
        g_target_driver[0] = 0;
        gui_clear_units("(no driver)");
        gui_set_unit_cycle(0);
        gui_refresh_parts(); gui_update_buttons();
        gui_status("Select a driver, then Scan.");
        return;
    }
    { const char *nm = g_drvlabels[idx]; int k;
      for (k = 0; k < 39 && nm[k]; k++) g_target_driver[k] = nm[k];
      g_target_driver[k] = 0; }
    gui_clear_units("(press Scan)");
    gui_set_unit_cycle(0);
    gui_refresh_parts(); gui_update_buttons();
    gui_status("Press Scan to query driver.");
}

/* Unit cycle handler: show the selected disk's partitions (reads its RDB). */
static void gui_select_unit(int uidx)
{
    DeviceHandle *h; DiscDisk *d; int k;

    g_have_model = 0; g_dirty = 0; g_sel_part = -1; g_geo.has_media = 0;
    g_cur_driver[0] = 0;

    if (uidx < 0 || uidx >= g_nunits || g_unitmap[uidx] < 0) {
        gui_refresh_parts(); gui_update_buttons();
        return;
    }
    d = &g_disks[g_unitmap[uidx]];
    for (k = 0; k < 39 && d->driver[k]; k++) g_cur_driver[k] = d->driver[k];
    g_cur_driver[k] = 0;
    g_cur_unit = d->unit;
    h = dev_open(d->driver, d->unit);
    if (h) {
        dev_geometry(h, &g_geo);
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) g_have_model = 1;
        dev_close(h);
    }
    gui_refresh_parts(); gui_update_buttons();
}

/* Scan button: probe the target driver's units, fill the Unit cycle with the
   disks found, and auto-select the first (showing its partitions). */
static void gui_scan_selected(void)
{
    int real;
    if (!g_target_driver[0]) return;

    gui_status("Scanning...");
    discover_probe_driver(g_disks, &g_ndisks, DISC_MAX, g_target_driver);
    real = gui_build_units("(no disk found)");
    gui_set_unit_cycle(0);

    if (real > 0) {
        gui_select_unit(0);
    } else {
        char m[64]; int p = 0, q = 0; const char *a = "No disk found on ";
        g_have_model = 0; g_sel_part = -1; g_geo.has_media = 0; g_cur_driver[0] = 0;
        gui_refresh_parts(); gui_update_buttons();
        while (a[p]) { m[p] = a[p]; p++; }
        while (g_target_driver[q] && p < 62) m[p++] = g_target_driver[q++];
        m[p] = 0; gui_status(m);
    }
}

/* No-probe startup: build the Driver cycle, select the prompt (which clears the
   Unit cycle and shows guidance). Touches no hardware. */
static void gui_init_picker(void)
{
    g_ndisks = 0;
    g_target_driver[0] = 0;
    gui_build_drivers();
    if (g_win && g_gad[GID_DEVICE])
        GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0,
                          GTCY_Labels, (ULONG)g_drvlabels, GTCY_Active, 0, TAG_END);
    gui_select_driver(0);
}

/* Map a DRVL_* failure code to a user-facing message. */
static const char *drv_err_text(int code)
{
    switch (code) {
        case DRVL_ELOAD:      return "Could not load that file as a driver.";
        case DRVL_ENOROMTAG:  return "That file has no driver (no Resident tag).";
        case DRVL_ENOTDEVICE: return "That file is not a .device driver.";
        case DRVL_EINIT:      return "Driver loaded but failed to initialise.";
        default:              return "Could not load that driver.";
    }
}

/* Ask for a .device file, load it, add it to the Driver cycle, and select it.
   No probe — the user presses Scan to query it. */
static void gui_load_driver(void)
{
    struct FileRequester *fr;
    static char path[256];
    static char name[DRV_NAME_LEN];
    int rc, i, n, sel = -1;

    if (!AslBase) return;   /* button is disabled, but guard anyway */

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Select a device driver",
            ASLFR_InitialDrawer, (ULONG)"DEVS:",
            ASLFR_DoPatterns,    TRUE,
            ASLFR_InitialPattern,(ULONG)"#?.device",
            TAG_END);
    if (!fr) { gui_msg("Driver", "Could not open the file requester."); return; }

    if (!AslRequest(fr, 0)) { FreeAslRequest(fr); return; }   /* cancelled */

    /* Join drawer + file into path[] (AddPart inserts any needed separator). */
    { int j = 0; const char *d = (const char *)fr->fr_Drawer;
      while (d && d[j] && j < (int)sizeof(path) - 1) { path[j] = d[j]; j++; }
      path[j] = 0; }
    AddPart((STRPTR)path, (CONST_STRPTR)fr->fr_File, sizeof(path));
    FreeAslRequest(fr);

    rc = driver_load_file(path, name, sizeof(name));
    if (rc != DRVL_OK) { gui_msg("Driver", drv_err_text(rc)); return; }

    /* Register the driver and show it as a Driver-cycle entry — no probe yet. */
    disc_add_extra_driver(name);
    n = gui_build_drivers();
    for (i = 1; i < n; i++)
        if (streq(g_drvlabels[i], name)) { sel = i; break; }
    if (sel < 0) sel = 0;

    if (g_win && g_gad[GID_DEVICE])
        GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0,
                          GTCY_Labels, (ULONG)g_drvlabels,
                          GTCY_Active, (ULONG)sel, TAG_END);
    gui_select_driver(sel);
    gui_status("Driver loaded - press Scan.");
}

/* The disk-map bar rectangle (window-relative). Shared by draw + hit-test. */
static void gui_bar_rect(int *bx, int *by, int *bw, int *bh)
{
    *bx = 10 + g_leftb; *by = 48 + g_topb; *bw = 440; *bh = 16;
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
    /* Re-render the rows so the '>' marker moves to the new selection (sticky on
       V37). gui_refresh_parts also re-applies GTLV_Selected (V39 highlight) and
       redraws the disk-map bar outline. */
    gui_refresh_parts();
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

    gui_draw_partheader();   /* column headings, shown even for an empty list */

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
        /* 1-based partition number centered on the segment (matches the list's
           # column); drawn only when the slice is wide enough to fit the digits. */
        { char num[6]; int nl = u2s(num, (ULONG)(i + 1));
          int tw = nl * 8, segw = x1 - x0;
          if (segw >= tw + 2) {
              SetAPen(rp, 1);                /* black: contrasts pens 2/3 */
              Move(rp, x0 + (segw - tw) / 2, by + 11);
              Text(rp, (CONST_STRPTR)num, (LONG)nl);
          } }
    }
}

/* Column headings drawn just above the listview, aligned to the row columns
   built in gui_refresh_parts. Manual text (like the bar), so it is redrawn
   whenever the bar is. The left inset (+4) approximates GadTools' listview text
   origin; nudge it if the columns look a pixel or two off against the rows. */
static void gui_draw_partheader(void)
{
    struct RastPort *rp;
    static char hdr[48];
    int p = 0, lx, ly;
    if (!g_win) return;
    rp = g_win->RPort;
    s_pad(hdr, &p, 2);                            /* blank selection-marker column */
    s_cat(hdr, &p, "#");     s_pad(hdr, &p, 6);
    s_cat(hdr, &p, "Name");  s_pad(hdr, &p, 18);
    s_cat(hdr, &p, "Type");  s_pad(hdr, &p, 24);
    s_cat(hdr, &p, "Start"); s_pad(hdr, &p, 32);
    s_cat(hdr, &p, "End");   s_pad(hdr, &p, 40);
    s_cat(hdr, &p, "Size");  hdr[p] = 0;
    lx = 10 + g_leftb + 4;               /* match the listview's text inset */
    ly = 74 + g_topb - 2;                /* baseline just above the listview */
    SetAPen(rp, 1);
    Move(rp, lx, ly);
    Text(rp, (CONST_STRPTR)hdr, (LONG)p);
}
