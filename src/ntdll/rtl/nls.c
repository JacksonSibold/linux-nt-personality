/*
 * Rtl national language support: code page conversion and case mapping
 *
 * The conversion functions and their semantics are those of Wine's dlls/ntdll/locale.c:
 *
 * Copyright 2000 Alexandre Julliard
 * Copyright 2003 Thomas Mertes
 * Copyright 2022 Alexandre Julliard (NLS tables rework)
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
 * Changes for ntpers: Wine loads the code page and case tables from NLS files that the Unix side maps. This
 * personality has no NLS files, so the tables are built from nls_tables.h (build/gen_nls_tables.py): single-byte
 * code pages and the simple Unicode case mapping. The default code pages are 1252 (ANSI) and 437 (OEM). Double-byte
 * pages are not provided yet; RtlCustomCPToUnicodeN and its relatives still take a caller's CPTABLEINFO.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "locale_private.h"
#include "nls_tables.h"

/* The tables of the two default pages. Each wide-character table is a 64 KB byte array in the bss. */
static CPTABLEINFO ansi_info, oem_info;
static USHORT ansi_mb[256], oem_mb[256];
static char ansi_wc[65536], oem_wc[65536];
static BOOLEAN nls_ready;
static ULONG ansi_cp = 1252, oem_cp = 437;

static const struct nls_sbcs *find_page( unsigned int cp )
{
    unsigned int i;
    for (i = 0; i < sizeof(nls_sbcs_pages) / sizeof(nls_sbcs_pages[0]); i++)
        if (nls_sbcs_pages[i].cp == cp) return &nls_sbcs_pages[i];
    return NULL;
}

static void build_page( CPTABLEINFO *info, USHORT *mb, char *wc, unsigned int cp )
{
    const struct nls_sbcs *page = find_page( cp );
    unsigned int i;

    memset( info, 0, sizeof(*info) );
    if (!page) { page = find_page( 1252 ); cp = 1252; }
    info->CodePage = cp;
    info->MaximumCharacterSize = 1;
    info->DefaultChar = page->default_char;
    info->UniDefaultChar = page->uni_default_char;
    info->TransDefaultChar = page->default_char;
    info->TransUniDefaultChar = page->uni_default_char;
    for (i = 0; i < 256; i++) mb[i] = page->to_wc[i];
    memset( wc, page->default_char, 65536 );
    for (i = 0; i < 0x80; i++) wc[i] = i;
    for (i = 0; i < page->to_mb_count; i++) wc[page->to_mb[i].wc] = page->to_mb[i].mb;
    info->MultiByteTable = mb;
    info->WideCharTable = wc;
}

static void nls_init(void)
{
    if (nls_ready) return;
    build_page( &ansi_info, ansi_mb, ansi_wc, ansi_cp );
    build_page( &oem_info, oem_mb, oem_wc, oem_cp );
    nls_ready = TRUE;
}

/* The case mapping of one UTF-16 code unit: the three-level tables of l_intl.nls. */
static WCHAR casemap( const USHORT *table, WCHAR ch )
{
    return ch + table[table[table[ch >> 8] + ((ch >> 4) & 0x0f)] + (ch & 0x0f)];
}

static WCHAR casemap_upper( WCHAR ch )
{
    return casemap( nls_casemap + 2, ch );
}

static WCHAR casemap_lower( WCHAR ch )
{
    return casemap( nls_casemap + 2 + nls_casemap[1], ch );
}

/**************************************************************************
 *      RtlGetDefaultCodePage   (NTDLL.@)
 */
void WINAPI RtlGetDefaultCodePage( USHORT *ansi, USHORT *oem )
{
    *ansi = ansi_cp;
    *oem = oem_cp;
}

/**************************************************************************
 *      RtlpSetDefaultCodePages   (ntpers)
 *
 * Selects the ANSI and OEM code pages. A page that nls_tables.h does not hold leaves the current one in place.
 */
