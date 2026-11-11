#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "xbox_memory_layout.h"
#include "kernel.h"
#include "guest_vmem.h"
#include "kernel_missing.h"
#include "nv2a_flip_hold.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;

size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}

void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

static HANDLE g_mapping_handle = NULL;

static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

static void *g_span_base = NULL;
static size_t g_span_size = 0;

static int g_span_whole_window = 0;

static void span_release_for(void *addr, size_t size)
{
#if !defined(_WIN32)
    uintptr_t a = (uintptr_t)addr, lo = (uintptr_t)g_span_base;
    if (g_span_whole_window && a >= lo && a + size <= lo + g_span_size)
        VirtualFree(addr, size, MEM_RELEASE);
#else
    (void)addr; (void)size;
#endif
}
static void *g_tiled_view = NULL;

static void *g_contig_memory = NULL;

#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;

#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;

static HANDLE g_contig_mapping = NULL;

#if defined(_WIN32)
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER  0x00004000
#endif
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER  0x00040000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#define WIN_GUEST_WINDOW 0x100000000ULL

typedef PVOID (WINAPI *pfn_VirtualAlloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                          void *, ULONG);
typedef PVOID (WINAPI *pfn_MapViewOfFile3)(HANDLE, HANDLE, PVOID, ULONG64,
                                           SIZE_T, ULONG, ULONG, void *, ULONG);
static pfn_VirtualAlloc2  s_VirtualAlloc2;
static pfn_MapViewOfFile3 s_MapViewOfFile3;

static uintptr_t g_win_window_base;
static size_t    g_win_window_size;
static const char *g_win_reserve_method = "none (fixed base list, nothing held)";

static int win_in_window(uintptr_t addr, size_t size)
{
    return g_win_window_size && addr >= g_win_window_base
        && addr + size <= g_win_window_base + g_win_window_size;
}

static void win_split_placeholder(uintptr_t addr, size_t size)
{
    VirtualFree((LPVOID)addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

static void win_release_window(void)
{
    uintptr_t a = g_win_window_base;
    uintptr_t end = g_win_window_base + g_win_window_size;

    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || !mbi.RegionSize)
            break;
        if (mbi.State == MEM_RESERVE && (uintptr_t)mbi.AllocationBase == a)
            VirtualFree((LPVOID)a, 0, MEM_RELEASE);
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    g_win_window_base = 0;
    g_win_window_size = 0;
}

static void win_settle_placeholders(void)
{
    uintptr_t a = g_win_window_base;
    uintptr_t end = g_win_window_base + g_win_window_size;

    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || !mbi.RegionSize)
            break;
        if (mbi.State == MEM_RESERVE && (uintptr_t)mbi.AllocationBase == a)
            s_VirtualAlloc2(GetCurrentProcess(), (PVOID)a, mbi.RegionSize,
                            MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                            PAGE_NOACCESS, NULL, 0);
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
}
#endif

static uint64_t ext_vma_span(uint64_t mirror_top)
{
    if (!getenv("RECOMP_EXT_VMA") || mirror_top >= GUEST_VMEM_TOP)
        return 0;
    return GUEST_VMEM_TOP - mirror_top;
}

static void *layout_map_view(HANDLE mapping, size_t size, void *addr)
{
#if defined(_WIN32)
    if (win_in_window((uintptr_t)addr, size)) {
        win_split_placeholder((uintptr_t)addr, size);
        return s_MapViewOfFile3(mapping, GetCurrentProcess(), addr, 0, size,
                                MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                NULL, 0);
    }
#endif
    span_release_for(addr, size);
    return MapViewOfFileEx(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size, addr);
}

static void *layout_commit_at(void *addr, size_t size)
{
#if defined(_WIN32)
    if (win_in_window((uintptr_t)addr, size)) {
        win_split_placeholder((uintptr_t)addr, size);
        if (!s_VirtualAlloc2(GetCurrentProcess(), addr, size,
                             MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                             PAGE_NOACCESS, NULL, 0))
            return NULL;
        return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
    }
#endif
    span_release_for(addr, size);
    return VirtualAlloc(addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

#if defined(_WIN32)

static void *win_reserve_guest_window(HANDLE mapping, size_t ram_size)
{
    size_t span = ram_size * (size_t)(1 + XBOX_NUM_MIRRORS);
    size_t window = span > WIN_GUEST_WINDOW ? span : (size_t)WIN_GUEST_WINDOW;
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    void *ram;

    window = (window + 0xFFFF) & ~(size_t)0xFFFF;
    if (kb) {
        s_VirtualAlloc2 = (pfn_VirtualAlloc2)(void (*)(void))
            GetProcAddress(kb, "VirtualAlloc2");
        s_MapViewOfFile3 = (pfn_MapViewOfFile3)(void (*)(void))
            GetProcAddress(kb, "MapViewOfFile3");
    }

    if (s_VirtualAlloc2 && s_MapViewOfFile3) {
        void *base = s_VirtualAlloc2(GetCurrentProcess(), NULL, window,
                                     MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                     PAGE_NOACCESS, NULL, 0);
        if (base) {
            g_win_window_base = (uintptr_t)base;
            g_win_window_size = window;
            ram = layout_map_view(mapping, ram_size, base);
            if (ram == base) {
                g_win_reserve_method = "placeholder (VirtualAlloc2 + MapViewOfFile3)";
                return ram;
            }
            fprintf(stderr, "xbox_MemoryLayoutInit: placeholder reservation at %p"
                    " could not take the RAM view (error %lu); falling back\n",
                    base, GetLastError());
            if (ram)
                UnmapViewOfFile(ram);
            win_release_window();
        } else {
            fprintf(stderr, "xbox_MemoryLayoutInit: VirtualAlloc2 placeholder"
                    " reservation of %zu MB failed (error %lu); falling back\n",
                    window >> 20, GetLastError());
        }
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: VirtualAlloc2/MapViewOfFile3 not"
                " found in kernelbase.dll; falling back to an unreserved"
                " 4 GB window\n");
    }

    {
        void *probe = VirtualAlloc(NULL, window, MEM_RESERVE, PAGE_NOACCESS);
        if (probe) {
            VirtualFree(probe, 0, MEM_RELEASE);
            ram = MapViewOfFileEx(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                  ram_size, probe);
            if (ram) {
                g_win_reserve_method = "probed free window (not held; racy)";
                return ram;
            }
        }
    }
    return NULL;
}
#endif

size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },

    { 0x000100, 0xFFFFFFFFu },
    { 0x001100, 0xFFFFFFFFu },
    { 0x002100, 0xFFFFFFFFu },
    { 0x400100, 0xFFFFFFFFu },
    { 0x600100, 0xFFFFFFFFu },
};

static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },
    { 0x003214, 0x00000010u },
};

#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

#define NV2A_PFIFO_DMA_PUT        0x003240u
#define NV2A_PFIFO_DMA_GET        0x003244u
#define NV2A_PFIFO_REF            0x003248u
#define NV2A_PFIFO_DMA_SUBROUTINE 0x00324Cu

#define AC97_NABM_OFFSET  0x400000u
#define AC97_TRAP_BYTES   0x1000u
#define AC97_RR           0x02u

static void *g_ac97_page = NULL;
static void *g_ac97_veh  = NULL;
static RECOMP_TLS int s_ac97_stepping = 0;

static void ac97_clear_reset_bits(void)
{

    uint32_t off;

    for (off = 0x10B; off < 0x180; off += 0x10) {
        volatile uint8_t *r = (volatile uint8_t *)((char *)g_ac97_page + off);
        if (*r & AC97_RR)
            *r = (uint8_t)(*r & ~AC97_RR);
    }
}

