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
#include "gui.h"

struct IntuitionBase *IntuitionBase = 0;

/* Minimal unsigned-decimal print to a DOS file handle (no libc). */
static void put_str(BPTR fh, const char *s)
{
    long n = 0; while (s[n]) n++;
    Write(fh, (CONST APTR)s, n);
}
static void put_uint(BPTR fh, ULONG v)
{
    char buf[12]; int i = 11;
    buf[i--] = 0;
    if (v == 0) buf[i--] = '0';
    while (v && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    put_str(fh, &buf[i + 1]);
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
    /* static: DiscDisk[32] is ~2.9KB; on the 4KB default Shell stack, keeping
       it (plus probe_one's RdbModel) on the stack overflows -> crash. */
    static DiscDisk disks[DISC_MAX];
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

    {
        int rc = gui_run();
        CloseLibrary((struct Library *)IntuitionBase);
        return rc;
    }
}
