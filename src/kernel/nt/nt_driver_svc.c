/* nt_driver_svc.c - the NtLoadDriver and NtUnloadDriver service entries.
 *
 * The driver host (driver/drvhost_svc.c) implements the work. It is linked
 * into the kernel build; a build without it gets the weak fallbacks below,
 * which return STATUS_NOT_IMPLEMENTED (the same pattern as weak.c).
 */
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

NTSTATUS __attribute__((weak)) dh_user_load_driver(void *RegistryPath)
{
	(void)RegistryPath;
	return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __attribute__((weak)) dh_user_unload_driver(void *RegistryPath)
{
	(void)RegistryPath;
	return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS nt_svc_NtLoadDriver(PUNICODE_STRING RegistryPath)
{
	return dh_user_load_driver(RegistryPath);
}

NTSTATUS nt_svc_NtUnloadDriver(PUNICODE_STRING RegistryPath)
{
	return dh_user_unload_driver(RegistryPath);
}
