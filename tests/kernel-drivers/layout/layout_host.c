/* layout_host.c - prints the layout of the structures in layout_list.h as the driver host defines them. */
#include <stdio.h>
#include <stddef.h>
#include "drvhost_int.h"

#define S(t) printf("%s %zu\n", #t, sizeof(t));
#define SM(t) printf("%s MISSING\n", #t);
#define FM(t, f) printf("%s.%s MISSING\n", #t, #f);
#define F(t, f) printf("%s.%s %zu\n", #t, #f, offsetof(t, f));

int main(void)
{
#include "layout_list.h"
    return 0;
}
