#include "kernel.h"
#include "guest_vmem.h"
#include "kernel_missing.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"
#include "recomp_icall_feedback.h"
#include <stdio.h>

#include <stdlib.h>
#include <float.h>

#include <string.h>
#include <ctype.h>
#include <wctype.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);

int  xbox_worker_stack_alloc(void);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

static int bridge_va_mapped(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    uint64_t mapped = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    if (va < 0x1000)
        return 0;
    if (end <= mapped)
        return 1;
    return va >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

static int bridge_buf_ok(uint32_t va, uint32_t bytes, const char *export_name)
{
    static const char *seen[16];
    static int distinct;
    int i;

    if (!bytes)
        return 1;
    if (va && bridge_va_mapped(va, bytes))
        return 1;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == export_name)
            return 0;
    if (distinct < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[distinct++] = export_name;

    fprintf(stderr,
            "  [KERNEL] %s: buffer 0x%08X length %u is not mapped guest "
            "memory; returning STATUS_ACCESS_VIOLATION\n",
            export_name, va, (unsigned)bytes);
    fflush(stderr);
    return 0;
}

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{

    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;
    }
}

static void kernel_data_init(void)
{

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1;
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1;

    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();

    {
        const char *cmdline = getenv("RECOMP_CMDLINE");

        if (cmdline && *cmdline) {
            uint32_t page = xbox_HeapAlloc(0x1000 + 0x0C00, 4096);
            if (page) {
                size_t n = strlen(cmdline);
                size_t i;

                if (n > 0x0BFF)
                    n = 0x0BFF;
                BRIDGE_MEM32(page + 0) = 3;
                BRIDGE_MEM32(page + 4) = 0x45410091;
                for (i = 0; i < n; i++)
                    BRIDGE_MEM8(page + 0x400 + (uint32_t)i) =
                        (uint8_t)cmdline[i];
                BRIDGE_MEM8(page + 0x400 + (uint32_t)n) = 0;

                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) =
                    page;
                fprintf(stderr, "  Launch data: type 3 at 0x%08X, "
                                "command line %s\n", page, cmdline);
            } else {
                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
            }
        } else {
            BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
        }
    }

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = 0;

    {
        uint32_t channel=XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
        memset(XBOX_TO_NATIVE(channel),0,0x200);
        BRIDGE_MEM32(channel+0x28)=channel+0x28;
        BRIDGE_MEM32(channel+0x2C)=channel+0x28;
    }

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },

            { KDATA_XE_IMAGE_FILENAME, KDATA_XE_IMAGE_BUF,
              "\\Device\\CdRom0\\default.xbe" },
        };
        for (int k = 0; k < (int)(sizeof(d) / sizeof(d[0])); k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);
            BRIDGE_MEM32(str_va + 4) = buf_va;
        }
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

static unsigned long long g_ordinal_calls[XBOX_KERNEL_THUNK_TABLE_SIZE];

static long long g_kernel_call_count = 0;

static long kernel_log_budget(void)
{
    static long budget = -1;

    if (budget < 0) {
        const char *env = getenv("RECOMP_KERNEL_LOG_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 200;
        if (budget < 0)
            budget = 0;
    }
    return budget;
}

#define KERNEL_LOG_ON()      (g_kernel_call_count <= kernel_log_budget())

#define KERNEL_LOG_ON_HALF() (g_kernel_call_count <= kernel_log_budget() / 2)

#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

RECOMP_TLS uint32_t g_xbox_kernel_caller;

static int g_thread_call_count = 0;

static RECOMP_TLS int g_is_spawned_thread = 0;

static RECOMP_TLS uint32_t g_thread_stack_top = 0;

static int g_defer_io_apc = 0;
#define IO_APC_QUEUE 16
static RECOMP_TLS uint32_t g_io_apc[IO_APC_QUEUE][3];
static RECOMP_TLS int g_io_apc_count = 0;

void xbox_SetDeferredIoApc(int on) { g_defer_io_apc = on; }

static int bridge_run_io_apcs(uint32_t alertable, uint32_t wait_mode);

struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top, obj_va, tls_size;
};

static uint32_t bridge_layout_tls(uint32_t kthread, uint32_t top,
                                  uint32_t tls_size)
{
    uint32_t data;

    if (!kthread || !top || !tls_size || (tls_size & 3) || tls_size > 0x10000)
        return 0;
    data = (top - tls_size) & ~0xFu;
    memset(XBOX_TO_NATIVE(data), 0, top - data);
    BRIDGE_MEM32(kthread + 0x28) = data;
    BRIDGE_MEM32(g_fs_base + 0x04) = data + tls_size;
    return data + tls_size;
}

static void bridge_write_handle(uint32_t handle_va, HANDLE h);

#define THOBJ_SIZE        0x200u
#define THOBJ_SIGNAL      0x004u
#define THOBJ_EXIT_STATUS 0x120u
#define THOBJ_MAX         256
#define THOBJ_PER_PAGE    (0x1000u / THOBJ_SIZE)

static uint32_t s_thobj_va[THOBJ_MAX];
static LONG     s_thobj_refs[THOBJ_MAX];
static uint32_t s_thobj_count;
static uint32_t s_token_thobj[16384];
static volatile LONG s_thobj_lock;
static RECOMP_TLS uint32_t g_thread_obj_va;

static void ke_shadow_insert(uint32_t guest_va, HANDLE host);
static void ke_shadow_remove(uint32_t guest_va);

static void thobj_lock(void)
{
    while (InterlockedCompareExchange(&s_thobj_lock, 1, 0) != 0)
        SwitchToThread();
}

static void thobj_unlock(void) { InterlockedExchange(&s_thobj_lock, 0); }

static int thobj_slot(uint32_t va)
{
    uint32_t i;

    if (!va)
        return -1;
    for (i = 0; i < s_thobj_count; i++)
        if (s_thobj_va[i] == va)
            return (int)i;
    return -1;
}

static uint32_t thobj_alloc(void)
{
    uint32_t i, va = 0;

    thobj_lock();
    for (i = 0; i < s_thobj_count; i++)
        if (s_thobj_refs[i] == 0)
            break;
    if (i == s_thobj_count && s_thobj_count < THOBJ_MAX) {
        uint32_t page = xbox_ContiguousAlloc(0x1000, 0x1000);
        uint32_t k;

        if (page) {
            for (k = 0; k < THOBJ_PER_PAGE && s_thobj_count < THOBJ_MAX; k++)
                s_thobj_va[s_thobj_count++] = page + k * THOBJ_SIZE;
        }
    }
    if (i < s_thobj_count) {
        va = s_thobj_va[i];
        s_thobj_refs[i] = 1;
    }
    thobj_unlock();

    if (!va) {
        fprintf(stderr, "  [KERNEL] no guest thread object (pool of %d full)\n",
                THOBJ_MAX);
        fflush(stderr);
        return 0;
    }
    memset(XBOX_TO_NATIVE(va), 0, THOBJ_SIZE);
    BRIDGE_MEM8(va + 0) = 6;
    BRIDGE_MEM8(va + 2) = (uint8_t)(THOBJ_SIZE / 4);
    BRIDGE_MEM32(va + 8)  = va + 8;
    BRIDGE_MEM32(va + 12) = va + 8;
    BRIDGE_MEM32(va + THOBJ_EXIT_STATUS) = 0x103;
    return va;
}

static void thobj_ref(uint32_t va)
{
    int s;

    thobj_lock();
    s = thobj_slot(va);
    if (s >= 0 && s_thobj_refs[s] > 0)
        s_thobj_refs[s]++;
    thobj_unlock();
}

static void thobj_unref(uint32_t va)
{
    int s, freed = 0;

    thobj_lock();
    s = thobj_slot(va);
    if (s >= 0 && s_thobj_refs[s] > 0 && --s_thobj_refs[s] == 0)
        freed = 1;
    thobj_unlock();
    if (freed)
        ke_shadow_remove(va);
}

static void thobj_exit(uint32_t va, uint32_t status)
{
    if (!va)
        return;
    BRIDGE_MEM32(va + THOBJ_EXIT_STATUS) = status;
    InterlockedExchange((volatile LONG *)XBOX_TO_NATIVE(va + THOBJ_SIGNAL), 1);
    thobj_unref(va);
}

static void thobj_attach_token(uint32_t token, uint32_t va)
{
    uint32_t i = token & 0x00FFFFFFu;

    if ((token & 0xFF000000u) != 0x48000000u || !i || i >= 16384 || !va)
        return;
    thobj_ref(va);
    thobj_lock();
    i = InterlockedExchange((volatile LONG *)&s_token_thobj[i], (LONG)va);
    thobj_unlock();
    if (i)
        thobj_unref(i);
}

static uint32_t thobj_from_token(uint32_t token)
{
    uint32_t i = token & 0x00FFFFFFu;

    if ((token & 0xFF000000u) != 0x48000000u || !i || i >= 16384)
        return 0;
    return s_token_thobj[i];
}

static void thobj_detach_token(uint32_t token)
{
    uint32_t i = token & 0x00FFFFFFu, va;

    if ((token & 0xFF000000u) != 0x48000000u || !i || i >= 16384)
        return;
    va = (uint32_t)InterlockedExchange((volatile LONG *)&s_token_thobj[i], 0);
    if (va)
        thobj_unref(va);
}

static uint32_t thobj_current(void)
{
    if (!g_thread_obj_va)
        g_thread_obj_va = thobj_alloc();
    return g_thread_obj_va;
}

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{

    int took = xbox_GuestCpuEnterCallback();

    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    fn();
    g_esp += 12;
    xbox_GuestCpuLeaveCallback(took);
}

void guest_cpu_join(void);
void guest_cpu_part(void);

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    g_thread_obj_va = s->obj_va;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (tib)
            g_fs_base = tib;
        else
            fprintf(stderr, "  [KERNEL] worker thread has no TIB of its own;"
                            " it shares the main thread's\n");

        if (tib && s->obj_va && s->tls_size) {
            uint32_t top = (s->stack_top + 16) & ~0xFu;
            BRIDGE_MEM32(tib + 0x28) = s->obj_va;
            if (bridge_layout_tls(s->obj_va, top, s->tls_size))
                g_esp = ((top - s->tls_size) & ~0xFu) - 16;
        }
    }
    free(s);

    xbox_DpcMarkGuestThread();
    xbox_GuestThreadPin("guest-worker");
    guest_cpu_join();
    bridge_run_thread_inline(fn, ctx1, ctx2);
    guest_cpu_part();

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);

    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;

    thobj_exit(g_thread_obj_va, 0);
    g_thread_obj_va = 0;
    return 0;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top,
                                  uint32_t obj_va, uint32_t tls_size)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;
    s->obj_va = obj_va;
    s->tls_size = tls_size;

    th = CreateThread(NULL, 0, bridge_thread_main, s, 0, NULL);
    if (!th) free(s);

    else xbox_set_game_thread(th);
    return th;
}

static HANDLE bridge_spawn_guest_thread(recomp_func_t fn, uint32_t ctx1,
                                        uint32_t ctx2, uint32_t stack_top,
                                        uint32_t handle_va, uint32_t tls_size)
{
    uint32_t obj = thobj_alloc();
    HANDLE th, shadow = NULL;

    thobj_ref(obj);
    th = bridge_spawn_thread(fn, ctx1, ctx2, stack_top, obj, tls_size);
    if (!th) {
        thobj_unref(obj);
    } else {
        if (obj && DuplicateHandle(GetCurrentProcess(), th, GetCurrentProcess(),
                                   &shadow, 0, FALSE, DUPLICATE_SAME_ACCESS))
            ke_shadow_insert(obj, shadow);
        if (handle_va) {
            bridge_write_handle(handle_va, th);
            thobj_attach_token(BRIDGE_MEM32(handle_va), obj);
        }
    }
    thobj_unref(obj);
    return th;
}

static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t tls_size        = STACK_ARG(3);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);

    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);

    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;
    }

    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {

                if (tls_size) {
                    uint32_t blk = xbox_HeapAlloc((tls_size + 0xFu) & ~0xFu, 16);
                    uint32_t fs4 = blk ? bridge_layout_tls(
                        BRIDGE_MEM32(g_fs_base + 0x28),
                        blk + ((tls_size + 0xFu) & ~0xFu), tls_size) : 0;
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: %u-byte"
                            " TLS at 0x%08X, fs:[4] = 0x%08X\n", tls_size,
                            fs4 ? fs4 - tls_size : 0, fs4);
                    fflush(stderr);
                }
                xbox_DpcMarkGuestThread();
                xbox_GuestThreadPin("guest-main");
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;

                {
                    int took = xbox_GuestCpuEnterCallback();
                    fn();
                    xbox_GuestCpuLeaveCallback(took);
                }
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {

                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    fprintf(stderr, "  [KERNEL] RECOMP_WORKERS=inline: running "
                            "worker 0x%08X on this thread\n", start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    return;
                }

                stack_top = xbox_AllocThreadStack();

                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: out of "
                            "thread stacks, running worker 0x%08X inline\n",
                            start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    bridge_spawn_guest_thread(fn, start_context1, start_context2,
                                              stack_top, xbox_handle_ptr,
                                              tls_size);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X)\n",
                            start_routine, start_context1, stack_top);
                    fflush(stderr);
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0;
}

static void bridge_note_async_handle(uint32_t token);
static void bridge_forget_async_handle(uint32_t token);
static int  bridge_handle_is_async(uint32_t token);

static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    bridge_forget_async_handle(raw_handle);
    thobj_detach_token(raw_handle);

    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE)
            xbox_NtClose(h);
    }
    g_eax = 0;
}

static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    uint32_t xbox_va = xbox_ContiguousAlloc(size, 4096);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_MmAllocateSystemMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t prot = STACK_ARG(1);
    uint32_t xbox_va;

    (void)prot;
    if (!size) {
        g_eax = 0;
        return;
    }
    xbox_va = xbox_HeapAlloc(size, 4096);
    if (xbox_va)
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);
    else
        fprintf(stderr, "  [KERNEL] MmAllocateSystemMemory: %u bytes REFUSED\n",
                size);
    g_eax = xbox_va;
}

#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);

        xbox_ContiguousClaim(xbox_va, size);

        if (KERNEL_LOG_ON_HALF()) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;

    if (high < low) {
        g_eax = 0;
        return;
    }
    xbox_va = xbox_ContiguousAllocEx(size, align, low, high);

    if (!xbox_va) {
        static int n;
        if (n++ < 8) {
            int k;
            fprintf(stderr, "  [CONTIG] refused: caller 0x%08X size=%u low=0x%08X "
                    "high=0x%08X align=%u prot=0x%X stack:", g_xbox_kernel_caller,
                    size, low, high, align, prot);
            for (k = 5; k < 40; k++)
                fprintf(stderr, " %08X", STACK_ARG(k));
            fprintf(stderr, "\n");
        }
    }

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);

    if (!xbox_ContiguousFree(addr))
        xbox_HeapFree(addr);
    g_eax = 0;
}

static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;

    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (base_ptr && size_ptr) {
        uint32_t vm_base = base_hint;
        uint32_t vm_size = size;
        uint32_t vm_status;

        if (guest_vmem_allocate(&vm_base, &vm_size, alloc_type, protect, &vm_status)) {
            if (KERNEL_LOG_ON()) {
                fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory (extended VMA): "
                                "base=0x%08X size=%u status=0x%08X\n",
                        vm_base, vm_size, vm_status);
                fflush(stderr);
            }
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    if (size == 0) {
        g_eax = 0xC0000045u;
        return;
    }

    if (base_hint >= (g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram)) {
        static unsigned warned;
        if (warned++ < 8)
            fprintf(stderr,
                    "  [KERNEL] WARNING: allocation at 0x%08X is above "
                    "%u MB mapped; it aliases 0x%08X\n",
                    base_hint,
                    (unsigned)((g_xbox_map_size ? g_xbox_map_size
                                                : g_xbox_total_ram)
                               / (1024 * 1024)),
                    (uint32_t)(base_hint % (g_xbox_map_size
                                            ? g_xbox_map_size
                                            : g_xbox_total_ram)));
        fflush(stderr);
    }

    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {

        if (KERNEL_LOG_ON()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0;
        return;
    }

    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {

        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        uint32_t want = size;
        while (want > 0x10000 && !xbox_va) {
            want /= 2;
            xbox_va = xbox_HeapAlloc(want, 4096);
        }
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    size, want);
            fflush(stderr);
            size = want;
        }
    }
    if (!xbox_va) {
        g_eax = 0xC0000017u;
        return;
    }

    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0;
}

static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t info_va = STACK_ARG(1);
    uint32_t page_base = base_va & ~0xFFFu;

    if (!info_va) {
        g_eax = 0xC000000Du;
        return;
    }

    {
        uint32_t vm_info[7];

        if (guest_vmem_query(base_va, vm_info)) {
            BRIDGE_MEM32(info_va + 0x00) = vm_info[0];
            BRIDGE_MEM32(info_va + 0x04) = vm_info[1];
            BRIDGE_MEM32(info_va + 0x08) = vm_info[2];
            BRIDGE_MEM32(info_va + 0x0C) = vm_info[3];
            BRIDGE_MEM32(info_va + 0x10) = vm_info[4];
            BRIDGE_MEM32(info_va + 0x14) = vm_info[5];
            BRIDGE_MEM32(info_va + 0x18) = vm_info[6];
            g_eax = 0;
            return;
        }
    }

    BRIDGE_MEM32(info_va + 0x00) = page_base;
    BRIDGE_MEM32(info_va + 0x04) = page_base;
    BRIDGE_MEM32(info_va + 0x08) = 0x04;
    BRIDGE_MEM32(info_va + 0x14) = 0x04;
    BRIDGE_MEM32(info_va + 0x18) = 0x20000;

    if (page_base >= g_xbox_code_lo && page_base < XBOX_TOTAL_RAM) {
        BRIDGE_MEM32(info_va + 0x0C) = XBOX_TOTAL_RAM - page_base;
        BRIDGE_MEM32(info_va + 0x10) = 0x1000;
    } else {

        uint32_t region = 0x1000;

        if (guest_vmem_active()) {

            uint32_t end = 0;

            if (page_base < g_xbox_code_lo)
                end = g_xbox_code_lo;
            else if (page_base >= GUEST_VMEM_TOP)
                end = 0xFFFFFFFFu;
            if (end > page_base)
                region = end - page_base;
        }
        BRIDGE_MEM32(info_va + 0x0C) = region;
        BRIDGE_MEM32(info_va + 0x10) = 0x10000;
        BRIDGE_MEM32(info_va + 0x08) = 0;
        BRIDGE_MEM32(info_va + 0x18) = 0;
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: base=0x%08X -> "
                        "state=0x%X size=%u\n", base_va,
                        BRIDGE_MEM32(info_va + 0x10),
                        BRIDGE_MEM32(info_va + 0x0C));
        fflush(stderr);
    }
    g_eax = 0;
}

#define BRIDGE_MEM_DECOMMIT 0x4000u
#define BRIDGE_MEM_RELEASE  0x8000u
#define BRIDGE_FREE_VM_NOT_AT_BASE 0xC000009Fu

static int bridge_heap_interior(uint32_t va, uint32_t left)
{
    return left && va && xbox_HeapBlockSize(va - 1) == left + 1;
}

static void bridge_free_guest_pages(uint32_t base_ptr, uint32_t size_ptr,
                                    uint32_t free_type)
{
    static unsigned long decommits, releases, refused;
    static uint64_t decommit_bytes;
    uint32_t base, size, page_base, end, left;
    uint32_t mapped = (uint32_t)(g_xbox_map_size ? g_xbox_map_size
                                                 : g_xbox_total_ram);

    if (!base_ptr || !size_ptr || !bridge_va_mapped(base_ptr, 4)
            || !bridge_va_mapped(size_ptr, 4)) {
        g_eax = 0xC0000005u;
        return;
    }
    base = BRIDGE_MEM32(base_ptr);
    size = BRIDGE_MEM32(size_ptr);

    if ((free_type != BRIDGE_MEM_DECOMMIT && free_type != BRIDGE_MEM_RELEASE)
            || (free_type == BRIDGE_MEM_RELEASE && size != 0)) {
        g_eax = 0xC00000F0u;
        if (refused++ < 8) {
            fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X "
                    "size=%u type=0x%X refused (bad type/size)\n",
                    base, size, free_type);
            fflush(stderr);
        }
        return;
    }

    page_base = base & ~0xFFFu;
    if (page_base < XBOX_HEAP_BASE || page_base >= mapped) {
        g_eax = 0xC00000A0u;
        if (refused++ < 8) {
            fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X "
                    "type=0x%X outside guest memory\n", base, free_type);
            fflush(stderr);
        }
        return;
    }

    left = xbox_HeapBlockSize(page_base);
    if (free_type == BRIDGE_MEM_RELEASE) {
        if (base != page_base || bridge_heap_interior(page_base, left)) {
            g_eax = BRIDGE_FREE_VM_NOT_AT_BASE;
            if (refused++ < 8) {
                fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X "
                        "release inside a block refused\n", base);
                fflush(stderr);
            }
            return;
        }
        size = left;
        xbox_HeapFree(page_base);
        releases++;
    } else {

        if (!size)
            size = left;
        end = (uint32_t)(((uint64_t)base + size + 0xFFFu) & ~(uint64_t)0xFFFu);
        if (end > mapped || end < page_base)
            end = mapped;

        if (left && end - page_base > left)
            end = page_base + left;
        size = end - page_base;
        if (size)
            memset(XBOX_TO_NATIVE(page_base), 0, size);
        decommits++;
        decommit_bytes += size;
    }

    BRIDGE_MEM32(base_ptr) = page_base;
    BRIDGE_MEM32(size_ptr) = size;
    g_eax = 0;

    if (KERNEL_LOG_ON() || decommits + releases <= 4
            || ((decommits + releases) & 0x3FFFu) == 0) {
        fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X size=%u "
                "type=0x%X (decommits=%lu/%llu KB releases=%lu)\n",
                page_base, size, free_type, decommits,
                (unsigned long long)(decommit_bytes / 1024), releases);
        fflush(stderr);
    }
}

