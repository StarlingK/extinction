#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GF(a)  (*(volatile float *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define G8(a)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define AVP_CLOCK        0x002F689Cu
#define AVP_CLOCK_SCALE  0x002F6888u
#define AVP_RNG_ACC      0x002CC280u
#define AVP_RNG_SEED     0x002CC284u
#define AVP_OBJ_TABLE    0x00289E40u
#define AVP_TIMER_LO     0x000DA400u
#define AVP_TIMER_HI     0x000DA900u
#define AVP_TIMER_TICK   0x000DA65Du
#define AVP_WAIT_START   0x000DA7EDu
#define AVP_WAIT_POLL    0x000DA82Au
#define OBJ_BYTES        0x80u

enum { ROLE_NONE, ROLE_HOST, ROLE_GUEST };

static int s_role;
static void coop_start_net(void);
static uint32_t s_rng_save[2];
static int s_rng_saved;
static int s_fixed = -1, s_hz = 60, s_log_every = 30, s_dump_tick = -1;
static uint64_t s_vcount = 0x100000000ull, s_step, s_real_at_tick;
static uint64_t s_tick, s_mission_tick;
static int s_mission;

static void coop_init(void)
{
    const char *e;
    if (s_fixed >= 0) return;
    e = getenv("AVP_FIXED_STEP");
    s_fixed = e && *e && *e != '0';
    if (s_fixed && atoi(e) > 1) s_hz = atoi(e);
    coop_start_net();
    if (s_role != ROLE_NONE) s_fixed = 1;
    e = getenv("AVP_SYNC_LOG");
    if (e && atoi(e) > 0) s_log_every = atoi(e);
    e = getenv("AVP_SYNC_DUMP");
    if (e && *e) s_dump_tick = atoi(e);
    if (s_fixed)
        fprintf(stderr, "[SYNC] fixed step %d Hz\n", s_hz);
}

int avp_coop_fixed(void)
{
    coop_init();
    return s_fixed;
}

uint64_t avp_coop_now_ms(void)
{
    coop_init();
    if (!s_fixed) return GetTickCount64();
    return s_tick * 1000u / (uint64_t)s_hz;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 0x100000001B3ull; }
    return h;
}

static uint64_t obj_hash(uint32_t obj)
{
    uint8_t buf[OBJ_BYTES];
    uint32_t i;
    for (i = 0; i < OBJ_BYTES; i++) buf[i] = G8(obj + i);
    return fnv(0xCBF29CE484222325ull, buf, sizeof buf);
}

static int obj_ok(uint32_t obj)
{
    return obj >= 0x10000u && obj < 0x84000000u - OBJ_BYTES;
}

static void sync_report(int dump)
{
    uint64_t h = 0xCBF29CE484222325ull, world = h;
    uint32_t clock = G32(AVP_CLOCK), acc = G32(AVP_RNG_ACC), seed = G32(AVP_RNG_SEED), i;
    int n = 0;
    for (i = 1; i < 0x10000u; i++) {
        uint32_t obj = G32(AVP_OBJ_TABLE + i * 4u);
        uint64_t oh;
        if (!obj_ok(obj)) continue;
        oh = obj_hash(obj);
        world = fnv(world, &i, sizeof i);
        world = fnv(world, &oh, sizeof oh);
        n++;
        if (dump)
        {
            uint32_t k;
            fprintf(stderr, "[SYNCOBJ] %04X vt %08X %016llX at %08X\n", i, G32(obj), (unsigned long long)oh, obj);
            fprintf(stderr, "[SYNCRAW] %04X", i);
            for (k = 0; k < OBJ_BYTES; k += 4) fprintf(stderr, " %08X", G32(obj + k));
            fprintf(stderr, "\n");
        }
    }
    h = fnv(h, &clock, sizeof clock);
    h = fnv(h, &acc, sizeof acc);
    h = fnv(h, &seed, sizeof seed);
    fprintf(stderr, "[SYNC] tick %llu clock %.4f rng %08X %08X objs %d core %016llX world %016llX\n",
            (unsigned long long)(s_tick - s_mission_tick), (double)GF(AVP_CLOCK), acc, seed, n,
            (unsigned long long)h, (unsigned long long)world);
}

static uint64_t core_hash(void)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t v[3] = { G32(AVP_CLOCK), G32(AVP_RNG_ACC), G32(AVP_RNG_SEED) };
    return fnv(h, v, sizeof v);
}

enum { MSG_HELLO = 1, MSG_START = 2, MSG_TURN = 3, MSG_PAGEHASH = 4, MSG_PAGES = 5 };

#define PAGE          0x1000u
#define BULK_MAX      (64u << 20)

static const uint32_t k_sync_ranges[][2] = {
    { 0x00265000u, 0x00311000u },
    { 0x00F80000u, 0x03000000u },
};

static uint8_t *volatile s_bulk[8];
static volatile uint32_t s_bulk_len[8];

#define TURN_TICKS   4
#define TURN_DELAY   2
#define TURN_RING    64
#define CMD_BYTES    8192
#define WAIT_MS      15000
#define COOP_MAGIC   0x43505641u
#define COOP_VERSION 1u

typedef struct {
    uint32_t turn;
    int have;
    uint64_t hash;
    uint32_t len;
    uint8_t data[CMD_BYTES];
} Turn;

typedef void (*avp_coop_exec_fn)(int from_host, const uint8_t *data, uint32_t len);

static int s_peer_started, s_coop_dead;
static SOCKET s_sock = INVALID_SOCKET;
static CRITICAL_SECTION s_lock;
static Turn s_remote[TURN_RING];
static Turn s_local[TURN_RING];
static uint64_t s_hashes[TURN_RING];
static uint8_t s_pending[CMD_BYTES];
static uint32_t s_npending;
static avp_coop_exec_fn s_exec;
static HANDLE s_net_ready;

