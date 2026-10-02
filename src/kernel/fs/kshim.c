/* kshim.c - non-inline definitions for the kernel API shim used in tests. */
#include "kshim.h"

struct kshim_mapping *kshim_mappings = NULL;
u64 kshim_preferred_base_ok = 1;
u64 kshim_forced_random_base = 0;