BOOLEAN WINAPI RtlpSetDefaultCodePages( ULONG ansi, ULONG oem )
{
    if (!find_page( ansi ) && ansi != CP_UTF8) return FALSE;
    if (!find_page( oem ) && oem != CP_UTF8) return FALSE;
    ansi_cp = ansi;
    oem_cp = oem;
    nls_ready = FALSE;
    return TRUE;
}

#define ANSI_IS_UTF8 (ansi_cp == CP_UTF8)
#define OEM_IS_UTF8 (oem_cp == CP_UTF8)

/**************************************************************************
 *      RtlAnsiCharToUnicodeChar   (NTDLL.@)
 */
WCHAR WINAPI RtlAnsiCharToUnicodeChar( char **ansi )
{
    unsigned char ch = *(*ansi)++;

    nls_init();
    if (ANSI_IS_UTF8)
    {
        unsigned int res;

        if (ch < 0x80) return ch;
        if ((res = decode_utf8_char( ch, (const char **)ansi, *ansi + 3 )) > 0x10ffff) res = 0xfffd;
        return res;
    }
    return ansi_info.MultiByteTable[ch];
}


/******************************************************************************
 *	RtlCompareUnicodeStrings   (NTDLL.@)
 */
LONG WINAPI RtlCompareUnicodeStrings( const WCHAR *s1, SIZE_T len1, const WCHAR *s2, SIZE_T len2,
                                      BOOLEAN case_insensitive )
{
    LONG ret = 0;
    SIZE_T len = min( len1, len2 );

    if (case_insensitive)
    {
        while (!ret && len--) ret = casemap_upper( *s1++ ) - casemap_upper( *s2++ );
    }
    else
    {
        while (!ret && len--) ret = *s1++ - *s2++;
    }
    if (!ret) ret = len1 - len2;
    return ret;
}


/**************************************************************************
 *	RtlPrefixUnicodeString   (NTDLL.@)
 */
BOOLEAN WINAPI RtlPrefixUnicodeString( const UNICODE_STRING *s1, const UNICODE_STRING *s2,
                                       BOOLEAN ignore_case )
{
    unsigned int i;

    if (s1->Length > s2->Length) return FALSE;
    if (ignore_case)
    {
        for (i = 0; i < s1->Length / sizeof(WCHAR); i++)
            if (casemap_upper( s1->Buffer[i] ) != casemap_upper( s2->Buffer[i] )) return FALSE;
    }
    else
    {
        for (i = 0; i < s1->Length / sizeof(WCHAR); i++)
            if (s1->Buffer[i] != s2->Buffer[i]) return FALSE;
    }
    return TRUE;
}


/******************************************************************************
 *	RtlHashUnicodeString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlHashUnicodeString( const UNICODE_STRING *string, BOOLEAN case_insensitive,
                                      ULONG alg, ULONG *hash )
{
    unsigned int i;

    if (!string || !hash) return STATUS_INVALID_PARAMETER;

    switch (alg)
    {
    case HASH_STRING_ALGORITHM_DEFAULT:
    case HASH_STRING_ALGORITHM_X65599:
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    *hash = 0;
    if (!case_insensitive)
        for (i = 0; i < string->Length / sizeof(WCHAR); i++)
            *hash = *hash * 65599 + string->Buffer[i];
    else
        for (i = 0; i < string->Length / sizeof(WCHAR); i++)
            *hash = *hash * 65599 + casemap_upper( string->Buffer[i] );
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlCustomCPToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlCustomCPToUnicodeN( CPTABLEINFO *info, WCHAR *dst, DWORD dstlen, DWORD *reslen,
                                       const char *src, DWORD srclen )
{
    unsigned int ret = cp_mbstowcs( info, dst, dstlen / sizeof(WCHAR), src, srclen );
    if (reslen) *reslen = ret * sizeof(WCHAR);
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUnicodeToCustomCPN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToCustomCPN( CPTABLEINFO *info, char *dst, DWORD dstlen, DWORD *reslen,
                                       const WCHAR *src, DWORD srclen )
{
    unsigned int ret = cp_wcstombs( info, dst, dstlen, src, srclen / sizeof(WCHAR) );
    if (reslen) *reslen = ret;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlMultiByteToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlMultiByteToUnicodeN( WCHAR *dst, DWORD dstlen, DWORD *reslen,
                                        const char *src, DWORD srclen )
{
    unsigned int ret;

    nls_init();
    if (!ANSI_IS_UTF8)
        ret = cp_mbstowcs( &ansi_info, dst, dstlen / sizeof(WCHAR), src, srclen );
    else
        utf8_mbstowcs( dst, dstlen / sizeof(WCHAR), &ret, src, srclen );

    if (reslen) *reslen = ret * sizeof(WCHAR);
    return STATUS_SUCCESS;
}


/**************************************************************************
 *      RtlMultiByteToUnicodeSize   (NTDLL.@)
 */
