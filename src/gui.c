/* HDPart GadTools GUI (read-only display in Plan 3a). */
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/execbase.h>
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
#include <graphics/modeid.h>      /* HIRES_KEY, INVALID_ID (own-screen display mode) */
#include <libraries/asl.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include "gui.h"
#include "discover.h"
#include "device.h"
#include "driver.h"
#include "rdb.h"
#include "format.h"
#include "safety.h"
#include "fsload.h"

extern struct IntuitionBase *IntuitionBase;   /* opened in main.c */
struct Library *GadToolsBase = 0;
struct GfxBase *GfxBase = 0;
struct Library *AslBase = 0;
extern struct DosLibrary *DOSBase;   /* opened in startup.c (for AddPart) */

/* Gadget IDs */
enum { GID_DEVICE = 1, GID_SCAN, GID_DRIVER, GID_PARTS, GID_NEW, GID_DELETE,
       GID_EDIT, GID_INIT, GID_SAVE, GID_STATUS, GID_UNIT, GID_SPLIT, GID_REFRESH,
       GID_RESIZE, GID_FORMAT, GID_FS };
/* GID_DEVICE is the Driver cycle; GID_UNIT is the Unit cycle. */

/* Module state for one GUI session. */
static struct Screen  *g_scr;        /* screen we render on (pub or own) */
static struct Screen  *g_pub;        /* locked pubscreen, or NULL */
static struct Window  *g_win;
static APTR            g_vi;          /* VisualInfo */
static struct Gadget  *g_glist;      /* gadtools context list */
static struct Gadget  *g_gad[17];    /* gadget pointers by a small index */
static struct TextAttr g_font = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct TextFont *g_sfont = 0;  /* explicit topaz 8 for our own screen (NULL if unavailable) */
static struct Menu    *g_menu = 0;   /* menu strip attached to g_win */

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
static int      g_cur_unitidx = -1; /* Unit cycle index currently shown (-1 = none): Refresh re-probes it */
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

/* Render a DosType to a short label: "DOS\3" / "PFS\3" / "SFS\0" when bytes 0-2
   are printable, else "0x........". out needs >= 12 bytes. */
static void dostype_label(char *out, uint32_t t)
{
    static const char hx[] = "0123456789ABCDEF";
    unsigned char b0 = (unsigned char)(t >> 24), b1 = (unsigned char)(t >> 16),
                  b2 = (unsigned char)(t >> 8),  b3 = (unsigned char)t;
    int p = 0;
    if (b0 >= 0x20 && b0 < 0x7F && b1 >= 0x20 && b1 < 0x7F && b2 >= 0x20 && b2 < 0x7F) {
        out[p++] = (char)b0; out[p++] = (char)b1; out[p++] = (char)b2; out[p++] = '\\';
        if (b3 < 10) out[p++] = (char)('0' + b3);
        else { out[p++] = hx[(b3 >> 4) & 0xF]; out[p++] = hx[b3 & 0xF]; }
    } else {
        int i; out[p++] = '0'; out[p++] = 'x';
        for (i = 0; i < 8; i++) out[p++] = hx[(t >> ((7 - i) * 4)) & 0xF];
    }
    out[p] = 0;
}

/* Friendly filesystem label for the list's Type column: "FFS"/"OFS" plus " Intl"
   and " DC" for a ROM DOS type, else the raw DosType. out needs >= 16 bytes. */
static void fstype_label(char *out, uint32_t t)
{
    if ((t & 0xFFFFFF00u) == 0x444F5300u && (t & 0xFFu) <= 7) {
        int p = 0;
        s_cat(out, &p, (t & 1u) ? "FFS" : "OFS");
        if (t & 2u) s_cat(out, &p, " Intl");
        if (t & 4u) s_cat(out, &p, " DC");
        out[p] = 0;
    } else {
        dostype_label(out, t);
    }
}

/* Forward decls. */
static void gui_about(void);
static void gui_load_driver(void);
static void gui_filesystems(void);
static void gui_split(void);                  /* quick split-into-N-equal dialog */
static void gui_init_picker(void);            /* no-probe startup picker */
static void gui_select_driver(int drvIdx);    /* Driver cycle: pick a scan target (resets) */
static void gui_select_unit(int unitIdx);     /* Unit cycle: show that disk's partitions */
static void gui_scan_selected(void);          /* probe the target driver (Scan button) */
static void gui_refresh_current(void);        /* re-probe the currently shown device+unit */
static int  gui_resize_dialog(int index);     /* grow/shrink the selected partition */
static void gui_format(int index);            /* format selected partition into empty volume */
static void gui_draw_chrome(void);        /* BevelBox group frames + captions */
void gui_draw_bar(void);
static void gui_draw_partheader(void);    /* column headings above the listview */
static void gui_draw_easter(void);        /* the bottom-right pi glyph */
static int  gui_hit_easter(int mx, int my);
static void gui_set_selection(int idx);   /* select partition `idx` (or -1) + redraw */
static int  gui_part_at_x(int mx, int my);/* partition under a window-relative point, or -1 */

/* Tiny string compare (freestanding: no libc strcmp). */
static int streq(const char *a, const char *b)
{ int i; for (i = 0; a[i] && a[i] == b[i]; i++) ; return a[i] == 0 && b[i] == 0; }

/* Set the status-line text. */
static void gui_status(const char *s)
{
    /* "Status: " is baked into the text (not a GadTools label) so the line's
       left edge aligns deterministically with the bevel content column. */
    static const char pre[] = "Status: ";
    int p = 0, i = 0;
    while (pre[p]) { g_statusbuf[p] = pre[p]; p++; }
    while (s[i] && p < (int)sizeof(g_statusbuf) - 1) { g_statusbuf[p++] = s[i++]; }
    g_statusbuf[p] = 0;
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
}

/* FULLMENUNUM indices (menu,item) for enable/disable + dispatch readability. */
enum { MN_PROJECT=0, MN_DISK=1, MN_PART=2 };
enum { IT_ABOUT=0, IT_SAVE=2, IT_QUIT=4 };               /* Project items */
enum { ID_SCAN=0, ID_LOAD=1, ID_REFRESH=3, ID_INIT=4, ID_FS=5 };  /* Disk items   */
enum { IP_NEW=0, IP_EDIT=1, IP_DELETE=2, IP_SPLIT=4, IP_RESIZE=5, IP_FORMAT=6 }; /* Partition items */

/* Enable/disable one menu item (menu,item) on the attached strip. */
static void gui_menu_enable(UWORD menu, UWORD item, int on)
{
    UWORD num = FULLMENUNUM(menu, item, NOSUB);
    if (!g_win || !g_menu) return;
    if (on) OnMenu(g_win, num); else OffMenu(g_win, num);
}

static void gui_update_buttons(void)
{
    int hasModel = g_have_model;
    int hasGeo   = g_geo.has_media;
    int hasSel   = (g_sel_part >= 0 && g_sel_part < g_model.num_parts);
    if (!g_win) return;
    GT_SetGadgetAttrs(g_gad[GID_SAVE],   g_win, 0, GA_Disabled, (ULONG)!(hasModel && g_dirty), TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_NEW],    g_win, 0, GA_Disabled, (ULONG)!hasModel, TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_SPLIT],  g_win, 0, GA_Disabled, (ULONG)!hasGeo,   TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_DELETE], g_win, 0, GA_Disabled, (ULONG)!hasSel,   TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_EDIT],   g_win, 0, GA_Disabled, (ULONG)!hasSel,   TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_SCAN],   g_win, 0, GA_Disabled, (ULONG)(g_target_driver[0] == 0), TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_REFRESH],g_win, 0, GA_Disabled, (ULONG)(g_cur_unitidx < 0), TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_RESIZE], g_win, 0, GA_Disabled, (ULONG)!hasSel, TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_FORMAT], g_win, 0, GA_Disabled, (ULONG)!hasSel, TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_FS],     g_win, 0, GA_Disabled, (ULONG)!hasModel, TAG_END);
    gui_menu_enable(MN_PROJECT, IT_SAVE,    hasModel && g_dirty);
    gui_menu_enable(MN_DISK,    ID_SCAN,    g_target_driver[0] != 0);
    gui_menu_enable(MN_DISK,    ID_REFRESH, g_cur_unitidx >= 0);
    gui_menu_enable(MN_DISK,    ID_INIT,    hasGeo);
    gui_menu_enable(MN_DISK,    ID_FS,      hasModel);
    gui_menu_enable(MN_PART,    IP_NEW,     hasModel);
    gui_menu_enable(MN_PART,    IP_EDIT,    hasSel);
    gui_menu_enable(MN_PART,    IP_DELETE,  hasSel);
    gui_menu_enable(MN_PART,    IP_SPLIT,   hasGeo);
    gui_menu_enable(MN_PART,    IP_RESIZE,  hasSel);
    gui_menu_enable(MN_PART,    IP_FORMAT,  hasSel);
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
               mark@0 #@2 Name@6 Type@18 Start@30 End@38 Size@46. */
            row[p++] = (i == g_sel_part) ? '>' : ' ';
            row[p++] = ' ';
            p += u2s(row + p, (ULONG)(i + 1));         s_pad(row, &p, 6);
            for (k = 0; pt->name[k] && k < 11; k++) row[p++] = pt->name[k];
            s_pad(row, &p, 18);
            { char tb[16]; fstype_label(tb, pt->dos_type); s_cat(row, &p, tb); }
            s_pad(row, &p, 30);
            p += u2s(row + p, pt->low_cyl);            s_pad(row, &p, 38);
            p += u2s(row + p, pt->high_cyl);           s_pad(row, &p, 46);
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

/* Screen-top menu strip. nm_CommKey letters are the right-Amiga shortcuts.
   Scan has none (S belongs to Save). Items map 1:1 to the existing handlers. */
static struct NewMenu g_newmenu[] = {
    { NM_TITLE, "Project",       0,  0, 0, 0 },
    {  NM_ITEM, "About HDPart...",0, 0, 0, (APTR)0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Save",         "S", 0, 0, 0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Quit",         "Q", 0, 0, 0 },
    { NM_TITLE, "Disk",          0,  0, 0, 0 },
    {  NM_ITEM, "Scan",          0,  0, 0, 0 },
    {  NM_ITEM, "Load Driver...","L",0, 0, 0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Refresh",      "F", 0, 0, 0 },
    {  NM_ITEM, "Init Disk...", "I", 0, 0, 0 },
    {  NM_ITEM, "Filesystems...", "Y", 0, 0, 0 },
    { NM_TITLE, "Partition",     0,  0, 0, 0 },
    {  NM_ITEM, "New",          "N", 0, 0, 0 },
    {  NM_ITEM, "Edit...",      "E", 0, 0, 0 },
    {  NM_ITEM, "Delete",       "D", 0, 0, 0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Split...",     "T", 0, 0, 0 },
    {  NM_ITEM, "Resize...",    "R", 0, 0, 0 },
    {  NM_ITEM, "Format...",    "O", 0, 0, 0 },
    { NM_END, 0, 0, 0, 0, 0 }
};
static struct Gadget *build_gadgets(void)
{
    struct NewGadget ng;
    struct Gadget *g;
    int i;
    for (i = 0; i < 17; i++) g_gad[i] = 0;

    /* CreateContext's inline macro (LP1) declares the input register as
       'volatile t1' which in GCC 14+ triggers -Wincompatible-pointer-types
       (now an error).  Suppress around this one call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&g_glist);
#pragma GCC diagnostic pop
    if (!g) return 0;

    /* Row 1 — Disk panel: Driver cycle + Load button */
    ng.ng_TextAttr   = &g_font;
    ng.ng_VisualInfo = g_vi;
    ng.ng_LeftEdge = 76 + g_leftb; ng.ng_TopEdge = 18 + g_topb; ng.ng_Width = 400; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Driver:"; ng.ng_GadgetID = GID_DEVICE; ng.ng_Flags = 0;
    g_drvlabels[0] = "Select or load a driver"; g_drvlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_drvlabels, TAG_END);
    g_gad[GID_DEVICE] = g;

    ng.ng_LeftEdge = 484 + g_leftb; ng.ng_TopEdge = 18 + g_topb; ng.ng_Width = 82; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"_Load..."; ng.ng_GadgetID = GID_DRIVER;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, (ULONG)(AslBase == 0), GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_DRIVER] = g;

