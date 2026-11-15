#ifndef AVP_COOP_PROTO_H
#define AVP_COOP_PROTO_H

#include <stddef.h>
#include <stdint.h>

extern ptrdiff_t g_xbox_mem_offset;

#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GF(a)  (*(volatile float *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define G8(a)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define CP_OBJ_TABLE   0x00289E40u
#define CP_WORLD       0x002F6AE4u
#define CP_CLOCK       0x002F689Cu
#define CP_RNG_ACC     0x002CC280u
#define CP_RNG_SEED    0x002CC284u
#define CP_TECH_TREE   0x002F6950u
#define CP_MSG_TYPES   0x002891F8u
#define CP_NOWHERE     0x00304EDCu
#define CP_LOCAL_LO    0x03082000u
#define CP_LOCAL_HI    0x04000000u
#define CP_PM_ARRAY    0x74u
#define CP_PM_SLOTS    8u
#define CP_MINIMAP     0x002F6990u
#define CP_POINTER_G   0x002F6AECu

#define CF_AVP   1u
#define CF_UNIT  2u
#define CF_PM    4u
#define CF_TRIG  8u
#define CF_SQUAD 16u
#define CF_STATIC 32u
#define CF_HELPER 64u
#define CF_NOREPLAY 128u

#define CP_FIELDS_MAX   64u
#define CP_STATE_MAGIC  0x54415453u
#define CP_HDR          48u
#define CP_STATE_MAX    (2u << 20)
#define CP_OBJ_MAX      8192u
#define CP_TECH_MAX     256u
#define CP_SEL_MAX      64u
#define CP_MSG_MAX      64u
#define CP_ORDER_WIRE_MAX (64u + CP_SEL_MAX * 4u + CP_MSG_MAX)
#define CP_SNAP_DIST    8.0f
#define CP_MATCH_DIST   64.0f
#define CP_FLAG_FULL    1u
#define CP_FLAG_TRUNC   2u

enum { CP_CMD_ORDER = 1, CP_CMD_SELECT = 3, CP_CMD_MOVE = 4, CP_CMD_PCALL = 5, CP_CMD_BUY = 7, CP_CMD_REINF = 8, CP_CMD_GIVE = 9, CP_CMD_UPG = 10, CP_CMD_TECH = 11, CP_CMD_TECH1 = 12, CP_CMD_TECH2 = 13 };

typedef struct CpHooks {
    int (*owner)(uint32_t handle);
    void (*owner_set)(uint32_t handle, int owner);
    void (*kill)(uint32_t handle);
    void (*unmatched)(uint32_t vt, uint32_t handle, const float *pos);
    void (*setpos)(uint32_t obj, const float *v);
    void (*setrot)(uint32_t obj, const float *v);
} CpHooks;

typedef struct CpBuildStats {
    uint32_t objects, records, words, pm, tech, truncated;
} CpBuildStats;

typedef struct CpApplyStats {
    uint32_t records, exact, nearby, unmatched, orphans, kills, pm, tech, bad, skipped;
} CpApplyStats;

typedef struct CpOrder {
    uint32_t cmd;
    uint32_t n;
    uint32_t h[CP_SEL_MAX];
    uint32_t a, b, c, d;
    float f[6];
    uint8_t msg[CP_MSG_MAX];
    uint32_t player;
} CpOrder;

extern uint32_t g_cp_orphan_need;
extern int g_cp_kill_orphan;
uint32_t cp_class(uint32_t vt);
int cp_classes_related(uint32_t vt_a, uint32_t vt_b);
int cp_obj_ok(uint32_t obj);
uint32_t cp_obj_of(uint32_t handle);
uint32_t cp_pm_object(uint32_t slot);
void cp_reset(void);

uint32_t cp_state_build(uint8_t *buf, uint32_t cap, uint32_t seq, int full, uint32_t host_ms, const CpHooks *hk,
                        CpBuildStats *bs);
