/* pmu_elf.c - ELF twin for PMU comparison, same loop as pmu_probe.c */
#include <unistd.h>
int main() {
    volatile unsigned long long sum = 0;
    for (unsigned long long i=0;i<10000000ULL;i++) sum += i;
    if (sum==0) return 1;
    return 0;
}
