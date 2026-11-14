#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include "avp_platform.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "avp_skirmish.h"

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax);
extern uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n);
extern uint32_t avp_guest_alloca(uint32_t bytes);
extern uint32_t avp_guest_esp(void);
extern void avp_guest_set_esp(uint32_t v);
extern int avp_coop_role(void);

#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GF(a)  (*(volatile float *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define G8(a)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define AVP_OBJ_TABLE    0x00289E40u
#define AVP_WORLD_OBJ    0x002F6AF0u
#define AVP_FIND_FILL    0x001368C0u
#define AVP_NEXT_HANDLE  0x000D6FE0u
#define AVP_START_DEATH  0x0003C400u
#define AVP_DYNCAST      0x001474DAu
#define AVP_TD_GOBJECT   0x0027E09Cu
#define AVP_TD_GUNIT     0x002821F4u
#define UNIT_PLAYER      0x474u
#define VS_FILTER        0x38u
#define VS_RECEIVE       0x3Cu
#define VS_UNIT_ISDEAD   55u
#define MSG_BYTES        0x90u
#define MSG_SPAWN_TYPE   0x15u
#define MSG_PRIORITY     0x0Au
#define RING_RADIUS      3.0f
#define MAX_FORCE_LINES  32
#define MAX_FORCE_COUNT  32
#define TICK_PERIOD      60
#define PI_F             3.14159265f

typedef struct {
    AvpRace race;
    char cls[48];
    int count;
} ForceLine;

static AvpSkirmishCfg s_cfg;
static int s_cfg_loaded;
static int s_editor;
static int s_running;
static int s_armed;
static int s_arm_editor;
static int s_env_done;
static char s_cur_map[4];
static int s_started;
static int s_tick_count;
static int s_seen[AVP_SKIRMISH_MAX_PLAYERS];
static int s_banner;
static int s_banner_logged;

static int g_ptr_ok(uint32_t p)
{
    return p >= 0x10000u && p < 0x84000000u - 0x1000u;
}

static time_t s_spawn_t;

static void reset_state(void)
{
    s_started = 0;
    s_banner = 0;
    s_banner_logged = 0;
    s_tick_count = 0;
    memset(s_seen, 0, sizeof s_seen);
}

static const char *kind_name(AvpPlayerKind k)
{
    return k == AVP_PK_AI ? "ai" : k == AVP_PK_REMOTE ? "remote" : "human";
}

static int kind_parse(const char *s, AvpPlayerKind *out)
{
    if (!strcmp(s, "human")) *out = AVP_PK_HUMAN;
    else if (!strcmp(s, "ai")) *out = AVP_PK_AI;
    else if (!strcmp(s, "remote")) *out = AVP_PK_REMOTE;
    else return 0;
    return 1;
}

static int to_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v;
    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 10);
    if (*end || v < lo || v > hi)
        return 0;
    *out = (int)v;
    return 1;
}

int avp_skirmish_config_load(AvpSkirmishCfg *c)
{
    FILE *f = fopen("skirmish.cfg", "rb");
    char line[256];
    int errors = 0;
    memset(c, 0, sizeof *c);
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *tok[8], *p = line;
        int n = 0;
        while (*p && n < 8) {
            while (*p && isspace((unsigned char)*p))
                p++;
            if (!*p)
                break;
            tok[n++] = p;
            while (*p && !isspace((unsigned char)*p))
                p++;
            if (*p)
                *p++ = 0;
        }
        if (!n || tok[0][0] == '#')
            continue;
        if (!strcmp(tok[0], "map") && n == 2 && strlen(tok[1]) < sizeof c->map) {
            size_t i;
            for (i = 0; tok[1][i]; i++)
                c->map[i] = (char)toupper((unsigned char)tok[1][i]);
            c->map[i] = 0;
        } else if (!strcmp(tok[0], "player") && n == 6 && c->nplayers < AVP_SKIRMISH_MAX_PLAYERS) {
            AvpSkirmishPlayer pl;
            int i, dup = 0;
            if (!to_int(tok[1], 1, 8, &pl.slot) || !kind_parse(tok[2], &pl.kind)
                || !avp_race_parse(tok[3], &pl.race) || !to_int(tok[4], 1, 3, &pl.spawn)
                || !to_int(tok[5], 0, 99, &pl.team)) {
                errors++;
                continue;
            }
            for (i = 0; i < c->nplayers; i++)
                dup |= c->player[i].slot == pl.slot;
            if (dup) {
                errors++;
                continue;
            }
            c->player[c->nplayers++] = pl;
        } else {
            errors++;
        }
    }
    fclose(f);
    if (errors)
        fprintf(stderr, "[SKIRMISH] skirmish.cfg: %d bad lines\n", errors);
    return 1;
}