    /* Row 2 — Disk panel: Unit cycle + Scan button (normal weight, single height) */
    ng.ng_LeftEdge = 76 + g_leftb; ng.ng_TopEdge = 36 + g_topb; ng.ng_Width = 400; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Unit:"; ng.ng_GadgetID = GID_UNIT;
    g_unitlabels[0] = "(press Scan)"; g_unitlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_unitlabels, GA_Disabled, TRUE, TAG_END);
    g_gad[GID_UNIT] = g;

    ng.ng_LeftEdge = 484 + g_leftb; ng.ng_TopEdge = 36 + g_topb; ng.ng_Width = 82; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Scan"; ng.ng_GadgetID = GID_SCAN;   /* no underscore: Scan has no shortcut */
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_SCAN] = g;

    /* Row 3 — Disk panel: Filesystems button (enabled only when g_have_model) */
    ng.ng_LeftEdge = 484 + g_leftb; ng.ng_TopEdge = 54 + g_topb; ng.ng_Width = 82; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"File_sys..."; ng.ng_GadgetID = GID_FS;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_FS] = g;

    /* Partition listview (inside Partitions panel, under bar+header; y shifted +18) */
    ng.ng_LeftEdge = 16 + g_leftb; ng.ng_TopEdge = 124 + g_topb; ng.ng_Width = 550; ng.ng_Height = 66;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_PARTS;
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, 0, GTLV_ShowSelected, 0, TAG_END);
    g_gad[GID_PARTS] = g;

    /* Partition-action toolbar (one row under the list) */
    {
        static const struct { int id; const char *txt; int x; int w; } pbtn[] = {
            { GID_NEW,    "_New",     16,  56 }, { GID_EDIT,   "_Edit...", 78,  72 },
            { GID_DELETE, "_Delete", 156,  72 }, { GID_SPLIT,  "Spli_t...",234, 74 },
            { GID_RESIZE, "_Resize...",314, 88 }, { GID_FORMAT, "F_ormat...",408, 92 }
        };
        int k;
        for (k = 0; k < 6; k++) {
            ng.ng_LeftEdge = pbtn[k].x + g_leftb; ng.ng_TopEdge = 192 + g_topb;
            ng.ng_Width = pbtn[k].w; ng.ng_Height = 14;
            ng.ng_GadgetText = (UBYTE *)pbtn[k].txt; ng.ng_GadgetID = pbtn[k].id;
            g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
            g_gad[pbtn[k].id] = g;
        }
    }

    /* Footer: status text (left, aligned with bevel content column) + Refresh + Save (right).
       No GadTools label — "Status: " is baked into the text by gui_status(). */
    ng.ng_LeftEdge = 16 + g_leftb; ng.ng_TopEdge = 214 + g_topb; ng.ng_Width = 374; ng.ng_Height = 12;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_STATUS;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"Status: no disk selected", TAG_END);
    g_gad[GID_STATUS] = g;

    ng.ng_LeftEdge = 396 + g_leftb; ng.ng_TopEdge = 212 + g_topb; ng.ng_Width = 82; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Re_fresh"; ng.ng_GadgetID = GID_REFRESH;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_REFRESH] = g;

    ng.ng_LeftEdge = 484 + g_leftb; ng.ng_TopEdge = 212 + g_topb; ng.ng_Width = 82; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"_Save"; ng.ng_GadgetID = GID_SAVE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_SAVE] = g;

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

/* Shared modal-dialog scaffolding. Each dialog supplies its own gadgets + body
   logic; these own the centering, the OpenWindowTags flags, the standard refresh
   response, and cleanup. */
static void dlg_center(int w, int h, int *left, int *top)
{
    int L = g_win->LeftEdge + (g_win->Width  - w) / 2;
    int T = g_win->TopEdge  + (g_win->Height - h) / 2;
    *left = L < 0 ? 0 : L;
    *top  = T < 0 ? 0 : T;
}
static struct Window *dlg_open(const char *title, int L, int T, int w, int h,
                               ULONG idcmp, struct Gadget *glist)
{
    return OpenWindowTags(0,
        WA_Left, L, WA_Top, T, WA_Width, w, WA_Height, h,
        WA_Title, (ULONG)title, WA_Gadgets, (ULONG)glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | idcmp,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
}
static void dlg_refresh(struct Window *w) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
static void dlg_close(struct Window *w, struct Gadget *glist)
{
    if (w) CloseWindow(w);
    if (glist) FreeGadgets(glist);
}

/* Modal requester centered over the main window. twoButtons=1 -> Proceed/Cancel
   (returns 1 for Proceed, 0 for Cancel/close); twoButtons=0 -> single OK. */
static int gui_request(const char *title, const char *body, int twoButtons,
                       const char *okLabel)
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
        ng.ng_LeftEdge = dl + (textW + 24 - 90) / 2;
        ng.ng_GadgetText = (UBYTE *)(okLabel ? okLabel : "OK"); ng.ng_GadgetID = 1;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (!g) { FreeGadgets(glist); return 0; }

    dwL = 0; dwT = 0; dlg_center(dwW, dwH, &dwL, &dwT);
    dw = dlg_open(title, dwL, dwT, dwW, dwH, BUTTONIDCMP, glist);
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
    dlg_close(dw, glist);
    return result;
}

static int  gui_confirm(const char *title, const char *body) { return gui_request(title, body, 1, 0); }
static void gui_msg(const char *title, const char *body)      { (void)gui_request(title, body, 0, 0); }

static char g_msgbuf[120];

static int gui_save(void)
{
    DeviceHandle *h;
    static RdbModel chk;          /* static: keep off the stack */
    int ok = 0, p = 0;

    if (!g_have_model) return 0;
    if (rdb_validate(&g_model) != RDB_OK) { gui_msg("Save", "Partition layout is invalid."); return 0; }

    {
        static char snames[8][8];
        int snn = 0;
        DevLiveness slv = safety_classify(g_cur_driver, g_cur_unit, snames, 8, &snn);
        if (slv == DEV_BOOT) {
            gui_msg("Save",
                    "You booted from this device.\n"
                    "Writing a new partition table now would\n"
                    "corrupt the running system. Boot from\n"
                    "floppy (or another disk) to modify it.");
            return 0;
        }
        if (slv == DEV_MOUNTED) {
            if (!gui_confirm("Save",
                    "This disk has mounted volumes. Writing a\n"
                    "new table may disturb them. Proceed?"))
                return 0;
        }
    }

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
    rdb_model_free(&g_model);
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


/* Write "0x" + 8 uppercase hex digits of v to o[<=11]; returns the length. */
static int u32_to_hex(char *o, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    o[0] = '0'; o[1] = 'x';
    for (i = 0; i < 8; i++) o[2 + i] = hx[(v >> ((7 - i) * 4)) & 0xF];
    o[10] = 0;
    return 10;
}

/* Parse a hex string (optional 0x/0X/$ prefix) into *out. Returns 1 on success
   (1..8 hex digits, nothing else), 0 on failure. Leading spaces are skipped. */
static int parse_hex32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int digits = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    else if (s[0] == '$') s += 1;
    for (; *s; s++) {
        char c = *s; int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        if (digits >= 8) return 0;            /* > 32 bits */
        v = (v << 4) | (uint32_t)d; digits++;
    }
    if (digits == 0) return 0;
    *out = v;
    return 1;
}

/* Edit partition `index` of g_model via a modal dialog. Returns 1 if applied. */
/* MaxTransfer / Mask presets for the editor cycles. The cycle shows the short
   NAME; the adjacent hex field always holds the effective value (filled+disabled
   for a preset, editable for Custom). Values/guidance from the captured EAB FAQ
   (docs/reference/amiga-disk-size-maxtransfer-mask.md). */
static const char *const kMaxTLabels[] = { "IDE", "SCSI", "No limit", "Custom", 0 };
static const uint32_t    kMaxTValues[] = { 0x0001FE00u, 0x00FFFFFFu, 0x7FFFFFFFu };
static const char *const kMaxTHelp[]   = {
    "0x1FE00: onboard IDE - required!",
    "0xFFFFFF: SCSI controllers",
    "0x7FFFFFFF: IDEfix/PFS3-AiO/PCMCIA",
    "Advanced - enter a hex value"
};
#define N_MAXT 3

static const char *const kMaskLabels[] = { "Default", "Zorro II DMA", "A1200/A4000", "No mask", "Custom", 0 };
static const uint32_t    kMaskValues[] = { 0x7FFFFFFEu, 0x00FFFFFFu, 0xFFFFFFFCu, 0xFFFFFFFFu };
static const char *const kMaskHelp[]   = {
    "DMA mask; OK as-is for plain IDE",
    "Zorro II DMA controller (24-bit)",
    "A1200/A4000 internal IDE",
    "No masking (Zorro III / no DMA)",
    "Advanced - enter a hex value"
};
#define N_MASK 4

/* Index of v among vals[0..n), or n (the 'Custom' slot) if it is not a preset. */
static int preset_index(const uint32_t *vals, int n, uint32_t v)
{
    int i; for (i = 0; i < n; i++) if (vals[i] == v) return i; return n;
}

/* React to a MaxTransfer/Mask cycle change: fill+disable the hex field for a
   preset, enable it for Custom, and update the help line. */
static void preset_apply(struct Window *dw, struct Gadget *field, struct Gadget *help,
                         int idx, const uint32_t *vals, int n, const char *const *helps)
{
    if (idx < n) {
        char buf[12]; u32_to_hex(buf, vals[idx]);
        GT_SetGadgetAttrs(field, dw, 0, GTST_String, (ULONG)buf, GA_Disabled, TRUE, TAG_END);
    } else {
        GT_SetGadgetAttrs(field, dw, 0, GA_Disabled, FALSE, TAG_END);
    }
    GT_SetGadgetAttrs(help, dw, 0, GTTX_Text, (ULONG)helps[idx], TAG_END);
}

