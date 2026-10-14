/* bad_import.c - negative fixture: imports a name the host does not export.
 * The loader must refuse it with STATUS_PROCEDURE_NOT_FOUND and name the import.
 */
#include "drvhost_types.h"
#include "drvhost_ntos.h"

extern void NoSuchFunctionXyz(void);

DRVCALL NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    (void)DriverObject;
    (void)RegistryPath;
    NoSuchFunctionXyz();
    return STATUS_SUCCESS;
}