int avp_skirmish_config_save(const AvpSkirmishCfg *c)
{
    FILE *f = fopen("skirmish.cfg", "wb");
    int i;
    if (!f)
        return 0;
    fprintf(f, "map %s\n", c->map);
    for (i = 0; i < c->nplayers && i < AVP_SKIRMISH_MAX_PLAYERS; i++) {
        const AvpSkirmishPlayer *p = &c->player[i];
        fprintf(f, "player %d %s %s %d %d\n", p->slot, kind_name(p->kind), avp_race_name(p->race),
                p->spawn, p->team);
    }
    if (fclose(f) != 0)
        return 0;
    s_cfg = *c;
    s_cfg_loaded = 1;
    return 1;
}

static void cfg_ensure(void)
{
    if (!s_cfg_loaded)
        s_cfg_loaded = avp_skirmish_config_load(&s_cfg);
}

static int env_on(void)
{
    const char *e = getenv("AVP_SKIRMISH");
    return e && *e && strcmp(e, "0") != 0;
}

static void init_once(void)
{
    if (s_env_done)
        return;
    s_env_done = 1;
    if (env_on()) {
        s_armed = 1;
        s_arm_editor = 0;
    }
}

void avp_skirmish_arm(int editor)
{
    init_once();
    cfg_ensure();
    s_arm_editor = editor != 0;
    s_armed = 1;
    fprintf(stderr, "[SKIRMISH] armed%s\n", editor ? " (editor)" : "");
}

void avp_skirmish_disarm(void)
{
    init_once();
    if (s_armed)
        fprintf(stderr, "[SKIRMISH] disarmed\n");
    s_armed = 0;
}

int avp_skirmish_armed(void)
{
    init_once();
    return s_armed;
}

int avp_skirmish_active(void)
{
    return s_running && !s_editor;
}

int avp_skirmish_editor(void)
{
    return s_running && s_editor;
}

const char *avp_skirmish_cur_map(void)
{
    return s_cur_map;
}

int avp_skirmish_block(uint32_t fn)
{
    static uint32_t fns[16];
    static int counts[16];
    int i, slot = -1;
    if (!s_running)
        return 0;
    for (i = 0; i < 16; i++) {
        if (fns[i] == fn) { slot = i; break; }
        if (!fns[i]) { fns[i] = fn; slot = i; break; }
    }
    if (slot >= 0 && counts[slot] < 3) {
        counts[slot]++;
        fprintf(stderr, "[SKIRMISH] blocked %08X (%d)\n", fn, counts[slot]);
    }
    return 1;
}

static void gstr(uint32_t va, char *buf, size_t n)
{
    size_t k = 0;
    if (g_ptr_ok(va))
        for (; k + 1 < n && k < 128 && G8(va + k) >= 0x20 && G8(va + k) < 0x7F; k++)
            buf[k] = (char)G8(va + k);
    buf[k] = 0;
}

static int is_boundary(uint32_t va, int before)
{
    uint8_t c = before ? G8(va - 1) : G8(va);
    return !isalnum(c);
}

static int find_code(uint32_t name, const char *before)
{
    int i, n = (int)strlen(before);
    for (i = 0; i + 3 <= n; i++) {
        char c = (char)toupper((unsigned char)before[i]);
        if ((c != 'A' && c != 'M' && c != 'P' && c != 'T') || !isdigit((unsigned char)before[i + 1])
            || !isdigit((unsigned char)before[i + 2]))
            continue;
        if (i > 0 && !is_boundary(name + (uint32_t)i, 1))
            continue;
        if (i + 3 < n && !is_boundary(name + (uint32_t)i + 3, 0))
            continue;
        if (before[i + 3] == 0 && G8(name + (uint32_t)i + 3) != 0)
            continue;
        return i;
    }
    return -1;
}

