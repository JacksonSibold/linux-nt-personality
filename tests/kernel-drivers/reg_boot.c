/* reg_boot.c - brings up the personality registry for the userspace driver-host
 * tests, the way the kernel boot does: object manager, registry, and a current
 * process and thread.  Kept in its own translation unit because the kernel-side
 * headers define NT names (UNICODE_STRING, OBJECT_ATTRIBUTES) that the driver
 * host's WDK-shaped headers also define.
 */
#include "registry.h"
#include "object.h"
#include "kshim.h"

void test_registry_boot(void)
{
    ntreg_cleanup();
    ObCleanupObjectManager();
    ObInitObjectManager();
    ntreg_init();
    struct nt_eprocess *proc = NULL;
    ObCreateProcessObject(&proc, 4243, "driver_host_test");
    kshim_set_current_process(proc);
    struct nt_ethread *thr = NULL;
    ObCreateThreadObject(&thr, proc, 4244);
    kshim_set_current_thread(thr);
}
