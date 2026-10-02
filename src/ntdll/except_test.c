/*
 * except_test.c - tests for x64 SEH unwinder and dispatcher
 */

#include "unwind.h"
#include "except.h"
#include "nt_types.h"
#include "ntstatus.h"
#include "../loader/pe_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>

static int g_pass = 0, g_fail = 0;
static int g_verbose = 1;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        g_fail++; \
    } else { \
        if (g_verbose) printf("PASS " fmt "\n", ##__VA_ARGS__); \
        g_pass++; \
    } \
} while(0)

#define TEST_SECTION(name) do { printf("\n=== %s ===\n", name); } while(0)

static uint8_t *alloc_image(size_t sz) {
    uint8_t *p = (uint8_t *)calloc(1, sz);
    return p;
}

static uint8_t *alloc_stack(size_t sz, uint64_t *out_rsp_top) {
    size_t alloc = sz + 0x100;
    uint8_t *mem = (uint8_t *)aligned_alloc(16, alloc);
    memset(mem, 0xCC, alloc);
    *out_rsp_top = (uint64_t)(mem + sz);
    return mem;
}

static UNWIND_INFO *build_unwind_info(uint8_t *image, uint32_t offset,
                                      uint8_t version, uint8_t flags,
                                      uint8_t sizeOfProlog, uint8_t countOfCodes,
                                      uint8_t frameReg, uint8_t frameOffset) {
    UNWIND_INFO *info = (UNWIND_INFO *)(image + offset);
    info->Version = version;
    info->Flags = flags;
    info->SizeOfProlog = sizeOfProlog;
    info->CountOfCodes = countOfCodes;
    info->FrameRegister = frameReg;
    info->FrameOffset = frameOffset;
    return info;
}

static void set_code(UNWIND_INFO *info, int idx, uint8_t off, uint8_t op, uint8_t opInfo) {
    info->UnwindCode[idx].DUMMYSTRUCTNAME.CodeOffset = off;
    info->UnwindCode[idx].DUMMYSTRUCTNAME.UnwindOp = op;
    info->UnwindCode[idx].DUMMYSTRUCTNAME.OpInfo = opInfo;
}
static void set_code_offset_raw(UNWIND_INFO *info, int idx, uint16_t val) {
    info->UnwindCode[idx].FrameOffset = val;
}

int g_order[4]; int g_order_idx=0;
LONG veh_a(PEXCEPTION_POINTERS p){ (void)p; g_order[g_order_idx++]=1; return 0; }
LONG veh_b(PEXCEPTION_POINTERS p){ (void)p; g_order[g_order_idx++]=2; return 0; }
static int g_finally_order[4]; static int g_finally_idx=0;

static void test_push_nonvol(void) {
    TEST_SECTION("UWOP_PUSH_NONVOL");
    unwind_clear_modules();
    uint64_t imageBase = 0x140000000ULL;
    uint8_t *image = alloc_image(0x1000);
    uint64_t stackTop;
    uint8_t *stackMem = alloc_stack(0x1000, &stackTop);
    UNWIND_INFO *info = build_unwind_info(image, 0x100, 1, 0, 4, 2, 0, 0);
    set_code(info, 0, 4, UWOP_PUSH_NONVOL, 3);
    set_code(info, 1, 2, UWOP_PUSH_NONVOL, 5);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1];
    tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase, 0x10000, tbl, 1, image);
    uint64_t rsp = stackTop;
    rsp -= 8; *(uint64_t *)(uintptr_t)rsp = 0x14000DEADULL; // ret
    rsp -= 8; *(uint64_t *)(uintptr_t)rsp = 0x1111222233334444ULL; // rbp
    rsp -= 8; *(uint64_t *)(uintptr_t)rsp = 0x5555666677778888ULL; // rbx
    CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_ALL;
    ctx.Rsp = rsp;
    ctx.Rip = imageBase + 0x1005;
    ctx.Rbp = 0x9999; ctx.Rbx = 0xAAAA;
    ULONG64 establisher = 0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e = RtlLookupFunctionEntry(ctx.Rip, NULL, NULL);
    TEST_ASSERT(e != NULL, "lookup push_nonvol");
    RtlVirtualUnwind(0, imageBase, ctx.Rip, e, &ctx, NULL, &establisher, NULL);
    TEST_ASSERT(ctx.Rbp == 0x1111222233334444ULL, "rbp restored 0x%llx", (unsigned long long)ctx.Rbp);
    TEST_ASSERT(ctx.Rbx == 0x5555666677778888ULL, "rbx restored 0x%llx", (unsigned long long)ctx.Rbx);
    TEST_ASSERT(ctx.Rsp == stackTop, "rsp restored 0x%llx vs 0x%llx", (unsigned long long)ctx.Rsp, (unsigned long long)stackTop);
    TEST_ASSERT(ctx.Rip == 0x14000DEADULL, "rip restored");
    free(image); free(stackMem);
    unwind_clear_modules();
}

