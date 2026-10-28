/*
 * winnls.h - the constants of Wine's include/winnls.h that the ported ntdll sources use (character type flags, code pages,
 * conversion flags). Taken from Wine (LGPL-2.1-or-later); the function prototypes are left out.
 */
#ifndef RTL_COMPAT_WINNLS_H
#define RTL_COMPAT_WINNLS_H
#include "windef.h"
#define MB_PRECOMPOSED              0x01
#define MB_COMPOSITE                0x02
#define MB_USEGLYPHCHARS            0x04
#define MB_ERR_INVALID_CHARS        0x08
# define LOCALE_NAME_INVARIANT      L""
# define LOCALE_NAME_SYSTEM_DEFAULT      L"!x-sys-default-locale"
#define LOCALE_NAME_USER_DEFAULT    NULL
#define NORM_IGNORECASE            0x00000001
#define NORM_IGNORENONSPACE        0x00000002
#define NORM_IGNORESYMBOLS         0x00000004
#define SORT_DIGITSASNUMBERS       0x00000008
#define LINGUISTIC_IGNORECASE      0x00000010
#define LINGUISTIC_IGNOREDIACRITIC 0x00000020
#define SORT_STRINGSORT            0x00001000 /* Take punctuation into account */
#define NORM_IGNOREKANATYPE        0x00010000
#define NORM_IGNOREWIDTH           0x00020000
#define NORM_LINGUISTIC_CASING     0x08000000
#define CP_ACP        0
#define CP_OEMCP      1
#define CP_MACCP      2
#define CP_THREAD_ACP 3
#define CP_SYMBOL     42
#define CP_UTF7       65000
#define CP_UTF8       65001
#define CP_UNIXCP     65010 /* Wine extension */
#define CP_INSTALLED 0x1
#define CP_SUPPORTED 0x2
#define WC_DISCARDNS         0x0010
#define WC_SEPCHARS          0x0020
#define WC_DEFAULTCHAR       0x0040
#define WC_ERR_INVALID_CHARS 0x0080
#define WC_COMPOSITECHECK    0x0200
#define WC_NO_BEST_FIT_CHARS 0x0400
#define LCMAP_LOWERCASE  0x00000100	/* Make lower-case */
#define LCMAP_UPPERCASE  0x00000200	/* Make upper-case */
#define LCMAP_TITLECASE  0x00000300	/* Make title-case */
#define LCMAP_SORTKEY    0x00000400	/* Create a sort key */
#define LCMAP_BYTEREV    0x00000800	/* Reverse the result */
#define LCMAP_HASH       0x00040000
#define LCMAP_HIRAGANA   0x00100000	/* Transform Japanese katakana into hiragana */
#define LCMAP_KATAKANA   0x00200000	/* Transform Japanese hiragana into katakana */
#define LCMAP_HALFWIDTH  0x00400000	/* Use single byte chars in output */
#define LCMAP_FULLWIDTH  0x00800000	/* Use double byte chars in output */
#define LCMAP_LINGUISTIC_CASING   0x01000000 /* Change case by using language context */
#define LCMAP_SIMPLIFIED_CHINESE  0x02000000 /* Transform Chinese traditional into simplified */
#define LCMAP_TRADITIONAL_CHINESE 0x04000000 /* Transform Chinese simplified into traditional */
#define LCMAP_SORTHANDLE 0x20000000
#define	CT_CTYPE1		0x0001	/* usual ctype */
#define	CT_CTYPE2		0x0002	/* bidirectional layout info */
#define	CT_CTYPE3		0x0004	/* textprocessing info */
#define C1_UPPER		0x0001
#define C1_LOWER		0x0002
#define C1_DIGIT		0x0004
#define C1_SPACE		0x0008
#define C1_PUNCT		0x0010
#define C1_CNTRL		0x0020
#define C1_BLANK		0x0040
#define C1_XDIGIT		0x0080
#define C1_ALPHA		0x0100
#define C1_DEFINED		0x0200
#define	C2_LEFTTORIGHT		0x0001
#define	C2_RIGHTTOLEFT		0x0002
#define	C2_EUROPENUMBER		0x0003
#define	C2_EUROPESEPARATOR	0x0004
#define	C2_EUROPETERMINATOR	0x0005
#define	C2_ARABICNUMBER		0x0006
#define	C2_COMMONSEPARATOR	0x0007
#define	C2_BLOCKSEPARATOR	0x0008
#define	C2_SEGMENTSEPARATOR	0x0009
#define	C2_WHITESPACE		0x000A
#define	C2_OTHERNEUTRAL		0x000B
#define	C2_NOTAPPLICABLE	0x0000
#define	C3_NONSPACING		0x0001
#define	C3_DIACRITIC		0x0002
#define	C3_VOWELMARK		0x0004
#define	C3_SYMBOL		0x0008
#define	C3_KATAKANA		0x0010
#define	C3_HIRAGANA		0x0020
#define	C3_HALFWIDTH		0x0040
#define	C3_FULLWIDTH		0x0080
#define	C3_IDEOGRAPH		0x0100
#define	C3_KASHIDA		0x0200
#define	C3_LEXICAL		0x0400
#define	C3_HIGHSURROGATE	0x0800
#define	C3_LOWSURROGATE		0x1000
#define	C3_ALPHA		0x8000
#define	C3_NOTAPPLICABLE	0x0000
#define CSTR_LESS_THAN    1
#define CSTR_EQUAL        2
#define CSTR_GREATER_THAN 3
#define IS_HIGH_SURROGATE(ch) ((ch) >= 0xd800 && (ch) < 0xdc00)
#define IS_LOW_SURROGATE(ch) ((ch) >= 0xdc00 && (ch) < 0xe000)
#define IS_SURROGATE_PAIR(hi, lo) (IS_HIGH_SURROGATE(hi) && IS_LOW_SURROGATE(lo))
#endif
