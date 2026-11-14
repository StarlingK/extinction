#include "ohci.h"
#include "../platform/mmio_decode.h"
#include "../platform/mmio_decode_a64.h"
#include "../platform/mmio_decode_x86_64.h"
#include "../kernel/xbox_memory_layout.h"
#include "../kernel/kernel.h"
#include "usb_gamepad.h"
#include "../input/xinput_xbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#if defined(_WIN32)
#  define OHCI_TRAP_WIN32 1
#elif defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
#  define OHCI_TRAP_POSIX 1
#endif

#if defined(__aarch64__)
#define MMIO_CTX_PC(uc) A64_CTX_PC(uc)
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);

#if defined(_MSC_VER)
#  define OHCI_TLS __declspec(thread)
#else
#  define OHCI_TLS __thread
#endif
extern OHCI_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern OHCI_TLS uint32_t g_ebx, g_esi, g_edi;
extern OHCI_TLS uint32_t g_fs_base;
extern uint32_t xbox_AllocThreadTib(void);

#define OHCI_VECTOR  1

#define HcRevision              0x00
#define HcControl               0x04
#define HcCommandStatus         0x08
#define HcInterruptStatus       0x0C
#define HcInterruptEnable       0x10
#define HcInterruptDisable      0x14
#define HcHCCA                  0x18
#define HcPeriodCurrentED       0x1C
#define HcControlHeadED         0x20
#define HcControlCurrentED      0x24
#define HcBulkHeadED            0x28
#define HcBulkCurrentED         0x2C
#define HcDoneHead              0x30
#define HcFmInterval            0x34
#define HcFmRemaining           0x38
#define HcFmNumber              0x3C
#define HcPeriodicStart         0x40
#define HcLSThreshold           0x44
#define HcRhDescriptorA         0x48
#define HcRhDescriptorB         0x4C
#define HcRhStatus              0x50
#define HcRhPortStatus1         0x54

#define OHCI_REG_MAX            0x64

#define CS_HCR                  0x00000001u

#define INTR_SO                 0x00000001u
#define INTR_WDH                0x00000002u
#define INTR_SF                 0x00000004u
#define INTR_RD                 0x00000008u
#define INTR_UE                 0x00000010u
#define INTR_FNO                0x00000020u
#define INTR_RHSC               0x00000040u

#define HCCA_DONE_HEAD 0x84
static void wr32(uint32_t va, uint32_t v);
#define INTR_MIE                0x80000000u

#define PORT_CCS                0x00000001u
#define PORT_PES                0x00000002u
#define PORT_PSS                0x00000004u
#define PORT_PPS                0x00000100u
#define PORT_LSDA               0x00000200u
#define PORT_CSC                0x00010000u
#define PORT_PESC               0x00020000u
#define PORT_PRSC               0x00100000u

#define PORT_W_CCS_CLEAR_ENABLE 0x00000001u
#define PORT_W_PES_SET_ENABLE   0x00000002u
#define PORT_W_PRS_SET_RESET    0x00000010u
#define PORT_W_PPS_SET_POWER    0x00000100u
#define PORT_W_CLEAR_POWER      0x00000200u

#define OHCI_PORTS              4

#define OHCI_IRQ_HOLDOFF        500

#define OHCI_TICK_MS            4u

typedef struct {
    uint32_t base;
    uint32_t reg[OHCI_REG_MAX / 4];
    unsigned reads, writes, decode_fail;
    int      index;
    int      periodic_seen;
    int      ple_seen;

    volatile unsigned ack_seq;
} OhciController;

static OhciController s_hc[2];

static int s_device_hc;

static unsigned s_ndp = OHCI_PORTS;
static int s_npads = 1;
static int s_enabled;
static int s_trace;

static uint32_t *reg_of(OhciController *hc, uint32_t off)
{
    return (off < OHCI_REG_MAX) ? &hc->reg[off / 4] : NULL;
}

static uint64_t ohci_read(void *dev, uint32_t off, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = r ? *r : 0;

    hc->reads++;

    if (aligned == HcFmNumber)
        v = (hc->reads >> 3) & 0xFFFFu;
    else if (aligned == HcFmRemaining)
        v = (hc->reads * 977u) & 0x3FFFu;

    if (size < 4) {
        unsigned shift = (off & 3u) * 8u;
        v >>= shift;
        if (size == 1) v &= 0xFFu;
        else if (size == 2) v &= 0xFFFFu;
    }

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] read  +0x%02X = %08X\n",
                    hc->index, off, (uint32_t)v);
    }
    return v;
}