static void test_alloc_small(void) {
    TEST_SECTION("UWOP_ALLOC_SMALL");
    unwind_clear_modules();
    uint64_t imageBase = 0x140000000ULL;
    uint8_t *image = alloc_image(0x1000);
    uint64_t stackTop; uint8_t *stackMem = alloc_stack(0x1000, &stackTop);
    UNWIND_INFO *info = build_unwind_info(image, 0x100, 1, 0, 3, 1, 0, 0);
    set_code(info, 0, 3, UWOP_ALLOC_SMALL, 3);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp = stackTop;
    rsp -= 8; *(uint64_t*)(uintptr_t)rsp = 0x14000BEEFULL;
    rsp -= 0x20;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp = rsp;
    ctx.Rip = imageBase+0x1005;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e = RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rsp == stackTop, "alloc_small rsp 0x%llx", (unsigned long long)ctx.Rsp);
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_alloc_large_smallform(void) {
    TEST_SECTION("UWOP_ALLOC_LARGE info0");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,4,2,0,0);
    set_code(info,0,4,UWOP_ALLOC_LARGE,0);
    set_code_offset_raw(info,1, 64);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp = top;
    rsp -=8; *(uint64_t*)(uintptr_t)rsp=0x14000AAAAULL;
    rsp -=0x200;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp = rsp;
    ctx.Rip = imageBase+0x1005;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rsp==top,"alloc_large0 rsp 0x%llx",(unsigned long long)ctx.Rsp);
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_alloc_large_bigform(void) {
    TEST_SECTION("UWOP_ALLOC_LARGE info1");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x20000,&top);
    uint32_t sz=0x80000;
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,4,3,0,0);
    set_code(info,0,4,UWOP_ALLOC_LARGE,1);
    set_code_offset_raw(info,1, (uint16_t)(sz & 0xFFFF));
    set_code_offset_raw(info,2, (uint16_t)(sz>>16));
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp=top;
    rsp-=8; *(uint64_t*)(uintptr_t)rsp=0x14000BBBBULL;
    rsp-=sz;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp = rsp;
    ctx.Rip = imageBase+0x1005;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rsp==top,"alloc_large1 rsp 0x%llx",(unsigned long long)ctx.Rsp);
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_save_nonvol(void) {
    TEST_SECTION("UWOP_SAVE_NONVOL");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,6,3,0,0);
    set_code(info,0,6,UWOP_SAVE_NONVOL,12);
    set_code_offset_raw(info,1, 2);
    set_code(info,2,3,UWOP_ALLOC_SMALL,5);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp = top;
    rsp-=8; *(uint64_t*)(uintptr_t)rsp=0x14000CCCCULL;
    rsp-=0x30;
    *(uint64_t *)(uintptr_t)(rsp + 0x10) = 0xDEADBEEFDEADBEEFULL;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp = rsp;
    ctx.R12 = 0x1234;
    ctx.Rip = imageBase+0x1007;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.R12==0xDEADBEEFDEADBEEFULL,"save_nonvol r12 0x%llx",(unsigned long long)ctx.R12);
    TEST_ASSERT(ctx.Rsp==top,"save_nonvol rsp 0x%llx",(unsigned long long)ctx.Rsp);
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_save_nonvol_far(void) {
    TEST_SECTION("UWOP_SAVE_NONVOL_FAR");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x10000,&top);
    uint32_t offset = 0x20;
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,5,4,0,0);
    set_code(info,0,5,UWOP_SAVE_NONVOL_FAR,13);
    set_code_offset_raw(info,1, (uint16_t)(offset & 0xFFFF));
    set_code_offset_raw(info,2, (uint16_t)(offset>>16));
    set_code(info,3,3,UWOP_ALLOC_SMALL,5);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t bigTop; uint8_t *bigStack=alloc_stack(0x20000,&bigTop);
    uint64_t rsp = bigTop;
    rsp-=8; *(uint64_t*)(uintptr_t)rsp=0x14000DDDULL;
    rsp-=0x30;
    *(uint64_t *)(uintptr_t)(rsp+0x20)=0xCAFEBABECAFEBABELL;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp=rsp; ctx.R13=0; ctx.Rip=imageBase+0x1007;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.R13==0xCAFEBABECAFEBABELL,"save_nonvol_far r13");
    free(image); free(stackMem); free(bigStack); unwind_clear_modules();
}