static int gui_edit_dialog(int index)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g;
    struct Gadget *gName = 0, *gSize = 0, *gBoot = 0, *gPri = 0, *gMaxT = 0, *gMask = 0;
    struct Gadget *gMaxTCyc = 0, *gMaskCyc = 0, *gMaxTHelp = 0, *gMaskHelp = 0;
    struct Gadget *gFsCyc = 0, *gIntl = 0, *gCache = 0;
    struct NewGadget ng;
    RdbPartition *pt = &g_model.parts[index];
    static char nameBuf[32];
    static char sizeMaxLabel[24];
    static char maxtBuf[12], maskBuf[12];
    static const char *fsLabels[4 + RDB_MAX_FS];   /* FFS,OFS,[pool*8],[keep],NULL */
    static int fsPoolIdx[4 + RDB_MAX_FS];
    static char fsKeep[12];
    int maxtIdx, maskIdx;
    int fsRom, fsActive, fsIdx, fsKeepIdx, fsPoolBase, v39;
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
    u32_to_hex(maxtBuf, pt->maxtransfer);
    u32_to_hex(maskBuf, pt->mask);
    maxtIdx = preset_index(kMaxTValues, N_MAXT, pt->maxtransfer);
    maskIdx = preset_index(kMaskValues, N_MASK, pt->mask);

    /* Filesystem: ROM DOS\x decodes to FFS/OFS + Intl + DirCache bits; anything
       else (PFS3/SFS/custom) gets a read-only 'keep' entry that preserves it.
       DirCache is KS3.0+ (exec V39). */
    v39   = (SysBase->LibNode.lib_Version >= 39);
    fsRom = ((pt->dos_type & 0xFFFFFF00u) == 0x444F5300u) && ((pt->dos_type & 0xFFu) <= 7);
    { int n = 0, kk;
      fsLabels[n++] = "FFS";
      fsLabels[n++] = "OFS";
      /* Pool entries from the embedded-FS model (PFS3, SFS, etc.) */
      fsPoolBase = n;
      for (kk = 0; kk < g_model.num_fs && n < (int)(sizeof fsLabels / sizeof fsLabels[0]) - 2; kk++) {
          fsLabels[n] = g_model.fs[kk].name;
          fsPoolIdx[n] = kk;
          n++;
      }
      /* keep entry: only if not a ROM DOS\x AND not already in pool */
      if (!fsRom) {
          int inPool = 0, kn;
          for (kn = fsPoolBase; kn < n; kn++) {
              if (g_model.fs[fsPoolIdx[kn]].dos_type == pt->dos_type) { inPool = 1; break; }
          }
          if (!inPool) { dostype_label(fsKeep, pt->dos_type); fsKeepIdx = n; fsLabels[n++] = fsKeep; }
          else fsKeepIdx = -1;
      } else { fsKeepIdx = -1; }
      fsLabels[n] = 0; }
    /* Initial selection: prefer pool match, then ROM FFS/OFS, then keep */
    { int kn, matched = 0;
      for (kn = fsPoolBase; kn < fsPoolBase + g_model.num_fs; kn++) {
          if (g_model.fs[fsPoolIdx[kn]].dos_type == pt->dos_type) {
              fsActive = kn; matched = 1; break;
          }
      }
      if (!matched) {
          if (fsRom) fsActive = (pt->dos_type & 1u) ? 0 : 1;  /* FFS=0, OFS=1 */
          else       fsActive = fsKeepIdx;                     /* keep entry */
      }
    }
    fsIdx = fsActive;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&dglist);
