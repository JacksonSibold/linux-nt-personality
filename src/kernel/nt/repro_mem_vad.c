#include "mm.h"
#include "kshim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
    ntmm_reset();
    PVOID base=NULL;
    SIZE_T size=0x100000;
    NTSTATUS st=NtAllocateVirtualMemory((HANDLE)-1,&base,0,&size,MEM_RESERVE,PAGE_READWRITE);
    if(st!=STATUS_SUCCESS){printf("reserve failed 0x%lx\n",(unsigned long)st); return 1;}
    printf("reserved base=%p size=0x%zx\n",base,(size_t)size);
    unsigned char state[256]; memset(state,0,sizeof(state));
    PVOID rbase=base;
    for(int i=0;i<4096;i++){
        int page=i&0xFF;
        unsigned char s=state[page];
        ULONG_PTR addr=(ULONG_PTR)rbase + page*0x1000;
        NTSTATUS rc=0;
        if(!ntmm_check_invariants(&g_nt_mm)){ printf("INVARIANT FAIL BEFORE iter %d page %d state %d\n",i,page,s); return 2;}
        if(s==0){
            PVOID b=(PVOID)addr; SIZE_T sz=0x1000;
            rc=NtAllocateVirtualMemory((HANDLE)-1,&b,0,&sz,MEM_COMMIT,PAGE_READWRITE);
            if(rc!=STATUS_SUCCESS){ printf("FAIL commit iter %d page %d rc=0x%lx\n",i,page,(unsigned long)rc); 
                if(!ntmm_check_invariants(&g_nt_mm)) printf(" invariants broken\n");
                MEMORY_BASIC_INFORMATION mbi; SIZE_T ret; NtQueryVirtualMemory((HANDLE)-1,(PVOID)addr,MemoryBasicInformation,&mbi,sizeof(mbi),&ret);
                printf(" query base=%p mbi: Base=%p Size=0x%zx State=0x%lx Prot=0x%lx AllocBase=%p\n",(void*)addr,mbi.BaseAddress,(size_t)mbi.RegionSize,(unsigned long)mbi.State,(unsigned long)mbi.Protect,mbi.AllocationBase);
                return 10;
            }
            if(b!=(PVOID)addr||sz!=0x1000){ printf("commit base/size mismatch iter %d\n",i); return 3;}
            state[page]=1;
        } else if(s==1){
            PVOID b=(PVOID)addr; SIZE_T sz=0x1000; ULONG old=0;
            rc=NtProtectVirtualMemory((HANDLE)-1,&b,&sz,PAGE_READONLY,&old);
            if(rc!=STATUS_SUCCESS){ printf("FAIL protect iter %d page %d rc=0x%lx old=0x%lx\n",i,page,(unsigned long)rc,(unsigned long)old); return 11; }
            state[page]=2;
        } else {
            PVOID b=(PVOID)addr; SIZE_T sz=0x1000;
            rc=NtFreeVirtualMemory((HANDLE)-1,&b,&sz,MEM_DECOMMIT);
            if(rc!=STATUS_SUCCESS){ printf("FAIL decommit iter %d page %d rc=0x%lx\n",i,page,(unsigned long)rc); return 12; }
            state[page]=0;
        }
        MEMORY_BASIC_INFORMATION mbi; SIZE_T ret=0;
        rc=NtQueryVirtualMemory((HANDLE)-1,(PVOID)addr,MemoryBasicInformation,&mbi,sizeof(mbi),&ret);
        if(rc!=STATUS_SUCCESS){ printf("FAIL query iter %d page %d rc=0x%lx\n",i,page,(unsigned long)rc); return 13;}
        ULONG expectedState = (state[page]==0 ? MEM_RESERVE : MEM_COMMIT);
        if(mbi.State!=expectedState){ printf("MISMATCH state iter %d page %d got 0x%lx exp 0x%lx\n",i,page,(unsigned long)mbi.State,(unsigned long)expectedState); return 14; }
        if(!ntmm_check_invariants(&g_nt_mm)){
            printf("INVARIANT FAIL AFTER iter %d page %d state %d new_state %d\n",i,page,s,state[page]);
            ULONG_PTR cur=NTMM_LOW_ADDRESS; int steps=0;
            while(cur < NTMM_HIGH_ADDRESS && steps<500){
                MEMORY_BASIC_INFORMATION q; SIZE_T rl;
                NtQueryVirtualMemory((HANDLE)-1,(PVOID)cur,MemoryBasicInformation,&q,sizeof(q),&rl);
                printf("  VAD: Base=%p AllocBase=%p Size=0x%zx State=0x%lx Prot=0x%lx Type=0x%lx\n",q.BaseAddress,q.AllocationBase,(size_t)q.RegionSize,(unsigned long)q.State,(unsigned long)q.Protect,(unsigned long)q.Type);
                if(q.RegionSize==0) break;
                cur=(ULONG_PTR)q.BaseAddress+q.RegionSize;
                steps++;
                if(cur>(ULONG_PTR)rbase+0x200000) break;
            }
            return 5;
        }
    }
    printf("4096 iterations PASS\n");
    // Verify all pages after loop
    for(int p=0;p<256;p++){
        ULONG_PTR addr=(ULONG_PTR)rbase + p*0x1000;
        MEMORY_BASIC_INFORMATION mbi; SIZE_T ret;
        NtQueryVirtualMemory((HANDLE)-1,(PVOID)addr,MemoryBasicInformation,&mbi,sizeof(mbi),&ret);
        ULONG expected = (state[p]==0 ? MEM_RESERVE : MEM_COMMIT);
        if(mbi.State != expected){
            printf("FINAL MISMATCH page %d state %d got 0x%lx exp 0x%lx Base %p Size 0x%zx\n",p,state[p],(unsigned long)mbi.State,(unsigned long)expected,mbi.BaseAddress,(size_t)mbi.RegionSize);
            return 30;
        }
    }
    printf("FINAL VERIFY PASS\n");
    // Dump all VADs
    {
        ULONG_PTR cur2 = NTMM_LOW_ADDRESS;
        int steps=0;
        while(cur2 < NTMM_HIGH_ADDRESS && steps<20){
            MEMORY_BASIC_INFORMATION q; SIZE_T r;
            NtQueryVirtualMemory((HANDLE)-1,(PVOID)cur2,MemoryBasicInformation,&q,sizeof(q),&r);
            printf("VAD [%p-%p) state 0x%lx prot 0x%lx alloc %p size 0x%zx\n", q.BaseAddress, (void*)((ULONG_PTR)q.BaseAddress+q.RegionSize), (unsigned long)q.State, (unsigned long)q.Protect, q.AllocationBase, (size_t)q.RegionSize);
            if(q.RegionSize==0) break;
            cur2 = (ULONG_PTR)q.BaseAddress + q.RegionSize;
            steps++;
            if(cur2 > (ULONG_PTR)rbase+0x200000) break;
        }
    }
    {
        PVOID b=rbase; SIZE_T sz=0x1000;
        NTSTATUS rc=NtFreeVirtualMemory((HANDLE)-1,&b,&sz,MEM_RELEASE);
        if(rc!=STATUS_INVALID_PARAMETER){ printf("NEG1 fail expected INVALID_PARAM got 0x%lx\n",(unsigned long)rc); return 20; }
    }
    {
        PVOID b=(PVOID)((ULONG_PTR)rbase+0x1000); SIZE_T sz=0;
        NTSTATUS rc=NtFreeVirtualMemory((HANDLE)-1,&b,&sz,MEM_RELEASE);
        if(rc==STATUS_SUCCESS){ printf("NEG2 should not succeed\n"); return 21; }
    }
    {
        int found=-1; for(int p=0;p<256;p++) if(state[p]==0){found=p;break;}
        if(found>=0){
            PVOID b=(PVOID)((ULONG_PTR)rbase+found*0x1000); SIZE_T sz=0x1000;
            NTSTATUS rc=NtFreeVirtualMemory((HANDLE)-1,&b,&sz,MEM_DECOMMIT);
            if(rc==STATUS_SUCCESS){ printf("NEG3 decommit reserve should fail page %d got success\n",found); return 22; }
        }
    }
    {
        PVOID b=rbase; SIZE_T sz=0;
        NTSTATUS rc=NtFreeVirtualMemory((HANDLE)-1,&b,&sz,MEM_RELEASE);
        if(rc!=STATUS_SUCCESS){ printf("final release failed 0x%lx\n",(unsigned long)rc); return 23;}
        MEMORY_BASIC_INFORMATION mbi; SIZE_T ret;
        rc=NtQueryVirtualMemory((HANDLE)-1,rbase,MemoryBasicInformation,&mbi,sizeof(mbi),&ret);
        if(rc!=STATUS_SUCCESS){ printf("query after release fail\n"); return 24; }
        if(mbi.State!=MEM_FREE){ printf("after release state 0x%lx exp MEM_FREE\n",(unsigned long)mbi.State); return 25; }
    }
    printf("ALL PASS including negatives\n");
    return 0;
}