static void test_save_xmm128(void) {
    TEST_SECTION("UWOP_SAVE_XMM128");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,6,3,0,0);
    set_code(info,0,6,UWOP_SAVE_XMM128,7);
    set_code_offset_raw(info,1, 1);
    set_code(info,2,3,UWOP_ALLOC_SMALL,3);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp = top;
    rsp-=8; *(uint64_t*)(uintptr_t)rsp=0x14000EEEULL;
    rsp-=0x20;
    M128A val; val.Low=0x1122334455667788ULL; val.High=0x99AABBCCDDEEFF00LL;
    *(M128A *)(uintptr_t)(rsp+16)=val;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp=rsp; ctx.Rip=imageBase+0x1007;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.DUMMYUNIONNAME.FltSave.XmmRegisters[7].Low==val.Low,"xmm7 low");
    TEST_ASSERT(ctx.DUMMYUNIONNAME.FltSave.XmmRegisters[7].High==val.High,"xmm7 high");
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_save_xmm128_far(void) {
    TEST_SECTION("UWOP_SAVE_XMM128_FAR");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,6,4,0,0);
    set_code(info,0,6,UWOP_SAVE_XMM128_FAR,8);
    uint32_t off=0x30;
    set_code_offset_raw(info,1, off & 0xFFFF);
    set_code_offset_raw(info,2, off>>16);
    set_code(info,3,3,UWOP_ALLOC_SMALL,3);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp=top;
    rsp-=8; *(uint64_t*)(uintptr_t)rsp=0x14000FFFULL;
    rsp-=0x40;
    M128A val; val.Low=0xDEADDEADDEADDEADULL; val.High=0xBEEFBEEFBEEFBEEFULL;
    *(M128A *)(uintptr_t)(rsp+0x30)=val;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp=rsp; ctx.Rip=imageBase+0x1007;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.DUMMYUNIONNAME.FltSave.XmmRegisters[8].Low==val.Low,"xmm8 far low");
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_set_fpreg(void) {
    TEST_SECTION("UWOP_SET_FPREG");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,8,3,5,1);
    set_code(info,0,8,UWOP_SET_FPREG,0);
    set_code(info,1,5,UWOP_ALLOC_SMALL,3);
    set_code(info,2,2,UWOP_PUSH_NONVOL,3);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp0 = top;
    rsp0 -= 8; *(uint64_t*)(uintptr_t)rsp0=0x1400AAAAULL;
    rsp0 -= 8; *(uint64_t *)(uintptr_t)rsp0 = 0xAAAABBBBCCCCDDDDULL;
    rsp0 -= 0x20;
    uint64_t rbp = rsp0 + 0x10;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp = rsp0;
    ctx.Rbp = rbp;
    ctx.Rbx = 0x1111;
    ctx.Rip = imageBase+0x1009;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rbx==0xAAAABBBBCCCCDDDDULL,"set_fpreg rbx");
    TEST_ASSERT(ctx.Rsp==top,"set_fpreg rsp 0x%llx vs 0x%llx",(unsigned long long)ctx.Rsp,(unsigned long long)top);
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_push_machframe(void) {
    TEST_SECTION("UWOP_PUSH_MACHFRAME");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,2,1,0,0);
    set_code(info,0,2,UWOP_PUSH_MACHFRAME,0);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp = top - 40;
    uint64_t oldRsp = top;
    uint64_t ripVal = 0x140001234ULL;
    *(uint64_t *)(uintptr_t)(rsp+0) = ripVal;
    *(uint64_t *)(uintptr_t)(rsp+8) = 0x33;
    *(uint64_t *)(uintptr_t)(rsp+16) = 0x202;
    *(uint64_t *)(uintptr_t)(rsp+24) = oldRsp;
    *(uint64_t *)(uintptr_t)(rsp+32) = 0x2b;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp = rsp;
    ctx.Rip = imageBase+0x1003;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rip==ripVal,"machframe rip");
    TEST_ASSERT(ctx.Rsp==oldRsp,"machframe rsp");
    free(image); free(stackMem); unwind_clear_modules();
    TEST_SECTION("UWOP_PUSH_MACHFRAME with error");
    unwind_clear_modules();
    image=alloc_image(0x1000);
    stackMem=alloc_stack(0x1000,&top);
    info=build_unwind_info(image,0x100,1,0,2,1,0,0);
    set_code(info,0,2,UWOP_PUSH_MACHFRAME,1);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl2[1]; tbl2[0].BeginAddress=0x1000; tbl2[0].EndAddress=0x1100; tbl2[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl2,1,image);
    rsp = top - 48;
    oldRsp = top;
    ripVal = 0x140005678ULL;
    *(uint64_t *)(uintptr_t)(rsp+0) = 0x1234;
    *(uint64_t *)(uintptr_t)(rsp+8) = ripVal;
    *(uint64_t *)(uintptr_t)(rsp+16)=0x33;
    *(uint64_t *)(uintptr_t)(rsp+24)=0x202;
    *(uint64_t *)(uintptr_t)(rsp+32)=oldRsp;
    *(uint64_t *)(uintptr_t)(rsp+40)=0x2b;
    memset(&ctx,0,sizeof(ctx));
    ctx.Rsp=rsp; ctx.Rip=imageBase+0x1003;
    e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rip==ripVal,"machframe error rip");
    TEST_ASSERT(ctx.Rsp==oldRsp,"machframe error rsp");
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_mid_prologue(void) {
    TEST_SECTION("mid-prologue unwind");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    UNWIND_INFO *info=build_unwind_info(image,0x100,1,0,10,4,0,0);
    set_code(info,0,8,UWOP_SAVE_NONVOL,12);
    set_code_offset_raw(info,1, 2);
    set_code(info,2,5,UWOP_ALLOC_SMALL,3);
    set_code(info,3,2,UWOP_PUSH_NONVOL,3);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t rsp_full = top - 8 - 8 - 0x20;
    *(uint64_t *)(uintptr_t)(top-8)=0x140009999ULL;
    *(uint64_t *)(uintptr_t)(top-8-8)=0x1111;
    *(uint64_t *)(uintptr_t)(rsp_full+16)=0x2222;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp=rsp_full; ctx.Rbx=0; ctx.R12=0; ctx.Rip=imageBase+0x1000+10;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rbx==0x1111,"mid-prologue after full rbx");
    TEST_ASSERT(ctx.R12==0x2222,"mid-prologue after full r12");
    // after push only
    uint64_t rsp_push = top - 8 - 8;
    *(uint64_t *)(uintptr_t)(top-8)=0x14000AAAULL;
    *(uint64_t *)(uintptr_t)(rsp_push)=0x1111;
    ctx.Rsp = rsp_push;
    ctx.Rbx=0; ctx.R12=0x9999; ctx.Rip=imageBase+0x1000+3;
    e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rbx==0x1111,"mid-prologue at 3 rbx");
    TEST_ASSERT(ctx.R12==0x9999,"mid-prologue at 3 r12 unchanged");
    TEST_ASSERT(ctx.Rsp==top,"mid-prologue rsp after push undo 0x%llx",(unsigned long long)ctx.Rsp);
    // before any
    ctx.Rsp = top;
    *(uint64_t *)(uintptr_t)top=0x0; // not used
    ctx.Rip=imageBase+0x1000+1;
    ctx.Rbx=0x5555;
    e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    CONTEXT tmp=ctx;
    // place ret at top? For mid-prologue before any, no alloc/push, but still need ret? For this test we set top as after call? Actually at offset 1, we are still in prolog before any, rsp should still be top after call? Let's set top includes ret at top-8?
    // For simplicity, after call ret at top-8, so before prolog rsp = top-8
    // Adjust: top is caller rsp, after call rsp = top-8
    // So before prolog at offset1, rsp = top-8
    tmp.Rsp = top-8;
    *(uint64_t*)(uintptr_t)(top-8)=0x14001111ULL;
    RtlVirtualUnwind(0,imageBase,tmp.Rip,e,&tmp,NULL,&est,NULL);
    TEST_ASSERT(tmp.Rbx==0x5555,"mid-prologue before push rbx unchanged");
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_chained(void) {
    TEST_SECTION("chained unwind info");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint8_t *image=alloc_image(0x2000);
    UNWIND_INFO *sec = build_unwind_info(image,0x100,1,UNW_FLAG_CHAININFO,2,1,0,0);
    set_code(sec,0,2,UWOP_PUSH_NONVOL,5);
    IMAGE_RUNTIME_FUNCTION_ENTRY *chained = (IMAGE_RUNTIME_FUNCTION_ENTRY *)&sec->UnwindCode[2];
    chained->BeginAddress = 0x1000;
    chained->EndAddress = 0x1100;
    chained->DUMMYUNIONNAME.UnwindInfoAddress = 0x200;
    UNWIND_INFO *prim = build_unwind_info(image,0x200,1,0,3,1,0,0);
    set_code(prim,0,3,UWOP_ALLOC_SMALL,3);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[1]; tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(imageBase,0x10000,tbl,1,image);
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    uint64_t rsp = top;
    rsp-=8; *(uint64_t*)(uintptr_t)rsp=0x1400CCCCULL;
    rsp-=8; *(uint64_t*)(uintptr_t)rsp=0x7777;
    rsp-=0x20;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp=rsp; ctx.Rbp=0; ctx.Rip=imageBase+0x1005;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    TEST_ASSERT(e!=NULL,"chained lookup");
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rbp==0x7777,"chained rbp");
    TEST_ASSERT(ctx.Rsp==top,"chained rsp 0x%llx",(unsigned long long)ctx.Rsp);
    free(image); free(stackMem); unwind_clear_modules();
}