void avp_skirmish_level(uint32_t name)
{
    static int logged;
    char before[132], after[132];
    int i, k;
    init_once();
    gstr(name, before, sizeof before);
    if (logged < 10) {
        logged++;
        fprintf(stderr, "[SKIRMISH] level name %08X \"%s\"\n", name, before);
    }
    i = g_ptr_ok(name) ? find_code(name, before) : -1;
    if (i < 0) {
        s_running = 0;
        s_editor = 0;
        reset_state();
        s_cur_map[0] = 0;
        fprintf(stderr, "[SKIRMISH] level name has no map code, unchanged\n");
        return;
    }
    if (s_armed) {
        cfg_ensure();
        s_armed = 0;
        if (s_cfg_loaded && strlen(s_cfg.map) == 3) {
            G8(name + (uint32_t)i) = (uint8_t)s_cfg.map[0];
            G8(name + (uint32_t)i + 1) = (uint8_t)s_cfg.map[1];
            G8(name + (uint32_t)i + 2) = (uint8_t)s_cfg.map[2];
            gstr(name, after, sizeof after);
            fprintf(stderr, "[SKIRMISH] level swap \"%s\" -> \"%s\"\n", before, after);
            s_running = 1;
            s_editor = s_arm_editor;
            memcpy(s_cur_map, s_cfg.map, 3);
            s_cur_map[3] = 0;
            return;
        }
    }
    s_running = 0;
    s_editor = 0;
    reset_state();
    for (k = 0; k < 3; k++)
        s_cur_map[k] = (char)toupper((unsigned char)before[i + k]);
    s_cur_map[3] = 0;
}

void avp_skirmish_fill_trace(uint32_t name)
{
    static char seen[256][64];
    static int nseen, lines, enabled = -1;
    char buf[132];
    int i;
    if (enabled < 0) {
        const char *e = getenv("AVP_FILL_TRACE");
        enabled = e && *e && strcmp(e, "0") != 0;
    }
    if (!enabled || lines >= 2000)
        return;
    gstr(name, buf, sizeof buf);
    for (i = 0; i < nseen; i++)
        if (!strncmp(seen[i], buf, sizeof seen[i] - 1))
            return;
    if (nseen < 256) {
        strncpy(seen[nseen], buf, sizeof seen[nseen] - 1);
        nseen++;
    }
    lines++;
    fprintf(stderr, "[SKIRMISH] fill %s\n", buf);
}

static int handle_live(uint32_t h, uint32_t *objp)
{
    uint32_t obj;
    if (!(h >> 16))
        return 0;
    obj = G32(AVP_OBJ_TABLE + (h >> 16) * 4u);
    if (!g_ptr_ok(obj) || G32(obj + 0x18) != h)
        return 0;
    if (objp)
        *objp = obj;
    return 1;
}