static void ohci_write(void *dev, uint32_t off, uint64_t val, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = (uint32_t)val;

    hc->writes++;
    if (!r)
        return;

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] write +0x%02X = %08X\n",
                    hc->index, off, v);
    }

    switch (aligned) {
    case HcRevision:
        return;

    case HcCommandStatus:

        *r |= v;
        if (*r & CS_HCR) {
            *r &= ~CS_HCR;
            hc->reg[HcControl / 4] &= ~0xC0u;
            hc->reg[HcInterruptStatus / 4] = 0;
            hc->reg[HcInterruptEnable / 4] = 0;
        }
        return;

    case HcInterruptStatus:

        if ((v & INTR_WDH) && (*r & INTR_WDH)) {
            uint32_t hcca = hc->reg[HcHCCA / 4];
            if (hcca)
                wr32(hcca + HCCA_DONE_HEAD, 0);
            hc->reg[HcDoneHead / 4] = 0;
        }
        *r &= ~v;
        hc->ack_seq++;
        return;

    case HcInterruptEnable:
        hc->reg[HcInterruptEnable / 4] |= v;
        return;

    case HcInterruptDisable:
        hc->reg[HcInterruptEnable / 4] &= ~v;
        return;

    case HcFmNumber:
    case HcFmRemaining:
        return;

    case HcRhDescriptorA:

        *r = (*r & 0x000000FFu) | (v & ~0x000000FFu);
        return;

    case HcRhPortStatus1:
    case HcRhPortStatus1 + 4:
    case HcRhPortStatus1 + 8:
    case HcRhPortStatus1 + 12: {

        unsigned port = (aligned - HcRhPortStatus1) / 4;
        uint32_t *ps = &hc->reg[(HcRhPortStatus1 + port * 4) / 4];

        *ps &= ~(v & 0xFFFF0000u);

        if (v & PORT_W_CCS_CLEAR_ENABLE) *ps &= ~PORT_PES;
        if (v & PORT_W_PES_SET_ENABLE)   *ps |= (*ps & PORT_CCS) ? PORT_PES : 0;
        if (v & PORT_W_PPS_SET_POWER)    *ps |= PORT_PPS;
        if (v & PORT_W_CLEAR_POWER)      *ps &= ~PORT_PPS;
        if (v & PORT_W_PRS_SET_RESET) {

            if (*ps & PORT_CCS)
                *ps |= PORT_PES;
            *ps |= PORT_PRSC;
            hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
        }
        return;
    }

    default:
        break;
    }

    if (size == 4) {
        *r = v;
    } else {
        unsigned shift = (off & 3u) * 8u;
        uint32_t mask = ((size == 1) ? 0xFFu : 0xFFFFu) << shift;
        *r = (*r & ~mask) | ((v << shift) & mask);
    }
}

#define ED_SKIP        (1u << 14)
#define ED_HEAD_HALT   (1u << 0)
#define ED_HEAD_TOGGLE (1u << 1)
#define ED_PTR_MASK    0xFFFFFFF0u

#define TD_DP_SETUP    0u
#define TD_DP_OUT      1u
#define TD_DP_IN       2u
#define TD_CC_NOERROR  0u
#define TD_CC_STALL    4u
#define TD_CC_DATAUNDERRUN 9u
#define TD_ROUNDING    (1u << 18)

static uint32_t g_setup_pending_[USB_GAMEPAD_MAX];
static UsbSetup g_setup_[USB_GAMEPAD_MAX];
#define g_setup_pending g_setup_pending_[pad]
#define g_setup         g_setup_[pad]

static uint8_t g_ctrl_buf_[USB_GAMEPAD_MAX][64];
static int     g_ctrl_len_[USB_GAMEPAD_MAX] = {-1, -1, -1, -1};
static int     g_ctrl_sent_[USB_GAMEPAD_MAX];
#define g_ctrl_buf  g_ctrl_buf_[pad]
#define g_ctrl_len  g_ctrl_len_[pad]
#define g_ctrl_sent g_ctrl_sent_[pad]

static int s_plugged_pads;

static int ohci_pad_for(uint32_t ed0)
{
    uint32_t fa = ed0 & 0x7Fu;
    int d;
    for (d = 0; d < s_plugged_pads; d++)
        if (fa ? usb_gamepad_address(d) == fa : usb_gamepad_address(d) == 0)
            return d;
    return -1;
}

#define OHCI_CONTIG_BASE 0x80000000u
#define OHCI_CONTIG_SIZE (64u * 1024u * 1024u)