void avp_coop_set_exec(avp_coop_exec_fn fn)
{
    s_exec = fn;
}

int avp_coop_role(void)
{
    return s_role;
}

int avp_coop_queue(const void *data, uint32_t len)
{
    int ok = 0;
    if (s_role == ROLE_NONE || s_coop_dead) return 0;
    EnterCriticalSection(&s_lock);
    if (s_npending + 4 + len <= CMD_BYTES) {
        memcpy(s_pending + s_npending, &len, 4);
        memcpy(s_pending + s_npending + 4, data, len);
        s_npending += 4 + len;
        ok = 1;
    }
    LeaveCriticalSection(&s_lock);
    return ok;
}

static int send_all(const void *p, int n)
{
    const char *c = (const char *)p;
    while (n > 0) {
        int k = send(s_sock, c, n, 0);
        if (k <= 0) return 0;
        c += k;
        n -= k;
    }
    return 1;
}

static int recv_all(void *p, int n)
{
    char *c = (char *)p;
    while (n > 0) {
        int k = recv(s_sock, c, n, 0);
        if (k <= 0) return 0;
        c += k;
        n -= k;
    }
    return 1;
}

static int send_msg(uint32_t type, const void *body, uint32_t len)
{
    uint32_t hdr[2] = { type, len };
    return send_all(hdr, sizeof hdr) && (!len || send_all(body, (int)len));
}

static void coop_fail(const char *why)
{
    if (s_coop_dead) return;
    s_coop_dead = 1;
    fprintf(stderr, "[COOP] connection lost: %s; carrying on alone\n", why);
}

static DWORD WINAPI net_thread(LPVOID arg)
{
    const char *spec = (const char *)arg;
    WSADATA wd;
    struct sockaddr_in a;
    uint32_t hello[3] = { COOP_MAGIC, COOP_VERSION, 0 }, peer[3];
    int one = 1;
    char host[128] = "127.0.0.1";
    int port = 27015;

    WSAStartup(MAKEWORD(2, 2), &wd);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    if (s_role == ROLE_HOST) {
        SOCKET ls;
        sscanf(spec, "host:%d", &port);
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)port);
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) {
            coop_fail("could not listen");
            SetEvent(s_net_ready);
            return 0;
        }
        fprintf(stderr, "[COOP] hosting on port %d, waiting for the other player\n", port);
        s_sock = accept(ls, NULL, NULL);
        closesocket(ls);
    } else {
        int tries;
        sscanf(spec, "join:%127[^:]:%d", host, &port);
        a.sin_port = htons((u_short)port);
        inet_pton(AF_INET, host, &a.sin_addr);
        for (tries = 0; tries < 120; tries++) {
            s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (!connect(s_sock, (struct sockaddr *)&a, sizeof a)) break;
            closesocket(s_sock);
            s_sock = INVALID_SOCKET;
            Sleep(500);
        }
        fprintf(stderr, "[COOP] %s %s:%d\n", s_sock == INVALID_SOCKET ? "could not reach" : "joined", host, port);
    }
    if (s_sock == INVALID_SOCKET) {
        coop_fail("no connection");
        SetEvent(s_net_ready);
        return 0;
    }
    setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    hello[2] = (uint32_t)s_role;
    if (!send_msg(MSG_HELLO, hello, sizeof hello)) { coop_fail("hello"); SetEvent(s_net_ready); return 0; }
    SetEvent(s_net_ready);
    for (;;) {
        uint32_t hdr[2];
        if (!recv_all(hdr, sizeof hdr)) { coop_fail("closed"); return 0; }
        if (hdr[0] == MSG_PAGEHASH || hdr[0] == MSG_PAGES) {
            uint8_t *b;
            if (hdr[1] > BULK_MAX || !(b = (uint8_t *)malloc(hdr[1] ? hdr[1] : 1))) { coop_fail("bulk size"); return 0; }
            if (hdr[1] && !recv_all(b, (int)hdr[1])) { free(b); coop_fail("bulk data"); return 0; }
            s_bulk_len[hdr[0]] = hdr[1];
            s_bulk[hdr[0]] = b;
            continue;
        }
        if (hdr[1] > sizeof(Turn) + 64) { coop_fail("bad packet"); return 0; }
        if (hdr[0] == MSG_HELLO) {
            if (hdr[1] != sizeof peer || !recv_all(peer, sizeof peer)) { coop_fail("hello"); return 0; }
            if (peer[0] != COOP_MAGIC || peer[1] != COOP_VERSION || peer[2] == (uint32_t)s_role) {
                coop_fail("the other game is a different version or the same role");
                return 0;
            }
            fprintf(stderr, "[COOP] connected to the %s\n", peer[2] == ROLE_HOST ? "host" : "guest");
        } else if (hdr[0] == MSG_START) {
            s_peer_started = 1;
            fprintf(stderr, "[COOP] the other player's mission has started\n");
        } else if (hdr[0] == MSG_TURN) {
            uint32_t head[4], len;
            Turn *t;
            if (hdr[1] < sizeof head || !recv_all(head, sizeof head)) { coop_fail("turn"); return 0; }
            len = head[3];
            if (len > CMD_BYTES || hdr[1] != sizeof head + len) { coop_fail("turn size"); return 0; }
            EnterCriticalSection(&s_lock);
            t = &s_remote[head[0] % TURN_RING];
            t->turn = head[0];
            t->hash = (uint64_t)head[1] | ((uint64_t)head[2] << 32);
            t->len = len;
            if (len && !recv_all(t->data, (int)len)) { LeaveCriticalSection(&s_lock); coop_fail("turn data"); return 0; }
            t->have = 1;
            LeaveCriticalSection(&s_lock);
            {
                static int told;
                if (told++ < 4) fprintf(stderr, "[COOP] got turn %u (%u bytes)\n", head[0], len);
            }
        } else {
            coop_fail("unknown message");
            return 0;
        }
    }
}

