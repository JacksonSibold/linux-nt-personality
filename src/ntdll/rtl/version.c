/*
 * Windows and DOS version functions
 *
 * Copyright 1997 Marcus Meissner
 * Copyright 1998 Patrik Stridvall
 * Copyright 1998, 2003 Andreas Mohr
 * Copyright 1997, 2003 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "ntstatus.h"
#include "windef.h"
#include "wine/debug.h"
#include "ntdll_misc.h"
#include "../os_identity.h"
#include "ddk/wdm.h"

WINE_DEFAULT_DEBUG_CHANNEL(ver);

/* ntpers: Wine selects a Windows version from the registry or an application setting and keeps a table of versions from
 * Windows 2.0 to 11 for it. This personality reports one version, the identity of os_identity.h (decision 0075), so the
 * table, the registry readers, the Wine version exports and version_init are left out. */
static const RTL_OSVERSIONINFOEXW ntpers_version =
{
    sizeof(RTL_OSVERSIONINFOEXW), NT_OS_MAJOR_VERSION, NT_OS_MINOR_VERSION, NT_OS_BUILD_NUMBER, NT_OS_PLATFORM_ID,
    { 0 }, 0, 0, 0, NT_OS_PRODUCT_TYPE
};
static const RTL_OSVERSIONINFOEXW *current_version = &ntpers_version;

/***********************************************************************
 *           RtlGetProductInfo    (NTDLL.@)
 *
 * Gives info about the current Windows product type, in a format compatible
 * with the given Windows version
 *
 * Returns TRUE if the input is valid, FALSE otherwise
 */
BOOLEAN WINAPI RtlGetProductInfo(DWORD dwOSMajorVersion, DWORD dwOSMinorVersion, DWORD dwSpMajorVersion,
                                 DWORD dwSpMinorVersion, PDWORD pdwReturnedProductType)
{
    TRACE("(%ld, %ld, %ld, %ld, %p)\n", dwOSMajorVersion, dwOSMinorVersion,
          dwSpMajorVersion, dwSpMinorVersion, pdwReturnedProductType);

    if (!pdwReturnedProductType)
        return FALSE;

    if (dwOSMajorVersion < 6)
    {
        *pdwReturnedProductType = PRODUCT_UNDEFINED;
        return FALSE;
    }

    if (current_version->wProductType == VER_NT_WORKSTATION)
        *pdwReturnedProductType = PRODUCT_ULTIMATE_N;
    else
        *pdwReturnedProductType = PRODUCT_STANDARD_SERVER;

    return TRUE;
}

/* ntpers: RtlGetVersion is in ntdll_main.c, which reports the same identity. */

/******************************************************************************
 *  RtlGetNtVersionNumbers   (NTDLL.@)
 *
 * Get the version numbers of the run time library.
 *
 * PARAMS
 *  major [O] Destination for the Major version
 *  minor [O] Destination for the Minor version
 *  build [O] Destination for the Build version
 *
 * RETURNS
 *  Nothing.
 *
 * NOTES
 * Introduced in Windows XP (NT5.1)
 */
void WINAPI RtlGetNtVersionNumbers( LPDWORD major, LPDWORD minor, LPDWORD build )
{
    if (major) *major = current_version->dwMajorVersion;
    if (minor) *minor = current_version->dwMinorVersion;
    /* FIXME: Does anybody know the real formula? */
    if (build) *build = (0xF0000000 | current_version->dwBuildNumber);
}


/******************************************************************************
 *  RtlGetNtProductType   (NTDLL.@)
 */
BOOLEAN WINAPI RtlGetNtProductType( LPDWORD type )
{
    if (type) *type = current_version->wProductType;
    return TRUE;
}

static inline UCHAR version_update_condition(UCHAR *last_condition, UCHAR condition)
{
    switch (*last_condition)
    {
        case 0:
            *last_condition = condition;
            break;
        case VER_EQUAL:
            if (condition >= VER_EQUAL && condition <= VER_LESS_EQUAL)
            {
                *last_condition = condition;
                return condition;
            }
            break;
        case VER_GREATER:
        case VER_GREATER_EQUAL:
            if (condition >= VER_EQUAL && condition <= VER_GREATER_EQUAL)
                return condition;
            break;
        case VER_LESS:
        case VER_LESS_EQUAL:
            if (condition == VER_EQUAL || (condition >= VER_LESS && condition <= VER_LESS_EQUAL))
                return condition;
            break;
    }
    if (!condition) *last_condition |= 0x10;
    return *last_condition & 0xf;
}

static inline NTSTATUS version_compare_values(ULONG left, ULONG right, UCHAR condition)
{
    switch (condition) {
        case VER_EQUAL:
            if (left != right) return STATUS_REVISION_MISMATCH;
            break;
        case VER_GREATER:
            if (left <= right) return STATUS_REVISION_MISMATCH;
            break;
        case VER_GREATER_EQUAL:
            if (left < right) return STATUS_REVISION_MISMATCH;
            break;
        case VER_LESS:
            if (left >= right) return STATUS_REVISION_MISMATCH;
            break;
        case VER_LESS_EQUAL:
            if (left > right) return STATUS_REVISION_MISMATCH;
            break;
        default:
            return STATUS_REVISION_MISMATCH;
    }
    return STATUS_SUCCESS;
}