NTSTATUS WINAPI RtlMultiByteToUnicodeSize( DWORD *size, const char *str, DWORD len )
{
    unsigned int ret;

    nls_init();
    if (!ANSI_IS_UTF8)
        ret = cp_mbstowcs_size( &ansi_info, str, len );
    else
        utf8_mbstowcs_size( str, len, &ret );

    *size = ret * sizeof(WCHAR);
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlOemToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlOemToUnicodeN( WCHAR *dst, DWORD dstlen, DWORD *reslen,
                                  const char *src, DWORD srclen )
{
    unsigned int ret;

    nls_init();
    if (!OEM_IS_UTF8)
        ret = cp_mbstowcs( &oem_info, dst, dstlen / sizeof(WCHAR), src, srclen );
    else
        utf8_mbstowcs( dst, dstlen / sizeof(WCHAR), &ret, src, srclen );

    if (reslen) *reslen = ret * sizeof(WCHAR);
    return STATUS_SUCCESS;
}


/**************************************************************************
 *      RtlOemStringToUnicodeSize   (NTDLL.@)
 *      RtlxOemStringToUnicodeSize  (NTDLL.@)
 */
DWORD WINAPI RtlOemStringToUnicodeSize( const STRING *str )
{
    unsigned int ret;

    nls_init();
    if (!OEM_IS_UTF8)
        ret = cp_mbstowcs_size( &oem_info, str->Buffer, str->Length );
    else
        utf8_mbstowcs_size( str->Buffer, str->Length, &ret );

    return (ret + 1) * sizeof(WCHAR);
}


/**************************************************************************
 *      RtlUnicodeStringToOemSize   (NTDLL.@)
 *      RtlxUnicodeStringToOemSize  (NTDLL.@)
 */
DWORD WINAPI RtlUnicodeStringToOemSize( const UNICODE_STRING *str )
{
    unsigned int ret;

    nls_init();
    if (!OEM_IS_UTF8)
        ret = cp_wcstombs_size( &oem_info, str->Buffer, str->Length / sizeof(WCHAR) );
    else
        utf8_wcstombs_size( str->Buffer, str->Length / sizeof(WCHAR), &ret );

    return ret + 1;
}


/**************************************************************************
 *	RtlUnicodeToMultiByteN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToMultiByteN( char *dst, DWORD dstlen, DWORD *reslen,
                                        const WCHAR *src, DWORD srclen )
{
    unsigned int ret;

    nls_init();
    if (!ANSI_IS_UTF8)
        ret = cp_wcstombs( &ansi_info, dst, dstlen, src, srclen / sizeof(WCHAR) );
    else
        utf8_wcstombs( dst, dstlen, &ret, src, srclen / sizeof(WCHAR) );

    if (reslen) *reslen = ret;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *      RtlUnicodeToMultiByteSize   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToMultiByteSize( DWORD *size, const WCHAR *str, DWORD len )
{
    unsigned int ret;

    nls_init();
    if (!ANSI_IS_UTF8)
        ret = cp_wcstombs_size( &ansi_info, str, len / sizeof(WCHAR) );
    else
        utf8_wcstombs_size( str, len / sizeof(WCHAR), &ret );

    *size = ret;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUnicodeToOemN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToOemN( char *dst, DWORD dstlen, DWORD *reslen,
                                  const WCHAR *src, DWORD srclen )
{
    unsigned int ret;

    nls_init();
    if (!OEM_IS_UTF8)
        ret = cp_wcstombs( &oem_info, dst, dstlen, src, srclen / sizeof(WCHAR) );
    else
        utf8_wcstombs( dst, dstlen, &ret, src, srclen / sizeof(WCHAR) );

    if (reslen) *reslen = ret;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlDowncaseUnicodeChar   (NTDLL.@)
 */
WCHAR WINAPI RtlDowncaseUnicodeChar( WCHAR wch )
{
    return casemap_lower( wch );
}


/**************************************************************************
 *	RtlDowncaseUnicodeString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlDowncaseUnicodeString( UNICODE_STRING *dest, const UNICODE_STRING *src,
                                          BOOLEAN alloc )
{
    DWORD i, len = src->Length;

    if (alloc)
    {
        dest->MaximumLength = len;
        if (!(dest->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, len ))) return STATUS_NO_MEMORY;
    }
    else if (len > dest->MaximumLength) return STATUS_BUFFER_OVERFLOW;

    for (i = 0; i < len / sizeof(WCHAR); i++)
        dest->Buffer[i] = casemap_lower( src->Buffer[i] );
    dest->Length = len;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUpcaseUnicodeChar   (NTDLL.@)
 */
WCHAR WINAPI RtlUpcaseUnicodeChar( WCHAR wch )
{
    return casemap_upper( wch );
}


/**************************************************************************
 *	RtlUpcaseUnicodeString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeString( UNICODE_STRING *dest, const UNICODE_STRING *src,
                                        BOOLEAN alloc )
{
    DWORD i, len = src->Length;

    if (alloc)
    {
        dest->MaximumLength = len;
        if (!(dest->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, len ))) return STATUS_NO_MEMORY;
    }
    else if (len > dest->MaximumLength) return STATUS_BUFFER_OVERFLOW;

    for (i = 0; i < len / sizeof(WCHAR); i++)
        dest->Buffer[i] = casemap_upper( src->Buffer[i] );
    dest->Length = len;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUpcaseUnicodeToCustomCPN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeToCustomCPN( CPTABLEINFO *info, char *dst, DWORD dstlen, DWORD *reslen,
                                             const WCHAR *src, DWORD srclen )
{
    DWORD i, ret;

    srclen /= sizeof(WCHAR);
    if (info->DBCSCodePage)
    {
        WCHAR *uni2cp = info->WideCharTable;

        for (i = dstlen; srclen && i; i--, srclen--, src++)
        {
            WCHAR ch = casemap_upper( *src );
            if (uni2cp[ch] & 0xff00)
            {
                if (i == 1) break;  /* do not output a partial char */
                i--;
                *dst++ = uni2cp[ch] >> 8;
            }
            *dst++ = (char)uni2cp[ch];
        }
        ret = dstlen - i;
    }
    else
    {
        char *uni2cp = info->WideCharTable;
        ret = min( srclen, dstlen );
        for (i = 0; i < ret; i++) dst[i] = uni2cp[casemap_upper( src[i] )];
    }
    if (reslen) *reslen = ret;
    return STATUS_SUCCESS;
}