static LONG CALLBACK ac97_write_veh(PEXCEPTION_POINTERS ep)
{
#if defined(_WIN32)
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_ac97_page)
        return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_SINGLE_STEP && s_ac97_stepping) {
        s_ac97_stepping = 0;
        ac97_clear_reset_bits();
        VirtualProtect(g_ac97_page, AC97_TRAP_BYTES, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_ac97_page
                && fault < (uintptr_t)g_ac97_page + AC97_TRAP_BYTES) {
            if (!VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                                PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_ac97_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
#else

    (void)ep;
    (void)s_ac97_stepping;
    (void)ac97_clear_reset_bits;
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

#if !defined(_WIN32) && (defined(__aarch64__) \
        || (defined(__x86_64__) && defined(__linux__)))
#if defined(__aarch64__)
#include "../platform/mmio_decode_a64.h"
#else
#include "../platform/mmio_decode_x86_64.h"
#endif
#include <sys/mman.h>
static size_t g_ac97_hsize;

static uint64_t ac97_posix_rd(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    memcpy(&v, (char *)dev + off, size > 8 ? 8 : size);
    return v;
}

static void ac97_posix_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    memcpy((char *)dev + off, &v, size > 8 ? 8 : size);
}

static volatile int g_ac97_posix_lock;

int xbox_Ac97PosixFault(void *ucv, uintptr_t fault)
{
    int ok;

    if (!g_ac97_page || fault < (uintptr_t)g_ac97_page
            || fault >= (uintptr_t)g_ac97_page + g_ac97_hsize)
        return 0;

    while (__atomic_exchange_n(&g_ac97_posix_lock, 1, __ATOMIC_ACQUIRE))
        ;

    if (mprotect(g_ac97_page, g_ac97_hsize, PROT_READ | PROT_WRITE) != 0) {
        __atomic_store_n(&g_ac97_posix_lock, 0, __ATOMIC_RELEASE);
        return 0;
    }
#if defined(__aarch64__)
    ok = mmio_emulate_a64((ucontext_t *)ucv,
                          (uint32_t)(fault - (uintptr_t)g_ac97_page),
                          g_ac97_page, ac97_posix_rd, ac97_posix_wr);
#else
    ok = mmio_emulate_x86_64((ucontext_t *)ucv,
                             (uint32_t)(fault - (uintptr_t)g_ac97_page),
                             g_ac97_page, ac97_posix_rd, ac97_posix_wr);
#endif
    if (ok)
        ac97_clear_reset_bits();
    mprotect(g_ac97_page, g_ac97_hsize, PROT_READ);
    __atomic_store_n(&g_ac97_posix_lock, 0, __ATOMIC_RELEASE);
    return ok;
}
#endif

static void ac97_arm_write_trap(void)
{
    DWORD old;

    if (!g_mcpx_memory || g_ac97_page)
        return;
#if !defined(_WIN32) && !defined(__aarch64__) \
        && !(defined(__x86_64__) && defined(__linux__))

    (void)old;
    fprintf(stderr, "  AC97: bus-master write trap not available on this"
                    " host; a channel reset will spin\n");
    return;
#endif
    g_ac97_page = (char *)g_mcpx_memory + AC97_NABM_OFFSET;
#if !defined(_WIN32) && defined(__aarch64__)
    (void)old;
    g_ac97_hsize = (size_t)sysconf(_SC_PAGESIZE);
    if (g_ac97_hsize < AC97_TRAP_BYTES)
        g_ac97_hsize = AC97_TRAP_BYTES;
    if (mprotect(g_ac97_page, g_ac97_hsize, PROT_READ) != 0) {
        g_ac97_page = NULL;
        fprintf(stderr, "  AC97: could not arm the bus-master write trap;"
                        " a channel reset will spin\n");
        return;
    }
    fprintf(stderr, "  AC97: bus-master writes trapped at 0x%08X"
                    " (POSIX; channel reset completes on write)\n",
            XBOX_MCPX_BASE + AC97_NABM_OFFSET);
    return;
#endif

    g_ac97_veh = AddVectoredExceptionHandler(1, ac97_write_veh);
    if (!g_ac97_veh
            || !VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                               PAGE_READONLY, &old)) {
        if (g_ac97_veh) {
            RemoveVectoredExceptionHandler(g_ac97_veh);
            g_ac97_veh = NULL;
        }
        g_ac97_page = NULL;
        fprintf(stderr, "  AC97: could not arm the bus-master write trap;"
                        " a channel reset will spin\n");
        return;
    }
    fprintf(stderr, "  AC97: bus-master writes trapped at 0x%08X"
                    " (channel reset completes on write)\n",
            XBOX_MCPX_BASE + AC97_NABM_OFFSET);
}

#include "apu/apu.h"
#include "apu/apu_wav.h"
#include "usb/ohci.h"

#define MMIO_APU_BASE 0xFE800000u
#define MMIO_APU_END  0xFE880000u

extern MCPXAPUState *g_apu_state;

static int mmio_fault_va(uintptr_t fault, uint32_t *va)
{
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
    uint64_t rel = (uint64_t)fault - (uint64_t)base;

    if (fault < base || rel >= 0x100000000ull)
        return 0;
    *va = (uint32_t)rel;
    return 1;
}

#if defined(_WIN32)
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

int xbox_VehMmioFault(void *exception_pointers)
{
    PEXCEPTION_POINTERS ep = (PEXCEPTION_POINTERS)exception_pointers;
    uintptr_t fault;
    uint32_t va;

    if (!ep || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return 0;
    fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    if (!mmio_fault_va(fault, &va))
        return 0;
    if (va >= MMIO_APU_BASE && va < MMIO_APU_END)
        return g_apu_state
            && apu_hook_handle_mmio(ep->ContextRecord, fault, va,
                   ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0);
    if (xbox_OhciOwnsAddress(va))
        return xbox_OhciHandleMmio(ep->ContextRecord, va);
    return 0;
}
#else
#if defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
extern bool apu_hook_handle_mmio_posix(void *ucv, uintptr_t fault_addr,
                                       uint32_t fault_xbox_va, int is_write);
#endif

int xbox_PosixMmioFault(void *ucv, uintptr_t fault)
{
#if defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
    uint32_t va;

    if (!mmio_fault_va(fault, &va))
        return 0;

    if (va >= MMIO_APU_BASE && va < MMIO_APU_END)
        return g_apu_state
            && apu_hook_handle_mmio_posix(ucv, fault, va, 0);
    if (xbox_Ac97PosixFault(ucv, fault))
        return 1;
    if (xbox_OhciOwnsAddress(va))
        return xbox_OhciHandleMmio(ucv, va);
#else
    (void)ucv; (void)fault;
#endif
    return 0;
}
#endif

#define XBOX_MAX_DSP_ACK 8
static uint32_t g_dsp_ack[XBOX_MAX_DSP_ACK];
static int g_dsp_ack_count = 0;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_DSP_ACK");
    char buf[128], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_dsp_ack_count < XBOX_MAX_DSP_ACK; ) {
        unsigned long va = strtoul(q, &end, 0);
        if (end == q)
            break;
        g_dsp_ack[g_dsp_ack_count++] = (uint32_t)va;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_dsp_ack_count)
        fprintf(stderr, "  DSP ack: %d command word(s) will be completed"
                        " immediately\n", g_dsp_ack_count);
}

static int fence_readable(uint32_t va, uint32_t bytes);

#define XBOX_MAX_POKE 8
static struct { uint32_t va, value; } g_poke[XBOX_MAX_POKE];
static int g_poke_count;

static void poke_init(void)
{
    const char *spec = getenv("RECOMP_POKE");
    char buf[192], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_poke_count < XBOX_MAX_POKE; ) {
        unsigned long va = strtoul(q, &end, 0);
        unsigned long val = 0;
        if (end == q)
            break;
        if (*end == ':')
            val = strtoul(end + 1, &end, 0);
        g_poke[g_poke_count].va    = (uint32_t)va;
        g_poke[g_poke_count].value = (uint32_t)val;
        g_poke_count++;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_poke_count)
        fprintf(stderr, "  POKE: holding %d guest global(s) -- bring-up probe,"
                        " not a fix\n", g_poke_count);
}

static void poke_tick(void)
{
    int i;

    for (i = 0; i < g_poke_count; i++) {
        if (!fence_readable(g_poke[i].va, 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_poke[i].va + g_memory_offset);
            if (*w != g_poke[i].value)
                *w = g_poke[i].value;
        }
    }
}

static void dsp_ack_tick(void)
{
    int i;

    for (i = 0; i < g_dsp_ack_count; i++) {

        if (!fence_readable(g_dsp_ack[i], 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_dsp_ack[i] + g_memory_offset);
            if (*w)
                *w = 0;
        }
    }
}

static const uint32_t MCPX_COUNTERS[] = {
    0x020010,
};

static void *g_mcpx_regs = NULL;

static int g_apu_mmio_trapped = 0;

#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}

static int fence_readable(uint32_t va, uint32_t bytes)
{

    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;

    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src)
                *dst = src;
        }
    }
}

#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
    XboxFrameCounterBump bump;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;
static uint32_t g_frame_counters_owned;

extern int nv2a_pb_exec_flip_pacing(void);

uint32_t xbox_Nv2aFrameCountersOwned(void) { return g_frame_counters_owned; }