static void coop_start_net(void)
{
    static int done;
    const char *e;
    if (done) return;
    done = 1;
    e = getenv("AVP_COOP");
    if (!e || !*e) return;
    if (!strncmp(e, "host", 4)) s_role = ROLE_HOST;
    else if (!strncmp(e, "join:", 5)) s_role = ROLE_GUEST;
    else return;
    InitializeCriticalSection(&s_lock);
    s_net_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, net_thread, (LPVOID)e, 0, NULL);
    fprintf(stderr, "[COOP] %s\n", s_role == ROLE_HOST ? "host" : "guest");
}

static void send_turn(uint32_t turn, uint64_t hash)
{
    uint32_t head[4];
    Turn *t = &s_local[turn % TURN_RING];
    EnterCriticalSection(&s_lock);
    t->turn = turn;
    t->len = s_npending;
    memcpy(t->data, s_pending, s_npending);
    s_npending = 0;
    t->have = 1;
    LeaveCriticalSection(&s_lock);
    head[0] = turn;
    head[1] = (uint32_t)hash;
    head[2] = (uint32_t)(hash >> 32);
    head[3] = t->len;
    {
        uint8_t buf[sizeof head + CMD_BYTES];
        memcpy(buf, head, sizeof head);
        memcpy(buf + sizeof head, t->data, t->len);
        if (!send_msg(MSG_TURN, buf, (uint32_t)(sizeof head + t->len)))
            coop_fail("send");
        {
            static int told;
            if (told++ < 4) fprintf(stderr, "[COOP] sent turn %u\n", turn);
        }
    }
}

#define RUN_BYTES (CMD_BYTES * 4)

static uint8_t s_run[RUN_BYTES];
static uint32_t s_nrun;
static int s_local_input, s_executing;

static void run_add(int from_host, const Turn *t)
{
    uint32_t o = 0;
    while (o + 4 <= t->len) {
        uint32_t n;
        memcpy(&n, t->data + o, 4);
        if (o + 4 + n > t->len || s_nrun + 8 + n > RUN_BYTES) break;
        memcpy(s_run + s_nrun, &from_host, 4);
        memcpy(s_run + s_nrun + 4, &n, 4);
        memcpy(s_run + s_nrun + 8, t->data + o + 4, n);
        s_nrun += 8 + n;
        o += 4 + n;
    }
}

static void run_cmds(int from_host, const Turn *t)
{
    run_add(from_host, t);
}

enum { CMD_ORDER = 1, CMD_GIVE = 2 };

#define AVP_MSG_TYPES 0x002891F8u
#define AVP_POINTER   0x002F6AECu
#define ORDER_MAX     900u

static uint8_t s_owner[0x10000];
static uint32_t s_owner_h[0x10000];

extern void avp_run_order(int route, uint32_t handle, const uint8_t *msg, uint32_t len);

static int local_player(void)
{
    return s_role == ROLE_GUEST ? 1 : 0;
}

static int owner_of(uint32_t handle)
{
    uint32_t i = handle >> 16;
    return s_owner_h[i] == handle ? s_owner[i] : 0;
}

static int coop_live(void)
{
    return s_role != ROLE_NONE && !s_coop_dead && s_mission;
}

enum { CMD_SELECT = 3, CMD_MOVE = 4 };

#define AVP_PTR_GLOBAL2  0x002F67F4u
#define AVP_WORLD        0x002F6AE4u
#define AVP_FACTORY      0x00027690u
#define AVP_SELECT_FN    0x000F0070u
#define AVP_MOVE_FN      0x0002D9F0u
#define AVP_NOWHERE      0x00304EDCu
#define PTR_BYTES        0x280u
#define SEL_MAX          64

extern uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax);
extern uint32_t avp_guest_alloca(uint32_t bytes);
extern uint32_t avp_guest_esp(void);
extern void avp_guest_set_esp(uint32_t v);

static void hide_pointer(uint32_t p);
static uint32_t s_ptr[2];
static uint32_t s_sel_pending[SEL_MAX];
static int s_nsel_pending;

static uint32_t player_pointer(int player)
{
    uint32_t h = s_ptr[player], obj;
    if (!h) return 0;
    obj = G32(AVP_OBJ_TABLE + (h >> 16) * 4u);
    return obj && G32(obj + 0x18) == h ? obj : 0;
}

static void make_pointers(void)
{
    uint32_t world = G32(AVP_WORLD), pm, a[2], b;
    s_ptr[0] = G32(AVP_POINTER) ? G32(G32(AVP_POINTER) + 0x18) : 0;
    pm = world ? G32(world + 0x78) : 0;
    if (!pm || !G32(pm + 0x19C)) { fprintf(stderr, "[COOP] no player manager for a second pointer\n"); return; }
    a[0] = G32(pm + 0x19C);
    a[1] = G32(pm + 0x18) & 0xFFFFu;
    b = avp_gcall(AVP_FACTORY, 0, a, 2, 0);
    if (!b) { fprintf(stderr, "[COOP] could not create player 2's pointer\n"); return; }
    s_ptr[1] = G32(b + 0x18);
    hide_pointer(b);
    fprintf(stderr, "[COOP] pointers: host %08X, player 2 %08X (object %08X)\n", s_ptr[0], s_ptr[1], b);
}

static void hide_pointer(uint32_t p)
{
    if (!p) return;
    GF(p + 0x40) = -10000.0f;
    GF(p + 0x44) = -10000.0f;
}

static uint32_t swap_pointer(uint32_t p)
{
    uint32_t old = G32(AVP_POINTER);
    G32(AVP_POINTER) = p;
    G32(AVP_PTR_GLOBAL2) = p;
    return old;
}