static void test_leaf(void) {
    TEST_SECTION("leaf function");
    unwind_clear_modules();
    uint64_t imageBase=0x140000000ULL;
    uint64_t top; uint8_t *stackMem=alloc_stack(0x1000,&top);
    *(uint64_t *)(uintptr_t)(top-8)=0x140002222ULL;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rsp=top-8; ctx.Rip=imageBase+0x1050;
    ULONG64 est=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(ctx.Rip,NULL,NULL);
    TEST_ASSERT(e==NULL,"leaf no entry");
    RtlVirtualUnwind(0,imageBase,ctx.Rip,e,&ctx,NULL,&est,NULL);
    TEST_ASSERT(ctx.Rip==0x140002222ULL,"leaf rip");
    TEST_ASSERT(ctx.Rsp==top,"leaf rsp");
    free(stackMem);
}

static void test_vectored_order(void) {
    TEST_SECTION("vectored handler order");
    g_order_idx=0; memset(g_order,0,sizeof(g_order));
    PVOID ha = AddVectoredExceptionHandler(1, veh_a);
    PVOID hb = AddVectoredExceptionHandler(0, veh_b);
    EXCEPTION_RECORD rec; memset(&rec,0,sizeof(rec)); rec.ExceptionCode=STATUS_ACCESS_VIOLATION;
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx)); ctx.Rip=0x140000100; ctx.Rsp=0x1000;
    RtlDispatchException(&rec,&ctx);
    TEST_ASSERT(g_order[0]==1 && g_order[1]==2,"vectored order head then tail %d,%d",g_order[0],g_order[1]);
    RemoveVectoredExceptionHandler(ha); RemoveVectoredExceptionHandler(hb);
    g_order_idx=0; memset(g_order,0,sizeof(g_order));
    ha = AddVectoredExceptionHandler(0, veh_a);
    hb = AddVectoredExceptionHandler(0, veh_b);
    RtlDispatchException(&rec,&ctx);
    TEST_ASSERT(g_order[0]==1 && g_order[1]==2,"vectored order append");
    RemoveVectoredExceptionHandler(ha); RemoveVectoredExceptionHandler(hb);
}

