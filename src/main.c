/* HDPart main: Milestone-0 smoke test. Opens an Intuition window on the
 * Workbench screen and waits for the close gadget. Later plans replace the
 * body with the real GadTools UI wired to the RDB engine. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase = 0;

int hdpart_main(struct WBStartup *wbmsg)
{
    struct Window *win;
    (void)wbmsg;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase)
        return 20;

    win = OpenWindowTags(0,
        WA_Left,        80,
        WA_Top,         40,
        WA_Width,       360,
        WA_Height,      120,
        WA_Title,       (ULONG)"HDPart 0.1",
        WA_IDCMP,       IDCMP_CLOSEWINDOW,
        WA_Flags,       WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                        WFLG_ACTIVATE | WFLG_SMART_REFRESH,
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

    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