uint32_t avp_skirmish_spawn(const char *cls, int slot, float x, float y, float z, float rot)
{
    uint32_t esp = avp_guest_esp(), base, name, out, msg, fill, handle, obj, vt, a[1], regs[6];
    uint32_t ret = 0, fn, accepted = 0, objout;
    size_t len;
    uint32_t i;
    float f;
    base = avp_guest_alloca(0x200);
    name = base;
    out = base + 0x80;
    msg = base + 0x100;
    len = strlen(cls);
    if (len > 60) {
        fprintf(stderr, "[SKIRMISH] spawn: class name too long\n");
        avp_guest_set_esp(esp);
        return 0;
    }
    G8(name) = '_';
    for (i = 0; i < len; i++)
        G8(name + 1 + i) = (uint8_t)cls[i];
    {
        static const char suf[] = "_DefaultParams";
        for (i = 0; i < sizeof suf; i++)
            G8(name + 1 + len + i) = (uint8_t)suf[i];
    }
    G32(out) = 0;
    for (i = 0; i < MSG_BYTES; i += 4)
        G32(msg + i) = 0;
    regs[0] = 0; regs[1] = 0; regs[2] = 0; regs[3] = out; regs[4] = name; regs[5] = 0;
    a[0] = 0;
    avp_gcall_regs(AVP_FIND_FILL, regs, a, 1);
    fill = G32(out);
    fprintf(stderr, "[SKIRMISH] spawn %s slot %d: fill _%s_DefaultParams = %08X\n", cls, slot, cls, fill);
    if (!fill) {
        avp_guest_set_esp(esp);
        return 0;
    }
    G32(out) = 0;
    regs[0] = out; regs[1] = 0; regs[2] = (uint32_t)slot; regs[3] = 0; regs[4] = 0; regs[5] = 0;
    avp_gcall_regs(AVP_NEXT_HANDLE, regs, NULL, 0);
    handle = G32(out);
    fprintf(stderr, "[SKIRMISH] spawn %s: handle %08X\n", cls, handle);
    obj = G32(AVP_WORLD_OBJ);
    if (!handle || !g_ptr_ok(obj)) {
        fprintf(stderr, "[SKIRMISH] spawn %s: no handle or world object %08X\n", cls, obj);
        avp_guest_set_esp(esp);
        return 0;
    }
    G32(msg) = MSG_SPAWN_TYPE;
    G32(msg + 4) = G32(obj + 0x18);
    G8(msg + 8) = MSG_PRIORITY;
    G32(msg + 0xC) = fill;
    G32(msg + 0x14) = handle;
    f = x; memcpy(&i, &f, 4); G32(msg + 0x18) = i;
    f = y; memcpy(&i, &f, 4); G32(msg + 0x1C) = i;
    f = z; memcpy(&i, &f, 4); G32(msg + 0x20) = i;
    GF(msg + 0x24) = 0.0f;
    GF(msg + 0x28) = 0.0f;
    f = rot; memcpy(&i, &f, 4); G32(msg + 0x2C) = i;
    vt = G32(obj);
    a[0] = msg;
    regs[0] = 0; regs[1] = obj; regs[2] = 0; regs[3] = 0; regs[4] = obj; regs[5] = 0;
    fn = G32(vt + VS_FILTER);
    accepted = avp_gcall_regs(fn, regs, a, 1) & 0xFFu;
    fprintf(stderr, "[SKIRMISH] spawn %s: filter %08X -> %u\n", cls, fn, accepted);
    if (accepted) {
        fn = G32(vt + VS_RECEIVE);
        regs[0] = 0; regs[1] = obj; regs[2] = 0; regs[3] = 0; regs[4] = obj; regs[5] = 0;
        avp_gcall_regs(fn, regs, a, 1);
        fprintf(stderr, "[SKIRMISH] spawn %s: receive %08X done\n", cls, fn);
    }
    if (handle_live(handle, &objout)) {
        ret = handle;
        fprintf(stderr, "[SKIRMISH] spawn %s: live object %08X at %08X\n", cls, handle, objout);
    } else {
        fprintf(stderr, "[SKIRMISH] spawn %s: handle %08X not in object table\n", cls, handle);
    }
    avp_guest_set_esp(esp);
    return ret;
}

static int unit_player(uint32_t obj)
{
    uint32_t a[5];
    uint32_t vt = G32(obj);
    if (vt < 0x10000u || vt >= 0x0031B000u)
        return 0;
    a[0] = obj; a[1] = 0; a[2] = AVP_TD_GOBJECT; a[3] = AVP_TD_GUNIT; a[4] = 0;
    if (!avp_gcall(AVP_DYNCAST, 0, a, 5, 0))
        return 0;
    return (int)G32(obj + UNIT_PLAYER);
}

static int unit_dead(uint32_t obj)
{
    uint32_t vt = G32(obj), regs[6];
    regs[0] = G32(obj + 0x18); regs[1] = obj; regs[2] = vt; regs[3] = 0; regs[4] = obj; regs[5] = 0;
    return (avp_gcall_regs(G32(vt + VS_UNIT_ISDEAD * 4u), regs, NULL, 0) & 0xFFu) != 0;
}

static int slot_cfg_index(int slot)
{
    int i;
    for (i = 0; i < s_cfg.nplayers; i++)
        if (s_cfg.player[i].slot == slot)
            return i;
    return -1;
}

static int each_unit(uint32_t *cursor, uint32_t *obj, int *player)
{
    uint32_t i;
    for (i = *cursor; i < 0x10000u; i++) {
        uint32_t o = G32(AVP_OBJ_TABLE + i * 4u);
        int p;
        if (!g_ptr_ok(o) || (G32(o + 0x18) >> 16) != i)
            continue;
        p = unit_player(o);
        if (p < 1 || p > 9)
            continue;
        *cursor = i + 1;
        *obj = o;
        *player = p;
        return 1;
    }
    *cursor = 0x10000u;
    return 0;
}

