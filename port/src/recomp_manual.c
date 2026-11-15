#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

extern ptrdiff_t g_xbox_mem_offset;

#include "recomp/gen/recomp_funcs.h"

static void math_pop2_zero(void)
{
    g_fp_top = (g_fp_top + 1u) & 7u;
    sub_0014FB3A();
}

static void math_pop2_one(void)
{
    g_fp_top = (g_fp_top + 1u) & 7u;
    sub_0014FB41();
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    switch (xbox_va) {
    case 0x0014FB38u: return math_pop2_zero;
    case 0x0014FB3Fu: return math_pop2_one;
    }
    return (recomp_func_t)0;
}

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;

    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (hits[i] <= 10 && g_xbox_mem_offset) {
        uint32_t esp = (uint32_t)g_esp, k, shown = 0;
        int j;
        fprintf(stderr, "  this %08X, last calls:", (uint32_t)g_ecx);
        for (j = 12; j < 16; j++) {
            uint32_t t = g_icall_trace[(g_icall_trace_idx - 16 + (uint32_t)j) & 15];
            if (t) fprintf(stderr, " 0x%08X", t);
        }
        fprintf(stderr, "\n  guest return addresses:");
        if (esp >= 0x10000u && esp < 0x04000000u - 0x400u)
            for (k = 0; k < 128u && shown < 8u; k++) {
                uint32_t v = *(volatile uint32_t *)((uintptr_t)(esp + 4u * k) + (uintptr_t)g_xbox_mem_offset);
                if (v >= 0x00011000u && v < 0x00170000u) {
                    fprintf(stderr, " 0x%08X", v);
                    shown++;
                }
            }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