static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    if (base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t vm_size = BRIDGE_MEM32(size_ptr);
        uint32_t vm_status;

        if (guest_vmem_free(&vm_base, &vm_size, free_type, &vm_status)) {
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    if (xbox_HeapReclaimEnabled() && base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t left = xbox_HeapBlockSize(vm_base);

        if (left) {
            if (free_type & 0x8000) {
                if (bridge_heap_interior(vm_base, left)) {
                    g_eax = BRIDGE_FREE_VM_NOT_AT_BASE;
                    return;
                }
                xbox_HeapFree(vm_base);

                BRIDGE_MEM32(size_ptr) = left;
            } else {

                uint32_t vm_size = BRIDGE_MEM32(size_ptr);
                uint32_t n = (vm_size && vm_size < left) ? vm_size : left;
                uint32_t page_base = vm_base & ~0xFFFu;
                uint64_t end = ((uint64_t)vm_base + n + 0xFFFu) & ~(uint64_t)0xFFFu;

                if (end > (uint64_t)vm_base + left)
                    end = (uint64_t)vm_base + left;
                if (xbox_HeapBlockSize(page_base) != left + (vm_base - page_base))
                    page_base = vm_base;
                memset(XBOX_TO_NATIVE(page_base), 0, (size_t)(end - page_base));
                BRIDGE_MEM32(base_ptr) = page_base;
                BRIDGE_MEM32(size_ptr) = (uint32_t)(end - page_base);
            }
            g_eax = 0;
            return;
        }
    }

    bridge_free_guest_pages(base_ptr, size_ptr, free_type);
}

#define XBOX_FSCACHE_DEFAULT_PAGES 16u

static uint32_t g_fscache_pages = XBOX_FSCACHE_DEFAULT_PAGES;

static void bridge_FscGetCacheSize(void)
{
    g_eax = g_fscache_pages;
}

static void bridge_FscSetCacheSize(void)
{
    uint32_t pages = STACK_ARG(0);

    g_fscache_pages = pages;
    g_eax = STATUS_SUCCESS;
}

static void bridge_RtlCompareMemory(void)
{
    uint32_t a_va   = STACK_ARG(0);
    uint32_t b_va   = STACK_ARG(1);
    uint32_t length = STACK_ARG(2);
    const uint8_t *a, *b;
    uint32_t i;

    if (!a_va || !b_va || !length) {
        g_eax = 0;
        return;
    }
    a = (const uint8_t *)XBOX_TO_NATIVE(a_va);
    b = (const uint8_t *)XBOX_TO_NATIVE(b_va);
    for (i = 0; i < length; i++) {
        if (a[i] != b[i])
            break;
    }
    g_eax = i;
}

static void bridge_RtlCompareMemoryUlong(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t length  = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!base_va) {
        g_eax = 0;
        return;
    }
    length &= ~3u;
    for (i = 0; i < length; i += 4) {
        if (BRIDGE_MEM32(base_va + i) != pattern)
            break;
    }
    g_eax = i;
}

static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);

    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);

        if (page) {
            char path[64];
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                uint8_t c = BRIDGE_MEM8(page + 8 + i);
                if (!c) break;
                path[i] = (char)c;
            }
            path[i] = 0;
            fprintf(stderr, "  [KERNEL] launch data page 0x%08X:"
                            " type=%u titleid=0x%08X path='%s'\n",
                    page, BRIDGE_MEM32(page), BRIDGE_MEM32(page + 4), path);

            fprintf(stderr, "  [KERNEL]   launch data:");
            for (i = 0; i < 8; i++)
                fprintf(stderr, " %08X", BRIDGE_MEM32(page + 1024 + i * 4));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [KERNEL] no launch data page set\n");
        }
    }

    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t i;

        fprintf(stderr, "  [KERNEL] exit requested, guest esp=0x%08X:\n", g_esp);
        for (i = 0; i < 200; i++) {
            uint32_t a = g_esp + i * 4;
            if (a < 0x00010000u || a >= 0x04000000u) break;
            fprintf(stderr, "    GS %08X %08X\n", a,
                    *(const uint32_t *)(mem + a));
        }
        fflush(stderr);
    }

    xbox_PeekSample("exit peek");
    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is exiting\n",
            routine);
    fflush(stderr);

    RECOMP_ICALL_FEEDBACK_DUMP();

    xbox_missing_summary("firmware");

    {
        extern int xbox_VideoIsPlaying(void);
        int waited = 0;

        while (xbox_VideoIsPlaying() && waited < 60000) {
            Sleep(50);
            waited += 50;
        }
        if (waited)
            fprintf(stderr, "  [KERNEL] waited %dms for the video to finish\n",
                    waited);
    }

    xbox_HalReturnToFirmware(routine);
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_KfRaiseIrql(void)
{
    uint32_t new_irql = g_ecx;
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = g_ecx;
    xbox_KfLowerIrql((UCHAR)new_irql);
    g_eax = 0;
}

static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

static HANDLE ke_shadow_lookup(uint32_t guest_va);
static void ke_shadow_insert(uint32_t guest_va, HANDLE host);
static HANDLE bridge_resolve_handle(uint32_t token);
static HANDLE ke_guest_event(uint32_t guest_va, int *type);

static HANDLE ke_guest_event_prepare(uint32_t guest_va, int *type)
{
    HANDLE h = ke_guest_event(guest_va, type);
    if (h) {
        if (BRIDGE_MEM32(guest_va + 4) == 0) {
            ResetEvent(h);
            if (BRIDGE_MEM32(guest_va + 4) != 0)
                SetEvent(h);
        } else {
            SetEvent(h);
        }
    }
    return h;
}

void (*volatile nv2a_sw_method_hook)(uint32_t);

void xbox_KeSetGuestEvent(uint32_t guest_va)
{
    int type;
    HANDLE ge = ke_guest_event(guest_va, &type);
    HANDLE h;

    if (ge) {
        BRIDGE_MEM32(guest_va + 4) = 1;
        SetEvent(ge);
        return;
    }
    h = ke_shadow_lookup(guest_va);
    if (h)
        SetEvent(h);
}

static void bridge_KeSetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            g_eax = BRIDGE_MEM32(guest_va + 4) != 0;
            BRIDGE_MEM32(guest_va + 4) = 1;
            SetEvent(ge);
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = bridge_resolve_handle(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        g_eax = (uint32_t)SetEvent(h);
    else
        g_eax = 0;
}

static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    HANDLE h;

    {

        int type;
        HANDLE ge = ke_guest_event_prepare(object, &type);
        if (ge) {
            g_eax = (uint32_t)xbox_KeWaitForSingleObject(
                ge, wait_reason, wait_mode,
                (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
            if (g_eax == 0 && type == 1)
                BRIDGE_MEM32(object + 4) = 0;
            return;
        }
    }

    h = ke_shadow_lookup(object);
    if (!h)
        h = bridge_resolve_handle(object);
    if (!h)
        h = XBOX_TO_NATIVE(object);

    g_eax = (uint32_t)xbox_KeWaitForSingleObject(
        h, wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);

    if (bridge_run_io_apcs(alertable, 1))
        return;
    g_eax = (uint32_t)xbox_NtWaitForSingleObject(
        handle, (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    g_eax = (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev));
}

static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    if (handle) PulseEvent(handle);
    g_eax = 0;
}

static HANDLE bridge_resolve_handle(uint32_t token);

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
        handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    uint32_t wait_mode   = STACK_ARG(3);
    uint32_t alertable   = STACK_ARG(4);
    uint32_t timeout_ptr = STACK_ARG(5);

    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handles_va) {
        g_eax = 0xC000000Du;
        return;
    }
    for (i = 0; i < count; i++)
        handles[i] = bridge_resolve_handle(BRIDGE_MEM32(handles_va + i * 4));

    {
        static int logged;
        if (logged++ < 20) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx: count=%u type=%u timeout=%s\n",
                    count, wait_type, timeout_ptr ? "finite" : "INFINITE");
            for (i = 0; i < count; i++)
                fprintf(stderr, "      [%u] token=0x%08X host=%p\n", i,
                        BRIDGE_MEM32(handles_va + i * 4), handles[i]);
            fflush(stderr);
        }
    }

    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    g_eax = (uint32_t)xbox_NtWaitForMultipleObjectsEx(
        count, handles, wait_type, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);

    g_eax = address ? (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address))
                    : 0;
}

typedef struct { uint32_t eax, ecx, edx, ebx, esi, edi, esp; } CallbackRegs;

static void callback_regs_save(CallbackRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx;
    r->ebx = g_ebx; r->esi = g_esi; r->edi = g_edi; r->esp = g_esp;
}

static void callback_regs_restore(const CallbackRegs *r, int keep_eax)
{
    if (!keep_eax)
        g_eax = r->eax;
    g_ecx = r->ecx; g_edx = r->edx;
    g_ebx = r->ebx; g_esi = r->esi; g_edi = r->edi; g_esp = r->esp;
}

static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    CallbackRegs saved;
    callback_regs_save(&saved);
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    {

        int took = xbox_GuestCpuEnterCallback();
        fn();
        xbox_GuestCpuLeaveCallback(took);
    }
    callback_regs_restore(&saved, 0);

    g_eax = 0;
}

static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);

    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(interval_ptr));
}

static void bridge_KeBugCheck(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheck: code=0x%08X ***\n",
            STACK_ARG(0));
    fflush(stderr);
    g_eax = 0;
}

static void bridge_KeBugCheckEx(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheckEx: code=0x%08X "
            "(0x%08X, 0x%08X, 0x%08X, 0x%08X) ***\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
            STACK_ARG(3), STACK_ARG(4));
    fflush(stderr);
    g_eax = 0;
}

static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);

    g_eax = (uint32_t)xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)addr);
}

static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    fprintf(stderr, "  [AV] SetDisplayMode mode=0x%08X format=0x%08X"
                    " pitch=%u fb=0x%08X\n", mode, format, pitch, fb);
    fflush(stderr);

    {

        extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
        extern void xbox_FramebufferWindowStart(void);
        uint32_t fb_va = fb;

        if (fb_va && fb_va < XBOX_CONTIG_SIZE)
            fb_va = XBOX_CONTIG_BASE + fb_va;

        xbox_FramebufferWindowSet(fb_va, pitch);
        xbox_FramebufferWindowStart();

        xbox_SetDisplayFramebuffer(fb_va, pitch);
    }
    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
            exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
    fflush(stderr);

    g_eax = exit_status;

    if (g_is_spawned_thread) {

        xbox_FreeThreadStack(g_thread_stack_top);
        g_thread_stack_top = 0;
        thobj_exit(g_thread_obj_va, exit_status);
        g_thread_obj_va = 0;
        ExitThread(exit_status);
    }
}

static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

static int kernel_run_dpc(uint32_t dpc_va, uint32_t arg1, uint32_t arg2)
{
    uint32_t routine, context;
    recomp_func_t fn;

    if (!dpc_va)
        return 0;
    routine = BRIDGE_MEM32(dpc_va + 12);
    context = BRIDGE_MEM32(dpc_va + 16);
    if (!routine)
        return 0;

    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] DPC routine 0x%08X not in dispatch\n",
                routine);
        fflush(stderr);
        return 0;
    }

    BRIDGE_MEM32(dpc_va + 20) = arg1;
    BRIDGE_MEM32(dpc_va + 24) = arg2;

    {
        CallbackRegs saved;
        callback_regs_save(&saved);
        g_esp -= 4; BRIDGE_MEM32(g_esp) = arg2;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = arg1;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        { int _irql = xbox_IrqlEnterInterrupt(2); fn(); xbox_IrqlLeaveInterrupt(_irql); }
        callback_regs_restore(&saved, 0);
    }
    return 1;
}

static void bridge_KeSynchronizeExecution(void)
{
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);
    recomp_func_t fn;

    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn && routine) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: routine 0x%08X "
                        "not in dispatch\n", routine);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    CallbackRegs saved;
    callback_regs_save(&saved);
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;

    xbox_DispatchGateEnter();
    {
        int took = xbox_GuestCpuEnterCallback();
        fn();
        xbox_GuestCpuLeaveCallback(took);
    }
    xbox_DispatchGateLeave();

    callback_regs_restore(&saved, 1);
}

static void bridge_KeRemoveQueueDpc(void);

#define XBOX_MAX_PENDING_DPC 64
typedef struct { uint32_t dpc, arg1, arg2; } PendingDpc;
static PendingDpc g_dpc_queue[XBOX_MAX_PENDING_DPC];
static volatile LONG g_dpc_head, g_dpc_tail;

static CRITICAL_SECTION g_dpc_lock;
static INIT_ONCE g_dpc_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dpc_lock_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_dpc_lock);
    return TRUE;
}

static void dpc_lock(void)
{
    InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
    EnterCriticalSection(&g_dpc_lock);
}

int xbox_DpcQueue(uint32_t dpc, uint32_t arg1, uint32_t arg2)
{
    LONG tail, next;

    if (!dpc)
        return 0;

    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {
        LeaveCriticalSection(&g_dpc_lock);
        return 0;
    }
    tail = g_dpc_tail;
    next = (tail + 1) % XBOX_MAX_PENDING_DPC;
    if (next == g_dpc_head) {
        LeaveCriticalSection(&g_dpc_lock);
        fprintf(stderr, "  [KERNEL] DPC queue full, dropping 0x%08X\n", dpc);
        fflush(stderr);
        return 0;
    }
    g_dpc_queue[tail].dpc  = dpc;
    g_dpc_queue[tail].arg1 = arg1;
    g_dpc_queue[tail].arg2 = arg2;
    BRIDGE_MEM8(dpc + 2) = 1;
    g_dpc_tail = next;
    LeaveCriticalSection(&g_dpc_lock);

    if (xbox_DpcOnRaise())
        xbox_HostTimerWake();
    return 1;
}

int xbox_DpcPending(void)
{
    return g_dpc_head != g_dpc_tail;
}

static void bridge_KeInsertQueueDpc(void)
{
    g_eax = (uint32_t)xbox_DpcQueue(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2));
}

static void bridge_KeRemoveQueueDpc(void)
{
    uint32_t dpc = STACK_ARG(0);
    LONG i;

    g_eax = 0;
    if (!dpc)
        return;
    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {
        for (i = g_dpc_head; i != g_dpc_tail; i = (i + 1) % XBOX_MAX_PENDING_DPC)
            if (g_dpc_queue[i].dpc == dpc)
                g_dpc_queue[i].dpc = 0;
        BRIDGE_MEM8(dpc + 2) = 0;
        g_eax = 1;
    }
    LeaveCriticalSection(&g_dpc_lock);
}

uint32_t xbox_GetConnectedInterrupt(uint32_t vector);

static int kernel_call_isr(uint32_t vector)
{
    uint32_t kint = xbox_GetConnectedInterrupt(vector);
    uint32_t routine, context;
    recomp_func_t fn;

    if (!kint)
        return -1;
    routine = BRIDGE_MEM32(kint + 0);
    context = BRIDGE_MEM32(kint + 4);
    if (!routine)
        return -1;
    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn)
        return -1;

    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = kint;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    { int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql); }
    return (int)(g_eax & 1u);
}

#define XBOX_NV2A_REG_BASE     0xFD000000u
#define NV2A_PMC_INTR_0        0x00000100u
#define NV2A_PMC_INTR_PCRTC    (1u << 24)
#define NV2A_PMC_INTR_EN_0     0x00000140u
#define NV2A_PCRTC_INTR_0      0x00600100u
#define NV2A_PCRTC_INTR_VBLANK (1u << 0)
#define NV2A_PCRTC_INTR_EN_0   0x00600140u
#define NV2A_VECTOR            3u

static long long kernel_clock_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (!freq.QuadPart && !QueryPerformanceFrequency(&freq))
        return (long long)GetTickCount64();
    QueryPerformanceCounter(&now);
    return (long long)(now.QuadPart / freq.QuadPart) * 1000
           + (long long)(now.QuadPart % freq.QuadPart) * 1000 / freq.QuadPart;
}

enum { VBL_ACK_FAST_MS = 2, VBL_ACK_LATE_MS = 1000, VBL_ACK_SLOW_STEP_MS = 16 };

int xbox_VblankAckWait(volatile uint32_t *pcrtc_intr, volatile uint32_t *pmc_intr,
                       volatile LONG *tick_busy, HANDLE rearm, long *late_ms)
{
    long long start = kernel_clock_ms(), now = start;

    *late_ms = 0;
    for (;;) {
        DWORD step;

        if (!*tick_busy && (*pcrtc_intr & NV2A_PCRTC_INTR_VBLANK)) {
            InterlockedAnd((volatile LONG *)pcrtc_intr,
                           (LONG)~NV2A_PCRTC_INTR_VBLANK);
            InterlockedAnd((volatile LONG *)pmc_intr,
                           (LONG)~NV2A_PMC_INTR_PCRTC);
            if (now - start <= VBL_ACK_FAST_MS)
                return XBOX_VBL_ACK_ON_TIME;
            *late_ms = (long)(now - start);
            return XBOX_VBL_ACK_LATE;
        }
        if (!*tick_busy && !(*pmc_intr & NV2A_PMC_INTR_PCRTC))
            return XBOX_VBL_ACK_NONE;
        if (now - start <= VBL_ACK_FAST_MS) {
            SwitchToThread();
            now = kernel_clock_ms();
            continue;
        }
        step = now - start <= VBL_ACK_LATE_MS ? 1 : VBL_ACK_SLOW_STEP_MS;
        if (WaitForSingleObject(rearm, step) == WAIT_OBJECT_0) {

            return XBOX_VBL_ACK_REARMED;
        }
        now = kernel_clock_ms();
    }
}

static HANDLE g_vbl_ack_event;

static volatile LONG g_vbl_ack_giveups;
static volatile LONG g_vbl_ack_late, g_vbl_ack_late_max;

static volatile LONG g_vbl_declined;

static volatile LONG g_vbl_tick_busy;

static volatile LONG g_vbl_ack_seen_ms;

static DWORD WINAPI kernel_vblank_ack_thread(LPVOID unused)
{
    int r = XBOX_VBL_ACK_ON_TIME;

    (void)unused;
    for (;;) {
        long late_ms;

        if (r != XBOX_VBL_ACK_REARMED)
            WaitForSingleObject(g_vbl_ack_event, INFINITE);
        r = xbox_VblankAckWait(
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0),
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_0),
            &g_vbl_tick_busy, g_vbl_ack_event, &late_ms);

        if (r != XBOX_VBL_ACK_REARMED)
            InterlockedExchange(&g_vbl_ack_seen_ms, (LONG)(uint32_t)GetTickCount64());
        if (r == XBOX_VBL_ACK_LATE) {
            InterlockedIncrement(&g_vbl_ack_late);
            if (late_ms > g_vbl_ack_late_max)
                InterlockedExchange(&g_vbl_ack_late_max, late_ms);
        } else if (r != XBOX_VBL_ACK_ON_TIME) {
            InterlockedIncrement(&g_vbl_ack_giveups);
        }
    }
    return 0;
}

static volatile LONG g_vbl_ack_owned;

int xbox_VblankAckHeld(void)
{
    if (!g_vbl_ack_owned)
        return 0;
    if (g_vbl_tick_busy)
        return 1;
    return (uint32_t)GetTickCount64() - (uint32_t)g_vbl_ack_seen_ms
           <= VBL_ACK_LATE_MS;
}

void xbox_VblankArmBits(volatile uint32_t *pcrtc_intr, volatile LONG *tick_busy,
                        int raised)
{
    if (raised)
        InterlockedAnd((volatile LONG *)pcrtc_intr, (LONG)~NV2A_PCRTC_INTR_VBLANK);
    InterlockedExchange(tick_busy, 0);
}

static INIT_ONCE g_vbl_ack_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK kernel_vblank_ack_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    HANDLE t;

    (void)o; (void)p; (void)c;
    g_vbl_ack_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_vbl_ack_event)
        return FALSE;
    InterlockedExchange(&g_vbl_ack_seen_ms, (LONG)(uint32_t)kernel_clock_ms());
    t = CreateThread(NULL, 0, kernel_vblank_ack_thread, NULL, 0, NULL);
    if (!t) {
        CloseHandle(g_vbl_ack_event);
        g_vbl_ack_event = NULL;
        return FALSE;
    }
    CloseHandle(t);
    InterlockedExchange(&g_vbl_ack_owned, 1);
    return TRUE;
}

static void kernel_vblank_arm(int raised)
{
    if (!InitOnceExecuteOnce(&g_vbl_ack_once, kernel_vblank_ack_init, NULL, NULL)) {
        InterlockedExchange(&g_vbl_tick_busy, 0);
        return;
    }
    xbox_VblankArmBits(&BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0),
                       &g_vbl_tick_busy, raised);
    SetEvent(g_vbl_ack_event);
}