static int load_forces(ForceLine *out, int max)
{
    static const ForceLine defs[] = {
        { AVP_RACE_MARINE, "GMarineCommTech", 1 }, { AVP_RACE_MARINE, "GMarineInfantry", 4 },
        { AVP_RACE_ALIEN, "GAlienPraetorian", 1 }, { AVP_RACE_ALIEN, "GAlienDrone", 4 },
        { AVP_RACE_PREDATOR, "GPredatorWarrior", 2 }, { AVP_RACE_PREDATOR, "GPredatorBlooded", 1 }
    };
    int n = 0, r, i, have[AVP_RACE_COUNT] = { 0, 0, 0 };
    FILE *f = fopen("skirmish_forces.txt", "rb");
    char line[160];
    if (f) {
        while (fgets(line, sizeof line, f) && n < max) {
            char rs[32], cls[48];
            int cnt;
            AvpRace race;
            if (line[0] == '#' || sscanf(line, "%31s %47s %d", rs, cls, &cnt) != 3)
                continue;
            if (!avp_race_parse(rs, &race) || cnt < 1)
                continue;
            out[n].race = race;
            strcpy(out[n].cls, cls);
            out[n].count = cnt > MAX_FORCE_COUNT ? MAX_FORCE_COUNT : cnt;
            have[race] = 1;
            n++;
        }
        fclose(f);
    }
    for (r = 0; r < AVP_RACE_COUNT; r++) {
        if (have[r])
            continue;
        for (i = 0; i < (int)(sizeof defs / sizeof defs[0]); i++)
            if (defs[i].race == (AvpRace)r && n < max)
                out[n++] = defs[i];
    }
    return n;
}

void avp_skirmish_mission_start(void)
{
    AvpLayout *lay;
    ForceLine forces[MAX_FORCE_LINES];
    int nforces, i, killed = 0, spawned = 0, failed = 0, nneutral = 0;
    uint32_t cursor = 1, obj;
    int player;
    int editing = s_editor, active = avp_skirmish_active();
    reset_state();
    if (!editing && !active)
        return;
    cfg_ensure();
    lay = malloc(sizeof *lay);
    if (!lay)
        return;
    i = avp_layout_load(lay, s_cfg.map);
    fprintf(stderr, "[SKIRMISH] mission start map %s editor %d layout %s (%d bad lines): %d units\n",
            s_cfg.map, editing, i < 0 ? "missing" : "loaded", i < 0 ? 0 : i, lay->nunits);
    if (editing || !active) {
        free(lay);
        return;
    }
    fprintf(stderr, "[SKIRMISH] clear: unit test is RTTI GUnit dyncast, player at +0x474\n");
    while (each_unit(&cursor, &obj, &player)) {
        if (slot_cfg_index(player) < 0)
            continue;
        avp_gcall(AVP_START_DEATH, 0, (uint32_t[]){ G32(obj + 0x18), 1 }, 2, G32(obj + 0x18));
        killed++;
    }
    fprintf(stderr, "[SKIRMISH] clear: %d units sent StartDeath\n", killed);
    nforces = load_forces(forces, MAX_FORCE_LINES);
    for (i = 0; i < s_cfg.nplayers; i++) {
        const AvpSkirmishPlayer *pl = &s_cfg.player[i];
        const AvpSpawn *sp = &lay->spawn[pl->race][pl->spawn - 1];
        int k, total = 0, idx = 0, mine = 0;
        for (k = 0; k < lay->nunits; k++) {
            const AvpUnitRec *u = &lay->unit[k];
            int own = (u->aff.kind == AVP_AFF_SLOT && u->aff.slot == pl->slot)
                || (u->aff.kind == AVP_AFF_SPAWN && u->aff.race == pl->race && u->aff.n == pl->spawn);
            if (!own)
                continue;
            mine++;
            if (avp_skirmish_spawn(u->cls, pl->slot, u->x, u->y, u->z, u->rot))
                spawned++;
            else
                failed++;
        }
        if (!sp->used) {
            fprintf(stderr, "[SKIRMISH] player %d: no %s spawn %d in layout, no starting force\n",
                    pl->slot, avp_race_name(pl->race), pl->spawn);
            continue;
        }
        for (k = 0; k < nforces; k++)
            if (forces[k].race == pl->race)
                total += forces[k].count;
        for (k = 0; k < nforces; k++) {
            int c;
            if (forces[k].race != pl->race)
                continue;
            for (c = 0; c < forces[k].count; c++, idx++) {
                float ang = 2.0f * PI_F * (float)idx / (float)(total ? total : 1);
                if (avp_skirmish_spawn(forces[k].cls, pl->slot, sp->x + RING_RADIUS * cosf(ang),
                                       sp->y + RING_RADIUS * sinf(ang), sp->z, sp->rot))
                    spawned++;
                else
                    failed++;
            }
        }
        fprintf(stderr, "[SKIRMISH] player %d (%s %s spawn %d team %d): %d layout units, %d force\n",
                pl->slot, kind_name(pl->kind), avp_race_name(pl->race), pl->spawn, pl->team, mine, total);
    }
    for (i = 0; i < lay->nunits; i++) {
        const AvpUnitRec *u = &lay->unit[i];
        if (u->aff.kind != AVP_AFF_NEUTRAL)
            continue;
        nneutral++;
        if (avp_skirmish_spawn(u->cls, 9, u->x, u->y, u->z, u->rot))
            spawned++;
        else
            failed++;
    }
    fprintf(stderr, "[SKIRMISH] spawn done: %d ok, %d failed (%d neutral in layout)\n", spawned, failed, nneutral);
    free(lay);
    reset_state();
    s_started = 1;
    s_spawn_t = time(NULL);
}

