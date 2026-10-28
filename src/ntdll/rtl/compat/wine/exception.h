/*
 * wine/exception.h - stand-in for Wine's structured exception macros.
 *
 * Wine's __TRY ... __EXCEPT_PAGE_FAULT ... __ENDTRY turns a fault on a user pointer into an error code. Here the guarded block
 * simply runs: an invalid pointer faults and the exception goes to the process's handlers, as an unguarded access would.
 * Known gap, listed in docs/status/RTL-PORT.md; it goes away when ntdll's SEH (seh.c) can carry these frames.
 */
#ifndef RTL_COMPAT_WINE_EXCEPTION_H
#define RTL_COMPAT_WINE_EXCEPTION_H

#define __TRY if (1)
#define __EXCEPT(func) else
#define __EXCEPT_CTX(func, ctx) else
#define __EXCEPT_PAGE_FAULT else
#define __EXCEPT_ALL else
#define __FINALLY(func)
#define __ENDTRY
#ifndef GetExceptionCode
#define GetExceptionCode() 0
#endif

#endif