static NTSTATUS upcase_unicode_to_utf8( char *dst, DWORD dstlen, DWORD *reslen,
                                        const WCHAR *src, DWORD srclen )
{
    char *end;
    unsigned int val;
    NTSTATUS status = STATUS_SUCCESS;

    srclen /= sizeof(WCHAR);

    for (end = dst + dstlen; srclen; srclen--, src++)
    {
        WCHAR ch = casemap_upper( *src );

        if (ch < 0x80)  /* 0x00-0x7f: 1 byte */
        {
            if (dst > end - 1) break;
            *dst++ = ch;
            continue;
        }
        if (ch < 0x800)  /* 0x80-0x7ff: 2 bytes */
        {
            if (dst > end - 2) break;
            dst[1] = 0x80 | (ch & 0x3f);
            ch >>= 6;
            dst[0] = 0xc0 | ch;
            dst += 2;
            continue;
        }
        if (!get_utf16( src, srclen, &val ))
        {
            val = 0xfffd;
            status = STATUS_SOME_NOT_MAPPED;
        }
        if (val < 0x10000)  /* 0x800-0xffff: 3 bytes */
        {
            if (dst > end - 3) break;
            dst[2] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[1] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[0] = 0xe0 | val;
            dst += 3;
        }
        else   /* 0x10000-0x10ffff: 4 bytes */
        {
            if (dst > end - 4) break;
            dst[3] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[2] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[1] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[0] = 0xf0 | val;
            dst += 4;
            src++;
            srclen--;
        }
    }
    if (srclen) status = STATUS_BUFFER_TOO_SMALL;
    if (reslen) *reslen = dstlen - (end - dst);
    return status;
}