void avp_skirmish_tick(void)
{
    int alive[AVP_SKIRMISH_MAX_PLAYERS] = { 0 };
    int teams_alive[100] = { 0 };
    uint32_t cursor = 1, obj;
    int player, i, nteams = 0, winner = -1, human_won = 0;
    if (!s_started || s_banner || s_editor || !avp_skirmish_active())
        return;
    if (++s_tick_count < TICK_PERIOD)
        return;
    s_tick_count = 0;
    while (each_unit(&cursor, &obj, &player)) {
        int ci = slot_cfg_index(player);
        if (ci >= 0 && !unit_dead(obj))
            alive[ci]++;
    }
    for (i = 0; i < s_cfg.nplayers; i++) {
        if (alive[i] > 0 && !s_seen[i])
            s_seen[i] = 1;
        if (alive[i] > 0)
            teams_alive[s_cfg.player[i].team] = 1;
    }
    for (i = 0; i < 100; i++)
        if (teams_alive[i]) {
            nteams++;
            winner = i;
        }
    {
        int seen_teams = 0, seen_team[100] = { 0 };
        for (i = 0; i < s_cfg.nplayers; i++)
            if (s_seen[i] && !seen_team[s_cfg.player[i].team]) {
                seen_team[s_cfg.player[i].team] = 1;
                seen_teams++;
            }
        if (seen_teams < 2 || nteams > 1)
            return;
    }
    if (time(NULL) - s_spawn_t < 30)
        return;
    for (i = 0; i < s_cfg.nplayers; i++) {
        AvpPlayerKind k = s_cfg.player[i].kind;
        if (((k == AVP_PK_HUMAN && avp_coop_role() != 2) || (k == AVP_PK_REMOTE && avp_coop_role() == 2)) && !s_seen[i])
            return;
    }
    for (i = 0; i < s_cfg.nplayers; i++)
        if (s_cfg.player[i].kind == (avp_coop_role() == 2 ? AVP_PK_REMOTE : AVP_PK_HUMAN) && nteams == 1 && s_cfg.player[i].team == winner)
            human_won = 1;
    s_banner = human_won ? 1 : 2;
    if (!s_banner_logged) {
        s_banner_logged = 1;
        if (nteams == 1)
            fprintf(stderr, "[SKIRMISH] team %d wins: %s\n", winner, human_won ? "VICTORY" : "DEFEAT");
        else
            fprintf(stderr, "[SKIRMISH] no one left: DEFEAT\n");
    }
}

