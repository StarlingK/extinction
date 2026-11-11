#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <xbox/xboxrecomp.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;

extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

#define AVP_ENTRY_POINT   0x001418E8
#define AVP_XBE_PATH      "game\\default.xbe"
#define AVP_GAME_DIR            "game"

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

extern void xbe_entry_point(void);

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern ptrdiff_t g_xbox_mem_offset;
static DWORD WINAPI avp_semaphore_watch(LPVOID unused)
{
    (void)unused;
    for (;;) {
        uint32_t dev = *(volatile uint32_t *)(uintptr_t)((ptrdiff_t)0x001815E8 + g_xbox_mem_offset);
        if (dev) {
            uint32_t sem = *(volatile uint32_t *)(uintptr_t)((ptrdiff_t)dev + 0x30 + g_xbox_mem_offset);
            if (sem) {
                nv2a_pb_set_semaphore_target(sem);
                fprintf(stderr, "[AVP] D3D device 0x%08X, GPU semaphore at 0x%08X\n", dev, sem);
                fflush(stderr);
                return 0;
            }
        }
        Sleep(1);
    }
}

static void avp_dump_stack(const char *name, uint32_t top, uint32_t bytes)
{
    uint32_t a;
    fprintf(stderr, "[AVPSTACK] %s\n", name);
    for (a = top - bytes; a < top; a += 4) {
        uint32_t w = *(volatile uint32_t *)(uintptr_t)((ptrdiff_t)a + g_xbox_mem_offset);
        if (w >= 0x00010000u && w < 0x00200000u)
            fprintf(stderr, "GS %08X %08X\n", a, w);
    }
}
#define AVP_G32(a) (*(volatile uint32_t *)(uintptr_t)((ptrdiff_t)(uint32_t)(a) + g_xbox_mem_offset))
extern bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4]);

static DWORD WINAPI avp_xmv_probe(LPVOID unused)
{
    static uint32_t regs[0x1000 / 4];
    (void)unused;
    if (!getenv("AVP_XMV_PROBE")) return 0;
    for (;;) {
        uint32_t ret, o, dev;
        Sleep(5000);
        dev = AVP_G32(0x001815E8);
        ret = AVP_G32(0x0319CCD0);

        o = (ret == 0x00120EE1u) ? AVP_G32(0x01422600u + 0x200u) : 0;
        fprintf(stderr, "[XMVP] ret=%08X obj=%08X dev: vblank=%u 19bc=%08X 1ff0=%u mmio=%08X\n",
                ret, o, dev ? AVP_G32(dev + 0x19C8) : 0, dev ? AVP_G32(dev + 0x19BC) : 0,
                dev ? AVP_G32(dev + 0x1FF0) : 0, dev ? AVP_G32(dev + 0x51C) : 0);
        if (ret == 0x00120EE1u && o >= 0x00010000u && o < 0x04000000u) {
            uint32_t n = AVP_G32(o + 0x48), st = AVP_G32(o + 0x148), i;
            fprintf(stderr, "[XMVP]  flags18=%08X left3c=%u have40=%u next50=%u pk60=%08X pk64=%08X pk68=%08X pk6c=%08X done74=%u\n",
                    AVP_G32(o + 0x18), AVP_G32(o + 0x3C), AVP_G32(o + 0x40), AVP_G32(o + 0x50),
                    AVP_G32(o + 0x60), AVP_G32(o + 0x64), AVP_G32(o + 0x68), AVP_G32(o + 0x6C), AVP_G32(o + 0x74));
            fprintf(stderr, "[XMVP]  start=%08X%08X vb0=%u rate=%u t_e4=%u e8=%u f8=%08X audio=%u st148=%08X:",
                    AVP_G32(o + 0xB4), AVP_G32(o + 0xB0), AVP_G32(o + 0xB8), AVP_G32(o + 0xBC),
                    AVP_G32(o + 0xE4), AVP_G32(o + 0xE8), AVP_G32(o + 0xF8), n, st);
            if (st >= 0x00010000u && st < 0x04000000u && n < 16)
                for (i = 0; i < n; i++) fprintf(stderr, " %08X", AVP_G32(st + i * 4));
            fprintf(stderr, "\n");
        }
        if (nv2a_hook_pvideo_snapshot(regs))
            fprintf(stderr, "[XMVP]  PVIDEO buffer=%08X stop=%08X base0=%08X off0=%08X size0=%08X fmt0=%08X out=%08X\n",
                    regs[0x700 / 4], regs[0x704 / 4], regs[0x900 / 4], regs[0x920 / 4],
                    regs[0x928 / 4], regs[0x958 / 4], regs[0x950 / 4]);
        fflush(stderr);
    }
}