#pragma GCC diagnostic pop
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 6; ng.ng_Width = 180; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Name"; ng.ng_GadgetID = 1;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)nameBuf, GTST_MaxChars, 31, TAG_END);
    gName = g;

    /* "max <N> MB" hint shown beside the size field */
    { int p = 0; sizeMaxLabel[p++]='m'; sizeMaxLabel[p++]='a'; sizeMaxLabel[p++]='x'; sizeMaxLabel[p++]=' ';
      p += u2s(sizeMaxLabel + p, maxMB); sizeMaxLabel[p++]=' '; sizeMaxLabel[p++]='M'; sizeMaxLabel[p++]='B';
      sizeMaxLabel[p] = 0; }

    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 26; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Size (MB)"; ng.ng_GadgetID = 2;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, curMB, GTIN_MaxChars, 7, TAG_END);
    gSize = g;

    ng.ng_LeftEdge = dl + 200; ng.ng_Width = 110; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)sizeMaxLabel, TAG_END);

    /* FS row: FFS/OFS (+ 'keep') cycle, plus Intl and DirCache checkboxes. */
    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 48; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"FS"; ng.ng_GadgetID = 4;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)fsLabels, GTCY_Active, (ULONG)fsActive, TAG_END);
    gFsCyc = g;

    ng.ng_LeftEdge = dl + 236; ng.ng_TopEdge = dt + 48; ng.ng_Width = 26; ng.ng_Height = 11;
    ng.ng_GadgetText = (UBYTE *)"Intl"; ng.ng_GadgetID = 14;
    { int ckDis = !fsRom || (fsActive >= fsPoolBase);
      g = CreateGadget(CHECKBOX_KIND, g, &ng,
                       GTCB_Checked, (ULONG)((fsRom && (pt->dos_type & 2u)) ? TRUE : FALSE),
                       GA_Disabled,  (ULONG)ckDis, TAG_END); }
    gIntl = g;

    ng.ng_LeftEdge = dl + 320; ng.ng_TopEdge = dt + 48; ng.ng_Width = 26; ng.ng_Height = 11;
    ng.ng_GadgetText = (UBYTE *)"Cache"; ng.ng_GadgetID = 15;
    { int ckDis = !fsRom || !v39 || (fsActive >= fsPoolBase);
      g = CreateGadget(CHECKBOX_KIND, g, &ng,
                       GTCB_Checked, (ULONG)((fsRom && v39 && (pt->dos_type & 4u)) ? TRUE : FALSE),
                       GA_Disabled,  (ULONG)ckDis, TAG_END); }
    gCache = g;

    /* Bootable (checkbox) + Boot Pri (integer) share a row. */
    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 70; ng.ng_Width = 26; ng.ng_Height = 11;
    ng.ng_GadgetText = (UBYTE *)"Bootable"; ng.ng_GadgetID = 5;
    g = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, (ULONG)(pt->bootable ? TRUE : FALSE), TAG_END);
    gBoot = g;

    ng.ng_LeftEdge = dl + 230; ng.ng_TopEdge = dt + 70; ng.ng_Width = 50; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Boot Pri"; ng.ng_GadgetID = 6;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, (ULONG)(LONG)pt->boot_pri, GTIN_MaxChars, 6, TAG_END);
    gPri = g;

    /* MaxTransfer: cycle (preset names) + hex value field + help line. */
    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 92; ng.ng_Width = 120; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"MaxTransfer"; ng.ng_GadgetID = 7;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)kMaxTLabels, GTCY_Active, (ULONG)maxtIdx, TAG_END);
    gMaxTCyc = g;
    ng.ng_LeftEdge = dl + 236; ng.ng_TopEdge = dt + 92; ng.ng_Width = 100; ng.ng_GadgetText = 0; ng.ng_GadgetID = 12;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)maxtBuf, GTST_MaxChars, 11,
                     GA_Disabled, (ULONG)(maxtIdx < N_MAXT), TAG_END);
    gMaxT = g;
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 110; ng.ng_Width = 300; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)kMaxTHelp[maxtIdx], TAG_END);
    gMaxTHelp = g;

    /* Mask: cycle (controller presets) + hex value field + help line. */
    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 128; ng.ng_Width = 120; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Mask"; ng.ng_GadgetID = 8;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)kMaskLabels, GTCY_Active, (ULONG)maskIdx, TAG_END);
    gMaskCyc = g;
    ng.ng_LeftEdge = dl + 236; ng.ng_TopEdge = dt + 128; ng.ng_Width = 100; ng.ng_GadgetText = 0; ng.ng_GadgetID = 13;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)maskBuf, GTST_MaxChars, 11,
                     GA_Disabled, (ULONG)(maskIdx < N_MASK), TAG_END);
    gMask = g;
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 146; ng.ng_Width = 300; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)kMaskHelp[maskIdx], TAG_END);
    gMaskHelp = g;

    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 168; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 270; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return 0; }

    {   int dwW = dl + 350 + g_scr->WBorRight;
        int dwH = dt + 190 + g_scr->WBorBottom;
        int dwL = 0, dwT = 0;
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open("Edit Partition", dwL, dwT, dwW, dwH,
                      BUTTONIDCMP | STRINGIDCMP | INTEGERIDCMP | CHECKBOXIDCMP | CYCLEIDCMP, dglist);
    }
    if (!dw) { FreeGadgets(dglist); return 0; }
    GT_RefreshWindow(dw, 0);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            UWORD code = im->Code;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) { dlg_refresh(dw); }
            else if (cl == IDCMP_GADGETUP) {
                if (ig == gMaxTCyc) {
                    preset_apply(dw, gMaxT, gMaxTHelp, (int)code, kMaxTValues, N_MAXT, kMaxTHelp);
                } else if (ig == gMaskCyc) {
                    preset_apply(dw, gMask, gMaskHelp, (int)code, kMaskValues, N_MASK, kMaskHelp);
                } else if (ig == gFsCyc) {
                    int keep = (fsKeepIdx >= 0 && (int)code == fsKeepIdx);
                    int pool = (!keep && (int)code >= fsPoolBase);
                    fsIdx = (int)code;
                    GT_SetGadgetAttrs(gIntl,  dw, 0, GA_Disabled, (ULONG)(keep || pool), TAG_END);
                    GT_SetGadgetAttrs(gCache, dw, 0, GA_Disabled, (ULONG)(keep || pool || !v39), TAG_END);
                } else if (ig == gSize) {
                    /* clamp the typed size to [1, maxMB] */
                    LONG v = ((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    if (v < 1) v = 1;
                    if ((ULONG)v > maxMB) v = (LONG)maxMB;
                    GT_SetGadgetAttrs(gSize, dw, 0, GTIN_Number, (ULONG)v, TAG_END);
                } else if (ig->GadgetID == 10) {                                /* Ok */
                    LONG mb = ((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    char *nm = (char *)((struct StringInfo *)gName->SpecialInfo)->Buffer;
                    const char *mt = (const char *)((struct StringInfo *)gMaxT->SpecialInfo)->Buffer;
                    const char *mk = (const char *)((struct StringInfo *)gMask->SpecialInfo)->Buffer;
                    uint32_t mtv, mkv;
                    int r;
                    if (!parse_hex32(mt, &mtv) || !parse_hex32(mk, &mkv)) {
                        gui_msg("Edit", "MaxTransfer / Mask must be hex (e.g. 0x7FFFFFFE).");
                    } else {
                        /* Compute the DosType from the FS controls (keep entry
                           preserves a non-ROM type). */
                        uint32_t fdt;
                        if (fsKeepIdx >= 0 && fsIdx == fsKeepIdx) {
                            fdt = pt->dos_type;
                        } else if (fsIdx >= fsPoolBase && (fsKeepIdx < 0 || fsIdx < fsKeepIdx)) {
                            fdt = g_model.fs[fsPoolIdx[fsIdx]].dos_type;  /* embedded FS */
                        } else {
                            fdt = 0x444F5300u;
                            if (fsIdx == 0)                            fdt |= 1u;  /* FFS */
                            if (gIntl->Flags & GFLG_SELECTED)          fdt |= 2u;  /* Intl */
                            if (v39 && (gCache->Flags & GFLG_SELECTED)) fdt |= 4u; /* DirCache */
                        }
                        if (mb < 1) mb = 1;
                        if ((ULONG)mb > maxMB) mb = (LONG)maxMB;
                        /* If the MB field is unchanged from what the dialog opened
                           with, keep the EXACT cylinder range (so a gap-filling
                           partition isn't silently shrunk by MB re-rounding); only
                           resize when the user actually changed the size. */
                        if ((uint32_t)mb == curMB)
                            r = rdb_rename_partition(&g_model, index, nm, fdt);
                        else
                            r = rdb_set_partition(&g_model, index, nm, (uint32_t)mb, fdt);
                        if (r == RDB_OK) {
                            /* Apply the flag fields directly (resize/rename keep
                               them; they don't affect geometry/validation). */
                            pt->bootable    = (gBoot->Flags & GFLG_SELECTED) ? 1 : 0;
                            pt->boot_pri    = (int32_t)((struct StringInfo *)gPri->SpecialInfo)->LongInt;
                            pt->maxtransfer = mtv;
                            pt->mask        = mkv;
                            applied = 1; done = 1;
                        } else {
                            gui_msg("Edit", "Invalid name or size (overlaps / out of range).");
                        }
                    }
                } else if (cl == IDCMP_GADGETUP && ig->GadgetID == 11) { done = 1; } /* Cancel */
            }
        }
    }

    dlg_close(dw, dglist);
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
    int idx, prev_sel, prev_dirty;
    if (!g_have_model) return;
    if (!rdb_largest_free_gap(&g_model, &gs, &ge)) {
        gui_msg("New Partition", "No free space on this disk."); return;
    }
    gui_auto_name(name);
    /* Fill the whole free gap exactly (by cylinder) — converting to MB and back
       would floor away the fractional cylinders and waste space. */
    idx = rdb_add_partition_cyl(&g_model, name, gs, ge, RDB_DOSTYPE_FFS_INTL);
    if (idx < 0) { gui_msg("New Partition", "Could not add the partition."); return; }
    /* The partition must exist in the model for the editor to size/name it, but
       it isn't committed until the user accepts. Remember the prior state so a
       Cancel removes the just-added partition instead of leaving it behind.
       rdb_add_partition_cyl appends, so idx is the last entry and prior indices
       are unaffected. */
    prev_sel = g_sel_part;
    prev_dirty = g_dirty;
    g_sel_part = idx;
    gui_refresh_parts();
    if (gui_edit_dialog(idx)) {      /* Ok -> keep it (dialog set g_dirty) */
        g_dirty = 1;
    } else {                         /* Cancel -> undo the add */
        rdb_delete_partition(&g_model, idx);
        g_sel_part = prev_sel;
        g_dirty = prev_dirty;
    }
    gui_refresh_parts();
    gui_update_buttons();
}

/* "N x ~<size> MB each" preview for the Split dialog. */
static void gui_split_preview(char *o, int n, uint32_t span,
                              uint32_t cylBlocks, uint32_t blockBytes)
{
    int p = 0;
    uint32_t perCyls = (n > 0) ? span / (uint32_t)n : 0;
    uint32_t mb = rdb_cyls_to_mb(perCyls, cylBlocks, blockBytes);
    p += u2s(o + p, (ULONG)n);
    s_cat(o, &p, " x ~");
    p += u2s(o + p, (ULONG)mb);
    s_cat(o, &p, " MB each");
    o[p] = 0;
}

/* Null-terminated decimal, for the Split stepper's number display. */
static void numstr(char *o, int v) { int ln = u2s(o, (ULONG)v); o[ln] = 0; }

/* Resize the selected partition: Size (MB) + anchor (Move End / Move Start)
   -> a new cylinder range, applied via rdb_resize_cyl. Confirms only when the
   change is destructive (shrink, or any Start-edge move). Returns 1 if applied
   (caller need not act — the dialog refreshes the view itself), else 0. */
static int gui_resize_dialog(int index)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g, *gAnchor = 0, *gSize = 0, *gMaxHint = 0, *gRead = 0;
    struct NewGadget ng;
    RdbPartition *pt;
    int dt = g_topb, dl = g_leftb;
    int done = 0, applied = 0, anchor = 0;          /* 0 = Move End, 1 = Move Start */
    uint32_t oldLow, oldHigh, cylBlocks, blockBytes;
    uint32_t gapAfter, gapBefore, maxCyls, maxMB, curMB, n;
    static const char *anchorLabels[3];
    static char maxBuf[24], readBuf[56];

    if (!g_have_model || index < 0 || index >= g_model.num_parts) return 0;
    pt = &g_model.parts[index];
    oldLow = pt->low_cyl; oldHigh = pt->high_cyl;
    cylBlocks = g_model.cyl_blocks; blockBytes = g_model.block_bytes;
    gapAfter  = rdb_gap_end_after(&g_model, index);     /* exclusive */
    gapBefore = rdb_gap_start_before(&g_model, index);  /* inclusive */

    /* current size + the max for the default (Move End) anchor */
    curMB  = rdb_cyls_to_mb(oldHigh - oldLow + 1, cylBlocks, blockBytes);
    if (curMB < 1) curMB = 1;
    maxCyls = gapAfter - oldLow;                        /* Move End: low fixed */
    maxMB   = rdb_cyls_to_mb(maxCyls, cylBlocks, blockBytes);
    if (maxMB < 1) maxMB = 1;
    if (curMB > maxMB) curMB = maxMB;
    n = curMB;

    anchorLabels[0] = "Move: End edge";
    anchorLabels[1] = "Move: Start edge";
    anchorLabels[2] = 0;

    { int p = 0; s_cat(maxBuf, &p, "max "); p += u2s(maxBuf + p, maxMB);
      s_cat(maxBuf, &p, " MB"); maxBuf[p] = 0; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&dglist);
#pragma GCC diagnostic pop
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    /* anchor cycle */
    ng.ng_LeftEdge = dl + 100; ng.ng_TopEdge = dt + 6; ng.ng_Width = 230; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Anchor"; ng.ng_GadgetID = 1;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)anchorLabels, GTCY_Active, 0, TAG_END);
    gAnchor = g;

    /* size (MB) */
    ng.ng_LeftEdge = dl + 100; ng.ng_TopEdge = dt + 26; ng.ng_Width = 90; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Size (MB)"; ng.ng_GadgetID = 2;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, (ULONG)n, GTIN_MaxChars, 7, TAG_END);
    gSize = g;

    /* max hint */
    ng.ng_LeftEdge = dl + 200; ng.ng_Width = 130; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)maxBuf, TAG_END);
    gMaxHint = g;

    /* live cyl readout (filled by RZ_RECOMPUTE below) */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 46; ng.ng_Width = 320; ng.ng_Height = 12;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"", TAG_END);
    gRead = g;

    /* persistent caveat */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 62; ng.ng_Width = 360; ng.ng_Height = 12;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text,
                     (ULONG)"NOTE: Resizing partitions implies data loss.", TAG_END);

    /* Ok / Cancel (Cancel right-aligned, equal padding to Ok) */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 80; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 300; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return 0; }

    {   int dwW = dl + 380 + g_scr->WBorRight;
        int dwH = dt + 102 + g_scr->WBorBottom;
        int dwL = 0, dwT = 0;
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open("Resize partition", dwL, dwT, dwW, dwH,
                      BUTTONIDCMP | INTEGERIDCMP | CYCLEIDCMP, dglist);
    }
    if (!dw) { FreeGadgets(dglist); return 0; }
    GT_RefreshWindow(dw, 0);

    /* helper to recompute max + clamp n + refresh the readout for the current anchor */
#define RZ_RECOMPUTE() do {                                                       \
        if (anchor == 0) maxCyls = gapAfter - oldLow;                             \
        else             maxCyls = oldHigh - gapBefore + 1;                       \
        maxMB = rdb_cyls_to_mb(maxCyls, cylBlocks, blockBytes);                   \
        if (maxMB < 1) maxMB = 1;                                                 \
        if (n < 1) n = 1;                                                        \
        if (n > maxMB) n = maxMB;                                                 \
        { int p = 0; s_cat(maxBuf, &p, "max "); p += u2s(maxBuf + p, maxMB);      \
          s_cat(maxBuf, &p, " MB"); maxBuf[p] = 0; }                              \
        GT_SetGadgetAttrs(gMaxHint, dw, 0, GTTX_Text, (ULONG)maxBuf, TAG_END);    \
        GT_SetGadgetAttrs(gSize, dw, 0, GTIN_Number, (ULONG)n, TAG_END);          \
        { uint32_t cyls = rdb_mb_to_cyls(n, cylBlocks, blockBytes);              \
          uint32_t nl, nh; int p = 0;                                            \
          if (cyls < 1) cyls = 1;                                                 \
          if (cyls > maxCyls) cyls = maxCyls; /* ceil(mb) can overshoot the gap */\
          if (anchor == 0) { nl = oldLow;  nh = oldLow + cyls - 1; }              \
          else             { nh = oldHigh; nl = oldHigh - cyls + 1; }            \
          s_cat(readBuf, &p, "-> cyls "); p += u2s(readBuf + p, nl);             \
          s_cat(readBuf, &p, ".."); p += u2s(readBuf + p, nh);                   \
          s_cat(readBuf, &p, "  ("); p += u2s(readBuf + p, n);                   \
          s_cat(readBuf, &p, " MB)"); readBuf[p] = 0; }                          \
        GT_SetGadgetAttrs(gRead, dw, 0, GTTX_Text, (ULONG)readBuf, TAG_END);      \
    } while (0)

    RZ_RECOMPUTE();                      /* fill the initial readout */
    ActivateGadget(gSize, dw, 0);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            UWORD code = im->Code;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) { dlg_refresh(dw); }
            else if (cl == IDCMP_GADGETUP) {
                if (ig == gAnchor) { anchor = (int)code; RZ_RECOMPUTE(); }
                else if (ig == gSize) {
                    n = (uint32_t)((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    RZ_RECOMPUTE();
                } else if (ig->GadgetID == 10) {                 /* Ok */
                    uint32_t cyls = rdb_mb_to_cyls(n, cylBlocks, blockBytes);
                    uint32_t nl, nh; int destructive, go = 1;
                    if (cyls < 1) cyls = 1;
                    if (cyls > maxCyls) cyls = maxCyls;  /* ceil(mb) can overshoot the gap */
                    if (anchor == 0) { nl = oldLow;  nh = oldLow + cyls - 1; }
                    else             { nh = oldHigh; nl = oldHigh - cyls + 1; }
                    if (nl == oldLow && nh == oldHigh) { done = 1; break; }   /* no change */
                    destructive = (nl != oldLow) || (nh < oldHigh);
                    if (destructive)
                        go = gui_request("Resize partition",
                                "Resizing changes the partition's extent.\n"
                                "Reformat afterward - existing data will be lost.",
                                1, "Proceed");
                    if (go) {
                        int r = rdb_resize_cyl(&g_model, index, nl, nh);
                        if (r == RDB_OK) {
                            g_dirty = 1; applied = 1; done = 1;
                        } else {
                            gui_msg("Resize", "Could not resize (overlaps / out of range).");
                        }
                    }
                } else if (ig->GadgetID == 11) { done = 1; }     /* Cancel */
            }
        }
    }