/******************************************************************************
 *        RtlVerifyVersionInfo   (NTDLL.@)
 */
NTSTATUS WINAPI RtlVerifyVersionInfo( const RTL_OSVERSIONINFOEXW *info,
                                      DWORD dwTypeMask, DWORDLONG dwlConditionMask )
{
    RTL_OSVERSIONINFOEXW ver;
    NTSTATUS status;

    TRACE("(%p,0x%lx,0x%s)\n", info, dwTypeMask, wine_dbgstr_longlong(dwlConditionMask));

    ver.dwOSVersionInfoSize = sizeof(ver);
    if ((status = RtlGetVersion( &ver )) != STATUS_SUCCESS) return status;

    if(!(dwTypeMask && dwlConditionMask)) return STATUS_INVALID_PARAMETER;

    if(dwTypeMask & VER_PRODUCT_TYPE)
    {
        status = version_compare_values(ver.wProductType, info->wProductType, dwlConditionMask >> 7*3 & 0x07);
        if (status != STATUS_SUCCESS)
            return status;
    }
    if(dwTypeMask & VER_SUITENAME)
        switch(dwlConditionMask >> 6*3 & 0x07)
        {
            case VER_AND:
                if((info->wSuiteMask & ver.wSuiteMask) != info->wSuiteMask)
                    return STATUS_REVISION_MISMATCH;
                break;
            case VER_OR:
                if(!(info->wSuiteMask & ver.wSuiteMask) && info->wSuiteMask)
                    return STATUS_REVISION_MISMATCH;
                break;
            default:
                return STATUS_INVALID_PARAMETER;
        }
    if(dwTypeMask & VER_PLATFORMID)
    {
        status = version_compare_values(ver.dwPlatformId, info->dwPlatformId, dwlConditionMask >> 3*3 & 0x07);
        if (status != STATUS_SUCCESS)
            return status;
    }
    if(dwTypeMask & VER_BUILDNUMBER)
    {
        status = version_compare_values(ver.dwBuildNumber, info->dwBuildNumber, dwlConditionMask >> 2*3 & 0x07);
        if (status != STATUS_SUCCESS)
            return status;
    }

    if(dwTypeMask & (VER_MAJORVERSION|VER_MINORVERSION|VER_SERVICEPACKMAJOR|VER_SERVICEPACKMINOR))
    {
        unsigned char condition, last_condition = 0;
        BOOLEAN do_next_check = TRUE;

        if(dwTypeMask & VER_MAJORVERSION)
        {
            condition = version_update_condition(&last_condition, dwlConditionMask >> 1*3 & 0x07);
            status = version_compare_values(ver.dwMajorVersion, info->dwMajorVersion, condition);
            do_next_check = (ver.dwMajorVersion == info->dwMajorVersion) &&
                ((condition >= VER_EQUAL) && (condition <= VER_LESS_EQUAL));
        }
        if((dwTypeMask & VER_MINORVERSION) && do_next_check)
        {
            condition = version_update_condition(&last_condition, dwlConditionMask >> 0*3 & 0x07);
            status = version_compare_values(ver.dwMinorVersion, info->dwMinorVersion, condition);
            do_next_check = (ver.dwMinorVersion == info->dwMinorVersion) &&
                ((condition >= VER_EQUAL) && (condition <= VER_LESS_EQUAL));
        }
        if((dwTypeMask & VER_SERVICEPACKMAJOR) && do_next_check)
        {
            condition = version_update_condition(&last_condition, dwlConditionMask >> 5*3 & 0x07);
            status = version_compare_values(ver.wServicePackMajor, info->wServicePackMajor, condition);
            do_next_check = (ver.wServicePackMajor == info->wServicePackMajor) &&
                ((condition >= VER_EQUAL) && (condition <= VER_LESS_EQUAL));
        }
        if((dwTypeMask & VER_SERVICEPACKMINOR) && do_next_check)
        {
            condition = version_update_condition(&last_condition, dwlConditionMask >> 4*3 & 0x07);
            status = version_compare_values(ver.wServicePackMinor, info->wServicePackMinor, condition);
        }

        if (status != STATUS_SUCCESS)
            return status;
    }

    return STATUS_SUCCESS;
}


/******************************************************************************
 *        VerSetConditionMask   (NTDLL.@)
 */
ULONGLONG WINAPI VerSetConditionMask( ULONGLONG condition_mask, DWORD type_mask, BYTE condition )
{
    condition &= 0x07;
    if (type_mask & VER_PRODUCT_TYPE) condition_mask |= condition << 7*3;
    else if (type_mask & VER_SUITENAME) condition_mask |= condition << 6*3;
    else if (type_mask & VER_SERVICEPACKMAJOR) condition_mask |= condition << 5*3;
    else if (type_mask & VER_SERVICEPACKMINOR) condition_mask |= condition << 4*3;
    else if (type_mask & VER_PLATFORMID) condition_mask |= condition << 3*3;
    else if (type_mask & VER_BUILDNUMBER) condition_mask |= condition << 2*3;
    else if (type_mask & VER_MAJORVERSION) condition_mask |= condition << 1*3;
    else if (type_mask & VER_MINORVERSION) condition_mask |= condition << 0*3;
    return condition_mask;
}
