/* minimal.c - smallest real PE32+ executable: no CRT, custom entry point.
 *
 * Built by tests/host/Makefile with -nostdlib and -Wl,-e,minimal_entry.
 * The image is only read by test_real_pe.c, never executed.
 */
volatile int minimal_counter = 42;

void minimal_entry(void)
{
    for (;;)
        minimal_counter++;
}
