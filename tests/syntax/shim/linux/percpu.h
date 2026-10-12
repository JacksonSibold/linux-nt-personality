/*
 * linux/percpu.h - syntax-check stub.
 * Per-CPU variables are modelled as plain variables and per_cpu() reads the
 * single instance. This type-checks the call sites only; it is not a
 * per-CPU model.
 */
#ifndef SYNTAX_SHIM_LINUX_PERCPU_H
#define SYNTAX_SHIM_LINUX_PERCPU_H

#include <linux/types.h>

#define DEFINE_PER_CPU(type, name) type name
#define per_cpu(var, cpu) ((void)(cpu), (var))
#define this_cpu_inc(pcp) ((pcp)++)
#define __this_cpu_add(pcp, val) ((pcp) += (val))
#define this_cpu_add(pcp, val) ((pcp) += (val))
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < 1; (cpu)++)

#endif