static void run_select(int player, const uint32_t *h, uint32_t n)
{
    uint32_t p = player_pointer(player), old, i, a[2];
    if (!p) return;
    old = swap_pointer(p);
    avp_gcall(G32(G32(p) + 0xB4), p, NULL, 0, 0);
    for (i = 0; i < n; i++) {
        if (owner_of(h[i]) != player) continue;
        a[0] = p;
        a[1] = h[i];
        avp_gcall(AVP_SELECT_FN, p, a, 2, 0);
    }
    swap_pointer(old);
}

static int ptr_keep(uint32_t off)
{
    return off < 0x1C || (off >= 0x1B8 && off < 0x1C0) || (off >= 0x270 && off < 0x278);
}

enum { ARG_RAW = 0, ARG_PTRFIELD = 1, ARG_BLOB = 2 };
#define ARG_BLOB_MAX 0x80u

static void run_move(int player, const uint8_t *d, uint32_t n)
{
    uint32_t p = player_pointer(player), old, off, a[4], esp, o = 0, i;
    if (!p || n < PTR_BYTES) return;
    for (off = 0; off < PTR_BYTES; off += 4) {
        uint32_t v;
        if (ptr_keep(off)) continue;
        memcpy(&v, d + off, 4);
        G32(p + off) = v;
    }
    o = PTR_BYTES;
    old = swap_pointer(p);
    esp = avp_guest_esp();
    a[0] = p;
    for (i = 1; i < 4; i++) {
        uint32_t kind, val, len;
        if (o + 12 > n) { avp_guest_set_esp(esp); swap_pointer(old); return; }
        memcpy(&kind, d + o, 4);
        memcpy(&val, d + o + 4, 4);
        memcpy(&len, d + o + 8, 4);
        o += 12;
        if (kind == ARG_PTRFIELD) {
            a[i] = p + val;
        } else if (kind == ARG_BLOB && len <= ARG_BLOB_MAX && o + len <= n) {
            uint32_t buf = avp_guest_alloca(len), k;
            for (k = 0; k < len; k++) G8(buf + k) = d[o + k];
            a[i] = buf + val;
            o += len;
        } else {
            a[i] = val;
        }
    }
    avp_gcall(AVP_MOVE_FN, p, a, 4, 0);
    avp_guest_set_esp(esp);
    swap_pointer(old);
    if (player == 1) hide_pointer(p);
}

int avp_coop_marked(uint32_t handle)
{
    return coop_live() && owner_of(handle) == 1;
}

int avp_coop_may_select(uint32_t handle)
{
    if (!coop_live() || s_executing) return 1;
    if (s_local_input) {
        if (owner_of(handle) == local_player() && s_nsel_pending < SEL_MAX)
            s_sel_pending[s_nsel_pending++] = handle;
        return 0;
    }
    return owner_of(handle) == local_player();
}

int avp_coop_take_move(uint32_t esp)
{
    uint8_t buf[4 + PTR_BYTES + 3 * (12 + ARG_BLOB_MAX)];
    uint32_t ptr, off, o, i, cmd = CMD_MOVE, type = 0;
    if (!coop_live() || s_executing || !s_local_input) return 0;
    ptr = G32(esp + 4);
    if (!ptr || ptr < 0x10000u || ptr >= 0x84000000u) return 0;
    memcpy(buf, &cmd, 4);
    for (off = 0; off < PTR_BYTES; off += 4) {
        uint32_t v = G32(ptr + off);
        memcpy(buf + 4 + off, &v, 4);
    }
    o = 4 + PTR_BYTES;
    for (i = 1; i < 4; i++) {
        uint32_t v = G32(esp + 4 + 4 * i), kind = ARG_RAW, val = v, len = 0;
        if (v >= ptr && v < ptr + PTR_BYTES) {
            kind = ARG_PTRFIELD;
            val = v - ptr;
        } else if (v >= esp - 0x1000u && v < esp + 0x1000u) {
            uint32_t base = v & ~3u, k;
            kind = ARG_BLOB;
            len = ARG_BLOB_MAX;
            val = v - base;
            for (k = 0; k < len; k++) buf[o + 12 + k] = G8(base + k);
            if (i == 1) type = G32(v);
        }
        memcpy(buf + o, &kind, 4);
        memcpy(buf + o + 4, &val, 4);
        memcpy(buf + o + 8, &len, 4);
        o += 12 + len;
    }
    if (!avp_coop_queue(buf, o)) return 0;
    {
        static int told;
        if (told++ < 10)
            fprintf(stderr, "[COOP] move sent: type %08X args %08X %08X %08X (esp %08X, pointer %08X)\n", type,
                    G32(esp + 8), G32(esp + 12), G32(esp + 16), esp, ptr);
    }
    return 1;
}

static uint32_t s_fx_rng[2] = { 0x13579BDFu, 0x2468ACE0u };
static uint32_t s_fx_saved[2];
static int s_fx_depth;

static uint32_t s_fx_callers[16] = { 0x0005D413u };
static int s_fx_ncallers = 1;

void avp_coop_fx_rng(int enter, uint32_t obj)
{
    int i;
    if (!s_fixed) return;
    {
        static uint8_t swapped[64];
        static int sp;
        if (enter) {
            int sw = obj >= 0x000F9000u && obj < 0x000FD000u;
            for (i = 0; i < s_fx_ncallers; i++)
                if (s_fx_callers[i] == obj) sw = 1;
            if (sw && s_fx_depth++ == 0) {
                s_fx_saved[0] = G32(AVP_RNG_ACC);
                s_fx_saved[1] = G32(AVP_RNG_SEED);
                G32(AVP_RNG_ACC) = s_fx_rng[0];
                G32(AVP_RNG_SEED) = s_fx_rng[1];
            }
            if (sp < 64) swapped[sp] = (uint8_t)sw;
            sp++;
        } else if (sp > 0) {
            sp--;
            if (sp < 64 && swapped[sp] && s_fx_depth > 0 && --s_fx_depth == 0) {
                s_fx_rng[0] = G32(AVP_RNG_ACC);
                s_fx_rng[1] = G32(AVP_RNG_SEED);
                G32(AVP_RNG_ACC) = s_fx_saved[0];
                G32(AVP_RNG_SEED) = s_fx_saved[1];
            }
        }
    }
}

