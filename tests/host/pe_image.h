/* pe_image.h - build small PE32+ images in memory for the host tests.
 *
 * Layout of the valid base image (IMG_LEN bytes, file alignment 0x200,
 * section alignment 0x1000, SizeOfImage 0x3000, SizeOfHeaders 0x400):
 *
 *   0x000  DOS header ("MZ", e_lfanew = IMG_NT)
 *   0x080  "PE\0\0", COFF header, optional header (PE32+, 240 bytes)
 *   0x188  section table: .text (RVA 0x1000, raw 0x400), .rdata (RVA 0x2000, raw 0x600)
 *   0x400  .text raw data (executable)
 *   0x600  .rdata raw data: import table, reloc block, exception table (see pe_image.c)
 */
#ifndef PE_IMAGE_H
#define PE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define IMG_LEN        0x800u
#define IMG_CAP        0x1000u
#define IMG_NT         0x080u
#define IMG_OPT        0x098u          /* IMG_NT + 4 + 20                    */
#define IMG_DIRS       0x108u          /* IMG_OPT + 112                      */
#define IMG_SEC(i)     (0x188u + 40u * (unsigned)(i))
#define IMG_TEXT_RAW   0x400u
#define IMG_RDATA_RAW  0x600u

/* Data directory indices used by the tests. */
#define DIR_EXPORT     0
#define DIR_IMPORT     1
#define DIR_EXCEPTION  3
#define DIR_BASERELOC  5

void img_w16(uint8_t *b, size_t off, uint16_t v);
void img_w32(uint8_t *b, size_t off, uint32_t v);
void img_w64(uint8_t *b, size_t off, uint64_t v);
uint32_t img_r32(const uint8_t *b, size_t off);

void img_set_dir(uint8_t *b, unsigned idx, uint32_t rva, uint32_t size);

/* Writes the valid base image into the first IMG_LEN bytes of b. */
void img_build_valid(uint8_t *b);

/* Adds a one-DLL import table (KERNEL32.dll!ExitProcess) to .rdata. */
void img_add_import(uint8_t *b);

/* Adds a base-relocation block with two DIR64/HIGHLOW entries to .rdata. */
void img_add_reloc(uint8_t *b);

/* Adds one RUNTIME_FUNCTION entry (begin 0x1000, end 0x1007) to .rdata. */
void img_add_exception(uint8_t *b);

#endif /* PE_IMAGE_H */