static DWORD WINAPI avp_vblank_ack(LPVOID unused)
{
    typedef unsigned (WINAPI *tbp_t)(unsigned);
    HMODULE mm = LoadLibraryA("winmm.dll");
    tbp_t tbp = mm ? (tbp_t)GetProcAddress(mm, "timeBeginPeriod") : NULL;
    volatile LONG *pmc = (volatile LONG *)(uintptr_t)((ptrdiff_t)0xFD000100u + g_xbox_mem_offset);
    volatile LONG *pcrtc = (volatile LONG *)(uintptr_t)((ptrdiff_t)0xFD600100u + g_xbox_mem_offset);
    ULONGLONG up_since = 0;
    (void)unused;
    if (tbp) tbp(1);
    for (;;) {
        if (*pmc & 0x01000000) {
            ULONGLONG now = GetTickCount64();
            if (!up_since) {
                up_since = now;
            } else if (now - up_since >= 2) {
                InterlockedAnd(pcrtc, ~1L);
                InterlockedAnd(pmc, ~0x01000000L);
                up_since = 0;
            }
        } else {
            up_since = 0;
        }
        Sleep(1);
    }
}

volatile uint32_t g_avp_xmv_obj;

static int avp_has_foreground(void)
{
    DWORD pid = 0;
    HWND h = GetForegroundWindow();
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI avp_movie_skip(LPVOID unused)
{
    static const int keys[] = { VK_ESCAPE, VK_SPACE, VK_RETURN };
    const char *t = getenv("AVP_SKIP_TEST_MS");
    DWORD test_ms = t ? (DWORD)atoi(t) : 0;
    uint32_t seen = 0;
    ULONGLONG since = 0;
    int was_down = 0;
    (void)unused;
    for (;;) {
        uint32_t o = g_avp_xmv_obj;
        int down = 0, i, fire = 0;
        Sleep(10);
        for (i = 0; i < 3; i++)
            if (GetAsyncKeyState(keys[i]) & 0x8000) down = 1;
        if (down && !was_down && avp_has_foreground()) fire = 1;
        was_down = down;
        if (o != seen) { seen = o; since = GetTickCount64(); }
        if (test_ms && o && since && GetTickCount64() - since >= test_ms) {
            fire = 1;
            since = GetTickCount64();
        }
        if (fire && o) {
            *(volatile uint8_t *)(uintptr_t)((ptrdiff_t)o + 0x78 + g_xbox_mem_offset) = 1;
            fprintf(stderr, "[AVP] movie skip: decoder 0x%08X told to stop\n", o);
            fflush(stderr);
        }
    }
}

extern void (*volatile nv2a_sw_method_hook)(uint32_t);
extern void xbox_KeSetGuestEvent(uint32_t guest_va);

uint32_t avp_frame_target_bits(void)
{
    static int fps = -1;
    static uint32_t bits;
    if (fps < 0) {
        const char *e = getenv("AVP_FPS");
        float t;
        fps = e ? atoi(e) : 60;
        if (fps < 0 || fps > 1000)
            fps = 60;
        t = fps ? 1.0f / (float)fps : 0.0f;
        memcpy(&bits, &t, 4);
        fprintf(stderr, "[AVP] frame limiter: %s\n",
                fps ? "fixed target" : "game's own 32/49/65 ms steps");
    }
    return fps ? bits : 0;
}

uint32_t avp_multisample_type(void)
{
    return getenv("AVP_AA") ? 0x41121u : 0x11u;
}

static void avp_sw_method(uint32_t param)
{
    static volatile LONG seen[32];
    uint32_t kind = param & 0x1Fu;
    if (InterlockedIncrement(&seen[kind]) <= 3) {
        fprintf(stderr, "[AVP] software method %u (param 0x%08X)\n", kind, param);
        fflush(stderr);
    }
    if (kind == 5)
        xbox_KeSetGuestEvent(AVP_G32(0x001815E8) + 0x19ACu);
}

static char avp_log_buf[1 << 16];
static DWORD WINAPI avp_log_flusher(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(500);
        fflush(stderr);
    }
}
static void avp_log_setup(void)
{
    if (getenv("AVP_LOG_UNBUFFERED")) {
        setvbuf(stderr, NULL, _IONBF, 0);
        return;
    }
    setvbuf(stderr, avp_log_buf, _IOFBF, sizeof avp_log_buf);
    CreateThread(NULL, 0, avp_log_flusher, NULL, 0, NULL);
}