void avp_coop_rng_site(uint32_t loc)
{
    static int lo = -1, hi;
    uint64_t t;
    if (lo < 0) {
        const char *e = getenv("AVP_RNGSITE");
        lo = 190; hi = 240;
        if (e && *e) sscanf(e, "%d-%d", &lo, &hi);
    }
    if (!s_mission) return;
    t = s_tick - s_mission_tick - 1;
    if ((int64_t)t >= lo && (int64_t)t <= hi) {
        fprintf(stderr, "[RNGSITE] %llu %08X %08X%s\n", (unsigned long long)t, loc, G32(AVP_RNG_SEED),
                s_local_input ? " input" : "");
        if (getenv("AVP_RNGSTACK") && (loc == 0xC005CF6Cu || loc == 0xC003930Bu)) {
            extern uint32_t avp_guest_esp(void);
            uint32_t sp = avp_guest_esp(), k, shown = 0;
            fprintf(stderr, "[RNGSTACK] %llu %08X:", (unsigned long long)t, loc);
            for (k = 0; k < 0x400 && shown < 12; k += 4) {
                uint32_t v = G32(sp + k);
                if (v >= 0x00011000u && v < 0x00170000u) { fprintf(stderr, " %08X", v); shown++; }
            }
            fprintf(stderr, "\n");
        }
    }
}

void avp_coop_visit(uint32_t obj)
{
    static int want = -2;
    if (want == -2) {
        const char *e = getenv("AVP_VISIT_TICK");
        want = e && *e ? atoi(e) : -1;
    }
    if (want < 0 || !s_mission) return;
    if ((int64_t)(s_tick - s_mission_tick - 1) == want)
        fprintf(stderr, "[VISIT] %08X %08X %08X\n", obj >= 0x10000u && obj < 0x84000000u ? G32(obj + 0x18) : 0,
                obj >= 0x10000u && obj < 0x84000000u ? G32(obj) : 0, obj);
}

static void rng_save(void)
{
    if (!coop_live()) return;
    s_rng_save[0] = G32(AVP_RNG_ACC);
    s_rng_save[1] = G32(AVP_RNG_SEED);
    s_rng_saved = 1;
}

static void rng_restore(void)
{
    if (!s_rng_saved) return;
    G32(AVP_RNG_ACC) = s_rng_save[0];
    G32(AVP_RNG_SEED) = s_rng_save[1];
    s_rng_saved = 0;
}

void avp_coop_input_begin(void)
{
    uint32_t o = 0;
    s_local_input = 1;
    if (!s_nrun) { rng_save(); return; }
    s_executing = 1;
    while (o + 8 <= s_nrun) {
        uint32_t from_host, n;
        const uint8_t *d;
        memcpy(&from_host, s_run + o, 4);
        memcpy(&n, s_run + o + 4, 4);
        d = s_run + o + 8;
        if (n >= 4) {
            uint32_t kind;
            memcpy(&kind, d, 4);
            if (kind == CMD_ORDER && n >= 16) {
                uint32_t route, h, len;
                memcpy(&route, d + 4, 4);
                memcpy(&h, d + 8, 4);
                memcpy(&len, d + 12, 4);
                if (len <= n - 16 && (route == 3 || owner_of(h) == (from_host ? 0 : 1)))
                    avp_run_order((int)route, h, d + 16, len);
            } else if (kind == CMD_GIVE && from_host && n >= 8) {
                uint32_t cnt, i, given = 0;
                memcpy(&cnt, d + 4, 4);
                for (i = 0; i < cnt && 8 + 4 * (i + 1) <= n; i++) {
                    uint32_t h;
                    memcpy(&h, d + 8 + 4 * i, 4);
                    if (owner_of(h) != 0) continue;
                    s_owner_h[h >> 16] = h;
                    s_owner[h >> 16] = 1;
                    given++;
                }
                fprintf(stderr, "[COOP] %u unit%s given to player 2\n", given, given == 1 ? "" : "s");
            } else if (kind == CMD_SELECT && n >= 8) {
                uint32_t cnt, hs[SEL_MAX];
                memcpy(&cnt, d + 4, 4);
                if (cnt <= SEL_MAX && 8 + 4 * cnt <= n) {
                    memcpy(hs, d + 8, 4 * cnt);
                    run_select(from_host ? 0 : 1, hs, cnt);
                }
            } else if (kind == CMD_MOVE && n > 4) {
                run_move(from_host ? 0 : 1, d + 4, n - 4);
            }
        }
        o += 8 + n;
    }
    s_nrun = 0;
    s_executing = 0;
    rng_save();
}

void avp_coop_input_end(void)
{
    s_local_input = 0;
    rng_restore();
    if (s_nsel_pending && coop_live()) {
        uint32_t buf[2 + SEL_MAX];
        buf[0] = CMD_SELECT;
        buf[1] = (uint32_t)s_nsel_pending;
        memcpy(buf + 2, s_sel_pending, 4u * (uint32_t)s_nsel_pending);
        avp_coop_queue(buf, 8u + 4u * (uint32_t)s_nsel_pending);
        fprintf(stderr, "[COOP] selection of %d unit%s sent\n", s_nsel_pending, s_nsel_pending == 1 ? "" : "s");
    }
    s_nsel_pending = 0;
}

