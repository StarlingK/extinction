#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include <stdio.h>

#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)
#define NV2A_PVIDEO_MMIO 0x8000u

static ptrdiff_t g_mem_offset = 0;
static uint8_t *g_nv2a_vram = NULL;

static int g_mmio_read_count = 0;
static int g_mmio_write_count = 0;
static int g_mmio_decode_fail = 0;

#if defined(_WIN32)

static SRWLOCK g_pvideo_lock = SRWLOCK_INIT;

bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{
    NV2AState *state = nv2a_get_state();
    unsigned i;
    if (!state) return false;
    AcquireSRWLockShared(&g_pvideo_lock);
    for (i = 0; i < 0x1000 / 4; ++i)
        regs[i] = state->pvideo.regs[i * 4];
    ReleaseSRWLockShared(&g_pvideo_lock);
    return true;
}

void nv2a_hook_pvideo_consume(unsigned bank)
{
    NV2AState *state = nv2a_get_state();
    uint64_t buffer;
    if (!state || bank > 1) return;
    AcquireSRWLockExclusive(&g_pvideo_lock);
    buffer = nv2a_mmio_read(state, NV2A_PVIDEO_MMIO + NV_PVIDEO_BUFFER, 4);
    nv2a_mmio_write(state, NV2A_PVIDEO_MMIO + NV_PVIDEO_BUFFER,
                    buffer & ~(uint64_t)(1u << (bank * 4)), 4);
    ReleaseSRWLockExclusive(&g_pvideo_lock);
}

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) {

        return len;
    }

    if ((rm & 7) == 4) {
        len++;
    }

    if (mod == 0) {
        if ((rm & 7) == 5) len += 4;
    } else if (mod == 1) {
        len += 1;
    } else if (mod == 2) {
        len += 4;
    }

    return len;
}