#include <tlhelp32.h>
#define AVP_PROF_BUCKETS (1u << 21)
#define AVP_PROF_THREADS 128
#define AVP_PROF_EXT 65536u
#define AVP_PROF_PER_THREAD 16384u
static uint64_t avp_ft(const FILETIME *f)
{
    return ((uint64_t)f->dwHighDateTime << 32) | f->dwLowDateTime;
}
static DWORD WINAPI avp_profiler(LPVOID unused)
{
    const char *s = getenv("AVP_PROFILE_SECS");
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint32_t *hist, *xcount;
    uintptr_t *xaddr;
    struct { DWORD tid; HANDLE h; uint32_t samples, inside; uint64_t u0, k0, cyc; uintptr_t *key; uint32_t *cnt; } th[AVP_PROF_THREADS];
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    uint32_t i;
    int nt = 0, start = 0, len = 0, k;
    ULONGLONG end, next_list = 0;
    (void)unused;
    if (!s || sscanf(s, "%d,%d", &start, &len) != 2) return 0;
    hist = (uint32_t *)calloc(AVP_PROF_BUCKETS, sizeof *hist);
    xcount = (uint32_t *)calloc(AVP_PROF_EXT, sizeof *xcount);
    xaddr = (uintptr_t *)calloc(AVP_PROF_EXT, sizeof *xaddr);
    if (!hist || !xcount || !xaddr) return 0;
    Sleep((DWORD)start * 1000);
    end = GetTickCount64() + (ULONGLONG)len * 1000;
    while (GetTickCount64() < end) {
        if (GetTickCount64() >= next_list) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                    for (k = 0; k < nt && th[k].tid != te.th32ThreadID; k++) {}
                    if (k == nt && nt < AVP_PROF_THREADS) {
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                        if (h) {
                            FILETIME c, e, ku, us;
                            th[nt].tid = te.th32ThreadID; th[nt].h = h;
                            th[nt].samples = th[nt].inside = 0; th[nt].u0 = th[nt].k0 = th[nt].cyc = 0;
                            th[nt].key = (uintptr_t *)calloc(AVP_PROF_PER_THREAD, sizeof(uintptr_t));
                            th[nt].cnt = (uint32_t *)calloc(AVP_PROF_PER_THREAD, sizeof(uint32_t));
                            if (GetThreadTimes(h, &c, &e, &ku, &us)) { th[nt].u0 = avp_ft(&us); th[nt].k0 = avp_ft(&ku); }
                            nt++;
                        }
                    }
                } while (Thread32Next(snap, &te));
            }
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
            next_list = GetTickCount64() + 2000;
        }
        for (k = 0; k < nt; k++) {
            CONTEXT ctx;
            ULONG64 cyc = 0;

            if (QueryThreadCycleTime(th[k].h, &cyc) && cyc - th[k].cyc < 100000) { th[k].cyc = cyc; continue; }
            th[k].cyc = cyc;
            if (SuspendThread(th[k].h) == (DWORD)-1) continue;
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th[k].h, &ctx)) {
                uintptr_t rip = (uintptr_t)ctx.Rip, rva = rip - base;
                th[k].samples++;

                if (rva >= (uintptr_t)AVP_PROF_BUCKETS * 16 && th[k].key && th[k].cnt) {
                    CONTEXT c = ctx;
                    int depth;
                    for (depth = 0; depth < 24; depth++) {
                        DWORD64 imgbase = 0, est = 0;
                        PVOID hd = NULL;
                        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imgbase, NULL);
                        if (fe) RtlVirtualUnwind(UNW_FLAG_NHANDLER, imgbase, c.Rip, fe, &c, &hd, &est, NULL);
                        else { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; }
                        if (!c.Rip) break;
                        if ((uintptr_t)c.Rip - base < (uintptr_t)AVP_PROF_BUCKETS * 16) {
                            uintptr_t key = ((uintptr_t)c.Rip & ~(uintptr_t)15) | 1;
                            uint32_t slot = (uint32_t)((key >> 4) * 2654435761u) & (AVP_PROF_PER_THREAD - 1), probes = 0;
                            while (th[k].key[slot] && th[k].key[slot] != key && probes++ < 64)
                                slot = (slot + 1) & (AVP_PROF_PER_THREAD - 1);
                            th[k].key[slot] = key;
                            th[k].cnt[slot]++;
                            break;
                        }
                    }
                }
                if (th[k].key && th[k].cnt) {
                    uintptr_t key = rip & ~(uintptr_t)15;
                    uint32_t slot = (uint32_t)((key >> 4) * 2654435761u) & (AVP_PROF_PER_THREAD - 1), probes = 0;
                    while (th[k].key[slot] && th[k].key[slot] != key && probes++ < 64)
                        slot = (slot + 1) & (AVP_PROF_PER_THREAD - 1);
                    th[k].key[slot] = key;
                    th[k].cnt[slot]++;
                }
                if (rva < (uintptr_t)AVP_PROF_BUCKETS * 16) {
                    hist[rva / 16]++;
                    th[k].inside++;
                } else {
                    uint32_t slot = (uint32_t)((rip >> 4) * 2654435761u) & (AVP_PROF_EXT - 1);
                    uint32_t probes = 0;
                    while (xaddr[slot] && xaddr[slot] != (rip & ~(uintptr_t)15) && probes++ < 64)
                        slot = (slot + 1) & (AVP_PROF_EXT - 1);
                    xaddr[slot] = rip & ~(uintptr_t)15;
                    xcount[slot]++;
                }
            }
            ResumeThread(th[k].h);
        }
        Sleep(1);
    }
    for (i = 0; i < AVP_PROF_BUCKETS; i++)
        if (hist[i]) fprintf(stderr, "PROF %X %u\n", i * 16, hist[i]);
    {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        HANDLE proc = GetCurrentProcess();
        SymRefreshModuleList(proc);
        for (i = 0; i < AVP_PROF_EXT; i++) {
            if (xcount[i]) {
                char mod[MAX_PATH] = "?";
                const char *leaf = mod, *p;
                HMODULE hm = NULL;
                DWORD64 disp = 0;
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 255;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)xaddr[i], &hm))
                    GetModuleFileNameA(hm, mod, sizeof mod);
                for (p = mod; *p; p++) if (*p == '\\') leaf = p + 1;
                fprintf(stderr, "PROFX %u %s!%s\n", xcount[i], leaf,
                        SymFromAddr(proc, xaddr[i], &disp, si) ? si->Name : "?");
            }
        }
    }

    {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        HANDLE proc = GetCurrentProcess();
        for (k = 0; k < nt; k++) {
            int shown;
            if (!th[k].key || th[k].samples < 500) continue;
            for (shown = 0; shown < 80; shown++) {
                uint32_t best = 0, bi = 0, j;
                for (j = 0; j < AVP_PROF_PER_THREAD; j++)
                    if (th[k].cnt[j] > best) { best = th[k].cnt[j]; bi = j; }
                if (!best) break;
                if (th[k].key[bi] & 1) {
                    fprintf(stderr, "PROFH %lu %u C %X\n", (unsigned long)th[k].tid, best,
                            (unsigned)((th[k].key[bi] & ~(uintptr_t)1) - base));
                } else if (th[k].key[bi] - base < (uintptr_t)AVP_PROF_BUCKETS * 16) {
                    fprintf(stderr, "PROFH %lu %u I %X\n", (unsigned long)th[k].tid, best,
                            (unsigned)(th[k].key[bi] - base));
                } else {
                    char mod[MAX_PATH] = "?";
                    const char *leaf = mod, *p;
                    HMODULE hm = NULL;
                    DWORD64 disp = 0;
                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                    si->MaxNameLen = 255;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)th[k].key[bi], &hm))
                        GetModuleFileNameA(hm, mod, sizeof mod);
                    for (p = mod; *p; p++) if (*p == '\\') leaf = p + 1;
                    fprintf(stderr, "PROFH %lu %u X %s!%s\n", (unsigned long)th[k].tid, best, leaf,
                            SymFromAddr(proc, th[k].key[bi], &disp, si) ? si->Name : "?");
                }
                th[k].cnt[bi] = 0;
            }
        }
    }
    for (k = 0; k < nt; k++) {
        FILETIME c, e, ku, us;
        if (GetThreadTimes(th[k].h, &c, &e, &ku, &us))
            fprintf(stderr, "PROFT %lu samples %u inside %u user_ms %llu kernel_ms %llu\n",
                    (unsigned long)th[k].tid, th[k].samples, th[k].inside,
                    (unsigned long long)((avp_ft(&us) - th[k].u0) / 10000),
                    (unsigned long long)((avp_ft(&ku) - th[k].k0) / 10000));
    }
    fprintf(stderr, "PROFEND\n");
    fflush(stderr);
    return 0;
}

