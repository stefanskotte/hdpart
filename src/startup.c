/* HDPart startup: freestanding entry point for an AmigaOS application.
 * No crt0/libc. Sets SysBase, opens dos.library, detects CLI vs Workbench
 * launch, calls hdpart_main(), then cleans up and returns a DOS rc. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct ExecBase  *SysBase = 0;
struct DosLibrary *DOSBase = 0;

/* Provided by main.c */
extern int hdpart_main(struct WBStartup *wbmsg);

/* CRITICAL: elf2hunk does NOT honor the ELF entry symbol — AmigaDOS begins
 * executing at the first byte of the code hunk. We must therefore guarantee
 * _start is the lowest-addressed code. The default GNU linker script places
 * .text.startup ahead of .text/.text.*, so putting _start there makes it the
 * hunk's first instruction. Without this, execution falls into whatever
 * function the linker happened to place first (e.g. hdpart_main) with SysBase
 * uninitialised, and the first library call dereferences a NULL base -> Guru.
 * (Verified by the Makefile's post-link entry guard.) */
__attribute__((section(".text.startup")))
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

    /* Run on a generous private stack: the GadTools GUI + library calls + the
       ~1.8KB RdbModel would overflow the ~4KB default Shell stack.
       NOTE: `sss` MUST be static (not a stack local). After the first
       StackSwap() switches A7 to the new stack, the compiler would address a
       stack-local `&sss` relative to the NEW stack for the second call, passing
       a wrong pointer -> crash. A static lives at a fixed address, valid on
       either stack. */
    {
        #define HDPART_STACK_SIZE (64UL * 1024UL)
        APTR newstack = AllocMem(HDPART_STACK_SIZE, MEMF_CLEAR);
        if (newstack) {
            static struct StackSwapStruct sss;
            sss.stk_Lower   = newstack;
            sss.stk_Upper   = (ULONG)newstack + HDPART_STACK_SIZE;
            sss.stk_Pointer = (APTR)((ULONG)newstack + HDPART_STACK_SIZE);
            StackSwap(&sss);
            rc = hdpart_main(wbmsg);   /* runs on the new stack */
            StackSwap(&sss);           /* restore the original stack */
            FreeMem(newstack, HDPART_STACK_SIZE);
        } else {
            rc = hdpart_main(wbmsg);   /* fallback: original stack */
        }
        #undef HDPART_STACK_SIZE
    }

    if (wbmsg) {
        /* Must Forbid() before replying so we are not unloaded mid-reply. */
        Forbid();
        ReplyMsg((struct Message *)wbmsg);
    }

    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