#undef RZ_RECOMPUTE
    dlg_close(dw, dglist);
    if (applied) { gui_refresh_parts(); gui_update_buttons(); }
    return applied;
}

/* Format the selected partition into an empty volume, live (no reboot).
   Preconditions: saved (not dirty), not the boot device, name free, ROM FFS. */
static void gui_format(int index)
{
    static char names[8][8];
    static char vol[36];
    static char msg[256];
    RdbPartition *pt;
    DevLiveness lv;
    int nn = 0, i, p;

    if (!g_have_model || index < 0 || index >= g_model.num_parts) return;
    pt = &g_model.parts[index];

    /* Save-first: the partition must persist before we format it. */
    if (g_dirty) {
        gui_msg("Format", "Save the partition table first,\nthen format.");
        return;
    }

    /* Boot/mount safety on the physical device HDPart is editing. */
    lv = safety_classify(g_cur_driver, g_cur_unit, names, 8, &nn);
    if (lv == DEV_BOOT) {
        gui_msg("Format",
                "You booted from this device.\n"
                "Formatting it now would corrupt the\n"
                "running system. Boot from floppy (or\n"
                "another disk) to format this one.");
        return;
    }
    if (lv == DEV_MOUNTED) {
        /* Build a warning listing mounted volumes, require explicit confirm. */
        p = 0;
        s_cat(msg, &p, "This disk has mounted volumes (");
        for (i = 0; i < nn && p < 180; i++) {
            if (i) { msg[p++] = ','; msg[p++] = ' '; }
            s_cat(msg, &p, names[i]);
        }
        s_cat(msg, &p, ").\nFormatting may destroy data. Proceed?");
        msg[p] = 0;
        if (!gui_confirm("Format", msg)) return;
    } else {
        /* DEV_CLEAR: still destructive. */
        if (!gui_confirm("Format",
                "Format this partition into an empty\n"
                "volume? All data on it is lost.")) return;
    }

    /* Default volume label = partition name. */
    for (i = 0; i < 35 && pt->name[i]; i++) vol[i] = pt->name[i];
    vol[i] = 0;

    switch (format_partition(g_cur_driver, g_cur_unit, &g_model, index, vol)) {
    case FMT_OK:
        p = 0;
        s_cat(msg, &p, "Formatted ");
        s_cat(msg, &p, pt->name);
        s_cat(msg, &p, ": as an empty volume.");
        msg[p] = 0;
        gui_status(msg);
        break;
    case FMT_ERR_NO_HANDLER:
        gui_msg("Format", "No filesystem handler for this type.\nNon-ROM filesystems need embedded-FS\nsupport (not available yet).");
        break;
    case FMT_ERR_RANGE:
    case FMT_ERR_MAKENODE:
    case FMT_ERR_ADDNODE:
    case FMT_ERR_FORMAT:
    default:
        gui_msg("Format", "Could not format the partition.");
        break;
    }
}

/* Quick partitioning: split the largest free gap into N equal partitions,
   keeping existing ones (on a blank disk the gap is the whole disk). */
static void gui_split(void)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g, *gNum = 0, *gPrev = 0;
    struct NewGadget ng;
    int dt = g_topb, dl = g_leftb;
    int done = 0, applied = 0, n = 4, slots;
    uint32_t cylBlocks, blockBytes, gs, ge, span, maxN;
    static char prevBuf[40];
    static char numBuf[8];

    if (!g_geo.has_media || g_geo.cylinders == 0) {
        gui_msg("Split", "No media / geometry on this device."); return;
    }
    /* Free gap + geometry for the preview, without touching the model yet. */
    if (g_have_model) {
        if (!rdb_largest_free_gap(&g_model, &gs, &ge)) {
            gui_msg("Split", "No free space on this disk."); return;
        }
        cylBlocks = g_model.cyl_blocks; blockBytes = g_model.block_bytes;
        slots = RDB_MAX_PARTS - g_model.num_parts;
    } else {                                   /* blank disk: whole partitionable range */
        gs = RDB_RESERVED_CYLS; ge = g_geo.cylinders - 1;
        cylBlocks = (uint32_t)g_geo.heads * (uint32_t)g_geo.sectors;
        blockBytes = g_geo.block_bytes;
        slots = RDB_MAX_PARTS;
    }
    span = (ge >= gs) ? (ge - gs + 1) : 0;
    if (span == 0 || slots < 1) { gui_msg("Split", "No room to add partitions."); return; }
    maxN = (span < (uint32_t)slots) ? span : (uint32_t)slots;
    if (maxN > 20) maxN = 20;        /* nobody sensibly makes more than 20 partitions */
    if ((uint32_t)n > maxN) n = (int)maxN;
    gui_split_preview(prevBuf, n, span, cylBlocks, blockBytes);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&dglist);
#pragma GCC diagnostic pop
    if (!g) return;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    /* "Partitions" label */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 6; ng.ng_Width = 84; ng.ng_Height = 14;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"Partitions", TAG_END);

    /* [<] number [>] steppers — no free-text field, so it can't be cleared or
       get stuck; the count is hard-bounded to 1..maxN (maxN capped at 20). */
    ng.ng_LeftEdge = dl + 100; ng.ng_TopEdge = dt + 4; ng.ng_Width = 26; ng.ng_Height = 16;
    ng.ng_GadgetText = (UBYTE *)"<"; ng.ng_GadgetID = 2;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    numstr(numBuf, n);
    ng.ng_LeftEdge = dl + 132; ng.ng_TopEdge = dt + 6; ng.ng_Width = 44; ng.ng_Height = 14;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)numBuf, GTTX_Border, TRUE, TAG_END);
    gNum = g;

    ng.ng_LeftEdge = dl + 182; ng.ng_TopEdge = dt + 4; ng.ng_Width = 26; ng.ng_Height = 16;
    ng.ng_GadgetText = (UBYTE *)">"; ng.ng_GadgetID = 3;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 26; ng.ng_Width = 270; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)prevBuf, TAG_END);
    gPrev = g;

    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 48; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 200; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return; }

    {   int dwW = dl + 290 + g_scr->WBorRight;
        int dwH = dt + 70 + g_scr->WBorBottom;
        int dwL = 0, dwT = 0;
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open("Split remaining free space", dwL, dwT, dwW, dwH,
                      BUTTONIDCMP | INTEGERIDCMP, dglist);
    }
    if (!dw) { FreeGadgets(dglist); return; }
    GT_RefreshWindow(dw, 0);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) { dlg_refresh(dw); }
            else if (cl == IDCMP_GADGETUP) {
                int id = ig->GadgetID;
                if (id == 2 || id == 3) {                        /* < / > steppers */
                    if (id == 2 && n > 1) n--;
                    else if (id == 3 && (uint32_t)n < maxN) n++;
                    numstr(numBuf, n);
                    GT_SetGadgetAttrs(gNum, dw, 0, GTTX_Text, (ULONG)numBuf, TAG_END);
                    gui_split_preview(prevBuf, n, span, cylBlocks, blockBytes);
                    GT_SetGadgetAttrs(gPrev, dw, 0, GTTX_Text, (ULONG)prevBuf, TAG_END);
                } else if (id == 10) { applied = 1; done = 1; }  /* Ok */
                else if (id == 11) { done = 1; }                 /* Cancel */
            }
        }
    }
    dlg_close(dw, dglist);
    if (!applied) return;

    /* Additive: split the free gap, keeping existing partitions. On a blank disk,
       create the empty table first (the gap is the whole partitionable range). */
    if (!g_have_model) {
        rdb_model_free(&g_model);
        rdb_init_model(&g_model, g_geo.cylinders, g_geo.heads, g_geo.sectors);
        g_have_model = 1;
    }
    if (rdb_split_range(&g_model, gs, ge, n, RDB_DOSTYPE_FFS_INTL) != RDB_OK)
        gui_msg("Split", "Could not split the free space.");
    g_dirty = 1; g_sel_part = -1;
    gui_refresh_parts();
    gui_update_buttons();
}