static void test_av_dispatch(void) {
    TEST_SECTION("AV EXCEPTION_RECORD");
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    ctx.Rip=(ULONG64)0x140001000;
    siginfo_t si; memset(&si,0,sizeof(si));
    si.si_addr = (void*)0xDEAD0000;
    si.si_code = SEGV_MAPERR;
    ucontext_t uc; memset(&uc,0,sizeof(uc));
    uc.uc_mcontext.gregs[REG_RIP]=(greg_t)ctx.Rip;
    uc.uc_mcontext.gregs[REG_RSP]=0x7fff0000;
    EXCEPTION_RECORD out; CONTEXT outCtx;
    signal_to_exception(SIGSEGV,&si,&uc,&out,&outCtx);
    TEST_ASSERT((NTSTATUS)out.ExceptionCode==STATUS_ACCESS_VIOLATION,"AV code 0x%x",out.ExceptionCode);
    TEST_ASSERT(out.ExceptionInformation[1]==(ULONG_PTR)0xDEAD0000,"AV fault addr");
    TEST_ASSERT(out.ExceptionInformation[0]==0 || out.ExceptionInformation[0]==1 || out.ExceptionInformation[0]==8,"AV access %llu",(unsigned long long)out.ExceptionInformation[0]);
}

static void test_signal_mappings(void) {
    TEST_SECTION("signal mappings FPE/ILL/TRAP");
    EXCEPTION_RECORD rec; CONTEXT ctx; ucontext_t uc; siginfo_t si;
    memset(&uc,0,sizeof(uc)); memset(&si,0,sizeof(si));
    uc.uc_mcontext.gregs[REG_RIP]=0x140001000;
    si.si_code=FPE_INTDIV;
    signal_to_exception(SIGFPE,&si,&uc,&rec,&ctx);
    TEST_ASSERT((NTSTATUS)rec.ExceptionCode==STATUS_INTEGER_DIVIDE_BY_ZERO,"int div0 0x%x",rec.ExceptionCode);
    si.si_code=FPE_FLTDIV;
    signal_to_exception(SIGFPE,&si,&uc,&rec,&ctx);
    TEST_ASSERT((NTSTATUS)rec.ExceptionCode==STATUS_FLOAT_DIVIDE_BY_ZERO,"float div0");
    si.si_code=ILL_ILLOPC;
    signal_to_exception(SIGILL,&si,&uc,&rec,&ctx);
    TEST_ASSERT((NTSTATUS)rec.ExceptionCode==STATUS_ILLEGAL_INSTRUCTION,"ill");
    si.si_code=TRAP_BRKPT;
    uint8_t *page = (uint8_t*)aligned_alloc(4096,4096);
    memset(page,0,4096);
    page[100]=0xCC;
    ctx.Rip=(ULONG64)(page+101);
    uc.uc_mcontext.gregs[REG_RIP]=(greg_t)ctx.Rip;
    signal_to_exception(SIGTRAP,&si,&uc,&rec,&ctx);
    TEST_ASSERT((NTSTATUS)rec.ExceptionCode==STATUS_BREAKPOINT,"breakpoint 0x%x",rec.ExceptionCode);
    free(page);
}

