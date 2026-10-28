/*
 * zlib.h - stand-in for the one zlib function Wine's rtl.c uses, crc32 (RtlComputeCrc32). The checksum is the standard
 * CRC-32 of zlib: polynomial 0xedb88320, reflected, with the initial value passed in and the final complement applied.
 * Original code of this repository.
 */
#ifndef RTL_COMPAT_ZLIB_H
#define RTL_COMPAT_ZLIB_H

static inline unsigned long crc32( unsigned long crc, const unsigned char *buf, unsigned int len )
{
    unsigned int i, bit;

    crc = ~crc & 0xffffffffUL;
    for (i = 0; i < len; i++)
    {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1)));
    }
    return ~crc & 0xffffffffUL;
}

#endif