/* Create + layout the menu strip and attach it to g_win. Returns 1 on success. */
static int build_menus(void)
{
    /* Lay menus out CLASSIC (no GTMN_NewLookMenus) on every Kickstart. NewLook
       menus (V39+) render the highlighted item with the screen's FILLPEN /
       FILLTEXTPEN DrawInfo pens; HDPart runs on the user's Workbench public
       screen, and on customised WBs those pens are not guaranteed to contrast
       (seen on an OS 3.2 setup: FILLTEXTPEN = black over a blue FILLPEN -> the
       hovered item is invisible). Classic menus instead use the screen's
       DetailPen / BlockPen, the fundamental window pens, which are always high
       contrast — giving readable, consistent menus across KS2.0..3.2+ and
       matching the classic look on the V37 baseline. */
    g_menu = CreateMenus(g_newmenu, TAG_END);
    if (!g_menu) return 0;
    if (!LayoutMenus(g_menu, g_vi, TAG_END)) {
        FreeMenus(g_menu); g_menu = 0; return 0;
    }
    SetMenuStrip(g_win, g_menu);
    return 1;
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

    /* Render on the Workbench screen ONLY if its font is the 8-point topaz our
       fixed-pixel layout is designed for. On a bare ADF boot Intuition opens a
       Workbench screen with no prefs loaded (topaz 9): the window title, the
       menu bar, and our directly-drawn text (captions/header/bar, which inherit
       the screen font) would all render oversized vs the topaz-8 gadgets. In
       that case drop the pubscreen and open our own screen forced to topaz 8.
       SA_Font (explicit topaz 8) is used rather than SA_SysFont 0 — the latter
       selects the system DefaultFont, which on a bare boot IS topaz 9. If topaz 8
       can't be opened at all (not in ROM, no diskfont/FONTS:), fall back to
       SA_SysFont 0 so the screen still opens (just larger). */
    g_sfont = OpenFont(&g_font);   /* topaz 8; NULL -> not available on this boot */
    g_pub = LockPubScreen(0);
    if (g_pub && g_pub->Font && g_pub->Font->ta_YSize == 8 &&
        streq((const char *)g_pub->Font->ta_Name, "topaz.font")) {
        /* (1) The Workbench screen is already topaz 8 — render there (window on WB). */
        g_scr = g_pub;
    } else if (g_pub) {
        /* (2) Bare-boot Workbench screen is topaz 9. Open our own topaz-8 screen
           but CLONE the WB screen's display mode, depth, palette and pens so it
           looks like the system screen, not a flat default one:
             - SA_DisplayID: same resolution (a bare custom screen defaults to
               LORES 320, too narrow for the 580px window).
             - SA_Pens (cloned): sets DRIF_NEWLOOK so GadTools renders the full 3D
               look — without it gadgets/bevels come out flat with the harsh
               default palette.
             - matching depth + copied colours.
           Screen font: SA_Font (explicit topaz 8) when available, else SA_SysFont 0. */
        struct DrawInfo *dri = GetScreenDrawInfo(g_pub);
        ULONG modeid = (ULONG)GetVPModeID(&g_pub->ViewPort);
        int   depth  = (dri ? (int)dri->dri_Depth : 2);
        int   i, np = 0, ncol;
        static UWORD clonepens[NUMDRIPENS + 1];
        static UWORD clonepal[256];
        if (depth < 1) depth = 1;
        if (depth > 8) depth = 8;
        ncol = 1 << depth;
        for (i = 0; i < ncol; i++)
            clonepal[i] = (UWORD)GetRGB4(g_pub->ViewPort.ColorMap, i);
        if (dri && dri->dri_Pens) {
            int n = (int)dri->dri_NumPens;
            if (n > NUMDRIPENS) n = NUMDRIPENS;
            for (; np < n; np++) clonepens[np] = dri->dri_Pens[np];
        }
        clonepens[np] = (UWORD)~0;
        if (dri) FreeScreenDrawInfo(g_pub, dri);
        if (modeid == (ULONG)INVALID_ID) modeid = HIRES_KEY;
        UnlockPubScreen(0, g_pub); g_pub = 0;
        g_scr = OpenScreenTags(0, SA_DisplayID, modeid, SA_Depth, depth,
                               SA_Pens, (ULONG)clonepens,
                               SA_Title, (ULONG)"HDPart",
                               (g_sfont ? SA_Font : SA_SysFont),
                               (g_sfont ? (ULONG)&g_font : 0),
                               SA_Type, CUSTOMSCREEN, TAG_END);
        if (g_scr)
            for (i = 0; i < ncol; i++)
                SetRGB4(&g_scr->ViewPort, i, (clonepal[i] >> 8) & 0xF,
                        (clonepal[i] >> 4) & 0xF, clonepal[i] & 0xF);
    } else {
        /* (3) No Workbench screen at all — basic own topaz-8 hi-res screen. */
        g_scr = OpenScreenTags(0, SA_DisplayID, HIRES_KEY, SA_Depth, 2,
                               SA_Title, (ULONG)"HDPart",
                               (g_sfont ? SA_Font : SA_SysFont),
                               (g_sfont ? (ULONG)&g_font : 0),
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
        WA_Width,  g_leftb + 580 + g_scr->WBorRight,
        WA_Height, g_topb + 232 + g_scr->WBorBottom,
        WA_Title, (ULONG)"HDPart 0.5",
        WA_Gadgets, (ULONG)g_glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MENUPICK | IDCMP_VANILLAKEY | CYCLEIDCMP | BUTTONIDCMP | LISTVIEWIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                  WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
    if (!g_win) goto cleanup_gad;

    if (!build_menus()) goto cleanup_win;   /* new cleanup label, Step 6 */

    GT_RefreshWindow(g_win, 0);
    gui_draw_chrome();
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
                    gui_draw_chrome();
                    gui_draw_bar();
                    GT_EndRefresh(g_win, TRUE);
                    break;
                case IDCMP_MOUSEBUTTONS:
                    if (code == SELECTDOWN) {           /* click on the disk-map bar selects */
                        int idx = gui_part_at_x((int)mx, (int)my);
                        if (idx >= 0) gui_set_selection(idx);
                        else if (gui_hit_easter((int)mx, (int)my))
                            gui_request("HDPart", "You watched too many movies.", 0, "I know");
                    }
                    break;
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GID_DEVICE) gui_select_driver((int)code);
                    else if (gad->GadgetID == GID_UNIT) gui_select_unit((int)code);
                    else if (gad->GadgetID == GID_SCAN) gui_scan_selected();
                    else if (gad->GadgetID == GID_REFRESH) gui_refresh_current();
                    else if (gad->GadgetID == GID_RESIZE) {
                        if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                            gui_resize_dialog(g_sel_part);
                    }
                    else if (gad->GadgetID == GID_FORMAT) {
                        if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                            gui_format(g_sel_part);
                    }
                    else if (gad->GadgetID == GID_FS) gui_filesystems();
                    else if (gad->GadgetID == GID_DRIVER) gui_load_driver();
                    else if (gad->GadgetID == GID_PARTS) gui_set_selection((int)code);
                    else if (gad->GadgetID == GID_SAVE) gui_save();
                    else if (gad->GadgetID == GID_SPLIT) gui_split();
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
                case IDCMP_MENUPICK: {
                    UWORD mnum = code;
                    while (mnum != MENUNULL) {
                        struct MenuItem *mi = ItemAddress(g_menu, mnum);
                        UWORD mn = MENUNUM(mnum), it = ITEMNUM(mnum);
                        if (mn == MN_PROJECT) {
                            if (it == IT_ABOUT) gui_about();
                            else if (it == IT_SAVE) gui_save();
                            else if (it == IT_QUIT) {
                                if (!g_dirty || gui_confirm("Quit HDPart",
                                        "Discard unsaved changes and quit?")) done = TRUE;
                            }
                        } else if (mn == MN_DISK) {
                            if (it == ID_SCAN) gui_scan_selected();
                            else if (it == ID_LOAD) gui_load_driver();
                            else if (it == ID_REFRESH) gui_refresh_current();
                            else if (it == ID_INIT) gui_init_disk();
                            else if (it == ID_FS) gui_filesystems();
                        } else if (mn == MN_PART) {
                            if (it == IP_NEW) gui_new();
                            else if (it == IP_EDIT) {
                                if (g_sel_part >= 0 && g_sel_part < g_model.num_parts) {
                                    gui_edit_dialog(g_sel_part); gui_refresh_parts(); gui_update_buttons();
                                }
                            }
                            else if (it == IP_DELETE) gui_delete();
                            else if (it == IP_SPLIT) gui_split();
                            else if (it == IP_RESIZE) {
                                if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                    gui_resize_dialog(g_sel_part);
                            }
                            else if (it == IP_FORMAT) {
                                if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                    gui_format(g_sel_part);
                            }
                        }
                        mnum = mi ? mi->NextSelect : MENUNULL;
                    }
                    break;
                }
                case IDCMP_VANILLAKEY: {
                    UBYTE ch = (UBYTE)code; if (ch >= 'A' && ch <= 'Z') ch += 32;
                    switch (ch) {
                        case 'y': gui_filesystems(); break;
                        case 'l': gui_load_driver(); break;
                        case 'n': gui_new(); break;
                        case 'd': gui_delete(); break;
                        case 't': gui_split(); break;
                        case 'f': gui_refresh_current(); break;
                        case 's': gui_save(); break;
                        case 'e':
                            if (g_sel_part >= 0 && g_sel_part < g_model.num_parts) {
                                gui_edit_dialog(g_sel_part); gui_refresh_parts(); gui_update_buttons();
                            }
                            break;
                        case 'r':
                            if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                gui_resize_dialog(g_sel_part);
                            break;
                        case 'o':
                            if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                gui_format(g_sel_part);
                            break;
                    }
                    break;
                }
            }
        }
    }

    ClearMenuStrip(g_win);
    FreeMenus(g_menu); g_menu = 0;
    CloseWindow(g_win); g_win = 0;
    goto cleanup_gad;
cleanup_win:
    FreeMenus(g_menu); g_menu = 0;
    CloseWindow(g_win); g_win = 0;
cleanup_gad:
    FreeGadgets(g_glist); g_glist = 0;
    FreeVisualInfo(g_vi); g_vi = 0;   /* reached by fall-through from cleanup_gad */
cleanup_scr:
    if (!g_pub && g_scr) CloseScreen(g_scr);
    if (g_pub) UnlockPubScreen(0, g_pub);
    g_scr = 0; g_pub = 0;
cleanup_libs:
    if (g_sfont) { CloseFont(g_sfont); g_sfont = 0; }   /* before GfxBase */
    if (AslBase) { CloseLibrary(AslBase); AslBase = 0; }
    rdb_model_free(&g_model);
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
    g_cur_driver[0] = 0; g_cur_unit = 0; g_cur_unitidx = -1;

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
        g_cur_unitidx = -1;
        gui_refresh_parts(); gui_update_buttons();
        return;
    }
    g_cur_unitidx = uidx;
    d = &g_disks[g_unitmap[uidx]];
    for (k = 0; k < 39 && d->driver[k]; k++) g_cur_driver[k] = d->driver[k];
    g_cur_driver[k] = 0;
    g_cur_unit = d->unit;
    h = dev_open(d->driver, d->unit);
    if (h) {
        dev_geometry(h, &g_geo);
        rdb_model_free(&g_model);
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) g_have_model = 1;
        dev_close(h);
    }
    gui_refresh_parts(); gui_update_buttons();
}

/* Refresh button: re-read the currently shown device+unit from disk (geometry +
   RDB), so the user can confirm the view matches what is actually stored (e.g.
   after a Save). Re-reading discards any unsaved in-memory edits, so confirm
   first when the model is dirty. */
