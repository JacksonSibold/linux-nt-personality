/* rtl_sha.h - SHA-1 and SHA-256 of a whole message (sha.c). The result is 20 and 32 bytes. */
#ifndef NTDLL_RTL_SHA_H
#define NTDLL_RTL_SHA_H

void rtl_sha1( const BYTE *data, SIZE_T len, BYTE *result );
void rtl_sha256( const BYTE *data, SIZE_T len, BYTE *result );

#endif
