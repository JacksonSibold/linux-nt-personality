/*
 * console_s11.c - S11 of the console milestone: the minimal console exe.
 *
 * The step (tests/kernel/s11/s11.step, run by the initramfs /init) starts this exe
 * 1500 times from a shell loop and requires every run to exit 0, and the NT handle
 * table to return to its baseline afterwards. The exe does nothing, so each run is
 * just an exec and an exit. binfmt_pe installs three standard handles per exec, and
 * process exit must free them (io.c ntio_release_owned_handles).
 *
 * Exit status 0 is the only result. The step prints the summary.
 */

__declspec(dllimport) void ExitProcess(unsigned int code);

void _start(void)
{
    ExitProcess(0);
    for (;;) {
    }
}
