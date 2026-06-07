/* HDPart startup: freestanding entry point for an AmigaOS application.
 * No crt0/libc. Sets SysBase, opens dos.library, detects CLI vs Workbench
 * launch, calls hdpart_main(), then cleans up and returns a DOS rc. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct ExecBase  *SysBase = 0;
struct DosLibrary *DOSBase = 0;

/* Minimal memcpy: required because the SDK's OpenWindowTags (and similar
 * varargs macros) use a VLA initialised from .rodata, causing GCC to emit
 * a compiler-generated memcpy call even in -nostdlib builds.
 * No libgcc.a is available in this stripped toolchain. */
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
    char       *d = (char *)dst;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* Provided by main.c */
extern int hdpart_main(struct WBStartup *wbmsg);

int _start(void)
{
    struct Process  *proc;
    struct WBStartup *wbmsg = 0;
    int rc = RETURN_FAIL;

    /* exec library base lives at absolute address 4 */
    SysBase = *(struct ExecBase **)4UL;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    if (!DOSBase)
        return RETURN_FAIL; /* nothing else we can do */

    proc = (struct Process *)FindTask(0);

    if (proc->pr_CLI == 0) {
        /* Started from Workbench: a WBStartup message is waiting at our
         * process port. Receive it now; reply only at exit. */
        WaitPort(&proc->pr_MsgPort);
        wbmsg = (struct WBStartup *)GetMsg(&proc->pr_MsgPort);
    }

    rc = hdpart_main(wbmsg);

    if (wbmsg) {
        /* Must Forbid() before replying so we are not unloaded mid-reply. */
        Forbid();
        ReplyMsg((struct Message *)wbmsg);
    }

    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