static int guest_ok(uint32_t va, uint32_t bytes)
{
    size_t mapped = xbox_GetMappedSize();

    if (va == 0 || mapped == 0)
        return 0;

    if (va >= OHCI_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)OHCI_CONTIG_BASE
                                     + OHCI_CONTIG_SIZE)
        return 1;
    return (uint64_t)va + bytes <= (uint64_t)mapped;
}

static uint32_t bus_resolve(uint32_t addr)
{
    if (addr >= g_xbox_image_lo && addr < g_xbox_image_hi)
        return addr;
    if (addr && addr < xbox_ContiguousAllocatedBytes())
        return OHCI_CONTIG_BASE + addr;
    return addr;
}

static uint32_t rd32(uint32_t va)
{
    va = bus_resolve(va);
    if (!guest_ok(va, 4))
        return 0;
    return *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}
static void wr32(uint32_t va, uint32_t v)
{
    va = bus_resolve(va);
    if (!guest_ok(va, 4))
        return;
    *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va) = v;
}
static uint8_t *guest_ptr(uint32_t va, uint32_t bytes)
{
    va = bus_resolve(va);
    return guest_ok(va, bytes)
         ? (uint8_t *)xbox_GetMemoryOffset() + va : NULL;
}

static unsigned long s_n_report, s_n_ctrl, s_n_out_ep0, s_n_out_other;

static uint32_t ohci_do_td(OhciController *hc, uint32_t ed0, uint32_t td)
{
    uint32_t info = rd32(td);
    uint32_t cbp  = rd32(td + 4);
    uint32_t be   = rd32(td + 12);
    uint32_t dp   = (info >> 19) & 3u;
    int      len  = (cbp && be >= cbp) ? (int)(be - cbp + 1) : 0;
    uint32_t endpoint = (ed0 >> 7) & 0xFu;
    int      moved = 0;
    int      pad = ohci_pad_for(ed0);

    if (pad < 0)
        return TD_CC_NOERROR;

    if (s_trace) {
        static unsigned n;
        if (n++ < 80) {
            fprintf(stderr, "  [OHCI%d] TD %08X dp=%u ep=%u cbp=%08X be=%08X "
                            "len=%d\n", hc->index, td, dp, endpoint, cbp, be,
                    len);
            fflush(stderr);
        }
    }
    if (dp == TD_DP_SETUP) {

        s_n_ctrl++;
        if (len >= 8) {
            const uint8_t *p = guest_ptr(cbp, 8);
            if (!p) return TD_CC_NOERROR;
            g_setup.bmRequestType = p[0];
            g_setup.bRequest      = p[1];
            g_setup.wValue        = (uint16_t)(p[2] | (p[3] << 8));
            g_setup.wIndex        = (uint16_t)(p[4] | (p[5] << 8));
            g_setup.wLength       = (uint16_t)(p[6] | (p[7] << 8));
            g_setup_pending = 1;
            g_ctrl_len = -1;
            g_ctrl_sent = 0;
            if (s_trace) {
                fprintf(stderr, "  [OHCI%d] SETUP %02X %02X value %04X "
                                "index %04X len %u\n",
                        hc->index, g_setup.bmRequestType, g_setup.bRequest,
                        g_setup.wValue, g_setup.wIndex, g_setup.wLength);
                fflush(stderr);
            }
            moved = 8;
        }
    } else if (dp == TD_DP_IN) {
        if (endpoint == 0) {

            int n;

            if (!g_setup_pending)
                return TD_CC_NOERROR;
            if (g_ctrl_len < 0) {
                g_ctrl_len = usb_gamepad_control(pad, &g_setup, g_ctrl_buf,
                                                 (int)sizeof g_ctrl_buf);
                if (g_ctrl_len < 0) {

                    fprintf(stderr, "  [OHCI%d] STALL: unhandled control "
                            "request %02X %02X value %04X index %04X len %u\n",
                            hc->index, g_setup.bmRequestType, g_setup.bRequest,
                            g_setup.wValue, g_setup.wIndex, g_setup.wLength);
                    fflush(stderr);
                    g_setup_pending = 0;
                    return TD_CC_STALL;
                }
                g_ctrl_sent = 0;
            }

            n = g_ctrl_len - g_ctrl_sent;
            if (n > len) n = len;
            if (n < 0) n = 0;
            if (n > 0 && guest_ptr(cbp, (uint32_t)n))
                memcpy(guest_ptr(cbp, (uint32_t)n),
                       g_ctrl_buf + g_ctrl_sent, (size_t)n);
            g_ctrl_sent += n;
            moved = n;
            if (s_trace && n > 0) {
                fprintf(stderr, "  [OHCI%d] IN ep0 %d bytes (%d/%d)\n",
                        hc->index, n, g_ctrl_sent, g_ctrl_len);
                fflush(stderr);
            }
        } else {

            uint8_t rep[32];
            s_n_report++;
            int n = usb_gamepad_report(pad, rep, (int)sizeof rep);
            if (n > len) n = len;
            if (n > 0 && guest_ptr(cbp, (uint32_t)n))
                memcpy(guest_ptr(cbp, (uint32_t)n), rep, (size_t)n);
            moved = n;
        }
    } else {

        moved = len;

        if (len >= 6 && guest_ptr(cbp, 6u)
            && (endpoint != 0 || (g_setup_pending && g_setup.bmRequestType == 0x21
                                  && g_setup.bRequest == 0x09))) {
            static int rumble = -1;
            const uint8_t *r = (const uint8_t *)guest_ptr(cbp, 6u);
            if (rumble < 0) {
                const char *e = getenv("RECOMP_RUMBLE");
                rumble = !(e && *e == '0');
            }
            if (rumble && r[0] == 0x00 && r[1] >= 6) {
                XBOX_VIBRATION v;
                v.wLeftMotorSpeed  = (WORD)(r[2] | r[3] << 8);
                v.wRightMotorSpeed = (WORD)(r[4] | r[5] << 8);
                if (s_trace)
                    fprintf(stderr, "  [OHCI%d] rumble pad %d left %u right %u (ep %u)\n",
                            hc->index, pad, v.wLeftMotorSpeed, v.wRightMotorSpeed, endpoint);
                xbox_InputSetState((DWORD)pad, &v);
            }
        }
        if (endpoint == 0) {
            g_setup_pending = 0;
            s_n_out_ep0++;
        } else {
            s_n_out_other++;
        }
    }

    wr32(td + 4, (moved >= len) ? 0u : cbp + (uint32_t)moved);

    if (dp == TD_DP_IN && moved < len && !(info & TD_ROUNDING))
        return TD_CC_DATAUNDERRUN;
    return TD_CC_NOERROR;
}