static void frame_counter_bump(int i)
{
    uint32_t dev;
    volatile uint32_t *p;
    int r;

    if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
        return;
    dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                 + g_memory_offset);
    if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
        return;
    p = (volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                              + g_memory_offset);
    r = xbox_FrameCounterBump(&g_frame_counters[i].bump, p,
                              nv2a_pb_exec_flip_pacing(), GetTickCount());
    if (r == XBOX_FRAME_COUNTER_HANDED_OVER) {
        g_frame_counters_owned++;
        fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X keeps"
                " moving on its own (0x%08X): the title counts it, the"
                " toolkit stops\n", g_frame_counters[i].device_ptr_va,
                g_frame_counters[i].counter_off, *p);
    } else if (r == XBOX_FRAME_COUNTER_HANDED_BACK) {
        g_frame_counters_owned--;
        fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X has"
                " stopped moving on its own: the toolkit bumps it again\n",
                g_frame_counters[i].device_ptr_va,
                g_frame_counters[i].counter_off);
    }
}

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;
    for (i = 0; i < g_frame_counter_count; i++)
        frame_counter_bump(i);
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;

    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++)
        frame_counter_bump(i);
}

uint32_t xbox_Nv2aFenceWordVa(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (get_ptr >= 0x80000000u && get_ptr - 0x80000000u <= 0x04000000u - 4u
                && !(get_ptr & 3u) && fence_readable(get_ptr, 4))
            return get_ptr;
    }
    return 0;
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put)
                *fence = put;
        }
    }
}

static int s_nv2a_trace = 0;

static int s_nv2a_trace_kicks = 0;

static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

#define NV2A_ACK_PFB_FLUSH          0x100410u

#define NV2A_ACK_PMC_INTR_0         0x000100u
#define NV2A_ACK_PMC_INTR_PCRTC     (1u << 24)
#define NV2A_ACK_PCRTC_INTR_0       0x600100u
#define NV2A_ACK_PCRTC_INTR_VBLANK  (1u << 0)

static void nv2a_ack_busy_bits(int flip_held)
{
    volatile uint32_t *regs = (volatile uint32_t *)g_nv2a_memory;
    int vbl_held;

    if (!regs)
        return;
    vbl_held = xbox_VblankAckHeld();
    for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
        volatile uint32_t *r =
            (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
        uint32_t mask = NV2A_ACK[i].busy_mask;
        if (flip_held && NV2A_ACK[i].offset == NV2A_ACK_PFB_FLUSH)
            continue;

        if (vbl_held) {
            if (NV2A_ACK[i].offset == NV2A_ACK_PMC_INTR_0)
                mask &= ~NV2A_ACK_PMC_INTR_PCRTC;
            else if (NV2A_ACK[i].offset == NV2A_ACK_PCRTC_INTR_0)
                mask &= ~NV2A_ACK_PCRTC_INTR_VBLANK;
        }

        if (*r & mask)
            InterlockedAnd((volatile LONG *)r, (LONG)~mask);
    }
}

void xbox_Nv2aAckBusyBits(void)
{
    nv2a_ack_busy_bits(0);
}

volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    if (!g_nv2a_memory || offset + 4 > XBOX_NV2A_SIZE)
        return NULL;
    return (volatile uint32_t *)((char *)g_nv2a_memory + offset);
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {

        extern int nv2a_pb_exec_flip_poll(void);
        int flip_held = nv2a_pb_exec_flip_poll();
        nv2a_ack_busy_bits(flip_held);
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
            }
        }

        fence_mirrors_tick();
        dsp_ack_tick();
        poke_tick();
        counter_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        {

            static DWORD  last_put_ms;
            static uint32_t last_put;
            static uint32_t get_written = 0xFFFFFFFFu;
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            if (!flip_held
                    && (put != last_put || (now_ms - last_put_ms) > 2000)) {

                {
                    extern void nv2a_pb_scan(uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    extern void nv2a_pb_resync(uint32_t);
                    extern int  nv2a_pb_dma_walk(void);
                    extern void nv2a_pb_scan_segment(uint32_t, uint32_t);

                    if (nv2a_pb_dma_walk()) {
                        uint32_t get_now = *(volatile uint32_t *)
                                           ((char *)regs + NV2A_USER_DMA_GET);

                        if (get_written == 0xFFFFFFFFu || get_now != get_written)
                            nv2a_pb_resync(get_now);
                        if (put != last_put)
                            nv2a_pb_scan(put);
                    } else {

                        static uint32_t put_lo, put_hi;
                        if (!put_lo || put < put_lo) put_lo = put;
                        if (put > put_hi) put_hi = put;
                        if (last_put && put > last_put) {
                            nv2a_pb_scan_segment(XBOX_CONTIG_BASE | (last_put & 0x0FFFFFFFu),
                                                 XBOX_CONTIG_BASE | (put      & 0x0FFFFFFFu));
                        } else if (last_put && put < last_put) {
                            if (put_hi > last_put)
                                nv2a_pb_scan_segment(
                                    XBOX_CONTIG_BASE | (last_put & 0x0FFFFFFFu),
                                    XBOX_CONTIG_BASE | (put_hi   & 0x0FFFFFFFu));
                            if (put > put_lo)
                                nv2a_pb_scan_segment(
                                    XBOX_CONTIG_BASE | (put_lo & 0x0FFFFFFFu),
                                    XBOX_CONTIG_BASE | (put    & 0x0FFFFFFFu));
                        }
                    }

                    static long report_ms = -1;
                    if (report_ms < 0) {
                        const char *e = getenv("RECOMP_PB_REPORT_MS");
                        report_ms = e ? atol(e) : 10000;
                        if (report_ms < 100) report_ms = 100;
                    }
                    if (s_nv2a_trace && now_ms - last_report > (uint64_t)report_ms) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }

                {
                    volatile uint32_t *get =
                        (volatile uint32_t *)((char *)regs
                                              + NV2A_USER_DMA_GET);
                    *get = put;
                    get_written = put;
                }
                last_put = put; last_put_ms = now_ms;

                if (s_nv2a_trace_kicks) {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                    fflush(stderr);
                }
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = GetTickCount();

        Sleep(0);
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    dsp_ack_init();
    poke_init();
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    }
}

static void *g_kernel_memory = NULL;

ptrdiff_t g_xbox_mem_offset = 0;

uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;

uint32_t g_xbox_region_base = XBOX_REGION_MIN;
uint32_t g_xbox_stack_base  = XBOX_REGION_MIN + XBOX_REGION_STACK_OFF;
uint32_t g_xbox_stack_size  = 8u * 1024 * 1024;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

RECOMP_TLS uint32_t g_fs_base = XBOX_TIB_MAIN;

static uint32_t g_tls_template_va, g_tls_total, g_tls_thread_size = 64;

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

#ifdef RECOMP_ABI_CHECK

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
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
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);

        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}
#endif

RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;

int g_force_return = 0;

RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS uint32_t g_mxcsr = 0x1F80u;
RECOMP_TLS int g_fp_cmp = 0;
RECOMP_TLS uint16_t g_fp_cc = 0x4000;

extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va)
            return &s_jmp[i].native;
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;
    s_jmp[s_jmp_used].buf_va = buf_va;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;

static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

static int peek_readable(uint32_t va)
{
    struct { const void *mapped; uint32_t base; uint64_t size; } win[] = {
        { g_memory_base,   XBOX_BASE_ADDRESS, (uint64_t)g_memory_size },
        { g_contig_memory, XBOX_CONTIG_BASE,  XBOX_CONTIG_SIZE },
        { g_nv2a_memory,   XBOX_NV2A_BASE,    XBOX_NV2A_SIZE },
        { g_mcpx_memory,   XBOX_MCPX_BASE,    XBOX_MCPX_SIZE },
        { g_flash_memory,  XBOX_FLASH_BASE,   XBOX_FLASH_SIZE },
    };
    size_t i;

    for (i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
        if (!win[i].mapped || !win[i].size)
            continue;
        if (va >= win[i].base
                && (uint64_t)va + 4 <= (uint64_t)win[i].base + win[i].size)
            return 1;
    }
    return 0;
}

static uint32_t g_watch_va;
static void    *g_watch_page;
static uint32_t g_watch_last;
static void    *g_watch_veh;
static RECOMP_TLS int s_watch_stepping;

static int watch_is_code(uint32_t va)
{
    return va >= g_xbox_code_lo && va < g_xbox_code_hi;
}