int xbox_VblankMode(const char *env, int paced_wanted)
{
    if (!env || !*env || !strcmp(env, "0"))
        return XBOX_VBLANK_OFF;
    if (paced_wanted || !strcmp(env, "paced"))
        return XBOX_VBLANK_PACED;
    return XBOX_VBLANK_TICK;
}

extern int nv2a_pb_exec_flip_pacing(void);

static int kernel_vblank_mode(void)
{
    static volatile LONG mode = -1;

    if (mode < 0)
        InterlockedExchange(&mode, xbox_VblankMode(getenv("RECOMP_VBLANK"),
                                                   xbox_DispatchGateOn()
                                                   || nv2a_pb_exec_flip_pacing()));
    return (int)mode;
}

static volatile LONG g_vblank_total;

uint32_t xbox_VblankCount(void)
{
    return (uint32_t)g_vblank_total;
}

static void kernel_vblank_count(long long now)
{
    static unsigned long long total;
    static long long last_ms;
    static LARGE_INTEGER qf, qlast;
    static double gap_min = 1e9, gap_max;
    LARGE_INTEGER q;

    InterlockedIncrement(&g_vblank_total);
    if (!qf.QuadPart)
        QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q);
    if (qlast.QuadPart && qf.QuadPart) {
        double g = (double)(q.QuadPart - qlast.QuadPart) * 1000.0
                   / (double)qf.QuadPart;
        if (g < gap_min) gap_min = g;
        if (g > gap_max) gap_max = g;
    }
    qlast = q;
    if (++total % 600 == 0) {
        fprintf(stderr, "  [NV2A] vblank %llu: last 600 in %lld ms,"
                " gap %.1f-%.1f ms, ack give-ups %ld, late acks %ld"
                " (max %ld ms), declined %ld\n",
                total, last_ms ? now - last_ms : 0, gap_min, gap_max,
                (long)InterlockedExchange(&g_vbl_ack_giveups, 0),
                (long)InterlockedExchange(&g_vbl_ack_late, 0),
                (long)InterlockedExchange(&g_vbl_ack_late_max, 0),
                (long)InterlockedExchange(&g_vbl_declined, 0));
        fflush(stderr);
        xbox_IrqReport();
        xbox_GuestCpuReport();
        gap_min = 1e9;
        gap_max = 0;
        last_ms = now;
    } else if (total == 1) {
        last_ms = now;
    }
}

static void kernel_vblank_deliver(void)
{
    static unsigned n;
    int claimed = kernel_call_isr(NV2A_VECTOR);

    if (claimed >= 0)
        kernel_vblank_arm(1);
    else
        InterlockedExchange(&g_vbl_tick_busy, 0);
    if (claimed == 0)
        InterlockedIncrement(&g_vbl_declined);
    if (n++ < 3) {
        fprintf(stderr, "  [NV2A] vblank -> ISR %s\n",
                claimed < 0 ? "not callable" :
                claimed ? "claimed it" : "declined it");
        fflush(stderr);
    }
}

static void kernel_vblank_on_holder(void)
{
    kernel_vblank_deliver();
}

int xbox_VblankRaiseBits(volatile uint32_t *pcrtc_intr, volatile uint32_t *pmc_intr,
                         volatile uint32_t *pmc_intr_en, volatile LONG *tick_busy)
{

    if (!InterlockedCompareExchange((volatile LONG *)pmc_intr_en, 0, 0)) {
        InterlockedOr((volatile LONG *)pmc_intr, (LONG)NV2A_PMC_INTR_PCRTC);
        return 0;
    }
    InterlockedExchange(tick_busy, 1);
    InterlockedOr((volatile LONG *)pcrtc_intr, (LONG)NV2A_PCRTC_INTR_VBLANK);
    InterlockedOr((volatile LONG *)pmc_intr, (LONG)NV2A_PMC_INTR_PCRTC);
    return 1;
}

static void kernel_vblank_raise(void)
{
    if (!xbox_GetConnectedInterrupt(NV2A_VECTOR))
        return;
    xbox_IrqSetHandler(XBOX_IRQ_VBLANK, kernel_vblank_on_holder);

    if (!(BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_EN_0)
          & NV2A_PCRTC_INTR_VBLANK)) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [NV2A] vblank held: PCRTC_INTR_EN_0 off\n");
        return;
    }

    if (!xbox_VblankRaiseBits(
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0),
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_0),
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_EN_0),
            &g_vbl_tick_busy)) {

        kernel_vblank_arm(0);
        InterlockedIncrement(&g_vbl_declined);
        return;
    }

    switch (xbox_IrqSafePointsOn() ? xbox_IrqPost(XBOX_IRQ_VBLANK)
                                   : XBOX_IRQ_UNGATED) {
    case XBOX_IRQ_GATED:
        kernel_vblank_deliver();
        xbox_DispatchGateLeave();
        break;
    case XBOX_IRQ_UNGATED: {

        int gated = xbox_DispatchGateTryEnter();
        if (!gated)
            xbox_IrqNoteUngated(XBOX_IRQ_VBLANK);
        kernel_vblank_deliver();
        if (gated)
            xbox_DispatchGateLeave();
        break;
    }
    default:
        break;
    }
}

#define VBLANK_RESTART_NS 100000000ull

static uint64_t vblank_due(uint64_t t0_ns, uint64_t n)
{
    return t0_ns + n * 1000000000ull / 60u;
}

static int kernel_vblank_tick_ns(uint64_t now_ns, uint64_t *next_due_ns)
{
    static uint64_t t0_ns, count;
    static int started;
    uint64_t due;

    *next_due_ns = 0;
    if (kernel_vblank_mode() != XBOX_VBLANK_PACED)
        return 0;
    if (!started) {
        started = 1;
        t0_ns = now_ns;
        count = 0;
    }
    due = vblank_due(t0_ns, count);
    if (now_ns < due) {
        *next_due_ns = due;
        return 0;
    }
    if (now_ns - due > VBLANK_RESTART_NS) {
        t0_ns = now_ns;
        count = 0;
    }
    count++;
    *next_due_ns = vblank_due(t0_ns, count);
    kernel_vblank_count(kernel_clock_ms());
    kernel_vblank_raise();
    return 1;
}

static void kernel_vblank_tick(void)
{
    static long long next_ms, last_ms;
    long long now;

    if (kernel_vblank_mode() != XBOX_VBLANK_TICK)
        return;

    now = (long long)GetTickCount64();
    if (now < next_ms)
        return;
    next_ms = now + 16;

    if (!last_ms)
        last_ms = now;
    if (InterlockedIncrement(&g_vblank_total) % 600 == 0) {
        fprintf(stderr, "  [NV2A] vblank %ld (tick): last 600 in %lld ms\n",
                (long)g_vblank_total, now - last_ms);
        fflush(stderr);
        last_ms = now;
    }

    if (!xbox_GetConnectedInterrupt(NV2A_VECTOR))
        return;

    BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0) |= NV2A_PCRTC_INTR_VBLANK;
    BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_0)   |= NV2A_PMC_INTR_PCRTC;

    {
        static unsigned n;
        int claimed = kernel_call_isr(NV2A_VECTOR);
        if (n++ < 3)
            fprintf(stderr, "  [NV2A] vblank -> ISR %s\n",
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
        fflush(stderr);
    }
}

static int dpc_pop(PendingDpc *d)
{
    dpc_lock();
    if (g_dpc_head == g_dpc_tail) {
        LeaveCriticalSection(&g_dpc_lock);
        return 0;
    }
    *d = g_dpc_queue[g_dpc_head];
    g_dpc_head = (g_dpc_head + 1) % XBOX_MAX_PENDING_DPC;
    if (d->dpc)
        BRIDGE_MEM8(d->dpc + 2) = 0;
    LeaveCriticalSection(&g_dpc_lock);
    return 1;
}

static int kernel_drain_dpcs(int *ran)
{
    *ran = 0;
    for (;;) {
        PendingDpc d;

        if (!xbox_DispatchGateTryEnter()) {

            return g_dpc_head != g_dpc_tail;
        }
        if (!dpc_pop(&d)) {
            xbox_DispatchGateLeave();
            return 0;
        }
        if (d.dpc && kernel_run_dpc(d.dpc, d.arg1, d.arg2))
            (*ran)++;
        xbox_DispatchGateLeave();
    }
}

int xbox_DpcDrainTimer(int *ran)
{
    return kernel_drain_dpcs(ran);
}

int xbox_DpcDrainHere(void)
{
    int ran = 0;
    PendingDpc d;

    while (dpc_pop(&d))
        if (d.dpc && kernel_run_dpc(d.dpc, d.arg1, d.arg2))
            ran++;
    return ran;
}

static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    if (!bridge_buf_ok(dpc_va, 32, "KeInitializeDpc")) {
        g_eax = 0;
        return;
    }

    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    BRIDGE_MEM16(dpc_va + 0) = 0x13;
    BRIDGE_MEM32(dpc_va + 12) = routine;
    BRIDGE_MEM32(dpc_va + 16) = context;
    g_eax = 0;
}

static void bridge_HalGetInterruptVector(void)
{
    uint32_t level   = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    if (irql_va) {

        BRIDGE_MEM8(irql_va) = (uint8_t)level;
    }
    g_eax = level;
}

static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    uint32_t vector       = STACK_ARG(3);

    if (!bridge_buf_ok(interrupt_va, 44, "KeInitializeInterrupt")) {
        g_eax = 0;
        return;
    }

    memset(XBOX_TO_NATIVE(interrupt_va), 0, 44);
    BRIDGE_MEM32(interrupt_va + 0)  = routine;
    BRIDGE_MEM32(interrupt_va + 4)  = context;
    BRIDGE_MEM32(interrupt_va + 8)  = vector;
    g_eax = 0;
}

#define XBOX_MAX_VECTORS 32
static uint32_t g_connected_isr[XBOX_MAX_VECTORS];

static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);

    if (interrupt_va) {
        uint32_t vector = BRIDGE_MEM32(interrupt_va + 8);
        if (vector < XBOX_MAX_VECTORS) {
            g_connected_isr[vector] = interrupt_va;
            fprintf(stderr, "  [KERNEL] KeConnectInterrupt: vector %u -> "
                            "routine 0x%08X context 0x%08X\n",
                    vector, BRIDGE_MEM32(interrupt_va + 0),
                    BRIDGE_MEM32(interrupt_va + 4));
            fflush(stderr);
        }
    }
    g_eax = 1;
}

uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return (vector < XBOX_MAX_VECTORS) ? g_connected_isr[vector] : 0;
}

static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

static void bridge_HalRegisterShutdownNotification(void)
{
    g_eax = 0;
}

static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type     = STACK_ARG(1);
    HANDLE   ev;

    if (!timer_va) {
        g_eax = 0;
        return;
    }

    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    BRIDGE_MEM8(timer_va + 0)  = (uint8_t)(0x08 + (type & 1));
    BRIDGE_MEM8(timer_va + 2)  = 40;
    BRIDGE_MEM8(timer_va + 3)  = 0;
    BRIDGE_MEM32(timer_va + 4) = 0;
    BRIDGE_MEM32(timer_va + 8) = 0;
    BRIDGE_MEM32(timer_va + 12) = 0;

    ev = CreateEventW(NULL, (type == 0) ? TRUE : FALSE, FALSE, NULL);
    if (!ev) {
        fprintf(stderr, "  [KERNEL] KeInitializeTimerEx: CreateEventW failed "
                        "(error %u)\n", GetLastError());
        g_eax = 0;
        return;
    }
    ke_shadow_insert(timer_va, ev);
    g_eax = 0;
}

#define XBOX_MAX_TIMERS 32
typedef struct {
    uint32_t timer_va;
    uint32_t dpc_va;
    long long due_ms;
    long      period_ms;
} XboxTimer;
static XboxTimer g_timers[XBOX_MAX_TIMERS];
static CRITICAL_SECTION g_timer_lock;
static int g_timer_started;

static int kernel_timer_paced(void)
{
    static int paced = -1;

    if (paced < 0)
        paced = kernel_vblank_mode() == XBOX_VBLANK_PACED
                || xbox_DispatchGateOn();
    return paced;
}

static long long kernel_timer_clock_ms(void)
{
    return kernel_timer_paced() ? kernel_clock_ms()
                                : (long long)GetTickCount64();
}

static int kernel_fire_timers(long long now, long long *next_due_ms)
{
    int i, fired = 0;

    *next_due_ms = -1;
    for (i = 0; i < XBOX_MAX_TIMERS; i++) {
        uint32_t dpc, fired_va;

        EnterCriticalSection(&g_timer_lock);
        if (!g_timers[i].timer_va || now < g_timers[i].due_ms) {
            if (g_timers[i].timer_va
                && (*next_due_ms < 0 || g_timers[i].due_ms < *next_due_ms))
                *next_due_ms = g_timers[i].due_ms;
            LeaveCriticalSection(&g_timer_lock);
            continue;
        }
        fired_va = g_timers[i].timer_va;
        dpc = g_timers[i].dpc_va;
        if (g_timers[i].period_ms > 0) {
            g_timers[i].due_ms = now + g_timers[i].period_ms;
            if (*next_due_ms < 0 || g_timers[i].due_ms < *next_due_ms)
                *next_due_ms = g_timers[i].due_ms;
        } else {
            g_timers[i].timer_va = 0;
        }
        LeaveCriticalSection(&g_timer_lock);
        fired++;

        if (dpc) {
            if (xbox_DispatchGateTryEnter()) {
                kernel_run_dpc(dpc, 0, 0);
                xbox_DispatchGateLeave();
            } else {
                xbox_DpcQueue(dpc, 0, 0);
            }
        }

        {
            HANDLE ev = ke_shadow_lookup(fired_va);
            if (ev) {
                SetEvent(ev);
                BRIDGE_MEM32(fired_va + 4) = 1;
            }
        }
    }
    return fired;
}

static void kernel_timer_loop_ns(void)
{
    const uint64_t ms = 1000000u;
    uint64_t wait = 10 * ms;

    xbox_HostTimerThreadInit();
    for (;;) {
        uint64_t now, vbl_next, target;
        long long now_ms, timer_due;
        int ran, busy;

        xbox_HostTimerSleepNs(wait);
        kernel_vblank_tick_ns(xbox_HostNowNs(), &vbl_next);
        busy = kernel_drain_dpcs(&ran);
        now_ms = kernel_timer_clock_ms();
        kernel_fire_timers(now_ms, &timer_due);
        if (g_dpc_head != g_dpc_tail)
            busy = 1;

        now = xbox_HostNowNs();
        target = now + 10 * ms;
        if (vbl_next) {

            uint64_t v = vbl_next > now ? vbl_next : now + ms;
            if (v < target)
                target = v;
        }
        if (timer_due >= 0) {
            uint64_t t = now + (timer_due > now_ms
                                ? (uint64_t)(timer_due - now_ms) * ms : 0);
            if (t < target)
                target = t;
        }
        if (busy && now + ms < target)
            target = now + ms;
        wait = target > now ? target - now : 0;
    }
}

static DWORD WINAPI kernel_timer_thread(LPVOID unused)
{
    int slot = xbox_worker_stack_alloc();

    (void)unused;
    if (slot < 0) {
        fprintf(stderr, "  [KERNEL] timer thread has no worker stack; "
                        "timer DPCs will not run\n");
        fflush(stderr);
        return 0;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    {

        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "  [KERNEL] timer thread has no TIB; "
                            "timer DPCs will not run\n");
            fflush(stderr);
            return 0;
        }
        g_fs_base = tib;
    }

    if (!kernel_timer_paced()) {

        for (;;) {
            long long timer_due;
            int ran;

            Sleep(10);
            kernel_vblank_tick();
            kernel_drain_dpcs(&ran);
            kernel_fire_timers(kernel_timer_clock_ms(), &timer_due);
        }
    }

#ifdef _WIN32

    {
        typedef unsigned (WINAPI *tbp_fn)(unsigned);
        HMODULE winmm = LoadLibraryA("winmm.dll");
        tbp_fn tbp = winmm ? (tbp_fn)(void *)GetProcAddress(winmm,
                                                           "timeBeginPeriod")
                           : NULL;
        if (tbp)
            tbp(1);
    }
#endif

    kernel_timer_loop_ns();
    return 0;
}

static void kernel_set_timer(uint32_t timer_va, long long due_100ns,
                             long period_ms, uint32_t dpc_va)
{
    long long delay_ms = (due_100ns < 0) ? (-due_100ns) / 10000 : 0;
    int i, free_slot = -1;
    uint32_t was_set = 0;

    if (!g_timer_started) {
        InitializeCriticalSection(&g_timer_lock);
        g_timer_started = 1;
        CloseHandle(CreateThread(NULL, 0, kernel_timer_thread, NULL, 0, NULL));
    }

    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++) {
        if (g_timers[i].timer_va == timer_va) { free_slot = i; was_set = 1; break; }
        if (!g_timers[i].timer_va && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_timers[free_slot].timer_va  = timer_va;
        g_timers[free_slot].dpc_va    = dpc_va;
        g_timers[free_slot].due_ms    = kernel_timer_clock_ms() + delay_ms;
        g_timers[free_slot].period_ms = period_ms;
    }
    LeaveCriticalSection(&g_timer_lock);

    {
        HANDLE ev = ke_shadow_lookup(timer_va);
        if (ev)
            ResetEvent(ev);
    }
    g_eax = was_set;
}

static void bridge_KeSetTimer(void)
{

    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, 0, STACK_ARG(3));
}

static void bridge_KeSetTimerEx(void)
{
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, (long)STACK_ARG(3), STACK_ARG(4));
}

int xbox_kernel_cancel_timer(uint32_t timer_va)
{
    int i, was_set = 0;

    if (!g_timer_started)
        return 0;
    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++)
        if (g_timers[i].timer_va == timer_va) {
            g_timers[i].timer_va = 0;
            was_set = 1;
        }
    LeaveCriticalSection(&g_timer_lock);
    return was_set;
}

static void bridge_ExQueryPoolBlockSize(void)
{

    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    switch (status) {
    case 0x00000000: g_eax = 0; break;
    case 0xC0000034: g_eax = 2; break;
    case 0xC000003A: g_eax = 3; break;
    case 0xC0000022: g_eax = 5; break;
    case 0xC0000008: g_eax = 6; break;
    case 0xC0000017: g_eax = 8; break;
    case 0xC000000D: g_eax = 87; break;

    case 0x00000103: g_eax = 997; break;
    case 0x00000102: g_eax = 1460; break;
    case 0x00000104: g_eax = 0; break;
    case 0x80000005: g_eax = 234; break;
    case 0x80000006: g_eax = 18; break;
    case 0xC0000011: g_eax = 38; break;
    case 0xC0000023: g_eax = 122; break;
    case 0xC0000035: g_eax = 183; break;
    case 0xC00000BB: g_eax = 50; break;

    case 0xC0000018: g_eax = 487; break;

    default:         g_eax = 317; break;
    }
}

static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;

    static RECOMP_TLS char path[65536];
    uint16_t length=BRIDGE_MEM16(ansi_str_va);
    if(length>BRIDGE_MEM16(ansi_str_va+2)) return NULL;

    if (length && !bridge_va_mapped(buf_va, length)) return NULL;
    memcpy(path,XBOX_TO_NATIVE(buf_va),length);
    path[length]='\0';
    return path;
}

static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384

typedef char thobj_tag_matches[(BRIDGE_HANDLE_TAG == 0x48000000u &&
                                BRIDGE_HANDLE_MAX == 16384) ? 1 : -1];
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];

static volatile LONG s_handle_lock;

static void handle_lock(void)
{
    while (InterlockedCompareExchange(&s_handle_lock, 1, 0) != 0)
        SwitchToThread();
}

static void handle_unlock(void) { InterlockedExchange(&s_handle_lock, 0); }

static uint32_t bridge_handle_token_ex(HANDLE h, int fresh)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    handle_lock();
    for (i = 1; !fresh && i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) {
            handle_unlock();
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            handle_unlock();
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    handle_unlock();
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

static uint32_t bridge_handle_token(HANDLE h)
{
    return bridge_handle_token_ex(h, 0);
}

static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }

    return (HANDLE)(uintptr_t)token;
}

static HANDLE bridge_resolve_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }

    if (token >= 0xFFFFFFF0u)
        return (HANDLE)(intptr_t)(int32_t)token;

    return (HANDLE)(uintptr_t)token;
}

static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h;
            handle_lock();
            h = s_handle_table[i];
            s_handle_table[i] = NULL;
            handle_unlock();
            return h;
        }
    }
    return NULL;
}

static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    uint32_t root = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va) : 0;

    oa->RootDirectory = root && root != 0xFFFFFFFDu ? bridge_resolve_handle(root) : NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

#define BRIDGE_CDROM_HANDLE 0xDECD0001u

static int bridge_is_cdrom_device(const char *path)
{
    if (!path) return 0;
    if (_strnicmp(path, "\\??\\", 4) == 0) path += 4;
    return _stricmp(path, "\\Device\\CdRom0") == 0;
}

