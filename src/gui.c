/* HDPart GUI: GadTools gadgets plus hand-drawn disk map, unit list, partition
   table and details panel, in a window on the Workbench screen. */
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/execbase.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <intuition/gadgetclass.h>
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
#include <dos/dos.h>              /* SHARED_LOCK, BPTR (asl.library LIBS: fallback) */
#include <dos/dosextens.h>        /* struct Process, FileLock, ACTION_FLUSH: Quick setup */
#include <proto/dos.h>
#include "gui.h"
#include "discover.h"
#include "device.h"
#include "driver.h"
#include "rdb.h"
#include "format.h"
#include "safety.h"
#include "fsload.h"
#include "quick.h"

extern struct IntuitionBase *IntuitionBase;   /* opened in main.c */
struct Library *GadToolsBase = 0;
struct GfxBase *GfxBase = 0;
struct Library *AslBase = 0;
extern struct DosLibrary *DOSBase;   /* opened in startup.c (for AddPart) */

/* Gadget IDs */
enum { GID_DEVICE = 1, GID_SCAN, GID_DRIVER, GID_PARTS, GID_NEW, GID_DELETE,
       GID_EDIT, GID_INIT, GID_SAVE, GID_STATUS, GID_UNIT, GID_SPLIT, GID_REFRESH,
       GID_RESIZE, GID_FORMAT, GID_FS, GID_QUICK };
/* GID_UNIT and GID_PARTS are the scrollers of the Units list and partition table. */

/* Module state for one GUI session. */
static struct Screen  *g_scr;        /* screen we render on (pub or own) */
static struct Screen  *g_pub;        /* locked pubscreen, or NULL */
static struct Window  *g_win;
static APTR            g_vi;          /* VisualInfo */
static struct Gadget  *g_glist;      /* gadtools context list */
static struct Gadget  *g_gad[18];    /* gadget pointers by GID_* */
static struct TextAttr g_font = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct TextFont *g_sfont = 0;  /* explicit topaz 8 for our own screen (NULL if unavailable) */
static struct Menu    *g_menu = 0;   /* menu strip attached to g_win */

/* Discovery + current selection (static: keep off the stack). */
static DiscDisk g_disks[DISC_MAX];
static int      g_ndisks;
static const char *g_drvlabels[DISC_MAX + 2];   /* Driver cycle: prompt + names */
static char     g_unittext[DISC_MAX][24];        /* "#<n> <size> MB", one per listed unit */
static const char *g_unit_empty = "";            /* shown in the Units list when it has no units */
static RdbModel g_model;
static int      g_have_model;
static int      g_dirty;            /* unsaved edits in g_model */
static DeviceInfo g_geo;            /* geometry of the selected device */
static char     g_cur_driver[40];   /* selected device driver/unit (for Save) */
static uint32_t g_cur_unit;
static int      g_sel_part = -1;    /* selected partition index, or -1 */
static int      g_cur_unitidx = -1; /* Units list row currently shown (-1 = none): Refresh re-probes it */
static int      g_drv_idx;          /* Driver cycle index currently shown: restored if a change is declined */
static int      g_unitmap[DISC_MAX + 1]; /* Units list row -> g_disks index (-1 = none) */
static int      g_nunits;           /* number of rows in the Units list */
static int      g_no_rdb;           /* current disk had no RDB when read */
static char     g_target_driver[40];/* driver the Scan button will probe ("" = none) */
static WORD     g_topb, g_leftb;     /* window border offsets (title bar / left edge);
                                        gadget coords are relative to the window's
                                        outer top-left, so all gadgets shift by these */

/* Session filesystem pool — survives Refresh/disk-switch.  Owns each seg_data. */
static RdbFileSys g_fs_pool[RDB_MAX_FS];
static int        g_fs_npool;

/* Layout, in pixels relative to the window's inner top-left (add g_leftb/g_topb).
   Left column: driver and units. Right column: map, table, details, actions. */
#define WIN_W      604
#define WIN_H      204
#define LEFT_X     8
#define LEFT_W     156
#define RIGHT_X    172
#define RIGHT_W    424
#define SCROLL_W   16
#define UNITS_Y    52
#define UNITS_H    130
#define UNIT_ROW_H 21
#define UNIT_ROWS  6
#define MAP_Y      2
#define MAP_H      36
#define TABLE_Y    42
#define TABLE_H    56
#define PART_ROW_H 10
#define PART_ROWS  4
#define DETAIL_Y   102
#define DETAIL_H   64
#define ACTION_Y   170
#define FOOTER_Y   186
/* The Quick setup panel, inside the details box. */
#define QP_BTN_W   140
#define QP_BTN_H   18
#define QP_BTN_Y   19

/* A hand-drawn list: GadTools cannot highlight a row on V37. Window-relative. */
typedef struct { WORD x, y, w, h, rowh, vis, top, count; } ListGeom;
static ListGeom g_plist, g_ulist;
static struct { WORD x, y, w, h; } g_bar;

/* Screen pens, read from DrawInfo so a customised Workbench palette still works. */
static struct DrawInfo *g_dri;
static int g_pen_bg, g_pen_text, g_pen_shine, g_pen_fill, g_pen_seltext;

/* Partition table rows: fixed char columns, Name@0 Type@12 Start@24 End@32 Size@40.
   80 bytes is headroom for any legal RDB. */
static char        g_partrows[RDB_MAX_PARTS][80];
static char        g_statusbuf[80];
/* safety_is_internal_ide() at startup, for the on-target test to read. -1 until then. */
int                 g_qs_is_ide = -1;
/* Quick setup, for the on-target test too: the screen open (0 = closed) and the reason
   for the refusal on it (0 = none). Volatile so they survive being written only. */
static volatile int g_qs_screen;
static volatile int g_qs_resume_checks;   /* times gui_quick_resume_check has run: once, at startup */
static volatile int g_qs_check_stop;      /* what stopped it: see gui_quick_resume_check. 0: the popup was shown */
static volatile int g_qs_refusal;
static int          g_qs_panel_gone;   /* a driver was picked: no Quick setup panel this session */

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

/* ---------- Session filesystem pool helpers -------------------------------- */

/* Deep-copy one filesystem into the pool.  Returns 1 ok, 0 if full or OOM. */
static int fs_pool_add_copy(const RdbFileSys *src)
{
    RdbFileSys *d;
    if (g_fs_npool >= RDB_MAX_FS) return 0;
    d = &g_fs_pool[g_fs_npool];
    *d = *src;
    d->seg_data = (uint8_t *)rdb_seg_alloc(src->seg_len ? src->seg_len : 1);
    if (!d->seg_data) return 0;
    { uint32_t i; for (i = 0; i < src->seg_len; i++) d->seg_data[i] = src->seg_data[i]; }
    g_fs_npool++;
    return 1;
}

/* Has the pool already got an entry for this dos_type? */
static int fs_pool_has(uint32_t dostype)
{
    int i; for (i = 0; i < g_fs_npool; i++) if (g_fs_pool[i].dos_type == dostype) return 1; return 0;
}

/* Merge a freshly-parsed disk's embedded FS into the pool (dedup by dos_type). */
static void fs_pool_merge_from_model(const RdbModel *m)
{
    int i; for (i = 0; i < m->num_fs && g_fs_npool < RDB_MAX_FS; i++)
        if (!fs_pool_has(m->fs[i].dos_type)) fs_pool_add_copy(&m->fs[i]);
}

/* Free the whole pool (app exit). */
static void fs_pool_free(void)
{
    int i; for (i = 0; i < g_fs_npool; i++)
        if (g_fs_pool[i].seg_data) { rdb_seg_free(g_fs_pool[i].seg_data); g_fs_pool[i].seg_data = 0; }
    g_fs_npool = 0;
}

/* Remove pool entry idx (free its seg_data, shift array down). */
static void fs_pool_remove(int idx)
{
    int i; if (idx < 0 || idx >= g_fs_npool) return;
    if (g_fs_pool[idx].seg_data) rdb_seg_free(g_fs_pool[idx].seg_data);
    for (i = idx; i < g_fs_npool - 1; i++) g_fs_pool[i] = g_fs_pool[i + 1];
    { int z; uint8_t *p = (uint8_t *)&g_fs_pool[g_fs_npool - 1];
      for (z = 0; z < (int)sizeof(RdbFileSys); z++) p[z] = 0; }
    g_fs_npool--;
}

/* -------------------------------------------------------------------------- */

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
static int  gui_build_drivers(void);
static int  gui_build_units(const char *emptyText);
static void gui_set_units(void);
static void gui_driver_picked(int drvIdx);    /* Driver cycle clicked: confirm, then select */
static void gui_unit_picked(int unitIdx);     /* Units row clicked: confirm, then select */
static void gui_scan_selected(void);          /* probe the target driver (Scan button) */
static void gui_refresh_current(void);        /* re-probe the currently shown device+unit */
static int  gui_resize_dialog(int index);     /* grow/shrink the selected partition */
static void gui_format(int index);            /* format selected partition into empty volume */
static void gui_quick_setup(void);            /* the Quick setup wizard */
static void gui_quick_hide(void);             /* a driver was picked: the panel goes for good */
static void gui_draw_chrome(void);        /* BevelBox group frames + captions */
void gui_draw_bar(void);
static void gui_map_draw(struct RastPort *rp, const RdbModel *m, int x, int y, int w, int h, int sel, int rdb_label, const uint32_t *vols);
static void gui_draw_table(void);         /* partition table rows */
static void gui_draw_units(void);         /* Units list rows + meters */
static void gui_draw_details(void);       /* details of the selected partition */
static void gui_list_changed(ListGeom *l, int gid);
static void gui_list_scrolled(ListGeom *l, int top);
static int  gui_list_hit(const ListGeom *l, int mx, int my);
static void gui_load_pens(void);
static void gui_draw_easter(void);        /* the bottom-right pi glyph */
static int  gui_hit_easter(int mx, int my);
static void gui_set_selection(int idx);   /* select partition `idx` (or -1) + redraw */
static int  gui_part_at_x(int mx, int my);/* partition under a window-relative point, or -1 */

/* Tiny string compare (freestanding: no libc strcmp). */
static int streq(const char *a, const char *b)
{ int i; for (i = 0; a[i] && a[i] == b[i]; i++) ; return a[i] == 0 && b[i] == 0; }

/* Set the status-line text. The footer holds 33 characters. */
#define STATUS_CHARS 33
static void gui_status(const char *s)
{
    int p = 0;
    while (s[p] && p < STATUS_CHARS) { g_statusbuf[p] = s[p]; p++; }
    g_statusbuf[p] = 0;
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
}

/* FULLMENUNUM indices (menu,item) for enable/disable + dispatch readability. */
enum { MN_PROJECT=0, MN_DISK=1, MN_PART=2 };
enum { IT_ABOUT=0, IT_ASL=1, IT_SAVE=3, IT_QUIT=5 };     /* Project items */
enum { ID_SCAN=0, ID_LOAD=1, ID_REFRESH=3, ID_INIT=4, ID_FS=5, ID_QUICK=6 };  /* Disk items   */
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
    gui_menu_enable(MN_PROJECT, IT_ASL,     !AslBase);   /* nothing to do once loaded */
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

/* Rebuild the partition rows, status text, map, table and details from g_model. */
static void gui_refresh_parts(void)
{
    int i, count = 0;
    if (g_have_model) {
        for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
            RdbPartition *pt = &g_model.parts[i];
            char *row = g_partrows[i];
            int p = 0, k;
            for (k = 0; pt->name[k] && k < 11; k++) row[p++] = pt->name[k];
            s_pad(row, &p, 12);
            { char tb[16]; fstype_label(tb, pt->dos_type); s_cat(row, &p, tb); }
            s_pad(row, &p, 24);
            p += u2s(row + p, pt->low_cyl);            s_pad(row, &p, 32);
            p += u2s(row + p, pt->high_cyl);           s_pad(row, &p, 40);
            { ULONG cyls = pt->high_cyl - pt->low_cyl + 1;
              ULONG mb = disc_blocks_to_mb(cyls * g_model.cyl_blocks, g_model.block_bytes);
              p += u2s(row + p, mb); s_cat(row, &p, " MB"); }
            row[p] = 0;
            count++;
        }
        { int p = 0;
          s_cat(g_statusbuf, &p, g_dirty ? "Modified, " : g_no_rdb ? "Blank disk, " : "");
          p += u2s(g_statusbuf + p, (ULONG)g_model.num_parts);
          s_cat(g_statusbuf, &p, " parts, ");
          p += u2s(g_statusbuf + p, g_model.cylinders); s_cat(g_statusbuf, &p, " cyl");
          g_statusbuf[p] = 0; }
    } else {
        int p = 0; s_cat(g_statusbuf, &p, "No disk selected"); g_statusbuf[p] = 0;
    }
    /* Keep the selection on screen. */
    g_plist.count = (WORD)count;
    if (g_sel_part >= 0) {
        if (g_sel_part < g_plist.top) g_plist.top = (WORD)g_sel_part;
        if (g_sel_part >= g_plist.top + g_plist.vis) g_plist.top = (WORD)(g_sel_part - g_plist.vis + 1);
    }
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
    gui_list_changed(&g_plist, GID_PARTS);
    gui_draw_bar();
    gui_draw_units();
    gui_draw_details();
}

/* Screen-top menu strip. nm_CommKey letters are the right-Amiga shortcuts.
   Scan has none (S belongs to Save). Items map 1:1 to the existing handlers. */
static struct NewMenu g_newmenu[] = {
    { NM_TITLE, "Project",       0,  0, 0, 0 },
    {  NM_ITEM, "About HDPart...",0, 0, 0, (APTR)0 },
    {  NM_ITEM, "Load asl.library...", 0, 0, 0, 0 },
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
    {  NM_ITEM, "Quick setup...", 0, 0, 0, 0 },
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
    for (i = 0; i < 18; i++) g_gad[i] = 0;

    /* CreateContext's inline macro (LP1) declares the input register as
       'volatile t1' which in GCC 14+ triggers -Wincompatible-pointer-types
       (now an error).  Suppress around this one call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&g_glist);
#pragma GCC diagnostic pop
    if (!g) return 0;

    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    /* Left column: driver cycle, Scan + Load. */
    ng.ng_LeftEdge = LEFT_X + g_leftb; ng.ng_TopEdge = 11 + g_topb; ng.ng_Width = LEFT_W; ng.ng_Height = 13;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_DEVICE;
    g_drvlabels[0] = "Select a driver"; g_drvlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_drvlabels, TAG_END);
    g_gad[GID_DEVICE] = g;

    ng.ng_TopEdge = 26 + g_topb; ng.ng_Width = 76;
    ng.ng_GadgetText = (UBYTE *)"Scan"; ng.ng_GadgetID = GID_SCAN;   /* no underscore: Scan has no shortcut */
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_SCAN] = g;

    ng.ng_LeftEdge = LEFT_X + LEFT_W - 76 + g_leftb;
    ng.ng_GadgetText = (UBYTE *)"_Load..."; ng.ng_GadgetID = GID_DRIVER;
    /* Always enabled: gui_pick_file falls back to a typed-path dialog when
       asl.library is absent (e.g. booted from the bare HDPart floppy). */
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_DRIVER] = g;

    /* Scrollers beside the two hand-drawn lists. */
    ng.ng_LeftEdge = LEFT_X + LEFT_W - SCROLL_W - 2 + g_leftb; ng.ng_TopEdge = UNITS_Y + 2 + g_topb;
    ng.ng_Width = SCROLL_W; ng.ng_Height = UNITS_H - 4;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_UNIT;
    g = CreateGadget(SCROLLER_KIND, g, &ng, GTSC_Top, 0, GTSC_Total, 0, GTSC_Visible, UNIT_ROWS,
                     GTSC_Arrows, 8, PGA_Freedom, LORIENT_VERT, GA_RelVerify, TRUE, GA_Immediate, TRUE, TAG_END);
    g_gad[GID_UNIT] = g;

    ng.ng_LeftEdge = RIGHT_X + RIGHT_W - SCROLL_W - 2 + g_leftb; ng.ng_TopEdge = TABLE_Y + 2 + g_topb;
    ng.ng_Width = SCROLL_W; ng.ng_Height = TABLE_H - 4; ng.ng_GadgetID = GID_PARTS;
    g = CreateGadget(SCROLLER_KIND, g, &ng, GTSC_Top, 0, GTSC_Total, 0, GTSC_Visible, PART_ROWS,
                     GTSC_Arrows, 8, PGA_Freedom, LORIENT_VERT, GA_RelVerify, TRUE, GA_Immediate, TRUE, TAG_END);
    g_gad[GID_PARTS] = g;

    /* Partition actions. */
    {
        static const struct { int id; const char *txt; int w; } pbtn[] = {
            { GID_NEW, "_New", 44 }, { GID_EDIT, "_Edit...", 66 }, { GID_DELETE, "_Delete", 58 },
            { GID_SPLIT, "Spli_t...", 74 }, { GID_RESIZE, "_Resize...", 80 }, { GID_FORMAT, "F_ormat...", 80 }
        };
        int k, x = RIGHT_X;
        for (k = 0; k < 6; k++) {
            ng.ng_LeftEdge = x + g_leftb; ng.ng_TopEdge = ACTION_Y + g_topb;
            ng.ng_Width = pbtn[k].w; ng.ng_Height = 13;
            ng.ng_GadgetText = (UBYTE *)pbtn[k].txt; ng.ng_GadgetID = pbtn[k].id;
            g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
            g_gad[pbtn[k].id] = g;
            x += pbtn[k].w + 4;
        }
    }

    /* Footer: Filesystems under the units; status, Refresh and Save under the table. */
    ng.ng_LeftEdge = LEFT_X + g_leftb; ng.ng_TopEdge = FOOTER_Y + g_topb; ng.ng_Width = LEFT_W; ng.ng_Height = 13;
    ng.ng_GadgetText = (UBYTE *)"Files_ystems..."; ng.ng_GadgetID = GID_FS;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_FS] = g;

    ng.ng_LeftEdge = RIGHT_X + g_leftb; ng.ng_Width = RIGHT_W - 164;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_STATUS;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"No disk selected", TAG_END);
    g_gad[GID_STATUS] = g;

    ng.ng_LeftEdge = RIGHT_X + RIGHT_W - 160 + g_leftb; ng.ng_Width = 76;
    ng.ng_GadgetText = (UBYTE *)"Re_fresh"; ng.ng_GadgetID = GID_REFRESH;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_REFRESH] = g;

    ng.ng_LeftEdge = RIGHT_X + RIGHT_W - 80 + g_leftb; ng.ng_Width = 80;
    ng.ng_GadgetText = (UBYTE *)"_Save"; ng.ng_GadgetID = GID_SAVE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_SAVE] = g;

    /* Last in the chain: gui_quick_hide takes it off the end, leaving g_glist whole. */
    ng.ng_LeftEdge = RIGHT_X + (RIGHT_W - QP_BTN_W) / 2 + g_leftb; ng.ng_TopEdge = DETAIL_Y + QP_BTN_Y + g_topb;
    ng.ng_Width = QP_BTN_W; ng.ng_Height = QP_BTN_H;
    ng.ng_GadgetText = (UBYTE *)"Quick setup..."; ng.ng_GadgetID = GID_QUICK;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_QUICK] = g;

    return g;
}

/* Draw a multi-line ('\n'-separated) message into a window's RastPort. */
static void gui_draw_text(struct Window *w, const char *body, int x, int y)
{
    struct RastPort *rp = w->RPort;
    const char *line = body, *p = body;
    int ly = y + 7;                 /* first baseline */
    SetAPen(rp, g_pen_text);
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
/* The dialog window currently on top, or NULL. Only used to parent the ASL
   requester (see gui_pick_file); dlg_close always clears it, so it is either a
   live window or NULL — a nested dialog just falls back to the main window. */
static struct Window *g_dlgwin = 0;

static __attribute__((noinline)) struct Window *dlg_open_flags(const char *title, int L, int T, int w, int h,
                                     ULONG idcmp, struct Gadget *glist, ULONG flags)
{
    g_dlgwin = OpenWindowTags(0,
        WA_Left, L, WA_Top, T, WA_Width, w, WA_Height, h,
        WA_Title, (ULONG)title, WA_Gadgets, (ULONG)glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | idcmp,
        WA_Flags, WFLG_DRAGBAR | WFLG_ACTIVATE | WFLG_SMART_REFRESH | flags,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
    if (g_dlgwin && g_sfont) SetFont(g_dlgwin->RPort, g_sfont);
    return g_dlgwin;
}
static __attribute__((noinline)) struct Window *dlg_open(const char *title, int L, int T, int w, int h,
                               ULONG idcmp, struct Gadget *glist)
{
    return dlg_open_flags(title, L, T, w, h, idcmp, glist, WFLG_DEPTHGADGET);
}
static void dlg_refresh(struct Window *w) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
static void dlg_close(struct Window *w, struct Gadget *glist)
{
    if (w == g_dlgwin) g_dlgwin = 0;
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

/* Switching driver or disk replaces g_model, so ask before dropping unsaved
   edits. Returns 1 if there is nothing to lose or the user agreed. */
static int gui_discard_ok(const char *title)
{
    return !g_dirty || gui_confirm(title, "Discard unsaved changes to this disk?");
}

/* Fallback for when asl.library is unavailable (e.g. booting the bare HDPart
   floppy, which carries no LIBS:asl.library): a one-field modal dialog to type
   a full file path. Pre-fills `init` (a starting drawer); returns 1 with the
   typed path in out[] on Ok, 0 on Cancel/close. */
static int gui_string_dialog(const char *title, const char *prompt,
                             const char *init, char *out, int outsz)
{
    struct Window *dw;
    struct Gadget *glist = 0, *g, *gStr;
    struct NewGadget ng;
    static char pathbuf[256];
    int dt = g_topb, dl = g_leftb;
    int dwW, dwH, dwL = 0, dwT = 0, btnY;
    int done = 0, result = 0, j;

    for (j = 0; init && init[j] && j < (int)sizeof(pathbuf) - 1; j++) pathbuf[j] = init[j];
    pathbuf[j] = 0;

    dwW  = dl + 360 + 24 + g_scr->WBorRight;
    btnY = dt + 6 + 10 + 6 + 14 + 8;          /* prompt + string + gap */
    dwH  = btnY + 14 + 8 + g_scr->WBorBottom;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;
    ng.ng_LeftEdge = dl + 12; ng.ng_TopEdge = dt + 6 + 10;
    ng.ng_Width = 360; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)""; ng.ng_GadgetID = 20;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)pathbuf,
                     GTST_MaxChars, outsz - 1 < 255 ? outsz - 1 : 255, TAG_END);
    gStr = g;
    ng.ng_TopEdge = btnY; ng.ng_Width = 90; ng.ng_Height = 14;
    ng.ng_LeftEdge = dl + 12;             ng.ng_GadgetText = (UBYTE *)"Ok";     ng.ng_GadgetID = 21;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 360 + 12 - 90;  ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 22;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(glist); return 0; }

    dlg_center(dwW, dwH, &dwL, &dwT);
    dw = dlg_open(title, dwL, dwT, dwW, dwH, BUTTONIDCMP | STRINGIDCMP, glist);
    if (!dw) { FreeGadgets(glist); return 0; }
    GT_RefreshWindow(dw, 0);
    gui_draw_text(dw, prompt, dl + 12, dt + 6);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { result = 0; done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(dw);
                gui_draw_text(dw, prompt, dl + 12, dt + 6);
                GT_EndRefresh(dw, TRUE);
            } else if (cl == IDCMP_GADGETUP) {
                if (ig->GadgetID == 21) {            /* Ok */
                    const char *s = (const char *)((struct StringInfo *)gStr->SpecialInfo)->Buffer;
                    int k = 0;
                    while (s[k] && k < outsz - 1) { out[k] = s[k]; k++; }
                    out[k] = 0;
                    result = (out[0] != 0);          /* empty path = cancel */
                    done = 1;
                } else if (ig->GadgetID == 22) { result = 0; done = 1; }
            }
        }
    }
    dlg_close(dw, glist);
    return result;
}