static void watch_report(void)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t now = *(const uint32_t *)(mem + g_watch_va);
    uint32_t esp = g_esp, i, shown = 0;

    if (now == g_watch_last)
        return;
    fprintf(stderr, "[WATCH] [%08X] %08X -> %08X  (esp=%08X)\n",
            g_watch_va, g_watch_last, now, esp);
    g_watch_last = now;

    for (i = 0; esp && i < 256u && shown < 12u; i++) {
        uint32_t slot = esp + i * 4u;
        uint32_t v;
        if (!peek_readable(slot))
            break;
        v = *(const uint32_t *)(mem + slot);
        if (watch_is_code(v)) {
            fprintf(stderr, "         [esp+%-4u] %08X\n", i * 4u, v);
            shown++;
        }
    }

    if (getenv("RECOMP_WATCH_RAW")) {
        for (i = 0; esp && i < 24u; i++) {
            uint32_t slot = esp + i * 4u;
            if (!peek_readable(slot))
                break;
            fprintf(stderr, "         raw[esp+%-4u] %08X\n", i * 4u,
                    *(const uint32_t *)(mem + slot));
        }
    }
    fflush(stderr);
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep)
{
#if defined(_WIN32)
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_watch_page)
        return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_SINGLE_STEP && s_watch_stepping) {
        s_watch_stepping = 0;
        watch_report();
        VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_watch_page
                && fault < (uintptr_t)g_watch_page + 4096) {
            if (!VirtualProtect(g_watch_page, 4096, PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_watch_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
#else

    (void)ep;
    (void)watch_report;
    (void)s_watch_stepping;
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

#if !defined(_WIN32)

#include <signal.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

static uint32_t g_watch_len = 4;
static uintptr_t g_watch_hpage, g_watch_hsize;
static uint32_t g_watch_snap[4096];
static volatile int g_watch_open;
static struct sigaction g_watch_prev_bus, g_watch_prev_segv;

static void watch_sym(void *pc, const char *tag, int n)
{
    Dl_info di;
    if (pc && dladdr(pc, &di) && di.dli_sname)
        fprintf(stderr, "         %s%-2d %s+0x%lX\n", tag, n, di.dli_sname,
                (unsigned long)((uintptr_t)pc - (uintptr_t)di.dli_saddr));
    else
        fprintf(stderr, "         %s%-2d %p\n", tag, n, pc);
}

static void watch_posix_stack(void *ucv)
{
#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
    uintptr_t prev = 0;
    int n = 0, reps = 0, depth = 0;
    watch_sym((void *)(uintptr_t)uc->uc_mcontext->__ss.__pc, "#", n++);
    watch_sym((void *)(uintptr_t)uc->uc_mcontext->__ss.__lr, "#", n++);

    {
        const char *dv = getenv("RECOMP_WATCH_DEPTH");
        depth = dv && *dv ? atoi(dv) : 14;
    }
    while (n < depth && fp && (fp & 7) == 0) {
        uintptr_t next = ((const uintptr_t *)fp)[0];
        uintptr_t ret = ((const uintptr_t *)fp)[1];
        if (ret && ret == prev) {
            reps++;
        } else if (ret) {
            if (reps)
                fprintf(stderr, "            (previous frame x%d more)\n",
                        reps);
            reps = 0;
            watch_sym((void *)ret, "#", n++);
            prev = ret;
        }
        if (next <= fp)
            break;
        fp = next;
    }
    if (reps)
        fprintf(stderr, "            (previous frame x%d more)\n", reps);
#else
    (void)ucv;
    fprintf(stderr, "         (host stack not available on this host)\n");
#endif
}

static void watch_posix_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    uintptr_t page = g_watch_hpage;
    struct sigaction *prev;

    if (g_watch_page && fault >= page && fault < page + g_watch_hsize) {
        uintptr_t lo = (uintptr_t)g_memory_offset + g_watch_va;
        if (fault >= lo && fault < lo + g_watch_len) {
            fprintf(stderr, "[WATCH] write to %08X (+0x%X) esp=%08X\n",
                    (uint32_t)(fault - (uintptr_t)g_memory_offset),
                    (unsigned)(fault - lo), g_esp);
            watch_posix_stack(ucv);
            fflush(stderr);
        }
        mprotect((void *)g_watch_hpage, g_watch_hsize, PROT_READ | PROT_WRITE);
        g_watch_open = 1;
        return;
    }

    prev = sig == SIGBUS ? &g_watch_prev_bus : &g_watch_prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) {
            prev->sa_sigaction(sig, si, ucv);
            return;
        }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

static void watch_posix_diff(void)
{
    const uint32_t *cur = (const uint32_t *)((const uint8_t *)g_memory_offset
                                             + g_watch_va);
    uint32_t i, n = (g_watch_len + 3) / 4;
    for (i = 0; i < n; i++) {
        if (cur[i] != g_watch_snap[i]) {
            fprintf(stderr, "[WATCH] [%08X] %08X -> %08X\n",
                    g_watch_va + i * 4, g_watch_snap[i], cur[i]);
            g_watch_snap[i] = cur[i];
        }
    }
    fflush(stderr);
}

static void *watch_posix_reprotect(void *unused)
{
    struct timespec ts = { 0, 200 * 1000 };
    (void)unused;
    for (;;) {
        nanosleep(&ts, NULL);
        if (!g_watch_open)
            continue;
        mprotect((void *)g_watch_hpage, g_watch_hsize, PROT_READ);
        g_watch_open = 0;
        watch_posix_diff();
    }
    return NULL;
}

static int watch_posix_arm(void)
{
    struct sigaction sa;
    pthread_t th;
    const char *len = getenv("RECOMP_WATCH_LEN");
    uint32_t room;

    g_watch_hsize = (uintptr_t)sysconf(_SC_PAGESIZE);
    g_watch_hpage = ((uintptr_t)g_memory_offset + g_watch_va)
                    & ~(g_watch_hsize - 1);
    room = (uint32_t)(g_watch_hpage + g_watch_hsize
                      - ((uintptr_t)g_memory_offset + g_watch_va));
    if (room > sizeof g_watch_snap)
        room = sizeof g_watch_snap;
    if (len && *len)
        g_watch_len = (uint32_t)strtoul(len, NULL, 0);
    if (!g_watch_len)
        g_watch_len = 4;
    if (g_watch_len > room)
        g_watch_len = room;
    memcpy(g_watch_snap, (const uint8_t *)g_memory_offset + g_watch_va,
           (g_watch_len + 3) & ~3u);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = watch_posix_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, &g_watch_prev_bus) != 0
            || sigaction(SIGSEGV, &sa, &g_watch_prev_segv) != 0)
        return 0;
    if (pthread_create(&th, NULL, watch_posix_reprotect, NULL) != 0)
        return 0;
    pthread_detach(th);
    if (mprotect((void *)g_watch_hpage, g_watch_hsize, PROT_READ) != 0)
        return 0;
    fprintf(stderr, "  WATCH: writes to %u bytes at 0x%08X are trapped "
                    "(POSIX, current %08X)\n", g_watch_len, g_watch_va,
            g_watch_last);
    fflush(stderr);
    return 1;
}
#endif

static unsigned g_watch_derefs;
static uint32_t g_watch_root;
static uint32_t g_watch_off;

static int watch_resolve(uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t a = g_watch_root;
    unsigned k;

    for (k = 0; k < g_watch_derefs; k++) {
        if (!peek_readable(a))
            return 0;
        a = *(const uint32_t *)(mem + a);
        if (!a)
            return 0;
    }
    a += g_watch_off;
    if (!peek_readable(a))
        return 0;
    *out = a;
    return 1;
}

static int watch_arm(uint32_t va);

static DWORD WINAPI watch_resolver(LPVOID unused)
{
    unsigned tries;

    (void)unused;
    for (tries = 0; tries < 15000u && !g_watch_page; tries++) {
        uint32_t va;
        if (watch_resolve(&va) && watch_arm(va))
            return 0;
        Sleep(20);
    }
    if (!g_watch_page)
        fprintf(stderr, "  WATCH: %u-deep chain from 0x%08X never resolved\n",
                g_watch_derefs, g_watch_root);
    return 0;
}

void xbox_WatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    const char *q;
    char *endp;

    if (!spec || !*spec || g_memory_base == NULL || g_watch_page)
        return;

    for (q = spec; *q == '['; q++)
        g_watch_derefs++;
    g_watch_root = (uint32_t)strtoul(q, &endp, 0);
    while (*endp == ']')
        endp++;
    if (*endp == '+')
        g_watch_off = (uint32_t)strtoul(endp + 1, NULL, 0);

    if (g_watch_derefs) {
        fprintf(stderr, "  WATCH: resolving %u-deep chain from 0x%08X "
                        "+0x%X\n", g_watch_derefs, g_watch_root, g_watch_off);
        CloseHandle(CreateThread(NULL, 0, watch_resolver, NULL, 0, NULL));
        return;
    }
    watch_arm(g_watch_root + g_watch_off);
}