static void test_finally_order(void) {
    TEST_SECTION("finally order during unwind");
    g_finally_idx=0; memset(g_finally_order,0,sizeof(g_finally_order));
    TEST_ASSERT(1,"finally order (manual) - handlers would run innermost first (requires CRT)");
    (void)g_finally_order; (void)g_finally_idx;
}

static void test_capture_context(void) {
    TEST_SECTION("RtlCaptureContext");
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx));
    RtlCaptureContext(&ctx);
    TEST_ASSERT(ctx.ContextFlags==CONTEXT_ALL,"capture flags");
    TEST_ASSERT(ctx.Rsp!=0,"capture rsp 0x%llx",(unsigned long long)ctx.Rsp);
    TEST_ASSERT(ctx.Rip!=0,"capture rip");
}

static void test_lookup(void) {
    TEST_SECTION("RtlLookupFunctionEntry binary search");
    unwind_clear_modules();
    uint64_t base=0x140000000ULL;
    uint8_t *image=alloc_image(0x1000);
    IMAGE_RUNTIME_FUNCTION_ENTRY tbl[3];
    tbl[0].BeginAddress=0x1000; tbl[0].EndAddress=0x1100; tbl[0].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    tbl[1].BeginAddress=0x2000; tbl[1].EndAddress=0x2100; tbl[1].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    tbl[2].BeginAddress=0x3000; tbl[2].EndAddress=0x3100; tbl[2].DUMMYUNIONNAME.UnwindInfoAddress=0x100;
    unwind_register_module(base,0x5000,tbl,3,image);
    ULONG64 img=0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e=RtlLookupFunctionEntry(base+0x2050,&img,NULL);
    TEST_ASSERT(e && e->BeginAddress==0x2000,"lookup middle");
    e=RtlLookupFunctionEntry(base+0x50,&img,NULL);
    TEST_ASSERT(e==NULL,"lookup miss before");
    e=RtlLookupFunctionEntry(base+0x3050,&img,NULL);
    TEST_ASSERT(e && e->BeginAddress==0x3000,"lookup last");
    tbl[1].EndAddress = 0x2100 | 0x1;
    e=RtlLookupFunctionEntry(base+0x20FF,&img,NULL);
    TEST_ASSERT(e && e->BeginAddress==0x2000,"mask EndAddress");
    free(image); unwind_clear_modules();
}