static int ohci_walk_eds(OhciController *hc, uint32_t ed, uint32_t *done_head)
{
    int completed = 0, guard = 0;

    while (guest_ok(ed, 16) && ++guard < 64) {
        uint32_t ed0  = rd32(ed);
        uint32_t tail = rd32(ed + 4) & ED_PTR_MASK;
        uint32_t head = rd32(ed + 8);
        int tguard = 0;

        if (s_trace) {
            static unsigned n;
            if (n++ < 60) {
                fprintf(stderr, "  [OHCI%d] ED %08X info=%08X head=%08X "
                                "tail=%08X next=%08X\n", hc->index, ed, ed0,
                        head, tail, rd32(ed + 12));
                fflush(stderr);
            }
        }
        if (ed0 & ED_SKIP) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }
        if (head & ED_HEAD_HALT) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }

        while ((head & ED_PTR_MASK) != tail
            && guest_ok(head & ED_PTR_MASK, 16) && ++tguard < 64) {
            uint32_t td = head & ED_PTR_MASK;
            uint32_t next = rd32(td + 8) & ED_PTR_MASK;
            uint32_t cc = ohci_do_td(hc, ed0, td);

            wr32(td, (rd32(td) & 0x0FFFFFFFu) | (cc << 28));
            wr32(td + 8, *done_head);
            *done_head = td;
            completed++;

            head = next | (head & ED_HEAD_TOGGLE);
            if (cc != TD_CC_NOERROR) {
                head |= ED_HEAD_HALT;
                break;
            }
        }
        wr32(ed + 8, head);
        ed = rd32(ed + 12) & ED_PTR_MASK;
    }

    return completed;
}

static void ohci_publish_done(OhciController *hc, uint32_t done_head)
{
    uint32_t hcca = hc->reg[HcHCCA / 4];

    if (hcca)
        wr32(hcca + HCCA_DONE_HEAD, done_head);
    hc->reg[HcDoneHead / 4] = done_head;
    hc->reg[HcInterruptStatus / 4] |= INTR_WDH;
}

static int ohci_run_control_list(OhciController *hc, uint32_t *done_head)
{
    return ohci_walk_eds(hc, hc->reg[HcControlHeadED / 4] & ED_PTR_MASK,
                         done_head);
}

