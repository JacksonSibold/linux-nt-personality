/*
 * console_s5.c - S5 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * Imports one function from missing_s5.dll, which is not in the initramfs.
 * The strict loader (src/ntdll/ntdll_main.c, ldr_fail_unbound_import) must
 * fail the process before _start runs with STATUS_DLL_NOT_FOUND (0xC0000135).
 * The Linux exit status keeps only the low byte, so the shell sees 53 (0x35).
 * The full status and the DLL name are on the stderr line
 * "cannot bind import image -> missing_s5.dll: STATUS_DLL_NOT_FOUND".
 */

__declspec(dllimport) int s5_missing_fn(void);

void _start(void)
{
    s5_missing_fn();
}