/**************************************************************************
 *	RtlUpcaseUnicodeToMultiByteN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeToMultiByteN( char *dst, DWORD dstlen, DWORD *reslen,
                                              const WCHAR *src, DWORD srclen )
{
    nls_init();
    if (ANSI_IS_UTF8)
        return upcase_unicode_to_utf8( dst, dstlen, reslen, src, srclen );
    return RtlUpcaseUnicodeToCustomCPN( &ansi_info, dst, dstlen, reslen, src, srclen );
}


/**************************************************************************
 *	RtlUpcaseUnicodeToOemN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeToOemN( char *dst, DWORD dstlen, DWORD *reslen,
                                        const WCHAR *src, DWORD srclen )
{
    nls_init();
    if (OEM_IS_UTF8)
        return upcase_unicode_to_utf8( dst, dstlen, reslen, src, srclen );
    return RtlUpcaseUnicodeToCustomCPN( &oem_info, dst, dstlen, reslen, src, srclen );
}


/*********************************************************************
 *	towlower   (NTDLL.@)
 */
WCHAR __cdecl towlower( WCHAR ch )
{
    if (ch >= 0x100) return ch;
    return casemap_lower( ch );
}


/*********************************************************************
 *           towupper    (NTDLL.@)
 */
WCHAR __cdecl towupper( WCHAR ch )
{
    return casemap_upper( ch );
}


/**************************************************************************
 *	RtlUTF8ToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUTF8ToUnicodeN( WCHAR *dst, DWORD dstlen, DWORD *reslen, const char *src, DWORD srclen )
{
    unsigned int ret;
    NTSTATUS status;

    if (!src) return STATUS_INVALID_PARAMETER_4;
    if (!reslen) return STATUS_INVALID_PARAMETER;

    if (!dst)
        status = utf8_mbstowcs_size( src, srclen, &ret );
    else
        status = utf8_mbstowcs( dst, dstlen / sizeof(WCHAR), &ret, src, srclen );

    *reslen = ret * sizeof(WCHAR);
    return status;
}


/**************************************************************************
 *	RtlUnicodeToUTF8N   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToUTF8N( char *dst, DWORD dstlen, DWORD *reslen, const WCHAR *src, DWORD srclen )
{
    unsigned int ret;
    NTSTATUS status;

    if (!src) return STATUS_INVALID_PARAMETER_4;
    if (!reslen) return STATUS_INVALID_PARAMETER;
    if (dst && (srclen & 1)) return STATUS_INVALID_PARAMETER_5;

    if (!dst)
        status = utf8_wcstombs_size( src, srclen / sizeof(WCHAR), &ret );
    else
        status = utf8_wcstombs( dst, dstlen, &ret, src, srclen / sizeof(WCHAR) );

    *reslen = ret;
    return status;
}
