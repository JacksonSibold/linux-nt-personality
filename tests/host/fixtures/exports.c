/* exports.c - DLL with named, ordinal-only and DATA exports; the export list
 * and forwarders come from fixtures/exports.def.
 *
 * Built by tests/host/Makefile with -shared -nostdlib. The image is only read
 * by test_real_pe.c, never loaded.
 */
int exports_counter = 7;

int alpha_fn(void) { return 1; }
int beta_fn(void)  { return 2; }
int hidden_fn(void) { return 10; }

int exports_entry(void *inst, unsigned reason, void *reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return 1;
}
