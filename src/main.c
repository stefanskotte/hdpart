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
#include "rdb.h"
#include "device.h"

struct IntuitionBase *IntuitionBase = 0;

int hdpart_main(struct WBStartup *wbmsg)
{
    struct Window *win;
    struct Screen *pubScr;       /* locked Workbench pubscreen, or NULL */
    struct Screen *ownScr = 0;   /* our custom screen, if we open one */
    RdbModel probe;
    DeviceHandle *probe_dev;
    rdb_init_model(&probe, 100, 4, 17);   /* link-smoke: pulls in rdb.o */
    probe_dev = dev_open("__nonexistent.device", 0); /* link-smoke: pulls in device.o */
    if (probe_dev) dev_close(probe_dev);
    (void)probe;
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