static int gui_path_dialog(const char *title, const char *init,
                           char *out, int outsz)
{
    return gui_string_dialog(title, "Enter the full file path:", init, out, outsz);
}

/* Prompt for a volume name (label written by ACTION_FORMAT). Pre-fills with the
   partition's RDB name. Returns 1 with the name in out[], 0 on Cancel. */
static int gui_volname_dialog(const char *deflt, char *out, int outsz)
{
    return gui_string_dialog("Volume name", "Enter a volume name:",
                             deflt, out, outsz);
}

/* Pick a file: an ASL requester when asl.library is present, otherwise the
   typed-path fallback. `init` is the starting drawer (e.g. "DEVS:", "L:").
   Returns 1 with the full path in out[], 0 on cancel. */
static int gui_pick_file(const char *title, const char *init, char *out, int outsz)
{
    struct FileRequester *fr;
    int ok = 0;

    if (!AslBase) return gui_path_dialog(title, init, out, outsz);

    /* ASLFR_Window is the ONE way a V37 asl.library can be told where to open:
       it is tag ASL_TB+2, the old V37 ASL_Window, whereas ASLFR_Screen (ASL_TB+40)
       is V38+ and a V37 library ignores it — it then tries the default public
       screen, and on a Workbench-less boot there is none, so AslRequest() simply
       returned FALSE and the button appeared to do nothing. Parent to whichever
       window is in front (a dialog, e.g. Filesystems, else the main window) so
       the requester also cannot open behind it. The last three tags are V38/V44/
       V45 and are ignored by older libraries. */
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)title,
            ASLFR_InitialDrawer, (ULONG)init,
            ASLFR_Window,        (ULONG)(g_dlgwin ? g_dlgwin : g_win),
            ASLFR_Screen,        (ULONG)g_scr,   /* V38+: used when no window */
            ASLFR_SleepWindow,   TRUE,
            ASLFR_PopToFront,    TRUE,
            ASLFR_Activate,      TRUE,
            TAG_END);
    if (!fr) { gui_msg(title, "Could not open the file requester."); return 0; }
    if (AslRequest(fr, 0)) {
        int j = 0; const char *d = (const char *)fr->fr_Drawer;
        while (d && d[j] && j < outsz - 1) { out[j] = d[j]; j++; }
        out[j] = 0;
        AddPart((STRPTR)out, (CONST_STRPTR)fr->fr_File, outsz);
        ok = 1;
    }
    FreeAslRequest(fr);
    return ok;
}

/* Case-insensitive test that `s` ends in ".library". */
static int ends_in_library(const char *s)
{
    static const char suf[] = ".library";
    int n = 0, i;
    while (s[n]) n++;
    if (n < 8) return 0;
    for (i = 0; i < 8; i++) {
        char c = s[n - 8 + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suf[i]) return 0;
    }
    return 1;
}

/* Open asl.library out of `drawer` by making LIBS: resolve it. OpenLibrary only
   ever searches LIBS: (and RAM:), so pointing LIBS: at the drawer is the way to
   load a library from an arbitrary place. The drawer is probed for the file
   FIRST so a wrong path never leaves a junk assign behind. AssignAdd appends to
   an existing LIBS: (never clobbering a real Workbench one); AssignLock creates
   it when there is none. Both consume the lock on success — UnLock only on
   failure. Returns 1 if AslBase is open afterwards. */
static int asl_open_from_drawer(const char *drawer)
{
    static char probe[256];
    BPTR libsdir, existing, f;
    LONG ok;
    int i = 0;

    if (AslBase) return 1;
    while (drawer[i] && i < (int)sizeof(probe) - 1) { probe[i] = drawer[i]; i++; }
    probe[i] = 0;
    if (!AddPart((STRPTR)probe, (CONST_STRPTR)"asl.library", sizeof(probe))) return 0;
    f = Lock((CONST_STRPTR)probe, SHARED_LOCK);
    if (!f) return 0;                       /* no asl.library in that drawer */
    UnLock(f);

    libsdir = Lock((CONST_STRPTR)drawer, SHARED_LOCK);
    if (!libsdir) return 0;
    existing = Lock((CONST_STRPTR)"LIBS:", SHARED_LOCK);
    if (existing) { UnLock(existing); ok = AssignAdd((STRPTR)"LIBS", libsdir); }
    else          { ok = AssignLock((STRPTR)"LIBS", libsdir); }
    if (!ok) { UnLock(libsdir); return 0; }
    AslBase = OpenLibrary("asl.library", 37);
    return AslBase ? 1 : 0;
}

/* Project/Load asl.library... — HDPart bundles no library of its own, so on a
   bare floppy boot (no system LIBS:) the file requester is unavailable and the
   pickers fall back to typed paths. This lets the user point HDPart at any
   asl.library they do have — on a Workbench disk, another partition, wherever —
   and get the real requester for the rest of the session. Accepts the drawer or
   the full path to the library file. */
static void gui_load_asl(void)
{
    static char path[256];
    int n = 0, cut = -1, i;

    if (AslBase) { gui_msg("Load asl.library", "asl.library is already loaded."); return; }
    if (!gui_string_dialog("Load asl.library", "Drawer holding asl.library:",
                           "SYS:Libs", path, sizeof(path)))
        return;

    /* Trim a trailing "...library" so a full file path works too: keep a ':'
       (it is part of the volume name), drop a '/'. */
    while (path[n]) n++;
    for (i = 0; i < n; i++) if (path[i] == '/' || path[i] == ':') cut = i;
    if (ends_in_library(path + cut + 1))
        path[(cut < 0) ? 0 : (path[cut] == ':' ? cut + 1 : cut)] = 0;
    if (!path[0]) { const char *pd = "PROGDIR:"; for (i = 0; pd[i]; i++) path[i] = pd[i]; path[i] = 0; }

    if (asl_open_from_drawer(path)) {
        gui_status("asl.library loaded.");
        gui_update_buttons();
    } else {
        gui_msg("Load asl.library", "No asl.library could be opened from\nthat drawer.");
    }
}

static char g_msgbuf[120];

/* Stage the pool filesystems a partition references into g_model.fs[], ready to
   serialize. Deep-copies so pool and model never alias seg_data. All-or-nothing:
   every copy is allocated before g_model.fs[] is touched, so running out of
   memory returns 0 with the model intact rather than writing an RDB that is
   missing a handler its partitions need. */
static int gui_stage_fs(void)
{
    uint8_t *seg[RDB_MAX_FS];
    int src[RDB_MAX_FS];
    int n = 0, pi, qi;

    for (pi = 0; pi < g_fs_npool && n < RDB_MAX_FS; pi++) {
        uint32_t len = g_fs_pool[pi].seg_len, z;
        int used = 0;
        for (qi = 0; qi < g_model.num_parts; qi++)
            if (g_model.parts[qi].dos_type == g_fs_pool[pi].dos_type) { used = 1; break; }
        if (!used) continue;
        seg[n] = (uint8_t *)rdb_seg_alloc(len ? len : 1);
        if (!seg[n]) {
            while (n > 0) rdb_seg_free(seg[--n]);
            return 0;
        }
        for (z = 0; z < len; z++) seg[n][z] = g_fs_pool[pi].seg_data[z];
        src[n++] = pi;
    }

    rdb_model_free(&g_model);   /* clear stale fs[] staging; parts[] untouched */
    for (qi = 0; qi < n; qi++) {
        g_model.fs[qi] = g_fs_pool[src[qi]];
        g_model.fs[qi].seg_data = seg[qi];
    }
    g_model.num_fs = n;
    return 1;
}

/* The table read back by qs_write_verified. Static: keep off the stack. */
static RdbModel g_wv_chk;

enum { WV_OK = 0, WV_NO_DEVICE, WV_NO_SPACE, WV_WRITE_FAILED, WV_MISMATCH };

/* Write a table to one disk and read it back: the part of Save the Quick setup wizard shares.
   Touches the model's setup_pending not at all: each caller decides that for itself. The read
   table stays in g_wv_chk. `written`, if given, is called between the write and the read-back. */
static int qs_write_verified(const char *driver, ULONG unit, RdbModel *m, void (*written)(void))
{
    DeviceHandle *h = dev_open(driver, unit);
    int serr, rc = WV_WRITE_FAILED;

    if (!h) return WV_NO_DEVICE;
    serr = rdb_serialize(m, dev_block_io, h);
    if (serr == RDB_OK) {
        if (written) written();
        rc = WV_MISMATCH;
        /* read back and verify the partition count + first/last cylinders */
        if (rdb_parse(&g_wv_chk, dev_block_io, h) == RDB_OK &&
            g_wv_chk.num_parts == m->num_parts) {
            int i; rc = WV_OK;
            for (i = 0; i < g_wv_chk.num_parts; i++)
                if (g_wv_chk.parts[i].low_cyl  != m->parts[i].low_cyl ||
                    g_wv_chk.parts[i].high_cyl != m->parts[i].high_cyl) { rc = WV_MISMATCH; break; }
        }
        rdb_model_free(&g_wv_chk);
    } else if (serr == RDB_ERR_NO_RDB_SPACE) rc = WV_NO_SPACE;
    dev_close(h);
    return rc;
}