static void gui_refresh_current(void)
{
    if (g_cur_unitidx < 0) return;
    if (g_dirty &&
        !gui_request("Refresh", "Re-read this disk from storage?\n"
                                "Unsaved changes will be lost.", 1, "Proceed"))
        return;
    gui_status("Refreshing...");
    gui_select_unit(g_cur_unitidx);   /* re-opens the device, re-reads geometry + RDB */
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
        g_cur_unitidx = -1;
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

static void gui_about(void)
{
    gui_msg("About HDPart",
            "HDPart 0.5\n"
            "RDB hard-disk partition tool\n"
            "for AmigaOS 2.04+ (KS V37+).\n"
            "\n"
            "Based on rdbtool (amitools) by\n"
            "Christian Vogelgsang, \xA9 2020-23.\n"
            "github.com/cnvogelg/amitools");
}

/* ----- Filesystems dialog ------------------------------------------------ */

/* Static storage for the FS listview (mirrors g_partlist / g_partnodes pattern). */
static struct List g_fslist;
static struct Node g_fsnodes[RDB_MAX_FS];
/* 64 bytes is enough for "XXXXXXXX  PFSFileSystem       file   512K" (worst case). */
static char        g_fsrows[RDB_MAX_FS][64];

/* DosType preset cycle for the Filesystems editor. */
static const char *const kFsPresetLabels[] = {
    "PFS\\3", "PDS\\3", "SFS\\0", "SFS\\2", "Custom", 0
};
static const uint32_t kFsPresetValues[] = {
    0x50465303u, 0x50445303u, 0x53465300u, 0x53465302u
};
#define N_FS_PRESETS 4

/* Map a dos_type to a kFsPresetValues[] index, or N_FS_PRESETS (Custom). */
static int fs_preset_index(uint32_t v)
{
    int i; for (i = 0; i < N_FS_PRESETS; i++) if (kFsPresetValues[i] == v) return i;
    return N_FS_PRESETS;
}

/* Build one FS list row.  Fields: "> DosType  Name            src   NNK"
   col 0=mark, col 2=dostype(10), col 13=name(20), col 34=src(8), col 43=size. */
static void fs_build_row(char *row, int idx, int sel)
{
    RdbFileSys *fs = &g_model.fs[idx];
    int p = 0;
    char dtbuf[12];
    dostype_label(dtbuf, fs->dos_type);
    row[p++] = (idx == sel) ? '>' : ' ';
    row[p++] = ' ';
    s_cat(row, &p, dtbuf); s_pad(row, &p, 13);
    { int k; for (k = 0; fs->name[k] && k < 19; k++) row[p++] = fs->name[k]; }
    s_pad(row, &p, 34);
    switch (fs->source) {
        case RDB_FS_FILE:     s_cat(row, &p, "file");     break;
        case RDB_FS_COPIED:   s_cat(row, &p, "copied");   break;
        default:              s_cat(row, &p, "embedded"); break;
    }
    s_pad(row, &p, 43);
    { uint32_t kb = (fs->seg_len + 1023u) / 1024u; p += u2s(row + p, kb); s_cat(row, &p, "K"); }
    row[p] = 0;
}

/* Rebuild g_fslist and g_fsnodes from g_model.fs[0..num_fs). */
static void fs_rebuild_list(int sel, struct Window *dw, struct Gadget *glv)
{
    int i;
    NewList(&g_fslist);
    for (i = 0; i < g_model.num_fs && i < RDB_MAX_FS; i++) {
        fs_build_row(g_fsrows[i], i, sel);
        g_fsnodes[i].ln_Name = g_fsrows[i];
        AddTail(&g_fslist, &g_fsnodes[i]);
    }
    if (dw && glv)
        GT_SetGadgetAttrs(glv, dw, 0,
                          GTLV_Labels,   (ULONG)&g_fslist,
                          GTLV_Selected, (ULONG)(sel >= 0 ? (ULONG)sel : ~0UL),
                          TAG_END);
}

/* Sub-picker for "Copy from disk": Driver cycle + Unit cycle (0..7) + OK/Cancel.
   Collects driver name (into drvOut[40]) and unit number (into *unitOut).
   Returns 1 if the user pressed OK, 0 if cancelled/closed.
   Multi-FS source (which > 0) is out of scope; caller always uses which=0. */
static int gui_copy_from_disk_picker(char drvOut[40], uint32_t *unitOut)
{
    /* Local driver labels built from disc_candidate_drivers (same as gui_build_drivers
       but in local arrays so this picker is self-contained). */
    static const char *plabels[DISC_MAX + 2];
    static const char *ulabels[9];  /* "0".."7" + NULL */
    static char       unumbufs[8][4];
    const char *cands[DISC_MAX];
    int nc, i;
    struct Window *pw;
    struct Gadget *pglist = 0, *pg;
    struct Gadget *gDrv = 0, *gUnit = 0;
    struct NewGadget ng;
    int dt = g_topb, dl = g_leftb;
    int pdone = 0, presult = 0;
    int drvIdx = 0, unitIdx = 0;

    /* Build driver list. */
    nc = disc_candidate_drivers(cands, DISC_MAX);
    if (nc <= 0) {
        gui_msg("Filesystems", "No disk drivers found.\nLoad a driver first.");
        return 0;
    }
    for (i = 0; i < nc && i < DISC_MAX; i++) plabels[i] = cands[i];
    plabels[nc] = 0;

    /* Build unit labels 0..7. */
    for (i = 0; i < 8; i++) {
        unumbufs[i][0] = (char)('0' + i);
        unumbufs[i][1] = 0;
        ulabels[i] = unumbufs[i];
    }
    ulabels[8] = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    pg = CreateContext(&pglist);
#pragma GCC diagnostic pop
    if (!pg) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    /* Driver cycle */
    ng.ng_LeftEdge = dl + 80; ng.ng_TopEdge = dt + 6; ng.ng_Width = 220; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Driver"; ng.ng_GadgetID = 1;
    pg = CreateGadget(CYCLE_KIND, pg, &ng, GTCY_Labels, (ULONG)plabels, GTCY_Active, 0, TAG_END);
    gDrv = pg;

    /* Unit cycle (0..7) */
    ng.ng_LeftEdge = dl + 80; ng.ng_TopEdge = dt + 26; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Unit"; ng.ng_GadgetID = 2;
    pg = CreateGadget(CYCLE_KIND, pg, &ng, GTCY_Labels, (ULONG)ulabels, GTCY_Active, 0, TAG_END);
    gUnit = pg;

    /* OK / Cancel */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 48; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"OK"; ng.ng_GadgetID = 10;
    pg = CreateGadget(BUTTON_KIND, pg, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 230; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    pg = CreateGadget(BUTTON_KIND, pg, &ng, TAG_END);
    if (!pg) { FreeGadgets(pglist); return 0; }

    {   int pwW = dl + 320 + g_scr->WBorRight;
        int pwH = dt + 70 + g_scr->WBorBottom;
        int pwL = 0, pwT = 0;
        dlg_center(pwW, pwH, &pwL, &pwT);
        pw = dlg_open("Copy FS from disk", pwL, pwT, pwW, pwH,
                      BUTTONIDCMP | CYCLEIDCMP, pglist);
    }
    if (!pw) { FreeGadgets(pglist); return 0; }
    GT_RefreshWindow(pw, 0);

    while (!pdone) {
        struct IntuiMessage *im;
        WaitPort(pw->UserPort);
        while ((im = GT_GetIMsg(pw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            UWORD code = im->Code;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { pdone = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) { dlg_refresh(pw); }
            else if (cl == IDCMP_GADGETUP) {
                if (ig == gDrv)       drvIdx  = (int)code;
                else if (ig == gUnit) unitIdx = (int)code;
                else if (ig->GadgetID == 10) { presult = 1; pdone = 1; } /* OK */
                else if (ig->GadgetID == 11) { pdone = 1; }              /* Cancel */
            }
        }
    }
    dlg_close(pw, pglist);

    if (presult) {
        /* Copy the selected driver name into stable caller buffer. */
        const char *src = plabels[drvIdx];
        int k; for (k = 0; k < 39 && src[k]; k++) drvOut[k] = src[k];
        drvOut[k] = 0;
        *unitOut = (uint32_t)unitIdx;
    }
    return presult;
}

static void gui_filesystems(void)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g;
    struct Gadget *gLV = 0, *gDT = 0, *gPreset = 0, *gRemove = 0;
    struct NewGadget ng;
    int dt = g_topb, dl = g_leftb;
    static char dtBuf[12];   /* hex string for DosType field */
    int sel = -1;            /* selected FS index, V37-safe */
    int done = 0;
    int presetIdx = N_FS_PRESETS; /* Custom */

    if (!g_have_model) return;

    /* Pre-select first entry if any. */
    if (g_model.num_fs > 0) {
        sel = 0;
        u32_to_hex(dtBuf, g_model.fs[0].dos_type);
        presetIdx = fs_preset_index(g_model.fs[0].dos_type);
    } else {
        u32_to_hex(dtBuf, 0x00000000u);
        presetIdx = N_FS_PRESETS;
    }

    /* Build the initial list (no window yet; dw/glv = 0). */
    fs_rebuild_list(sel, 0, 0);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&dglist);
#pragma GCC diagnostic pop
    if (!g) return;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    /* Listview — RDB_MAX_FS rows (height 8 rows = 8*10 = 80px) */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 6; ng.ng_Width = 420; ng.ng_Height = 80;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = 1;
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&g_fslist,
                     GTLV_Selected, (ULONG)(sel >= 0 ? (ULONG)sel : ~0UL),
                     GTLV_ShowSelected, 0, TAG_END);
    gLV = g;

    /* DosType string gadget */
    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 94; ng.ng_Width = 100; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"DosType"; ng.ng_GadgetID = 2;
    g = CreateGadget(STRING_KIND, g, &ng,
                     GTST_String, (ULONG)dtBuf, GTST_MaxChars, 10,
                     GA_Disabled, (ULONG)(sel < 0), TAG_END);
    gDT = g;

    /* Preset cycle */
    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 114; ng.ng_Width = 140; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Preset"; ng.ng_GadgetID = 3;
    g = CreateGadget(CYCLE_KIND, g, &ng,
                     GTCY_Labels, (ULONG)kFsPresetLabels,
                     GTCY_Active, (ULONG)presetIdx,
                     GA_Disabled, (ULONG)(sel < 0), TAG_END);
    gPreset = g;

    /* Add from file button */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 138; ng.ng_Width = 110; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Add from file..."; ng.ng_GadgetID = 4;
    g = CreateGadget(BUTTON_KIND, g, &ng,
                     GA_Disabled, (ULONG)(AslBase == 0), TAG_END);

    /* Copy from disk button — copies the first embedded FS (which=0) from another
       disk's RDB into this model.  Multi-FS source (which>0) is out of scope. */
    ng.ng_LeftEdge = dl + 130; ng.ng_TopEdge = dt + 138; ng.ng_Width = 120; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Copy from disk..."; ng.ng_GadgetID = 7;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    /* Remove button */
    ng.ng_LeftEdge = dl + 260; ng.ng_TopEdge = dt + 138; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Remove"; ng.ng_GadgetID = 5;
    g = CreateGadget(BUTTON_KIND, g, &ng,
                     GA_Disabled, (ULONG)(sel < 0), TAG_END);
    gRemove = g;

    /* Done button */
    ng.ng_LeftEdge = dl + 350; ng.ng_TopEdge = dt + 138; ng.ng_Width = 90; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Done"; ng.ng_GadgetID = 6;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return; }

    {   int dwW = dl + 450 + g_scr->WBorRight;
        int dwH = dt + 160 + g_scr->WBorBottom;
        int dwL = 0, dwT = 0;
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open("Filesystems", dwL, dwT, dwW, dwH,
                      BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP | LISTVIEWIDCMP, dglist);
    }
    if (!dw) { FreeGadgets(dglist); return; }
    GT_RefreshWindow(dw, 0);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            UWORD code = im->Code;
            GT_ReplyIMsg(im);

            if (cl == IDCMP_CLOSEWINDOW) {
                done = 1;
            } else if (cl == IDCMP_REFRESHWINDOW) {
                dlg_refresh(dw);
            } else if (cl == IDCMP_GADGETUP) {
                int gid = ig->GadgetID;

                if (gid == 1) {
                    /* Listview selection changed. */
                    int newsel = (int)code;
                    if (newsel >= 0 && newsel < g_model.num_fs) {
                        sel = newsel;
                        /* Rebuild labels to move the '>' marker (V37-safe). */
                        fs_rebuild_list(sel, dw, gLV);
                        /* Update DosType field + preset cycle. */
                        u32_to_hex(dtBuf, g_model.fs[sel].dos_type);
                        presetIdx = fs_preset_index(g_model.fs[sel].dos_type);
                        GT_SetGadgetAttrs(gDT, dw, 0,
                                          GTST_String, (ULONG)dtBuf,
                                          GA_Disabled, FALSE, TAG_END);
                        GT_SetGadgetAttrs(gPreset, dw, 0,
                                          GTCY_Active, (ULONG)presetIdx,
                                          GA_Disabled, FALSE, TAG_END);
                        GT_SetGadgetAttrs(gRemove, dw, 0, GA_Disabled, FALSE, TAG_END);
                    }

                } else if (gid == 2) {
                    /* DosType string field committed (user pressed Return). */
                    if (sel >= 0 && sel < g_model.num_fs) {
                        const char *s = (const char *)
                            ((struct StringInfo *)gDT->SpecialInfo)->Buffer;
                        uint32_t v;
                        if (parse_hex32(s, &v)) {
                            g_model.fs[sel].dos_type = v;
                            g_dirty = 1;
                            /* Re-sync preset cycle. */
                            presetIdx = fs_preset_index(v);
                            GT_SetGadgetAttrs(gPreset, dw, 0,
                                              GTCY_Active, (ULONG)presetIdx, TAG_END);
                            fs_rebuild_list(sel, dw, gLV);
                        } else {
                            gui_msg("Filesystems", "Enter a hex value (e.g. 0x50465303).");
                        }
                    }

                } else if (gid == 3) {
                    /* Preset cycle changed. */
                    presetIdx = (int)code;
                    if (sel >= 0 && sel < g_model.num_fs) {
                        uint32_t v = (presetIdx < N_FS_PRESETS)
                                     ? kFsPresetValues[presetIdx]
                                     : g_model.fs[sel].dos_type;
                        if (presetIdx < N_FS_PRESETS) {
                            g_model.fs[sel].dos_type = v;
                            g_dirty = 1;
                            u32_to_hex(dtBuf, v);
                            GT_SetGadgetAttrs(gDT, dw, 0,
                                              GTST_String, (ULONG)dtBuf, TAG_END);
                            fs_rebuild_list(sel, dw, gLV);
                        }
                    }

                } else if (gid == 4) {
                    /* Add from file: ASL requester (no pattern filter — WB 2.04 gotcha). */
                    struct FileRequester *fr;
                    static char fspath[256];
                    int rc;

                    if (!AslBase) break;
                    if (g_model.num_fs >= RDB_MAX_FS) {
                        gui_msg("Filesystems", fsl_err_text(FSL_EFULL));
                        break;
                    }
                    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
                            ASLFR_TitleText,     (ULONG)"Select a filesystem handler",
                            ASLFR_InitialDrawer, (ULONG)"L:",
                            ASLFR_Screen,        (ULONG)g_scr,
                            TAG_END);
                    if (!fr) { gui_msg("Filesystems", "Could not open the file requester."); break; }
                    if (!AslRequest(fr, 0)) { FreeAslRequest(fr); break; } /* cancelled */

                    { int j = 0; const char *d = (const char *)fr->fr_Drawer;
                      while (d && d[j] && j < (int)sizeof(fspath) - 1) { fspath[j] = d[j]; j++; }
                      fspath[j] = 0; }
                    AddPart((STRPTR)fspath, (CONST_STRPTR)fr->fr_File, sizeof(fspath));
                    FreeAslRequest(fr);

                    rc = fsload_from_file(fspath, &g_model.fs[g_model.num_fs]);
                    if (rc != FSL_OK) {
                        gui_msg("Filesystems", fsl_err_text(rc));
                    } else {
                        sel = g_model.num_fs;
                        g_model.num_fs++;
                        g_dirty = 1;
                        u32_to_hex(dtBuf, g_model.fs[sel].dos_type);
                        presetIdx = fs_preset_index(g_model.fs[sel].dos_type);
                        fs_rebuild_list(sel, dw, gLV);
                        GT_SetGadgetAttrs(gDT, dw, 0,
                                          GTST_String, (ULONG)dtBuf,
                                          GA_Disabled, FALSE, TAG_END);
                        GT_SetGadgetAttrs(gPreset, dw, 0,
                                          GTCY_Active, (ULONG)presetIdx,
                                          GA_Disabled, FALSE, TAG_END);
                        GT_SetGadgetAttrs(gRemove, dw, 0, GA_Disabled, FALSE, TAG_END);
                    }

                } else if (gid == 7) {
                    /* Copy from disk: open sub-picker, call fsload_from_disk. */
                    char srcDrv[40];
                    uint32_t srcUnit;
                    int rc;

                    if (g_model.num_fs >= RDB_MAX_FS) {
                        gui_msg("Filesystems", fsl_err_text(FSL_EFULL));
                        break;
                    }
                    if (!gui_copy_from_disk_picker(srcDrv, &srcUnit)) break; /* cancelled */

                    rc = fsload_from_disk(srcDrv, srcUnit, 0, &g_model.fs[g_model.num_fs]);
                    if (rc != FSL_OK) {
                        gui_msg("Filesystems", fsl_err_text(rc));
                    } else {
                        sel = g_model.num_fs;
                        g_model.num_fs++;
                        g_dirty = 1;
                        u32_to_hex(dtBuf, g_model.fs[sel].dos_type);
                        presetIdx = fs_preset_index(g_model.fs[sel].dos_type);
                        fs_rebuild_list(sel, dw, gLV);
                        GT_SetGadgetAttrs(gDT, dw, 0,
                                          GTST_String, (ULONG)dtBuf,
                                          GA_Disabled, FALSE, TAG_END);
                        GT_SetGadgetAttrs(gPreset, dw, 0,
                                          GTCY_Active, (ULONG)presetIdx,
                                          GA_Disabled, FALSE, TAG_END);
                        GT_SetGadgetAttrs(gRemove, dw, 0, GA_Disabled, FALSE, TAG_END);
                    }

                } else if (gid == 5) {
                    /* Remove: free seg_data, shift array, decrement num_fs. */
                    int i;
                    if (sel < 0 || sel >= g_model.num_fs) break;

                    /* If any partition uses this dos_type, confirm first. */
                    { int uses = 0, kk;
                      uint32_t fdt = g_model.fs[sel].dos_type;
                      for (kk = 0; kk < g_model.num_parts; kk++)
                          if (g_model.parts[kk].dos_type == fdt) { uses = 1; break; }
                      if (uses) {
                          if (!gui_confirm("Filesystems",
                                  "A partition uses this filesystem type.\n"
                                  "Remove it anyway?"))
                              break;
                      }
                    }

                    rdb_seg_free(g_model.fs[sel].seg_data);
                    g_model.fs[sel].seg_data = 0;
                    for (i = sel; i < g_model.num_fs - 1; i++)
                        g_model.fs[i] = g_model.fs[i + 1];
                    /* zero out the vacated slot */
                    { RdbFileSys *z = &g_model.fs[g_model.num_fs - 1];
                      int zz; for (zz = 0; zz < (int)sizeof(RdbFileSys); zz++)
                          ((uint8_t *)z)[zz] = 0; }
                    g_model.num_fs--;
                    g_dirty = 1;

                    /* Adjust selection. */
                    if (g_model.num_fs == 0) {
                        sel = -1;
                        GT_SetGadgetAttrs(gDT,     dw, 0, GA_Disabled, TRUE, TAG_END);
                        GT_SetGadgetAttrs(gPreset, dw, 0, GA_Disabled, TRUE, TAG_END);
                        GT_SetGadgetAttrs(gRemove, dw, 0, GA_Disabled, TRUE, TAG_END);
                    } else {
                        if (sel >= g_model.num_fs) sel = g_model.num_fs - 1;
                        u32_to_hex(dtBuf, g_model.fs[sel].dos_type);
                        presetIdx = fs_preset_index(g_model.fs[sel].dos_type);
                        GT_SetGadgetAttrs(gDT, dw, 0,
                                          GTST_String, (ULONG)dtBuf, TAG_END);
                        GT_SetGadgetAttrs(gPreset, dw, 0,
                                          GTCY_Active, (ULONG)presetIdx, TAG_END);
                    }
                    fs_rebuild_list(sel, dw, gLV);

                } else if (gid == 6) {
                    /* Done. */
                    done = 1;
                }
            }
        }
    }
    dlg_close(dw, dglist);

    /* Refresh the partition list in case FS assignments changed. */
    gui_refresh_parts();
    gui_update_buttons();
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
            ASLFR_Screen,        (ULONG)g_scr,   /* open on OUR screen, not WB */
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
    *bx = 16 + g_leftb; *by = 92 + g_topb; *bw = 550; *bh = 16;
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

