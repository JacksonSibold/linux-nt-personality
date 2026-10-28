/*
 * Rtl last-error access for ntdll.dll
 *
 * The values live in the TEB: LastErrorValue, as in Wine's dlls/ntdll/error.c (LGPL-2.1-or-later). The NTSTATUS to Win32
 * error table is the repository's own (ntstatus_to_dos.c), so this file has only the accessors.
 */
#include <stdarg.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"

/***********************************************************************
 *      RtlGetLastWin32Error  (NTDLL.@)
 */
DWORD WINAPI RtlGetLastWin32Error(void)
{
    return NtCurrentTeb()->LastErrorValue;
}

/***********************************************************************
 *      RtlSetLastWin32Error  (NTDLL.@)
 */
void WINAPI RtlSetLastWin32Error( DWORD err )
{
    NtCurrentTeb()->LastErrorValue = err;
}

/***********************************************************************
 *      RtlSetLastWin32ErrorAndNtStatusFromNtStatus (NTDLL.@)
 */
void WINAPI RtlSetLastWin32ErrorAndNtStatusFromNtStatus( NTSTATUS status )
{
    NtCurrentTeb()->LastErrorValue = RtlNtStatusToDosError( status );
}
