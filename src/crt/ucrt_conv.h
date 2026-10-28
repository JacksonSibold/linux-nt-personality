/*
 * ucrt_conv.h - number conversion for the C runtime: integers from and to text (strtol family, atoi, _itoa)
 * and floating point from text (strtod, strtof, atof), for char and 16-bit wide characters. Printing floating
 * point values is in ucrt_printf.h on top of ucrt_dtoa.h. Host tests: tests/crt-host/test_conv.c.
 */
#ifndef UCRT_CONV_H
#define UCRT_CONV_H

#include "ucrt_ctype.h"
#include "ucrt_state.h"

#define UCRT_T char
#define UCRT_P ucrt_str
#define UCRT_ISSPACE(c) ucrt_isspace((unsigned char)(c))
#include "ucrt_conv_t.h"
#undef UCRT_T
#undef UCRT_P
#undef UCRT_ISSPACE

#define UCRT_T ucrt_wchar
#define UCRT_P ucrt_wcs
#define UCRT_ISSPACE(c) ucrt_iswspace((ucrt_wchar)(c))
#include "ucrt_conv_t.h"
#undef UCRT_T
#undef UCRT_P
#undef UCRT_ISSPACE

/* The plain C names of the narrow forms that the template spells with the str prefix. */
static inline int ucrt_atoi(const char *s) { return ucrt_stratoi(s); }
static inline int64_t ucrt_atoi64(const char *s) { return ucrt_stratoi64(s); }

#endif /* UCRT_CONV_H */