static int watch_arm(uint32_t va)
{
    DWORD old;

    g_watch_va = va;
    if (!peek_readable(g_watch_va)) {
        fprintf(stderr, "  WATCH: 0x%08X is not in a mapped window; "
                        "not armed\n", g_watch_va);
        return 0;
    }
    g_watch_last = *(const uint32_t *)((const uint8_t *)g_memory_offset
                                       + g_watch_va);

    g_watch_page = (void *)(((uintptr_t)((const uint8_t *)g_memory_offset
                                         + g_watch_va)) & ~(uintptr_t)4095);
#if !defined(_WIN32)
    (void)old;
    if (!watch_posix_arm()) {
        g_watch_page = NULL;
        fprintf(stderr, "  WATCH: cannot trap 0x%08X; not armed\n",
                g_watch_va);
        return 0;
    }
    return 1;
#endif
    g_watch_veh = AddVectoredExceptionHandler(1, watch_veh);
    if (!g_watch_veh
            || !VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old)) {
        if (g_watch_veh) {
            RemoveVectoredExceptionHandler(g_watch_veh);
            g_watch_veh = NULL;
        }
        g_watch_page = NULL;
        fprintf(stderr, "  WATCH: cannot trap 0x%08X; not armed\n",
                g_watch_va);
        return 0;
    }
    fprintf(stderr, "  WATCH: writes to the page of 0x%08X are trapped "
                    "(current %08X)\n", g_watch_va, g_watch_last);
    fflush(stderr);
    return 1;
}

void xbox_PeekSample(const char *label)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    const char *spec = getenv("RECOMP_PEEK");
    char buf[256], *q, *end;

    if (!spec || !*spec || g_memory_base == NULL)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    fprintf(stderr, "  %s:", label ? label : "peek");
    for (q = buf; *q; ) {

        unsigned derefs = 0, k;
        unsigned long va;
        uint32_t a;
        int ok = 1;

        while (*q == '[') { derefs++; q++; }
        va = strtoul(q, &end, 0);
        if (end == q)
            break;
        while (*end == ']')
            end++;
        a = (uint32_t)va;
        for (k = 0; k < derefs && ok; k++) {
            if (!peek_readable(a) || !(a = *(const uint32_t *)(mem + a)))
                ok = 0;
        }
        if (*end == '+')
            a += (uint32_t)strtoul(end + 1, &end, 0);
        if (ok && peek_readable(a))
            fprintf(stderr, " [%08X]=%08X", a, *(const uint32_t *)(mem + a));
        else
            fprintf(stderr, " [%08X]=??", a);
        q = (*end == ',') ? end + 1 : end;
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);

    {
        uint32_t k;

        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }

    xbox_PeekSample("peek");

    if (g_nv2a_memory) {
        const char *r = (const char *)g_nv2a_memory;
#define WD_NV2A(off) (*(const volatile uint32_t *)(r + (off)))
        fprintf(stderr, "  NV2A USER  PUT=%08X GET=%08X\n"
                        "  NV2A PFIFO PUT=%08X GET=%08X REF=%08X SUBR=%08X\n",
                WD_NV2A(NV2A_USER_DMA_PUT), WD_NV2A(NV2A_USER_DMA_GET),
                WD_NV2A(NV2A_PFIFO_DMA_PUT), WD_NV2A(NV2A_PFIFO_DMA_GET),
                WD_NV2A(NV2A_PFIFO_REF), WD_NV2A(NV2A_PFIFO_DMA_SUBROUTINE));
#undef WD_NV2A
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);

    {
        uint32_t hi, lo;
        xbox_missing_counts(&hi, &lo, NULL);
        if (hi + lo)
            fprintf(stderr, "  [FILE] missing at watchdog: %u high, %u low\n",
                    (unsigned)hi, (unsigned)lo);
        fflush(stderr);
    }

    apu_wav_close_nowait(500);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

RECOMP_TLS uint32_t g_ebp = 0;

RECOMP_TLS int g_df = 0;

RECOMP_TLS uint32_t g_eflags = 0x00000202u;

volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

static void xbox_heap_reset_base(void);

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    g_force_return = getenv("RECOMP_FORCE_RETURN") != NULL;
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        (DWORD)g_memory_size,
        NULL
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    {
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,
            0x00800000,
            0x01000000,
            0x02000000,
            0x10000000,
            0,
        };

#if defined(_WIN32)

        if (getenv("RECOMP_WIN_PLACEHOLDER"))
            g_memory_base = win_reserve_guest_window(g_mapping_handle,
                                                     g_memory_size);
#else

        g_span_size = g_memory_size * (size_t)(1 + XBOX_NUM_MIRRORS);

        if (g_span_size > XBOX_CONTIG_BASE)
            g_span_size = XBOX_CONTIG_BASE;

        if (ext_vma_span(g_span_size)) {
            g_span_size = (size_t)1 << 32;
            g_span_whole_window = 1;
        }
        g_span_base = VirtualAlloc(NULL, g_span_size, MEM_RESERVE, PAGE_NOACCESS);
        if (g_span_base) {
            VirtualFree(g_span_base, g_memory_size, MEM_RELEASE);
            g_memory_base = MapViewOfFileEx(g_mapping_handle,
                                            FILE_MAP_ALL_ACCESS, 0, 0,
                                            g_memory_size, g_span_base);
            if (!g_memory_base) {
                VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
                g_span_base = NULL;
                g_span_size = 0;
                g_span_whole_window = 0;
            }
        } else {
            g_span_whole_window = 0;
        }