static DWORD WINAPI avp_stack_watch(LPVOID unused)
{
    int i;
    const char *s = getenv("AVP_STACK_DUMP_SECS");
    (void)unused;
    if (!s) return 0;
    Sleep((DWORD)atoi(s) * 1000);
    for (i = 0; i < 2; i++) {
        avp_dump_stack("main", 0x00F80000u, 0x400);
        avp_dump_stack("worker-DE9B0", 0x03111FF0u, 0x800);
        avp_dump_stack("worker-120DE0", 0x0319CFF0u, 0x800);
        avp_dump_stack("dpc-7BF", 0x007C0000u, 0x100);
        fflush(stderr);
        Sleep(5000);
    }
    return 0;
}

static void print_guest_context(void *rip)
{

    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }

        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        if (!shown) {
            uint32_t k;

            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

extern void nv2a_hook_init(ptrdiff_t xbox_mem_offset);
extern bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                  uint32_t fault_xbox_va, int is_write);

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        {
            uint32_t va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
            if (va >= 0xFD008000u && va < 0xFD009000u) {
                const uint8_t *ip = (const uint8_t *)ep->ContextRecord->Rip;
                if (nv2a_hook_handle_mmio(ep->ContextRecord, fault_addr, va,
                                          ep->ExceptionRecord->ExceptionInformation[0] != 0))
                    return EXCEPTION_CONTINUE_EXECUTION;
                fprintf(stderr, "[AVP] PVIDEO access not decoded: va %08X %s, bytes "
                        "%02X %02X %02X %02X %02X %02X %02X %02X\n", va,
                        ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                        ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
            }
        }

        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            fprintf(stderr, "[AVP] unhandled NV2A register fault at 0x%llX, RIP=0x%llX, esp=0x%08X\n",
                    (unsigned long long)fault_addr,
                    (unsigned long long)ep->ContextRecord->Rip, g_esp);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static char s_base_dir[MAX_PATH * 2];
static char s_save_dir[MAX_PATH * 2];

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    if (!getenv("RECOMP_MKB_MOUSE"))
        _putenv("RECOMP_MKB_MOUSE=0");

    if (!getenv("RECOMP_MKB_WASD"))
        _putenv("RECOMP_MKB_WASD=0");

    if (!getenv("RECOMP_KB_UNBIND"))
        _putenv("RECOMP_KB_UNBIND=WASD");

    if (!getenv("RECOMP_DISPATCH_GATE"))
        _putenv("RECOMP_DISPATCH_GATE=1");

    if (!getenv("RECOMP_HEAP_RECLAIM") && !getenv("AVP_NO_RECLAIM"))
        _putenv("RECOMP_HEAP_RECLAIM=1");

    if (!getenv("RECOMP_D3D_PRESENT"))
        _putenv("RECOMP_D3D_PRESENT=1");

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Aliens versus Predator: Extinction ===\n");
    printf("Loading XBE...\n");

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    {
        extern int avp_setup_locate(char *base, size_t n);
        extern void avp_setup_save_dir(const char *base, char *out, size_t n);
        if (!avp_setup_locate(s_base_dir, sizeof s_base_dir))
            return 1;
        avp_setup_save_dir(s_base_dir, s_save_dir, sizeof s_save_dir);
        fprintf(stderr, "[SETUP] saves in %s\n", s_save_dir);
    }

    {
        extern void avp_settings_load(void);
        extern void avp_pcmenu_init(void);
        avp_settings_load();
        avp_pcmenu_init();
    }

    if (!load_xbe(AVP_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    avp_log_setup();
    nv2a_hook_init(g_xbox_mem_offset);

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(AVP_GAME_DIR, s_save_dir);
    }

    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    {
        extern void xbox_OhciInit(void);
        extern void xbox_InputInit(void);
        xbox_InputInit();
        xbox_OhciInit();
    }

    g_esp = XBOX_STACK_TOP;

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", AVP_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    xbox_WatchdogStart();

    printf("\nStarting game...\n");
    fflush(stdout);

    nv2a_sw_method_hook = avp_sw_method;
    CreateThread(NULL, 0, avp_semaphore_watch, NULL, 0, NULL);
    CreateThread(NULL, 0, avp_stack_watch, NULL, 0, NULL);
    CreateThread(NULL, 0, avp_xmv_probe, NULL, 0, NULL);
    CreateThread(NULL, 0, avp_movie_skip, NULL, 0, NULL);
    CreateThread(NULL, 0, avp_profiler, NULL, 0, NULL);

    if (getenv("AVP_FAST_ACK"))
        CreateThread(NULL, 0, avp_vblank_ack, NULL, 0, NULL);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}