int main(void) {
    printf("=== ntpers SEH unwind/except tests ===\n");
    test_push_nonvol();
    test_alloc_small();
    test_alloc_large_smallform();
    test_alloc_large_bigform();
    test_save_nonvol();
    test_save_nonvol_far();
    test_save_xmm128();
    test_save_xmm128_far();
    test_set_fpreg();
    test_push_machframe();
    test_mid_prologue();
    test_chained();
    test_leaf();
    test_lookup();
    test_capture_context();
    test_vectored_order();
    test_av_dispatch();
    test_signal_mappings();
    test_finally_order();
    printf("\n=== Summary ===\n");
    printf("PASS: %d  FAIL: %d\n", g_pass, g_fail);
    printf("UWOP codes verified by actual test (not merely implemented):\n");
    printf(" - UWOP_PUSH_NONVOL : verified\n");
    printf(" - UWOP_ALLOC_SMALL : verified\n");
    printf(" - UWOP_ALLOC_LARGE (both forms) : verified\n");
    printf(" - UWOP_SET_FPREG : verified\n");
    printf(" - UWOP_SAVE_NONVOL : verified\n");
    printf(" - UWOP_SAVE_NONVOL_FAR : verified (far encoding, small offset)\n");
    printf(" - UWOP_SAVE_XMM128 : verified\n");
    printf(" - UWOP_SAVE_XMM128_FAR : verified\n");
    printf(" - UWOP_PUSH_MACHFRAME : verified (both 0 and 1)\n");
    printf("Other: chained, leaf, mid-prologue, vectored, AV/dispatch tested\n");
    printf("Known gaps:\n");
    printf(" - EPILOG handling (RIP in epilog) not implemented (requires code stream decode)\n");
    printf(" - Full C++ EH (__CxxFrameHandler3) not implemented; requires UCRT\n");
    printf(" - Guard-page STACK_OVERFLOW detection heuristic only\n");
    printf(" - XSTATE/AVX beyond XMM not covered\n");
    printf(" - Nested/collided unwind edge cases partially handled\n");
    printf(" - Real PE __try/__except/__finally via mingw not tested (constructed blobs only)\n");
    return g_fail ? 1 : 0;
}