static int gui_save(void)
{
    int ok, wv, p = 0;

    /* A save from the normal UI always drops the Quick setup marker, even if
       this save then fails: the user is editing by hand now. */
    g_model.setup_pending = 0;
    if (!g_have_model) return 0;
    if (rdb_validate(&g_model) != RDB_OK) { gui_msg("Save", "Partition layout is invalid."); return 0; }

    {
        static char snames[8][8];
        int snn = 0;
        DevLiveness slv = safety_classify(g_cur_driver, g_cur_unit, snames, 8, &snn);
        if (slv == DEV_BOOT) {
            /* You booted from this device. This is dangerous, but an advanced
               user may know what they're doing (e.g. only editing a non-system
               partition's table), so warn hard and let them override rather than
               block outright. Changes typically take effect only after reboot. */
            if (!gui_confirm("Save",
                    "WARNING: you booted from this device.\n"
                    "Writing a new partition table can corrupt\n"
                    "the running system, and changes may not\n"
                    "take effect until you reboot.\n"
                    "Only proceed if you know what you are doing.\n"
                    "Write anyway?"))
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

    if (!gui_stage_fs()) {
        gui_msg("Save", "Not enough memory for the embedded\nfilesystem(s). Nothing was written.");
        return 0;
    }

    wv = qs_write_verified(g_cur_driver, g_cur_unit, &g_model, 0);
    if (wv == WV_NO_DEVICE) { gui_msg("Save", "Could not open the device."); return 0; }
    ok = wv == WV_OK;

    if (ok) { g_dirty = 0; gui_msg("Save", "Saved and verified."); }
    else if (wv == WV_NO_SPACE)
        gui_msg("Save", "Embedded filesystem(s) don't fit the reserved RDB area.\nRemove one or use a smaller handler. Nothing was written.");
    else gui_msg("Save", "WRITE FAILED or verify mismatch.\nThe disk RDB may be inconsistent.");
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
/* Presets for the advanced settings. The cycle shows the short NAME; the adjacent
   field always holds the effective value (filled + disabled for a preset, editable
   for Custom). Values and wording come from disassembling the Kickstart ROMs, PFS3
   and SFS: docs/reference/rom-findings-maxtransfer-mask-buffers.md. */
static const char *const kMaxTLabels[] = { "IDE safe", "SCSI", "No limit", "Custom", 0 };
static const uint32_t    kMaxTValues[] = { 0x0001FE00u, 0x00FFFFFFu, 0x7FFFFFFFu };
static const char *const kMaxTHelp[]   = {
    "Safe on every disk",
    "SCSI controllers",
    "3.1.4+ IDE, or drivers that split",
    "Too high can silently corrupt data"
};
#define N_MAXT 3

static const char *const kMaskLabels[] = { "Default", "Zorro II DMA", "No mask", "Long-aligned", "Custom", 0 };
static const uint32_t    kMaskValues[] = { 0x7FFFFFFEu, 0x00FFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFCu };
static const char *const kMaskHelp[]   = {
    "IDE, A3000 and 32-bit DMA",
    "24-bit DMA: see Buffer memory too",
    "IDE and A3000 only",
    "Only if your controller asks for it",
    "Advanced - enter a hex value"
};
#define N_MASK 4

static const char *const kBufLabels[] = { "Default", "Low memory", "More cache", "Custom", 0 };
static const uint32_t    kBufValues[] = { 30u, 10u, 100u };
static const char *const kBufHelp[]   = {
    "FFS ~17 KB; PFS3 uses 150, ~155 KB",
    "FFS ~6 KB; PFS3 ~11 KB; SFS uses 32",
    "FFS ~56 KB, PFS3 ~106, SFS ~54",
    "Type a number, 1 or more"
};
#define N_BUF 3

static const char *const kMemLabels[] = { "Any", "Chip", "Custom", 0 };
static const uint32_t    kMemValues[] = { 0u, 3u };
static const char *const kMemHelp[]   = {
    "IDE and 32-bit DMA",
    "Zorro II DMA with RAM above 16 MB",
    "Advanced - enter a hex value"
};
#define N_MEM 2

/* The long explanations behind the "?" buttons. */
static const char kMaxTInfo[] =
    "MaxTransfer: the largest single read or write\n"
    "the filesystem asks the driver for.\n"
    "\n"
    "Too high can silently corrupt data with FFS or\n"
    "SFS on IDE before OS 3.1.4: no error shows.\n"
    "The 3.1.4 and later drivers handle any length.\n"
    "PFS3 limits itself on scsi.device.\n"
    "\n"
    "Too low only makes large transfers slower, so\n"
    "IDE safe is the safe choice if unsure.";
static const char kMaskInfo[] =
    "Mask: which addresses the controller can reach\n"
    "directly.\n"
    "\n"
    "A transfer outside the mask goes through another\n"
    "buffer: slower, but it still works. Onboard IDE\n"
    "and the A3000 do not depend on it.\n"
    "\n"
    "A Zorro II DMA card needs Zorro II DMA here, and\n"
    "Buffer memory set so FFS buffers are in reach:\n"
    "FFS does not apply the mask to its own buffers.";
static const char kBufInfo[] =
    "Buffers: how many blocks the filesystem keeps in\n"
    "memory. More uses more RAM, per buffer about:\n"
    "FFS 0.6 KB, PFS3 1 KB, SFS 0.5 KB.\n"
    "\n"
    "PFS3 uses at least 10 and at most 600, and treats\n"
    "exactly 30 as 150. SFS uses at least 32.\n"
    "If RAM runs out, FFS keeps what it got, but PFS3\n"
    "and SFS can fail to set up.\n"
    "\n"
    "Speed was not measured, so no value is promised\n"
    "to be the fastest.";
static const char kMemInfo[] =
    "Buffer memory: the memory type the filesystem\n"
    "allocates its buffers from.\n"
    "\n"
    "Any suits IDE and 32-bit DMA.\n"
    "Chip is for a Zorro II DMA card in a machine with\n"
    "RAM above 16 MB. Chip memory is slower, and FFS\n"
    "does not apply Mask to its own buffers.";

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

/* Group frames for the Edit dialog: identity, boot, and advanced transfer settings. */
#define ED_W 408
static void edit_chrome(struct Window *dw)
{
    struct RastPort *rp = dw->RPort;
    int x = g_leftb + 4, w = ED_W - 8;
    DrawBevelBox(rp, x, g_topb + 3,  w, 56, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    DrawBevelBox(rp, x, g_topb + 61, w, 20, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    DrawBevelBox(rp, x, g_topb + 83, w, 128, GT_VisualInfo, (ULONG)g_vi, TAG_END);
}

/* Same as preset_apply, for the decimal Buffers field. */
static void buffers_apply(struct Window *dw, struct Gadget *field, struct Gadget *help, int idx)
{
    if (idx < N_BUF)
        GT_SetGadgetAttrs(field, dw, 0, GTIN_Number, (ULONG)kBufValues[idx], GA_Disabled, TRUE, TAG_END);
    else
        GT_SetGadgetAttrs(field, dw, 0, GA_Disabled, FALSE, TAG_END);
    GT_SetGadgetAttrs(help, dw, 0, GTTX_Text, (ULONG)kBufHelp[idx], TAG_END);
}

/* One advanced setting: label + preset cycle, value field, "?" button, help line.
   `field` is created by the caller, which knows its kind. Returns the cycle. */
static struct Gadget *edit_row(struct Gadget **g, struct NewGadget *ng, int y, const char *label,
                               int cycId, int qId, const char *const *labels, int active)
{
    struct Gadget *cyc;
    ng->ng_LeftEdge = g_leftb + 104; ng->ng_TopEdge = g_topb + y; ng->ng_Width = 110; ng->ng_Height = 14;
    ng->ng_GadgetText = (UBYTE *)label; ng->ng_GadgetID = cycId;
    cyc = *g = CreateGadget(CYCLE_KIND, *g, ng, GTCY_Labels, (ULONG)labels, GTCY_Active, (ULONG)active, TAG_END);
    ng->ng_LeftEdge = g_leftb + 376; ng->ng_Width = 20; ng->ng_GadgetText = (UBYTE *)"?"; ng->ng_GadgetID = qId;
    *g = CreateGadget(BUTTON_KIND, *g, ng, TAG_END);
    ng->ng_LeftEdge = g_leftb + 218; ng->ng_Width = 100; ng->ng_GadgetText = 0;
    return cyc;
}

static int gui_edit_dialog(int index)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g;
    struct Gadget *gName = 0, *gSize = 0, *gBoot = 0, *gPri = 0, *gMaxT = 0, *gMask = 0;
    struct Gadget *gMaxTCyc = 0, *gMaskCyc = 0, *gMaxTHelp = 0, *gMaskHelp = 0;
    struct Gadget *gFsCyc = 0, *gIntl = 0, *gCache = 0;
    struct Gadget *gBufCyc = 0, *gBuf = 0, *gBufHelp = 0, *gMemCyc = 0, *gMem = 0, *gMemHelp = 0;
    struct NewGadget ng;
    RdbPartition *pt = &g_model.parts[index];
    static char nameBuf[32];
    static char sizeMaxLabel[24];
    static char maxtBuf[12], maskBuf[12], memBuf[12];
    static const char *fsLabels[4 + RDB_MAX_FS];   /* FFS,OFS,[pool*8],[keep],NULL */
    static int fsPoolIdx[4 + RDB_MAX_FS];
    static char fsKeep[12];
    static char fsPoolLbl[RDB_MAX_FS][12];  /* stable DosType label strings for cycle */
    int maxtIdx, maskIdx, bufIdx, memIdx;
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
    u32_to_hex(memBuf, pt->bufmemtype);
    bufIdx  = preset_index(kBufValues, N_BUF, pt->num_buffers);
    memIdx  = preset_index(kMemValues, N_MEM, pt->bufmemtype);

    /* Filesystem: ROM DOS\x decodes to FFS/OFS + Intl + DirCache bits; anything
       else (PFS3/SFS/custom) gets a read-only 'keep' entry that preserves it.
       DirCache is KS3.0+ (exec V39). */
    v39   = (SysBase->LibNode.lib_Version >= 39);
    fsRom = ((pt->dos_type & 0xFFFFFF00u) == 0x444F5300u) && ((pt->dos_type & 0xFFu) <= 7);
    { int n = 0, kk;
      fsLabels[n++] = "FFS";
      fsLabels[n++] = "OFS";
      /* Pool entries from g_fs_pool (PFS3, SFS, etc.) — label shows DosType (#3 fix). */
      fsPoolBase = n;
      for (kk = 0; kk < g_fs_npool && n < (int)(sizeof fsLabels / sizeof fsLabels[0]) - 2; kk++) {
          dostype_label(fsPoolLbl[kk], g_fs_pool[kk].dos_type);
          fsLabels[n] = fsPoolLbl[kk];
          fsPoolIdx[n] = kk;
          n++;
      }
      /* keep entry: only if not a ROM DOS\x AND not already in pool */
      if (!fsRom) {
          int inPool = 0, kn;
          for (kn = fsPoolBase; kn < n; kn++) {
              if (g_fs_pool[fsPoolIdx[kn]].dos_type == pt->dos_type) { inPool = 1; break; }
          }
          if (!inPool) { dostype_label(fsKeep, pt->dos_type); fsKeepIdx = n; fsLabels[n++] = fsKeep; }
          else fsKeepIdx = -1;
      } else { fsKeepIdx = -1; }
      fsLabels[n] = 0; }
    /* Initial selection: prefer pool match, then ROM FFS/OFS, then keep */
    { int kn, matched = 0;
      for (kn = fsPoolBase; kn < fsPoolBase + g_fs_npool; kn++) {
          if (g_fs_pool[fsPoolIdx[kn]].dos_type == pt->dos_type) {
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

    /* Fields share one left edge (x=104) with labels to their left. */
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 7; ng.ng_Width = 292; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Name"; ng.ng_GadgetID = 1;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)nameBuf, GTST_MaxChars, 31, TAG_END);
    gName = g;

    /* "max <N> MB" hint shown beside the size field */
    { int p = 0; sizeMaxLabel[p++]='m'; sizeMaxLabel[p++]='a'; sizeMaxLabel[p++]='x'; sizeMaxLabel[p++]=' ';
      p += u2s(sizeMaxLabel + p, maxMB); sizeMaxLabel[p++]=' '; sizeMaxLabel[p++]='M'; sizeMaxLabel[p++]='B';
      sizeMaxLabel[p] = 0; }

    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 23; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Size (MB)"; ng.ng_GadgetID = 2;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, curMB, GTIN_MaxChars, 7, TAG_END);
    gSize = g;

    ng.ng_LeftEdge = dl + 192; ng.ng_Width = 110; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)sizeMaxLabel, TAG_END);

    /* Filesystem: FFS/OFS (+ pool and 'keep') cycle, plus Intl and DirCache. */
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 40; ng.ng_Width = 110; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Filesystem"; ng.ng_GadgetID = 4;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)fsLabels, GTCY_Active, (ULONG)fsActive, TAG_END);
    gFsCyc = g;

    ng.ng_LeftEdge = dl + 268; ng.ng_TopEdge = dt + 41; ng.ng_Width = 26; ng.ng_Height = 11;
    ng.ng_GadgetText = (UBYTE *)"Intl"; ng.ng_GadgetID = 14;
    { int ckDis = !fsRom || (fsActive >= fsPoolBase);
      g = CreateGadget(CHECKBOX_KIND, g, &ng,
                       GTCB_Checked, (ULONG)((fsRom && (pt->dos_type & 2u)) ? TRUE : FALSE),
                       GA_Disabled,  (ULONG)ckDis, TAG_END); }
    gIntl = g;

    ng.ng_LeftEdge = dl + 362; ng.ng_TopEdge = dt + 41; ng.ng_Width = 26; ng.ng_Height = 11;
    ng.ng_GadgetText = (UBYTE *)"Cache"; ng.ng_GadgetID = 15;
    { int ckDis = !fsRom || !v39 || (fsActive >= fsPoolBase);
      g = CreateGadget(CHECKBOX_KIND, g, &ng,
                       GTCB_Checked, (ULONG)((fsRom && v39 && (pt->dos_type & 4u)) ? TRUE : FALSE),
                       GA_Disabled,  (ULONG)ckDis, TAG_END); }
    gCache = g;

    /* Boot group. */
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 66; ng.ng_Width = 26; ng.ng_Height = 11;
    ng.ng_GadgetText = (UBYTE *)"Bootable"; ng.ng_GadgetID = 5;
    g = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, (ULONG)(pt->bootable ? TRUE : FALSE), TAG_END);
    gBoot = g;

    ng.ng_LeftEdge = dl + 268; ng.ng_TopEdge = dt + 64; ng.ng_Width = 50; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Boot Pri"; ng.ng_GadgetID = 6;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, (ULONG)(LONG)pt->boot_pri, GTIN_MaxChars, 6, TAG_END);
    gPri = g;

    /* Advanced settings: preset + value + "?" on one line, help line beneath. */
    gMaxTCyc = edit_row(&g, &ng, 87, "MaxTransfer", 7, 20, kMaxTLabels, maxtIdx);
    ng.ng_GadgetID = 12;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)maxtBuf, GTST_MaxChars, 11,
                     GA_Disabled, (ULONG)(maxtIdx < N_MAXT), TAG_END);
    gMaxT = g;
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 102; ng.ng_Width = 292; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)kMaxTHelp[maxtIdx], TAG_END);
    gMaxTHelp = g;

    gMaskCyc = edit_row(&g, &ng, 118, "Mask", 8, 21, kMaskLabels, maskIdx);
    ng.ng_GadgetID = 13;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)maskBuf, GTST_MaxChars, 11,
                     GA_Disabled, (ULONG)(maskIdx < N_MASK), TAG_END);
    gMask = g;
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 133; ng.ng_Width = 292; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)kMaskHelp[maskIdx], TAG_END);
    gMaskHelp = g;

    gBufCyc = edit_row(&g, &ng, 149, "Buffers", 16, 22, kBufLabels, bufIdx);
    ng.ng_GadgetID = 17;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, (ULONG)pt->num_buffers, GTIN_MaxChars, 6,
                     GA_Disabled, (ULONG)(bufIdx < N_BUF), TAG_END);
    gBuf = g;
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 164; ng.ng_Width = 292; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)kBufHelp[bufIdx], TAG_END);
    gBufHelp = g;

    gMemCyc = edit_row(&g, &ng, 180, "Buffer mem", 18, 23, kMemLabels, memIdx);
    ng.ng_GadgetID = 19;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)memBuf, GTST_MaxChars, 11,
                     GA_Disabled, (ULONG)(memIdx < N_MEM), TAG_END);
    gMem = g;
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 195; ng.ng_Width = 292; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)kMemHelp[memIdx], TAG_END);
    gMemHelp = g;

    ng.ng_LeftEdge = dl + 4; ng.ng_TopEdge = dt + 214; ng.ng_Width = 90; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + ED_W - 94; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return 0; }

    {   int dwW = dl + ED_W + g_scr->WBorRight;
        int dwH = dt + 232 + g_scr->WBorBottom;
        int dwL = 0, dwT = 0;
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open("Edit Partition", dwL, dwT, dwW, dwH,
                      BUTTONIDCMP | STRINGIDCMP | INTEGERIDCMP | CHECKBOXIDCMP | CYCLEIDCMP, dglist);
    }
    if (!dw) { FreeGadgets(dglist); return 0; }
    GT_RefreshWindow(dw, 0);
    edit_chrome(dw);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            UWORD code = im->Code;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(dw); edit_chrome(dw); GT_EndRefresh(dw, TRUE);
            }
            else if (cl == IDCMP_GADGETUP) {
                if (ig == gMaxTCyc) {
                    preset_apply(dw, gMaxT, gMaxTHelp, (int)code, kMaxTValues, N_MAXT, kMaxTHelp);
                } else if (ig == gMaskCyc) {
                    preset_apply(dw, gMask, gMaskHelp, (int)code, kMaskValues, N_MASK, kMaskHelp);
                } else if (ig == gBufCyc) {
                    buffers_apply(dw, gBuf, gBufHelp, (int)code);
                } else if (ig == gMemCyc) {
                    preset_apply(dw, gMem, gMemHelp, (int)code, kMemValues, N_MEM, kMemHelp);
                } else if (ig->GadgetID >= 20 && ig->GadgetID <= 23) {
                    static const char *const info[] = { kMaxTInfo, kMaskInfo, kBufInfo, kMemInfo };
                    static const char *const head[] = { "MaxTransfer", "Mask", "Buffers", "Buffer memory" };
                    gui_msg(head[ig->GadgetID - 20], info[ig->GadgetID - 20]);
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
                    const char *mm = (const char *)((struct StringInfo *)gMem->SpecialInfo)->Buffer;
                    LONG bufs = ((struct StringInfo *)gBuf->SpecialInfo)->LongInt;
                    uint32_t mtv, mkv, mmv;
                    int r;
                    if (!parse_hex32(mt, &mtv) || !parse_hex32(mk, &mkv) || !parse_hex32(mm, &mmv)) {
                        gui_msg("Edit", "MaxTransfer, Mask and Buffer memory\nmust be hex (e.g. 0x7FFFFFFE).");
                    } else if (mtv < g_model.block_bytes || mtv >= 0x80000000u) {
                        /* Below a block PFS3 can loop forever; at the top bit FFS 40.1
                           and earlier compare signed and can overrun the caller's buffer. */
                        gui_msg("Edit", "MaxTransfer must be at least one block\nand below 0x80000000.");
                    } else if (bufs < 1) {
                        gui_msg("Edit", "Buffers must be 1 or more.");
                    } else {
                        /* Compute the DosType from the FS controls (keep entry
                           preserves a non-ROM type). */
                        uint32_t fdt;
                        if (fsKeepIdx >= 0 && fsIdx == fsKeepIdx) {
                            fdt = pt->dos_type;
                        } else if (fsIdx >= fsPoolBase && (fsKeepIdx < 0 || fsIdx < fsKeepIdx)) {
                            fdt = g_fs_pool[fsPoolIdx[fsIdx]].dos_type;  /* pool FS */
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
                            pt->num_buffers = (uint32_t)bufs;
                            pt->bufmemtype  = mmv;
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

/* Group frames for the Resize dialog: what it is now, what it becomes, the caveat. */
#define RS_W 360
static void resize_chrome(struct Window *dw)
{
    struct RastPort *rp = dw->RPort;
    int x = g_leftb + 4, w = RS_W - 8;
    DrawBevelBox(rp, x, g_topb + 3,  w, 31, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    DrawBevelBox(rp, x, g_topb + 36, w, 54, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    DrawBevelBox(rp, x, g_topb + 92, w, 17, GT_VisualInfo, (ULONG)g_vi, TAG_END);
}

/* "cyl 5 to 8 (4 cyls)": whole MB rounds too much to show an extent exactly. */
static void resize_extent(char *buf, uint32_t lo, uint32_t hi)
{
    int p = 0;
    s_cat(buf, &p, "cyl "); p += u2s(buf + p, lo);
    s_cat(buf, &p, " to "); p += u2s(buf + p, hi);
    s_cat(buf, &p, " ("); p += u2s(buf + p, hi - lo + 1);
    s_cat(buf, &p, " cyls)"); buf[p] = 0;
}

/* Resize the selected partition: Size (MB) + which edge moves -> a new cylinder
   range, applied via rdb_resize_cyl. Confirms any actual change: growing can lose the
   volume too, because FFS places its root block relative to the partition size.
   Returns 1 if applied (caller need not act: the dialog refreshes the view itself),
   else 0. */
static int gui_resize_dialog(int index)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g, *gAnchor = 0, *gSize = 0, *gMaxHint = 0, *gRead = 0;
    struct NewGadget ng;
    RdbPartition *pt;
    int dt = g_topb, dl = g_leftb;
    int done = 0, applied = 0, anchor = 0;          /* 0 = End edge moves, 1 = Start edge */
    uint32_t oldLow, oldHigh, curCyls, cylBlocks, blockBytes;
    uint32_t gapAfter, gapBefore, maxCyls, maxMB, curMB, n;
    static const char *anchorLabels[3];
    static char maxBuf[24], readBuf[40], nowBuf[40];

    if (!g_have_model || index < 0 || index >= g_model.num_parts) return 0;
    pt = &g_model.parts[index];
    oldLow = pt->low_cyl; oldHigh = pt->high_cyl;
    curCyls = oldHigh - oldLow + 1;
    cylBlocks = g_model.cyl_blocks; blockBytes = g_model.block_bytes;
    gapAfter  = rdb_gap_end_after(&g_model, index);     /* exclusive */
    gapBefore = rdb_gap_start_before(&g_model, index);  /* inclusive */

    /* current size + the max for the default (End edge) anchor */
    curMB  = rdb_cyls_to_mb(curCyls, cylBlocks, blockBytes);
    if (curMB < 1) curMB = 1;
    maxCyls = gapAfter - oldLow;
    n = curMB;

    anchorLabels[0] = "End edge";
    anchorLabels[1] = "Start edge";
    anchorLabels[2] = 0;
    resize_extent(nowBuf, oldLow, oldHigh);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&dglist);
#pragma GCC diagnostic pop
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    /* What it is now */
    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 6; ng.ng_Width = 244; ng.ng_Height = 12;
    ng.ng_GadgetText = (UBYTE *)"Partition"; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)pt->name, TAG_END);
    ng.ng_TopEdge = dt + 19; ng.ng_GadgetText = (UBYTE *)"Now";
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)nowBuf, TAG_END);

    /* What it becomes */
    ng.ng_TopEdge = dt + 40; ng.ng_Width = 130; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Move"; ng.ng_GadgetID = 1;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)anchorLabels, GTCY_Active, 0, TAG_END);
    gAnchor = g;

    ng.ng_TopEdge = dt + 56; ng.ng_Width = 80;
    ng.ng_GadgetText = (UBYTE *)"Size (MB)"; ng.ng_GadgetID = 2;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, (ULONG)n, GTIN_MaxChars, 7, TAG_END);
    gSize = g;

    ng.ng_LeftEdge = dl + 192; ng.ng_Width = 156; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"", TAG_END);
    gMaxHint = g;

    ng.ng_LeftEdge = dl + 104; ng.ng_TopEdge = dt + 73; ng.ng_Width = 244; ng.ng_Height = 12;
    ng.ng_GadgetText = (UBYTE *)"New";
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"", TAG_END);
    gRead = g;

    /* The caveat */
    ng.ng_LeftEdge = dl + 12; ng.ng_TopEdge = dt + 95; ng.ng_Width = RS_W - 24;
    ng.ng_GadgetText = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text,
                     (ULONG)"Resizing needs a reformat: data is lost.", TAG_END);

    ng.ng_LeftEdge = dl + 4; ng.ng_TopEdge = dt + 112; ng.ng_Width = 90; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + RS_W - 94; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return 0; }

    {   int dwW = dl + RS_W + g_scr->WBorRight;
        int dwH = dt + 130 + g_scr->WBorBottom;
        int dwL = 0, dwT = 0;
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open("Resize partition", dwL, dwT, dwW, dwH,
                      BUTTONIDCMP | INTEGERIDCMP | CYCLEIDCMP, dglist);
    }
    if (!dw) { FreeGadgets(dglist); return 0; }
    GT_RefreshWindow(dw, 0);
    resize_chrome(dw);

    /* Recompute max, clamp n and refresh the readouts for the current anchor. */
