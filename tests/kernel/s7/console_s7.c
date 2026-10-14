/*
 * console_s7.c - S7 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * A thread made with CreateThread (kernel32, NtCreateThreadEx) reads its own
 * TEB through %gs and stores what it saw. The main thread checks it:
 *   gs:[0x30]  NtTib.Self        non-zero, and not the main thread's TEB
 *   gs:[0x60]  ProcessEnvironmentBlock   the same PEB as the main thread
 *   gs:[0x48]  ClientId.UniqueThread     the thread id that CreateThread returned
 *   gs:[0x08]  NtTib.StackBase / gs:[0x10] NtTib.StackLimit  bracket the stack
 *              pointer the thread runs on
 * A TEB that is not user memory faults at the first %gs read, so a clean exit
 * shows the TEB is mapped and populated. Exit status 0 means every check passed;
 * failures exit with 10 to 16.
 */

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned long long ULLONG;

__declspec(dllimport) HANDLE CreateThread(void *sa, unsigned long long stack, DWORD (*start)(void *),
                                          void *param, DWORD flags, DWORD *id);
__declspec(dllimport) DWORD WaitForSingleObject(HANDLE h, DWORD ms);
__declspec(dllimport) BOOL GetExitCodeThread(HANDLE h, DWORD *code);
__declspec(dllimport) BOOL CloseHandle(HANDLE h);
__declspec(dllimport) void ExitProcess(unsigned int code);

#define INFINITE_WAIT 0xFFFFFFFFu

/* What the worker saw through %gs, written by the worker before it returns. */
struct teb_view {
    ULLONG self;
    ULLONG peb;
    ULLONG thread_id;
    ULLONG stack_base;
    ULLONG stack_limit;
    ULLONG sp;
    volatile int done;
};

static struct teb_view worker_view;

static DWORD worker(void *arg)
{
    ULLONG self, peb, tid, base, limit, sp;
    (void)arg;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(self));
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    __asm__ volatile("movq %%gs:0x48, %0" : "=r"(tid));
    __asm__ volatile("movq %%gs:0x08, %0" : "=r"(base));
    __asm__ volatile("movq %%gs:0x10, %0" : "=r"(limit));
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    worker_view.self = self;
    worker_view.peb = peb;
    worker_view.thread_id = tid;
    worker_view.stack_base = base;
    worker_view.stack_limit = limit;
    worker_view.sp = sp;
    worker_view.done = 1;
    return 0;
}

void _start(void)
{
    ULLONG main_self, main_peb;
    DWORD tid = 0, code = 0xFFFFFFFFu;
    HANDLE h;

    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(main_self));
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(main_peb));

    h = CreateThread(0, 0, worker, 0, 0, &tid);
    if (!h)
        ExitProcess(10);
    if (WaitForSingleObject(h, INFINITE_WAIT) != 0)
        ExitProcess(11);
    if (!worker_view.done)
        ExitProcess(12);

    if (worker_view.self == 0 || worker_view.self == main_self)
        ExitProcess(13);
    if (worker_view.peb == 0 || worker_view.peb != main_peb)
        ExitProcess(14);
    if (worker_view.thread_id == 0 || worker_view.thread_id != (ULLONG)tid)
        ExitProcess(15);
    if (!(worker_view.stack_limit < worker_view.sp && worker_view.sp <= worker_view.stack_base))
        ExitProcess(16);

    if (!GetExitCodeThread(h, &code) || code != 0)
        ExitProcess(17);
    CloseHandle(h);
    ExitProcess(0);
    for (;;) {
    }
}