static NTSTATUS bridge_create_file_open(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options, XBOX_ANSI_STRING *name_out)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    *name_out = name;
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (bridge_is_cdrom_device(name.Buffer)) {
        fprintf(stderr, "  [FILE] %s -> synthetic DVD device handle\n",
                name.Buffer);
        if (handle_va)
            BRIDGE_MEM32(handle_va) = BRIDGE_CDROM_HANDLE;
        bridge_write_iostatus(iostatus_va, 0, 1  );
        return 0;
    }

    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    if (NT_SUCCESS(st)) {
        bridge_write_handle(handle_va, h);
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options, int ntcreate)
{
    XBOX_ANSI_STRING name;
    NTSTATUS st = bridge_create_file_open(handle_va, access, obj_attrs_va,
                                          iostatus_va, file_attrs, share,
                                          disposition, options, &name);
    uint32_t status = (uint32_t)st;

    {
        extern uint32_t xbox_LastFileError(void);
        uint32_t _e = status ? xbox_LastFileError() : 0u;

        char gpath[160] = "?";
        if (name.Buffer) {
            uint32_t len = name.Length, k;
            if (len >= sizeof gpath) len = sizeof gpath - 1;
            for (k = 0; k < len; k++)
                gpath[k] = name.Buffer[k];
            gpath[len] = 0;
        }
        if (status) {

            if (!xbox_missing_is_reported(gpath))
                fprintf(stderr, "  [FILE] %s (disp %u) -> 0x%08X FAILED (win32 err=%u%s)\n",
                        gpath, (unsigned)disposition, status, _e,
                        _e == 32u ? " ERROR_SHARING_VIOLATION"
                      : _e ==  2u ? " ERROR_FILE_NOT_FOUND"
                      : _e ==  3u ? " ERROR_PATH_NOT_FOUND" : "");
            if (name.Buffer)
                xbox_missing_note(gpath, status, (uint32_t)access,
                                  (uint32_t)disposition, (uint32_t)options, 0);
        } else if (ntcreate || xbox_missing_verbose())
            fprintf(stderr, "  [FILE] %s (disp %u opts 0x%X from 0x%08X) -> 0x%08X\n",
                    gpath, (unsigned)disposition, (unsigned)options,
                    g_xbox_kernel_caller, status);
    }
    fflush(stderr);
    return st;
}

static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);

    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options, 1);

    if (g_eax == 0 && handle_va && (options & 0x30u) == 0u)
        bridge_note_async_handle(BRIDGE_MEM32(handle_va));

    if (g_eax == 0 && getenv("RECOMP_FMV_HOST")) {

        extern int xbox_VideoPlayFile(const char *host_path);
        extern int xbox_VideoIsPlaying(void);

        char host[MAX_PATH * 2];
        size_t n = 0;

        const wchar_t *w = xbox_LastHostPath();

        while (n < sizeof(host) - 1 && w[n]) {
            host[n] = (char)w[n];
            n++;
        }
        host[n] = 0;
        if (n > 4 && _stricmp(host + n - 4, ".wmv") == 0
                && !xbox_VideoIsPlaying())
            xbox_VideoPlayFile(host);
    }
}

static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);
    uint32_t access    = STACK_ARG(1);
    uint32_t obj_attrs = STACK_ARG(2);
    uint32_t iostatus  = STACK_ARG(3);
    uint32_t share     = STACK_ARG(4);
    uint32_t options   = STACK_ARG(5);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1  , options, 0);
}

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

static void deliver_one_apc(uint32_t apc_routine, uint32_t apc_context,
                            uint32_t iostatus)
{

    recomp_func_t fn = recomp_lookup(apc_routine);
    if (!fn) fn = recomp_lookup_manual(apc_routine);
    if (!fn) fn = recomp_lookup_kernel(apc_routine);
    if (fn) {

        CallbackRegs saved;
        callback_regs_save(&saved);
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = apc_context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        {

            int took = xbox_GuestCpuEnterCallback();
            fn();
            xbox_GuestCpuLeaveCallback(took);
        }
        callback_regs_restore(&saved, 0);
    } else {
        uint32_t ord = 0;
        if (apc_routine >= KERNEL_VA_BASE && apc_routine < KERNEL_VA_END) {
            ord = g_slot_ordinals[(apc_routine - KERNEL_VA_BASE) / 4];
        }
        fprintf(stderr, "  [KERNEL] file I/O APC 0x%08X unresolved"
                " (kernel ordinal %u)\n", apc_routine, ord);
        fflush(stderr);
    }
}

static void bridge_complete_file_io(uint32_t event_token, uint32_t apc_routine,
                                    uint32_t apc_context, uint32_t iostatus)
{
    if (event_token) {
        HANDLE ev = bridge_resolve_handle(event_token);
        if (ev) SetEvent(ev);
    }
    if (apc_routine) {
        if (g_defer_io_apc && g_io_apc_count < IO_APC_QUEUE) {
            uint32_t *q = g_io_apc[g_io_apc_count++];
            q[0] = apc_routine; q[1] = apc_context; q[2] = iostatus;
        } else {
            deliver_one_apc(apc_routine, apc_context, iostatus);
        }
    }
}

static int bridge_run_io_apcs(uint32_t alertable, uint32_t wait_mode)
{
    uint32_t q[IO_APC_QUEUE][3];
    int n = g_io_apc_count, i;

    if (!alertable || wait_mode != 1   || n == 0)
        return 0;
    memcpy(q, g_io_apc, sizeof(q[0]) * n);
    g_io_apc_count = 0;
    for (i = 0; i < n; i++)
        deliver_one_apc(q[i][0], q[i][1], q[i][2]);
    g_eax = 0x000000C0u;
    return 1;
}

#define XBE_SECTION_REFCOUNT_OFFSET 0x18

static void bridge_XeSection(int load)
{
    uint32_t section = STACK_ARG(0);
    uint32_t count;

    if (!section) {
        g_eax = 0xC000000Du;
        return;
    }
    count = BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET);
    if (load)
        count++;
    else if (count)
        count--;
    BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET) = count;

    if (KERNEL_LOG_ON())
        fprintf(stderr, "  [XBE] Xe%sSection(0x%08X) refcount=%u\n",
                load ? "Load" : "Unload", section, count);
    g_eax = 0;
}

static void bridge_XeLoadSection(void)   { bridge_XeSection(1); }
static void bridge_XeUnloadSection(void) { bridge_XeSection(0); }

#define XBOX_SEH_END_OF_CHAIN 0xFFFFFFFFu
#define XBOX_EXCEPTION_UNWINDING  0x02u
#define XBOX_EXCEPTION_EXIT_UNWIND 0x04u
#define XBOX_SEH_MAX_FRAMES 64

static void bridge_RtlUnwind(void)
{
    uint32_t target_frame = STACK_ARG(0);
    uint32_t exc_record   = STACK_ARG(2);
    uint32_t reg          = BRIDGE_MEM32(XBOX_FS_BASE);
    uint32_t prev_reg     = 0;
    uint32_t scratch      = 0;
    int      guard;

    if (!exc_record) {
        g_esp -= 0x50;
        scratch = g_esp;
        memset((uint8_t *)XBOX_TO_NATIVE(scratch), 0, 0x50);
        BRIDGE_MEM32(scratch) = 0xC0000027u;
        exc_record = scratch;
    }
    BRIDGE_MEM32(exc_record + 4) |= XBOX_EXCEPTION_UNWINDING
        | (target_frame ? 0u : XBOX_EXCEPTION_EXIT_UNWIND);

    for (guard = 0; guard < XBOX_SEH_MAX_FRAMES; guard++) {
        uint32_t next, handler;

        if (reg == XBOX_SEH_END_OF_CHAIN || reg == 0 || reg == target_frame)
            break;
        if ((reg & 3u) || reg < XBOX_STACK_BASE || reg >= XBOX_STACK_TOP)
            break;
        if (prev_reg && reg <= prev_reg)
            break;

        next    = BRIDGE_MEM32(reg);
        handler = BRIDGE_MEM32(reg + 4);

        BRIDGE_MEM32(XBOX_FS_BASE) = next;

        if (handler) {
            recomp_func_t fn = recomp_lookup(handler);
            if (!fn) fn = recomp_lookup_manual(handler);
            if (!fn) fn = recomp_lookup_kernel(handler);
            if (fn) {

                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = reg;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = exc_record;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                {

                    int took = xbox_GuestCpuEnterCallback();
                    fn();
                    xbox_GuestCpuLeaveCallback(took);
                }

                g_esp += 16;
            }
        }

        prev_reg = reg;
        reg      = next;
    }

    if (target_frame && target_frame != XBOX_SEH_END_OF_CHAIN)
        BRIDGE_MEM32(XBOX_FS_BASE) = target_frame;

    if (scratch)
        g_esp += 0x50;
}

#define BRIDGE_ASYNC_MAX 64
static uint32_t g_async_handles[BRIDGE_ASYNC_MAX];

static void bridge_note_async_handle(uint32_t token)
{
    int i;
    if (!token)
        return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (!g_async_handles[i]) { g_async_handles[i] = token; return; }
}

static void bridge_forget_async_handle(uint32_t token)
{
    int i;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token) { g_async_handles[i] = 0; return; }
}

static int bridge_handle_is_async(uint32_t token)
{
    int i;
    if (!token)
        return 0;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return 1;
    return 0;
}

static int bridge_async_io_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_ASYNC_IO") ? 1 : 0;
    return on;
}

static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtReadFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);

    {
        const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(buffer_va);
        uint32_t got = (uint32_t)ios.Information;

        if (poff)
            fprintf(stderr, "  [READ] from=0x%08X ev=%08X apc=%08X @%lld want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    g_xbox_kernel_caller, STACK_ARG(1), STACK_ARG(2),
                    (long long)off.QuadPart, length, got,
                    (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        else
            fprintf(stderr, "  [READ] from=0x%08X @seq want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    g_xbox_kernel_caller,
                    length, got, (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        fflush(stderr);
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);

    if (bridge_async_io_enabled() && bridge_handle_is_async(STACK_ARG(0)))
        g_eax = STATUS_PENDING;
}

static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtWriteFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtSetInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;
        return;
    }

    if (infoclass == 10u) {
        XBOX_OBJECT_ATTRIBUTES oa;
        XBOX_ANSI_STRING name;
        static RECOMP_TLS char path[1024];
        uint32_t root, buf;
        uint16_t len;

        if (length < 16u) {
            g_eax = 0xC0000004u;
            bridge_write_iostatus(ios_va, (NTSTATUS)g_eax, 0);
            return;
        }
        root = BRIDGE_MEM32(info_va + 4);
        len  = BRIDGE_MEM16(info_va + 8);
        buf  = BRIDGE_MEM32(info_va + 12);
        if (!len || len >= sizeof path || !bridge_va_mapped(buf, len)) {
            g_eax = (uint32_t)STATUS_OBJECT_NAME_INVALID;
            bridge_write_iostatus(ios_va, (NTSTATUS)g_eax, 0);
            return;
        }
        memcpy(path, XBOX_TO_NATIVE(buf), len);
        path[len] = '\0';
        name.Buffer = path;
        name.Length = len;
        name.MaximumLength = (USHORT)(len + 1);

        oa.RootDirectory = root && root != 0xFFFFFFFDu
                         ? bridge_resolve_handle(root) : NULL;
        oa.ObjectName = &name;
        oa.Attributes = 0;
        g_eax = (uint32_t)xbox_RenameFileByHandle(handle, &oa,
                    (BOOLEAN)(BRIDGE_MEM8(info_va) != 0));
        if (g_eax)
            fprintf(stderr, "  [FILE] rename to %s -> 0x%08X FAILED\n",
                    path, g_eax);
        bridge_write_iostatus(ios_va, (NTSTATUS)g_eax, 0);
        return;
    }

    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryVolumeInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));

    if (g_eax)
        xbox_missing_note(name.Buffer, g_eax, 0, XBOX_FILE_OPEN, 0, 1);
}

static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t info_class  = STACK_ARG(7);
    uint32_t filename_va = STACK_ARG(8);
    uint32_t restart     = STACK_ARG(9);
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {

        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, (XBOX_FILE_INFORMATION_CLASS)info_class,
                pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);

    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

static void bridge_NtQuerySymbolicLinkObject(void)
{

    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;

    if (!target_va) {
        g_eax = 0xC0000023u;
        return;
    }
    {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);

        if (!buf_va || len >= max_len) {
            static unsigned warned;
            if (warned++ < 4) {
                fprintf(stderr,
                        "  [KERNEL] NtQuerySymbolicLinkObject: buffer 0x%08X "
                        "max=%u cannot hold %u bytes; returning "
                        "STATUS_BUFFER_TOO_SMALL\n",
                        buf_va, (unsigned)max_len, (unsigned)len + 1);
                fflush(stderr);
            }
            g_eax = 0xC0000023u;
            return;
        }
        memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
        BRIDGE_MEM16(target_va) = len;
    }
    g_eax = STATUS_SUCCESS;
}

static void bridge_IoCreateFile(void)
{

    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options, 0);
}

#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u

#define XBOX_DISK_BYTES_PER_SECTOR   512u
#define XBOX_DISK_SECTORS_PER_TRACK  63u
#define XBOX_DISK_TRACKS_PER_CYL     255u
#define XBOX_DISK_CYLINDERS          1216u

static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t handle  = STACK_ARG(0);
    uint32_t ios_va  = STACK_ARG(4);
    uint32_t ioctl   = STACK_ARG(5);
    uint32_t out_va  = STACK_ARG(8);
    uint32_t out_len = STACK_ARG(9);

    if (handle == BRIDGE_CDROM_HANDLE) {
        uint32_t in_va  = STACK_ARG(6);
        uint32_t in_len = STACK_ARG(7);

        if (in_va && in_len >= 44 && BRIDGE_MEM8(in_va + 28) == 0x5A) {
            uint32_t data_va  = BRIDGE_MEM32(in_va + 20);
            uint32_t data_len = BRIDGE_MEM32(in_va + 12);
            uint32_t page     = BRIDGE_MEM8(in_va + 30) & 0x3F;

            fprintf(stderr, "  [FILE] DVD MODE SENSE(10) page 0x%02X, "
                            "%u bytes -> authentication page\n", page, data_len);

            if (data_va && data_len) {
                uint32_t i;
                for (i = 0; i < data_len; i++)
                    BRIDGE_MEM8(data_va + i) = 0;

                if (data_len >= 2) {
                    BRIDGE_MEM8(data_va + 0) = 0;
                    BRIDGE_MEM8(data_va + 1) = 26;
                }
                if (data_len >= 10) {
                    BRIDGE_MEM8(data_va + 8) = 0x3E;
                    BRIDGE_MEM8(data_va + 9) = 18;
                }
                if (data_len >= 13) {
                    BRIDGE_MEM8(data_va + 10) = 1;
                    BRIDGE_MEM8(data_va + 11) = 1;
                    BRIDGE_MEM8(data_va + 12) = 1;
                }
            }

            BRIDGE_MEM8(in_va + 2) = 0;
            bridge_write_iostatus(ios_va, 0, in_len);
            g_eax = 0;
            return;
        }

        if (ioctl == 0x4D014u && in_va && in_len >= 44) {
            uint32_t data_va  = BRIDGE_MEM32(in_va + 20);
            uint32_t data_len = BRIDGE_MEM32(in_va + 12);
            uint32_t i;

            fprintf(stderr, "  [FILE] DVD SCSI command 0x%02X, %u bytes "
                            "-> zeroed\n", BRIDGE_MEM8(in_va + 28), data_len);
            if (data_va)
                for (i = 0; i < data_len; i++)
                    BRIDGE_MEM8(data_va + i) = 0;
            BRIDGE_MEM8(in_va + 2) = 0;
            bridge_write_iostatus(ios_va, 0, in_len);
            g_eax = 0;
            return;
        }

        fprintf(stderr, "  [FILE] DVD device IOCTL 0x%X (in=%u out=%u) "
                        "-> STATUS_SUCCESS\n", ioctl, in_len, out_len);
        if (out_va && out_len) {
            uint32_t i;
            for (i = 0; i < out_len; i++)
                BRIDGE_MEM8(out_va + i) = 0;
        }
        bridge_write_iostatus(ios_va, 0, out_len);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_DRIVE_GEOMETRY) {

        if (!out_va || out_len < 24) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0);
            g_eax = 0xC0000023u;
            return;
        }
        BRIDGE_MEM32(out_va +  0) = XBOX_DISK_CYLINDERS;
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = 0x0B;
        BRIDGE_MEM32(out_va + 12) = XBOX_DISK_TRACKS_PER_CYL;
        BRIDGE_MEM32(out_va + 16) = XBOX_DISK_SECTORS_PER_TRACK;
        BRIDGE_MEM32(out_va + 20) = XBOX_DISK_BYTES_PER_SECTOR;
        bridge_write_iostatus(ios_va, 0, 24);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_PARTITION_INFO) {

        HANDLE h = bridge_resolve_handle(STACK_ARG(0));
        LARGE_INTEGER size;

        if (!out_va || out_len < 32) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0);
            g_eax = 0xC0000023u;
            return;
        }
        size.QuadPart = 0;
        if (h && h != INVALID_HANDLE_VALUE)
            GetFileSizeEx(h, &size);
        BRIDGE_MEM32(out_va +  0) = 0;
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = (uint32_t)size.LowPart;
        BRIDGE_MEM32(out_va + 12) = (uint32_t)size.HighPart;
        BRIDGE_MEM32(out_va + 16) = 0;
        BRIDGE_MEM32(out_va + 20) = 1;
        BRIDGE_MEM32(out_va + 24) = 0x00010106u;
        BRIDGE_MEM32(out_va + 28) = 0;
        bridge_write_iostatus(ios_va, 0, 32);
        g_eax = 0;
        return;
    }

    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - unhandled\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

static void bridge_NtCreateDirectoryObject(void)
{

    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;
}

static void bridge_IoCreateSymbolicLink(void)
{
    uint32_t link_va   = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    XBOX_ANSI_STRING link, target;

    if (!link_va) {
        g_eax = 0xC000000Du;
        return;
    }
    link.Length        = BRIDGE_MEM16(link_va + 0);
    link.MaximumLength = BRIDGE_MEM16(link_va + 2);
    link.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(link_va + 4));

    if (target_va) {
        target.Length        = BRIDGE_MEM16(target_va + 0);
        target.MaximumLength = BRIDGE_MEM16(target_va + 2);
        target.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(target_va + 4));
    } else {
        target.Length = target.MaximumLength = 0;
        target.Buffer = NULL;
    }

    g_eax = (uint32_t)xbox_IoCreateSymbolicLink(&link,
                                                target_va ? &target : NULL);
}

static void bridge_ObReferenceObjectByHandle(void)
{

    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);

    uint32_t thread = handle == 0xFFFFFFFEu ? thobj_current()
                                            : thobj_from_token(handle);

    (void)obj_type;
    if (thread) {
        thobj_ref(thread);
        if (object_ptr) BRIDGE_MEM32(object_ptr) = thread;
        g_eax = 0;
        return;
    }

    if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
    g_eax = 0;
}

static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

static void bridge_MmPersistContiguousMemory(void)
{

    g_eax = 0;
}

static void bridge_generic_stub(void)
{

    g_eax = 0;
}

static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

