/*
 * PE image helpers: RtlImageNtHeader, RtlImageDirectoryEntryToData, RtlImageRvaToSection, RtlImageRvaToVa and
 * RtlAddressInSectionTable.
 *
 * Extracted from Wine's dlls/ntdll/loader.c:
 *
 * Copyright 1993 Robert J. Amstadt
 * Copyright 1995 Martin von Loewis
 * Copyright 1995, 1996, 1997 Alexandre Julliard
 * Copyright 1997 Eric Youngdale
 * Copyright 1999 Ulrich Weigand
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
 *
 * The loader itself is not taken from Wine: this project has its own (ldr.c). These functions are small and exported by
 * ntdll.dll, and other Rtl code (environment, resources, driver images) calls them.
 */

#include <stdarg.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/exception.h"

/***********************************************************************
 *           RtlImageNtHeader   (NTDLL.@)
 */
PIMAGE_NT_HEADERS WINAPI RtlImageNtHeader(HMODULE hModule)
{
    IMAGE_NT_HEADERS *ret;

    __TRY
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)hModule;

        ret = NULL;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            ret = (IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);
            if (ret->Signature != IMAGE_NT_SIGNATURE) ret = NULL;
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        return NULL;
    }
    __ENDTRY
    return ret;
}

/***********************************************************************
 *           RtlImageDirectoryEntryToData   (NTDLL.@)
 */
PVOID WINAPI RtlImageDirectoryEntryToData( HMODULE module, BOOL image, WORD dir, ULONG *size )
{
    const IMAGE_NT_HEADERS *nt;
    DWORD addr;

    if ((ULONG_PTR)module & 1) image = FALSE;  /* mapped as data file */
    module = (HMODULE)((ULONG_PTR)module & ~3);
    if (!(nt = RtlImageNtHeader( module ))) return NULL;
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        const IMAGE_NT_HEADERS64 *nt64 = (const IMAGE_NT_HEADERS64 *)nt;

        if (dir >= nt64->OptionalHeader.NumberOfRvaAndSizes) return NULL;
        if (!(addr = nt64->OptionalHeader.DataDirectory[dir].VirtualAddress)) return NULL;
        *size = nt64->OptionalHeader.DataDirectory[dir].Size;
        if (image || addr < nt64->OptionalHeader.SizeOfHeaders) return (char *)module + addr;
    }
    else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        const IMAGE_NT_HEADERS32 *nt32 = (const IMAGE_NT_HEADERS32 *)nt;

        if (dir >= nt32->OptionalHeader.NumberOfRvaAndSizes) return NULL;
        if (!(addr = nt32->OptionalHeader.DataDirectory[dir].VirtualAddress)) return NULL;
        *size = nt32->OptionalHeader.DataDirectory[dir].Size;
        if (image || addr < nt32->OptionalHeader.SizeOfHeaders) return (char *)module + addr;
    }
    else return NULL;

    /* not mapped as image, need to find the section containing the virtual address */
    return RtlImageRvaToVa( nt, module, addr, NULL );
}


/***********************************************************************
 *           RtlImageRvaToSection   (NTDLL.@)
 */
PIMAGE_SECTION_HEADER WINAPI RtlImageRvaToSection( const IMAGE_NT_HEADERS *nt,
                                                   HMODULE module, DWORD rva )
{
    int i;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION( nt );

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        if ((sec->VirtualAddress <= rva) && (sec->VirtualAddress + sec->SizeOfRawData > rva))
            return (PIMAGE_SECTION_HEADER)sec;
    }
    return NULL;
}


/***********************************************************************
 *           RtlImageRvaToVa   (NTDLL.@)
 */
PVOID WINAPI RtlImageRvaToVa( const IMAGE_NT_HEADERS *nt, HMODULE module,
                              DWORD rva, IMAGE_SECTION_HEADER **section )
{
    IMAGE_SECTION_HEADER *sec;

    if (section && *section)  /* try this section first */
    {
        sec = *section;
        if ((sec->VirtualAddress <= rva) && (sec->VirtualAddress + sec->SizeOfRawData > rva))
            goto found;
    }
    if (!(sec = RtlImageRvaToSection( nt, module, rva ))) return NULL;
 found:
    if (section) *section = sec;
    return (char *)module + sec->PointerToRawData + (rva - sec->VirtualAddress);
}


/***********************************************************************
 *           RtlAddressInSectionTable   (NTDLL.@)
 */
PVOID WINAPI RtlAddressInSectionTable( const IMAGE_NT_HEADERS *nt, HMODULE module,
                                       DWORD rva )
{
    return RtlImageRvaToVa( nt, module, rva, NULL );
}