#define RZ_RECOMPUTE() do {                                                       \
        if (anchor == 0) maxCyls = gapAfter - oldLow;                             \
        else             maxCyls = oldHigh - gapBefore + 1;                       \
        maxMB = rdb_cyls_to_mb(maxCyls, cylBlocks, blockBytes);                   \
        if (maxMB < 1) maxMB = 1;                                                 \
        if (n < 1) n = 1;                                                         \
        if (n > maxMB) n = maxMB;                                                 \
        { int p = 0; s_cat(maxBuf, &p, "max "); p += u2s(maxBuf + p, maxMB);      \
          s_cat(maxBuf, &p, " MB"); maxBuf[p] = 0; }                              \
        GT_SetGadgetAttrs(gMaxHint, dw, 0, GTTX_Text, (ULONG)maxBuf, TAG_END);    \
        GT_SetGadgetAttrs(gSize, dw, 0, GTIN_Number, (ULONG)n, TAG_END);          \
        { uint32_t cyls = rdb_resize_cyls(n, curCyls, maxCyls, cylBlocks, blockBytes); \
          if (anchor == 0) resize_extent(readBuf, oldLow, oldLow + cyls - 1);     \
          else             resize_extent(readBuf, oldHigh - cyls + 1, oldHigh); } \
        GT_SetGadgetAttrs(gRead, dw, 0, GTTX_Text, (ULONG)readBuf, TAG_END);      \
    } while (0)

    RZ_RECOMPUTE();                      /* fill the initial readouts */
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
            else if (cl == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(dw); resize_chrome(dw); GT_EndRefresh(dw, TRUE);
            }
            else if (cl == IDCMP_GADGETUP) {
                if (ig == gAnchor) { anchor = (int)code; RZ_RECOMPUTE(); }
                else if (ig == gSize) {
                    n = (uint32_t)((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    RZ_RECOMPUTE();
                } else if (ig->GadgetID == 10) {                 /* Ok */
                    uint32_t cyls, nl, nh;
                    /* A click leaves a typed number without a GADGETUP: read it here. */
                    n = (uint32_t)((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    RZ_RECOMPUTE();
                    cyls = rdb_resize_cyls(n, curCyls, maxCyls, cylBlocks, blockBytes);
                    if (anchor == 0) { nl = oldLow;  nh = oldLow + cyls - 1; }
                    else             { nh = oldHigh; nl = oldHigh - cyls + 1; }
                    if (nl == oldLow && nh == oldHigh) { done = 1; break; }   /* no change */
                    if (gui_request("Resize partition",
                                "Resizing changes the partition's extent.\n"
                                "Reformat afterward - existing data will be lost.",
                                1, "Proceed")) {
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
    static char msg[256];
    RdbPartition *pt;
    DevLiveness lv;
    int nn = 0, p;

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
    /* Confirm is specific to the ONE partition being formatted (not the whole
       device): name it, and if it is currently mounted, name its live volume.
       Only this partition's blocks are touched. */
    {
        char livename[8];
        int live = safety_partition_mounted(g_cur_driver, g_cur_unit,
                                            pt->low_cyl, pt->high_cyl, livename);
        p = 0;
        s_cat(msg, &p, "Format ");
        s_cat(msg, &p, pt->name);
        s_cat(msg, &p, " into an empty volume?");
        if (live) {
            int k;
            s_cat(msg, &p, "\nIt is mounted as ");
            for (k = 0; k < 7 && livename[k]; k++) msg[p++] = livename[k];
            s_cat(msg, &p, ": and will be reformatted.");
        }
        s_cat(msg, &p, "\nAll data on it is lost. Proceed?");
        msg[p] = 0;
        if (!gui_confirm("Format", msg)) return;
    }

    {
        static char volname[32];
        const char *deflt = g_model.parts[index].name;
        if (!gui_volname_dialog(deflt, volname, sizeof(volname)))
            return;                       /* user cancelled the format */
        switch (format_partition(g_cur_driver, g_cur_unit, &g_model, index, volname)) {
        case FMT_OK:
            p = 0;
            s_cat(msg, &p, "Formatted ");
            s_cat(msg, &p, pt->name);
            s_cat(msg, &p, ":");
            msg[p] = 0;
            gui_status(msg);
            gui_draw_details();
            break;
        case FMT_ERR_NO_HANDLER:
            gui_msg("Format", "No filesystem handler for this type.\nLoad it in Filesys... first, then format.");
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

/* Perceived brightness (0..15) of a 4-bit-per-gun RGB value, ITU-R 601 weights.
   Only used to compare two pens, so the coarse GetRGB4 view of the palette is
   plenty — an 8-bit screen's low nibbles cannot flip a contrast verdict. */
static int rgb4_luma(ULONG rgb4)
{
    int r = (int)((rgb4 >> 8) & 0xF);
    int g = (int)((rgb4 >> 4) & 0xF);
    int b = (int)(rgb4 & 0xF);
    return (r * 77 + g * 151 + b * 28) >> 8;
}

/* Minimum luminance gap (of 15) for text to be legible over its background. */
#define MENU_MIN_LUMA_DIFF 4

/* Luminance of one DrawInfo pen: -1 = not a usable palette index, -2 = a V39
   "magic" pen (PEN_C3 and friends, 0xFEFC..0xFEFF), which names the complement
   of colours 0..3 rather than an index and contrasts by construction. */
static int pen_luma(struct Screen *scr, int pen)
{
    struct ColorMap *cm = scr->ViewPort.ColorMap;
    if (pen >= 0xFEFC) return -2;
    if (!cm || pen < 0 || pen >= (int)cm->Count) return -1;
    return rgb4_luma(GetRGB4(cm, pen));
}

/* 1 if pen `fg` reads over pen `bg` on scr's palette. */
static int pens_readable(struct Screen *scr, int fg, int bg)
{
    int lf = pen_luma(scr, fg), lb = pen_luma(scr, bg), d;
    if (lf == -2 || lb == -2) return 1;
    if (lf < 0 || lb < 0) return 0;
    d = lf - lb; if (d < 0) d = -d;
    return d >= MENU_MIN_LUMA_DIFF;
}

/* NewLook (V39) menus, or the classic black-on-white ones?

   NewLook menus are the KS3.x system look: the menu box is rendered 3D from the
   screen's DrawInfo pens (BARDETAILPEN text over BARBLOCKPEN fill, the hovered
   item FILLTEXTPEN over FILLPEN) instead of DetailPen/BlockPen. We want them
   whenever the screen can actually carry them, and the classic look below KS3.0
   — where GadTools has no such tag anyway.

   Three conditions, all necessary:
     - gadtools.library V39+ (KS3.0). GTMN_NewLookMenus does not exist before
       that, so KS2.0/2.04/2.1 always get classic menus.
     - the screen was opened with SA_Pens (DRIF_NEWLOOK) and its DrawInfo is
       DRI_VERSION 2+ with the bar pens present: without pens the whole screen
       renders flat/old-look, and NewLook menus on it would be both wrong and
       inconsistent with our (equally flat) gadgets. Our cloned own-screen does
       pass SA_Pens, so it qualifies; the bare no-Workbench fallback does not.
     - the two pen pairs actually contrast. This is what bit an OS 3.2 tester on
       v0.3: a customised palette had FILLTEXTPEN = black over a dark blue
       FILLPEN, so the highlighted item was invisible. Rather than give up on
       NewLook everywhere (what v0.4 did), check the palette and fall back to
       classic only on the screens where it would be unreadable. */
/* Decided once, before the window opens: WA_NewLookMenus is a window property
   and CreateMenus/LayoutMenus must agree with it. */
static int g_newlook = 0;

static int menus_use_newlook(void)
{
    struct DrawInfo *dri;
    int ok = 0;

    if (!GadToolsBase || GadToolsBase->lib_Version < 39) return 0;
    dri = GetScreenDrawInfo(g_scr);
    if (!dri) return 0;
    if ((dri->dri_Flags & DRIF_NEWLOOK) && dri->dri_Version >= 2 &&
        dri->dri_Pens && dri->dri_NumPens > BARBLOCKPEN) {
        ok = pens_readable(g_scr, (int)dri->dri_Pens[BARDETAILPEN],
                                  (int)dri->dri_Pens[BARBLOCKPEN]) &&
             pens_readable(g_scr, (int)dri->dri_Pens[FILLTEXTPEN],
                                  (int)dri->dri_Pens[FILLPEN]);
    }
    FreeScreenDrawInfo(g_scr, dri);
    return ok;
}

/* Create + layout the menu strip and attach it to g_win. Returns 1 on success. */
static int build_menus(void)
{
    LONG laid;

    /* NewLook takes all three of these, or none of them: the window flag
       (WA_NewLookMenus, set at OpenWindow — Intuition renders the menu, and
       without it the strip comes out old-look no matter how it was laid out),
       CreateMenus (picks the V39 checkmark / Amiga-key images) and LayoutMenus
       (sizes the items for them). Passing only the GadTools half was exactly the
       first attempt at this, and changed nothing on screen. */
    if (g_newlook) {
        g_menu = CreateMenus(g_newmenu, GTMN_NewLookMenus, TRUE, TAG_END);
        if (!g_menu) return 0;
        laid = LayoutMenus(g_menu, g_vi, GTMN_NewLookMenus, TRUE, TAG_END);
    } else {
        /* Classic: a V37 GadTools never sees the V39 tag at all. */
        g_menu = CreateMenus(g_newmenu, TAG_END);
        if (!g_menu) return 0;
        laid = LayoutMenus(g_menu, g_vi, TAG_END);
    }
    if (!laid) {
        FreeMenus(g_menu); g_menu = 0; return 0;
    }
    SetMenuStrip(g_win, g_menu);
    return 1;
}

/* ---------- Quick setup ----------------------------------------------------- */

/* Why the wizard stops at the card screen. Each says what happened and what to do, with
   no technical words. '#' in the text is replaced by the digit of the name in question. */
enum { QS_R_NONE = 0, QS_R_NO_PORT, QS_R_UNPROVEN, QS_R_NO_CARD, QS_R_TWO_DRIVES, QS_R_UNREADABLE,
       QS_R_BOOTED, QS_R_BOOT_UNKNOWN, QS_R_NAME_TAKEN, QS_R_TOO_SMALL };
static const char *const kQsRefusals[] = {
    "",
    "Quick setup found no IDE port with a card on it.\n"
    "Either this Amiga has no built-in IDE port, like\n"
    "the A500, or no card answered on it.\n"
    "Nothing was changed.\n\n"
    "On an A600, A1200 or A4000, turn the Amiga off,\n"
    "check that the card sits firmly in its adapter,\n"
    "then try again.",
    "Quick setup can't confirm this setup. The disks\n"
    "here could not be proven to be on the built-in\n"
    "IDE port, the only place it is made for. SCSI\n"
    "machines such as the A3000 and IDE drivers\n"
    "loaded from disk end up here.\n"
    "Nothing was changed.\n\n"
    "You can do it by hand in the main window: pick\n"
    "a driver there and press Scan.",
    "No card was found on the IDE port.\n"
    "Nothing was changed.\n\n"
    "Turn the Amiga off, check that the card sits\n"
    "firmly in its adapter, then try again.",
    "More than one drive was found on the IDE port.\n"
    "Quick setup works with one card only, so that it\n"
    "can't erase the wrong one.\n"
    "Nothing was changed.\n\n"
    "Remove the other drive and try again, or set up\n"
    "your drives by hand in the main window.",
    "The card could not be read.\n"
    "Nothing was changed.\n\n"
    "Check that it sits firmly in its adapter and\n"
    "try again, or try another card.",
    "This Amiga started from this card, and a card\n"
    "can't be erased while it is running the Amiga.\n"
    "Nothing was changed.\n\n"
    "Start from your HDPart floppy instead, then run\n"
    "Quick setup again.",
    "Quick setup can't tell which disk this Amiga\n"
    "started from, so it can't be sure this card is\n"
    "safe to erase.\n"
    "Nothing was changed.\n\n"
    "Start from your HDPart floppy and try again, or\n"
    "do it by hand in the main window.",
    "Another disk is already using the name DH#:, and\n"
    "this card will need it.\n"
    "Nothing was changed.\n\n"
    "Remove that disk, restart and try again, or do\n"
    "it by hand in the main window.",
    "This card is too small for Quick setup, which\n"
    "needs at least 128 MB.\n"
    "Nothing was changed.\n\n"
    "Use a bigger card, or set this one up by hand in\n"
    "the main window."
};

#define QS_DRIVER     "scsi.device"
#define QS_VALUE_CHARS 46               /* what fits in a row of the card screen */

static DeviceInfo g_qs_geo;             /* the card's geometry and model */
static DiscDisk   g_qs_card;            /* the card that was found */
static RdbModel   g_qs_model;           /* the card's table as it stands, static: keep off the stack */
static RdbModel   g_qs_layout;          /* a trial of the new one */
static ULONG      g_qs_block[128];      /* longs: word-aligned for any driver */
static char       g_qs_said[420];       /* the refusal on screen */
static char       g_qs_found[48], g_qs_size[48], g_qs_holds[QS_VALUE_CHARS + 2];
static int        g_qs_taken;           /* the digit of the DH name held by another disk */

/* Is a drive on this unit? A unit that does not open or fails the question is not there:
   scsi.device can answer for a unit no drive is behind. One that answers with no capacity
   is no card either, and can't be erased by mistake. Any stated capacity counts, whatever
   the other figures say: qs_find_card repairs those, and a real drive must not go unseen. */
static int qs_probe_unit(ULONG unit, DeviceInfo *info)
{
    DeviceHandle *h;
    int i, found = 0;
    for (i = 0; i < (int)sizeof(*info); i++) ((UBYTE *)info)[i] = 0;
    h = dev_open(QS_DRIVER, unit);
    if (!h) return 0;
    if (dev_unit_ready(h) && dev_geometry(h, info) == 0 && info->total_blocks) {
        found = 1;
        (void)dev_inquiry_model(h, info->model);
    }
    dev_close(h);
    return found;
}

/* Case-insensitive ASCII equality of DOS names. */
static int qs_name_eq(const char *a, const char *b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return a[i] == b[i];
}

/* Is this name held by anything but the card? mine[] holds the names safety_classify read
   from a node's FileSysStartupMsg as being on the card. A node it could not read is not
   among them, so a holder that can't be established counts as another disk. */
static int qs_name_elsewhere(const char *name, char mine[][8], int nmine)
{
    struct DosList *dl;
    int i, there;
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    there = FindDosEntry(dl, (CONST_STRPTR)name, LDF_DEVICES | LDF_READ) != 0;
    UnLockDosList(LDF_DEVICES | LDF_READ);
    if (!there) return 0;
    for (i = 0; i < nmine; i++) if (qs_name_eq(name, mine[i])) return 0;
    return 1;
}

/* A size as a card's label gives it: decimal GB from 2000 MB up, otherwise MB. */
static int qs_size_str(char *o, ULONG mb)
{
    int n;
    if (mb < 2000) { n = u2s(o, mb); o[n++] = ' '; o[n++] = 'M'; o[n++] = 'B'; }
    else { n = u2s(o, (mb * 1049u + 500000u) / 1000000u); o[n++] = ' '; o[n++] = 'G'; o[n++] = 'B'; }
    o[n] = 0;
    return n;
}

/* The volume name in an Amiga partition's root block, or "" if it holds none. The name is
   only for showing, so any doubt just leaves it out. */
/* Read a partition's root block into g_qs_block. 1 if it reads and is valid. */
static int qs_read_root(DeviceHandle *h, const RdbPartition *pt, uint32_t cyl_blocks)
{
    uint32_t blocks, root;
    if (!cyl_blocks || pt->low_cyl > 8388608u / cyl_blocks) return 0;   /* a foreign table: must not overflow */
    blocks = (pt->high_cyl - pt->low_cyl + 1) * cyl_blocks;
    root = pt->low_cyl * cyl_blocks + (blocks + 1) / 2;
    if ((pt->dos_type & 0xFFFFFF00u) != 0x444F5300u || root >= 8388608u) return 0;   /* offsets are 32-bit */
    if (dev_read_block(h, root, (UBYTE *)g_qs_block) != 0) return 0;
    return g_qs_block[0] == 2 && g_qs_block[127] == 1 && rdb_checksum_ok((const UBYTE *)g_qs_block, 128);
}

static void qs_volume_name(DeviceHandle *h, const RdbPartition *pt, char *out, int outsz)
{
    const UBYTE *b = (const UBYTE *)g_qs_block;
    int n = 0, i;
    out[0] = 0;
    if (!qs_read_root(h, pt, g_qs_model.cyl_blocks)) return;
    if (b[432] <= 30)
        for (i = 0; i < b[432] && n < outsz - 1; i++)
            if (b[433 + i] >= 32 && b[433 + i] < 127) out[n++] = (char)b[433 + i];
    out[n] = 0;
}

/* Fill "Now holds" from the card. Returns 0, or QS_R_UNREADABLE. */
static int qs_read_holds(DeviceHandle *h)
{
    int p = 0, rc, i;
    rc = rdb_parse(&g_qs_model, dev_block_io, h);
    if (rc == RDB_ERR_IO) return QS_R_UNREADABLE;
    if (rc != RDB_OK) {
        /* The table search looks in these blocks, so a blank card is zero here. */
        int blk, zero = 1;
        for (blk = 0; blk < 16 && zero; blk++) {
            if (dev_read_block(h, (ULONG)blk, (UBYTE *)g_qs_block) != 0) return QS_R_UNREADABLE;
            for (i = 0; i < 128; i++) if (g_qs_block[i]) { zero = 0; break; }
        }
        s_cat(g_qs_holds, &p, zero ? "Empty" : "Not an Amiga disk");
    } else if (g_qs_model.num_parts == 0) {
        s_cat(g_qs_holds, &p, "Empty");
    } else {
        for (i = 0; i < g_qs_model.num_parts; i++) {
            const RdbPartition *pt = &g_qs_model.parts[i];
            char vol[32]; int len, k, room;
            int left = g_qs_model.num_parts - i - 1;
            qs_volume_name(h, pt, vol, sizeof(vol));
            for (len = 0; pt->name[len]; len++) ;
            if (vol[0]) { for (k = 0; vol[k]; k++) ; len += k + 1; }
            /* Room for this entry, and for saying what is left after it. */
            if (i > 0 && p + 2 + len + (left ? 11 : 0) > QS_VALUE_CHARS) {
                s_cat(g_qs_holds, &p, " and ");
                p += u2s(g_qs_holds + p, (ULONG)(left + 1));
                s_cat(g_qs_holds, &p, " more");
                break;
            }
            if (i > 0) s_cat(g_qs_holds, &p, ", ");
            /* The first goes in whatever its length, cut to leave room for the rest. */
            room = i ? QS_VALUE_CHARS : QS_VALUE_CHARS - 12;
            for (k = 0; pt->name[k] && p < room; k++) g_qs_holds[p++] = pt->name[k];
            if (vol[0]) {
                g_qs_holds[p++] = ' ';
                for (k = 0; vol[k] && p < room; k++) g_qs_holds[p++] = vol[k];
            }
        }
    }
    g_qs_holds[p] = 0;
    return 0;
}

/* Find the one card this wizard may use and fill the card screen's rows. Touches nothing
   on any disk. Returns 0 with the card in *out, or the QS_R_* reason it can't go on. */
static int __attribute__((noinline)) qs_find_card(DiscDisk *out)
{
    static DeviceInfo other;
    static char mine[8][8], names[8][8];
    int a, b, n = 0, i, p, rc;
    DeviceHandle *h;
    uint32_t mb;

    /* Asked now, not at startup: a patch or a loaded driver can land in between. */
    if (!safety_is_internal_ide()) {
        struct Node *nd;
        Forbid();
        nd = FindName(&SysBase->DeviceList, (CONST_STRPTR)QS_DRIVER);
        Permit();
        /* With no driver at all this may still be an A1200 whose card did not answer. */
        return nd ? QS_R_UNPROVEN : QS_R_NO_PORT;
    }

    /* Units 0 and 1 are the IDE port's two positions. */
    a = qs_probe_unit(0, &g_qs_geo);
    b = qs_probe_unit(1, &other);
    if (a && b) return QS_R_TWO_DRIVES;
    if (!a && !b) return QS_R_NO_CARD;
    if (b) g_qs_geo = other;

    /* Partitions come from these figures, so they must add up to the drive's real size.
       The driver addresses by byte offset, so any consistent figures do: proved by the
       test "synth" in tests/target/quick.mjs. */
    if (!quick_geometry_agrees(g_qs_geo.cylinders, g_qs_geo.heads, g_qs_geo.sectors, g_qs_geo.total_blocks)) {
        uint32_t c, hd, sc;
        disc_synth_chs(g_qs_geo.total_blocks, &c, &hd, &sc);
        g_qs_geo.cylinders = c; g_qs_geo.heads = hd; g_qs_geo.sectors = sc;
    }
    /* Figures can agree and still overstate: no partition may end past the card. */
    g_qs_geo.cylinders = quick_cap_cylinders(g_qs_geo.cylinders, g_qs_geo.heads, g_qs_geo.sectors, g_qs_geo.total_blocks);

    for (i = 0; i < (int)sizeof(*out); i++) ((UBYTE *)out)[i] = 0;
    for (i = 0; QS_DRIVER[i]; i++) out->driver[i] = QS_DRIVER[i];
    out->unit = b ? 1 : 0;

    /* A name nothing is mounted on comes back clear unless the boot device is unknown,
       and then the card might be it, whatever the answer below says. */
    if (safety_classify("no.device", 0xFFFFFFFFu, names, 8, &n) != DEV_CLEAR) return QS_R_BOOT_UNKNOWN;
    if (safety_classify(out->driver, out->unit, mine, 8, &n) == DEV_BOOT) return QS_R_BOOTED;

    /* A layout with every extra ticked needs DH0 to DH3. The card's own may be mounted. */
    { static char dh[] = "DH0";
      for (i = 0; i < 4; i++) {
          dh[2] = (char)('0' + i);
          if (qs_name_elsewhere(dh, mine, n)) { g_qs_taken = i; return QS_R_NAME_TAKEN; }
      } }

    h = dev_open(out->driver, out->unit);
    if (!h) return QS_R_UNREADABLE;
    { static DeviceInfo again; (void)dev_geometry(h, &again); }   /* tells the handle the block size */
    rc = qs_read_holds(h);
    rdb_model_free(&g_qs_model);     /* only parts[] is wanted: drop any embedded filesystem */
    dev_close(h);
    if (rc) return rc;

    rdb_init_model(&g_qs_layout, g_qs_geo.cylinders, g_qs_geo.heads, g_qs_geo.sectors);
    if (quick_layout(&g_qs_layout, QS_WORK | QS_GAMES | QS_DEMOS) != RDB_OK) return QS_R_TOO_SMALL;

    p = 0;
    if (g_qs_geo.model[0]) s_cat(g_qs_found, &p, g_qs_geo.model);
    else s_cat(g_qs_found, &p, "Card on the IDE port");
    g_qs_found[p] = 0;

    mb = disc_blocks_to_mb(g_qs_geo.total_blocks, g_qs_geo.block_bytes);
    p = qs_size_str(g_qs_size, mb);
    if (g_qs_layout.hi_cyl + 1u < g_qs_layout.cylinders) {
        s_cat(g_qs_size, &p, " (this Amiga can use ");
        p += qs_size_str(g_qs_size + p,
                         disc_blocks_to_mb((g_qs_layout.hi_cyl + 1u) * g_qs_layout.cyl_blocks, g_qs_geo.block_bytes));
        s_cat(g_qs_size, &p, ")");
    }
    g_qs_size[p] = 0;

    for (i = 0; i < DISC_LABEL_LEN - 1 && g_qs_found[i]; i++) out->label[i] = g_qs_found[i];
    out->size_mb = mb;
    out->partitionable = 1;
    out->used_pct = -1;
    return 0;
}

/* Layout of the card screen, in pixels from the window's inner top-left. */
#define QS_W      488
#define QS_INFO_Y 3
#define QS_INFO_H 38
#define QS_WARN_Y 45
#define QS_WARN_H 14
#define QS_BTN_Y  65
#define QS_KEY_R  88                    /* labels end here */
#define QS_VAL_X  96

static void qs_text(struct RastPort *rp, int x, int y, const char *s)
{
    int n = 0;
    while (s[n]) n++;
    SetAPen(rp, g_pen_text);
    SetDrMd(rp, JAM1);
    Move(rp, x, y + 6);
    Text(rp, (CONST_STRPTR)s, (LONG)n);
}

static void qs_row(struct RastPort *rp, int y, const char *key, const char *value)
{
    int n = 0;
    while (key[n]) n++;
    qs_text(rp, g_leftb + QS_KEY_R - n * 8, y, key);
    qs_text(rp, g_leftb + QS_VAL_X, y, value);
}

static void qs_card_draw(struct Window *w)
{
    struct RastPort *rp = w->RPort;
    int x = g_leftb + 4, bw = QS_W - 8, y = g_topb + QS_WARN_Y;
    DrawBevelBox(rp, x, g_topb + QS_INFO_Y, bw, QS_INFO_H, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    qs_row(rp, g_topb + QS_INFO_Y + 4,  "Found", g_qs_found);
    qs_row(rp, g_topb + QS_INFO_Y + 14, "Size", g_qs_size);
    qs_row(rp, g_topb + QS_INFO_Y + 24, "Now holds", g_qs_holds);
    SetAPen(rp, g_pen_text);
    RectFill(rp, x, y, x + bw - 1, y + QS_WARN_H - 1);
    SetAPen(rp, g_pen_shine);
    RectFill(rp, x + 2, y + 1, x + bw - 3, y + QS_WARN_H - 2);
    qs_text(rp, x + 8, y + 3, "Everything on this card will be erased.");
}

static const char kQsNoWindow[] = "Quick setup could not open its window, probably for\n"
                                  "lack of memory. Nothing was changed. Close other\n"
                                  "programs and try again.";

/* Screen 2. Returns 1 for Continue, 0 for Cancel. */
static int qs_card_screen(void)
{
    struct Window *dw;
    struct Gadget *glist = 0, *g;
    struct NewGadget ng;
    int dwW = g_leftb + QS_W + g_scr->WBorRight, dwH = g_topb + QS_BTN_Y + 14 + 6 + g_scr->WBorBottom;
    int dwL = 0, dwT = 0, done = 0, result = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (!g) { gui_msg("Quick setup", kQsNoWindow); return 0; }
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;
    ng.ng_TopEdge = g_topb + QS_BTN_Y; ng.ng_Width = 90; ng.ng_Height = 14;
    ng.ng_LeftEdge = g_leftb + 4; ng.ng_GadgetText = (UBYTE *)"Continue"; ng.ng_GadgetID = 1;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = g_leftb + QS_W - 4 - 90; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 0;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(glist); gui_msg("Quick setup", kQsNoWindow); return 0; }

    dlg_center(dwW, dwH, &dwL, &dwT);
    dw = dlg_open("Quick setup: your card", dwL, dwT, dwW, dwH, BUTTONIDCMP, glist);
    if (!dw) { FreeGadgets(glist); gui_msg("Quick setup", kQsNoWindow); return 0; }
    GT_RefreshWindow(dw, 0);
    qs_card_draw(dw);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        /* The first answer stands: a later click in the same batch must not replace it. */
        while (!done && (im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) done = 1;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); qs_card_draw(dw); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) { result = ig->GadgetID == 1; done = 1; }
        }
    }
    dlg_close(dw, glist);
    return result;
}

/* Screens 3 and 4 share one width: the longest line is 62 characters of topaz 8. */
#define QS2_W       520
#define QS_CH_Q_Y   3                   /* the question and the ticks */
#define QS_CH_Q_H   46
#define QS_CH_MAP_Y 53                  /* the map's frame; the map sits 2 inside it */
#define QS_CH_MAP_H 36
#define QS_CH_MSG_Y 93
#define QS_CH_MSG_H 14
#define QS_CH_BTN_Y 113
#define QS_CF_ROW_H 10
#define QS_BTN_W    90

enum { QS_ANS_CANCEL = 0, QS_ANS_NEXT = 1, QS_ANS_BACK = 2 };
enum { QS_GID_WORK = 11, QS_GID_GAMES = 12, QS_GID_DEMOS = 13 };

static uint32_t g_qs_mask;              /* the extras ticked: QS_WORK | QS_GAMES | QS_DEMOS */
static char     g_qs_msg[72];           /* the line under the map */
static char     g_qs_rows[4][60];       /* the last check's table, as drawn */
static char     g_qs_warn[80];          /* and its warning */

static const char kQsNoRoom[] = "These choices do not fit on this card. Take one away.";

static void qs_clear(struct RastPort *rp, int x, int y, int w, int h)
{
    SetAPen(rp, g_pen_bg);
    RectFill(rp, x, y, x + w - 1, y + h - 1);
}

/* Megabytes of the whole cylinders lo..hi of m, as the map and the table both say them. */
static ULONG qs_part_mb(const RdbModel *m, const RdbPartition *pt)
{
    return disc_blocks_to_mb((pt->high_cyl - pt->low_cyl + 1) * m->cyl_blocks, m->block_bytes);
}

/* Lay the card out for `mask` and word the line under the map. Returns 1 when it fits.
   A failed layout leaves no partitions, so no stale table is drawn or confirmed. */
static int qs_relayout(RdbModel *m, uint32_t mask)
{
    int p = 0;
    if (quick_layout(m, mask) != RDB_OK) {
        m->num_parts = 0;
        s_cat(g_qs_msg, &p, kQsNoRoom);
        g_qs_msg[p] = 0;
        return 0;
    }
    if (mask == 0) {
        s_cat(g_qs_msg, &p, "Only System. The rest of the card stays unused.");
    } else {
        const RdbPartition *last = &m->parts[m->num_parts - 1];
        ULONG spare = disc_blocks_to_mb((m->hi_cyl - last->high_cyl) * m->cyl_blocks, m->block_bytes);
        if (spare > 0 && qs_part_mb(m, last) >= 2000) {   /* the 2 GB limit is what held them back */
            s_cat(g_qs_msg, &p, "Each partition is limited to 2 GB, so ");
            p += u2s(g_qs_msg + p, spare);
            s_cat(g_qs_msg, &p, " MB stays unused.");
        } else if (m->hi_cyl + 1 < m->cylinders) {        /* 62 characters is all the line holds */
            s_cat(g_qs_msg, &p, "This Amiga can use the first 4 GB, shared evenly between them.");
        } else {
            s_cat(g_qs_msg, &p, "The rest of the card is shared evenly between your choices.");
        }
    }
    g_qs_msg[p] = 0;
    return 1;
}

static void qs_choose_draw(struct Window *w, const RdbModel *m)
{
    struct RastPort *rp = w->RPort;
    int x = g_leftb + 4, bw = QS2_W - 8, y;
    DrawBevelBox(rp, x, g_topb + QS_CH_Q_Y, bw, QS_CH_Q_H, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    qs_text(rp, x + 8, g_topb + QS_CH_Q_Y + 4, "What else do you want on the card?");
    qs_text(rp, x + 8, g_topb + QS_CH_Q_Y + 15, "System is always included, and is what the Amiga starts from.");
    y = g_topb + QS_CH_MAP_Y;
    DrawBevelBox(rp, x, y, bw, QS_CH_MAP_H, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    qs_clear(rp, x + 2, y + 2, bw - 4, QS_CH_MAP_H - 4);
    if (m->num_parts > 0 && m->cylinders > 0)
        gui_map_draw(rp, m, x + 2, y + 2, bw - 4, QS_CH_MAP_H - 4, -1, 0, &g_qs_mask);
    y = g_topb + QS_CH_MSG_Y;
    DrawBevelBox(rp, x, y, bw, QS_CH_MSG_H, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    qs_clear(rp, x + 2, y + 2, bw - 4, QS_CH_MSG_H - 4);
    qs_text(rp, x + 8, y + 3, g_qs_msg);
}

/* Screen 3: which extras. m is laid out afresh for each choice and always matches the screen.
   Returns QS_ANS_NEXT, QS_ANS_BACK or QS_ANS_CANCEL. */
static int qs_choose(const DiscDisk *card, RdbModel *m, uint32_t *mask)
{
    static const char *const names[3] = { "Work", "Games", "Demos" };
    struct Window *dw;
    struct Gadget *glist = 0, *g, *cont = 0;
    struct NewGadget ng;
    int dwW = g_leftb + QS2_W + g_scr->WBorRight, dwH = g_topb + QS_CH_BTN_Y + 14 + 6 + g_scr->WBorBottom;
    int dwL = 0, dwT = 0, done = 0, result = QS_ANS_CANCEL, fits, i;

    (void)card;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (!g) { gui_msg("Quick setup", kQsNoWindow); return QS_ANS_CANCEL; }
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi;
    ng.ng_Width = 26; ng.ng_Height = 11; ng.ng_Flags = PLACETEXT_RIGHT;
    ng.ng_TopEdge = g_topb + QS_CH_Q_Y + 30;
    for (i = 0; i < 3 && g; i++) {
        ng.ng_LeftEdge = g_leftb + 16 + i * 140;
        ng.ng_GadgetText = (UBYTE *)names[i]; ng.ng_GadgetID = (UWORD)(QS_GID_WORK + i);
        g = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, (ULONG)((*mask & (1u << i)) ? TRUE : FALSE), TAG_END);
    }
    ng.ng_Flags = 0; ng.ng_Width = QS_BTN_W; ng.ng_Height = 14; ng.ng_TopEdge = g_topb + QS_CH_BTN_Y;
    if (g) {
        ng.ng_LeftEdge = g_leftb + 4; ng.ng_GadgetText = (UBYTE *)"Back"; ng.ng_GadgetID = QS_ANS_BACK;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (g) {
        ng.ng_LeftEdge = g_leftb + 4 + QS_BTN_W + 8; ng.ng_GadgetText = (UBYTE *)"Continue"; ng.ng_GadgetID = QS_ANS_NEXT;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
        cont = g;
    }
    if (g) {
        ng.ng_LeftEdge = g_leftb + QS2_W - 4 - QS_BTN_W; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = QS_ANS_CANCEL;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (!g) { FreeGadgets(glist); gui_msg("Quick setup", kQsNoWindow); return QS_ANS_CANCEL; }

    dlg_center(dwW, dwH, &dwL, &dwT);
    dw = dlg_open("Quick setup: partitions", dwL, dwT, dwW, dwH, BUTTONIDCMP | CHECKBOXIDCMP, glist);
    if (!dw) { FreeGadgets(glist); gui_msg("Quick setup", kQsNoWindow); return QS_ANS_CANCEL; }

    g_qs_screen = 3;
    fits = qs_relayout(m, *mask);
    g_qs_mask = *mask;
    if (!fits) GT_SetGadgetAttrs(cont, dw, 0, GA_Disabled, TRUE, TAG_END);
    GT_RefreshWindow(dw, 0);
    qs_choose_draw(dw, m);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        /* The first answer stands: a later click in the same batch must not replace it. */
        while (!done && (im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) done = 1;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); qs_choose_draw(dw, m); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) {
                if (ig->GadgetID >= QS_GID_WORK && ig->GadgetID <= QS_GID_DEMOS) {
                    uint32_t bit = 1u << (ig->GadgetID - QS_GID_WORK);
                    int was = fits;
                    if (ig->Flags & GFLG_SELECTED) *mask |= bit; else *mask &= ~bit;
                    fits = qs_relayout(m, *mask);
                    g_qs_mask = *mask;
                    if (fits != was) GT_SetGadgetAttrs(cont, dw, 0, GA_Disabled, fits ? FALSE : TRUE, TAG_END);
                    qs_choose_draw(dw, m);
                } else if (ig->GadgetID == QS_ANS_NEXT && !fits) {
                    /* Clicked before the tick queued ahead of it turned the gadget off:
                       with nothing that fits there is nothing to go on with. */
                } else {
                    result = ig->GadgetID == QS_ANS_NEXT ? QS_ANS_NEXT : ig->GadgetID == QS_ANS_BACK ? QS_ANS_BACK : QS_ANS_CANCEL;
                    done = 1;
                }
            }
        }
    }
    dlg_close(dw, glist);
    return result;
}

/* Pad a text row to a column: out gets s, then spaces up to `col` characters. */
static void qs_pad(char *o, int *p, const char *s, int col)
{
    int start = *p;
    s_cat(o, p, s);
    while (*p - start < col) o[(*p)++] = ' ';
}

static void qs_confirm_draw(struct Window *w, int nrows, int tableH)
{
    struct RastPort *rp = w->RPort;
    int x = g_leftb + 4, bw = QS2_W - 8, y = g_topb + 3 + tableH + 4, i;
    DrawBevelBox(rp, x, g_topb + 3, bw, tableH, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    for (i = 0; i < nrows; i++) qs_text(rp, x + 8, g_topb + 3 + 4 + i * QS_CF_ROW_H, g_qs_rows[i]);
    SetAPen(rp, g_pen_text);
    RectFill(rp, x, y, x + bw - 1, y + QS_WARN_H - 1);
    SetAPen(rp, g_pen_shine);
    RectFill(rp, x + 2, y + 1, x + bw - 3, y + QS_WARN_H - 2);
    qs_text(rp, x + 8, y + 3, g_qs_warn);
}

/* Screen 4: the last check. Lists what would be made, from the same model screen 3 showed. */
static int qs_confirm(const DiscDisk *card, const RdbModel *m, uint32_t mask)
{
    struct Window *dw;
    struct Gadget *glist = 0, *g;
    struct NewGadget ng;
    int n = m->num_parts, tableH = n * QS_CF_ROW_H + 8, btnY = 3 + tableH + 4 + QS_WARN_H + 6;
    int dwW = g_leftb + QS2_W + g_scr->WBorRight, dwH = g_topb + btnY + 14 + 6 + g_scr->WBorBottom;
    int dwL = 0, dwT = 0, done = 0, result = QS_ANS_CANCEL, i, p;

    if (n < 1 || n > 4) return QS_ANS_CANCEL;
    for (i = 0; i < n; i++) {
        const RdbPartition *pt = &m->parts[i];
        const char *vol = quick_volume_name(mask, i);
        char num[12];
        int k, q = 0;
        p = 0;
        qs_pad(g_qs_rows[i], &p, pt->name, 3);
        g_qs_rows[i][p++] = ':';
        qs_pad(g_qs_rows[i], &p, "", 2);
        qs_pad(g_qs_rows[i], &p, vol ? vol : "", 8);
        q = u2s(num, qs_part_mb(m, pt));
        for (k = q; k < 4; k++) g_qs_rows[i][p++] = ' ';       /* sizes line up on the right */
        num[q] = 0;
        s_cat(g_qs_rows[i], &p, num);
        s_cat(g_qs_rows[i], &p, " MB");
        if (pt->bootable) { s_cat(g_qs_rows[i], &p, "  starts the Amiga"); }
        g_qs_rows[i][p] = 0;
    }
    for (i = n; i < 4; i++) g_qs_rows[i][0] = 0;
    p = 0;
    s_cat(g_qs_warn, &p, "Erases ");
    for (i = 0; card->label[i] && i < 31; i++) g_qs_warn[p++] = card->label[i];
    s_cat(g_qs_warn, &p, ". This cannot be undone.");
    g_qs_warn[p] = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (!g) { gui_msg("Quick setup", kQsNoWindow); return QS_ANS_CANCEL; }
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;
    ng.ng_TopEdge = g_topb + btnY; ng.ng_Width = QS_BTN_W; ng.ng_Height = 14;
    ng.ng_LeftEdge = g_leftb + 4; ng.ng_GadgetText = (UBYTE *)"Back"; ng.ng_GadgetID = QS_ANS_BACK;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (g) {
        ng.ng_LeftEdge = g_leftb + 4 + QS_BTN_W + 8; ng.ng_Width = 144;
        ng.ng_GadgetText = (UBYTE *)"Erase and set up"; ng.ng_GadgetID = QS_ANS_NEXT;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (g) {
        ng.ng_LeftEdge = g_leftb + QS2_W - 4 - QS_BTN_W; ng.ng_Width = QS_BTN_W;
        ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = QS_ANS_CANCEL;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (!g) { FreeGadgets(glist); gui_msg("Quick setup", kQsNoWindow); return QS_ANS_CANCEL; }

    dlg_center(dwW, dwH, &dwL, &dwT);
    dw = dlg_open("Quick setup: ready?", dwL, dwT, dwW, dwH, BUTTONIDCMP, glist);
    if (!dw) { FreeGadgets(glist); gui_msg("Quick setup", kQsNoWindow); return QS_ANS_CANCEL; }
    g_qs_screen = 4;
    GT_RefreshWindow(dw, 0);
    qs_confirm_draw(dw, n, tableH);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while (!done && (im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) done = 1;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); qs_confirm_draw(dw, n, tableH); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) {
                result = ig->GadgetID == QS_ANS_NEXT ? QS_ANS_NEXT : ig->GadgetID == QS_ANS_BACK ? QS_ANS_BACK : QS_ANS_CANCEL;
                done = 1;
            }
        }
    }
    dlg_close(dw, glist);
    return result;
}

/* Screens 5 and 6: the run. The first thing that writes to a card. */

/* What stopped or failed, said on one box. Wizard words only. '%' stands for a volume's name. */
static const char kQsLostTrack[] = "Quick setup lost track of your choices.\n"
                                   "Nothing was changed.\n\n"
                                   "Run Quick setup again.";
static const char kQsChanged[] = "The card has changed since you confirmed it.\n"
                                 "Nothing was changed.\n\n"
                                 "Run Quick setup again, and check what it finds.";
static const char kQsWriteFailed[] = "Writing the partition table failed, so the card\n"
                                     "is not ready.\n\n"
                                     "Check that it sits firmly in its adapter and run\n"
                                     "Quick setup again. If it fails again, try another\n"
                                     "card.";
static const char kQsCheckFailed[] = "Checking the partition table failed: the card did\n"
                                     "not give back what was written, so it is not ready.\n\n"
                                     "Check that it sits firmly in its adapter and run\n"
                                     "Quick setup again. If it fails again, try another\n"
                                     "card.";
static const char kQsFormatFailed[] = "Formatting % failed, so the card is not ready.\n\n"
                                      "Restart the Amiga and run Quick setup again. If it\n"
                                      "fails again, try another card.";
static const char kQsVolumeFailed[] = "Checking % failed: the new volume can't be used,\n"
                                      "so the card is not ready.\n\n"
                                      "Restart the Amiga and run Quick setup again. If it\n"
                                      "fails again, try another card.";
static const char kQsFlushFailed[] = "Saving the new volumes to the card failed, so it\n"
                                     "is not ready.\n\n"
                                     "Restart the Amiga and run Quick setup again. If it\n"
                                     "fails again, try another card.";
/* The same three on the restart path: the marker stays, so the next start brings the popup. */
static const char kQsResumeFormatFailed[] = "Formatting % failed, so the card is not ready.\n\n"
                                            "Restart the Amiga with this same disk and HDPart\n"
                                            "will offer to finish. If it fails again, try\n"
                                            "another card.";
static const char kQsResumeVolumeFailed[] = "Checking % failed: the new volume can't be used,\n"
                                            "so the card is not ready.\n\n"
                                            "Restart the Amiga with this same disk and HDPart\n"
                                            "will offer to finish. If it fails again, try\n"
                                            "another card.";
static const char kQsResumeFlushFailed[] = "Saving the new volumes to the card failed, so it\n"
                                           "is not ready.\n\n"
                                           "Restart the Amiga with this same disk and HDPart\n"
                                           "will offer to finish. If it fails again, try\n"
                                           "another card.";
/* The marker may or may not have gone, so these say "may". */
static const char kQsClearFailed[] = "The new volumes are formatted, but Quick setup\n"
                                     "could not update the card afterwards. You can go\n"
                                     "on and install Workbench.\n\n"
                                     "The next time HDPart starts it may ask to finish\n"
                                     "setting up this card. Cancel there keeps the\n"
                                     "volumes as they are. Continue erases them again.";
static const char kQsCancelFailed[] = "Quick setup could not update the card, so HDPart\n"
                                      "may ask about it again the next time it starts.\n\n"
                                      "Nothing more was erased, but your old partition\n"
                                      "table was already replaced.";
static const char kQsRestartLine[] = "Restart with this same disk to finish setting up the card";
static const char kQsRestartMore[] = "The new partitions are on the card, ready to format after the restart.";
static const char kQsDoneNoWindow[] = "Your card is ready for Workbench.\n\n"
                                      "Insert your Install disk, restart the Amiga and\n"
                                      "install Workbench to System (DH0:).";
static const char *const kQsDone[5] = {
    "Your card is ready for Workbench.",
    "1. Remove this disk and insert your Install disk.",
    "2. Press Restart.",
    "3. Install Workbench to System (DH0:).",
    "4. Remove the floppy and restart. Your Amiga now starts from the card."
};

enum { QS_RUN_STOPPED = 0, QS_RUN_DONE, QS_RUN_FAILED, QS_RUN_MARKED, QS_RUN_CANCELLED };
enum { QS_STEP_CHARS = 40, QS_STEPS = 6, QS_DONE_RESTART = 1, QS_DONE_BACK = 2 };
#define QS_WK_BAR_H 8                   /* the progress bar under the checklist */
#define QS3_W       584                 /* screen 6's longest line is 70 characters */
#define QS_DONE_BTN_H 20                /* Restart is the big one, as what to do next */

static char          g_qs_steps[QS_STEPS][QS_STEP_CHARS];   /* the checklist, as drawn */
static volatile int  g_qs_failed;       /* the step that failed, from 1. 0: none */
static struct Window *g_qs_wk;          /* the working window */
static int           g_qs_nsteps, g_qs_ndone;
static int           g_qs_live;         /* the last check found this card's blocks mounted */
static char          g_qs_live_names[8][8];   /* and by what, for the flush on the way to Restart */
static int           g_qs_live_n;
static char          g_qs_resume_rows[4][60]; /* the popup's words, as drawn */

/* Say why a run stopped or failed. '#' and '%' fill in; the text stays for the test to read. */
static void qs_say(const char *t, const char *vol)
{
    int p = 0, i;
    for (i = 0; t[i]; i++) {
        if (t[i] == '%') s_cat(g_qs_said, &p, vol ? vol : "");
        else g_qs_said[p++] = (t[i] == '#') ? (char)('0' + g_qs_taken) : t[i];
    }
    g_qs_said[p] = 0;
}

static void qs_working_draw(void)
{
    struct RastPort *rp;
    int x = g_leftb + 4, bw = QS2_W - 8, tableH = g_qs_nsteps * QS_CF_ROW_H + 8, i, y;
    if (!g_qs_wk) return;
    rp = g_qs_wk->RPort;
    DrawBevelBox(rp, x, g_topb + 3, bw, tableH, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    for (i = 0; i < g_qs_nsteps; i++) {
        y = g_topb + 3 + 4 + i * QS_CF_ROW_H;
        qs_clear(rp, x + 2, y, bw - 4, QS_CF_ROW_H);
        qs_text(rp, x + 8, y, g_qs_steps[i]);
    }
    y = g_topb + 3 + tableH + 4;
    DrawBevelBox(rp, x, y, bw, QS_WK_BAR_H, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    qs_clear(rp, x + 2, y + 1, bw - 4, QS_WK_BAR_H - 2);
    if (g_qs_ndone > 0) {
        SetAPen(rp, g_pen_fill);
        RectFill(rp, x + 2, y + 1, x + 2 + (bw - 4) * g_qs_ndone / g_qs_nsteps - 1, y + QS_WK_BAR_H - 2);
    }
}

/* Answer the window while the run is between steps. Nothing on it can be clicked. */
static void qs_working_show(void)
{
    struct IntuiMessage *im;
    qs_working_draw();
    while ((im = GT_GetIMsg(g_qs_wk->UserPort)) != 0) {
        ULONG cl = im->Class;
        GT_ReplyIMsg(im);
        if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(g_qs_wk); qs_working_draw(); GT_EndRefresh(g_qs_wk, TRUE); }
    }
}

/* One checklist row: its mark, then two words at most. */
static void qs_step(int i, const char *mark, const char *a, const char *b)
{
    int p = 0;
    s_cat(g_qs_steps[i], &p, mark);
    s_cat(g_qs_steps[i], &p, "  ");
    s_cat(g_qs_steps[i], &p, a);
    if (b) s_cat(g_qs_steps[i], &p, b);
    g_qs_steps[i][p] = 0;
}

static void qs_table_written(void)
{
    qs_step(0, "ok", "Partition table written", 0);
    qs_step(1, "..", "Reading the table back", 0);
    g_qs_ndone = 1;
    qs_working_show();
}

/* Do two tables say everything the same, and not just the same cylinders? */
static int qs_tables_same(const RdbModel *c, const RdbModel *m)
{
    int i;
    if (c->num_parts != m->num_parts || c->cyl_blocks != m->cyl_blocks || c->hi_cyl != m->hi_cyl ||
        c->lo_cyl != m->lo_cyl || c->setup_pending != m->setup_pending) return 0;
    for (i = 0; i < m->num_parts; i++) {
        const RdbPartition *a = &c->parts[i], *b = &m->parts[i];
        if (!streq(a->name, b->name) || a->low_cyl != b->low_cyl || a->high_cyl != b->high_cyl ||
            a->dos_type != b->dos_type || a->bootable != b->bootable || a->boot_pri != b->boot_pri ||
            a->flags != b->flags || a->maxtransfer != b->maxtransfer || a->mask != b->mask ||
            a->num_buffers != b->num_buffers || a->bufmemtype != b->bufmemtype) return 0;
    }
    return 1;
}

/* Does the read-back table say everything the wizard meant? */
static int __attribute__((noinline)) qs_table_matches(const RdbModel *m)
{
    return qs_tables_same(&g_wv_chk, m);
}

/* Is the marker on the card, and is the table everything the wizard would have written for
   it on this card? Reads the card and nothing else. Anything else, or any doubt, is a no.
   The mask goes to *mask_out. */
static int __attribute__((noinline)) qs_marker_on_card(const DiscDisk *card, uint32_t *mask_out)
{
    static RdbModel plan;
    static DeviceInfo again;
    const RdbModel *c = &g_qs_model;
    DeviceHandle *h = dev_open(card->driver, card->unit);
    int rc, nfs;
    if (!h) return 0;
    (void)dev_geometry(h, &again);           /* tells the handle the block size */
    rc = rdb_parse(&g_qs_model, dev_block_io, h);
    nfs = g_qs_model.num_fs;
    rdb_model_free(&g_qs_model);
    dev_close(h);
    if (rc != RDB_OK || nfs != 0 || !quick_pending_valid(c)) return 0;
    rdb_init_model(&plan, g_qs_geo.cylinders, g_qs_geo.heads, g_qs_geo.sectors);
    if (quick_layout(&plan, c->setup_pending & 7u) != RDB_OK) return 0;
    plan.setup_pending = c->setup_pending;
    if (c->cylinders != plan.cylinders || c->heads != plan.heads || c->sectors != plan.sectors ||
        !qs_tables_same(c, &plan)) return 0;
    *mask_out = c->setup_pending & 7u;
    return 1;
}

/* Ask the driver to let go of the table it was just given. */
static int qs_table_update(const DiscDisk *card)
{
    DeviceHandle *h = dev_open(card->driver, card->unit);
    int rc;
    if (!h) return 1;
    rc = dev_update(h);
    dev_close(h);
    return rc;
}

/* Is this partition's device up, on the card, with a validated volume of the right name, type
   and size? Goes by device name: a floppy called System must not pass for the card. A lock
   by volume name could find either one. */
static int __attribute__((noinline)) qs_volume_ok(const DiscDisk *card, const RdbModel *m, int idx, const char *vol)
{
    static struct InfoData info __attribute__((aligned(4)));
    static char path[RDB_NAME_LEN + 2];
    const RdbPartition *pt = &m->parts[idx];
    uint32_t blocks = (pt->high_cyl - pt->low_cyl + 1) * m->cyl_blocks;
    char live[8];
    int tries, p = 0;

    s_cat(path, &p, pt->name);
    path[p++] = ':';
    path[p] = 0;
    /* It may still be coming up after the format: about 12 seconds. */
    for (tries = 0; tries < 60; tries++) {
        if (safety_partition_mounted(card->driver, card->unit, pt->low_cyl, pt->high_cyl, live) &&
            qs_name_eq(live, pt->name)) {
            BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
            if (lock) {
                int answer = -1;
                if (Info(lock, &info) && info.id_DiskState == ID_VALIDATED) {
                    struct FileLock *fl = (struct FileLock *)BADDR(lock);
                    struct DosList *dl = (struct DosList *)BADDR(fl->fl_Volume);
                    const UBYTE *b = (const UBYTE *)BADDR(dl->dol_Name);
                    static char name[32];
                    int k;
                    for (k = 0; k < b[0] && k < 31; k++) name[k] = (char)b[1 + k];
                    name[k] = 0;
                    answer = qs_name_eq(name, vol) && (uint32_t)info.id_DiskType == pt->dos_type &&
                             (uint32_t)info.id_NumBlocks == blocks - 2;
                }
                UnLock(lock);
                if (answer >= 0) return answer;
            }
        }
        Delay(10);
    }
    return 0;
}

/* Get every partition's volume onto the card, out of its filesystem's buffers. Inhibits nothing.
   No library or font opens here, and it loads nothing: this also runs after the floppy is out.
   Returns 0, or 1 + the index of the first partition that failed. */
static int __attribute__((noinline)) qs_flush(const RdbModel *m)
{
    static char path[RDB_NAME_LEN + 2];
    int i, failed = 0;
    for (i = 0; i < m->num_parts; i++) {
        struct MsgPort *port;
        int p = 0;
        s_cat(path, &p, m->parts[i].name);
        path[p++] = ':';
        path[p] = 0;
        port = DeviceProc((CONST_STRPTR)path);
        if ((!port || !DoPkt(port, ACTION_FLUSH, 0L, 0L, 0L, 0L, 0L)) && !failed) failed = i + 1;
    }
    return failed;
}

/* The same for what stands on the card's blocks before the restart: the old volumes, by the
   names the last check found. Asked for before Restart can be pressed, so this loads nothing. */
static void __attribute__((noinline)) qs_flush_live(void)
{
    static char path[10];
    int i;
    for (i = 0; i < g_qs_live_n; i++) {
        struct MsgPort *port;
        int p = 0;
        s_cat(path, &p, g_qs_live_names[i]);
        path[p++] = ':';
        path[p] = 0;
        port = DeviceProc((CONST_STRPTR)path);
        if (port) (void)DoPkt(port, ACTION_FLUSH, 0L, 0L, 0L, 0L, 0L);
    }
}

/* Write the table, read it back and see it says what it should. */
static int qs_table_steps(const DiscDisk *card, RdbModel *m)
{
    int wv;

    qs_step(0, "..", "Writing the partition table", 0);
    qs_working_show();
    wv = qs_write_verified(card->driver, card->unit, m, qs_table_written);
    if (wv == WV_NO_DEVICE || wv == WV_WRITE_FAILED || wv == WV_NO_SPACE) {
        g_qs_failed = 1;
        qs_step(0, "!!", "Writing the partition table", 0);
        qs_say(kQsWriteFailed, 0);
        return QS_RUN_FAILED;
    }
    if (wv != WV_OK || !qs_table_matches(m)) {
        g_qs_failed = 2;
        qs_step(1, "!!", "Reading the table back", 0);
        qs_say(kQsCheckFailed, 0);
        return QS_RUN_FAILED;
    }
    qs_step(1, "ok", "Table read back and checked", 0);
    g_qs_ndone = 2;
    return QS_RUN_DONE;
}

/* May this partition be formatted now? On a first run, only if nothing is on its blocks: a
   format would go to the old mount, wherever that is on the card. Only the restart path may
   go through a mount, and only one this table made: see safety_resume_decide. */
static int qs_may_format(const DiscDisk *card, const RdbModel *m, int i, int resume)
{
    const RdbPartition *pt = &m->parts[i];
    char live[8];
    if (resume)
        return safety_resume_mount(card->driver, card->unit, pt->name, pt->low_cyl, pt->high_cyl,
                                   m->cyl_blocks, pt->dos_type) != SAFETY_RESUME_MISMATCH;
    return !safety_partition_mounted(card->driver, card->unit, pt->low_cyl, pt->high_cyl, live);
}

/* Format every partition, check it and flush them all, with the checklist's rows from `base`. */
static int qs_format_steps(const DiscDisk *card, RdbModel *m, uint32_t mask, int base, int resume)
{
    int i;
    for (i = 0; i < m->num_parts; i++) {
        const char *vol = quick_volume_name(mask, i);
        qs_step(base + i, "..", "Formatting ", vol);
        qs_working_show();
        if (!qs_may_format(card, m, i, resume) ||
            format_partition(card->driver, card->unit, m, i, vol) != FMT_OK) {
            g_qs_failed = base + 1 + i;
            qs_step(base + i, "!!", "Formatting ", vol);
            qs_say(resume ? kQsResumeFormatFailed : kQsFormatFailed, vol);
            return QS_RUN_FAILED;
        }
        if (!qs_volume_ok(card, m, i, vol)) {
            g_qs_failed = base + 1 + i;
            qs_step(base + i, "!!", "Checking ", vol);
            qs_say(resume ? kQsResumeVolumeFailed : kQsVolumeFailed, vol);
            return QS_RUN_FAILED;
        }
        qs_step(base + i, "ok", vol, " formatted");
        g_qs_ndone = base + 1 + i;
    }
    qs_working_show();
    i = qs_flush(m);
    if (i) {
        g_qs_failed = base + i;
        qs_step(base - 1 + i, "!!", "Saving ", quick_volume_name(mask, i - 1));
        g_qs_ndone = base - 1 + i;
        qs_say(resume ? kQsResumeFlushFailed : kQsFlushFailed, 0);
        return QS_RUN_FAILED;
    }
    return QS_RUN_DONE;
}

/* The steps with the window up. Everything goes to card->unit and nowhere else. A card with
   live volumes gets only the table, with the marker: formatting waits for the restart. */
static int qs_run_steps(const DiscDisk *card, RdbModel *m, uint32_t mask)
{
    int rc = qs_table_steps(card, m);
    if (rc != QS_RUN_DONE) return rc;
    if (qs_table_update(card) != 0) {
        g_qs_failed = 2;
        qs_step(1, "!!", "Reading the table back", 0);
        qs_say(kQsCheckFailed, 0);
        return QS_RUN_FAILED;
    }
    if (g_qs_live) return QS_RUN_MARKED;
    return qs_format_steps(card, m, mask, 2, 0);
}

/* Write the table without its marker, and see that it says so. Formats nothing. */
static int qs_clear_marker(const DiscDisk *card, RdbModel *m)
{
    m->setup_pending = 0;
    return qs_write_verified(card->driver, card->unit, m, 0) == WV_OK && qs_table_matches(m) &&
           qs_table_update(card) == 0;
}

/* Is the machine still as the card screen found it, and is this still the card that was
   confirmed? Runs every check again. Leaves m laid out as confirmed. Says why not and
   returns 0 if anything differs. Writes nothing. */
static int qs_recheck(const DiscDisk *card, RdbModel *m, uint32_t mask)
{
    static DeviceInfo was;
    static RdbModel plan;
    static DiscDisk now;
    static char holds[QS_VALUE_CHARS + 2];
    DevLiveness lv;
    int i, same, rc;

    was = g_qs_geo;
    plan = *m;
    for (i = 0; i < (int)sizeof(holds); i++) holds[i] = g_qs_holds[i];

    gui_status("Checking your card...");
    rc = qs_find_card(&now);                 /* lays out g_qs_layout, which may be m: see below */
    gui_status("");
    if (rc) {
        g_qs_refusal = rc;
        qs_say(kQsRefusals[rc], 0);
        gui_msg("Quick setup", g_qs_said);
        return 0;
    }
    rdb_init_model(m, g_qs_geo.cylinders, g_qs_geo.heads, g_qs_geo.sectors);
    same = quick_layout(m, mask) == RDB_OK;
    same = same && streq(now.driver, card->driver) && now.unit == card->unit &&
           streq(g_qs_geo.model, was.model) && g_qs_geo.total_blocks == was.total_blocks &&
           g_qs_geo.block_bytes == was.block_bytes && streq(g_qs_holds, holds) &&
           m->cylinders == plan.cylinders && m->cyl_blocks == plan.cyl_blocks &&
           m->hi_cyl == plan.hi_cyl && m->num_parts == plan.num_parts;
    for (i = 0; same && i < m->num_parts; i++)
        same = m->parts[i].low_cyl == plan.parts[i].low_cyl && m->parts[i].high_cyl == plan.parts[i].high_cyl;
    if (!same) {
        qs_say(kQsChanged, 0);
        gui_msg("Quick setup", g_qs_said);
        return 0;
    }
    /* Any live mount on the card: the single-run flow is proven unsafe, in
       docs/reference/quick-setup-tests.md, so qs_run goes by the restart. */
    g_qs_live_n = 0;
    lv = safety_classify(card->driver, card->unit, g_qs_live_names, 8, &g_qs_live_n);
    if (lv == DEV_BOOT) {
        qs_say(kQsRefusals[QS_R_BOOTED], 0);
        gui_msg("Quick setup", g_qs_said);
        return 0;
    }
    g_qs_live = lv != DEV_CLEAR;
    return 1;
}

/* The same before the popup's answer acts, and the marker must still be there saying the same. */
static int qs_resume_recheck(const DiscDisk *card, RdbModel *m, uint32_t mask)
{
    uint32_t now = 0;
    if (!qs_recheck(card, m, mask)) return 0;
    if (!qs_marker_on_card(card, &now) || now != mask) {
        qs_say(kQsChanged, 0);
        gui_msg("Quick setup", g_qs_said);
        return 0;
    }
    return 1;
}

/* Open the working window for g_qs_nsteps rows. A disk problem from here to qs_working_end is
   the run's to report, not a system requester's. */
static APTR g_qs_oldwin;
static int qs_working_begin(void)
{
    struct Process *me = (struct Process *)FindTask(0);
    int dwL = 0, dwT = 0;
    int dwW = g_leftb + QS2_W + g_scr->WBorRight;
    int dwH = g_topb + 3 + g_qs_nsteps * QS_CF_ROW_H + 8 + 4 + QS_WK_BAR_H + 6 + g_scr->WBorBottom;
    dlg_center(dwW, dwH, &dwL, &dwT);
    g_qs_wk = dlg_open("Quick setup: working", dwL, dwT, dwW, dwH, 0, 0);
    if (!g_qs_wk) { gui_msg("Quick setup", kQsNoWindow); return 0; }
    g_qs_screen = 5;
    g_qs_oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    return 1;
}

static void qs_working_end(int rc)
{
    ((struct Process *)FindTask(0))->pr_WindowPtr = g_qs_oldwin;
    qs_working_show();
    if (rc == QS_RUN_FAILED) gui_msg("Quick setup", g_qs_said);
    dlg_close(g_qs_wk, 0);
    g_qs_wk = 0;
}

/* Erase the confirmed card and set it up, with the checklist drawn as it goes. Returns
   QS_RUN_DONE, QS_RUN_MARKED (the table is written and the rest waits for a restart),
   QS_RUN_FAILED (something was written), or QS_RUN_STOPPED (nothing was). */
static int qs_run(const DiscDisk *card, RdbModel *m, uint32_t mask)
{
    int n = m->num_parts, rc, i;

    g_qs_failed = 0;
    /* Every partition must have its volume name, and every name its partition. */
    rc = n >= 1 && n <= 4 && quick_volume_name(mask, n) == 0;
    for (i = 0; rc && i < n; i++) rc = quick_volume_name(mask, i) != 0;
    if (!rc) {
        qs_say(kQsLostTrack, 0);
        gui_msg("Quick setup", g_qs_said);
        return QS_RUN_STOPPED;
    }
    if (!qs_recheck(card, m, mask)) return QS_RUN_STOPPED;
    /* After the check, which lays m out afresh. The read-back proves it reached the card. */
    m->setup_pending = g_qs_live ? (uint8_t)(0x80u | mask) : 0;

    g_qs_nsteps = g_qs_live ? 2 : n + 2;
    g_qs_ndone = 0;
    qs_step(0, "  ", "Writing the partition table", 0);
    qs_step(1, "  ", "Reading the table back", 0);
    for (i = 2; i < QS_STEPS; i++) g_qs_steps[i][0] = 0;
    if (!g_qs_live) for (i = 0; i < n; i++) qs_step(2 + i, "  ", "Formatting ", quick_volume_name(mask, i));

    if (!qs_working_begin()) return QS_RUN_STOPPED;
    rc = qs_run_steps(card, m, mask);
    qs_working_end(rc);
    return rc;
}

/* The popup's Continue: format everything through the mounts the table made, then write the
   table again without its marker. If a format fails the marker stays. */
static int qs_resume_steps(const DiscDisk *card, RdbModel *m, uint32_t mask)
{
    int n = m->num_parts, rc = qs_format_steps(card, m, mask, 0, 1);
    if (rc != QS_RUN_DONE) return rc;
    qs_step(n, "..", "Finishing the card", 0);
    qs_working_show();
    if (!qs_clear_marker(card, m)) {
        g_qs_failed = n + 1;
        qs_step(n, "!!", "Finishing the card", 0);
        qs_say(kQsClearFailed, 0);
        return QS_RUN_FAILED;
    }
    qs_step(n, "ok", "Card finished", 0);
    g_qs_ndone = n + 1;
    return QS_RUN_DONE;
}

static int qs_resume_run(const DiscDisk *card, RdbModel *m, uint32_t mask)
{
    int n = m->num_parts, rc, i;
    g_qs_failed = 0;
    if (!qs_resume_recheck(card, m, mask)) return QS_RUN_STOPPED;

    g_qs_nsteps = n + 1;
    g_qs_ndone = 0;
    for (i = 0; i < QS_STEPS; i++) g_qs_steps[i][0] = 0;
    for (i = 0; i < n; i++) qs_step(i, "  ", "Formatting ", quick_volume_name(mask, i));
    qs_step(n, "  ", "Finishing the card", 0);

    if (!qs_working_begin()) return QS_RUN_STOPPED;
    rc = qs_resume_steps(card, m, mask);
    qs_working_end(rc);
    return rc;
}

/* The popup's Cancel: the table stays and the marker goes, so the popup never comes back. */
static int qs_resume_cancel(const DiscDisk *card, RdbModel *m, uint32_t mask)
{
    struct Process *me = (struct Process *)FindTask(0);
    APTR oldWin;
    int ok;
    if (!qs_resume_recheck(card, m, mask)) return QS_RUN_STOPPED;
    oldWin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    ok = qs_clear_marker(card, m);
    me->pr_WindowPtr = oldWin;
    if (ok) return QS_RUN_CANCELLED;
    qs_say(kQsCancelFailed, 0);
    gui_msg("Quick setup", g_qs_said);
    return QS_RUN_FAILED;
}

static void qs_finished_draw(struct Window *w, int boxH)
{
    struct RastPort *rp = w->RPort;
    int x = g_leftb + 4, i;
    DrawBevelBox(rp, x, g_topb + 3, QS3_W - 8, boxH, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    qs_text(rp, x + 8, g_topb + 3 + 4, kQsDone[0]);
    for (i = 1; i < 5; i++) qs_text(rp, x + 8, g_topb + 3 + 8 + i * QS_CF_ROW_H, kQsDone[i]);
}

/* Screen 6. Returns QS_DONE_RESTART or QS_DONE_BACK. The user takes the floppy out while this
   is up, so whatever follows Restart must not load anything: see gui_quick_setup. */
static int qs_finished(void)
{
    struct Window *dw;
    struct Gadget *glist = 0, *g;
    struct NewGadget ng;
    int boxH = 5 * QS_CF_ROW_H + 12, btnY = 3 + boxH + 6;
    int dwW = g_leftb + QS3_W + g_scr->WBorRight, dwH = g_topb + btnY + QS_DONE_BTN_H + 6 + g_scr->WBorBottom;
    int dwL = 0, dwT = 0, done = 0, result = QS_DONE_BACK;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (g) {
        ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;
        ng.ng_TopEdge = g_topb + btnY; ng.ng_Width = 132; ng.ng_Height = QS_DONE_BTN_H;
        ng.ng_LeftEdge = g_leftb + 4; ng.ng_GadgetText = (UBYTE *)"Restart"; ng.ng_GadgetID = QS_DONE_RESTART;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (g) {
        ng.ng_Width = 128; ng.ng_Height = 14; ng.ng_LeftEdge = g_leftb + QS3_W - 4 - 128;
        ng.ng_TopEdge = g_topb + btnY + (QS_DONE_BTN_H - 14) / 2;
        ng.ng_GadgetText = (UBYTE *)"Back to HDPart"; ng.ng_GadgetID = QS_DONE_BACK;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (!g) { FreeGadgets(glist); gui_msg("Quick setup", kQsDoneNoWindow); return QS_DONE_BACK; }

    dlg_center(dwW, dwH, &dwL, &dwT);
    dw = dlg_open("Quick setup: done", dwL, dwT, dwW, dwH, BUTTONIDCMP, glist);
    if (!dw) { FreeGadgets(glist); gui_msg("Quick setup", kQsDoneNoWindow); return QS_DONE_BACK; }
    g_qs_screen = 6;
    GT_RefreshWindow(dw, 0);
    qs_finished_draw(dw, boxH);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while (!done && (im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) done = 1;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); qs_finished_draw(dw, boxH); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) { result = ig->GadgetID; done = 1; }
        }
    }
    dlg_close(dw, glist);
    return result;
}

/* Show the card in the main window, with nothing marked unsaved. The wizard has already
   proved it is the only disk on this driver, so no scan is needed. */
static void qs_show_in_main(const DiscDisk *card)
{
    int i, idx = -1;
    g_dirty = 0;
    gui_build_drivers();
    for (i = 1; g_drvlabels[i]; i++) if (streq(g_drvlabels[i], card->driver)) idx = i;
    if (idx < 0) { gui_init_picker(); return; }
    GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0, GTCY_Labels, (ULONG)g_drvlabels, GTCY_Active, (ULONG)idx, TAG_END);
    gui_select_driver(idx);
    g_disks[0] = *card;
    g_ndisks = 1;
    gui_build_units("(no disk found)");
    gui_set_units();
    gui_select_unit(0);
}

static void __attribute__((noinline)) qs_reboot(void) { ColdReboot(); }

static void qs_offer_draw(struct Window *w, int boxH)
{
    struct RastPort *rp = w->RPort;
    int x = g_leftb + 4;
    DrawBevelBox(rp, x, g_topb + 3, QS3_W - 8, boxH, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    qs_text(rp, x + 8, g_topb + 3 + 4, kQsRestartLine);
    qs_text(rp, x + 8, g_topb + 3 + 4 + 2 * QS_CF_ROW_H, kQsRestartMore);
}

/* The table is on the card and the old volumes are still live, so going back to the main window
   would leave them in use over a table that no longer describes them. Restart is the only way
   on, and there is no close gadget. With no window for it, say so and return. */
static void qs_restart_offer(void)
{
    struct Window *dw;
    struct Gadget *glist = 0, *g;
    struct NewGadget ng;
    int boxH = 3 * QS_CF_ROW_H + 8, btnY = 3 + boxH + 6;
    int dwW = g_leftb + QS3_W + g_scr->WBorRight, dwH = g_topb + btnY + QS_DONE_BTN_H + 6 + g_scr->WBorBottom;
    int dwL = 0, dwT = 0, done = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (g) {
        ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;
        ng.ng_TopEdge = g_topb + btnY; ng.ng_Width = 132; ng.ng_Height = QS_DONE_BTN_H;
        ng.ng_LeftEdge = g_leftb + 4; ng.ng_GadgetText = (UBYTE *)"Restart"; ng.ng_GadgetID = QS_DONE_RESTART;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    dw = 0;
    if (g) {
        dlg_center(dwW, dwH, &dwL, &dwT);
        /* No depth gadget: sent behind the main window, this would look frozen. */
        dw = dlg_open_flags("Quick setup: restart", dwL, dwT, dwW, dwH, BUTTONIDCMP, glist, 0);
    }
    if (!dw) { FreeGadgets(glist); gui_msg("Quick setup", kQsRestartLine); return; }
    g_qs_screen = 8;
    GT_RefreshWindow(dw, 0);
    qs_offer_draw(dw, boxH);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while (!done && (im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); qs_offer_draw(dw, boxH); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) done = 1;
        }
    }
    dlg_close(dw, glist);
    /* As at the end of the Finished screen: nothing may load from disk or ask for a volume. */
    ((struct Process *)FindTask(0))->pr_WindowPtr = (APTR)-1;
    qs_flush_live();
    qs_reboot();
}

/* Whatever a run ended in, on to what comes next. */
static void qs_after_run(const DiscDisk *card, RdbModel *m, int r)
{
    if (r == QS_RUN_MARKED) qs_restart_offer();
    if (r == QS_RUN_DONE && qs_finished() == QS_DONE_RESTART) {
        /* The floppy is out by now. From here to the reboot nothing may load from
           disk or ask for a volume: no library, no font, no requester, just a
           packet to each handler. Tested by "Restart" in tests/target/quick.mjs. */
        ((struct Process *)FindTask(0))->pr_WindowPtr = (APTR)-1;
        (void)qs_flush(m);
        qs_reboot();
    }
    /* The main window's table is out of date once anything was written. */
    if (r != QS_RUN_STOPPED) qs_show_in_main(card);
}

static void qs_resume_draw(struct Window *w, int boxH)
{
    struct RastPort *rp = w->RPort;
    int x = g_leftb + 4, i;
    DrawBevelBox(rp, x, g_topb + 3, QS_W - 8, boxH, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    for (i = 0; i < 4 && g_qs_resume_rows[i][0]; i++) qs_text(rp, x + 8, g_topb + 3 + 4 + i * QS_CF_ROW_H, g_qs_resume_rows[i]);
}

/* "Finish setting up this card?" with the names the marker gives. Returns QS_ANS_NEXT for
   Continue or QS_ANS_CANCEL for Cancel, or -1 with no window, which is no popup at all. */
static int qs_resume_screen(uint32_t mask, int has_files)
{
    struct Window *dw;
    struct Gadget *glist = 0, *g;
    struct NewGadget ng;
    int boxH = (has_files ? 4 : 3) * QS_CF_ROW_H + 8, btnY = 3 + boxH + 6;
    int dwW = g_leftb + QS_W + g_scr->WBorRight, dwH = g_topb + btnY + 14 + 6 + g_scr->WBorBottom;
    int dwL = 0, dwT = 0, done = 0, result = QS_ANS_CANCEL, i, p = 0;
    const char *vol;

    for (i = 0; (vol = quick_volume_name(mask, i)) != 0; i++) {
        if (i) s_cat(g_qs_resume_rows[1], &p, ", ");
        s_cat(g_qs_resume_rows[1], &p, vol);
    }
    s_cat(g_qs_resume_rows[1], &p, ".");
    g_qs_resume_rows[1][p] = 0;
    p = 0; s_cat(g_qs_resume_rows[0], &p, "Finish setting up this card?"); g_qs_resume_rows[0][p] = 0;
    p = 0; s_cat(g_qs_resume_rows[2], &p, "Everything on them will be erased."); g_qs_resume_rows[2][p] = 0;
    p = 0; if (has_files) s_cat(g_qs_resume_rows[3], &p, "This card already has files on it."); g_qs_resume_rows[3][p] = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&glist);
#pragma GCC diagnostic pop
    if (g) {
        ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;
        ng.ng_TopEdge = g_topb + btnY; ng.ng_Width = 90; ng.ng_Height = 14;
        ng.ng_LeftEdge = g_leftb + 4; ng.ng_GadgetText = (UBYTE *)"Continue"; ng.ng_GadgetID = 1;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (g) {
        ng.ng_LeftEdge = g_leftb + QS_W - 4 - 90; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 0;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    dw = 0;
    if (g) {
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open_flags("Quick setup: finish", dwL, dwT, dwW, dwH, BUTTONIDCMP, glist, 0);
    }
    if (!dw) { FreeGadgets(glist); return -1; }
    g_qs_screen = 7;
    GT_RefreshWindow(dw, 0);
    qs_resume_draw(dw, boxH);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while (!done && (im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); qs_resume_draw(dw, boxH); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) { result = ig->GadgetID == 1 ? QS_ANS_NEXT : QS_ANS_CANCEL; done = 1; }
        }
    }
    dlg_close(dw, glist);
    return result;
}

/* Does any partition of this table hold files? Only a valid root block whose directory has an
   entry counts, so a freshly formatted one does not. A read that fails counts for nothing. */
static int __attribute__((noinline)) qs_card_has_files(const DiscDisk *card, const RdbModel *m)
{
    static DeviceInfo again;
    DeviceHandle *h = dev_open(card->driver, card->unit);
    int i, k, found = 0;
    if (!h) return 0;
    (void)dev_geometry(h, &again);           /* tells the handle the block size */
    for (i = 0; i < m->num_parts && !found; i++) {
        if (!qs_read_root(h, &m->parts[i], m->cyl_blocks) || g_qs_block[3] != 72) continue;
        for (k = 0; k < 72 && !found; k++) found = g_qs_block[6 + k] != 0;
    }
    dev_close(h);
    return found;
}

/* Everything queued for the main window while the popup and what it led to were up: a press
   from then is no longer about this card, and must not open anything now. */
static void qs_drop_queued(void)
{
    struct IntuiMessage *im;
    while ((im = GT_GetIMsg(g_win->UserPort)) != 0) {
        ULONG cl = im->Class;
        GT_ReplyIMsg(im);
        if (cl == IDCMP_REFRESHWINDOW) {
            GT_BeginRefresh(g_win);
            gui_draw_chrome();
            gui_draw_bar();
            GT_EndRefresh(g_win, TRUE);
        }
    }
    if (g_gad[GID_DEVICE])
        GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0, GTCY_Active, (ULONG)g_drv_idx, TAG_END);
}

/* Is there a card to offer the popup for? Sets g_qs_check_stop to the gate that said no:
   1 not the IDE port, 10 + a QS_R_* reason the card was refused, 3 no marker that matches.
   The caller adds 5: the popup's window could not open. */
static int qs_resume_wanted(uint32_t *mask)
{
    int rc;
    if (!safety_is_internal_ide()) { g_qs_check_stop = 1; return 0; }
    rc = qs_find_card(&g_qs_card);
    if (rc != 0) { g_qs_check_stop = 10 + rc; return 0; }
    if (!qs_marker_on_card(&g_qs_card, mask)) { g_qs_check_stop = 3; return 0; }
    rdb_init_model(&g_qs_layout, g_qs_geo.cylinders, g_qs_geo.heads, g_qs_geo.sectors);
    if (quick_layout(&g_qs_layout, *mask) != RDB_OK) { g_qs_check_stop = 3; return 0; }
    return 1;
}

/* Once, at startup, with the main window up. The only thing that opens the wizard by itself:
   the same conditions as the refusals, then the marker on the card it would use, with a table
   that matches it. Reads only. Any doubt means nothing happens and nothing is said. The window
   answers nothing meanwhile, which the status line accounts for. */
static void gui_quick_resume_check(void)
{
    static char status[sizeof(g_statusbuf)];
    uint32_t mask = 0;
    int i, r, wanted, has_files;

    g_qs_resume_checks++;
    g_qs_check_stop = 0;
    for (i = 0; i < (int)sizeof(status); i++) status[i] = g_statusbuf[i];
    gui_status("Looking for your card...");
    wanted = qs_resume_wanted(&mask);
    has_files = wanted && qs_card_has_files(&g_qs_card, &g_qs_layout);     /* only the wording */
    gui_status(status);
    if (!wanted) return;

    g_qs_mask = mask;
    r = qs_resume_screen(mask, has_files);
    if (r >= 0) {
        r = r == QS_ANS_NEXT ? qs_resume_run(&g_qs_card, &g_qs_layout, mask)
                             : qs_resume_cancel(&g_qs_card, &g_qs_layout, mask);
        gui_status(status);
        qs_after_run(&g_qs_card, &g_qs_layout, r);
        qs_drop_queued();
    } else {
        g_qs_check_stop = 5;
    }
    g_qs_screen = 0;
}

/* The wizard's entry, running one screen function per step. */
static void gui_quick_setup(void)
{
    static char status[sizeof(g_statusbuf)];
    int i;

    /* Back to HDPart replaces what the main window holds. */
    if (!gui_discard_ok("Quick setup")) return;

    for (i = 0; i < (int)sizeof(status); i++) status[i] = g_statusbuf[i];
    g_qs_screen = 2;
    gui_status("Looking for your card...");
    g_qs_refusal = qs_find_card(&g_qs_card);
    gui_status(status);

    if (g_qs_refusal) {
        qs_say(kQsRefusals[g_qs_refusal], 0);
        gui_msg("Quick setup", g_qs_said);
    } else {
        int step = 2;
        g_qs_mask = QS_WORK | QS_GAMES;
        while (step) {
            int r;
            if (step == 2) {
                g_qs_screen = 2;
                step = qs_card_screen() ? 3 : 0;
            } else if (step == 3) {
                r = qs_choose(&g_qs_card, &g_qs_layout, &g_qs_mask);
                step = r == QS_ANS_NEXT ? 4 : r == QS_ANS_BACK ? 2 : 0;
            } else if (step == 4) {
                r = qs_confirm(&g_qs_card, &g_qs_layout, g_qs_mask);
                step = r == QS_ANS_NEXT ? 5 : r == QS_ANS_BACK ? 3 : 0;
            } else {
                r = qs_run(&g_qs_card, &g_qs_layout, g_qs_mask);
                step = 0;
                gui_status(status);
                qs_after_run(&g_qs_card, &g_qs_layout, r);
            }
        }
    }
    g_qs_refusal = 0;
    g_qs_screen = 0;
}

/* A driver has been picked. The panel stays gone for this session. The gadget is freed at
   exit, since a click on it may still be waiting in the window's message port. */
static void gui_quick_hide(void)
{
    if (g_qs_panel_gone) return;
    g_qs_panel_gone = 1;
    if (g_win && g_gad[GID_QUICK]) RemoveGadget(g_win, g_gad[GID_QUICK]);
}

int gui_run(void)
{
    BOOL done = FALSE;

    g_qs_is_ide = safety_is_internal_ide();

    GadToolsBase = OpenLibrary("gadtools.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    if (!GadToolsBase || !GfxBase) {
        if (GadToolsBase) CloseLibrary(GadToolsBase);
        if (GfxBase) CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    /* asl.library is optional: it is not ROM-resident on KS2.04/3.x (it lives in
       LIBS: on the Workbench disk) and HDPart bundles none, so a bare floppy boot
       has no file requester. Try the system LIBS: first, then a Libs/ drawer next
       to the binary for anyone who copied one onto the disk themselves. Failing
       both, the pickers use the typed-path dialog and Project/Load asl.library...
       can still bring in a requester at any time. */
    AslBase = OpenLibrary("asl.library", 37);
    if (!AslBase) (void)asl_open_from_drawer("PROGDIR:Libs");

    /* Render on the Workbench screen, with our own topaz 8 for gadgets and drawn
       text, so a fixed-pixel layout survives any screen font: only the title bar
       and menus follow the screen font, and g_topb allows for that. A bare floppy
       boot still has a Workbench screen (LockPubScreen opens it). A machine with
       none at all gets a basic hi-res screen. SA_Font rather than SA_SysFont 0,
       which means the system DefaultFont: topaz 9 on a bare boot. */
    g_sfont = OpenFont(&g_font);   /* topaz 8; NULL -> not available on this boot */
    g_pub = LockPubScreen(0);
    if (g_pub) {
        g_scr = g_pub;
    } else {
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

    gui_load_pens();

    g_bar.x = RIGHT_X + 2 + g_leftb; g_bar.y = MAP_Y + 2 + g_topb;
    g_bar.w = RIGHT_W - 4;           g_bar.h = MAP_H - 4;
    g_ulist.x = LEFT_X + 2 + g_leftb; g_ulist.y = UNITS_Y + 2 + g_topb;
    g_ulist.w = LEFT_W - SCROLL_W - 5; g_ulist.h = UNITS_H - 4;
    g_ulist.rowh = UNIT_ROW_H; g_ulist.vis = UNIT_ROWS;
    g_plist.x = RIGHT_X + 2 + g_leftb; g_plist.y = TABLE_Y + 15 + g_topb;
    g_plist.w = RIGHT_W - SCROLL_W - 5; g_plist.h = PART_ROWS * PART_ROW_H;
    g_plist.rowh = PART_ROW_H; g_plist.vis = PART_ROWS;

    g_vi = GetVisualInfo(g_scr, TAG_END);
    if (!g_vi) goto cleanup_scr;

    if (!build_gadgets()) goto cleanup_gad;  /* FreeGadgets handles a partial/NULL chain */

    g_newlook = menus_use_newlook();   /* needed by WA_NewLookMenus, just below */

    g_win = OpenWindowTags(0,
        WA_Left, (g_scr->Width - (g_leftb + WIN_W + g_scr->WBorRight)) / 2,
        WA_Top,  g_scr->BarHeight + 4,
        WA_Width,  g_leftb + WIN_W + g_scr->WBorRight,
        WA_Height, g_topb + WIN_H + g_scr->WBorBottom,
        WA_Title, (ULONG)"HDPart 0.12",
        WA_Gadgets, (ULONG)g_glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MENUPICK | IDCMP_VANILLAKEY | CYCLEIDCMP | BUTTONIDCMP | ARROWIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                  WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        /* V39 tag, documented as ignored by V37 and earlier, so it is safe to
           pass unconditionally with the decision as its value. */
        WA_NewLookMenus, (ULONG)(g_newlook ? TRUE : FALSE),
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
    if (!g_win) goto cleanup_gad;

    if (g_sfont) SetFont(g_win->RPort, g_sfont);

    if (!build_menus()) goto cleanup_win;   /* new cleanup label, Step 6 */

    GT_RefreshWindow(g_win, 0);
    gui_draw_chrome();
    gui_init_picker();     /* no-probe picker: prompt + driver names */
    gui_draw_bar();        /* initial bar (Task 3a.4.1) */
    gui_quick_resume_check();

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
                        int row;
                        if (idx >= 0) gui_set_selection(idx);
                        else if ((row = gui_list_hit(&g_plist, (int)mx, (int)my)) >= 0) gui_set_selection(row);
                        else if ((row = gui_list_hit(&g_ulist, (int)mx, (int)my)) >= 0) gui_unit_picked(row);
                        else if (gui_hit_easter((int)mx, (int)my))
                            gui_request("HDPart", "You watched too many movies.", 0, "I know");
                    }
                    break;
                case IDCMP_GADGETDOWN:
                case IDCMP_MOUSEMOVE:
                    if (gad->GadgetID == GID_PARTS) gui_list_scrolled(&g_plist, (int)code);
                    else if (gad->GadgetID == GID_UNIT) gui_list_scrolled(&g_ulist, (int)code);
                    break;
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GID_DEVICE) gui_driver_picked((int)code);
                    else if (gad->GadgetID == GID_UNIT) gui_list_scrolled(&g_ulist, (int)code);
                    else if (gad->GadgetID == GID_PARTS) gui_list_scrolled(&g_plist, (int)code);
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
                    else if (gad->GadgetID == GID_QUICK) gui_quick_setup();
                    else if (gad->GadgetID == GID_DRIVER) gui_load_driver();
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
                            else if (it == IT_ASL) gui_load_asl();
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
                            else if (it == ID_QUICK) gui_quick_setup();
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
    if (g_qs_panel_gone && g_gad[GID_QUICK]) { FreeGadgets(g_gad[GID_QUICK]); g_gad[GID_QUICK] = 0; }
    FreeVisualInfo(g_vi); g_vi = 0;   /* reached by fall-through from cleanup_gad */
cleanup_scr:
    if (g_dri) { FreeScreenDrawInfo(g_scr, g_dri); g_dri = 0; }
    if (!g_pub && g_scr) CloseScreen(g_scr);
    if (g_pub) UnlockPubScreen(0, g_pub);
    g_scr = 0; g_pub = 0;
cleanup_libs:
    if (g_sfont) { CloseFont(g_sfont); g_sfont = 0; }   /* before GfxBase */
    if (AslBase) { CloseLibrary(AslBase); AslBase = 0; }
    rdb_model_free(&g_model);
    fs_pool_free();
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
    g_drvlabels[n++] = "Select a driver";
    for (j = 0; j < nc && n < DISC_MAX + 1; j++) g_drvlabels[n++] = cand[j];
    g_drvlabels[n] = 0;
    return n;
}

/* Empty the Units list (on every driver change, so no stale unit can linger). */
static void gui_clear_units(const char *text)
{
    g_nunits = 0;
    g_unit_empty = text;
    g_unitmap[0] = -1;
}

/* Fill the Units list from the disks in g_disks[] on the target driver. Returns
   the number of units; `emptyText` is shown when there are none. */
static int gui_build_units(const char *emptyText)
{
    int i, n = 0, j, k;
    int sel[DISC_MAX];
    /* Collect the disks belonging to the current driver. */
    for (i = 0; i < g_ndisks && n < DISC_MAX; i++) {
        DiscDisk *d = &g_disks[i];
        if (!d->partitionable || !streq(d->driver, g_target_driver)) continue;
        sel[n++] = i;
    }
    /* Sort by unit number ascending (selection sort; n <= 32). */
    for (j = 0; j < n - 1; j++) {
        k = j;
        for (i = j + 1; i < n; i++)
            if (g_disks[sel[i]].unit < g_disks[sel[k]].unit) k = i;
        if (k != j) { int tmp = sel[j]; sel[j] = sel[k]; sel[k] = tmp; }
    }
    for (j = 0; j < n; j++) {
        DiscDisk *d = &g_disks[sel[j]];
        char *t = g_unittext[j];
        int p = 0;
        s_cat(t, &p, "#");
        p += u2s(t + p, (ULONG)d->unit);
        s_pad(t, &p, 5);
        p += u2s(t + p, (ULONG)d->size_mb);
        s_cat(t, &p, " MB");
        t[p] = 0;
        g_unitmap[j] = sel[j];
    }
    g_nunits = n;
    g_unit_empty = emptyText;
    if (n == 0) g_unitmap[0] = -1;
    return n;
}

/* Redraw the Units list after its contents changed. */
static void gui_set_units(void)
{
    g_ulist.count = (WORD)g_nunits;
    if (g_ulist.top + g_ulist.vis > g_ulist.count)
        g_ulist.top = (WORD)((g_ulist.count > g_ulist.vis) ? g_ulist.count - g_ulist.vis : 0);
    gui_list_changed(&g_ulist, GID_UNIT);
}

/* Driver cycle handler. ANY driver change fully resets state — model, edit
   flags, partition selection, current disk, AND the Units list — so nothing from
   the previous driver lingers. */
static void gui_select_driver(int idx)
{
    g_drv_idx = (idx > 0) ? idx : 0;
    g_have_model = 0; g_dirty = 0; g_sel_part = -1; g_geo.has_media = 0;
    g_cur_driver[0] = 0; g_cur_unit = 0; g_cur_unitidx = -1;

    if (idx <= 0) {                               /* the prompt */
        g_target_driver[0] = 0;
        gui_clear_units("(no driver)");
        gui_set_units();
        gui_refresh_parts(); gui_update_buttons();
        gui_status("Select a driver, then Scan.");
        return;
    }
    gui_quick_hide();
    { const char *nm = g_drvlabels[idx]; int k;
      for (k = 0; k < 39 && nm[k]; k++) g_target_driver[k] = nm[k];
      g_target_driver[k] = 0; }
    gui_clear_units("(press Scan)");
    gui_set_units();
    gui_refresh_parts(); gui_update_buttons();
    gui_status("Press Scan to query driver.");
}

/* Show a listed unit's partitions (reads its RDB). */
static void gui_select_unit(int uidx)
{
    DeviceHandle *h; DiscDisk *d; int k;

    g_have_model = 0; g_dirty = 0; g_sel_part = -1; g_geo.has_media = 0; g_no_rdb = 0;
    g_plist.top = 0;
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
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) {
            g_have_model = 1;
            fs_pool_merge_from_model(&g_model);
        } else if (g_geo.has_media && g_geo.cylinders > 0) {
            /* Blank disk (valid geometry, no RDB): start an empty in-memory table
               so New / Filesystems work immediately without a failed Split first.
               g_dirty stays 0 — nothing is written until the user Saves. */
            rdb_init_model(&g_model, g_geo.cylinders, g_geo.heads, g_geo.sectors);
            g_have_model = 1;
            g_no_rdb = 1;
        }
        dev_close(h);
    }
    gui_refresh_parts(); gui_update_buttons();
}

/* The user clicked the Driver cycle. A cycle gadget has already moved on by the
   time we hear about it, so declining has to put it back. Same index = a cycle
   with a single entry, where the click changes nothing. */
static void gui_driver_picked(int idx)
{
    if (idx == g_drv_idx) return;
    if (gui_discard_ok("Driver")) { gui_select_driver(idx); return; }
    GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0, GTCY_Active, (ULONG)g_drv_idx, TAG_END);
}

/* The user clicked a row of the Units list. */
static void gui_unit_picked(int uidx)
{
    if (uidx == g_cur_unitidx) return;
    if (gui_discard_ok("Unit")) gui_select_unit(uidx);
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

/* Scan button: probe the target driver's units, fill the Units list with the
   disks found, and auto-select the first (showing its partitions). */
static void gui_scan_selected(void)
{
    int real;
    if (!g_target_driver[0]) return;
    if (!gui_discard_ok("Scan")) return;

    gui_status("Scanning...");
    discover_probe_driver(g_disks, &g_ndisks, DISC_MAX, g_target_driver);
    real = gui_build_units("(no disk found)");
    gui_set_units();

    if (real > 0) {
        gui_select_unit(0);
    } else {
        g_have_model = 0; g_sel_part = -1; g_geo.has_media = 0; g_cur_driver[0] = 0;
        g_cur_unitidx = -1;
        gui_refresh_parts(); gui_update_buttons();
        gui_status("No disk found.");
    }
}

/* No-probe startup: build the Driver cycle, select the prompt (which clears the
   Units list and shows guidance). Touches no hardware. */
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
            "HDPart 0.12\n"
            "RDB hard-disk partition tool\n"
            "for AmigaOS 2.04+ (KS V37+).\n"
            "\n"
            "Orchestrated by Stefan Skotte,\n"
            "who directed every byte while\n"
            "an AI did the typing. Any bugs\n"
            "are, naturally, the AI's fault.\n"
            "\n"
            "Based on rdbtool (amitools) by\n"
            "Christian Vogelgsang, \xA9 2020-23.\n"
            "github.com/cnvogelg/amitools");
}

/* ----- Filesystems dialog ------------------------------------------------ */

/* Static storage for the FS listview (mirrors g_partlist / g_partnodes pattern). */
static struct List g_fslist;
static struct Node g_fsnodes[RDB_MAX_FS];
/* 80 bytes: mark(2)+dostype(11)+ver(8)+name(20)+src(9)+size(7)+NUL = ~57 worst case. */
static char        g_fsrows[RDB_MAX_FS][80];

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

/* Format version field as "vMAJOR.MINOR" (e.g. 0x00130002 -> "v19.2").
   If version == 0, emits "v?".  Returns length written. */
static int fs_ver_str(char *o, uint32_t version)
{
    int p = 0;
    o[p++] = 'v';
    if (version == 0) {
        o[p++] = '?';
    } else {
        p += u2s(o + p, version >> 16);
        o[p++] = '.';
        p += u2s(o + p, version & 0xFFFFu);
    }
    return p;
}

/* Build one FS list row.
   Fields: "> DosType  Version  Name            src   NNK"
   col 0=mark, col 2=dostype(11), col 13=version(8), col 21=name(20), col 42=src(9), col 51=size. */
static void fs_build_row(char *row, int idx, int sel)
{
    RdbFileSys *fs = &g_fs_pool[idx];
    int p = 0;
    char dtbuf[12];
    dostype_label(dtbuf, fs->dos_type);
    row[p++] = (idx == sel) ? '>' : ' ';
    row[p++] = ' ';
    s_cat(row, &p, dtbuf); s_pad(row, &p, 13);
    p += fs_ver_str(row + p, fs->version); s_pad(row, &p, 21);
    { int k; for (k = 0; fs->name[k] && k < 19; k++) row[p++] = fs->name[k]; }
    s_pad(row, &p, 42);
    switch (fs->source) {
        case RDB_FS_FILE:     s_cat(row, &p, "file");     break;
        case RDB_FS_COPIED:   s_cat(row, &p, "copied");   break;
        default:              s_cat(row, &p, "embedded"); break;
    }
    s_pad(row, &p, 51);
    { uint32_t kb = (fs->seg_len + 1023u) / 1024u; p += u2s(row + p, kb); s_cat(row, &p, "K"); }
    row[p] = 0;
}

/* Rebuild g_fslist and g_fsnodes from g_fs_pool[0..g_fs_npool). */
static void fs_rebuild_list(int sel, struct Window *dw, struct Gadget *glv)
{
    int i;
    NewList(&g_fslist);
    for (i = 0; i < g_fs_npool && i < RDB_MAX_FS; i++) {
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
    if (g_fs_npool > 0) {
        sel = 0;
        u32_to_hex(dtBuf, g_fs_pool[0].dos_type);
        presetIdx = fs_preset_index(g_fs_pool[0].dos_type);
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

    /* Listview — RDB_MAX_FS rows (height 8 rows = 8*10 = 80px).
       Width 540 fills the 560px inner area (10px margins each side). */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 6; ng.ng_Width = 540; ng.ng_Height = 80;
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
    ng.ng_LeftEdge = dl + 110; ng.ng_TopEdge = dt + 114; ng.ng_Width = 180; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Preset"; ng.ng_GadgetID = 3;
    g = CreateGadget(CYCLE_KIND, g, &ng,
                     GTCY_Labels, (ULONG)kFsPresetLabels,
                     GTCY_Active, (ULONG)presetIdx,
                     GA_Disabled, (ULONG)(sel < 0), TAG_END);
    gPreset = g;

    /* Buttons: laid out left-to-right with 8px gaps, sized to fit their labels.
       Topaz-8: 8px/char + ~16px border padding.
       "Add from file..."  16 chars -> 144+16 = 160 -> use 152 (topaz chars are narrow in cycle)
       "Copy from disk..." 17 chars -> 152+16 = 168 -> use 160
       "Remove"             6 chars ->  48+16 =  64 -> use  80 (min width)
       "Done"               4 chars ->  32+16 =  48 -> use  96 (fills remaining space to 550)
       Layout: x=10 w=152 | x=170 w=160 | x=338 w=80 | x=426 w=124 => last btn ends at 550, 10px right margin = 560 inner. */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 138; ng.ng_Width = 152; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Add from file..."; ng.ng_GadgetID = 4;
    /* Always enabled: gui_pick_file falls back to a typed-path dialog when
       asl.library is absent (e.g. booted from the bare HDPart floppy). */
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    /* Copy from disk button — copies the first embedded FS (which=0) from another
       disk's RDB into this model.  Multi-FS source (which>0) is out of scope. */
    ng.ng_LeftEdge = dl + 170; ng.ng_TopEdge = dt + 138; ng.ng_Width = 160; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Copy from disk..."; ng.ng_GadgetID = 7;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    /* Remove button */
    ng.ng_LeftEdge = dl + 338; ng.ng_TopEdge = dt + 138; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Remove"; ng.ng_GadgetID = 5;
    g = CreateGadget(BUTTON_KIND, g, &ng,
                     GA_Disabled, (ULONG)(sel < 0), TAG_END);
    gRemove = g;

    /* Done button — stretches to fill the remaining space to the right margin. */
    ng.ng_LeftEdge = dl + 426; ng.ng_TopEdge = dt + 138; ng.ng_Width = 124; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Done"; ng.ng_GadgetID = 6;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return; }

    {   int dwW = dl + 560 + g_scr->WBorRight;  /* 560px inner: listview 540 + 10px margins */
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
                    if (newsel >= 0 && newsel < g_fs_npool) {
                        sel = newsel;
                        /* Rebuild labels to move the '>' marker (V37-safe). */
                        fs_rebuild_list(sel, dw, gLV);
                        /* Update DosType field + preset cycle. */
                        u32_to_hex(dtBuf, g_fs_pool[sel].dos_type);
                        presetIdx = fs_preset_index(g_fs_pool[sel].dos_type);
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
                    if (sel >= 0 && sel < g_fs_npool) {
                        const char *s = (const char *)
                            ((struct StringInfo *)gDT->SpecialInfo)->Buffer;
                        uint32_t v;
                        if (parse_hex32(s, &v)) {
                            g_fs_pool[sel].dos_type = v;
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
                    if (sel >= 0 && sel < g_fs_npool) {
                        uint32_t v = (presetIdx < N_FS_PRESETS)
                                     ? kFsPresetValues[presetIdx]
                                     : g_fs_pool[sel].dos_type;
                        if (presetIdx < N_FS_PRESETS) {
                            g_fs_pool[sel].dos_type = v;
                            g_dirty = 1;
                            u32_to_hex(dtBuf, v);
                            GT_SetGadgetAttrs(gDT, dw, 0,
                                              GTST_String, (ULONG)dtBuf, TAG_END);
                            fs_rebuild_list(sel, dw, gLV);
                        }
                    }

                } else if (gid == 4) {
                    /* Add from file (ASL requester, or a typed path when asl.library
                       is absent — e.g. booting the bare HDPart floppy). */
                    static char fspath[256];
                    int rc;

                    if (g_fs_npool >= RDB_MAX_FS) {
                        gui_msg("Filesystems", fsl_err_text(FSL_EFULL));
                        break;
                    }
                    if (!gui_pick_file("Select a filesystem handler", "L:",
                                       fspath, sizeof(fspath)))
                        break;                                   /* cancelled */

                    /* fsload allocates seg_data; pool takes ownership directly. */
                    rc = fsload_from_file(fspath, &g_fs_pool[g_fs_npool]);
                    if (rc != FSL_OK) {
                        gui_msg("Filesystems", fsl_err_text(rc));
                    } else {
                        sel = g_fs_npool;
                        g_fs_npool++;
                        g_dirty = 1;
                        u32_to_hex(dtBuf, g_fs_pool[sel].dos_type);
                        presetIdx = fs_preset_index(g_fs_pool[sel].dos_type);
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

                    if (g_fs_npool >= RDB_MAX_FS) {
                        gui_msg("Filesystems", fsl_err_text(FSL_EFULL));
                        break;
                    }
                    if (!gui_copy_from_disk_picker(srcDrv, &srcUnit)) break; /* cancelled */

                    /* fsload allocates seg_data; pool takes ownership directly. */
                    rc = fsload_from_disk(srcDrv, srcUnit, 0, &g_fs_pool[g_fs_npool]);
                    if (rc != FSL_OK) {
                        gui_msg("Filesystems", fsl_err_text(rc));
                    } else {
                        sel = g_fs_npool;
                        g_fs_npool++;
                        g_dirty = 1;
                        u32_to_hex(dtBuf, g_fs_pool[sel].dos_type);
                        presetIdx = fs_preset_index(g_fs_pool[sel].dos_type);
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
                    /* Remove from pool. */
                    if (sel < 0 || sel >= g_fs_npool) break;

                    /* If any partition uses this dos_type, confirm first. */
                    { int uses = 0, kk;
                      uint32_t fdt = g_fs_pool[sel].dos_type;
                      for (kk = 0; kk < g_model.num_parts; kk++)
                          if (g_model.parts[kk].dos_type == fdt) { uses = 1; break; }
                      if (uses) {
                          if (!gui_confirm("Filesystems",
                                  "A partition uses this filesystem type.\n"
                                  "Remove it anyway?"))
                              break;
                      }
                    }

                    fs_pool_remove(sel);
                    g_dirty = 1;

                    /* Adjust selection. */
                    if (g_fs_npool == 0) {
                        sel = -1;
                        GT_SetGadgetAttrs(gDT,     dw, 0, GA_Disabled, TRUE, TAG_END);
                        GT_SetGadgetAttrs(gPreset, dw, 0, GA_Disabled, TRUE, TAG_END);
                        GT_SetGadgetAttrs(gRemove, dw, 0, GA_Disabled, TRUE, TAG_END);
                    } else {
                        if (sel >= g_fs_npool) sel = g_fs_npool - 1;
                        u32_to_hex(dtBuf, g_fs_pool[sel].dos_type);
                        presetIdx = fs_preset_index(g_fs_pool[sel].dos_type);
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
    static char path[256];
    static char name[DRV_NAME_LEN];
    int rc, i, n, sel = -1;

    if (!gui_discard_ok("Load Driver")) return;
    if (!gui_pick_file("Select a device driver", "DEVS:", path, sizeof(path)))
        return;                                   /* cancelled */

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

/* ----- Hand-drawn parts of the main window --------------------------------- */

static void gui_load_pens(void)
{
    g_pen_bg = 0; g_pen_text = 1; g_pen_shine = 2; g_pen_fill = 3; g_pen_seltext = 1;
    g_dri = GetScreenDrawInfo(g_scr);
    if (g_dri && g_dri->dri_Pens) {
        g_pen_bg    = g_dri->dri_Pens[BACKGROUNDPEN];
        g_pen_text  = g_dri->dri_Pens[TEXTPEN];
        g_pen_shine = g_dri->dri_Pens[SHINEPEN];
        g_pen_fill  = g_dri->dri_Pens[FILLPEN];
        g_pen_seltext = g_dri->dri_Pens[FILLTEXTPEN];
    }
    /* White on blue is the design on a stock screen; a customised palette keeps
       the system's own selected-text pen if white would not read. */
    if (pens_readable(g_scr, g_pen_shine, g_pen_fill)) g_pen_seltext = g_pen_shine;
}

/* The wizard draws the disk map into its own window: while set, the drawing helpers use it. */
static struct RastPort *g_map_rp;
#define MAP_RP() (g_map_rp ? g_map_rp : g_win->RPort)

static void gui_text(int x, int y, const char *s, int pen)
{
    struct RastPort *rp = MAP_RP();
    int n = 0;
    while (s[n]) n++;
    SetAPen(rp, pen);
    SetDrMd(rp, JAM1);
    Move(rp, x, y + 6);
    Text(rp, (CONST_STRPTR)s, (LONG)n);
}

static void gui_box(int x, int y, int w, int h, int pen)
{
    SetAPen(MAP_RP(), pen);
    RectFill(MAP_RP(), x, y, x + w - 1, y + h - 1);
}

/* Fill patterns distinguish filesystem families in only 4 colours. Line patterns,
   not area patterns, so nothing has to live in chip memory. */
enum { FILL_DOS, FILL_PFS, FILL_SFS, FILL_OTHER, FILL_FREE };

static void gui_pattern(int x, int y, int w, int h, int kind)
{
    struct RastPort *rp = MAP_RP();
    int r;
    if (w <= 0 || h <= 0) return;
    if (kind == FILL_DOS) { gui_box(x, y, w, h, g_pen_fill); return; }
    SetDrMd(rp, JAM2);
    SetAPen(rp, (kind == FILL_PFS || kind == FILL_SFS) ? g_pen_shine : g_pen_text);
    SetBPen(rp, (kind == FILL_PFS) ? g_pen_fill : g_pen_bg);
    for (r = 0; r < h; r++) {
        UWORD p;
        switch (kind) {
            case FILL_PFS:   p = (r & 1) ? 0xAAAA : 0x5555; break;
            case FILL_SFS:   p = 0x3333; break;
            case FILL_OTHER: p = (r & 3) ? 0x0000 : 0xFFFF; break;
            default:         p = (r & 1) ? 0x0000 : 0x8888; break;
        }
        rp->LinePtrn = p; rp->Flags |= FRST_DOT; rp->linpatcnt = 15;
        Move(rp, x, y + r);
        Draw(rp, x + w - 1, y + r);
    }
    rp->LinePtrn = 0xFFFF; rp->Flags |= FRST_DOT; rp->linpatcnt = 15;
    SetDrMd(rp, JAM1);
    SetBPen(rp, g_pen_bg);
}

static int fs_fill_kind(uint32_t t)
{
    switch (t & 0xFFFFFF00u) {
        case 0x444F5300u: return FILL_DOS;
        case 0x50465300u:
        case 0x50445300u: return FILL_PFS;
        case 0x53465300u: return FILL_SFS;
        default:          return FILL_OTHER;
    }
}

/* Short "name, size" label on a map slice, when the slice is wide enough. */
static void gui_slice_label(int x, int y, int w, const char *top, const char *bot)
{
    int a = 0, b = 0, lw;
    while (top[a]) a++;
    while (bot[b]) b++;
    lw = ((a > b) ? a : b) * 8;
    if (lw + 10 > w) return;
    gui_box(x + 3, y + 4, lw + 4, 24, g_pen_shine);
    gui_text(x + 5, y + 5, top, g_pen_text);
    gui_text(x + 5, y + 16, bot, g_pen_text);
}

/* Return the partition index whose map slice contains (mx,my), or -1. */
static int gui_part_at_x(int mx, int my)
{
    int i;
    if (!g_have_model || g_model.cylinders == 0) return -1;
    if (mx < g_bar.x || mx >= g_bar.x + g_bar.w || my < g_bar.y || my >= g_bar.y + g_bar.h) return -1;
    for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
        RdbPartition *pt = &g_model.parts[i];
        int x0 = g_bar.x + (int)((pt->low_cyl      * (ULONG)g_bar.w) / g_model.cylinders);
        int x1 = g_bar.x + (int)(((pt->high_cyl+1) * (ULONG)g_bar.w) / g_model.cylinders);
        if (x1 <= x0) x1 = x0 + 1;
        if (mx >= x0 && mx < x1) return i;
    }
    return -1;
}

/* Select partition `idx` (or -1 = none): updates the map, table and details. */
static void gui_set_selection(int idx)
{
    g_sel_part = idx;
    gui_refresh_parts();
    gui_update_buttons();
}

/* Row under a window-relative point, or -1. Rows past the end are not rows. */
static int gui_list_hit(const ListGeom *l, int mx, int my)
{
    int row;
    if (mx < l->x || mx >= l->x + l->w || my < l->y || my >= l->y + l->vis * l->rowh) return -1;
    row = l->top + (my - l->y) / l->rowh;
    return (row < l->count) ? row : -1;
}

static void gui_list_draw(const ListGeom *l)
{
    if (l == &g_plist) gui_draw_table(); else gui_draw_units();
}

/* The list's contents or position changed: resync its scroller and redraw. */
static void gui_list_changed(ListGeom *l, int gid)
{
    if (l->top + l->vis > l->count) l->top = (WORD)((l->count > l->vis) ? l->count - l->vis : 0);
    if (g_win && g_gad[gid])
        GT_SetGadgetAttrs(g_gad[gid], g_win, 0, GTSC_Top, (ULONG)l->top,
                          GTSC_Total, (ULONG)l->count, GTSC_Visible, (ULONG)l->vis, TAG_END);
    gui_list_draw(l);
}

/* The user moved a scroller. It already shows the new position. */
static void gui_list_scrolled(ListGeom *l, int top)
{
    if (top == l->top) return;
    l->top = (WORD)top;
    gui_list_draw(l);
}

static void gui_draw_table(void)
{
    int i;
    if (!g_win) return;
    gui_box(g_plist.x, g_plist.y, g_plist.w, g_plist.h, g_pen_bg);
    for (i = g_plist.top; i < g_plist.count && i < g_plist.top + g_plist.vis; i++) {
        int y = g_plist.y + (i - g_plist.top) * g_plist.rowh, sel = (i == g_sel_part);
        if (sel) gui_box(g_plist.x, y, g_plist.w, g_plist.rowh, g_pen_fill);
        gui_text(g_plist.x + 4, y + 1, g_partrows[i], sel ? g_pen_seltext : g_pen_text);
    }
}

/* Partitioned share of a listed unit. The shown unit's comes from the live model,
   so the meter follows edits; the others from the last Scan. */
static int gui_unit_percent(int row)
{
    if (row == g_cur_unitidx && g_have_model) {
        uint32_t span = (g_model.hi_cyl >= g_model.lo_cyl) ? g_model.hi_cyl - g_model.lo_cyl + 1u : 0u, used = 0;
        int i;
        for (i = 0; i < g_model.num_parts; i++)
            used += g_model.parts[i].high_cyl - g_model.parts[i].low_cyl + 1u;
        if (!span) return 0;
        return (int)((span > 40000000u) ? used / (span / 100u) : (used * 100u) / span);
    }
    return g_disks[g_unitmap[row]].used_pct;
}

static void gui_draw_units(void)
{
    int i;
    if (!g_win) return;
    gui_box(g_ulist.x, g_ulist.y, g_ulist.w, g_ulist.h, g_pen_bg);
    if (g_nunits == 0) {
        gui_text(g_ulist.x + 4, g_ulist.y + 4, g_unit_empty, g_pen_text);
        return;
    }
    for (i = g_ulist.top; i < g_nunits && i < g_ulist.top + g_ulist.vis; i++) {
        int y = g_ulist.y + (i - g_ulist.top) * g_ulist.rowh, sel = (i == g_cur_unitidx);
        int pct = gui_unit_percent(i), mx = g_ulist.x + 4, mw = g_ulist.w - 8;
        if (sel) gui_box(g_ulist.x, y, g_ulist.w, g_ulist.rowh, g_pen_fill);
        gui_text(mx, y + 2, g_unittext[i], sel ? g_pen_seltext : g_pen_text);
        DrawBevelBox(g_win->RPort, mx, y + 12, mw, 7, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
        gui_box(mx + 2, y + 13, mw - 4, 5, g_pen_shine);
        if (pct > 0) gui_box(mx + 2, y + 13, ((mw - 4) * pct) / 100, 5, sel ? g_pen_text : g_pen_fill);
    }
}

static void gui_detail_row(int y, const char *label, const char *value)
{
    gui_text(RIGHT_X + 6 + g_leftb, y, label, g_pen_text);
    gui_text(RIGHT_X + 110 + g_leftb, y, value, g_pen_text);
}

static void gui_draw_details(void)
{
    static char buf[48];
    int x = RIGHT_X + 3 + g_leftb, y = DETAIL_Y + g_topb, w = RIGHT_W - 6, p;
    if (!g_win) return;
    if (!g_qs_panel_gone) {
        /* No clearing here: the button sits in this box and would need redrawing. */
        static const char q1[] = "First time setting up a CF or SD card?";
        static const char q2[] = "Or pick a driver above to do it yourself.";
        gui_text(x + (w - (int)(sizeof(q1) - 1) * 8) / 2, y + 6, q1, g_pen_text);
        gui_text(x + (w - (int)(sizeof(q2) - 1) * 8) / 2, y + 44, q2, g_pen_text);
        return;
    }
    gui_box(x, y + 2, w, DETAIL_H - 4, g_pen_bg);
    if (!g_have_model || g_sel_part < 0 || g_sel_part >= g_model.num_parts) {
        gui_text(x + 3, y + 4, "No partition selected", g_pen_text);
        return;
    }
    {
        const RdbPartition *pt = &g_model.parts[g_sel_part];
        int i;
        gui_text(x + 3, y + 4, pt->name, g_pen_text);
        gui_box(x + 2, y + 14, w - 4, 1, g_pen_text);

        p = 0; { char tb[16]; fstype_label(tb, pt->dos_type); s_cat(buf, &p, tb); }
        for (i = 0; i < g_fs_npool; i++)
            if (g_fs_pool[i].dos_type == pt->dos_type) {
                s_cat(buf, &p, " ");
                p += fs_ver_str(buf + p, g_fs_pool[i].version);
                s_cat(buf, &p, " (embedded)");
                break;
            }
        buf[p] = 0;
        gui_detail_row(y + 17, "Filesystem", buf);

        p = 0; p += u2s(buf + p, pt->low_cyl); s_cat(buf, &p, " to "); p += u2s(buf + p, pt->high_cyl);
        s_cat(buf, &p, pt->bootable ? ", bootable" : ", not bootable");
        buf[p] = 0;
        gui_detail_row(y + 28, "Cylinders", buf);

        { char live[8];
          if (safety_partition_mounted(g_cur_driver, g_cur_unit, pt->low_cyl, pt->high_cyl, live)) {
              p = 0; s_cat(buf, &p, live); s_cat(buf, &p, ":"); buf[p] = 0;
              gui_detail_row(y + 39, "Mounted", buf);
          } else {
              gui_detail_row(y + 39, "Mounted", "No");
          } }

        p = u32_to_hex(buf, pt->maxtransfer);
        /* See docs/reference/rom-findings-maxtransfer-mask-buffers.md. scsi.device below
           V45 (OS 3.1.4) trusts the drive for the next address past 256 sectors. PFS3 caps itself. */
        if (pt->maxtransfer > 0x1FE00u && streq(g_cur_driver, "scsi.device") &&
            (pt->dos_type & 0xFFFFFF00u) != 0x50465300u) {
            struct Library *dv;
            Forbid();
            dv = (struct Library *)FindName(&SysBase->DeviceList, (CONST_STRPTR)"scsi.device");
            if (dv && dv->lib_Version < 45) s_cat(buf, &p, "  ! too high for IDE");
            Permit();
        }
        buf[p] = 0;
        gui_detail_row(y + 50, "MaxTransfer", buf);
    }
}

/* Static parts of the window: labels and frames. Redrawn on IDCMP_REFRESHWINDOW. */
static void gui_draw_chrome(void)
{
    struct RastPort *rp;
    if (!g_win) return;
    rp = g_win->RPort;
    gui_text(LEFT_X + g_leftb, 1 + g_topb, "Driver", g_pen_text);
    gui_text(LEFT_X + g_leftb, 42 + g_topb, "Units", g_pen_text);
    DrawBevelBox(rp, LEFT_X + g_leftb, UNITS_Y + g_topb, LEFT_W, UNITS_H, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    DrawBevelBox(rp, RIGHT_X + g_leftb, MAP_Y + g_topb, RIGHT_W, MAP_H, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    DrawBevelBox(rp, RIGHT_X + g_leftb, TABLE_Y + g_topb, RIGHT_W, TABLE_H, GT_VisualInfo, (ULONG)g_vi, GTBB_Recessed, TRUE, TAG_END);
    DrawBevelBox(rp, RIGHT_X + g_leftb, DETAIL_Y + g_topb, RIGHT_W, DETAIL_H, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    gui_text(RIGHT_X + 6 + g_leftb, TABLE_Y + 3 + g_topb, "Name        Type        Start   End     Size", g_pen_text);
    gui_box(RIGHT_X + 3 + g_leftb, TABLE_Y + 13 + g_topb, RIGHT_W - SCROLL_W - 7, 1, g_pen_text);
    gui_draw_table();
    gui_draw_units();
    gui_draw_details();
}

/* The disk map for model m in the rectangle x,y,w,h on rp: free space first, then each
   partition to scale. The main window and the Quick setup screens share it. rdb_label says
   what the blank block at the start is called; the wizard leaves it unlabelled. vols, when
   given, is a Quick setup mask: slices carry its volume names (System, Work...) not DH0... */
static void gui_map_draw(struct RastPort *rp, const RdbModel *m, int x, int y, int w, int h, int sel, int rdb_label,
                         const uint32_t *vols)
{
    static char lbl[12];
    int i, rw;
    g_map_rp = rp;
    rw = (int)((m->lo_cyl * (ULONG)w) / m->cylinders);
    gui_pattern(x + rw, y, w - rw, h, FILL_FREE);
    if (rdb_label && rw >= 26)   /* Cylinders below lo_cyl hold the RDB: say so, or it reads as a drawing fault. */
        gui_text(x + (rw - 24) / 2, y + (h - 8) / 2, "RDB", g_pen_text);
    for (i = 0; i < m->num_parts && i < RDB_MAX_PARTS; i++) {
        const RdbPartition *pt = &m->parts[i];
        int x0 = x + (int)((pt->low_cyl  * (ULONG)w) / m->cylinders);
        int x1 = x + (int)(((pt->high_cyl + 1) * (ULONG)w) / m->cylinders);
        int p = 0;
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > x + w) x1 = x + w;
        gui_pattern(x0, y, x1 - x0, h, fs_fill_kind(pt->dos_type));
        p += u2s(lbl + p, disc_blocks_to_mb((pt->high_cyl - pt->low_cyl + 1) * m->cyl_blocks, m->block_bytes));
        s_cat(lbl, &p, " MB"); lbl[p] = 0;
        gui_slice_label(x0, y, x1 - x0, (vols && quick_volume_name(*vols, i)) ? quick_volume_name(*vols, i) : pt->name, lbl);
        if (x1 - x0 > 1) {            /* a 1-pixel gap keeps neighbours apart */
            gui_box(x1 - 1, y, 1, h, g_pen_bg);
        }
        if (i == sel) {
            gui_box(x0, y, x1 - x0, 2, g_pen_text);
            gui_box(x0, y + h - 2, x1 - x0, 2, g_pen_text);
            gui_box(x0, y, 2, h, g_pen_text);
            gui_box(x1 - 2, y, 2, h, g_pen_text);
        }
    }
    g_map_rp = 0;
}

void gui_draw_bar(void)
{
    if (!g_win) return;
    gui_box(g_bar.x, g_bar.y, g_bar.w, g_bar.h, g_pen_bg);
    gui_draw_easter();
    if (!g_have_model || g_model.cylinders == 0) return;
    gui_map_draw(g_win->RPort, &g_model, g_bar.x, g_bar.y, g_bar.w, g_bar.h, g_sel_part, 1, 0);
}

/* A tiny hand-drawn pi glyph in the bottom-right corner (no Topaz glyph for it).
   Clicking it is an easter egg. */
static void gui_draw_easter(void)
{
    struct RastPort *rp;
    int px = WIN_W - 6 + g_leftb, py = WIN_H - 7 + g_topb;
    if (!g_win) return;
    rp = g_win->RPort;
    SetAPen(rp, g_pen_text);
    Move(rp, px,     py);     Draw(rp, px + 5, py);       /* top bar (half height) */
    Move(rp, px + 1, py);     Draw(rp, px + 1, py + 4);   /* left leg */
    Move(rp, px + 4, py);     Draw(rp, px + 4, py + 4);   /* right leg */
}

/* True if (mx,my) is in the bottom-right corner where the pi glyph lives. */
static int gui_hit_easter(int mx, int my)
{
    return mx >= WIN_W - 7 + g_leftb && my >= WIN_H - 8 + g_topb;
}