static void bridge_AvSendTVEncoderOption(void)
{
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

static void bridge_ExFreePool(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

#define XBOX_DEVICE_OBJECT_SIZE 0x38u

static void bridge_IoCreateDevice(void)
{
    uint32_t extension_size = STACK_ARG(1);
    uint32_t device_type    = STACK_ARG(3);
    uint32_t out_va         = STACK_ARG(5);
    uint32_t object_va;

    if (!out_va) {
        g_eax = 0xC000000Du;
        return;
    }

    object_va = xbox_HeapAlloc(XBOX_DEVICE_OBJECT_SIZE + extension_size, 16);
    if (!object_va) {
        g_eax = 0xC000009Au;
        return;
    }

    memset(XBOX_TO_NATIVE(object_va), 0,
           XBOX_DEVICE_OBJECT_SIZE + extension_size);
    BRIDGE_MEM16(object_va + 0x02) = (uint16_t)XBOX_DEVICE_OBJECT_SIZE;
    BRIDGE_MEM32(object_va + 0x04) = 1;
    BRIDGE_MEM32(object_va + 0x08) = STACK_ARG(0);
    BRIDGE_MEM32(object_va + 0x18) =
        extension_size ? object_va + XBOX_DEVICE_OBJECT_SIZE : 0;
    BRIDGE_MEM8(object_va + 0x1C)  = (uint8_t)device_type;
    BRIDGE_MEM8(object_va + 0x1D)  = 1;

    BRIDGE_MEM32(out_va) = object_va;
    g_eax = 0;
}

static void bridge_KeCancelTimer(void)
{

    uint32_t timer_va = STACK_ARG(0);
    int armed = xbox_kernel_cancel_timer(timer_va);
    g_eax = (uint32_t)xbox_KeCancelTimer(XBOX_TO_NATIVE(timer_va)) || armed;
}

static void bridge_KeDisconnectInterrupt(void)
{
    g_eax = (uint32_t)xbox_KeDisconnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(STACK_ARG(0)));
}

static HANDLE bridge_thread_host(uint32_t obj)
{
    HANDLE h = ke_shadow_lookup(obj);
    if (!h && obj && obj == g_thread_obj_va)
        h = GetCurrentThread();
    return h;
}

static void bridge_KeQueryBasePriorityThread(void)
{
    HANDLE h = bridge_thread_host(STACK_ARG(0));
    g_eax = h ? (uint32_t)xbox_KeQueryBasePriorityThread(h) : 0;
}

static void bridge_KeSetBasePriorityThread(void)
{
    HANDLE h = bridge_thread_host(STACK_ARG(0));
    g_eax = h ? (uint32_t)xbox_KeSetBasePriorityThread(h, (LONG)STACK_ARG(1)) : 0;
}

static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

static void bridge_MmQueryAllocationSize(void)
{
    uint32_t va = STACK_ARG(0);
    uint32_t n = xbox_ContiguousBlockSize(va);
    g_eax = n ? n : xbox_HeapBlockSize(va);
}

static void bridge_NtCreateMutant(void)
{
    uint32_t handle_va = STACK_ARG(0);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING name;
    HANDLE h = NULL;
    NTSTATUS st;

    bridge_build_oa(STACK_ARG(1), &oa, &name);
    st = xbox_NtCreateMutant(&h, STACK_ARG(1) ? &oa : NULL,
                             (BOOLEAN)STACK_ARG(2));
    if (st >= 0 && handle_va) bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

static void bridge_NtReleaseMutant(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtReleaseMutant(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

static void bridge_NtSuspendThread(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
}

static void bridge_NtResumeThread(void)
{
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_ObfDereferenceObject(void)
{
    thobj_unref(g_ecx);
    xbox_ObfDereferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

static void bridge_ObfReferenceObject(void)
{
    thobj_ref(g_ecx);
    xbox_ObfReferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

static void bridge_HalReadSMBusValue(void)
{
    uint32_t data_va = STACK_ARG(3);

    g_eax = (uint32_t)xbox_HalReadSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), (PULONG)XBOX_TO_NATIVE(data_va));
}

static void bridge_HalWriteSMBusValue(void)
{
    g_eax = (uint32_t)xbox_HalWriteSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), STACK_ARG(3));
}

static void bridge_HalReadWritePCISpace(void)
{
    xbox_HalReadWritePCISpace(
        STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
        XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4), (BOOLEAN)STACK_ARG(5));
    g_eax = 0;
}

static void bridge_HalRequestSoftwareInterrupt(void)
{
    xbox_HalRequestSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

static void bridge_HalClearSoftwareInterrupt(void)
{
    xbox_HalClearSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

static void bridge_ExSaveNonVolatileSetting(void)
{
    g_eax = (uint32_t)xbox_ExSaveNonVolatileSetting(
        STACK_ARG(0), STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        STACK_ARG(3));
}

static void bridge_MmUnmapIoSpace(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_IoDeleteDevice(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_KeQueryInterruptTime(void)
{
    uint64_t t = xbox_KeQueryInterruptTime();

    g_eax = (uint32_t)t;
    g_edx = (uint32_t)(t >> 32);
}

static void bridge_KeSaveFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeSaveFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeRestoreFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeRestoreFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_NtCreateSemaphore(void)
{
    uint32_t handle_va = STACK_ARG(0);
    HANDLE h = NULL;
    NTSTATUS st;

    st = xbox_NtCreateSemaphore(
        &h, XBOX_TO_NATIVE(STACK_ARG(1)),
        (LONG)STACK_ARG(2), (LONG)STACK_ARG(3));
    if (st >= 0 && handle_va)
        bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

static void bridge_NtReleaseSemaphore(void)
{
    uint32_t count_va = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtReleaseSemaphore(
        bridge_resolve_handle(STACK_ARG(0)), (LONG)STACK_ARG(1),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

static void bridge_KeAlertThread(void)
{
    g_eax = (uint32_t)xbox_KeAlertThread(
        bridge_resolve_handle(STACK_ARG(0)), (KPROCESSOR_MODE)STACK_ARG(1));
}

#define BRIDGE_MAXIMUM_WAIT_OBJECTS 64

static void bridge_KeWaitForMultipleObjects(void)
{
    uint32_t count      = STACK_ARG(0);
    uint32_t objects_va = STACK_ARG(1);
    uint32_t wait_type  = STACK_ARG(2);
    uint32_t alertable  = STACK_ARG(5);
    uint32_t timeout_va = STACK_ARG(6);
    HANDLE handles[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t guest_events[BRIDGE_MAXIMUM_WAIT_OBJECTS] = {0};
    int event_types[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    if (count > BRIDGE_MAXIMUM_WAIT_OBJECTS)
        count = BRIDGE_MAXIMUM_WAIT_OBJECTS;
    for (i = 0; i < count; i++) {
        uint32_t object = objects_va ? BRIDGE_MEM32(objects_va + i * 4) : 0;
        handles[i] = ke_guest_event_prepare(object, &event_types[i]);
        if (handles[i]) {
            guest_events[i] = object;
        } else {
            handles[i] = ke_shadow_lookup(object);
            if (!handles[i])
                handles[i] = bridge_resolve_handle(object);
        }
    }

    g_eax = (uint32_t)xbox_KeWaitForMultipleObjects(
        count, (PVOID *)handles, wait_type,
        STACK_ARG(3), (KPROCESSOR_MODE)STACK_ARG(4),
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_va),
        XBOX_TO_NATIVE(STACK_ARG(7)));

    for (i = 0; i < count; i++) {
        if (guest_events[i] && event_types[i] == 1 &&
            ((wait_type == 0 && g_eax == 0) ||
             (wait_type != 0 && g_eax < count && g_eax == i)))
            BRIDGE_MEM32(guest_events[i] + 4) = 0;
    }
}

#undef BRIDGE_MAXIMUM_WAIT_OBJECTS

static void bridge_NtSetSystemTime(void)
{
    uint32_t prev_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSetSystemTime(
        XBOX_TO_NATIVE(STACK_ARG(0)),
        prev_va ? (PLARGE_INTEGER)XBOX_TO_NATIVE(prev_va) : NULL);
}

static void bridge_ObReferenceObjectByName(void)
{
    uint32_t name_va   = STACK_ARG(0);
    uint32_t object_va = STACK_ARG(4);
    XBOX_ANSI_STRING name;
    PVOID object_local = NULL;

    if (!name_va) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_ObReferenceObjectByName(
        &name, STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)), &object_local);
    if (object_va)
        BRIDGE_MEM32(object_va) = (uint32_t)(uintptr_t)object_local;
}

static void bridge_RtlInitUnicodeString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
        size_t bytes = 0;

        while (wp[bytes / 2])
            bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            bytes = 0xFFFE;
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)bytes;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

static void bridge_RtlTimeFieldsToTime(void)
{
    g_eax = (uint32_t)xbox_RtlTimeFieldsToTime(
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

static void bridge_DbgBreakPoint(void)
{
    xbox_DbgBreakPoint();
    g_eax = 0;
}

static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_HalDisableSystemInterrupt(void)
{
    xbox_HalDisableSystemInterrupt(STACK_ARG(0), (KIRQL)STACK_ARG(1));
    g_eax = 0;
}

static void bridge_HalInitiateShutdown(void)
{
    xbox_HalInitiateShutdown();
    g_eax = 0;
}

static void bridge_HalIsResetOrShutdownPending(void)
{
    g_eax = (uint32_t)xbox_HalIsResetOrShutdownPending();
}

static void bridge_WRITE_PORT_BUFFER_ULONG(void)
{
    xbox_WRITE_PORT_BUFFER_ULONG((PULONG)(uintptr_t)STACK_ARG(0),
        (PULONG)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_WRITE_PORT_BUFFER_USHORT(void)
{
    xbox_WRITE_PORT_BUFFER_USHORT((PUSHORT)(uintptr_t)STACK_ARG(0),
        (PUSHORT)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_IoBuildDeviceIoControlRequest(void)
{
    g_eax = (uint32_t)xbox_IoBuildDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (BOOLEAN)STACK_ARG(6), bridge_resolve_handle(STACK_ARG(7)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(8)));
}

static void bridge_IoBuildSynchronousFsdRequest(void)
{
    g_eax = (uint32_t)(uintptr_t)xbox_IoBuildSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), bridge_resolve_handle(STACK_ARG(5)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(6)));
}

static void bridge_IoDeleteSymbolicLink(void)
{
    uint32_t name_va = STACK_ARG(0);
    XBOX_ANSI_STRING name;

    if (!name_va) {
        g_eax = 0xC000000Du;
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va + 0);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_IoDeleteSymbolicLink(&name);
}

static void bridge_IoInitializeIrp(void)
{
    xbox_IoInitializeIrp(XBOX_TO_NATIVE(STACK_ARG(0)),
        (USHORT)STACK_ARG(1), (CCHAR)STACK_ARG(2));
    g_eax = 0;
}

static void bridge_IoInvalidDeviceRequest(void)
{
    g_eax = (uint32_t)xbox_IoInvalidDeviceRequest(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_IoMarkIrpMustComplete(void)
{
    xbox_IoMarkIrpMustComplete(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_IoSetIoCompletion(void)
{
    g_eax = (uint32_t)xbox_IoSetIoCompletion(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), (NTSTATUS)STACK_ARG(3),
        (ULONG_PTR)STACK_ARG(4));
}

static void bridge_IoStartNextPacket(void)
{
    xbox_IoStartNextPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
        (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

static void bridge_IoStartNextPacketByKey(void)
{
    xbox_IoStartNextPacketByKey(XBOX_TO_NATIVE(STACK_ARG(0)),
        (BOOLEAN)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_IoStartPacket(void)
{
    xbox_IoStartPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

static void bridge_IoSynchronousDeviceIoControlRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(6)), (BOOLEAN)STACK_ARG(7));
}

static void bridge_IoSynchronousFsdRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)));
}

static void bridge_IofCallDriver(void)
{
    g_eax = (uint32_t)xbox_IofCallDriver(XBOX_TO_NATIVE(g_ecx),
                                        XBOX_TO_NATIVE(g_edx));
}

static void bridge_IofCompleteRequest(void)
{
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(g_ecx), (CCHAR)g_edx);
    g_eax = 0;
}

static void bridge_MmLockUnlockPhysicalPage(void)
{
    xbox_MmLockUnlockPhysicalPage(STACK_ARG(0), (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

static void bridge_MmFreeSystemMemory(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_MmCreateKernelStack(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t base_va = xbox_HeapAlloc(size, 16);

    g_eax = base_va ? base_va + size : 0;
}

static void bridge_MmDeleteKernelStack(void)
{
    xbox_HeapFree(STACK_ARG(1));
    g_eax = 0;
}

static void bridge_XcPKGetKeyLen(void)
{
    g_eax = (uint32_t)xbox_XcPKGetKeyLen(XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_XcPKDecPrivate(void)
{
    g_eax = (uint32_t)xbox_XcPKDecPrivate(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcPKEncPublic(void)
{
    g_eax = (uint32_t)xbox_XcPKEncPublic(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcVerifyPKCS1Signature(void)
{
    g_eax = (uint32_t)xbox_XcVerifyPKCS1Signature(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcModExp(void)
{
    g_eax = (uint32_t)xbox_XcModExp((PULONG)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4));
}

static void bridge_XcKeyTable(void)
{
    xbox_XcKeyTable(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcBlockCrypt(void)
{
    xbox_XcBlockCrypt(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        STACK_ARG(4));
    g_eax = 0;
}

static void bridge_XcBlockCryptCBC(void)
{
    xbox_XcBlockCryptCBC(STACK_ARG(0), STACK_ARG(1),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

static void bridge_XcCryptService(void)
{
    xbox_XcCryptService(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

static void bridge_XcUpdateCrypto(void)
{
    xbox_XcUpdateCrypto(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

static void bridge_RtlRip(void)
{
    xbox_RtlRip((PCHAR)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_RtlAnsiStringToUnicodeString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t unicode_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    unicode_bytes = (uint32_t)(s_len + 1) * sizeof(WCHAR);

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(unicode_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)unicode_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < unicode_bytes) {
            g_eax = 0xC0000205u;
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = MultiByteToWideChar(CP_ACP, 0,
        (const char*)XBOX_TO_NATIVE(s_buf_va), (int)s_len,
        (WCHAR*)XBOX_TO_NATIVE(d_buf_va), (int)(unicode_bytes / sizeof(WCHAR)));
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)(result * sizeof(WCHAR));
        ((WCHAR*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;
    }
}

static void bridge_RtlUnicodeStringToAnsiString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t ansi_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    ansi_bytes = s_len / sizeof(WCHAR) + 1;

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(ansi_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)ansi_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < ansi_bytes) {
            g_eax = 0xC0000205u;
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR*)XBOX_TO_NATIVE(s_buf_va), (int)(s_len / sizeof(WCHAR)),
        (char*)XBOX_TO_NATIVE(d_buf_va), (int)ansi_bytes, NULL, NULL);
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)result;
        if ((uint16_t)result < d_max)
            ((char*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;
    }
}

static void bridge_NtDuplicateObject(void)
{
    uint32_t target_va = STACK_ARG(1);
    HANDLE src = bridge_resolve_handle(STACK_ARG(0));
    HANDLE dup = NULL;
    DWORD opts = 0;

    if (!target_va) {
        g_eax = 0xC000000Du;
        return;
    }
    if (STACK_ARG(2) & 0x1) opts |= DUPLICATE_CLOSE_SOURCE;
    if (STACK_ARG(2) & 0x2) opts |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(),
                         &dup, 0, FALSE, opts)) {
        static int logged = 0;
        if (logged++ < 8) {
            fprintf(stderr, "  [KERNEL] NtDuplicateObject: token=0x%08X "
                    "handle=%p failed (error %lu)\n",
                    STACK_ARG(0), src, (unsigned long)GetLastError());
            fflush(stderr);
        }
        g_eax = 0xC0000001u;
        return;
    }

    if (dup == (HANDLE)(intptr_t)-1) {
        BRIDGE_MEM32(target_va) = 0xFFFFFFFFu;
        g_eax = 0;
        return;
    }
    BRIDGE_MEM32(target_va) = bridge_handle_token_ex(dup, 1);
    thobj_attach_token(BRIDGE_MEM32(target_va), thobj_from_token(STACK_ARG(0)));
    if (opts & DUPLICATE_CLOSE_SOURCE)
        thobj_detach_token(STACK_ARG(0));
    g_eax = 0;
}

typedef void (*bridge_func_t)(void);

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL    ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INTEGER_OVERFLOW
#define STATUS_INTEGER_OVERFLOW    ((NTSTATUS)0xC0000095L)
#endif

static uint32_t bridge_guest_wcslen_bytes(uint32_t src_va)
{
    const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
    uint32_t bytes = 0;

    while (wp[bytes / 2]) {
        bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            return 0xFFFE;
    }
    return bytes;
}

static void bridge_RtlAppendStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;
}

static void bridge_RtlAppendUnicodeStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;
}

static void bridge_RtlAppendUnicodeToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0;
    uint32_t d_buf = 0, add_bytes;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    add_bytes = bridge_guest_wcslen_bytes(src_va);
    if ((uint32_t)d_len + add_bytes > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(src_va), add_bytes);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + add_bytes);
    g_eax = 0;
}

static void bridge_RtlCompareString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t l1, l2, common, i;
    const uint8_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    l1 = BRIDGE_MEM16(s1_va + 0);
    l2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = l1 < l2 ? l1 : l2;
    diff = 0;
    for (i = 0; i < common; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = tolower(a); b = tolower(b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)l1 - (int)l2;
    g_eax = (uint32_t)(int32_t)diff;
}

static void bridge_RtlCompareUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, common, i;
    const uint16_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = b1 < b2 ? b1 : b2;
    diff = 0;
    for (i = 0; i < common / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = towlower((wint_t)a); b = towlower((wint_t)b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)b1 - (int)b2;
    g_eax = (uint32_t)(int32_t)diff;
}

static void bridge_RtlEqualUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, i;
    const uint16_t *p1, *p2;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    if (b1 != b2) { g_eax = 0; return; }
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if ((!p1 || !p2) && b1) { g_eax = 0; return; }

    for (i = 0; i < b1 / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) {
            a = towlower((wint_t)a);
            b = towlower((wint_t)b);
        }
        if (a != b) { g_eax = 0; return; }
    }
    g_eax = 1;
}

static void bridge_RtlCopyString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

static void bridge_RtlCopyUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

static void bridge_RtlUpperString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy, i;
    const uint8_t *sp;
    uint8_t *dp;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf) {
        sp = (const uint8_t *)XBOX_TO_NATIVE(s_buf);
        dp = (uint8_t *)XBOX_TO_NATIVE(d_buf);
        for (i = 0; i < copy; i++)
            dp[i] = (uint8_t)toupper(sp[i]);
    }
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

static void bridge_RtlCreateUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t bytes, buf_va;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    bytes = bridge_guest_wcslen_bytes(src_va);
    buf_va = xbox_HeapAlloc(bytes + sizeof(uint16_t), 16);
    if (!buf_va) { g_eax = 0; return; }
    memcpy(XBOX_TO_NATIVE(buf_va), XBOX_TO_NATIVE(src_va), bytes);
    ((uint16_t *)XBOX_TO_NATIVE(buf_va))[bytes / 2] = 0;
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)bytes;
    BRIDGE_MEM16(dst_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
    BRIDGE_MEM32(dst_va + 4) = buf_va;
    g_eax = 1;
}

static void bridge_RtlFreeUnicodeString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf_va;

    if (!str_va) { g_eax = 0; return; }
    buf_va = BRIDGE_MEM32(str_va + 4);
    if (buf_va)
        xbox_HeapFree(buf_va);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

static void bridge_RtlDowncaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towlower((wint_t)(uint16_t)ch);
}

static void bridge_RtlLowerChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)tolower((int)(uint8_t)ch);
}

static void bridge_RtlUpcaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towupper((wint_t)(uint16_t)ch);
}

static void bridge_RtlUpperChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)toupper((int)(uint8_t)ch);
}

static void bridge_RtlDowncaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towlower((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;
}

static void bridge_RtlUpcaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towupper((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;
}

static void bridge_RtlIntegerToChar(void)
{
    static const char digs[] = "0123456789ABCDEF";
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t len   = STACK_ARG(2);
    uint32_t str_va = STACK_ARG(3);
    char tmp[40];
    uint32_t i, n;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    n = 0;
    {
        uint32_t v = value;
        if (v == 0)
            tmp[n++] = '0';
        while (v) {
            tmp[n++] = digs[v % base];
            v /= base;
        }
        for (i = 0; i < n / 2; i++) {
            char t = tmp[i];
            tmp[i] = tmp[n - 1 - i];
            tmp[n - 1 - i] = t;
        }
    }
    if (n > len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    for (i = 0; i < len - n; i++)
        BRIDGE_MEM8(str_va + i) = (uint8_t)'0';
    for (i = 0; i < n; i++)
        BRIDGE_MEM8(str_va + (len - n + i)) = (uint8_t)tmp[i];
    BRIDGE_MEM8(str_va + len) = 0;
    g_eax = 0;
}

static void bridge_RtlIntegerToUnicodeString(void)
{
    static const uint16_t digs[] = { '0','1','2','3','4','5','6','7','8','9',
                                     'A','B','C','D','E','F' };
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t str_va = STACK_ARG(2);
    uint16_t body[32], d_max;
    uint32_t d_buf, i, n_body = 0, total;
    uint32_t v;
    int neg = 0;
    uint16_t *dst;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_max = BRIDGE_MEM16(str_va + 2);
    d_buf = BRIDGE_MEM32(str_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (base == 10 && (LONG)value < 0) {
        neg = 1;
        v = (uint32_t)(0u - value);
    } else {
        v = value;
    }
    if (v == 0)
        body[n_body++] = digs[0];
    while (v) {
        body[n_body++] = digs[v % base];
        v /= base;
    }
    for (i = 0; i < n_body / 2; i++) {
        uint16_t t = body[i];
        body[i] = body[n_body - 1 - i];
        body[n_body - 1 - i] = t;
    }

    total = n_body + (uint32_t)neg;
    if (total * 2 > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    dst = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    {
        uint32_t k = 0;
        if (neg)
            dst[k++] = (uint16_t)L'-';
        for (i = 0; i < n_body; i++)
            dst[k++] = body[i];
        dst[k] = 0;
    }
    BRIDGE_MEM16(str_va + 0) = (uint16_t)(total * 2);
    g_eax = 0;
}

static void bridge_RtlFillMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);
    uint32_t fill    = STACK_ARG(2);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), (int)(UCHAR)fill, (size_t)len);
    g_eax = 0;
}

static void bridge_RtlMoveMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t len     = STACK_ARG(2);

    if (dest_va && src_va && len)
        memmove(XBOX_TO_NATIVE(dest_va), XBOX_TO_NATIVE(src_va), (size_t)len);
    g_eax = 0;
}

static void bridge_RtlZeroMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), 0, (size_t)len);
    g_eax = 0;
}

static void bridge_RtlMultiByteToUnicodeN(void)
{
    uint32_t uni_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t mb_va     = STACK_ARG(3);
    uint32_t mb_bytes  = STACK_ARG(4);
    int chars, result;

    if (!uni_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    chars = (int)(max_bytes / sizeof(WCHAR));
    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes,
        uni_va ? (WCHAR *)XBOX_TO_NATIVE(uni_va) : NULL, chars);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

static void bridge_RtlMultiByteToUnicodeSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t mb_va    = STACK_ARG(1);
    uint32_t mb_bytes = STACK_ARG(2);
    int result;

    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes, NULL, 0);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

static void bridge_RtlUnicodeToMultiByteN(void)
{
    uint32_t mb_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va = STACK_ARG(2);
    uint32_t uni_va   = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    int result;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

static void bridge_RtlUnicodeToMultiByteSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t uni_va   = STACK_ARG(1);
    uint32_t uni_bytes = STACK_ARG(2);
    int result;

    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        NULL, 0, NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

static void bridge_RtlUpcaseUnicodeToMultiByteN(void)
{
    uint32_t mb_va     = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t uni_va    = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    uint32_t wc = uni_bytes / sizeof(WCHAR);
    uint16_t *tmp = NULL;
    int result;
    uint32_t i;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (wc) {
        tmp = (uint16_t *)malloc(((size_t)wc + 1) * sizeof(uint16_t));
        if (!tmp) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        {
            const uint16_t *sp = (const uint16_t *)XBOX_TO_NATIVE(uni_va);
            for (i = 0; i < wc; i++)
                tmp[i] = (uint16_t)towupper((wint_t)sp[i]);
            tmp[wc] = 0;
        }
    }
    result = WideCharToMultiByte(CP_ACP, 0,
        tmp, (int)wc,
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    free(tmp);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

static void bridge_RtlCharToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    const char *p;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0;
    uint32_t st = 0;

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    p = (const char *)XBOX_TO_NATIVE(str_va);

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\f' || *p == '\v')
        p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    if (base_ == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base_ = 16; p += 2; }
        else if (p[0] == '0') { base_ = 8; }
        else base_ = 10;
    }

    for (; *p; p++) {
        int d, c = (unsigned char)*p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

static void bridge_RtlUnicodeStringToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    uint16_t s_len;
    uint32_t s_buf;
    const uint16_t *wp;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0, i, nch;
    uint32_t st = 0;

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    s_len = BRIDGE_MEM16(str_va + 0);
    s_buf = BRIDGE_MEM32(str_va + 4);
    if (!s_buf) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    wp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    nch = s_len / 2;

    i = 0;
    while (i < nch && (wp[i] == (uint16_t)' ' || wp[i] == (uint16_t)'\t' ||
                       wp[i] == (uint16_t)'\n' || wp[i] == (uint16_t)'\r'))
        i++;
    if (i < nch && wp[i] == (uint16_t)'-') { neg = 1; i++; }
    else if (i < nch && wp[i] == (uint16_t)'+') i++;

    if (base_ == 0) {
        if (i + 1 < nch && wp[i] == (uint16_t)'0' &&
            (wp[i + 1] == (uint16_t)'x' || wp[i + 1] == (uint16_t)'X')) {
            base_ = 16;
            i += 2;
        } else if (i < nch && wp[i] == (uint16_t)'0') {
            base_ = 8;
        } else {
            base_ = 10;
        }
    }

    for (; i < nch; i++) {
        uint16_t c = wp[i];
        int d;
        if (c >= (uint16_t)'0' && c <= (uint16_t)'9') d = c - (uint16_t)'0';
        else if (c >= (uint16_t)'a' && c <= (uint16_t)'f') d = c - (uint16_t)'a' + 10;
        else if (c >= (uint16_t)'A' && c <= (uint16_t)'F') d = c - (uint16_t)'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

static void bridge_RtlUlongByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = ((v & 0x000000FFu) << 24) |
            ((v & 0x0000FF00u) << 8)  |
            ((v & 0x00FF0000u) >> 8)  |
            ((v & 0xFF000000u) >> 24);
}

static void bridge_RtlUshortByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}

#define BRIDGE_TRY_CS_SLOTS 512

typedef struct {
    uint32_t key;
    int      ready;
    CRITICAL_SECTION cs;
} BRIDGE_TRY_CS_SLOT;

static BRIDGE_TRY_CS_SLOT g_try_cs[BRIDGE_TRY_CS_SLOTS];
static CRITICAL_SECTION g_try_cs_guard;
static int g_try_cs_guard_ready;

static void bridge_try_cs_init_guard(void)
{

    if (!g_try_cs_guard_ready) {
        InitializeCriticalSection(&g_try_cs_guard);
        g_try_cs_guard_ready = 1;
    }
}

static CRITICAL_SECTION *bridge_try_cs_shadow(uint32_t guest)
{
    size_t home, i;

    bridge_try_cs_init_guard();
    EnterCriticalSection(&g_try_cs_guard);
    home = (size_t)((guest >> 4) % BRIDGE_TRY_CS_SLOTS);
    for (i = 0; i < BRIDGE_TRY_CS_SLOTS; i++) {
        BRIDGE_TRY_CS_SLOT *slot = &g_try_cs[(home + i) % BRIDGE_TRY_CS_SLOTS];
        if (slot->key == guest) {
            LeaveCriticalSection(&g_try_cs_guard);
            return &slot->cs;
        }
        if (slot->key == 0) {
            if (!slot->ready) {
                InitializeCriticalSection(&slot->cs);
                slot->ready = 1;
            }
            slot->key = guest;
            LeaveCriticalSection(&g_try_cs_guard);
            return &slot->cs;
        }
    }
    LeaveCriticalSection(&g_try_cs_guard);
    return NULL;
}

static void bridge_RtlEnterCriticalSectionAndRegion(void)
{
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSectionAndRegion(void)
{
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_RtlTryEnterCriticalSection(void)
{
    CRITICAL_SECTION *cs = bridge_try_cs_shadow(STACK_ARG(0));
    if (!cs) { g_eax = 0; return; }
    g_eax = TryEnterCriticalSection(cs) ? 1 : 0;
}

static void bridge_RtlGetCallersAddress(void)
{
    uint32_t a_va = STACK_ARG(0);
    uint32_t c_va = STACK_ARG(1);

    if (a_va) BRIDGE_MEM32(a_va) = 0;
    if (c_va) BRIDGE_MEM32(c_va) = 0;
    g_eax = 0;
}

static void bridge_RtlMapGenericMask(void)
{
    g_eax = 0;
}

static void bridge_RtlWalkFrameChain(void)
{
    g_eax = 0;
}

static void bridge_XboxEEPROMKey(void)
{
    static uint32_t eeprom_va = 0;

    if (!eeprom_va) {
        eeprom_va = xbox_HeapAlloc(16, 16);
        if (eeprom_va)
            memset(XBOX_TO_NATIVE(eeprom_va), 0, 16);
    }
    g_eax = eeprom_va;
}

static void bridge_RtlAssert(void)
{
    uint32_t assert_va = STACK_ARG(0);
    uint32_t file_va   = STACK_ARG(1);
    uint32_t line      = STACK_ARG(2);
    const char *a = assert_va ? (const char *)XBOX_TO_NATIVE(assert_va) : NULL;
    const char *f = file_va   ? (const char *)XBOX_TO_NATIVE(file_va) : NULL;

    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlAssert: %s:%u %s",
             f ? f : "?", line, a ? a : "?");
#ifdef _DEBUG
    DebugBreak();
#endif
    g_eax = 0;
}

static void bridge_RtlCaptureContext(void)
{
    uint32_t ctx_va = STACK_ARG(0);

    if (ctx_va)
        memset(XBOX_TO_NATIVE(ctx_va), 0, 0x2CC);
    g_eax = 0;
}

static void bridge_RtlCaptureStackBackTrace(void)
{
    g_eax = 0;
}

#define KE_SHADOW_SIZE 4096
typedef struct { uint32_t guest_va; HANDLE host_handle; uint32_t in_use; } KE_SHADOW_SLOT;
static KE_SHADOW_SLOT g_ke_shadow[KE_SHADOW_SIZE];
static CRITICAL_SECTION g_ke_shadow_cs;
static int g_ke_shadow_init;

static void ke_shadow_init(void)
{
    if (g_ke_shadow_init)
        return;
    InitializeCriticalSection(&g_ke_shadow_cs);

    memset(g_ke_shadow, 0, sizeof(g_ke_shadow));
    g_ke_shadow_init = 1;
}

static void ke_shadow_insert(uint32_t guest_va, HANDLE host)
{
    uint32_t i;

    if (!guest_va || !host)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);

    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle && g_ke_shadow[i].host_handle != host)
                CloseHandle(g_ke_shadow[i].host_handle);
            g_ke_shadow[i].host_handle = host;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return;
        }
    }

    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (!g_ke_shadow[i].in_use) {
            g_ke_shadow[i].in_use      = 1;
            g_ke_shadow[i].guest_va    = guest_va;
            g_ke_shadow[i].host_handle = host;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return;
        }
    }

    fprintf(stderr, "  [KERNEL] Ke shadow table full; dropping VA 0x%08X\n",
            guest_va);
    LeaveCriticalSection(&g_ke_shadow_cs);
    CloseHandle(host);
}

static HANDLE ke_shadow_lookup(uint32_t guest_va)
{
    uint32_t i;
    HANDLE h = NULL;

    if (!guest_va)
        return NULL;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            h = g_ke_shadow[i].host_handle;
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    return h;
}

static void ke_shadow_remove(uint32_t guest_va)
{
    uint32_t i;

    if (!guest_va)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle)
                CloseHandle(g_ke_shadow[i].host_handle);
            g_ke_shadow[i].in_use      = 0;
            g_ke_shadow[i].guest_va    = 0;
            g_ke_shadow[i].host_handle = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
}

static HANDLE ke_guest_event(uint32_t guest_va, int *type)
{
    static int enabled = -1;
    uint32_t hdr, i;
    HANDLE h = NULL;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_TITLE_KEVENTS");
        enabled = e && *e == '1';
    }
    if (!enabled)
        return NULL;
    if (guest_va < 0x10000u || (guest_va & 3u))
        return NULL;
    if (guest_va >= 0x04000000u && (guest_va < 0x80000000u || guest_va >= 0x84000000u))
        return NULL;
    hdr = BRIDGE_MEM32(guest_va);
    if (((hdr & 0xFFu) != 0 && (hdr & 0xFFu) != 1) || ((hdr >> 16) & 0xFFu) != 4)
        return NULL;
    *type = (int)(hdr & 0xFFu);

    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            h = g_ke_shadow[i].in_use == 2 ? g_ke_shadow[i].host_handle : NULL;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return h;
        }
    }
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (!g_ke_shadow[i].in_use) {
            h = CreateEventA(NULL, *type == 0, BRIDGE_MEM32(guest_va + 4) != 0, NULL);
            if (h) {
                g_ke_shadow[i].in_use      = 2;
                g_ke_shadow[i].guest_va    = guest_va;
                g_ke_shadow[i].host_handle = h;
            }
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    if (h) {
        static int shown;
        if (shown++ < 16)
            fprintf(stderr, "  [KERNEL] event 0x%08X built by the title: host event created "
                            "(%s)\n", guest_va, *type ? "synchronization" : "notification");
    }
    return h;
}

#ifndef KDATA_MMGLOBAL
#define KDATA_MMGLOBAL          0x510
#endif
#ifndef KDATA_INTERRUPT_TIME
#define KDATA_INTERRUPT_TIME    0x520
#endif
#ifndef KDATA_SYSTEM_TIME
#define KDATA_SYSTEM_TIME       0x530
#endif
#ifndef KDATA_OBJ_DIR_TYPE
#define KDATA_OBJ_DIR_TYPE      0x540
#endif
#ifndef KDATA_OBJ_HANDLE_TABLE
#define KDATA_OBJ_HANDLE_TABLE  0x548
#endif
#ifndef KDATA_OBJ_SYM_LINK_TYPE
#define KDATA_OBJ_SYM_LINK_TYPE 0x550
#endif

static void bridge_MmGlobalData(void)
{

    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL) =
        XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
}

static void bridge_KeInterruptTime(void)
{

    ULONGLONG t = (ULONGLONG)GetTickCount64() * 10000ull;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 0) = (uint32_t)t;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 4) = (uint32_t)(t >> 32);
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME;
}