static int ohci_run_periodic_list(OhciController *hc, uint32_t *done_head)
{
    uint32_t hcca = hc->reg[HcHCCA / 4];
    uint32_t slot, ed;
    int completed;

    if (!hcca)
        return 0;

    completed = 0;
    for (slot = 0; slot < 32u; slot++) {
        ed = rd32(hcca + slot * 4u) & ED_PTR_MASK;
        if (!ed)
            continue;

        if (!hc->periodic_seen) {
            hc->periodic_seen = 1;
            fprintf(stderr, "  [OHCI%d] periodic slot %u -> ED %08X "
                    "info=%08X\n", hc->index, slot, ed, rd32(ed));
            fflush(stderr);
        }
        completed += ohci_walk_eds(hc, ed, done_head);
    }

    {
        static int stats = -1;
        static unsigned long last;
        static unsigned total;
        unsigned long now;
        total += (unsigned)completed;
        if (stats < 0)
            stats = getenv("RECOMP_USB_STATS") != NULL;
        now = (unsigned long)GetTickCount();
        if (stats && now - last >= 5000) {
            uint32_t seen[8] = {0};
            int nseen = 0, i;
            last = now;
            fprintf(stderr, "  [OHCI%d] stats: %u periodic transfers; "
                    "reports %lu setups %lu out0 %lu outN %lu; "
                    "ctl=%08X ien=%08X ists=%08X irql-depth=%d",
                    hc->index, total, s_n_report, s_n_ctrl, s_n_out_ep0,
                    s_n_out_other, hc->reg[HcControl / 4],
                    hc->reg[HcInterruptEnable / 4],
                    hc->reg[HcInterruptStatus / 4], xbox_IrqlRaisedCount());
            {
                uint32_t c = hc->reg[HcControlHeadED / 4] & ED_PTR_MASK;
                int n;
                fprintf(stderr, " cmd=%08X ctlhead=%08X",
                        hc->reg[HcCommandStatus / 4], c);
                for (n = 0; c && n < 4; n++, c = rd32(c + 12) & ED_PTR_MASK)
                    fprintf(stderr, " [cED %08X info=%08X head=%08X tail=%08X]",
                            c, rd32(c), rd32(c + 8), rd32(c + 4));
            }
            for (slot = 0; slot < 32u; slot++) {
                ed = rd32(hcca + slot * 4u) & ED_PTR_MASK;
                while (ed && nseen < 8) {
                    for (i = 0; i < nseen && seen[i] != ed; i++) ;
                    if (i < nseen)
                        break;
                    seen[nseen++] = ed;
                    if (rd32(ed) & 0x7FFu)
                        fprintf(stderr, " | ED %08X info=%08X head=%08X "
                                "tail=%08X", ed, rd32(ed), rd32(ed + 8),
                                rd32(ed + 4));
                    ed = rd32(ed + 12) & ED_PTR_MASK;
                }
            }
            fprintf(stderr, "\n");

            if (xbox_IrqlRaisedCount() > 0)
                xbox_IrqlDumpHolders();
            fflush(stderr);
        }
    }
    return completed;
}

static int ohci_isr_call(int index)
{
    uint32_t kinterrupt = xbox_GetConnectedInterrupt(OHCI_VECTOR);
    uint32_t routine, context;
    recomp_func_t fn;
    uint8_t *mem;

    if (!kinterrupt)
        return -1;

    mem     = (uint8_t *)xbox_GetMemoryOffset();
    routine = *(uint32_t *)(mem + kinterrupt + 0);
    context = *(uint32_t *)(mem + kinterrupt + 4);
    if (!routine)
        return -1;

    fn = recomp_lookup(routine);
    if (!fn) {
        fprintf(stderr, "  [OHCI%d] ISR 0x%08X has no translation\n",
                index, routine);
        fflush(stderr);
        return -1;
    }

    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kinterrupt;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;

    { int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql); }
    return (int)(g_eax & 1u);
}

static int ohci_call_isr(OhciController *hc)
{
    int slot, claimed;

    if (!xbox_GetConnectedInterrupt(OHCI_VECTOR))
        return -1;

    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        fprintf(stderr, "  [OHCI%d] no worker stack for the ISR\n", hc->index);
        fflush(stderr);
        return -1;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    claimed = ohci_isr_call(hc->index);
    xbox_worker_stack_free(slot);
    return claimed;
}

static volatile int s_posting_hc;

static void ohci_isr_on_holder(void)
{
    ohci_isr_call(s_posting_hc);
}