static bool decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a) return false;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0;
    int has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) {
            has_66 = 1;
            prefix_len++;
        } else if (b == 0xF2 || b == 0xF3) {

            prefix_len++;
        } else if (b >= 0x40 && b <= 0x4F) {

            rex = b;
            has_rex = 1;
            prefix_len++;
        } else {
            break;
        }
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);

        if (access_size == 1) val &= 0xFF;
        else if (access_size == 2) val &= 0xFFFF;
        else if (access_size == 4) val &= 0xFFFFFFFF;

        nv2a_mmio_write(nv2a, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, access_size);

        uint64_t *dest = ctx_reg64(ctx, reg);
        if (access_size == 1) {
            *dest = (*dest & ~0xFFULL) | (val & 0xFF);
        } else if (access_size == 2) {
            *dest = (*dest & ~0xFFFFULL) | (val & 0xFFFF);
        } else if (access_size == 4) {
            *dest = val & 0xFFFFFFFF;
        } else {
            *dest = val;
        }

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);

        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        uint32_t imm = *(const uint32_t *)imm_ptr;
        int imm_len = (rex_w ? 4 : 4);

        nv2a_mmio_write(nv2a, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);

        nv2a_mmio_write(nv2a, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 1) & 0xFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val;

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 2) & 0xFFFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val;

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size == 1) { mem_val &= 0xFF; reg_val &= 0xFF; }
        else if (access_size == 2) { mem_val &= 0xFFFF; reg_val &= 0xFFFF; }
        else if (access_size == 4) { mem_val &= 0xFFFFFFFF; reg_val &= 0xFFFFFFFF; }

        uint64_t result = mem_val & reg_val;

        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (mem_val < reg_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x3B || opcode[0] == 0x3A) {
        if (opcode[0] == 0x3A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        uint64_t result = reg_val - mem_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (reg_val < mem_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val | reg_val;

        nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;

        nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    {
        uint8_t op = opcode[0];
        int kind = -1;
        int imm_len = 0, to_reg = 0, is_byte = 0;
        uint64_t rhs = 0;
        int modrm_len;

        switch (op) {
        case 0x02: case 0x03: kind = 0; to_reg = 1; is_byte = op == 0x02; break;
        case 0x0A: case 0x0B: kind = 1; to_reg = 1; is_byte = op == 0x0A; break;
        case 0x22: case 0x23: kind = 4; to_reg = 1; is_byte = op == 0x22; break;
        case 0x2A: case 0x2B: kind = 5; to_reg = 1; is_byte = op == 0x2A; break;
        case 0x32: case 0x33: kind = 6; to_reg = 1; is_byte = op == 0x32; break;
        case 0x80: kind = (opcode[1] >> 3) & 7; imm_len = 1; is_byte = 1; break;
        case 0x83: kind = (opcode[1] >> 3) & 7; imm_len = 1; break;
        case 0x81: kind = (opcode[1] >> 3) & 7; imm_len = has_66 ? 2 : 4; break;
        case 0xF6: if (((opcode[1] >> 3) & 7) == 0) { kind = 8; imm_len = 1; is_byte = 1; } break;
        case 0xF7: if (((opcode[1] >> 3) & 7) == 0) { kind = 8; imm_len = has_66 ? 2 : 4; } break;
        default: break;
        }
        if (kind == 0 || kind == 1 || (kind >= 4 && kind <= 8)) {
            uint64_t mem_val, lhs, result, mask;
            int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
            uint64_t *dest = ctx_reg64(ctx, reg);

            if (is_byte) access_size = 1;
            mask = access_size == 8 ? ~0ULL : (1ULL << (access_size * 8)) - 1;
            modrm_len = decode_modrm_len(opcode + 1, rex_b);
            mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;

            if (to_reg) {
                lhs = *dest & mask;
                rhs = mem_val;
            } else {
                const uint8_t *imm = opcode + 1 + modrm_len;
                lhs = mem_val;
                if (imm_len == 1)      rhs = (uint64_t)(int64_t)*(const int8_t *)imm;
                else if (imm_len == 2) rhs = (uint64_t)(int64_t)*(const int16_t *)imm;
                else                   rhs = (uint64_t)(int64_t)*(const int32_t *)imm;
                rhs &= mask;
            }
            switch (kind) {
            case 0:  result = lhs + rhs; break;
            case 1:  result = lhs | rhs; break;
            case 5:
            case 7:  result = lhs - rhs; break;
            case 6:  result = lhs ^ rhs; break;
            default: result = lhs & rhs; break;
            }
            result &= mask;

            ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
            if (result == 0) ctx->EFlags |= 0x0040;
            if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
            if ((kind == 5 || kind == 7) && lhs < rhs) ctx->EFlags |= 0x0001;
            if (kind == 0 && result < lhs) ctx->EFlags |= 0x0001;

            if (to_reg) {
                if (access_size == 4)      *dest = result;
                else if (access_size == 8) *dest = result;
                else *dest = (*dest & ~mask) | result;
                g_mmio_read_count++;
            } else if (kind != 7 && kind != 8) {
                nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
                g_mmio_write_count++;
            } else {
                g_mmio_read_count++;
            }
            ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
            return true;
        }
    }

    g_mmio_decode_fail++;
    if (g_mmio_decode_fail <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{
    g_mem_offset = xbox_mem_offset;

    g_nv2a_vram = (uint8_t *)VirtualAlloc(NULL, NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_nv2a_vram) {
        fprintf(stderr, "[NV2A] Failed to allocate %u MB for VRAM!\n",
                (NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE) / (1024*1024));
        return;
    }

    uint8_t *ramin_ptr = g_nv2a_vram + NV2A_VRAM_SIZE;

    nv2a_init_standalone(g_nv2a_vram, NV2A_VRAM_SIZE,
                         ramin_ptr, NV2A_RAMIN_SIZE);

    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void*)g_nv2a_vram, (void*)ramin_ptr);

    {
        DWORD old_protect;
        void *page = (void *)((uintptr_t)(NV2A_MMIO_BASE + NV2A_PVIDEO_MMIO) +
                              g_mem_offset);
        if (!VirtualProtect(page, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "[NV2A] PVIDEO registers not trapped (error %lu); "
                    "the video overlay will not show\n", GetLastError());
    }
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{

    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;
    bool pvideo = mmio_offset >= NV2A_PVIDEO_MMIO &&
                  mmio_offset < NV2A_PVIDEO_MMIO + 0x1000u;
    bool handled;

    if (pvideo) AcquireSRWLockExclusive(&g_pvideo_lock);
    handled = decode_and_handle(ctx, mmio_offset, is_write);
    if (pvideo) ReleaseSRWLockExclusive(&g_pvideo_lock);
    return handled;
}

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{

    uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF;
    LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!result) {
        result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                              MEM_COMMIT, PAGE_READWRITE);
    }
    if (result) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
}

#else

bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{ (void)regs; return false; }
void nv2a_hook_pvideo_consume(unsigned bank) { (void)bank; }

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{ (void)xbox_mem_offset; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }

#endif