static void bridge_KeSystemTime(void)
{

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 0) = ft.dwLowDateTime;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 4) = ft.dwHighDateTime;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME;
}

static void bridge_ObDirectoryObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
}

static void bridge_ObpObjectHandleTable(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
}

static void bridge_ObSymbolicLinkObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
}

static void bridge_DbgBreakPointWithStatus(void)
{
    (void)STACK_ARG(0);
    DebugBreak();
    g_eax = 0;
}

static void bridge_DbgLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static void bridge_DbgPrint(void)
{

    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(0));
    if (fmt && KERNEL_LOG_ON()) {
        size_t i, n = 0;
        for (i = 0; i < 512 && fmt[i]; i++)
            n++;
        fwrite(fmt, 1, n, stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }
    g_eax = 0;
}

static void bridge_DbgPrompt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_DbgUnLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static void bridge_ExAcquireReadWriteLockExclusive(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_ExAcquireReadWriteLockShared(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_ExInitializeReadWriteLock(void)
{
    uint32_t lock_va = STACK_ARG(0);
    if (lock_va)
        memset(XBOX_TO_NATIVE(lock_va), 0, 24);
    g_eax = 0;
}

static void bridge_ExInterlockedAddLargeInteger(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONGLONG increment = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
                       | (LONGLONG)(uint32_t)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);
    LONGLONG old = 0;

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
            old = cmp;
        } else {

            old = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            {
                LONGLONG newv = old + increment;
                p[0] = (LONG)(uint32_t)newv;
                p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
            }
        }
    }

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

static void bridge_ExInterlockedAddLargeStatistic(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONG increment = (LONG)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + (LONGLONG)increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
        } else {
            LONGLONG val = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            LONGLONG newv = val + (LONGLONG)increment;
            p[0] = (LONG)(uint32_t)newv;
            p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
        }
    }
    g_eax = 0;
}

static void bridge_ExInterlockedCompareExchange64(void)
{
    LONGLONG old = 0;
    volatile LONGLONG *qp;
    LONGLONG exchange;
    LONGLONG comparand;

    exchange  = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
              | (LONGLONG)(uint32_t)STACK_ARG(1);
    comparand = (LONGLONG)(uint32_t)STACK_ARG(3);
    qp = (volatile LONGLONG *)XBOX_TO_NATIVE(STACK_ARG(0));

    if (qp)
        old = InterlockedCompareExchange64(qp, exchange, comparand);

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

static void bridge_ExReadWriteRefurbInfo(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static void bridge_ExRaiseException(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_ExRaiseStatus(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_ExReleaseReadWriteLock(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_ExfInterlockedInsertHeadList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t old_flink = 0;

    if (h_flink) {

        old_flink = (uint32_t)InterlockedExchange(h_flink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = old_flink;
        BRIDGE_MEM32(entry_va + 4) = head_va;
        if (old_flink)
            BRIDGE_MEM32(old_flink + 4) = entry_va;
    }
    g_eax = old_flink;
}

static void bridge_ExfInterlockedInsertTailList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_blink = (volatile LONG *)XBOX_TO_NATIVE(head_va + 4);
    uint32_t old_blink = 0;

    if (h_blink) {

        old_blink = (uint32_t)InterlockedExchange(h_blink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = head_va;
        BRIDGE_MEM32(entry_va + 4) = old_blink;
        if (old_blink)
            BRIDGE_MEM32(old_blink + 0) = entry_va;
    }
    g_eax = old_blink;
}

static void bridge_ExfInterlockedRemoveHeadList(void)
{
    uint32_t head_va = STACK_ARG(0);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t removed = 0;

    if (h_flink) {
        for (;;) {
            uint32_t first = (uint32_t)*h_flink;
            if (first == head_va) {
                removed = 0;
                break;
            }

            {
                uint32_t entry_flink = BRIDGE_MEM32(first + 0);
                if (InterlockedCompareExchange(h_flink,
                                              (LONG)entry_flink,
                                              (LONG)first) == (LONG)first) {
                    if (entry_flink)
                        BRIDGE_MEM32(entry_flink + 4) = head_va;
                    removed = first;
                    break;
                }
            }
        }
    }
    g_eax = removed;
}

static void bridge_FscInvalidateIdleBlocks(void)
{
    g_eax = 0;
}

static void bridge_HalEnableSystemInterrupt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_HalEnableSecureTrayEject(void)
{
    g_eax = 0;
}

static void bridge_HalWriteSMCScratchRegister(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_InterlockedCompareExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedCompareExchange(dst, (LONG)g_edx, (LONG)g_eax);
}

static void bridge_InterlockedDecrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedDecrement(dst);
}

static void bridge_InterlockedIncrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedIncrement(dst);
}

static void bridge_InterlockedExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchange(dst, (LONG)g_edx);
}

static void bridge_InterlockedExchangeAdd(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchangeAdd(dst, (LONG)g_edx);
}

static void bridge_InterlockedFlushSList(void)
{
    (void)g_ecx;
    g_eax = 0;
}

static void bridge_InterlockedPopEntrySList(void)
{
    (void)g_ecx;
    g_eax = 0;
}

static void bridge_InterlockedPushEntrySList(void)
{
    (void)g_ecx;
    (void)g_edx;
    g_eax = 0;
}

static void bridge_IoAllocateIrp(void)
{
    uint32_t stack_size = STACK_ARG(0);
    uint32_t charge_quota = STACK_ARG(1);
    uint32_t total = 0x28 + stack_size * 0x20;
    uint32_t va;

    (void)charge_quota;
    va = xbox_HeapAlloc(total, 16);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, total);
    else
        fprintf(stderr, "  [KERNEL] IoAllocateIrp: %u bytes REFUSED\n", total);
    g_eax = va;
}

static void bridge_IoBuildAsynchronousFsdRequest(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

static void bridge_IoCheckShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_IoFreeIrp(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_IoQueryFileInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;
}

static void bridge_IoQueryVolumeInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;
}

static void bridge_IoQueueThreadIrp(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_IoRemoveShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
}

static void bridge_IoSetShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
}

static void bridge_IoDismountVolume(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_IoDismountVolumeByName(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_KdDebuggerEnabled(void)
{
    g_eax = 0;
}

static void bridge_KdDebuggerNotPresent(void)
{
    g_eax = 1;
}

static void bridge_KeInitializeEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t event_type = STACK_ARG(1);
    uint32_t initial_state = STACK_ARG(2);

    BOOL manual_reset = (event_type == 0) ? TRUE : FALSE;
    HANDLE h = CreateEventW(NULL, manual_reset, initial_state ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeEvent: CreateEventW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    BRIDGE_MEM8(guest_va + 0)  = (uint8_t)(event_type ? 1 : 0);
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = initial_state ? 1 : 0;
    BRIDGE_MEM32(guest_va + 8) = 0;
    BRIDGE_MEM32(guest_va + 12) = 0;

    g_eax = 0;
}

static void bridge_KeInitializeMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t initial_owner = STACK_ARG(1);

    HANDLE h = CreateMutexW(NULL, initial_owner ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeMutant: CreateMutexW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    BRIDGE_MEM8(guest_va + 0)  = 19;
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = initial_owner ? 1 : 0;
    BRIDGE_MEM32(guest_va + 8) = 0;
    BRIDGE_MEM32(guest_va + 12) = 0;

    g_eax = 0;
}

static void bridge_KeInitializeSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);
    uint32_t limit = STACK_ARG(2);

    HANDLE h = CreateSemaphoreW(NULL, (LONG)count, (LONG)limit, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeSemaphore: CreateSemaphoreW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    BRIDGE_MEM8(guest_va + 0)  = 18;
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = (LONG)count;
    BRIDGE_MEM32(guest_va + 8) = 0;
    BRIDGE_MEM32(guest_va + 12) = 0;

    g_eax = 0;
}

static void bridge_KePulseEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            PulseEvent(ge);
            BRIDGE_MEM32(guest_va + 4) = 0;
            g_eax = 0;
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        PulseEvent(h);

    g_eax = 0;
}

static void bridge_KeReleaseMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    uint32_t abandoned = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;
    (void)abandoned;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseMutex(h);

    g_eax = 0;
}

static void bridge_KeReleaseSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t adjustment = STACK_ARG(2);
    uint32_t wait = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseSemaphore(h, (LONG)adjustment, NULL);

    g_eax = 0;
}

static void bridge_KeResetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    HANDLE h;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            BRIDGE_MEM32(guest_va + 4) = 0;
            ResetEvent(ge);
            g_eax = 0;
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ResetEvent(h);

    g_eax = 0;
}

static void bridge_KeResumeThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)ResumeThread(hThread);
    else
        g_eax = 0;
}

static void bridge_KeSuspendThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)SuspendThread(hThread);
    else
        g_eax = 0;
}

