/* HDPart main: Milestone-0 smoke test. Opens a window on the Workbench public
 * screen if one exists, otherwise opens its OWN custom screen first. The custom-
 * screen fallback matters for a partition tool: it is often run on a machine
 * that cannot boot Workbench yet (blank/fresh media, no system partition).
 * Later plans replace the body with the real GadTools UI wired to the RDB engine. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <exec/execbase.h>
#include "rdb.h"
#include "device.h"
#include "discover.h"

struct IntuitionBase *IntuitionBase = 0;

/* Minimal unsigned-decimal print to a DOS file handle (no libc). */
static void put_str(BPTR fh, const char *s)
{
    long n = 0; while (s[n]) n++;
    Write(fh, (CONST APTR)s, n);
}
static void put_uint(BPTR fh, ULONG v)
{
    /* No libgcc on this toolchain: avoid % and / by repeated subtraction. */
    static const ULONG pow10[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL,
        100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    char buf[12]; int n = 0, d, p;
    int started = 0;
    if (v == 0) { buf[n++] = '0'; buf[n] = 0; put_str(fh, buf); return; }
    for (p = 0; p < 10; p++) {
        d = 0;
        while (v >= pow10[p]) { v -= pow10[p]; d++; }
        if (d || started) { buf[n++] = (char)('0' + d); started = 1; }
    }
    buf[n] = 0;
    put_str(fh, buf);
}
static const char *status_name(DiskStatus s)
{
    switch (s) {
        case DST_NOMEDIA: return "no-media";
        case DST_BLANK:   return "blank";
        case DST_RDB:     return "RDB";
        case DST_MOUNTED: return "mounted";
        default:          return "unknown";
    }
}

/* Print discovered disks to the CLI. Returns process rc. */
static int run_scan(void)
{
    BPTR out = Output();
    DiscDisk disks[DISC_MAX];
    int count, i;

    put_str(out, "HDPart device scan\n------------------\n");
    count = discover_disks(disks, DISC_MAX);
    if (count == 0) { put_str(out, "(no block devices found)\n"); return 0; }

    for (i = 0; i < count; i++) {
        put_str(out, disks[i].driver);
        put_str(out, " unit ");
        put_uint(out, disks[i].unit);
        put_str(out, "  ");
        put_uint(out, disks[i].size_mb);
        put_str(out, " MB  [");
        put_str(out, status_name(disks[i].status));
        put_str(out, "]");
        if (disks[i].label[0]) { put_str(out, "  "); put_str(out, disks[i].label); }
        put_str(out, "\n");
    }
    return 0;
}

int hdpart_main(struct WBStartup *wbmsg)
{
    struct Window *win;
    struct Screen *pubScr;       /* locked Workbench pubscreen, or NULL */
    struct Screen *ownScr = 0;   /* our custom screen, if we open one */
    {
        /* CLI arg check: "scan" -> text report, else open the GUI window.
           GetArgStr returns the command line (args after the verb), or "" for WB. */
        const char *args = (const char *)GetArgStr();
        if (args && (args[0] == 's' || args[0] == 'S') &&
            args[1] == 'c' && args[2] == 'a' && args[3] == 'n')
            return run_scan();
    }
    (void)wbmsg;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase)
        return 20;

    /* Prefer the Workbench public screen; if none exists, open our own. */
    pubScr = LockPubScreen(0);
    if (!pubScr) {
        ownScr = OpenScreenTags(0,
            SA_Depth,   2,
            SA_Title,   (ULONG)"HDPart",
            SA_Type,    CUSTOMSCREEN,
            TAG_END);
        if (!ownScr) {
            CloseLibrary((struct Library *)IntuitionBase);
            return 20;
        }
    }

    win = OpenWindowTags(0,
        WA_Left,        80,
        WA_Top,         40,
        WA_Width,       360,
        WA_Height,      120,
        WA_Title,       (ULONG)"HDPart 0.1",
        WA_IDCMP,       IDCMP_CLOSEWINDOW,
        WA_Flags,       WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                        WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        pubScr ? WA_PubScreen   : WA_CustomScreen,
        (ULONG)(pubScr ? pubScr : ownScr),
        TAG_END);

    if (win) {
        BOOL done = FALSE;
        while (!done) {
            struct IntuiMessage *msg;
            WaitPort(win->UserPort);
            while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
                ULONG cls = msg->Class;
                ReplyMsg((struct Message *)msg);
                if (cls == IDCMP_CLOSEWINDOW)
                    done = TRUE;
            }
        }
        CloseWindow(win);
    }

    if (ownScr)
        CloseScreen(ownScr);
    if (pubScr)
        UnlockPubScreen(0, pubScr);

    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
