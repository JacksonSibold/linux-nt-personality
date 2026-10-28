/*
 * test_vcr_typeinfo_pure.c - tests for src/ntdll/vcr_typeinfo_pure.h: name comparison, the hash, and the decoder of decorated
 * type names, including template instances with back-references.
 */
#include "minitest.h"
#include "vcr_typeinfo_pure.h"

#include <string.h>

static const char *decode(const char *d)
{
    static char buf[512];
    size_t n = vcr_ti_undecorate(d, buf, sizeof buf);
    return n ? buf : NULL;
}

#define EXPECT_NAME(d, want) do { const char *g = decode(d); MT_EXPECT(g != NULL); if (g) { if (strcmp(g, want) != 0) printf("    %s -> '%s', want '%s'\n", d, g, want); MT_EXPECT(strcmp(g, want) == 0); } } while (0)

static void plain_names(void)
{
    EXPECT_NAME(".?AVexception@std@@", "class std::exception");
    EXPECT_NAME(".?AVbad_alloc@std@@", "class std::bad_alloc");
    EXPECT_NAME(".?AUFoo@@", "struct Foo");
    EXPECT_NAME(".?ATBar@ns@@", "union ns::Bar");
    EXPECT_NAME(".?AVInner@Outer@ns@@", "class ns::Outer::Inner");
    EXPECT_NAME(".?AW4Color@@", "enum Color");
}

static void builtin_and_pointer_types(void)
{
    EXPECT_NAME(".H", "int");
    EXPECT_NAME(".D", "char");
    EXPECT_NAME(".PEAD", "char * __ptr64");
    EXPECT_NAME(".PEBD", "char const * __ptr64");
    EXPECT_NAME(".PEAX", "void * __ptr64");
    EXPECT_NAME(".PEAVFoo@@", "class Foo * __ptr64");
    EXPECT_NAME(".AEAH", "int & __ptr64");
    EXPECT_NAME(".PEAPEAD", "char * __ptr64 * __ptr64");
    EXPECT_NAME(".$$T", "std::nullptr_t");
    EXPECT_NAME(".N", "double");
    EXPECT_NAME(".K", "unsigned long");
}

static void template_instances(void)
{
    EXPECT_NAME(".?AV?$allocator@H@std@@", "class std::allocator<int>");
    EXPECT_NAME(".?AV?$vector@HV?$allocator@H@std@@@std@@", "class std::vector<int,class std::allocator<int> >");
    EXPECT_NAME(".?AV?$pair@HN@std@@", "class std::pair<int,double>");
    /* the '2' is a back-reference to "std" in the template's own table */
    EXPECT_NAME(".?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@",
                "class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >");
    EXPECT_NAME(".?AV?$shared_ptr@VFoo@@@std@@", "class std::shared_ptr<class Foo>");
}

static void names_it_cannot_read(void)
{
    MT_EXPECT(decode("?AVFoo@@") == NULL);            /* no leading '.' */
    MT_EXPECT(decode(".?AVFoo") == NULL);             /* unterminated */
    MT_EXPECT(decode(".?AV?$array@H$03@std@@") == NULL); /* a non-type argument */
    MT_EXPECT(decode(".?AVFoo@@extra") == NULL);
    MT_EXPECT(decode(".?AV5@@") == NULL);             /* back-reference to nothing */
    MT_EXPECT(decode(".") == NULL);
}

static void compare_and_hash(void)
{
    MT_EXPECT_EQ(vcr_ti_compare(".?AVFoo@@", ".?AVFoo@@"), 0);
    MT_EXPECT(vcr_ti_compare(".?AVFoo@@", ".?AVBar@@") > 0);
    MT_EXPECT(vcr_ti_compare(".?AVBar@@", ".?AVFoo@@") < 0);
    {
        char copy[] = ".?AVFoo@@";
        MT_EXPECT_EQ(vcr_ti_compare(".?AVFoo@@", copy), 0); /* equal contents, different storage */
    }
    /* the hash skips the '.', so it is the FNV-1a of "?AVFoo@@" */
    {
        uint64_t h = 14695981039346656037ull;
        const char *s = "?AVFoo@@";
        for (; *s; ++s) {
            h ^= (unsigned char)*s;
            h *= 1099511628211ull;
        }
        MT_EXPECT(vcr_ti_hash(".?AVFoo@@") == h);
        MT_EXPECT(vcr_ti_hash(".?AVFoo@@") != vcr_ti_hash(".?AVBar@@"));
    }
}

static const mt_case_t cases[] = {
    MT_CASE(plain_names), MT_CASE(builtin_and_pointer_types), MT_CASE(template_instances), MT_CASE(names_it_cannot_read),
    MT_CASE(compare_and_hash),
};

int main(void)
{
    return MT_RUN("vcr_typeinfo_pure", cases);
}