static void bridge_KeAlertResumeThread(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_KeBoostPriorityThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeEnterCriticalRegion(void)
{
    g_eax = 0;
}

static void bridge_KeInitializeApc(void)
{
    uint32_t apc_va = STACK_ARG(0);

    if (apc_va) {
        BRIDGE_MEM8(apc_va + 0)  = 0;
        BRIDGE_MEM8(apc_va + 1)  = 0;
        BRIDGE_MEM8(apc_va + 2)  = 0;
        BRIDGE_MEM8(apc_va + 3)  = 0;
        BRIDGE_MEM32(apc_va + 4) = 0;
        BRIDGE_MEM32(apc_va + 8) = 0;
        BRIDGE_MEM32(apc_va + 12) = 0;
        BRIDGE_MEM32(apc_va + 16) = 0;
        BRIDGE_MEM32(apc_va + 20) = 0;
    }
    g_eax = 0;
}

static void bridge_KeInitializeDeviceQueue(void)
{
    uint32_t queue_va = STACK_ARG(0);

    if (queue_va) {
        BRIDGE_MEM8(queue_va + 0)  = 0;
        BRIDGE_MEM8(queue_va + 1)  = 0;
        BRIDGE_MEM8(queue_va + 2)  = 0;
        BRIDGE_MEM8(queue_va + 3)  = 0;
        BRIDGE_MEM32(queue_va + 4) = 0;
        BRIDGE_MEM32(queue_va + 8) = 0;
        BRIDGE_MEM32(queue_va + 12) = 0;
    }
    g_eax = 0;
}

static void bridge_KeInitializeQueue(void)
{
    uint32_t queue_va = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);

    if (queue_va) {
        BRIDGE_MEM8(queue_va + 0)  = 0;
        BRIDGE_MEM8(queue_va + 1)  = 0;
        BRIDGE_MEM8(queue_va + 2)  = 0;
        BRIDGE_MEM8(queue_va + 3)  = 1;
        BRIDGE_MEM32(queue_va + 4) = 0;
        BRIDGE_MEM32(queue_va + 8) = 0;
        BRIDGE_MEM32(queue_va + 12) = (LONG)count;
        BRIDGE_MEM32(queue_va + 16) = 0;
        BRIDGE_MEM32(queue_va + 20) = 0;
        BRIDGE_MEM32(queue_va + 24) = 0;
        BRIDGE_MEM32(queue_va + 28) = 0;
    }
    g_eax = queue_va;
}

static void bridge_KeInsertByKeyDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static void bridge_KeInsertDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeInsertHeadQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeInsertQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeInsertQueueApc(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 1;
}

static void bridge_KeIsExecutingDpc(void)
{
    g_eax = 0;
}

static void bridge_KeLeaveCriticalRegion(void)
{
    g_eax = 0;
}

static void bridge_KeRaiseIrqlToSynchLevel(void)
{
    g_eax = 0;
}

static void bridge_KeRemoveByKeyDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeRemoveDeviceQueue(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_KeRemoveEntryDeviceQueue(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeRemoveQueue(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_KeRundownQueue(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_KeSetEventBoostPriority(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    HANDLE h;

    (void)increment;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        SetEvent(h);

    g_eax = 0;
}

static void bridge_KeSetPriorityProcess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeSetPriorityThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_KeTestAlertThread(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_KiBugCheckData(void)
{
    g_eax = XBOX_KERNEL_DATA_BASE + 0x500;
}

static void bridge_KiUnlockDispatcherDatabase(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_KeGetCurrentIrql(void)
{
    g_eax = 0;
}

static void bridge_KeGetCurrentThread(void)
{
    g_eax = 0;
}

static void bridge_KeSetDisableBoostThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_MmIsAddressValid(void)
{
    (void)STACK_ARG(0);
    g_eax = 1;
}

static void bridge_MmDbgAllocateMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t va;

    (void)tag;
    if (!size) {
        g_eax = 0;
        return;
    }
    va = xbox_HeapAlloc(size, 16);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, size);
    g_eax = va;
}

static void bridge_MmDbgFreeMemory(void)
{
    (void)STACK_ARG(1);
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_MmDbgQueryAvailablePages(void)
{
    g_eax = 0x10000;
}

static void bridge_MmDbgReleaseAddress(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_MmDbgWriteCheck(void)
{
    (void)STACK_ARG(1);
    g_eax = STACK_ARG(0);
}

static void bridge_NtCancelTimer(void)
{
    HANDLE h = bridge_resolve_handle(STACK_ARG(0));
    (void)STACK_ARG(1);

    if (h)
        CancelWaitableTimer(h);
    g_eax = 0;
}

static void bridge_NtCreateIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 0;
}

static void bridge_NtCreateTimer(void)
{
    HANDLE h;

    (void)STACK_ARG(1);

    if (STACK_ARG(0) && !bridge_buf_ok(STACK_ARG(0), 4, "NtCreateTimer")) {
        g_eax = 0xC0000005u;
        return;
    }

    h = CreateWaitableTimerW(NULL, STACK_ARG(2) == 0, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] NtCreateTimer: CreateWaitableTimerW failed (error %u)\n",
                GetLastError());
        g_eax = 0xC000009Au;
        return;
    }
    if (STACK_ARG(0))
        bridge_write_handle(STACK_ARG(0), h);
    g_eax = 0;
}

static void bridge_NtOpenDirectoryObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static void bridge_NtProtectVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t new_prot = STACK_ARG(2);
    uint32_t old_ptr  = STACK_ARG(3);
    uint32_t base_va = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;
    uint32_t size    = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    DWORD old = 0;

    if (VirtualProtect(XBOX_TO_NATIVE(base_va), size, (DWORD)new_prot, &old)) {
        if (old_ptr)
            BRIDGE_MEM32(old_ptr) = (uint32_t)old;
        g_eax = 0;
    } else {
        g_eax = 0xC0000005u;
    }
}

static void bridge_NtQueueApcThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtQueryDirectoryObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

static void bridge_NtQueryEvent(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 0;
}

static void bridge_NtQueryIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtQueryMutant(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtQuerySemaphore(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtQueryTimer(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtReadFileScatter(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    (void)STACK_ARG(7);
    g_eax = 0;
}

static void bridge_NtRemoveIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtSetIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtSetTimerEx(void)
{
    HANDLE   h       = bridge_resolve_handle(STACK_ARG(0));
    uint32_t due_va  = STACK_ARG(1);
    uint32_t apc     = STACK_ARG(2);
    LONG     period  = (LONG)STACK_ARG(6);
    uint32_t prev_va = STACK_ARG(7);
    LARGE_INTEGER due;

    if (!h) {
        g_eax = 0xC0000008u;
        return;
    }
    if (!bridge_buf_ok(due_va, 8, "NtSetTimerEx")
            || (prev_va && !bridge_buf_ok(prev_va, 1, "NtSetTimerEx"))) {
        g_eax = 0xC0000005u;
        return;
    }
    due.QuadPart = (LONGLONG)((uint64_t)BRIDGE_MEM32(due_va)
                              | ((uint64_t)BRIDGE_MEM32(due_va + 4) << 32));
    if (apc) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [KERNEL] NtSetTimerEx: timer APC 0x%08X not delivered\n", apc);
    }
    if (prev_va)
        BRIDGE_MEM8(prev_va) = 0;
    g_eax = SetWaitableTimer(h, &due, period, NULL, NULL, FALSE) ? 0 : 0xC000000Du;
}

static void bridge_NtSignalAndWaitForSingleObjectEx(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_NtWriteFileGather(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    (void)STACK_ARG(7);
    g_eax = 0;
}

static void bridge_ObCreateObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

static void bridge_ObInsertObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    g_eax = 0;
}

static void bridge_ObMakeTemporaryObject(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_ObOpenObjectByName(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 0;
}

static void bridge_ObOpenObjectByPointer(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

static void bridge_ObReferenceObjectByPointer(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

static void bridge_PsCreateSystemThread(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(4);
    uint32_t start_context2  = STACK_ARG(5);
    uint32_t start_routine   = STACK_ARG(6);
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);

    g_thread_call_count++;

    if (xbox_handle_ptr)
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0002;

    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn)
            fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                xbox_DpcMarkGuestThread();
                xbox_GuestThreadPin("guest-main");
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                {
                    int took = xbox_GuestCpuEnterCallback();
                    fn();
                    xbox_GuestCpuLeaveCallback(took);
                }
                g_esp += 12;
            } else {
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    g_eax = 0;
                    return;
                }
                stack_top = xbox_AllocThreadStack();
                if (!stack_top) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    bridge_spawn_guest_thread(fn, start_context1, start_context2,
                                              stack_top, xbox_handle_ptr, 0);
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThread: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }
    g_eax = 0;
}

static void bridge_PsQueryStatistics(void)
{
    uint32_t stats_va = STACK_ARG(0);
    if (stats_va)
        memset(XBOX_TO_NATIVE(stats_va), 0, 0x80);
    g_eax = 0;
}

static void bridge_PsSetCreateThreadNotifyRoutine(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

static void bridge_RtlExtendedIntegerMultiply(void)
{
    int32_t mult = (int32_t)STACK_ARG(2);
    int64_t multi = ((int64_t)(int32_t)STACK_ARG(1) << 32)
                  | (uint32_t)STACK_ARG(0);
    int64_t result = multi * (int64_t)mult;

    g_eax = (uint32_t)result;
    g_edx = (uint32_t)((uint64_t)result >> 32);
}

static void bridge_RtlExtendedLargeIntegerDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {

        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

static void bridge_RtlExtendedMagicDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

static void bridge_RtlFillMemoryUlong(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t length = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!dst)
        return;
    for (i = 0; i < length / 4; i++)
        dst[i] = pattern;
    g_eax = 0;
}

static void bridge_RtlFreeAnsiString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf;

    if (!str_va) {
        g_eax = 0;
        return;
    }
    buf = BRIDGE_MEM32(str_va + 4);
    if (buf)
        xbox_HeapFree(buf);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

static void bridge_RtlRaiseStatus(void)
{
    uint32_t status = STACK_ARG(0);
    fprintf(stderr, "  [KERNEL] RtlRaiseStatus: 0x%08X (swallowed)\n", status);
    g_eax = 0;
}

static uint32_t bridge_va_next32(uint32_t *pos)
{
    uint32_t v = BRIDGE_MEM32(*pos);
    *pos += 4;
    return v;
}

static uint64_t bridge_va_next64(uint32_t *pos)
{
    uint32_t p = (*pos + 7) & ~7u;
    uint32_t lo = BRIDGE_MEM32(p);
    uint32_t hi = BRIDGE_MEM32(p + 4);
    *pos = p + 8;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static int bridge_format_printf(char *out, size_t cap, const char *fmt,
                                uint32_t pos)
{
    size_t used = 0;
    size_t i = 0;

    if (!out || cap == 0)
        return 0;

    while (fmt[i] && used + 1 < cap) {
        if (fmt[i] != '%') {
            out[used++] = fmt[i++];
            continue;
        }
        i++;
        if (fmt[i] == '%') {
            out[used++] = '%';
            i++;
            continue;
        }

        {
            char flags[8];
            int  nflags = 0;
            char conv[32];
            char *cp = conv;
            int  width = 0, have_width = 0, width_star = 0;
            int  prec  = 0, have_prec  = 0, prec_star  = 0;
            char len = 0;
            char spec;
            char tmp[512];
            size_t tlen;

            while (fmt[i] && strchr("-+ 0#", fmt[i]) && nflags < 7)
                flags[nflags++] = fmt[i++];
            flags[nflags] = 0;

            if (fmt[i] == '*') { width_star = 1; have_width = 1; i++; }
            else if (fmt[i] >= '0' && fmt[i] <= '9') {
                have_width = 1;
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    width = width * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == '.') {
                i++;
                have_prec = 1;
                if (fmt[i] == '*') { prec_star = 1; i++; }
                else while (fmt[i] >= '0' && fmt[i] <= '9') {
                    prec = prec * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == 'h') { len = 1; i++; if (fmt[i] == 'h') { len = 2; i++; } }
            else if (fmt[i] == 'l') { len = 3; i++; if (fmt[i] == 'l') { len = 4; i++; } }
            else if (fmt[i] == 'q') { len = 4; i++; }
            else if (fmt[i] == 'I' && fmt[i+1] == '6' && fmt[i+2] == '4') {
                len = 4; i += 3;
            }

            spec = fmt[i];
            if (!spec)
                break;
            i++;

            *cp++ = '%';
            if (nflags) { memcpy(cp, flags, (size_t)nflags); cp += nflags; }

            if (width_star) {
                int w = (int32_t)bridge_va_next32(&pos);
                if (w < 0) { *cp++ = '-'; w = -w; }
                cp += sprintf(cp, "%d", w);
            } else if (have_width) {
                cp += sprintf(cp, "%d", width);
            }
            if (have_prec) {
                *cp++ = '.';
                if (prec_star) {
                    int p = (int32_t)bridge_va_next32(&pos);
                    if (p < 0) p = 0;
                    cp += sprintf(cp, "%d", p);
                } else {
                    cp += sprintf(cp, "%d", prec);
                }
            }

            if (len == 2) { *cp++ = 'h'; *cp++ = 'h'; }
            else if (len == 1) { *cp++ = 'h'; }
            else if (len == 4) { *cp++ = 'l'; *cp++ = 'l'; }

            switch (spec) {
            case 'd': case 'i': {
                if (len == 4) {
                    long long v = (long long)bridge_va_next64(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    int v = (int)(int32_t)bridge_va_next32(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'u': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'o': case 'x': case 'X': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'c': {
                int v = (int)bridge_va_next32(&pos);
                *cp++ = 'c'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            case 's': {
                uint32_t sva = bridge_va_next32(&pos);
                const char *gs = sva ? (const char *)XBOX_TO_NATIVE(sva)
                                     : "(null)";
                *cp++ = 's'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, gs);
                break;
            }
            case 'p': {

                unsigned int v = bridge_va_next32(&pos);
                const char *pfx = strchr(flags, '#') ? "%#08x" : "%08x";
                snprintf(tmp, sizeof tmp, pfx, v);
                break;
            }
            case 'n': {
                uint32_t tva = bridge_va_next32(&pos);
                if (tva)
                    BRIDGE_MEM32(tva) = (uint32_t)used;
                tlen = 0;
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G': {
                double v = (double)bridge_va_next64(&pos);
                *cp++ = spec; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            default:

                out[used++] = '%';
                out[used++] = spec;
                continue;
            }

            tlen = (spec == 'n') ? 0 : strlen(tmp);
            {
                size_t k;
                for (k = 0; k < tlen && used + 1 < cap; k++)
                    out[used++] = tmp[k];
            }
        }
    }

    if (used + 1 >= cap)
        used = cap - 1;
    out[used] = 0;
    return (int)used;
}

static void bridge_RtlSnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }

    g_eax = bridge_format_printf(dst, count, fmt, g_esp + 12);
}

static void bridge_RtlSprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }

    g_eax = bridge_format_printf(dst, 65536, fmt, g_esp + 8);
}

static void bridge_RtlVsnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));
    uint32_t  list = STACK_ARG(3);

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, count, fmt, list);
}

static void bridge_RtlVsprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t  list = STACK_ARG(2);

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, 65536, fmt, list);
}

static void bridge_READ_PORT_BUFFER_UCHAR(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count);
    g_eax = 0;
}

static void bridge_READ_PORT_BUFFER_USHORT(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint16_t));
    g_eax = 0;
}

static void bridge_READ_PORT_BUFFER_ULONG(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint32_t));
    g_eax = 0;
}

