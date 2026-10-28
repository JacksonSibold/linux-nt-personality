/* intrin.h - stand-in: the compiler builtins behind the MSVC intrinsics that the ported sources use. */
#ifndef RTL_COMPAT_INTRIN_H
#define RTL_COMPAT_INTRIN_H
#define __popcnt(x) ((unsigned int)__builtin_popcount(x))
#endif
