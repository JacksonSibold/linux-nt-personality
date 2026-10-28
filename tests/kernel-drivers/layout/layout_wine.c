/* layout_wine.c - prints the layout of the structures in layout_list.h as Wine's WDK headers define them. */
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ddk/wdm.h"

#define S(t) printf("%s %zu\n", #t, sizeof(t));
#define SM(t) S(t)
#define FM(t, f) F(t, f)
#define F(t, f) printf("%s.%s %zu\n", #t, #f, offsetof(t, f));

int main(void)
{
#include "layout_list.h"
    return 0;
}