int avp_coop_take_route(int kind, uint32_t unit, uint32_t msg)
{
    uint32_t type, entry, size, h, pkt[4];
    uint8_t buf[16 + ORDER_MAX];
    if (!coop_live() || s_executing || !s_local_input || !unit || !msg) return 0;
    h = G32(unit + 0x18);
    {
        static int told;
        if (told++ < 20)
            fprintf(stderr, "[COOP] local SendCommand type %08X unit %08X owner %d (we are %d)\n", G32(msg), h,
                    owner_of(h), local_player());
    }
    if (kind != 1 || owner_of(h) != local_player()) return 1;
    type = G32(msg);
    entry = AVP_MSG_TYPES + (type & 0xFFu) * 12u;
    if (!G32(entry)) {
        static int told;
        if (told++ < 5) fprintf(stderr, "[COOP] order type %08X has no size; runs here only\n", type);
        return 0;
    }
    size = G32(entry + 4);
    if (size == 0xFFFFFFFFu) size = G32(msg + 0xC) & 0xFFFFu;
    if (size < 4 || size > ORDER_MAX) return 0;
    pkt[0] = CMD_ORDER;
    pkt[1] = (uint32_t)kind;
    pkt[2] = h;
    pkt[3] = size;
    memcpy(buf, pkt, sizeof pkt);
    {
        uint32_t i;
        for (i = 0; i < size; i++) buf[16 + i] = G8(msg + i);
    }
    if (!avp_coop_queue(buf, 16 + size)) return 0;
    {
        static int told;
        if (told++ < 10) fprintf(stderr, "[COOP] order %08X route %d for %08X (%u bytes) sent\n", type, kind, h, size);
    }
    return 1;
}

int avp_coop_take_order(uint32_t unit, uint32_t msg)
{
    return avp_coop_take_route(1, unit, msg);
}

static int unit_handle(uint32_t v, uint32_t *out);

static int sel_handle(uint32_t v, uint32_t *out)
{
    uint32_t o;
    static int told;
    if (v < 0x10000u || v >= 0x84000000u) return 0;
    for (o = 4; o <= 0x60; o += 4) {
        uint32_t f = G32(v + o);
        if (f == v) continue;
        if (unit_handle(f, out) && *out != G32(v + 0x18)) {
            if (told++ < 2) fprintf(stderr, "[COOP] selection ring %08X: unit at +%02X\n", v, o);
            return 1;
        }
    }
    return 0;
}

static int unit_handle(uint32_t v, uint32_t *out)
{
    uint32_t obj;
    if (v >= 0x10000u && v < 0x84000000u && (G32(v + 0x18) >> 16) && G32(AVP_OBJ_TABLE + (G32(v + 0x18) >> 16) * 4u) == v) {
        *out = G32(v + 0x18);
        return 1;
    }
    obj = G32(AVP_OBJ_TABLE + (v >> 16) * 4u);
    if ((v >> 16) && obj >= 0x10000u && obj < 0x84000000u && G32(obj + 0x18) == v) {
        *out = v;
        return 1;
    }
    return 0;
}

int avp_coop_give_selection(void)
{
    uint32_t ptr, sel, n, data, i, buf[2 + 64], cnt = 0;
    if (!coop_live() || s_role != ROLE_HOST) return 0;
    ptr = G32(AVP_POINTER);
    sel = ptr ? G32(ptr + 0x1B8) : 0;
    if (!sel) return 0;
    data = G32(sel);
    n = G32(sel + 4);
    {
        static int told;
        if (told++ < 2) {
            fprintf(stderr, "[COOP] selection list %08X: %08X %08X %08X %08X\n", sel, G32(sel), G32(sel + 4),
                    G32(sel + 8), G32(sel + 12));
            for (i = 0; data && i < 3 && i < n; i++) {
                uint32_t v = G32(data + i * 4u);
                fprintf(stderr, "[COOP]   item %u: %08X -> %08X %08X %08X %08X %08X %08X %08X\n", i, v,
                        v >= 0x10000u && v < 0x84000000u ? G32(v) : 0, v >= 0x10000u && v < 0x84000000u ? G32(v + 4) : 0,
                        v >= 0x10000u && v < 0x84000000u ? G32(v + 8) : 0, v >= 0x10000u && v < 0x84000000u ? G32(v + 0xC) : 0,
                        v >= 0x10000u && v < 0x84000000u ? G32(v + 0x10) : 0, v >= 0x10000u && v < 0x84000000u ? G32(v + 0x14) : 0,
                        v >= 0x10000u && v < 0x84000000u ? G32(v + 0x18) : 0);
            }
        }
    }
    if (n > 64 || !data) return 0;
    for (i = 0; i < n; i++) {
        uint32_t h;
        if (sel_handle(G32(data + i * 4u), &h) && owner_of(h) == 0)
            buf[2 + cnt++] = h;
    }
    if (!cnt) return 0;
    buf[0] = CMD_GIVE;
    buf[1] = cnt;
    avp_coop_queue(buf, 8 + cnt * 4);
    fprintf(stderr, "[COOP] giving %u selected unit%s to player 2\n", cnt, cnt == 1 ? "" : "s");
    return (int)cnt;
}