#endif

        const size_t n_bases = sizeof(try_bases) / sizeof(try_bases[0]);
        for (size_t i = 0; !g_memory_base && i < n_bases; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                hint
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {

                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;
#if defined(_WIN32)
    fprintf(stderr, "xbox_MemoryLayoutInit: guest window reservation: %s,"
            " base %p", g_win_reserve_method, g_memory_base);
    if (g_win_window_size)
        fprintf(stderr, ", %zu MB held\n", g_win_window_size >> 20);
    else
        fprintf(stderr, "\n");
#endif

    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;

#if defined(_WIN32)
        SYSTEM_INFO si;
        long host_page;
        GetSystemInfo(&si);
        host_page = (long)si.dwPageSize;
#else
        long host_page = sysconf(_SC_PAGESIZE);
#endif
        if (host_page > 0 && (uint32_t)host_page > XBOX_TIB_MAIN) {
            fprintf(stderr, "  RECOMP_TRAP_NULL: not available -- the host page "
                    "is %ld bytes, so trapping guest page zero would also trap "
                    "the TIB at 0x%08X\n", host_page, XBOX_TIB_MAIN);
        } else {
            if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect)) {
                fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                                " (null dereferences fault)\n");
            }
        }
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    {

        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;

        DWORD cert_off = *(const DWORD *)(xbe + 0x118) - base_addr;
        if (cert_off + 0x0C + 80 <= xbe_size) {
            extern void xbox_FramebufferWindowSetTitle(const uint16_t *, int);
            xbox_FramebufferWindowSetTitle(
                (const uint16_t *)(xbe + cert_off + 0x0C), 40);
        }
        int sections_short = 0;
        size_t total_bytes = 0;

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            memset(XBOX_VA(sec_va), 0, sec_vsize);

            int have_data = (copy_size == 0) ||
                            (sec_raw_off + copy_size <= xbe_size);
            if (copy_size > 0 && have_data) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            } else if (!have_data) {
                fprintf(stderr,
                        "  WARNING: section %u (%s) raw data 0x%08X+%u runs past "
                        "the %zu-byte image -- left zeroed\n",
                        si, sec_name, sec_raw_off, copy_size, xbe_size);
                sections_short++;
            }

            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            if (have_data) {
                sections_loaded++;
                total_bytes += copy_size;
            }

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
        if (sections_short) {
            fprintf(stderr,
                    "  ERROR: %d section(s) had no data in the image -- the XBE "
                    "is truncated or was read short; the title will not run\n",
                    sections_short);
        }
    }

    {
        const uint32_t MB = 1024u * 1024u;
        uint32_t image_end = (g_xbox_image_hi + 0xFFFFu) & ~0xFFFFu;
        uint32_t region = image_end > XBOX_REGION_MIN ? image_end : XBOX_REGION_MIN;
        uint32_t stack_base = region + XBOX_REGION_STACK_OFF;
        uint32_t top = XBOX_HEAP_TOP;
        uint32_t avail = top > stack_base ? top - stack_base : 0;
        uint32_t stack_size = 8 * MB;

        if (avail < 2 * MB) {
            fprintf(stderr,
                    "  ERROR: the title image ends at 0x%08X; with the toolkit "
                    "region at 0x%08X that leaves %u KB below the end of RAM "
                    "(0x%08X), less than the 1 MB stack + 1 MB heap minimum\n",
                    g_xbox_image_hi, region, avail / 1024, top);
            return FALSE;
        }
        if (avail - stack_size < 16 * MB) {
            stack_size = avail > 17 * MB ? (avail - 16 * MB) & ~0xFFFFu : 1 * MB;
            if (stack_size < 1 * MB) stack_size = 1 * MB;
        }

        g_xbox_region_base = region;
        g_xbox_stack_base  = stack_base;
        g_xbox_stack_size  = stack_size;
        xbox_heap_reset_base();

        fprintf(stderr, "  Toolkit region: 0x%08X (image ends 0x%08X)%s\n",
                region, g_xbox_image_hi,
                region != XBOX_REGION_MIN ? " -- moved above the image" : "");
        if (stack_size != 8 * MB)
            fprintf(stderr, "  Stack shrunk to %u KB so the heap keeps %u MB\n",
                    stack_size / 1024, (avail - stack_size) / MB);
    }

    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {

            uint32_t thunk_count = 0;

            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    (void)old_protect;

    #undef XBOX_VA

    g_xbox_mem_offset = g_memory_offset;

    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);

        #define FAKE_PRCB_VA (g_xbox_region_base + XBOX_REGION_PRCB_OFF)
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        #define FAKE_TLS_VA     (g_xbox_region_base + XBOX_REGION_TLS_OFF)
        #define FAKE_RWDATA_VA  (g_xbox_region_base)

        MEM32_INIT(XBOX_FS_BASE + 0x28, FAKE_TLS_VA);

        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        #define FAKE_TLS_BLOCK_VA  (g_xbox_region_base + XBOX_REGION_TLSBLK_OFF)
        #define FAKE_TLS_THREAD_VA (g_xbox_region_base + XBOX_REGION_TLSBLK_OFF + 0x200)
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);

                g_tls_template_va = FAKE_TLS_BLOCK_VA;
                g_tls_total       = total;

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? layout_map_view(g_contig_mapping, XBOX_CONTIG_SIZE,
                              (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = layout_commit_at((LPVOID)contig_native,
                                               XBOX_CONTIG_SIZE);
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {

            DWORD err = GetLastError();
            MEMORY_BASIC_INFORMATION mbi;
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X (native %p) "
                    "failed (error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, (void *)contig_native, err);
            if (VirtualQuery((LPCVOID)contig_native, &mbi, sizeof(mbi)))
                fprintf(stderr, "    occupant: base %p alloc-base %p size 0x%zx "
                        "state 0x%lx type 0x%lx\n",
                        mbi.BaseAddress, mbi.AllocationBase, mbi.RegionSize,
                        (unsigned long)mbi.State, (unsigned long)mbi.Type);
        }
    }

    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = layout_commit_at((LPVOID)nv2a_native, XBOX_NV2A_SIZE);

        s_nv2a_trace_kicks = getenv("RECOMP_NV2A_TRACE") != NULL;
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = layout_commit_at((LPVOID)mcpx_native, XBOX_MCPX_SIZE);
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {

            #define MCPX_AC97_CODEC_STATUS 0x00400130u
            #define MCPX_AC97_CODEC_READY  0x00000100u

            if (getenv("RECOMP_AC97_READY")) {

                enum { APU_TRAP_BYTES = 0x00030000 };
                DWORD old_protect;
                if (VirtualProtect((char *)g_mcpx_memory, APU_TRAP_BYTES,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO"
                                    " (GP/EP DSP memory left as RAM)\n",
                            XBOX_MCPX_BASE, XBOX_MCPX_BASE + APU_TRAP_BYTES);
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;

                ac97_arm_write_trap();
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (DirectSound will initialise)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = layout_commit_at((LPVOID)flash_native,
                                          XBOX_FLASH_SIZE);
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096

#if defined(_WIN32)
        #define KERNEL_ALLOC_SIZE 0x10000
#else
        #define KERNEL_ALLOC_SIZE KERNEL_PAGE_SIZE
#endif
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;

        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : layout_commit_at((LPVOID)kernel_native, KERNEL_ALLOC_SIZE);
        if (g_kernel_memory) {

            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
        #undef KERNEL_ALLOC_SIZE
    }

    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    {
        int mirrors_ok = 0;

        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }

            if (guest_lo < (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                    && XBOX_CONTIG_BASE < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the contiguous"
                                " window at 0x%08X\n",
                        m + 1, (unsigned)XBOX_CONTIG_BASE);
                continue;
            }
            if (guest_lo < (uint64_t)XBOX_FLASH_BASE + XBOX_FLASH_SIZE
                    && XBOX_NV2A_BASE < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the device"
                                " apertures at 0x%08X\n",
                        m + 1, (unsigned)XBOX_NV2A_BASE);
                continue;
            }

            if (g_span_base && !g_span_whole_window)
                VirtualFree((LPVOID)mirror_base, g_memory_size, MEM_RELEASE);
            g_mirror_views[m] = layout_map_view(g_mapping_handle,
                                                g_memory_size,
                                                (LPVOID)mirror_base);
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    {
        uint64_t mirror_top = (uint64_t)g_memory_size * (1u + XBOX_NUM_MIRRORS);
        uint64_t ext = ext_vma_span(mirror_top);

        if (ext) {
            uintptr_t host = (uintptr_t)(mirror_top + (uint64_t)g_memory_offset);
#if defined(_WIN32)
            if (win_in_window(host, (size_t)ext)) {
                win_split_placeholder(host, (size_t)ext);
                VirtualFree((LPVOID)host, 0, MEM_RELEASE);
            }
#else
            if (g_span_base && host >= (uintptr_t)g_span_base
                    && host + ext <= (uintptr_t)g_span_base + g_span_size)
                VirtualFree((LPVOID)host, (SIZE_T)ext, MEM_RELEASE);
#endif
        }
        guest_vmem_init(g_memory_offset, g_memory_size, mirror_top);
    }

    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();

        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = g_contig_mapping
            ? layout_map_view(g_contig_mapping, tiled_size,
                              (LPVOID)tiled_native)
            : NULL;
        if (g_tiled_view) {

            if (g_contig_memory !=
                    (void *)(uintptr_t)(XBOX_CONTIG_BASE + g_memory_offset)) {
                fprintf(stderr, "  WARNING: tiled aperture alias not checked:"
                        " contiguous window is not mapped\n");
            } else {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases the contiguous window)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

#if defined(_WIN32)
    if (g_win_window_size)
        win_settle_placeholders();
#endif
    xbox_WatchInit();
    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }

    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }

    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }

    if (g_tiled_view) {
        UnmapViewOfFile(g_tiled_view);
        g_tiled_view = NULL;
    }
    if (g_contig_memory) {
#if defined(_WIN32)

        if (!UnmapViewOfFile(g_contig_memory))
#endif
        VirtualFree(g_contig_memory, 0, MEM_RELEASE);
        g_contig_memory = NULL;
    }
#if defined(_WIN32)
    if (g_contig_mapping) {
        CloseHandle(g_contig_mapping);
        g_contig_mapping = NULL;
    }
#endif
    if (g_mcpx_memory) {
        VirtualFree(g_mcpx_memory, 0, MEM_RELEASE);
        g_mcpx_memory = NULL;
    }
    if (g_flash_memory) {
        VirtualFree(g_flash_memory, 0, MEM_RELEASE);
        g_flash_memory = NULL;
    }

    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }

    guest_vmem_shutdown();

    if (g_span_base) {
        VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
        g_span_base = NULL;
        g_span_size = 0;
        g_span_whole_window = 0;
    }
#if defined(_WIN32)

    if (g_win_window_size)
        win_release_window();
#endif
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

static uint32_t g_heap_next = 0;

static void xbox_heap_reset_base(void)
{
    g_heap_next = XBOX_HEAP_BASE;
}

static int g_heap_alloc_count = 0;

#define XBOX_HEAP_MAX_BLOCKS 65536

static struct { uint32_t addr; uint32_t size; uint8_t free; uint8_t named; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

static SRWLOCK g_heap_lock = SRWLOCK_INIT;

int xbox_HeapReclaimEnabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_HEAP_RECLAIM") != NULL;
    return on;
}