static const uint8_t *glyph(char c)
{
    static const uint8_t g[][7] = {
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
        { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        { 0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 },
        { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }
    };
    static const char order[] = "ABCDEFITORVY";
    const char *p = strchr(order, c);
    int k = p && c ? (int)(p - order) : -1;
    return k >= 0 ? g[k] : NULL;
}

void avp_skirmish_draw(uint32_t *rgb, int w, int h)
{
    const char *text = s_banner == 1 ? "VICTORY" : s_banner == 2 ? "DEFEAT" : NULL;
    int scale = 4, n, tw, th = 7 * scale, bw, bh, bx, by, i, x, y;
    uint32_t fg = s_banner == 1 ? 0x0060E060u : 0x00E05050u;
    if (!text || !rgb || !avp_skirmish_active())
        return;
    n = (int)strlen(text);
    tw = n * 6 * scale - scale;
    bw = tw + 8 * scale;
    bh = th + 8 * scale;
    bx = (w - bw) / 2;
    by = (h - bh) / 2;
    for (y = by; y < by + bh; y++)
        for (x = bx; x < bx + bw; x++) {
            int edge = x < bx + 2 || x >= bx + bw - 2 || y < by + 2 || y >= by + bh - 2;
            if (x >= 0 && x < w && y >= 0 && y < h)
                rgb[(size_t)y * w + x] = edge ? fg : 0x00101010u;
        }
    for (i = 0; i < n; i++) {
        const uint8_t *g = glyph(text[i]);
        int gx = bx + 4 * scale + i * 6 * scale, gy = by + 4 * scale, r, c;
        if (!g)
            continue;
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c)))
                    continue;
                for (y = 0; y < scale; y++)
                    for (x = 0; x < scale; x++) {
                        int px = gx + c * scale + x, py = gy + r * scale + y;
                        if (px >= 0 && px < w && py >= 0 && py < h)
                            rgb[(size_t)py * w + px] = fg;
                    }
            }
    }
}

static int code_ok(const char *c)
{
    return c && strlen(c) == 3 && strchr("AMPT", c[0]) && isdigit((unsigned char)c[1]) && isdigit((unsigned char)c[2]);
}

void avp_skirmish_capture(const uint32_t *rgb, int w, int h)
{
    static char done[4];
    char path[48];
    uint32_t *px;
    uint8_t head[8] = { 'A', 'V', 'P', 'T', AVP_THUMB_W & 255, AVP_THUMB_W >> 8, AVP_THUMB_H & 255, AVP_THUMB_H >> 8 };
    int tx, ty;
    FILE *f;
    if (!s_running || !rgb || w < AVP_THUMB_W || h < AVP_THUMB_H || !code_ok(s_cur_map) || !strcmp(done, s_cur_map))
        return;
    px = malloc((size_t)AVP_THUMB_W * AVP_THUMB_H * sizeof *px);
    if (!px)
        return;
    for (ty = 0; ty < AVP_THUMB_H; ty++) {
        int y0 = ty * h / AVP_THUMB_H, y1 = (ty + 1) * h / AVP_THUMB_H;
        for (tx = 0; tx < AVP_THUMB_W; tx++) {
            int x0 = tx * w / AVP_THUMB_W, x1 = (tx + 1) * w / AVP_THUMB_W, x, y;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    uint32_t p = rgb[(size_t)y * w + x];
                    r += (p >> 16) & 255;
                    g += (p >> 8) & 255;
                    b += p & 255;
                    n++;
                }
            if (!n)
                n = 1;
            px[(size_t)ty * AVP_THUMB_W + tx] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
#ifdef _WIN32
    CreateDirectoryA("maps", NULL);
#else
    mkdir("maps", 0777);
#endif
    snprintf(path, sizeof path, "maps/%s.thumb", s_cur_map);
    f = fopen(path, "wb");
    if (f) {
        fwrite(head, 1, sizeof head, f);
        fwrite(px, sizeof *px, (size_t)AVP_THUMB_W * AVP_THUMB_H, f);
        fclose(f);
        memcpy(done, s_cur_map, sizeof done);
        fprintf(stderr, "[SKIRMISH] saved %s\n", path);
    }
    free(px);
}

int avp_skirmish_thumb_load(const char *code, uint32_t *px)
{
    char path[48];
    uint8_t head[8];
    FILE *f;
    int ok;
    if (!code_ok(code))
        return 0;
    snprintf(path, sizeof path, "maps/%s.thumb", code);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    ok = fread(head, 1, sizeof head, f) == sizeof head && !memcmp(head, "AVPT", 4)
        && head[4] + (head[5] << 8) == AVP_THUMB_W && head[6] + (head[7] << 8) == AVP_THUMB_H
        && fread(px, sizeof *px, (size_t)AVP_THUMB_W * AVP_THUMB_H, f) == (size_t)AVP_THUMB_W * AVP_THUMB_H;
    fclose(f);
    return ok;
}