static void ohci_raise(OhciController *hc, uint32_t source)
{
    uint32_t enable = hc->reg[HcInterruptEnable / 4];
    int claimed;

    hc->reg[HcInterruptStatus / 4] |= source;

    if (!(enable & INTR_MIE) || !(enable & source))
        return;

    claimed = ohci_call_isr(hc);
    if (s_trace || claimed >= 0) {
        static unsigned n;
        if (n++ < 20) {
            fprintf(stderr, "  [OHCI%d] raised %08X -> ISR %s\n",
                    hc->index, source,
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
            fflush(stderr);
        }
    }
}

static void ohci_reset(OhciController *hc, uint32_t base, int index)
{
    memset(hc, 0, sizeof *hc);
    hc->base  = base;
    hc->index = index;

    hc->reg[HcRevision / 4]       = 0x00000010u;
    hc->reg[HcFmInterval / 4]     = 0x27782EDFu;
    hc->reg[HcPeriodicStart / 4]  = 0x00003E67u;
    hc->reg[HcLSThreshold / 4]    = 0x00000628u;

    hc->reg[HcRhDescriptorA / 4]  = (uint32_t)s_ndp | (1u << 9);
    hc->reg[HcRhDescriptorB / 4]  = 0x00000000u;
    hc->reg[HcRhStatus / 4]       = 0x00000000u;

    {
        unsigned p;
        for (p = 0; p < 4u; p++)
            hc->reg[(HcRhPortStatus1 + p * 4) / 4] =
                (p < s_ndp) ? PORT_PPS : 0u;
    }
}

static DWORD WINAPI ohci_thread(LPVOID unused)
{

    unsigned waited = 0;
    int plugged = 0;
    uint32_t last_status = 0;
    unsigned repeats = 0;
    unsigned held_off = 0;
    int      held_off_warned = 0, held_off_forced = 0, gated = 0;
    unsigned last_ack = 0;
    unsigned first_port = 0, settle = 0;

    (void)unused;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "  [OHCI0] no TIB for the controller thread; "
                            "not delivering interrupts\n");
            fflush(stderr);
            return 0;
        }
        g_fs_base = tib;
    }

    for (;;) {
        OhciController *hc = &s_hc[s_device_hc];
        uint32_t control, enable, status;
        uint32_t pass_done = 0;

        Sleep(OHCI_TICK_MS);
        control = hc->reg[HcControl / 4];
        enable  = hc->reg[HcInterruptEnable / 4];
        if (s_trace) {
            static unsigned beat;
            if (beat++ % 100 == 0) {
                fprintf(stderr, "  [OHCI0] beat: control=%08X enable=%08X plugged=%d waited=%u\n",
                        control, enable, plugged, waited);
                fflush(stderr);
            }
        }

        if ((control & 0xC0u) != 0x80u) {

            ++waited;
            continue;
        }
        if (!(enable & INTR_MIE))
            continue;

        if (!plugged && (enable & INTR_RHSC)) {

            const char *pspec = getenv("RECOMP_USB_PORT");
            unsigned port = pspec ? (unsigned)atoi(pspec) : (s_ndp >= 4 ? 2u : 0u);
            if (port >= s_ndp) port = s_ndp - 1u;
            hc->reg[(HcRhPortStatus1 + port * 4) / 4] |= PORT_CCS | PORT_CSC;
            hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
            plugged = 1;
            s_plugged_pads = 1;
            first_port = port;
            fprintf(stderr, "  [OHCI0] operational after %u ms; device "
                            "arriving on port %u\n", waited * 20, port + 1);
            fflush(stderr);
        }

        if (plugged && s_plugged_pads < s_npads
            && usb_gamepad_configured(s_plugged_pads - 1)) {
            if (++settle > 50) {
                unsigned port = (first_port + (unsigned)s_plugged_pads) % s_ndp;
                hc->reg[(HcRhPortStatus1 + port * 4) / 4] |= PORT_CCS | PORT_CSC;
                hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
                fprintf(stderr, "  [OHCI0] pad %d arriving on port %u%c",
                        s_plugged_pads + 1, port + 1, 10);
                fflush(stderr);
                s_plugged_pads++;
                settle = 0;
            }
        }

        {
            uint32_t hcca = hc->reg[HcHCCA / 4];

            hc->reg[HcFmNumber / 4] =
                (hc->reg[HcFmNumber / 4] + OHCI_TICK_MS) & 0xFFFFu;

            if (hcca)
                wr32(hcca + 0x80, hc->reg[HcFmNumber / 4]);
            hc->reg[HcInterruptStatus / 4] |= INTR_SF;
        }

        if (hc->reg[HcInterruptStatus / 4] & INTR_WDH)
            goto deliver;

        if (control & 0x04u) {
            if (!hc->ple_seen) {
                hc->ple_seen = 1;
                fprintf(stderr, "  [OHCI%d] periodic list enabled "
                        "(HcControl=%08X HCCA=%08X)\n",
                        hc->index, control, hc->reg[HcHCCA / 4]);
                fflush(stderr);
            }
            ohci_run_periodic_list(hc, &pass_done);
        }

        if ((control & 0x20u)
         && guest_ok(hc->reg[HcBulkHeadED / 4] & ED_PTR_MASK, 16)) {
            if (ohci_walk_eds(hc, hc->reg[HcBulkHeadED / 4] & ED_PTR_MASK,
                              &pass_done))
                hc->reg[HcCommandStatus / 4] &= ~0x04u;
        }

        if ((control & 0x10u)
         && guest_ok(hc->reg[HcControlHeadED / 4] & ED_PTR_MASK, 16)) {
            if (ohci_run_control_list(hc, &pass_done))
                hc->reg[HcCommandStatus / 4] &= ~0x02u;
        }

        if (pass_done)
            ohci_publish_done(hc, pass_done);

    deliver:
        status = hc->reg[HcInterruptStatus / 4] & enable & 0x7Fu;
        if (!status)
            continue;

        xbox_IrqSetHandler(XBOX_IRQ_OHCI, ohci_isr_on_holder);
        s_posting_hc = hc->index;
        if (xbox_IrqSafePointsOn()) {
            int how = xbox_IrqPost(XBOX_IRQ_OHCI);
            if (how == XBOX_IRQ_POSTED)
                continue;
            gated = (how == XBOX_IRQ_GATED);
            if (!gated)
                xbox_IrqNoteUngated(XBOX_IRQ_OHCI);
        } else {

            gated = xbox_DispatchGateOn() ? xbox_DispatchGateTryEnter()
                                          : !xbox_IrqlBlocksInterrupts();
            if (!gated && ++held_off <= OHCI_IRQ_HOLDOFF) {
                if (!held_off_warned) {
                    held_off_warned = 1;
                    fprintf(stderr, "  [OHCI%d] irq held off by guest IRQL "
                            "(depth %d, status=0x%02X)\n",
                            hc->index, xbox_IrqlRaisedCount(), status);
                    fflush(stderr);
                }
                continue;
            }
            if (held_off > OHCI_IRQ_HOLDOFF && !held_off_forced) {
                held_off_forced = 1;
                fprintf(stderr, "  [OHCI%d] guest IRQL never dropped (depth %d); "
                        "delivering anyway\n", hc->index, xbox_IrqlRaisedCount());
                fflush(stderr);
            }
            if (!gated)
                xbox_IrqNoteUngated(XBOX_IRQ_OHCI);
            held_off = 0;
        }

        if (status == last_status && hc->ack_seq == last_ack) {
            if (++repeats > 200) {
                fprintf(stderr, "  [OHCI%d] status %08X delivered 200 times "
                                "with no acknowledgement; stopping\n",
                        hc->index, status);
                fflush(stderr);
                if (gated)
                    xbox_DispatchGateLeave();
                break;
            }
        } else {
            last_status = status;
            last_ack    = hc->ack_seq;
            repeats     = 0;
        }
        ohci_raise(hc, status);
        if (gated)
            xbox_DispatchGateLeave();
    }
    return 0;
}