static int heap_insert_free(int at, uint32_t addr, uint32_t size)
{
    if (!(at < g_heap_block_count && g_heap_blocks[at].size == 0)) {
        if (g_heap_block_count >= XBOX_HEAP_MAX_BLOCKS)
            return 0;
        memmove(&g_heap_blocks[at + 1], &g_heap_blocks[at],
                (size_t)(g_heap_block_count - at) * sizeof g_heap_blocks[0]);
        g_heap_block_count++;
    }
    g_heap_blocks[at].addr = addr;
    g_heap_blocks[at].size = size;
    g_heap_blocks[at].free = 1; g_heap_blocks[at].named = 0;
    return 1;
}

static uint32_t heap_carve_free(uint32_t size, uint32_t alignment)
{
    for (int i = 0; i < g_heap_block_count; i++) {
        uint32_t a, start, end, front, back;
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size)
            continue;
        a = g_heap_blocks[i].addr;
        start = (a + alignment - 1) & ~(alignment - 1);
        end = a + g_heap_blocks[i].size;
        if (start + size > end || start + size < start)
            continue;
        front = start - a;
        back = end - (start + size);
        if (front && !heap_insert_free(i, a, front))
            continue;
        if (front)
            i++;
        g_heap_blocks[i].addr = start;
        g_heap_blocks[i].size = size;
        g_heap_blocks[i].free = 0; g_heap_blocks[i].named = 0;
        if (back && !heap_insert_free(i + 1, start + size, back))
            g_heap_blocks[i].size += back;
        memset((void *)((uintptr_t)start + g_memory_offset), 0, size);
        return start;
    }
    return 0;
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#define XBOX_THREAD_STACK_SIZE  (512 * 1024)
#define XBOX_MAX_THREAD_STACKS  8

static int g_thread_stacks_used = 0;

uint32_t xbox_AllocThreadTib(void)
{

    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_tls_total)
        return 0;

    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + total + g_tls_thread_size, 16);
    if (!tib)
        return 0;
    block       = tib + tib_size;
    thread_data = block + total;

    memcpy(TIB_VA(tib), TIB_VA(XBOX_TIB_MAIN), tib_size);
    memcpy(TIB_VA(block), TIB_VA(g_tls_template_va), total);
    memset(TIB_VA(thread_data), 0, g_tls_thread_size);

    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;
    *(uint32_t *)TIB_VA(block)      = thread_data;
    *(uint32_t *)TIB_VA(tib + 0x04) = block + total;

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;

    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        return 0;
    }

    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    if (!base)
        return 0;
    g_thread_stacks_used++;

    return base + XBOX_THREAD_STACK_SIZE - 16;
}

void xbox_FreeThreadStack(uint32_t stack_top)
{
    if (!stack_top)
        return;
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
}

#define CONTIG_PAGE       4096u
#define CONTIG_PAGES      (XBOX_CONTIG_SIZE / CONTIG_PAGE)
#define CONTIG_USABLE     ((XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT) / CONTIG_PAGE)

#define CONTIG_LOW_RESERVE_START ((15u << 20) / CONTIG_PAGE)
#define CONTIG_LOW_RESERVE_END   ((16u << 20) / CONTIG_PAGE)

#define CONTIG_RESERVED   2u

static uint8_t  g_contig_used[CONTIG_PAGES] = { CONTIG_RESERVED };

static uint32_t g_contig_len[CONTIG_PAGES];
static uint32_t g_contig_pages_in_use;
static uint32_t g_contig_high = CONTIG_PAGE;
static uint32_t g_contig_first_free = 1;
static uint32_t g_contig_allocs, g_contig_frees;
static uint32_t g_contig_peak_pages;
static DWORD    g_contig_t0;
static volatile LONG g_contig_lock;

static void contig_lock(void)
{
    while (InterlockedCompareExchange(&g_contig_lock, 1, 0) != 0)
        SwitchToThread();
}

static void contig_unlock(void)
{
    InterlockedExchange(&g_contig_lock, 0);
}

static void contig_report(const char *what, uint32_t va, uint32_t pages)
{
    static uint32_t last_pages, ops;
    uint32_t delta = g_contig_pages_in_use > last_pages
                   ? g_contig_pages_in_use - last_pages
                   : last_pages - g_contig_pages_in_use;

    if (!g_contig_t0)
        g_contig_t0 = GetTickCount();
    if (++ops > 16 && delta < 512 && (ops & 1023) != 0)
        return;
    last_pages = g_contig_pages_in_use;
    fprintf(stderr, "  [CONTIG] t=%.1fs %s 0x%08X %u KB: in use %u KB, "
            "peak %u KB, high-water %u KB (allocs=%u frees=%u)\n",
            (GetTickCount() - g_contig_t0) / 1000.0, what, va,
            pages * (CONTIG_PAGE / 1024),
            g_contig_pages_in_use * (CONTIG_PAGE / 1024),
            g_contig_peak_pages * (CONTIG_PAGE / 1024),
            g_contig_high / 1024, g_contig_allocs, g_contig_frees);
    fflush(stderr);
}

static void contig_take(uint32_t first, uint32_t n)
{
    uint32_t end = (first + n) * CONTIG_PAGE;

    memset(&g_contig_used[first], 1, n);
    g_contig_len[first] = n;
    g_contig_pages_in_use += n;
    if (g_contig_pages_in_use > g_contig_peak_pages)
        g_contig_peak_pages = g_contig_pages_in_use;
    if (end > g_contig_high)
        g_contig_high = end;
    while (g_contig_first_free < CONTIG_PAGES
           && g_contig_used[g_contig_first_free])
        g_contig_first_free++;
}

static int contig_avoid_image(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CONTIG_AVOID_IMAGE");
        on = e && atoi(e) != 0;
    }
    return on;
}

static int contig_page_free(uint32_t q, int avoid_image)
{
    if (g_contig_used[q])
        return 0;
    return !(avoid_image && g_xbox_image_hi
             && q >= g_xbox_image_lo / CONTIG_PAGE
             && q < (g_xbox_image_hi + CONTIG_PAGE - 1) / CONTIG_PAGE);
}

uint32_t xbox_ContiguousAllocEx(uint32_t size, uint32_t alignment,
                                uint32_t phys_lo, uint32_t phys_hi)
{
    uint32_t n, align_pages, lo_page, limit, p, q, pass, result = 0;

    if (alignment < CONTIG_PAGE) alignment = CONTIG_PAGE;
    if (alignment & (alignment - 1)) {
        uint32_t a = CONTIG_PAGE;
        while (a && a < alignment) a <<= 1;
        alignment = a ? a : 0x80000000u;
    }
    if (size == 0) size = 1;
    n = (uint32_t)(((uint64_t)size + CONTIG_PAGE - 1) / CONTIG_PAGE);
    align_pages = alignment / CONTIG_PAGE;

    lo_page = (uint32_t)(((uint64_t)phys_lo + CONTIG_PAGE - 1) / CONTIG_PAGE);
    limit = (uint32_t)(((uint64_t)phys_hi + 1) / CONTIG_PAGE);
    if (limit > CONTIG_USABLE) limit = CONTIG_USABLE;

    contig_lock();
    if (lo_page < g_contig_first_free) lo_page = g_contig_first_free;
    for (pass = contig_avoid_image() ? 0 : 1; pass < 2 && !result; pass++) {
        int avoid = pass == 0;
        p = (lo_page + align_pages - 1) & ~(align_pages - 1);
        while ((uint64_t)p + n <= limit) {

            if (limit > CONTIG_LOW_RESERVE_END && p < CONTIG_LOW_RESERVE_END
                    && p + n > CONTIG_LOW_RESERVE_START) {
                p = (CONTIG_LOW_RESERVE_END + align_pages - 1) & ~(align_pages - 1);
                continue;
            }
            for (q = p; q < p + n && contig_page_free(q, avoid); q++)
                ;
            if (q == p + n) {
                contig_take(p, n);
                result = XBOX_CONTIG_BASE + p * CONTIG_PAGE;
                break;
            }

            while (q < limit && !contig_page_free(q, avoid)) q++;
            p = (q + align_pages - 1) & ~(align_pages - 1);
        }
    }
    if (result) {
        g_contig_allocs++;
        contig_report("alloc", result, n);
    } else {
        uint32_t best = 0, run = 0;
        for (q = 0; q < CONTIG_USABLE; q++) {
            run = g_contig_used[q] ? 0 : run + 1;
            if (run > best) best = run;
        }
        fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, align %u, "
                "phys 0x%08X..0x%08X; %u of %u KB in use, largest free run "
                "%u KB)\n", size, alignment, phys_lo, phys_hi,
                g_contig_pages_in_use * (CONTIG_PAGE / 1024),
                (unsigned)(CONTIG_USABLE * (CONTIG_PAGE / 1024)),
                best * (CONTIG_PAGE / 1024));
        fflush(stderr);
    }
    contig_unlock();

    if (result)
        memset((void *)((uintptr_t)result + g_memory_offset), 0,
               (size_t)n * CONTIG_PAGE);
    return result;
}

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    return xbox_ContiguousAllocEx(size, alignment, 0, 0xFFFFFFFFu);
}