static void coop_turn(uint32_t turn)
{
    static uint32_t comp[TURN_RING][3];
    uint64_t h = core_hash();
    ULONGLONG t0;
    Turn *r, *l;
    s_hashes[turn % TURN_RING] = h;
    comp[turn % TURN_RING][0] = G32(AVP_CLOCK);
    comp[turn % TURN_RING][1] = G32(AVP_RNG_ACC);
    comp[turn % TURN_RING][2] = G32(AVP_RNG_SEED);
    send_turn(turn + TURN_DELAY, h);
    if (turn < TURN_DELAY) return;
    t0 = GetTickCount64();
    for (;;) {
        int ok;
        EnterCriticalSection(&s_lock);
        r = &s_remote[turn % TURN_RING];
        ok = r->have && r->turn == turn;
        LeaveCriticalSection(&s_lock);
        if (ok || s_coop_dead) break;
        if (GetTickCount64() - t0 > WAIT_MS) {
            fprintf(stderr, "[COOP] waited for turn %u; slot holds turn %u have %d\n", turn, r->turn, r->have);
            coop_fail("the other player stopped answering");
            return;
        }
        Sleep(1);
    }
    if (s_coop_dead) return;
    l = &s_local[turn % TURN_RING];
    if (r->hash != s_hashes[(turn - TURN_DELAY) % TURN_RING]) {
        static int told;
        if (told++ < 5) {
            uint32_t *c = comp[(turn - TURN_DELAY) % TURN_RING];
            fprintf(stderr, "[COOP] desync: turn %u, ours %016llX, theirs %016llX; our clock %08X rng %08X %08X\n",
                    turn - TURN_DELAY, (unsigned long long)s_hashes[(turn - TURN_DELAY) % TURN_RING],
                    (unsigned long long)r->hash, c[0], c[1], c[2]);
        }
    }
    if (s_role == ROLE_HOST) { run_cmds(1, l); run_cmds(0, r); }
    else { run_cmds(1, r); run_cmds(0, l); }
    r->have = 0;
}

static void coop_tick(void)
{
    uint64_t t;
    if (s_tick % 300u == 0)
        fprintf(stderr, "[SYNC] heartbeat tick %llu clock %.3f real %llu ms\n",
                (unsigned long long)s_tick, (double)GF(AVP_CLOCK), (unsigned long long)GetTickCount64());
    if (!s_mission) return;
    t = s_tick - s_mission_tick - 1;
    if ((int64_t)t == s_dump_tick)
        sync_report(1);
    else if (s_log_every && t % (uint64_t)s_log_every == 0)
        sync_report(0);
    if (s_role != ROLE_NONE && !s_coop_dead && t % TURN_TICKS == 0)
        coop_turn((uint32_t)(t / TURN_TICKS));
}

static uint32_t sync_page_count(void)
{
    uint32_t n = 0, r;
    for (r = 0; r < sizeof k_sync_ranges / sizeof k_sync_ranges[0]; r++)
        n += (k_sync_ranges[r][1] - k_sync_ranges[r][0]) / PAGE;
    return n;
}

static uint32_t sync_page_addr(uint32_t i)
{
    uint32_t r;
    for (r = 0; r < sizeof k_sync_ranges / sizeof k_sync_ranges[0]; r++) {
        uint32_t n = (k_sync_ranges[r][1] - k_sync_ranges[r][0]) / PAGE;
        if (i < n) return k_sync_ranges[r][0] + i * PAGE;
        i -= n;
    }
    return 0;
}

static uint8_t *guest_page(uint32_t a)
{
    return (uint8_t *)((uintptr_t)a + (uintptr_t)g_xbox_mem_offset);
}

static uint64_t page_hash(uint32_t a)
{
    return fnv(0xCBF29CE484222325ull, guest_page(a), PAGE);
}

static uint8_t *wait_bulk(int type, uint32_t *len)
{
    ULONGLONG t0 = GetTickCount64();
    while (!s_bulk[type] && !s_coop_dead) {
        if (GetTickCount64() - t0 > 60000) { coop_fail("state copy timed out"); return NULL; }
        Sleep(2);
    }
    if (s_coop_dead) return NULL;
    *len = s_bulk_len[type];
    return s_bulk[type];
}

static void state_sync(void)
{
    uint32_t n = sync_page_count(), i, len;
    if (s_role == ROLE_GUEST) {
        uint64_t *hashes = (uint64_t *)malloc((size_t)n * 8u);
        uint8_t *b;
        uint32_t applied = 0, still = 0, o;
        if (!hashes) { coop_fail("memory"); return; }
        for (i = 0; i < n; i++) hashes[i] = page_hash(sync_page_addr(i));
        if (!send_msg(MSG_PAGEHASH, hashes, n * 8u)) { free(hashes); coop_fail("send hashes"); return; }
        b = wait_bulk(MSG_PAGES, &len);
        if (!b || len < 12) { free(hashes); return; }
        memcpy(&s_vcount, b, 8);
        memcpy(&applied, b + 8, 4);
        for (o = 12, i = 0; i < applied && o + 4 + PAGE <= len; i++, o += 4 + PAGE) {
            uint32_t a;
            memcpy(&a, b + o, 4);
            memcpy(guest_page(a), b + o + 4, PAGE);
        }
        free(b);
        s_bulk[MSG_PAGES] = NULL;
        for (i = 0; i < n; i++) if (page_hash(sync_page_addr(i)) != hashes[i]) still++;
        free(hashes);
        fprintf(stderr, "[COOP] state copied from the host: %u pages (%u KB); %u pages changed\n",
                applied, applied * 4u, still);
    } else {
        uint8_t *b = wait_bulk(MSG_PAGEHASH, &len), *out;
        uint32_t diff = 0, o = 12;
        if (!b) return;
        if (len != n * 8u) { free(b); coop_fail("the other game has a different memory layout"); return; }
        out = (uint8_t *)malloc(12u + (size_t)n * (4u + PAGE));
        if (!out) { free(b); coop_fail("memory"); return; }
        for (i = 0; i < n; i++) {
            uint32_t a = sync_page_addr(i);
            uint64_t theirs;
            memcpy(&theirs, b + (size_t)i * 8u, 8);
            if (page_hash(a) == theirs) continue;
            memcpy(out + o, &a, 4);
            memcpy(out + o + 4, guest_page(a), PAGE);
            o += 4 + PAGE;
            diff++;
        }
        free(b);
        s_bulk[MSG_PAGEHASH] = NULL;
        memcpy(out, &s_vcount, 8);
        memcpy(out + 8, &diff, 4);
        if (!send_msg(MSG_PAGES, out, o)) coop_fail("send pages");
        free(out);
        fprintf(stderr, "[COOP] state sent to the guest: %u pages (%u KB)\n", diff, diff * 4u);
    }
}