void xbox_OhciInit(void)
{
    static int done;

    if (done)
        return;
    done = 1;

    if (!getenv("RECOMP_USB"))
        return;

    s_enabled = 1;
    s_trace   = getenv("RECOMP_USB_TRACE") != NULL;
    {
        const char *hcspec = getenv("RECOMP_USB_HC");
        const char *ndpspec = getenv("RECOMP_USB_NDP");
        s_device_hc = (hcspec && atoi(hcspec) == 1) ? 1 : 0;
        const char *padspec = getenv("RECOMP_USB_PADS");
        if (ndpspec) {
            int n = atoi(ndpspec);
            if (n >= 1 && n <= 4) s_ndp = (unsigned)n;
        }
        if (padspec) {
            int n = atoi(padspec);
            if (n >= 1 && n <= USB_GAMEPAD_MAX) s_npads = n;
        }
        if ((unsigned)s_npads > s_ndp)
            s_ndp = (unsigned)s_npads;
    }
#if !defined(OHCI_TRAP_WIN32) && !defined(OHCI_TRAP_POSIX)

    s_enabled = 0;
    fprintf(stderr, "  OHCI: no trapped-MMIO decoder for this host; "
                    "USB controllers disabled\n");
    fflush(stderr);
    return;
#endif

    ohci_reset(&s_hc[0], XBOX_OHCI0_BASE, 0);
    ohci_reset(&s_hc[1], XBOX_OHCI1_BASE, 1);

    {
        ptrdiff_t off = xbox_GetMemoryOffset();
        int i;

        if (!off) {
            s_enabled = 0;
            fprintf(stderr, "  OHCI: guest memory not mapped yet; disabled\n");
            return;
        }
        for (i = 0; i < 2; i++) {
            DWORD old_protect;
            LPVOID at = (LPVOID)((uintptr_t)off + s_hc[i].base);
            if (!VirtualProtect(at, XBOX_OHCI_SIZE, PAGE_NOACCESS,
                                &old_protect)) {
                s_enabled = 0;
                fprintf(stderr, "  OHCI: cannot trap 0x%08X (error %lu); "
                                "disabled\n", s_hc[i].base,
                                (unsigned long)GetLastError());
                return;
            }
        }
    }

    fprintf(stderr, "  OHCI: two controllers at 0x%08X and 0x%08X, "
                    "%d ports each, one device on HC0 port 1\n",
            XBOX_OHCI0_BASE, XBOX_OHCI1_BASE, OHCI_PORTS);
    fflush(stderr);

    {
        HANDLE th = CreateThread(NULL, 0, ohci_thread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
    }
}

static OhciController *hc_for(uint32_t va)
{
    int i;

    if (!s_enabled)
        return NULL;
    for (i = 0; i < 2; i++)
        if (va >= s_hc[i].base && va < s_hc[i].base + XBOX_OHCI_SIZE)
            return &s_hc[i];
    return NULL;
}

int xbox_OhciOwnsAddress(uint32_t xbox_va)
{
    return hc_for(xbox_va) != NULL;
}

#if defined(OHCI_TRAP_POSIX) || defined(OHCI_TRAP_WIN32)

static uint64_t ohci_read_split(void *dev, uint32_t off, int size)
{
    if (size == 8)
        return ohci_read(dev, off, 4) | (ohci_read(dev, off + 4, 4) << 32);
    return ohci_read(dev, off, size);
}

static void ohci_write_split(void *dev, uint32_t off, uint64_t val, int size)
{
    if (size == 8) {
        ohci_write(dev, off, (uint32_t)val, 4);
        ohci_write(dev, off + 4, val >> 32, 4);
        return;
    }
    ohci_write(dev, off, val, size);
}
#endif

int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va)
{
#if defined(OHCI_TRAP_POSIX)
    ucontext_t *uc = (ucontext_t *)ctx;
    OhciController *hc = hc_for(xbox_va);
    int ok;

    if (!hc)
        return 0;
#if defined(__aarch64__)
    ok = mmio_emulate_a64(uc, xbox_va - hc->base, hc,
                          ohci_read_split, ohci_write_split);
#else
    ok = mmio_emulate_x86_64(uc, xbox_va - hc->base, hc,
                             ohci_read_split, ohci_write_split);
#endif

    if (!ok && hc->decode_fail++ < 20) {
        fprintf(stderr, "  [OHCI%d] undecoded access at +0x%03X: insn %08X\n",
                hc->index, xbox_va - hc->base,
                *(const uint32_t *)(uintptr_t)MMIO_CTX_PC(uc));
        fflush(stderr);
    }
    return ok;
#elif defined(OHCI_TRAP_WIN32)
    OhciController *hc = hc_for(xbox_va);
    int ok;

    if (!hc)
        return 0;
    ok = mmio_emulate((PCONTEXT)ctx, xbox_va - hc->base, hc,
                      ohci_read_split, ohci_write_split);
    if (!ok && hc->decode_fail++ < 20) {
        const uint8_t *ip = (const uint8_t *)((PCONTEXT)ctx)->Rip;
        fprintf(stderr, "  [OHCI%d] undecoded access at +0x%03X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                hc->index, xbox_va - hc->base,
                ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
#else
    (void)ctx; (void)xbox_va;
    return 0;
#endif
}

void xbox_OhciReport(void)
{
    int i;

    if (!s_enabled)
        return;
    for (i = 0; i < 2; i++)
        fprintf(stderr, "  [OHCI%d] %u reads, %u writes, %u undecoded; "
                        "HcControl=%08X HcIntStatus=%08X port1=%08X\n",
                i, s_hc[i].reads, s_hc[i].writes, s_hc[i].decode_fail,
                s_hc[i].reg[HcControl / 4],
                s_hc[i].reg[HcInterruptStatus / 4],
                s_hc[i].reg[HcRhPortStatus1 / 4]);
    fflush(stderr);
}