void xbox_ContiguousClaim(uint32_t va, uint32_t size)
{
    uint32_t first, n, q, run;

    if (va < XBOX_CONTIG_BASE || size == 0)
        return;
    first = (va - XBOX_CONTIG_BASE) / CONTIG_PAGE;
    n = (uint32_t)(((uint64_t)(va & (CONTIG_PAGE - 1)) + size + CONTIG_PAGE - 1)
                   / CONTIG_PAGE);
    if ((uint64_t)first + n > CONTIG_PAGES)
        return;
    contig_lock();
    for (q = first; q < first + n; q += run) {
        if (g_contig_used[q]) { run = 1; continue; }
        for (run = 0; q + run < first + n && !g_contig_used[q + run]; run++)
            ;
        contig_take(q, run);
        g_contig_allocs++;
        contig_report("claim", XBOX_CONTIG_BASE + q * CONTIG_PAGE, run);
    }
    contig_unlock();
}

int xbox_ContiguousFree(uint32_t va)
{
    static int warned;
    uint32_t first, n;

    if (va < XBOX_CONTIG_BASE
            || (uint64_t)va >= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return 0;

    if (!xbox_HeapReclaimEnabled())
        return 1;
    first = (va - XBOX_CONTIG_BASE) / CONTIG_PAGE;

    contig_lock();
    n = (va & (CONTIG_PAGE - 1)) ? 0 : g_contig_len[first];
    if (!n) {
        contig_unlock();
        if (warned++ < 8) {
            fprintf(stderr, "  [CONTIG] free of 0x%08X: not the start of a "
                    "block, ignored\n", va);
            fflush(stderr);
        }
        return 0;
    }
    memset(&g_contig_used[first], 0, n);
    g_contig_len[first] = 0;
    g_contig_pages_in_use -= n;
    if (first < g_contig_first_free)
        g_contig_first_free = first;
    g_contig_frees++;
    contig_report("free", va, n);
    contig_unlock();
    return 1;
}

uint32_t xbox_ContiguousBlockSize(uint32_t va)
{
    uint32_t page, first, result = 0;

    if (va < XBOX_CONTIG_BASE
            || (uint64_t)va >= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return 0;
    page = (va - XBOX_CONTIG_BASE) / CONTIG_PAGE;
    contig_lock();
    if (g_contig_used[page] == 1) {
        first = page;
        while (first > 0 && !g_contig_len[first])
            first--;
        result = XBOX_CONTIG_BASE + (first + g_contig_len[first]) * CONTIG_PAGE
               - va;
    }
    contig_unlock();
    return result;
}

uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return g_contig_high;
}

const volatile uint8_t *xbox_ContiguousPageMap(void)
{
    return g_contig_used;
}

uint32_t xbox_ContiguousInUseBytes(void)
{
    return g_contig_pages_in_use * CONTIG_PAGE;
}

static volatile uint8_t g_phys_ram_named[CONTIG_PAGES];

static int heap_block_at(uint32_t va)
{
    int lo = 0, hi = g_heap_block_count - 1, at = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_heap_blocks[mid].addr <= va) {
            at = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    for (; at >= 0; at--) {
        if (!g_heap_blocks[at].size)
            continue;
        return (uint64_t)va < (uint64_t)g_heap_blocks[at].addr + g_heap_blocks[at].size ? at : -1;
    }
    return -1;
}

void xbox_PhysNoteRamName(uint32_t va)
{
    if (va >= XBOX_HEAP_BASE && va < XBOX_HEAP_TOP && va < XBOX_CONTIG_SIZE) {
        int i;
        g_phys_ram_named[va / CONTIG_PAGE] = 1;

        AcquireSRWLockShared(&g_heap_lock);
        i = heap_block_at(va);
        if (i >= 0 && !g_heap_blocks[i].free)
            g_heap_blocks[i].named = 1;
        ReleaseSRWLockShared(&g_heap_lock);
    }
}

int xbox_PhysRamNamedAt(uint32_t pa)
{
    int i, named;
    if (pa >= XBOX_CONTIG_SIZE || !g_phys_ram_named[pa / CONTIG_PAGE])
        return 0;
    AcquireSRWLockShared(&g_heap_lock);
    i = heap_block_at(pa);
    named = i < 0 || (!g_heap_blocks[i].free && g_heap_blocks[i].named);
    ReleaseSRWLockShared(&g_heap_lock);
    return named;
}

static void heap_forget_ram_names(int m)
{
    uint32_t lo = g_heap_blocks[m].addr;
    int64_t first, last;

    if (!g_heap_blocks[m].size)
        return;
    first = lo / CONTIG_PAGE;
    last = ((uint64_t)lo + g_heap_blocks[m].size - 1) / CONTIG_PAGE;
    for (int j = m - 1; j >= 0; j--) {
        if (!g_heap_blocks[j].size)
            continue;
        if ((uint64_t)g_heap_blocks[j].addr + g_heap_blocks[j].size <=
                (uint64_t)first * CONTIG_PAGE)
            break;
        if (!g_heap_blocks[j].free) {
            first++;
            break;
        }
    }
    for (int j = m + 1; j < g_heap_block_count; j++) {
        if (!g_heap_blocks[j].size)
            continue;
        if ((uint64_t)g_heap_blocks[j].addr >= (uint64_t)(last + 1) * CONTIG_PAGE)
            break;
        if (!g_heap_blocks[j].free) {
            last--;
            break;
        }
    }
    if (last >= (int64_t)CONTIG_PAGES)
        last = (int64_t)CONTIG_PAGES - 1;
    for (int64_t p = first; p <= last; p++)
        g_phys_ram_named[p] = 0;
}

const volatile uint8_t *xbox_PhysRamNameMap(void)
{
    return g_phys_ram_named;
}

static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment);

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t r;

    AcquireSRWLockExclusive(&g_heap_lock);
    r = heap_alloc_locked(size, alignment);
    ReleaseSRWLockExclusive(&g_heap_lock);
    return r;
}

static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (alignment < 4) alignment = 4;

    if (size < 16) size = 16;

    if (xbox_HeapReclaimEnabled()) {

        size = (size + 15) & ~15u;
        result = heap_carve_free(size, alignment);
        if (result)
            return result;
    } else {
        for (int i = 0; i < g_heap_block_count; i++) {
            if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
                continue;
            }
            if (g_heap_blocks[i].addr & (alignment - 1)) {
                continue;
            }
            g_heap_blocks[i].free = 0; g_heap_blocks[i].named = 0;
            result = g_heap_blocks[i].addr;
            memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
            return result;
        }
    }

    if (!g_heap_next) xbox_heap_reset_base();
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));

        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0; g_heap_blocks[g_heap_block_count].named = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;

    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    int i;
    uint32_t r = 0;

    if (!xbox_va)
        return 0;
    AcquireSRWLockShared(&g_heap_lock);
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size) {
            r = g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
            break;
        }
    }
    ReleaseSRWLockShared(&g_heap_lock);
    return r;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    AcquireSRWLockExclusive(&g_heap_lock);
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1; g_heap_blocks[i].named = 0;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        {
            int n = i + 1, p = i - 1, m = i;
            if (xbox_HeapReclaimEnabled()) {
                while (n < g_heap_block_count && g_heap_blocks[n].size == 0) n++;
                while (p >= 0 && g_heap_blocks[p].size == 0) p--;
            }
            if (n < g_heap_block_count && g_heap_blocks[n].free &&
                g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[n].addr) {
                g_heap_blocks[i].size += g_heap_blocks[n].size;
                g_heap_blocks[n].size = 0;
                g_heap_blocks[n].addr = 0;
            }
            if (p >= 0 && g_heap_blocks[p].free &&
                g_heap_blocks[p].addr + g_heap_blocks[p].size == g_heap_blocks[i].addr) {
                g_heap_blocks[p].size += g_heap_blocks[i].size;
                g_heap_blocks[i].size = 0;
                g_heap_blocks[i].addr = 0;
                m = p;
            }

            heap_forget_ram_names(m);
        }
        ReleaseSRWLockExclusive(&g_heap_lock);
        return;
    }
    ReleaseSRWLockExclusive(&g_heap_lock);
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}