/* Draw the two BevelBox group frames + their captions. Not gadgets, so must be
   redrawn on every window refresh (like gui_draw_bar). Coordinates are window-
   relative and match the gadget layout in build_gadgets(). */
static void gui_draw_chrome(void)
{
    struct RastPort *rp;
    if (!g_win) return;
    rp = g_win->RPort;
    /* Disk panel */
    DrawBevelBox(rp, 6 + g_leftb, 4 + g_topb, 568, 68, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    SetAPen(rp, 1); SetBPen(rp, 0);
    Move(rp, 16 + g_leftb, 4 + g_topb + 8); Text(rp, (CONST_STRPTR)" Disk ", 6);
    /* Partitions panel (shifted +18 from row-3 addition) */
    DrawBevelBox(rp, 6 + g_leftb, 76 + g_topb, 568, 132, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    Move(rp, 16 + g_leftb, 76 + g_topb + 8); Text(rp, (CONST_STRPTR)" Partitions ", 12);
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
    gui_draw_easter();       /* tiny pi in the corner, always present */

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
    static char hdr[56];
    int p = 0, lx, ly;
    if (!g_win) return;
    rp = g_win->RPort;
    s_pad(hdr, &p, 2);                            /* blank selection-marker column */
    s_cat(hdr, &p, "#");     s_pad(hdr, &p, 6);
    s_cat(hdr, &p, "Name");  s_pad(hdr, &p, 18);
    s_cat(hdr, &p, "Type");  s_pad(hdr, &p, 30);
    s_cat(hdr, &p, "Start"); s_pad(hdr, &p, 38);
    s_cat(hdr, &p, "End");   s_pad(hdr, &p, 46);
    s_cat(hdr, &p, "Size");  hdr[p] = 0;
    lx = 16 + g_leftb + 4;               /* match the listview's text inset */
    ly = 120 + g_topb - 2;               /* baseline just above the listview */
    SetAPen(rp, 1);
    Move(rp, lx, ly);
    Text(rp, (CONST_STRPTR)hdr, (LONG)p);
}

/* A tiny hand-drawn pi glyph in the bottom-right corner (no Topaz glyph for it).
   Clicking it is an easter egg. */
static void gui_draw_easter(void)
{
    struct RastPort *rp;
    int px = 571 + g_leftb, py = 227 + g_topb;   /* bottom-right corner, right of the Save button */
    if (!g_win) return;
    rp = g_win->RPort;
    SetAPen(rp, 1);
    Move(rp, px,     py);     Draw(rp, px + 5, py);       /* top bar (half height) */
    Move(rp, px + 1, py);     Draw(rp, px + 1, py + 4);   /* left leg */
    Move(rp, px + 4, py);     Draw(rp, px + 4, py + 4);   /* right leg */
}

/* True if (mx,my) is in the bottom-right corner where the pi glyph lives. Only
   non-gadget clicks reach this (GadTools eats gadget clicks), so the generous
   rect won't conflict with the Save button to its left. */
static int gui_hit_easter(int mx, int my)
{
    return mx >= 568 + g_leftb && my >= 223 + g_topb;   /* clear of the Save button (ends x=566) */
}