static void bridge_WRITE_PORT_BUFFER_UCHAR(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {

    case   1: return  0;
    case   2: return 16;
    case   3: return 24;
    case   4: return  4;
    case   5: return  0;
    case   8: return  0;
    case   9: return  8;
    case  14: return  4;
    case  15: return  8;
    case  17: return  4;
    case  23: return  4;
    case  24: return 20;
    case  29: return 16;
    case  35: return  0;
    case  37: return  4;
    case  38: return  4;
    case  39: return  8;
    case  42: return  0;
    case  44: return  8;
    case  47: return  8;
    case  46: return 24;
    case  45: return 16;
    case  50: return 16;
    case  48: return  4;
    case  49: return  4;
    case  61: return 36;
    case  62: return 28;
    case  65: return 24;
    case  66: return 40;
    case  67: return  8;
    case  68: return  4;
    case  69: return  4;
    case  73: return 12;
    case  74: return  8;
    case  79: return 20;
    case  81: return  8;
    case  82: return 12;
    case  83: return 16;
    case  84: return 32;
    case  85: return 20;
    case  86: return  0;
    case  87: return  0;

    case  90: return  4;
    case  91: return  4;
    case  93: return  8;
    case  95: return  4;
    case  96: return 20;
    case  97: return  4;
    case  98: return  4;
    case  99: return 12;
    case 100: return  4;
    case 107: return 12;
    case 109: return 28;
    case 113: return  8;
    case 119: return 12;
    case 124: return  4;
    case 125: return  0;
    case 126: return  0;
    case 127: return  0;
    case 128: return  4;
    case 129: return  0;
    case 137: return  4;
    case 139: return  4;
    case 142: return  4;
    case 143: return  8;
    case 144: return  8;
    case 145: return 12;
    case 149: return 16;
    case 150: return 20;
    case 151: return  4;
    case 153: return 12;
    case 158: return 32;
    case 159: return 20;
    case 160: return  0;
    case 161: return  0;
    case 165: return  4;
    case 166: return 20;
    case 167: return  8;
    case 168: return  8;
    case 169: return  8;
    case 170: return  8;
    case 171: return  4;
    case 172: return  8;
    case 173: return  4;
    case 175: return 12;
    case 176: return  8;
    case 177: return 12;
    case 178: return 12;
    case 179: return  4;
    case 180: return  4;
    case 181: return  4;
    case 182: return 12;
    case 183: return  8;
    case 184: return 20;
    case 185: return  8;
    case 186: return  4;
    case 187: return  4;
    case 188: return  8;
    case 189: return 16;
    case 190: return 36;
    case 191: return 16;
    case 192: return 12;
    case 193: return 16;
    case 194: return 12;
    case 195: return  4;
    case 196: return 40;
    case 197: return 12;
    case 198: return  8;
    case 199: return 12;
    case 200: return 40;
    case 202: return 24;
    case 203: return  8;
    case 204: return 16;
    case 205: return  8;
    case 206: return 20;
    case 207: return 40;
    case 210: return  8;
    case 211: return 20;
    case 215: return 12;
    case 217: return 8;

    case 218: return 20;
    case 219: return 32;
    case 220: return 32;
    case 221: return  8;
    case 222: return 12;
    case 223: return 20;
    case 224: return  8;
    case 225: return  8;
    case 226: return 20;
    case 227: return 20;
    case 228: return  8;
    case 229: return 32;
    case 230: return 20;
    case 231: return  8;
    case 232: return 12;
    case 233: return 12;
    case 234: return 16;
    case 235: return 24;
    case 236: return 32;
    case 237: return 32;
    case 238: return  0;
    case 243: return 16;
    case 247: return 20;
    case 250: return  0;
    case 251: return  0;
    case 252: return  4;
    case 253: return  8;
    case 255: return 40;
    case 258: return  4;
    case 260: return 12;
    case 268: return 12;
    case 269: return 12;
    case 270: return 12;
    case 261: return  8;
    case 262: return  8;
    case 263: return  8;
    case 264: return 12;
    case 265: return  4;
    case 266: return 20;
    case 267: return 12;
    case 271: return 12;
    case 272: return  8;
    case 273: return  8;
    case 274: return  8;
    case 275: return  4;
    case 276: return 12;
    case 278: return  4;
    case 280: return 12;
    case 284: return 12;
    case 287: return  4;
    case 288: return  8;
    case 292: return 16;
    case 293: return 12;
    case 295: return  4;
    case 296: return  4;
    case 297: return  8;
    case 298: return 12;
    case 299: return 20;
    case 300: return 12;
    case 306: return  4;
    case 307: return  4;
    case 309: return 12;
    case 310: return 20;
    case 311: return 12;
    case 313: return  4;
    case 314: return 12;
    case 315: return 20;
    case 316: return  4;
    case 317: return  8;
    case 318: return  4;
    case 319: return 12;
    case 320: return  8;
    case 321: return  0;
    case 277: return  4;
    case 279: return 12;
    case 285: return 12;
    case 286: return  4;
    case 289: return  8;
    case 290: return  8;
    case 291: return  4;
    case 294: return  4;
    case 301: return  4;
    case 302: return  4;
    case 304: return  8;
    case 305: return  8;
    case 308: return 12;
    case 312: return 16;
    case 327: return  4;
    case 328: return  4;
    case 333: return 12;
    case 334: return 12;
    case 335: return  4;
    case 336: return 12;
    case 337: return  8;
    case 338: return 12;
    case 339: return 12;
    case 340: return 28;
    case 341: return 12;
    case 342: return 12;
    case 343: return  4;
    case 344: return 12;
    case 345: return 20;
    case 346: return  8;
    case 347: return 12;
    case 348: return 20;
    case 349: return 28;
    case 350: return  8;
    case 351: return  8;
    case 352: return 12;
    case 358: return  0;
    case 359: return  4;

    case 246: return 12;
    case 360: return  0;

    case 102: return 0;
    case 120: return 0;
    case 154: return 0;
    case 240: return 0;
    case 245: return 0;
    case 249: return 0;

    case   6: return  4;
    case   7: return 12;
    case  10: return  8;
    case  11: return 12;

    case  12: return  4;
    case  13: return  4;
    case  18: return  4;
    case  19: return 12;
    case  20: return  8;
    case  21: return 16;
    case  25: return 12;
    case  26: return  4;
    case  27: return  4;
    case  28: return  4;

    case  32: return  8;
    case  33: return  8;
    case  34: return  4;

    case  36: return  0;

    case  43: return  8;
    case 365: return  0;
    case 366: return  4;

    case  51: return  0;
    case  52: return  0;
    case  53: return  0;
    case  54: return  0;
    case  55: return  0;
    case  56: return  0;
    case  57: return  0;
    case  58: return  0;

    case  59: return  8;
    case  60: return 28;
    case  63: return 20;
    case  72: return  4;
    case  75: return 20;
    case  76: return 20;
    case  77: return  4;
    case  78: return  8;
    case  80: return 20;

    case  88: return  0;
    case  89: return  0;

    case  92: return  4;
    case  94: return  8;
    case 101: return  0;
    case 103: return  0;
    case 104: return  0;
    case 105: return 24;
    case 106: return  4;
    case 108: return 12;
    case 110: return  8;
    case 111: return  8;
    case 112: return 12;
    case 114: return 12;
    case 115: return  8;
    case 116: return  8;
    case 117: return  8;
    case 118: return 16;
    case 121: return  0;
    case 122: return  0;
    case 123: return 12;
    case 130: return  0;
    case 131: return 16;
    case 132: return 16;
    case 133: return  8;
    case 134: return  4;
    case 135: return  8;
    case 136: return  4;
    case 138: return  4;
    case 140: return  4;
    case 141: return  4;
    case 146: return  8;
    case 147: return  8;
    case 148: return  8;
    case 152: return  4;
    case 155: return  4;
    case 162: return  0;
    case 163: return  4;

    case 174: return  4;
    case 374: return  8;
    case 375: return  8;
    case 376: return  0;
    case 377: return  8;
    case 378: return  8;

    case 201: return 12;
    case 208: return 28;
    case 209: return 16;
    case 212: return 20;
    case 213: return 20;
    case 214: return 20;
    case 216: return 20;

    case 239: return 28;
    case 241: return 24;
    case 242: return  4;
    case 244: return 20;
    case 248: return  4;

    case 254: return 28;
    case 256: return  4;
    case 257: return  8;

    case 281: return 12;
    case 282: return 16;
    case 283: return 16;
    case 303: return  4;
    case 361: return  0;
    case 362: return  0;
    case 363: return  0;
    case 364: return  0;

    case 329: return 12;
    case 330: return 12;
    case 331: return 12;
    case 332: return 12;

    default:  return -1;
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {

    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;

    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  23: return bridge_ExQueryPoolBlockSize;
    case 268: return bridge_RtlCompareMemory;
    case 269: return bridge_RtlCompareMemoryUlong;
    case  35: return bridge_FscGetCacheSize;
    case  37: return bridge_FscSetCacheSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    case 291: return bridge_RtlInitializeCriticalSection;

    case   2: return bridge_AvSendTVEncoderOption;

    case 305: return bridge_RtlTimeToTimeFields;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimerEx;

    case 107: return bridge_KeInitializeDpc;
    case 119: return bridge_KeInsertQueueDpc;
    case 137: return bridge_KeRemoveQueueDpc;
    case 153: return bridge_KeSynchronizeExecution;
    case 113: return bridge_KeInitializeTimerEx;

    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 179: return bridge_MmQueryAddressProtect;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 238: return bridge_NtYieldExecution;

    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    case   3: return bridge_AvSetDisplayMode;

    case  66: return bridge_IoCreateFile;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;

    case 261: return bridge_RtlAppendStringToString;
    case 262: return bridge_RtlAppendUnicodeStringToString;
    case 263: return bridge_RtlAppendUnicodeToString;
    case 264: return bridge_RtlAssert;
    case 265: return bridge_RtlCaptureContext;
    case 266: return bridge_RtlCaptureStackBackTrace;
    case 267: return bridge_RtlCharToInteger;
    case 270: return bridge_RtlCompareString;
    case 271: return bridge_RtlCompareUnicodeString;
    case 272: return bridge_RtlCopyString;
    case 273: return bridge_RtlCopyUnicodeString;
    case 274: return bridge_RtlCreateUnicodeString;
    case 275: return bridge_RtlDowncaseUnicodeChar;
    case 276: return bridge_RtlDowncaseUnicodeString;
    case 278: return bridge_RtlEnterCriticalSectionAndRegion;
    case 280: return bridge_RtlEqualUnicodeString;
    case 284: return bridge_RtlFillMemory;
    case 287: return bridge_RtlFreeUnicodeString;
    case 288: return bridge_RtlGetCallersAddress;
    case 292: return bridge_RtlIntegerToChar;
    case 293: return bridge_RtlIntegerToUnicodeString;
    case 295: return bridge_RtlLeaveCriticalSectionAndRegion;
    case 296: return bridge_RtlLowerChar;
    case 297: return bridge_RtlMapGenericMask;
    case 298: return bridge_RtlMoveMemory;
    case 299: return bridge_RtlMultiByteToUnicodeN;
    case 300: return bridge_RtlMultiByteToUnicodeSize;
    case 306: return bridge_RtlTryEnterCriticalSection;
    case 307: return bridge_RtlUlongByteSwap;
    case 309: return bridge_RtlUnicodeStringToInteger;
    case 310: return bridge_RtlUnicodeToMultiByteN;
    case 311: return bridge_RtlUnicodeToMultiByteSize;
    case 313: return bridge_RtlUpcaseUnicodeChar;
    case 314: return bridge_RtlUpcaseUnicodeString;
    case 315: return bridge_RtlUpcaseUnicodeToMultiByteN;
    case 316: return bridge_RtlUpperChar;
    case 317: return bridge_RtlUpperString;
    case 318: return bridge_RtlUshortByteSwap;
    case 319: return bridge_RtlWalkFrameChain;
    case 320: return bridge_RtlZeroMemory;
    case 321: return bridge_XboxEEPROMKey;

    case 338: return bridge_XcRC4Key;
    case 339: return bridge_XcRC4Crypt;

    case   1: return bridge_AvGetSavedDataAddress;
    case  17: return bridge_ExFreePool;
    case  97: return bridge_KeCancelTimer;
    case 100: return bridge_KeDisconnectInterrupt;

    case 124: return bridge_KeQueryBasePriorityThread;
    case 143: return bridge_KeSetBasePriorityThread;

    case 151: return bridge_KeStallExecutionProcessor;
    case 175: return bridge_MmLockUnlockBufferPages;

    case 180: return bridge_MmQueryAllocationSize;
    case 192: return bridge_NtCreateMutant;
    case 221: return bridge_NtReleaseMutant;

    case 224: return bridge_NtResumeThread;
    case 231: return bridge_NtSuspendThread;

    case 250: return bridge_ObfDereferenceObject;
    case 252: return bridge_PhyGetLinkState;
    case 253: return bridge_PhyInitialize;

    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    case 340: return bridge_XcHMAC;
    case 346: return bridge_XcDESKeyParity;

    case  29: return bridge_ExSaveNonVolatileSetting;
    case  38: return bridge_HalClearSoftwareInterrupt;
    case  45: return bridge_HalReadSMBusValue;
    case  46: return bridge_HalReadWritePCISpace;
    case  48: return bridge_HalRequestSoftwareInterrupt;
    case  50: return bridge_HalWriteSMBusValue;
    case  68: return bridge_IoDeleteDevice;
    case  93: return bridge_KeAlertThread;
    case 125: return bridge_KeQueryInterruptTime;
    case 139: return bridge_KeRestoreFloatingPointState;
    case 142: return bridge_KeSaveFloatingPointState;
    case 158: return bridge_KeWaitForMultipleObjects;
    case 183: return bridge_MmUnmapIoSpace;
    case 193: return bridge_NtCreateSemaphore;
    case 222: return bridge_NtReleaseSemaphore;
    case 228: return bridge_NtSetSystemTime;
    case 247: return bridge_ObReferenceObjectByName;
    case 251: return bridge_ObfReferenceObject;
    case 290: return bridge_RtlInitUnicodeString;
    case 304: return bridge_RtlTimeFieldsToTime;

    case   4: return bridge_AvSetSavedDataAddress;
    case   5: return bridge_DbgBreakPoint;
    case  39: return bridge_HalDisableSystemInterrupt;
    case 360: return bridge_HalInitiateShutdown;
    case 358: return bridge_HalIsResetOrShutdownPending;
    case 333: return bridge_WRITE_PORT_BUFFER_USHORT;
    case 334: return bridge_WRITE_PORT_BUFFER_ULONG;

    case  61: return bridge_IoBuildDeviceIoControlRequest;
    case  62: return bridge_IoBuildSynchronousFsdRequest;
    case  69: return bridge_IoDeleteSymbolicLink;
    case  73: return bridge_IoInitializeIrp;
    case  74: return bridge_IoInvalidDeviceRequest;
    case  79: return bridge_IoSetIoCompletion;
    case  81: return bridge_IoStartNextPacket;
    case  82: return bridge_IoStartNextPacketByKey;
    case  83: return bridge_IoStartPacket;
    case  84: return bridge_IoSynchronousDeviceIoControlRequest;
    case  85: return bridge_IoSynchronousFsdRequest;
    case  86: return bridge_IofCallDriver;
    case  87: return bridge_IofCompleteRequest;
    case 359: return bridge_IoMarkIrpMustComplete;

    case 176: return bridge_MmLockUnlockPhysicalPage;

    case 167: return bridge_MmAllocateSystemMemory;
    case 172: return bridge_MmFreeSystemMemory;
    case 169: return bridge_MmCreateKernelStack;
    case 170: return bridge_MmDeleteKernelStack;

    case 341: return bridge_XcPKEncPublic;
    case 342: return bridge_XcPKDecPrivate;
    case 343: return bridge_XcPKGetKeyLen;
    case 344: return bridge_XcVerifyPKCS1Signature;
    case 345: return bridge_XcModExp;
    case 347: return bridge_XcKeyTable;
    case 348: return bridge_XcBlockCrypt;
    case 349: return bridge_XcBlockCryptCBC;
    case 350: return bridge_XcCryptService;
    case 351: return bridge_XcUpdateCrypto;

    case 352: return bridge_RtlRip;
    case 260: return bridge_RtlAnsiStringToUnicodeString;
    case 308: return bridge_RtlUnicodeStringToAnsiString;
    case 197: return bridge_NtDuplicateObject;

    case 102: return bridge_MmGlobalData;
    case 120: return bridge_KeInterruptTime;
    case 154: return bridge_KeSystemTime;
    case 240: return bridge_ObDirectoryObjectType;
    case 245: return bridge_ObpObjectHandleTable;
    case 249: return bridge_ObSymbolicLinkObjectType;

    case   6: return bridge_DbgBreakPointWithStatus;
    case   7: return bridge_DbgLoadImageSymbols;
    case   8: return bridge_DbgPrint;
    case  10: return bridge_DbgPrompt;
    case  11: return bridge_DbgUnLoadImageSymbols;

    case  12: return bridge_ExAcquireReadWriteLockExclusive;
    case  13: return bridge_ExAcquireReadWriteLockShared;
    case  18: return bridge_ExInitializeReadWriteLock;
    case  19: return bridge_ExInterlockedAddLargeInteger;
    case  20: return bridge_ExInterlockedAddLargeStatistic;
    case  21: return bridge_ExInterlockedCompareExchange64;
    case  25: return bridge_ExReadWriteRefurbInfo;
    case  26: return bridge_ExRaiseException;
    case  27: return bridge_ExRaiseStatus;
    case  28: return bridge_ExReleaseReadWriteLock;

    case  32: return bridge_ExfInterlockedInsertHeadList;
    case  33: return bridge_ExfInterlockedInsertTailList;
    case  34: return bridge_ExfInterlockedRemoveHeadList;

    case  36: return bridge_FscInvalidateIdleBlocks;

    case  43: return bridge_HalEnableSystemInterrupt;
    case 365: return bridge_HalEnableSecureTrayEject;
    case 366: return bridge_HalWriteSMCScratchRegister;

    case  51: return bridge_InterlockedCompareExchange;
    case  52: return bridge_InterlockedDecrement;
    case  53: return bridge_InterlockedIncrement;
    case  54: return bridge_InterlockedExchange;
    case  55: return bridge_InterlockedExchangeAdd;
    case  56: return bridge_InterlockedFlushSList;
    case  57: return bridge_InterlockedPopEntrySList;
    case  58: return bridge_InterlockedPushEntrySList;

    case  59: return bridge_IoAllocateIrp;
    case  60: return bridge_IoBuildAsynchronousFsdRequest;
    case  63: return bridge_IoCheckShareAccess;
    case  72: return bridge_IoFreeIrp;
    case  75: return bridge_IoQueryFileInformation;
    case  76: return bridge_IoQueryVolumeInformation;
    case  77: return bridge_IoQueueThreadIrp;
    case  78: return bridge_IoRemoveShareAccess;
    case  80: return bridge_IoSetShareAccess;
    case  90: return bridge_IoDismountVolume;
    case  91: return bridge_IoDismountVolumeByName;

    case  88: return bridge_KdDebuggerEnabled;
    case  89: return bridge_KdDebuggerNotPresent;

    case  92: return bridge_KeAlertResumeThread;
    case  94: return bridge_KeBoostPriorityThread;
    case 101: return bridge_KeEnterCriticalRegion;
    case 103: return bridge_KeGetCurrentIrql;
    case 104: return bridge_KeGetCurrentThread;
    case 105: return bridge_KeInitializeApc;
    case 106: return bridge_KeInitializeDeviceQueue;
    case 108: return bridge_KeInitializeEvent;
    case 110: return bridge_KeInitializeMutant;
    case 111: return bridge_KeInitializeQueue;
    case 112: return bridge_KeInitializeSemaphore;
    case 114: return bridge_KeInsertByKeyDeviceQueue;
    case 115: return bridge_KeInsertDeviceQueue;
    case 116: return bridge_KeInsertHeadQueue;
    case 117: return bridge_KeInsertQueue;
    case 118: return bridge_KeInsertQueueApc;
    case 121: return bridge_KeIsExecutingDpc;
    case 122: return bridge_KeLeaveCriticalRegion;
    case 123: return bridge_KePulseEvent;
    case 130: return bridge_KeRaiseIrqlToSynchLevel;
    case 131: return bridge_KeReleaseMutant;
    case 132: return bridge_KeReleaseSemaphore;
    case 133: return bridge_KeRemoveByKeyDeviceQueue;
    case 134: return bridge_KeRemoveDeviceQueue;
    case 135: return bridge_KeRemoveEntryDeviceQueue;
    case 136: return bridge_KeRemoveQueue;
    case 138: return bridge_KeResetEvent;
    case 140: return bridge_KeResumeThread;
    case 141: return bridge_KeRundownQueue;
    case 144: return bridge_KeSetDisableBoostThread;
    case 146: return bridge_KeSetEventBoostPriority;
    case 147: return bridge_KeSetPriorityProcess;
    case 148: return bridge_KeSetPriorityThread;
    case 152: return bridge_KeSuspendThread;
    case 155: return bridge_KeTestAlertThread;
    case 162: return bridge_KiBugCheckData;
    case 163: return bridge_KiUnlockDispatcherDatabase;

    case 174: return bridge_MmIsAddressValid;
    case 374: return bridge_MmDbgAllocateMemory;
    case 375: return bridge_MmDbgFreeMemory;
    case 376: return bridge_MmDbgQueryAvailablePages;
    case 377: return bridge_MmDbgReleaseAddress;
    case 378: return bridge_MmDbgWriteCheck;

    case 185: return bridge_NtCancelTimer;
    case 191: return bridge_NtCreateIoCompletion;
    case 194: return bridge_NtCreateTimer;
    case 201: return bridge_NtOpenDirectoryObject;
    case 204: return bridge_NtProtectVirtualMemory;
    case 206: return bridge_NtQueueApcThread;
    case 208: return bridge_NtQueryDirectoryObject;
    case 209: return bridge_NtQueryEvent;
    case 212: return bridge_NtQueryIoCompletion;
    case 213: return bridge_NtQueryMutant;
    case 214: return bridge_NtQuerySemaphore;
    case 216: return bridge_NtQueryTimer;
    case 220: return bridge_NtReadFileScatter;
    case 223: return bridge_NtRemoveIoCompletion;
    case 227: return bridge_NtSetIoCompletion;
    case 229: return bridge_NtSetTimerEx;
    case 230: return bridge_NtSignalAndWaitForSingleObjectEx;
    case 237: return bridge_NtWriteFileGather;

    case 239: return bridge_ObCreateObject;
    case 241: return bridge_ObInsertObject;
    case 242: return bridge_ObMakeTemporaryObject;
    case 243: return bridge_ObOpenObjectByName;
    case 244: return bridge_ObOpenObjectByPointer;
    case 248: return bridge_ObReferenceObjectByPointer;

    case 254: return bridge_PsCreateSystemThread;
    case 256: return bridge_PsQueryStatistics;
    case 257: return bridge_PsSetCreateThreadNotifyRoutine;

    case 281: return bridge_RtlExtendedIntegerMultiply;
    case 282: return bridge_RtlExtendedLargeIntegerDivide;
    case 283: return bridge_RtlExtendedMagicDivide;
    case 285: return bridge_RtlFillMemoryUlong;
    case 286: return bridge_RtlFreeAnsiString;
    case 303: return bridge_RtlRaiseStatus;
    case 361: return bridge_RtlSnprintf;
    case 362: return bridge_RtlSprintf;
    case 363: return bridge_RtlVsnprintf;
    case 364: return bridge_RtlVsprintf;

    case 329: return bridge_READ_PORT_BUFFER_UCHAR;
    case 330: return bridge_READ_PORT_BUFFER_USHORT;
    case 331: return bridge_READ_PORT_BUFFER_ULONG;
    case 332: return bridge_WRITE_PORT_BUFFER_UCHAR;

    default:  return NULL;
    }
}

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];
static uint8_t g_slot_arg_unknown[XBOX_KERNEL_THUNK_TABLE_SIZE];

uint32_t g_kernel_watch_va = 0;

static void kernel_watch_arm_once(void)
{
    static int done;
    const char *env;
    if (done)
        return;
    done = 1;
    env = getenv("RECOMP_KERNEL_WATCH");
    if (env)
        g_kernel_watch_va = (uint32_t)strtoul(env, NULL, 0);
}

static RECOMP_TLS int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch_body(void);

void guest_cpu_join(void)
{
    xbox_GuestCpuJoin();
}

void guest_cpu_part(void)
{
    xbox_GuestCpuPart();
}

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    int held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL,
                                    slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE
                                    ? g_slot_ordinals[slot] : 0);
    kernel_thunk_dispatch_body();
    if (held)
        xbox_GuestCpuAcquire();
}

static void kernel_thunk_dispatch_body(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    xbox_IrqSafePoint();

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;
    if (ordinal < XBOX_KERNEL_THUNK_TABLE_SIZE)
        g_ordinal_calls[ordinal]++;

    if (KERNEL_LOG_ON()) {

        fprintf(stderr,
                "  [KERNEL] #%lld: ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %lld total calls, latest ordinal %u (slot %d) esp=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp);

            {
                static unsigned char shown_ord[XBOX_KERNEL_THUNK_TABLE_SIZE];
                int r, shown;

                memset(shown_ord, 0, sizeof shown_ord);
                for (shown = 0; shown < 6; shown++) {
                    int best = -1;
                    for (r = 0; r < XBOX_KERNEL_THUNK_TABLE_SIZE; r++)
                        if (g_ordinal_calls[r] && !shown_ord[r]
                            && (best < 0 || g_ordinal_calls[r] > g_ordinal_calls[best]))
                            best = r;
                    if (best < 0)
                        break;
                    shown_ord[best] = 1;
                    fprintf(stderr, "  [KERNEL]   ordinal %3d x%llu\n", best,
                            (unsigned long long)g_ordinal_calls[best]);
                }
            }
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    g_xbox_kernel_caller = g_esp ? BRIDGE_MEM32(g_esp) : 0;
    g_esp += 4;

    uint32_t _watch_before = 0;
    kernel_watch_arm_once();
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);

        if (getenv("RECOMP_KERNEL_WATCH_ALL")) {
            static uint32_t seen = 0xDEADBEEFu;
            if (_watch_before != seen) {
                seen = _watch_before;
                fprintf(stderr, "  [KWATCH] 0x%08X = %08X before ordinal %u"
                                " (call #%lld)\n",
                        g_kernel_watch_va, _watch_before, ordinal,
                        g_kernel_call_count);
                fflush(stderr);
            }
        }
    }

    if (bridge) {
        bridge();
    } else {

        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);

            if (g_slot_arg_unknown[slot])
                fprintf(stderr, "  [KERNEL]   ordinal %u has no entry in "
                        "stdcall_args_for_ordinal(). If it takes arguments, "
                        "this call is corrupting the caller's stack -- add its "
                        "argument size there before anything else.\n", ordinal);
            fflush(stderr);
        }
        g_eax = 0;
    }

    g_esp += g_slot_arg_bytes[slot];

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

void xbox_kernel_get_thunk_address(uint32_t *xbox_va, uint32_t *count)
{
    if (xbox_va) *xbox_va = g_thunk_table_base;
    if (count)  *count  = g_thunk_table_count;
}

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    xbox_DpcMarkGuestThread();
    guest_cpu_join();
    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {

            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {

                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            { int _n = stdcall_args_for_ordinal(ordinal);
              g_slot_arg_unknown[i] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[i] = (_n < 0) ? 0 : _n; }
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    {
        const int LOOKBEHIND = 16;
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            if (!(current & 0x80000000u) ||
                (current & 0x7FFFFFFFu) == 0 ||
                (current & 0x7FFFFFFFu) > XBOX_KERNEL_THUNK_TABLE_SIZE) {
                continue;
            }
            slot = g_thunk_table_count + i;
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            { int _n = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
              g_slot_arg_unknown[slot] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[slot] = (_n < 0) ? 0 : _n; }
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}