int cp_state_validate(const uint8_t *p, uint32_t len);
void cp_state_apply(const uint8_t *p, uint32_t len, const CpHooks *hk, CpApplyStats *st, uint32_t local_ms);
void cp_smooth_step(uint32_t now_ms, uint32_t dt_ms, const CpHooks *hk);
uint32_t cp_map_to_client(uint32_t host_handle);
uint32_t cp_map_to_host(uint32_t client_handle);
uint32_t cp_state_clock(const uint8_t *p, uint32_t len);

int cp_order_parse(const uint8_t *p, uint32_t len, CpOrder *o);
uint32_t cp_enc_select(uint8_t *out, const uint32_t *h, uint32_t n);
uint32_t cp_enc_move(uint8_t *out, uint32_t mtype, uint32_t nowhere, const float *pos, uint32_t queue,
                     uint32_t particle, uint32_t under, const float *world);
uint32_t cp_enc_pcall(uint8_t *out, uint32_t idx, uint32_t param, uint32_t under, const float *world);
uint32_t cp_enc_order(uint8_t *out, uint32_t unit, uint32_t type, uint32_t prio, const uint8_t *msg, uint32_t n);
uint32_t cp_enc_give(uint8_t *out, const uint32_t *h, uint32_t n);
uint32_t cp_enc_reinf(uint8_t *out, uint32_t cost, uint32_t supply, uint32_t drop, const uint32_t *types);
uint32_t cp_enc_buy(uint8_t *out, uint32_t idx);
uint32_t cp_enc_upg(uint8_t *out, uint32_t idx);
extern void (*g_cp_tech_flip)(uint32_t id);
uint32_t cp_enc_tech2(uint8_t *out, uint32_t fn, uint32_t handle, uint32_t vt, uint32_t id);
uint32_t cp_enc_tech1(uint8_t *out, uint32_t id, uint32_t handle, uint32_t vt);
uint32_t cp_enc_tech(uint8_t *out, uint32_t vt, const uint32_t *ids, uint32_t n);

int cp_order_type_ok(uint32_t type);
int cp_order_xlate_in(uint32_t type, uint8_t *msg, uint32_t size);
int cp_order_xlate_out(uint32_t type, uint8_t *msg, uint32_t size);
uint32_t cp_order_msg_build(const CpOrder *o, uint8_t *out, uint32_t cap);
int cp_target_ok(uint32_t handle);
int cp_unit_ok(uint32_t handle);

uint32_t cp_pcall_count(void);
uint32_t cp_pcall_fn(uint32_t idx);
int cp_pcall_find(uint32_t fn);
int cp_pcall_this_reg(uint32_t idx);
uint32_t cp_pcall_nstack(uint32_t idx);
int cp_pcall_param_ok(uint32_t idx, uint32_t param);
uint32_t cp_particle_off(uint32_t idx);
int cp_particle_index(uint32_t off);

#define CP_SPAWN_MSG15   1u
#define CP_SPAWN_FACT    2u
#define CP_SPAWN_CKEY    3u
#define CP_SPAWN_WORDS   16u
#define CP_SPAWN_FILL    256u
#define CP_SPAWN_HDR     28u
#define CP_SPAWN_WIRE_MAX (CP_SPAWN_HDR + 4u * CP_SPAWN_WORDS + CP_SPAWN_FILL)

typedef struct CpSpawn {
    uint32_t kind, host_h, host_vt, n, fill_ptr, fill_len, fill_hash;
    uint32_t w[CP_SPAWN_WORDS];
    uint8_t fill[CP_SPAWN_FILL];
} CpSpawn;

uint32_t cp_fnv32(const void *p, uint32_t n);
int cp_class_replicable(uint32_t vt);
uint32_t cp_enc_spawn(uint8_t *out, const CpSpawn *s);
int cp_spawn_parse(const uint8_t *p, uint32_t len, CpSpawn *s);
void cp_map_bind(uint32_t host_handle, uint32_t client_handle);
void cp_map_unbind(uint32_t host_handle);

uint32_t cp_read_selection(uint32_t ptr, uint32_t *out, uint32_t max);

#endif
