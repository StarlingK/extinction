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
#include "avp_pack.h"
#include "avp_nmap.h"
#include "avp_mapext.h"

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax);
extern uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n);
extern uint32_t avp_guest_alloca(uint32_t bytes);
extern uint32_t avp_guest_esp(void);
extern void avp_guest_set_esp(uint32_t v);
extern int avp_coop_role(void);
extern int avp_world_extent(float *w, float *h);
extern float avp_camera_z(void);
extern volatile int g_avp_guest_mapped;
extern int avp_world_rect(float *x0, float *y0, float *w, float *h);
extern void avp_camera_jump(float x, float y);

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
#define AVP_IMAGE_LO     0x00010000u
#define AVP_IMAGE_HI     0x0031B000u
#define AVP_WORLD        0x002F6AE4u
#define AVP_LOCAL_PTR    0x002F6AECu
#define AVP_SET_LOCAL    0x0002F950u
#define PM_ALLIES        0x110u
#define PM_RESOURCES     0x190u
#define PM_AIRESOURCES   0x104u
#define PM_POINTER_DATA  0x19Cu
#define MAX_SLOT         9
#define UNIT_ALLIES      0xACCu
#define VS_FILTER        0x38u
#define VS_RECEIVE       0x3Cu
#define VS_UNIT_ISDEAD   55u
#define MSG_BYTES        0x90u
#define MSG_SPAWN_TYPE   0x15u
#define MSG_CREATE_TYPE  0x16u
#define AVP_LAST_CREATED 0x002F68B4u
#define PHYS_RESOURCE    0xF0u
#define PHYS_RENDER      0x70u
#define AVP_RENDER_SET_RESOURCE 0x000DAAF0u
#define FILL_VT_PRESET   0x00207510u
#define FILL_VT_EMBEDDED 0x00207518u
#define AVP_CREATE_BY_ID 0x000D6DC0u
#define SKIP_INTRO_VT    0x00235E54u
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
static int s_intro_done;
static int s_tick_count;
static int s_seen[AVP_SKIRMISH_MAX_PLAYERS];
static int s_banner;
static int s_banner_logged;
static int s_gslot[AVP_SKIRMISH_MAX_PLAYERS];
static int s_planned;
static uint32_t s_ai_h[AVP_SKIRMISH_MAX_PLAYERS];
static int s_ai_n;

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
        } else if (!strcmp(tok[0], "nmap") && n == 2 && nmap_name_ok(tok[1])) {
            snprintf(c->nmap, sizeof c->nmap, "%s", tok[1]);
        } else if (!strcmp(tok[0], "player") && n == 6 && c->nplayers < AVP_SKIRMISH_MAX_PLAYERS) {
            AvpSkirmishPlayer pl;
            int i, dup = 0;
            if (!to_int(tok[1], 1, 8, &pl.slot) || !kind_parse(tok[2], &pl.kind)
                || !avp_race_parse(tok[3], &pl.race) || !to_int(tok[4], 1, 8, &pl.spawn)
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
    if (c->nmap[0])
        fprintf(f, "nmap %s\n", c->nmap);
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

void avp_skirmish_pick_code(AvpSkirmishCfg *c)
{
    char code[AVP_PACK_CODE_LEN + 1];
    int i, who = 0;
    AvpPlayerKind mine = avp_coop_role() == 2 ? AVP_PK_REMOTE : AVP_PK_HUMAN;
    if (!c->nmap[0] || c->nplayers < 1)
        return;
    for (i = 0; i < c->nplayers; i++)
        if (c->player[i].kind == mine) {
            who = i;
            break;
        }
    avp_pack_code(c->player[who].race, code);
    snprintf(c->map, sizeof c->map, "%s", code);
}

void avp_skirmish_arm(int editor)
{
    AvpRace race;
    char code[AVP_PACK_CODE_LEN + 1];
    init_once();
    cfg_ensure();
    if (!editor && s_cfg_loaded && s_cfg.nmap[0])
        avp_skirmish_pick_code(&s_cfg);
    if (!editor && s_cfg_loaded && s_cfg.nmap[0] && avp_pack_race(s_cfg.map, &race)
        && !avp_pack_build(race, code))
        fprintf(stderr, "[SKIRMISH] the level data for %s could not be built\n", s_cfg.map);
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

const char *avp_skirmish_nmap(void)
{
    return s_cfg.nmap;
}

int avp_skirmish_block_trig(void)
{
    return s_running && s_cfg.nmap[0];
}

#define PACK_SEP "\n#NMAP\n"
#define PACK_TERR "\n#NTERR\n"

static const char *pack_find(const char *buf, size_t n, const char *sep, size_t sl)
{
    size_t i;
    for (i = 0; i + sl <= n; i++)
        if (!memcmp(buf + i, sep, sl))
            return buf + i;
    return NULL;
}

int avp_skirmish_pack(char *buf, size_t cap)
{
    FILE *f = fopen("skirmish.cfg", "rb");
    size_t n = 0, m;
    char path[64];
    if (!f)
        return 0;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    if (!s_cfg.nmap[0] || n + sizeof PACK_SEP >= cap)
        return (int)n;
    snprintf(path, sizeof path, "maps/%s.nmap", s_cfg.nmap);
    f = fopen(path, "rb");
    if (!f)
        return (int)n;
    memcpy(buf + n, PACK_SEP, sizeof PACK_SEP - 1);
    n += sizeof PACK_SEP - 1;
    m = fread(buf + n, 1, cap - n, f);
    fclose(f);
    n += m;
    if (nmap_terrain_path(s_cfg.nmap, path, sizeof path) && n + sizeof PACK_TERR < cap) {
        f = fopen(path, "rb");
        if (f) {
            size_t t = fread(buf + n + sizeof PACK_TERR - 1, 1, cap - n - (sizeof PACK_TERR - 1), f);
            int more = fgetc(f) != EOF;
            fclose(f);
            if (t && !more) {
                memcpy(buf + n, PACK_TERR, sizeof PACK_TERR - 1);
                n += sizeof PACK_TERR - 1 + t;
            }
        }
    }
    return (int)n;
}

static void net_dir(void)
{
#ifdef _WIN32
    CreateDirectoryA("maps", NULL);
#else
    mkdir("maps", 0777);
#endif
}

int avp_skirmish_unpack(const char *buf, size_t n)
{
    const char *sep, *terr = NULL;
    size_t sl = sizeof PACK_SEP - 1, cfgn = n, nmapn = 0;
    FILE *f;
    sep = pack_find(buf, n, PACK_SEP, sl);
    if (sep)
        cfgn = (size_t)(sep - buf) + 1;
    f = fopen("skirmish.cfg", "wb");
    if (!f)
        return 0;
    if (sep) {
        char *tmp = malloc(cfgn + 32), *p;
        const char *rest = sep + sl;
        size_t restn = n - (size_t)(rest - buf), tl = sizeof PACK_TERR - 1;
        int w = 1;
        if (!tmp) {
            fclose(f);
            return 0;
        }
        memcpy(tmp, buf, cfgn);
        tmp[cfgn] = 0;
        for (p = strtok(tmp, "\n"); p; p = strtok(NULL, "\n"))
            fprintf(f, "%s\n", strncmp(p, "nmap ", 5) ? p : "nmap _net");
        free(tmp);
        fclose(f);
        net_dir();
        terr = pack_find(rest, restn, PACK_TERR, tl);
        nmapn = terr ? (size_t)(terr - rest) : restn;
        f = fopen("maps/_net.nmap", "wb");
        if (!f)
            return 0;
        w = fwrite(rest, 1, nmapn, f) == nmapn && nmapn != 0;
        fclose(f);
        remove("maps/_net.nterr");
        if (terr && n - (size_t)(terr + tl - buf) > 0) {
            f = fopen("maps/_net.nterr", "wb");
            if (f) {
                size_t tn = n - (size_t)(terr + tl - buf);
                if (fwrite(terr + tl, 1, tn, f) != tn)
                    w = 0;
                fclose(f);
            }
        }
        return w;
    }
    fwrite(buf, 1, n, f);
    fclose(f);
    return 1;
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
    avp_mapext_stop();
    s_planned = 0;
    s_intro_done = 0;
    s_ai_n = 0;
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

uint32_t avp_skirmish_fill_exists(const char *fill)
{
    uint32_t esp = avp_guest_esp(), base, name, out, a[1], regs[6], r;
    size_t i, len = strlen(fill);
    if (len > 100)
        return 0;
    base = avp_guest_alloca(0x100);
    name = base;
    out = base + 0xC0;
    for (i = 0; i <= len; i++)
        G8(name + i) = (uint8_t)fill[i];
    G32(out) = 0;
    regs[0] = 0; regs[1] = 0; regs[2] = 0; regs[3] = out; regs[4] = name; regs[5] = 0;
    a[0] = 0;
    avp_gcall_regs(AVP_FIND_FILL, regs, a, 1);
    r = G32(out);
    if (!g_ptr_ok(r) || !g_ptr_ok(G32(r)))
        r = 0;
    avp_guest_set_esp(esp);
    return r;
}

static uint32_t class_key(const char *s)
{
    static uint32_t tab[256];
    uint32_t c = 0xFFFFFFFFu;
    if (!tab[1]) {
        uint32_t i, k, x;
        for (i = 0; i < 256; i++) {
            x = i;
            for (k = 0; k < 8; k++)
                x = (x & 1u) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            tab[i] = x;
        }
    }
    for (; *s; s++)
        c = tab[(c ^ (uint8_t)*s) & 0xFFu] ^ (c >> 8);
    return c;
}

static uint32_t spawn_by_class(const char *cls, int slot, float x, float y, float z, float rot, uint32_t esp, uint32_t resource)
{
    uint32_t msg = avp_guest_alloca(0x100), obj = G32(AVP_WORLD_OBJ), vt, a[1], regs[6], fn, made, i, h = 0;
    float f;
    if (!g_ptr_ok(obj)) {
        fprintf(stderr, "[SKIRMISH] spawn %s: no world object %08X\n", cls, obj);
        avp_guest_set_esp(esp);
        return 0;
    }
    for (i = 0; i < MSG_BYTES; i += 4)
        G32(msg + i) = 0;
    G32(msg) = MSG_CREATE_TYPE;
    G32(msg + 4) = G32(obj + 0x18);
    G8(msg + 8) = MSG_PRIORITY;
    G32(msg + 0xC) = class_key(cls);
    G32(msg + 0x10) = (uint32_t)slot;
    f = x; memcpy(&i, &f, 4); G32(msg + 0x14) = i;
    f = y; memcpy(&i, &f, 4); G32(msg + 0x18) = i;
    f = z; memcpy(&i, &f, 4); G32(msg + 0x1C) = i;
    GF(msg + 0x20) = 0.0f;
    GF(msg + 0x24) = 0.0f;
    f = rot; memcpy(&i, &f, 4); G32(msg + 0x28) = i;
    G32(AVP_LAST_CREATED) = 0;
    vt = G32(obj);
    a[0] = msg;
    regs[0] = 0; regs[1] = obj; regs[2] = 0; regs[3] = 0; regs[4] = obj; regs[5] = 0;
    fn = G32(vt + VS_FILTER);
    if (avp_gcall_regs(fn, regs, a, 1) & 0xFFu) {
        fn = G32(vt + VS_RECEIVE);
        regs[0] = 0; regs[1] = obj; regs[2] = 0; regs[3] = 0; regs[4] = obj; regs[5] = 0;
        avp_gcall_regs(fn, regs, a, 1);
    }
    made = G32(AVP_LAST_CREATED);
    if (g_ptr_ok(made) && handle_live(G32(made + 0x18), NULL)) {
        h = G32(made + 0x18);
        if (resource)
            G32(made + PHYS_RESOURCE) = resource;
    }
    if (!h)
        fprintf(stderr, "[SKIRMISH] spawn %s: class %08X created nothing\n", cls, class_key(cls));
    avp_guest_set_esp(esp);
    return h;
}

static uint32_t next_handle(int slot)
{
    uint32_t esp = avp_guest_esp(), out = avp_guest_alloca(16), regs[6] = { 0 }, h;
    G32(out) = 0;
    regs[0] = out;
    regs[2] = (uint32_t)slot;
    avp_gcall_regs(AVP_NEXT_HANDLE, regs, NULL, 0);
    h = G32(out);
    avp_guest_set_esp(esp);
    return h;
}

static uint32_t create_obj(const char *cls, uint32_t handle, uint32_t fill)
{
    uint32_t esp = avp_guest_esp(), regs[6] = { 0 }, a[2], obj;
    a[0] = handle;
    a[1] = fill;
    regs[5] = class_key(cls);
    obj = avp_gcall_regs(AVP_CREATE_BY_ID, regs, a, 2);
    if (!g_ptr_ok(obj) || !handle_live(G32(obj + 0x18), NULL))
        obj = 0;
    avp_guest_set_esp(esp);
    return obj;
}

static uint32_t create_ai(const char *cls, int slot)
{
    uint32_t obj = create_obj(cls, (uint32_t)slot, 0);
    if (!obj)
        fprintf(stderr, "[SKIRMISH] AI player %s for slot %d could not be created\n", cls, slot);
    return obj ? G32(obj + 0x18) : 0;
}

static uint32_t s_decor_res;

uint32_t avp_skirmish_decor_pending(void)
{
    return s_decor_res;
}

static uint32_t place_decor(uint32_t resource, int slot, float x, float y, float z, float rot, uint32_t esp)
{
    uint32_t obj, pos, ang, regs[6], a[1];
    s_decor_res = resource;
    obj = create_obj("GDecor", next_handle(slot), 0);
    s_decor_res = 0;
    if (!obj) {
        fprintf(stderr, "[SKIRMISH] GDecor create failed\n");
        avp_guest_set_esp(esp);
        return 0;
    }
    G32(obj + PHYS_RESOURCE) = resource;
    {
        uint32_t sr[6] = { 0 }, sa[2];
        sr[4] = obj + PHYS_RENDER;
        sa[0] = resource;
        sa[1] = 0;
        avp_gcall_regs(AVP_RENDER_SET_RESOURCE, sr, sa, 2);
    }
    pos = avp_guest_alloca(32);
    ang = pos + 16;
    GF(pos) = x; GF(pos + 4) = y; GF(pos + 8) = z;
    GF(ang) = 0.0f; GF(ang + 4) = 0.0f; GF(ang + 8) = rot;
    a[0] = pos;
    regs[0] = 0; regs[1] = obj; regs[2] = 0; regs[3] = 0; regs[4] = obj; regs[5] = 0;
    avp_gcall_regs(G32(G32(obj) + 0x58u), regs, a, 1);
    a[0] = ang;
    regs[0] = 0; regs[1] = obj; regs[2] = 0; regs[3] = 0; regs[4] = obj; regs[5] = 0;
    avp_gcall_regs(G32(G32(obj) + 0x5Cu), regs, a, 1);
    avp_guest_set_esp(esp);
    return G32(obj + 0x18);
}

uint32_t avp_skirmish_spawn(const char *cls, int slot, float x, float y, float z, float rot)
{
    uint32_t esp = avp_guest_esp(), base, name, out, msg, fill, key, handle, obj, vt, a[1], regs[6];
    uint32_t ret = 0, fn, accepted = 0, objout;
    size_t len;
    uint32_t i;
    float f;
    if (!cls || !cls[0] || !isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(rot) || slot < 0 || slot > 0x1F)
        return 0;
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
    if (cls[0] != '@')
        return spawn_by_class(cls, slot, x, y, z, rot, esp, 0);
    for (i = 0; i <= len - 1; i++)
        G8(name + i) = (uint8_t)cls[1 + i];
    G32(out) = 0;
    for (i = 0; i < MSG_BYTES; i += 4)
        G32(msg + i) = 0;
    regs[0] = 0; regs[1] = 0; regs[2] = 0; regs[3] = out; regs[4] = name; regs[5] = 0;
    a[0] = 0;
    avp_gcall_regs(AVP_FIND_FILL, regs, a, 1);
    fill = G32(out);
    fprintf(stderr, "[SKIRMISH] spawn %s slot %d: template entry %08X\n", cls, slot, fill);
    if (!g_ptr_ok(fill) || !g_ptr_ok(G32(fill))) {
        avp_guest_set_esp(esp);
        return 0;
    }
    if (G32(G32(fill)) != FILL_VT_PRESET && G32(G32(fill)) != FILL_VT_EMBEDDED) {
        return place_decor(G32(fill), slot, x, y, z, rot, esp);
    }
    key = G32(fill + 4);
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
    G32(msg + 0xC) = key;
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
    return (int)(G32(obj + 0x18) & 0xFFFFu);
}

static int unit_dead(uint32_t obj)
{
    uint32_t vt = G32(obj), regs[6];
    regs[0] = G32(obj + 0x18); regs[1] = obj; regs[2] = vt; regs[3] = 0; regs[4] = obj; regs[5] = 0;
    return (avp_gcall_regs(G32(vt + VS_UNIT_ISDEAD * 4u), regs, NULL, 0) & 0xFFu) != 0;
}

void avp_skirmish_ally_all(uint32_t h)
{
    uint32_t obj;
    if (h && handle_live(h, &obj) && unit_player(obj) > 0)
        G32(obj + UNIT_ALLIES) = 0xFFFFFFFFu;
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
        if (p < 1 || p > MAX_SLOT)
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
        { AVP_RACE_MARINE, "GMarineMedic", 1 }, { AVP_RACE_MARINE, "GSentryGun", 1 },
        { AVP_RACE_ALIEN, "GQueen", 1 }, { AVP_RACE_ALIEN, "GHiveNode", 1 }, { AVP_RACE_ALIEN, "GAlienDrone", 4 },
        { AVP_RACE_ALIEN, "GEgg", 2 },
        { AVP_RACE_PREDATOR, "GSkullKeeper", 1 }, { AVP_RACE_PREDATOR, "GPredatorWarrior", 2 },
        { AVP_RACE_PREDATOR, "GPredatorBlooded", 1 }, { AVP_RACE_PREDATOR, "GPredSentryGun", 1 }
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

static int is_center_class(const char *cls)
{
    static const char *const c[] = { "GMarineCommTech", "GQueen", "GSkullKeeper", "GHiveNode", "GEggSack" };
    size_t i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++)
        if (!strcmp(cls, c[i]))
            return 1;
    return 0;
}

static uint32_t player_manager(int slot)
{
    uint32_t world = G32(0x002F6AE4u), pm;
    if (!g_ptr_ok(world) || slot < 0 || slot >= 0x20)
        return 0;
    pm = G32(world + 0x74u + 4u * (uint32_t)slot);
    return g_ptr_ok(pm) ? pm : 0;
}

static const char *obj_class(uint32_t obj, char *buf, size_t len)
{
    uint32_t vt, col, td, k;
    buf[0] = 0;
    if (!g_ptr_ok(obj))
        return buf;
    vt = G32(obj);
    if (vt < AVP_IMAGE_LO + 4 || vt >= AVP_IMAGE_HI)
        return buf;
    col = G32(vt - 4);
    if (col < AVP_IMAGE_LO || col >= AVP_IMAGE_HI)
        return buf;
    td = G32(col + 12);
    if (td < AVP_IMAGE_LO || td >= AVP_IMAGE_HI || G8(td + 8) != '.')
        return buf;
    for (k = 0; k + 1 < len && G8(td + 12 + k) && G8(td + 12 + k) != '@'; k++)
        buf[k] = (char)G8(td + 12 + k);
    buf[k] = 0;
    return buf;
}

static int mgr_race(int slot)
{
    char cb[48];
    uint32_t pm = player_manager(slot);
    if (!pm)
        return -1;
    obj_class(pm, cb, sizeof cb);
    if (!strcmp(cb, "GAlienPlayerManager"))
        return AVP_RACE_ALIEN;
    if (!strcmp(cb, "GMarinePlayerManager"))
        return AVP_RACE_MARINE;
    if (!strcmp(cb, "GPredatorPlayerManager"))
        return AVP_RACE_PREDATOR;
    return -1;
}

static void plan_slots(void)
{
    int i, t, used[MAX_SLOT + 1] = { 0 }, cfgslot[MAX_SLOT + 1] = { 0 };
    if (s_planned)
        return;
    cfg_ensure();
    for (i = 0; i < s_cfg.nplayers; i++) {
        int sl = s_cfg.player[i].slot;
        s_gslot[i] = 0;
        if (sl >= 0 && sl <= MAX_SLOT)
            cfgslot[sl] = 1;
    }
    for (i = 0; i < s_cfg.nplayers; i++) {
        int sl = s_cfg.player[i].slot;
        if (sl >= 0 && sl <= MAX_SLOT && mgr_race(sl) == (int)s_cfg.player[i].race) {
            s_gslot[i] = sl;
            used[sl] = 1;
        }
    }
    for (i = 0; i < s_cfg.nplayers; i++) {
        if (s_gslot[i])
            continue;
        for (t = 1; t < MAX_SLOT; t++)
            if (!used[t] && !cfgslot[t] && mgr_race(t) == (int)s_cfg.player[i].race) {
                s_gslot[i] = t;
                used[t] = 1;
                break;
            }
    }
    for (i = 0; i < s_cfg.nplayers; i++) {
        if (!s_gslot[i])
            s_gslot[i] = s_cfg.player[i].slot;
        fprintf(stderr, "[SKIRMISH] plan: player %d (%s) uses game slot %d, existing manager race %d\n",
                s_cfg.player[i].slot, avp_race_name(s_cfg.player[i].race), s_gslot[i], mgr_race(s_gslot[i]));
    }
    s_planned = 1;
}

static int gslot_of(int i)
{
    return s_planned ? s_gslot[i] : s_cfg.player[i].slot;
}

static int idx_of_gslot(int g)
{
    int i;
    for (i = 0; i < s_cfg.nplayers; i++)
        if (gslot_of(i) == g)
            return i;
    return -1;
}

int avp_skirmish_game_slot(int cfg_slot)
{
    int i;
    if (!s_running || s_editor)
        return cfg_slot;
    cfg_ensure();
    if (s_cfg.nmap[0])
        plan_slots();
    i = slot_cfg_index(cfg_slot);
    return i < 0 ? cfg_slot : gslot_of(i);
}

static int team_mask(int g)
{
    int i, ci = idx_of_gslot(g), mask = 1 << g;
    if (ci < 0)
        return mask;
    for (i = 0; i < s_cfg.nplayers; i++)
        if (s_cfg.player[i].team == s_cfg.player[ci].team)
            mask |= 1 << gslot_of(i);
    return mask;
}

static uint32_t spawn_team(const char *cls, int slot, float x, float y, float z, float rot)
{
    uint32_t h = avp_skirmish_spawn(cls, slot, x, y, z, rot), obj;
    if (h && handle_live(h, &obj) && unit_player(obj) > 0)
        G32(obj + UNIT_ALLIES) = (uint32_t)team_mask(slot);
    return h;
}

static void clear_all_units(const char *tag)
{
    uint32_t cursor = 1, obj;
    int player, killed = 0;
    while (each_unit(&cursor, &obj, &player)) {
        avp_gcall(AVP_START_DEATH, 0, (uint32_t[]){ G32(obj + 0x18), 1 }, 2, G32(obj + 0x18));
        killed++;
    }
    fprintf(stderr, "[SKIRMISH] %s: %d mission units cleared\n", tag, killed);
}

static int ensure_manager(int i, int *replaced)
{
    static const char *const named[AVP_RACE_COUNT] = { "@AlienPlayerManager", "@MarinePlayerManager", "@PredatorPlayerManager" };
    static const char *const mgr_cls[AVP_RACE_COUNT] = { "GAlienPlayerManager", "GMarinePlayerManager", "GPredatorPlayerManager" };
    const AvpSkirmishPlayer *pl = &s_cfg.player[i];
    int g = gslot_of(i);
    uint32_t pm;
    *replaced = 0;
    if (mgr_race(g) != (int)pl->race) {
        if (!avp_skirmish_spawn(named[pl->race], g, 0.0f, 0.0f, 0.0f, 0.0f))
            avp_skirmish_spawn(mgr_cls[pl->race], g, 0.0f, 0.0f, 0.0f, 0.0f);
        if (mgr_race(g) != (int)pl->race) {
            fprintf(stderr, "[SKIRMISH] slot %d: no %s player manager could be created\n", g, avp_race_name(pl->race));
            return 0;
        }
        *replaced = 1;
        pm = player_manager(g);
        if (pl->kind == AVP_PK_AI && GF(pm + PM_RESOURCES) == 0.0f && GF(pm + PM_AIRESOURCES) > 0.0f) {
            GF(pm + PM_RESOURCES) = GF(pm + PM_AIRESOURCES);
            GF(pm + PM_AIRESOURCES) = 0.0f;
        }
    }
    pm = player_manager(g);
    if (!pm)
        return 0;
    G32(pm + PM_ALLIES) = (uint32_t)team_mask(g);
    fprintf(stderr, "[SKIRMISH] player %d: game slot %d manager %08X (%s)%s, allies %08X\n", pl->slot, g, pm,
            avp_race_name(pl->race), *replaced ? " created" : "", (unsigned)team_mask(g));
    return 1;
}

static void switch_local(int g, int force)
{
    uint32_t world = G32(AVP_WORLD), ptr = G32(AVP_LOCAL_PTR), pm, regs[6] = { 0 };
    int cur = -1;
    if (g < 1 || g > MAX_SLOT || !g_ptr_ok(world))
        return;
    if (g_ptr_ok(ptr))
        cur = (int)(G32(ptr + 0x18) & 0xFFFFu);
    if (cur == g && !force)
        return;
    pm = player_manager(g);
    if (!pm || !G32(pm + PM_POINTER_DATA)) {
        fprintf(stderr, "[SKIRMISH] local player: slot %d has no pointer data, staying on slot %d\n", g, cur);
        return;
    }
    regs[0] = (uint32_t)g;
    regs[1] = world;
    avp_gcall_regs(AVP_SET_LOCAL, regs, NULL, 0);
    ptr = G32(AVP_LOCAL_PTR);
    fprintf(stderr, "[SKIRMISH] local player switched from slot %d to slot %d, pointer %08X\n", cur, g, ptr);
}

static void nmap_start(void)
{
    static const char *const ai_cls[AVP_RACE_COUNT] = { "GAIPlayerAlien", "GAIPlayerMarine", "GAIPlayerPredator" };
    Nmap *m = malloc(sizeof *m);
    ForceLine forces[MAX_FORCE_LINES];
    int nforces, i, spawned = 0, failed = 0, replaced[AVP_SKIRMISH_MAX_PLAYERS] = { 0 }, ok[AVP_SKIRMISH_MAX_PLAYERS] = { 0 };
    float x0 = 0.0f, y0 = 0.0f, rw = 0.0f, rh = 0.0f;
    int have_rect;
    if (!m)
        return;
    if (nmap_load(m, s_cfg.nmap) < 0) {
        fprintf(stderr, "[SKIRMISH] custom map %s missing\n", s_cfg.nmap);
        free(m);
        return;
    }
    have_rect = avp_world_rect(&x0, &y0, &rw, &rh);
    if (!have_rect)
        x0 = y0 = rw = rh = 0.0f;
    nmap_resolve(m, rw, rh, avp_camera_z());
    avp_mapext_prepare(m);
    if (have_rect)
        nmap_to_world(m, x0, y0, rw, rh, 3.0f);
    fprintf(stderr, "[SKIRMISH] world rect origin %.1f,%.1f size %.1f x %.1f\n", x0, y0, rw, rh);
    plan_slots();
    clear_all_units("custom map");
    fprintf(stderr, "[SKIRMISH] custom map %s on %s: %d objects\n", s_cfg.nmap, m->base, m->nobj);
    nforces = load_forces(forces, MAX_FORCE_LINES);
    G8(0x002F6B14u) = 0;
    s_ai_n = 0;
    for (i = 0; i < s_cfg.nplayers; i++)
        ok[i] = ensure_manager(i, &replaced[i]);
    for (i = 0; i < s_cfg.nplayers; i++) {
        const AvpSkirmishPlayer *pl = &s_cfg.player[i];
        const NmapSpawn *sp = &m->spawn[(pl->spawn - 1) & 7];
        uint32_t h;
        if (!ok[i] || pl->kind != AVP_PK_AI || !sp->used)
            continue;
        h = create_ai(ai_cls[pl->race], gslot_of(i));
        if (h) {
            if (s_ai_n < AVP_SKIRMISH_MAX_PLAYERS)
                s_ai_h[s_ai_n++] = h;
            spawned++;
        } else {
            failed++;
        }
    }
    for (i = 0; i < m->nobj; i++) {
        const NmapObj *o = &m->obj[i];
        int own = o->owner ? avp_skirmish_game_slot(o->owner) : 9;
        if (spawn_team(o->cls, own, o->x, o->y, o->z, o->rot))
            spawned++;
        else
            failed++;
    }
    for (i = 0; i < s_cfg.nplayers; i++) {
        const AvpSkirmishPlayer *pl = &s_cfg.player[i];
        const NmapSpawn *sp = &m->spawn[(pl->spawn - 1) & 7];
        int k, c, total = 0, idx = 0, g = gslot_of(i);
        if (!ok[i] || !sp->used)
            continue;
        for (k = 0; k < nforces; k++)
            if (forces[k].race == pl->race && !is_center_class(forces[k].cls))
                total += forces[k].count;
        for (k = 0; k < nforces; k++) {
            if (forces[k].race != pl->race)
                continue;
            for (c = 0; c < forces[k].count; c++) {
                float x = sp->x, y = sp->y;
                if (!is_center_class(forces[k].cls)) {
                    float ang = 2.0f * PI_F * (float)idx / (float)(total ? total : 1);
                    x += RING_RADIUS * cosf(ang);
                    y += RING_RADIUS * sinf(ang);
                    idx++;
                }
                if (spawn_team(forces[k].cls, g, x, y, sp->z, sp->rot))
                    spawned++;
                else
                    failed++;
            }
        }
        fprintf(stderr, "[SKIRMISH] slot %d %s %s team %d at spawn %d: base spawned\n", g,
                kind_name(pl->kind), avp_race_name(pl->race), pl->team, pl->spawn);
    }
    if (avp_coop_role() == 0)
        for (i = 0; i < s_cfg.nplayers; i++) {
            const AvpSkirmishPlayer *pl = &s_cfg.player[i];
            const NmapSpawn *sp = &m->spawn[(pl->spawn - 1) & 7];
            if (ok[i] && pl->kind == AVP_PK_HUMAN) {
                switch_local(gslot_of(i), replaced[i]);
                if (sp->used)
                    avp_camera_jump(sp->x, sp->y);
                break;
            }
        }
    fprintf(stderr, "[SKIRMISH] custom spawn done: %d ok, %d failed\n", spawned, failed);
    avp_mapext_start(m);
    free(m);
    reset_state();
    s_started = 1;
    s_spawn_t = time(NULL);
}

int avp_skirmish_hide_marker(uint32_t owner)
{
    uint32_t local = G32(0x002F6AECu);
    if (!s_running || s_editor || !local)
        return 0;
    return owner != local;
}

int avp_skirmish_skip_ai(uint32_t ai)
{
    int slot, i;
    if (!s_running)
        return 0;
    if (s_editor || avp_coop_role() == 2)
        return 1;
    if (!g_ptr_ok(ai))
        return 0;
    if (s_cfg.nmap[0]) {
        for (i = 0; i < s_ai_n; i++)
            if (s_ai_h[i] == G32(ai + 0x18))
                return 0;
        return 1;
    }
    slot = (int)(G32(ai + 0x18) & 0xFFFFu);
    for (i = 0; i < s_cfg.nplayers; i++)
        if (s_cfg.player[i].slot == slot)
            return s_cfg.player[i].kind != AVP_PK_AI;
    return 1;
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
    if (editing && s_cfg.nmap[0]) {
        free(lay);
        clear_all_units("editor");
        return;
    }
    if (editing || !active) {
        free(lay);
        return;
    }
    if (s_cfg.nmap[0]) {
        free(lay);
        nmap_start();
        return;
    }
    fprintf(stderr, "[SKIRMISH] clear: unit test is RTTI GUnit dyncast, player at +0x474\n");
    while (each_unit(&cursor, &obj, &player)) {
        if (idx_of_gslot(player) < 0)
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

void avp_skirmish_frame(void)
{
    static int frames;
    uint32_t i;
    if (!s_running || s_editor || s_intro_done || s_started || !g_avp_guest_mapped)
        return;
    if (++frames % 15)
        return;
    for (i = 1; i < 0x10000u; i++) {
        uint32_t o = G32(AVP_OBJ_TABLE + i * 4u);
        if (!g_ptr_ok(o) || G32(o) != SKIP_INTRO_VT || (G32(o + 0x18) >> 16) != i)
            continue;
        G8(o + 0x268) = 1;
        s_intro_done = 1;
        fprintf(stderr, "[SKIRMISH] level intro skipped\n");
        return;
    }
}

void avp_skirmish_tick(void)
{
    int alive[AVP_SKIRMISH_MAX_PLAYERS] = { 0 };
    int teams_alive[100] = { 0 };
    uint32_t cursor = 1, obj;
    int player, i, nteams = 0, winner = -1, human_won = 0;
    avp_mapext_tick();
    if (!s_started || s_banner || s_editor || !avp_skirmish_active())
        return;
    if (++s_tick_count < TICK_PERIOD)
        return;
    s_tick_count = 0;
    while (each_unit(&cursor, &obj, &player)) {
        int ci = idx_of_gslot(player);
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
    if (time(NULL) - s_spawn_t >= 30)
        for (i = 0; i < s_cfg.nplayers; i++) {
            AvpPlayerKind k = s_cfg.player[i].kind;
            int mine = (k == AVP_PK_HUMAN && avp_coop_role() != 2) || (k == AVP_PK_REMOTE && avp_coop_role() == 2);
            if (mine && s_seen[i] && !teams_alive[s_cfg.player[i].team]) {
                s_banner = 2;
                if (!s_banner_logged) {
                    s_banner_logged = 1;
                    fprintf(stderr, "[SKIRMISH] team %d has no units or buildings left: DEFEAT\n", s_cfg.player[i].team);
                }
                return;
            }
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