static void wait_peer_start(void)
{
    ULONGLONG t0 = GetTickCount64();
    if (s_role == ROLE_NONE || s_coop_dead) return;
    WaitForSingleObject(s_net_ready, 120000);
    if (s_coop_dead) return;
    if (!send_msg(MSG_START, NULL, 0)) { coop_fail("send start"); return; }
    fprintf(stderr, "[COOP] mission ready, waiting for the other player\n");
    while (!s_peer_started && !s_coop_dead) {
        if (GetTickCount64() - t0 > 120000) { coop_fail("the other player never started"); return; }
        Sleep(5);
    }
    if (!s_coop_dead && !getenv("AVP_NO_STATE_SYNC"))
        state_sync();
    if (!s_coop_dead)
        make_pointers();
    fprintf(stderr, "[COOP] both players in, lockstep on\n");
}

void avp_coop_mission_start(void)
{
    const char *e;
    coop_init();
    if (!s_fixed || s_mission) return;
    e = getenv("AVP_RNG_SEED");
    if (e && *e) {
        uint32_t v = (uint32_t)strtoul(e, NULL, 16);
        G32(AVP_RNG_SEED) = v;
        G32(AVP_RNG_ACC) = v ^ 0x5A5A5A5Au;
    }
    if (s_rng_saved) {
        s_rng_save[0] = G32(AVP_RNG_ACC);
        s_rng_save[1] = G32(AVP_RNG_SEED);
    }
    wait_peer_start();
    if (getenv("AVP_PAGE_DUMP")) {
        uint32_t a;
        for (a = 0; a < 0x04000000u; a += 0x1000u) {
            const uint8_t *p = (const uint8_t *)((uintptr_t)a + (uintptr_t)g_xbox_mem_offset);
            fprintf(stderr, "[PAGE] %08X %016llX\n", a, (unsigned long long)fnv(0xCBF29CE484222325ull, p, 0x1000u));
        }
    }
    s_mission = 1;
    s_mission_tick = s_tick;
    fprintf(stderr, "[SYNC] mission start at tick %llu\n", (unsigned long long)s_tick);
    sync_report(0);
}

static uint64_t s_reads_since_tick;

static int real_time_reader(uint32_t ret)
{
    return ret == 0x0012DFA6u || ret == 0x0012E03Au || ret == AVP_WAIT_START || ret == AVP_WAIT_POLL
        || (ret >= 0x00066000u && ret < 0x0006C000u)
        || (ret >= 0x0017C000u && ret < 0x0017D000u)
        || (ret >= 0x001D6000u && ret < 0x001D7000u);
}

static struct { uint32_t ret, n; } s_stall[64];
static int s_nstall, s_stall_told;
static ULONGLONG s_tick_ms;

static void stall_note(uint32_t ret)
{
    int i;
    ULONGLONG now = GetTickCount64();
    if (ret == AVP_TIMER_TICK) {
        s_nstall = 0;
        s_stall_told = 0;
        s_tick_ms = now;
        return;
    }
    for (i = 0; i < s_nstall && s_stall[i].ret != ret; i++) ;
    if (i == s_nstall && s_nstall < 64) { s_stall[i].ret = ret; s_stall[i].n = 0; s_nstall++; }
    if (i < 64) s_stall[i].n++;
    if (!s_stall_told && s_tick_ms && now - s_tick_ms > 3000) {
        s_stall_told = 1;
        fprintf(stderr, "[SYNC] no tick for 3 s after tick %llu; counter reads since:\n", (unsigned long long)s_tick);
        for (i = 0; i < s_nstall; i++)
            fprintf(stderr, "[SYNC]   %08X x%u (thread %lu)\n", s_stall[i].ret, s_stall[i].n, GetCurrentThreadId());
    }
}

uint64_t avp_coop_tsc(uint32_t esp, uint64_t real)
{
    uint32_t ret = G32(esp);
    coop_init();
    if (!s_fixed)
        return real;
    if (!s_real_at_tick)
        s_real_at_tick = real;
    stall_note(ret);
    if (s_mission && ret != AVP_TIMER_TICK) {
        static struct { uint32_t ret, n; } seen[48];
        static int nseen, dumped;
        uint64_t t = s_tick - s_mission_tick - 1;
        int i;
        if (t <= 120) {
            for (i = 0; i < nseen && seen[i].ret != ret; i++) ;
            if (i == nseen && nseen < 48) { seen[i].ret = ret; seen[i].n = 0; nseen++; }
            if (i < 48) seen[i].n++;
        } else if (!dumped) {
            dumped = 1;
            for (i = 0; i < nseen; i++)
                fprintf(stderr, "[TSCREAD] %08X x%u\n", seen[i].ret, seen[i].n);
        }
    }
    if (ret < AVP_TIMER_LO || ret >= AVP_TIMER_HI || ret == AVP_WAIT_START || ret == AVP_WAIT_POLL) {
        if (!s_mission || real_time_reader(ret))
            return s_vcount + (real - s_real_at_tick);
        s_reads_since_tick++;
        return s_vcount + s_reads_since_tick * ((s_step ? s_step : 12222222u) / 64u);
    }
    if (!s_step) {
        float scale = GF(AVP_CLOCK_SCALE);
        if (scale > 0.0f)
            s_step = (uint64_t)(1.0 / ((double)s_hz * (double)scale) + 0.5);
    }
    if (ret == AVP_TIMER_TICK) {
        s_vcount += s_step ? s_step : 12222222u;
        s_real_at_tick = real;
        s_reads_since_tick = 0;
        s_tick++;
        coop_tick();
    }
    return s_vcount;
}

