#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define AVP_SEND_FLAGS 0
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
typedef int SOCKET;
typedef unsigned long u_long;
typedef unsigned short u_short;
#define INVALID_SOCKET (-1)
#define closesocket close
#define WSAGetLastError() errno
#define WSAEADDRINUSE EADDRINUSE
#define WSAEACCES EACCES
#define SD_BOTH SHUT_RDWR
#define FIONBIO 1
#define AVP_SEND_FLAGS MSG_NOSIGNAL
static int ioctlsocket(SOCKET s, long cmd, u_long *arg)
{
    int fl = fcntl(s, F_GETFL, 0);
    (void)cmd;
    if (fl < 0)
        return -1;
    return fcntl(s, F_SETFL, *arg ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
#endif
#include "avp_platform.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avp_skirmish.h"
#include "avp_coop.h"

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
static int s_env_fixed, s_rebase, s_mission_seen;
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
    s_env_fixed = s_fixed;
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

#define LOCAL_LO 0x03082000u
#define LOCAL_HI 0x04000000u
#define LIB_MAX  4096u

typedef struct { uint32_t lo, hi; } Span;

static const Span k_lib_globals[] = {
    { 0x0026516Cu, 0x00265170u },
    { 0x00269408u, 0x0026A39Cu },
    { 0x00275D64u, 0x00275D68u },
    { 0x00285348u, 0x00285358u },
    { 0x00288354u, 0x002885E0u },
    { 0x0028C3FCu, 0x0028C400u },
    { 0x002E7264u, 0x002E7268u },
    { 0x002FE000u, 0x002FFDACu },
    { 0x002FFE08u, 0x00301014u },
    { 0x0030BDA0u, 0x0030BDF0u },
    { 0x0030BEDCu, 0x003102E4u },
};

static const struct { uint32_t lo, hi; uint8_t id; } k_lib_code[] = {
    { 0x00170D60u, 0x001836C0u, 1 },
    { 0x001836C0u, 0x0019F580u, 2 },
    { 0x0019F580u, 0x001BC700u, 3 },
    { 0x001BC700u, 0x001D5920u, 4 },
    { 0x001D5920u, 0x001FD980u, 5 },
    { 0x001FD980u, 0x002050C0u, 6 },
};

extern void xbox_HeapSetTagHook(uint8_t (*hook)(uint32_t caller));
extern uint32_t xbox_HeapTagGeneration(void);
extern uint32_t xbox_HeapEnumTagged(uint32_t *out, uint32_t max);

static Span s_lib_own[LIB_MAX], s_lib_peer[LIB_MAX];
static uint32_t s_lib_own_n, s_lib_peer_n, s_lib_gen = 0xFFFFFFFFu;
static uint32_t s_lib_triples[LIB_MAX * 3u];

static uint8_t lib_heap_tag(uint32_t caller)
{
    uint32_t i;
    for (i = 0; i < sizeof k_lib_code / sizeof k_lib_code[0]; i++)
        if (caller >= k_lib_code[i].lo && caller < k_lib_code[i].hi) return k_lib_code[i].id;
    return 0;
}

void avp_coop_install_heap_hook(void)
{
    xbox_HeapSetTagHook(lib_heap_tag);
}

static int span_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Span *)a)->lo, y = ((const Span *)b)->lo;
    return x < y ? -1 : x > y;
}

static uint32_t span_merge(Span *s, uint32_t n)
{
    uint32_t i, o = 0;
    if (!n) return 0;
    qsort(s, n, sizeof *s, span_cmp);
    for (i = 1; i < n; i++) {
        if (s[i].lo <= s[o].hi) {
            if (s[i].hi > s[o].hi) s[o].hi = s[i].hi;
        } else {
            s[++o] = s[i];
        }
    }
    return o + 1;
}

static int span_has(const Span *s, uint32_t n, uint32_t a)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a < s[mid].lo) hi = mid;
        else if (a >= s[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

static int lib_static(uint32_t a)
{
    uint32_t i;
    for (i = 0; i < sizeof k_lib_globals / sizeof k_lib_globals[0]; i++)
        if (a >= k_lib_globals[i].lo && a < k_lib_globals[i].hi) return 1;
    return 0;
}

static void lib_refresh(void)
{
    uint32_t gen = xbox_HeapTagGeneration(), n, i;
    if (gen == s_lib_gen) return;
    n = xbox_HeapEnumTagged(s_lib_triples, LIB_MAX);
    if (n >= LIB_MAX) fprintf(stderr, "[COOP] more than %u library heap blocks, the rest are not protected\n", LIB_MAX);
    for (i = 0; i < n; i++) {
        s_lib_own[i].lo = s_lib_triples[i * 3u];
        s_lib_own[i].hi = s_lib_triples[i * 3u] + s_lib_triples[i * 3u + 1u];
    }
    s_lib_own_n = span_merge(s_lib_own, n);
    s_lib_gen = gen;
}

static int lib_own_addr(uint32_t a)
{
    return lib_static(a) || span_has(s_lib_own, s_lib_own_n, a);
}

static int proc_local(uint32_t w)
{
    if (w < 0x10000u) return 0;
    if (w >= LOCAL_LO && w < LOCAL_HI) return 1;
    if (w >= 0x80000000u && w < 0x84000000u) return 1;
    if (w >= 0xF0000000u && w < 0xF4000000u) return 1;
    return lib_own_addr(w);
}

static uint64_t obj_hash(uint32_t obj)
{
    uint32_t w[OBJ_BYTES / 4u], i;
    for (i = 0; i < OBJ_BYTES / 4u; i++) {
        w[i] = G32(obj + i * 4u);
        if (proc_local(w[i])) w[i] = 0;
    }
    return fnv(0xCBF29CE484222325ull, w, sizeof w);
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
    lib_refresh();
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

static uint64_t world_hash(void)
{
    uint64_t world = 0xCBF29CE484222325ull;
    uint32_t i;
    lib_refresh();
    for (i = 1; i < 0x10000u; i++) {
        uint32_t obj = G32(AVP_OBJ_TABLE + i * 4u);
        uint64_t oh;
        if (!obj_ok(obj)) continue;
        oh = obj_hash(obj);
        world = fnv(world, &i, sizeof i);
        world = fnv(world, &oh, sizeof oh);
    }
    return world;
}

static uint64_t core_hash(void)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t v[3] = { G32(AVP_CLOCK), G32(AVP_RNG_ACC), G32(AVP_RNG_SEED) };
    return fnv(h, v, sizeof v);
}

enum { MSG_HELLO = 1, MSG_START = 2, MSG_TURN = 3, MSG_PAGEHASH = 4, MSG_PAGES = 5, MSG_CFG = 6, MSG_LAUNCH = 7, MSG_PICK = 8, MSG_READY = 9 };

#define PAGE          0x1000u
#define BULK_MAX      (64u << 20)

static const uint32_t k_sync_ranges[][2] = {
    { 0x00265000u, 0x00311000u },
    { 0x00F81000u, 0x03082000u },
};

static uint8_t *volatile s_bulk[8];
static volatile uint32_t s_bulk_len[8];

#define TURN_TICKS   4
#define TURN_DELAY   2
#define TURN_RING    64
#define CMD_BYTES    8192
#define WAIT_MS_DEFAULT 120000

static uint64_t wait_ms(void)
{
    static uint64_t v;
    if (!v) {
        const char *e = getenv("AVP_COOP_WAIT_MS");
        v = e && atoi(e) > 0 ? (uint64_t)atoi(e) : WAIT_MS_DEFAULT;
    }
    return v;
}
#define COOP_MAGIC   0x43505641u
#define COOP_VERSION 6u

typedef struct {
    uint32_t turn;
    int have;
    uint64_t hash;
    uint64_t whash;
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
static uint64_t s_whashes[TURN_RING];
static uint8_t s_pending[CMD_BYTES];
static uint32_t s_npending;
static avp_coop_exec_fn s_exec;
static HANDLE s_net_ready, s_cfg_ready, s_thread;
static SOCKET s_listen = INVALID_SOCKET;
static volatile int s_cancel, s_state;
static int s_lock_ready, s_tries = 120, s_wsa_ok;
static volatile int s_cfg_n;
static char s_spec[160], s_text[160], s_peer_name[24], s_peer_addr[48];

typedef struct {
    uint32_t magic, version, role;
    char name[24];
} Hello;

extern const char *avp_player_name(void);
#define CFG_MAX 2048u
#define SHELL_PTR 0x002F67F0u
#define MISSIONS 21

static const struct { char code[4]; int race; } k_missions[MISSIONS] = {
    { "M01", 0 }, { "M02", 0 }, { "M03", 0 }, { "M05", 0 }, { "M06", 0 }, { "M09", 0 }, { "M10", 0 },
    { "P01", 1 }, { "P02", 1 }, { "P03", 1 }, { "P04", 1 }, { "P06", 1 }, { "P08", 1 }, { "P11", 1 },
    { "A01", 2 }, { "A02", 2 }, { "A03", 2 }, { "A04", 2 }, { "A07", 2 }, { "A06", 2 }, { "A08", 2 },
};

static volatile LONG s_launch_req = -1, s_guest_mission = -1, s_host_pick = -1, s_peer_ready, s_self_ready;
static char s_peer_map[8];
static volatile ULONGLONG s_launch_t;

static int launch_busy(void)
{
    ULONGLONG t = s_launch_t;
    return t && GetTickCount64() - t < 600000u;
}

static int mission_find(const char *code)
{
    int i;
    for (i = 0; i < MISSIONS; i++)
        if (!strncmp(k_missions[i].code, code, 3)) return i;
    return -1;
}

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

static CRITICAL_SECTION s_send_lock;
static int s_send_lock_ready;
static volatile int s_last_net_err;
static volatile LONG s_netlog_n;
static struct { ULONGLONG ms; int dir; uint32_t type, len; } s_netlog[32];

static void net_note(int dir, uint32_t type, uint32_t len)
{
    LONG i = InterlockedIncrement(&s_netlog_n) - 1;
    s_netlog[i & 31].ms = GetTickCount64();
    s_netlog[i & 31].dir = dir;
    s_netlog[i & 31].type = type;
    s_netlog[i & 31].len = len;
}

static void net_dump(const char *why)
{
    LONG n = s_netlog_n, i, first = n > 32 ? n - 32 : 0;
    ULONGLONG now = GetTickCount64();
    fprintf(stderr, "[COOP] net state (%s): role %d state %d mission %d dead %d tick %llu since mission %llu last socket error %d\n",
            why, s_role, s_state, s_mission, s_coop_dead, (unsigned long long)s_tick,
            (unsigned long long)(s_mission ? s_tick - s_mission_tick : 0), s_last_net_err);
    for (i = first; i < n; i++)
        fprintf(stderr, "[COOP]   %s type %u len %u, %llu ms ago\n", s_netlog[i & 31].dir ? "recv" : "send",
                s_netlog[i & 31].type, s_netlog[i & 31].len, (unsigned long long)(now - s_netlog[i & 31].ms));
    fflush(stderr);
}

void avp_coop_crash_context(void)
{
    if (s_role == ROLE_NONE) return;
    net_dump("crash");
}

static int send_all(const void *p, int n)
{
    const char *c = (const char *)p;
    while (n > 0) {
        int k = send(s_sock, c, n, AVP_SEND_FLAGS);
#ifndef _WIN32
        if (k < 0 && errno == EINTR) continue;
#endif
        if (k <= 0) { s_last_net_err = k < 0 ? WSAGetLastError() : 0; return 0; }
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
#ifndef _WIN32
        if (k < 0 && errno == EINTR) continue;
#endif
        if (k <= 0) { s_last_net_err = k < 0 ? WSAGetLastError() : 0; return 0; }
        c += k;
        n -= k;
    }
    return 1;
}

static int send_msg(uint32_t type, const void *body, uint32_t len)
{
    uint32_t hdr[2] = { type, len };
    int ok;
    if (s_sock == INVALID_SOCKET) return 0;
    if (s_send_lock_ready) EnterCriticalSection(&s_send_lock);
    ok = send_all(hdr, sizeof hdr) && (!len || send_all(body, (int)len));
    if (s_send_lock_ready) LeaveCriticalSection(&s_send_lock);
    net_note(0, type, len);
    return ok;
}

static void set_state(int st, const char *fmt, ...)
{
    char t[sizeof s_text];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&s_lock);
    memcpy(s_text, t, sizeof s_text);
    s_state = st;
    LeaveCriticalSection(&s_lock);
}

static void lock_init(void)
{
    if (s_lock_ready) return;
    InitializeCriticalSection(&s_lock);
    InitializeCriticalSection(&s_send_lock);
    s_send_lock_ready = 1;
    s_lock_ready = 1;
}

#ifndef _WIN32
static int wsa_ensure(void)
{
    s_wsa_ok = 1;
    return 1;
}
#else
static int wsa_ensure(void)
{
    WSADATA wd;
    int rc;
    if (s_wsa_ok) return 1;
    rc = WSAStartup(MAKEWORD(2, 2), &wd);
    if (rc) {
        lock_init();
        set_state(3, "Network is not available: error %d", rc);
        fprintf(stderr, "[COOP] WSAStartup failed: %d\n", rc);
        return 0;
    }
    s_wsa_ok = 1;
    return 1;
}
#endif

static void coop_fail_text(const char *log, const char *fmt, ...)
{
    char t[sizeof s_text];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (s_coop_dead) return;
    s_coop_dead = 1;
    if (s_cfg_ready) SetEvent(s_cfg_ready);
    if (!s_cancel) set_state(3, "%s", t);
    if (s_cancel) return;
    fprintf(stderr, "[COOP] connection lost: %s; carrying on alone\n", log);
    net_dump(log);
}

static void coop_fail(const char *why)
{
    if (s_coop_dead) return;
    s_coop_dead = 1;
    if (s_cfg_ready) SetEvent(s_cfg_ready);
    if (!s_cancel)
        set_state(3, "%s: %s", s_state == 2 ? "Connection lost" : s_role == ROLE_HOST ? "Could not host" : "Could not connect", why);
    if (s_cancel) return;
    fprintf(stderr, "[COOP] connection lost: %s; carrying on alone\n", why);
    net_dump(why);
}

static void close_listen(void)
{
    EnterCriticalSection(&s_lock);
    if (s_listen != INVALID_SOCKET) {
        closesocket(s_listen);
        s_listen = INVALID_SOCKET;
    }
    LeaveCriticalSection(&s_lock);
}

static SOCKET try_connect(const struct sockaddr_in *a)
{
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    int t;
    if (c == INVALID_SOCKET) return c;
    ioctlsocket(c, FIONBIO, &nb);
    connect(c, (const struct sockaddr *)a, sizeof *a);
    for (t = 0; t < 30 && !s_cancel; t++) {
        fd_set w, x;
        struct timeval tv = { 0, 100000 };
        FD_ZERO(&w);
        FD_ZERO(&x);
        FD_SET(c, &w);
        FD_SET(c, &x);
        if (select((int)c + 1, NULL, &w, &x, &tv) > 0) {
            if (FD_ISSET(c, &w)) {
                nb = 0;
                ioctlsocket(c, FIONBIO, &nb);
                return c;
            }
            break;
        }
    }
    closesocket(c);
    return INVALID_SOCKET;
}

static int send_cfg(void)
{
    char cfg[CFG_MAX];
    uint32_t n = 0;
    FILE *f = avp_skirmish_armed() || avp_skirmish_active() ? fopen("skirmish.cfg", "rb") : NULL;
    if (f) {
        n = (uint32_t)fread(cfg, 1, sizeof cfg, f);
        fclose(f);
    }
    if (!send_msg(MSG_CFG, cfg, n)) return 0;
    fprintf(stderr, "[COOP] sent the skirmish config (%u bytes)\n", n);
    return 1;
}

static DWORD WINAPI net_thread(LPVOID arg)
{
    const char *spec = (const char *)arg;
    struct sockaddr_in a;
    Hello hello, peer;
    int one = 1;
    char host[128] = "127.0.0.1";
    int port = 27015;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    if (s_role == ROLE_HOST) {
        SOCKET ls;
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        int err = 0;
        sscanf(spec, "host:%d", &port);
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) {
            err = WSAGetLastError();
            coop_fail_text("socket", "Could not listen: error %d", err);
            SetEvent(s_net_ready);
            return 0;
        }
#ifndef _WIN32
        {
            int reuse = 1;
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof reuse);
        }
#endif
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)port);
        if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) {
            err = WSAGetLastError();
            closesocket(ls);
            if (err == WSAEADDRINUSE) coop_fail_text("port in use", "Port %d is in use", port);
            else if (err == WSAEACCES) coop_fail_text("port blocked", "Port %d is not allowed on this system", port);
            else coop_fail_text("listen", "Could not listen: error %d", err);
            SetEvent(s_net_ready);
            return 0;
        }
        EnterCriticalSection(&s_lock);
        s_listen = ls;
        LeaveCriticalSection(&s_lock);
        fprintf(stderr, "[COOP] hosting on port %d, waiting for the other player\n", port);
        while (!s_cancel) {
            fd_set rd;
            struct timeval tv = { 0, 100000 };
            int k;
            FD_ZERO(&rd);
            FD_SET(ls, &rd);
            k = select((int)ls + 1, &rd, NULL, NULL, &tv);
            if (k < 0) break;
            if (k > 0) {
                s_sock = accept(ls, (struct sockaddr *)&pa, &pl);
                break;
            }
        }
        close_listen();
        if (s_sock != INVALID_SOCKET) inet_ntop(AF_INET, &pa.sin_addr, s_peer_addr, sizeof s_peer_addr);
    } else {
        int tries;
        struct addrinfo hints, *ai = NULL;
        sscanf(spec, "join:%127[^:]:%d", host, &port);
        a.sin_port = htons((u_short)port);
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &ai) || !ai) {
            coop_fail("could not find that address");
            SetEvent(s_net_ready);
            return 0;
        }
        a.sin_addr = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
        freeaddrinfo(ai);
        for (tries = 0; tries < s_tries && !s_cancel; tries++) {
            int k;
            s_sock = try_connect(&a);
            if (s_sock != INVALID_SOCKET) break;
            for (k = 0; k < 10 && !s_cancel; k++) Sleep(50);
        }
        snprintf(s_peer_addr, sizeof s_peer_addr, "%.47s", host);
        fprintf(stderr, "[COOP] %s %s:%d\n", s_sock == INVALID_SOCKET ? "could not reach" : "joined", host, port);
    }
    if (s_sock != INVALID_SOCKET && s_cancel) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    if (s_sock == INVALID_SOCKET || s_cancel) {
        coop_fail("no connection");
        SetEvent(s_net_ready);
        return 0;
    }
    setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    memset(&hello, 0, sizeof hello);
    hello.magic = COOP_MAGIC;
    hello.version = COOP_VERSION;
    hello.role = (uint32_t)s_role;
    snprintf(hello.name, sizeof hello.name, "%s", avp_player_name());
    if (!send_msg(MSG_HELLO, &hello, sizeof hello)) { coop_fail("hello"); SetEvent(s_net_ready); return 0; }
    if (s_role == ROLE_HOST && !send_cfg()) { coop_fail("cfg"); SetEvent(s_net_ready); return 0; }
    set_state(2, "Connected to %s", s_peer_addr);
    SetEvent(s_net_ready);
    for (;;) {
        uint32_t hdr[2];
        if (!recv_all(hdr, sizeof hdr)) { coop_fail("closed"); return 0; }
        net_note(1, hdr[0], hdr[1]);
        if (hdr[0] == MSG_PAGEHASH || hdr[0] == MSG_PAGES) {
            uint8_t *b;
            if (hdr[1] > BULK_MAX || !(b = (uint8_t *)malloc(hdr[1] ? hdr[1] : 1))) { coop_fail("bulk size"); return 0; }
            if (hdr[1] && !recv_all(b, (int)hdr[1])) { free(b); coop_fail("bulk data"); return 0; }
            s_bulk_len[hdr[0]] = hdr[1];
            s_bulk[hdr[0]] = b;
            continue;
        }
        if (hdr[0] == MSG_CFG) {
            char cfg[CFG_MAX];
            if (hdr[1] > sizeof cfg || (hdr[1] && !recv_all(cfg, (int)hdr[1]))) { coop_fail("cfg"); return 0; }
            if (hdr[1]) {
                FILE *f = fopen("skirmish.cfg", "wb");
                AvpSkirmishCfg c;
                if (f) {
                    fwrite(cfg, 1, hdr[1], f);
                    fclose(f);
                }
                if (f && avp_skirmish_config_load(&c) && avp_skirmish_config_save(&c)) {
                    SetEnvironmentVariableA("AVP_SKIRMISH", "1");
                    avp_skirmish_arm(0);
                    fprintf(stderr, "[COOP] received the host's skirmish config (%u bytes), map %s\n", hdr[1], c.map);
                } else {
                    fprintf(stderr, "[COOP] could not apply the host's skirmish config\n");
                }
            }
            s_cfg_n++;
            SetEvent(s_cfg_ready);
            continue;
        }
        if (hdr[1] > sizeof(Turn) + 64) { coop_fail("bad packet"); return 0; }
        if (hdr[0] == MSG_HELLO) {
            if (hdr[1] != sizeof peer || !recv_all(&peer, sizeof peer)) { coop_fail("hello"); return 0; }
            if (peer.magic != COOP_MAGIC || peer.version != COOP_VERSION || peer.role == (uint32_t)s_role) {
                coop_fail("the other game is a different version or the same role");
                return 0;
            }
            peer.name[sizeof peer.name - 1] = 0;
            EnterCriticalSection(&s_lock);
            snprintf(s_peer_name, sizeof s_peer_name, "%s", *peer.name ? peer.name : "Player");
            LeaveCriticalSection(&s_lock);
            set_state(2, "Connected to %s", s_peer_name);
            fprintf(stderr, "[COOP] connected to the %s, %s\n", peer.role == ROLE_HOST ? "host" : "guest", s_peer_name);
        } else if (hdr[0] == MSG_LAUNCH) {
            char m[8] = { 0 };
            int i;
            if (hdr[1] != 4 || !recv_all(m, 4)) { coop_fail("launch"); return 0; }
            m[3] = 0;
            i = mission_find(m);
            if (s_role != ROLE_GUEST || i < 0 || s_mission || launch_busy()) {
                fprintf(stderr, "[COOP] ignored a mission launch for \"%s\"\n", m);
            } else {
                s_launch_t = GetTickCount64();
                InterlockedExchange(&s_guest_mission, i);
                InterlockedExchange(&s_launch_req, i);
                fprintf(stderr, "[COOP] the host chose mission %s\n", m);
            }
        } else if (hdr[0] == MSG_PICK) {
            char m[8] = { 0 };
            int i;
            if (hdr[1] != 4 || !recv_all(m, 4)) { coop_fail("pick"); return 0; }
            m[3] = 0;
            i = mission_find(m);
            if (s_role == ROLE_GUEST && i >= 0) {
                InterlockedExchange(&s_guest_mission, i);
                InterlockedExchange(&s_self_ready, 0);
                fprintf(stderr, "[COOP] the host picked mission %s\n", m);
            }
        } else if (hdr[0] == MSG_READY) {
            char r[4] = { 0 };
            if (hdr[1] != 4 || !recv_all(r, 4)) { coop_fail("ready"); return 0; }
            if (s_role == ROLE_HOST) {
                InterlockedExchange(&s_peer_ready, r[0] ? 1 : 0);
                fprintf(stderr, "[COOP] the other player is %s\n", r[0] ? "ready" : "not ready");
            }
        } else if (hdr[0] == MSG_START) {
            char m[8] = { 0 };
            if (hdr[1] > 4 || (hdr[1] && !recv_all(m, (int)hdr[1]))) { coop_fail("start"); return 0; }
            m[3] = 0;
            memcpy(s_peer_map, m, sizeof s_peer_map);
            s_peer_started = 1;
            fprintf(stderr, "[COOP] the other player's mission has started\n");
        } else if (hdr[0] == MSG_TURN) {
            uint32_t head[6], len;
            uint8_t body[CMD_BYTES];
            Turn *t;
            if (hdr[1] < sizeof head || !recv_all(head, sizeof head)) { coop_fail("turn"); return 0; }
            len = head[3];
            if (len > CMD_BYTES || hdr[1] != sizeof head + len) { coop_fail("turn size"); return 0; }
            if (len && !recv_all(body, (int)len)) { coop_fail("turn data"); return 0; }
            EnterCriticalSection(&s_lock);
            t = &s_remote[head[0] % TURN_RING];
            t->have = 0;
            t->turn = head[0];
            t->hash = (uint64_t)head[1] | ((uint64_t)head[2] << 32);
            t->whash = (uint64_t)head[4] | ((uint64_t)head[5] << 32);
            t->len = len;
            memcpy(t->data, body, len);
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

static void net_begin(const char *spec, int role, int tries, const char *status)
{
    int i;
    lock_init();
    snprintf(s_spec, sizeof s_spec, "%s", spec);
    for (i = 0; i < 8; i++) {
        free(s_bulk[i]);
        s_bulk[i] = NULL;
    }
    memset(s_remote, 0, sizeof s_remote);
    memset(s_local, 0, sizeof s_local);
    s_npending = 0;
    s_coop_dead = 0;
    s_peer_started = 0;
    s_cancel = 0;
    s_tries = tries;
    s_peer_name[0] = 0;
    s_peer_addr[0] = 0;
    s_sock = INVALID_SOCKET;
    s_role = role;
    s_cfg_n = 0;
    set_state(1, "%s", status);
    if (s_net_ready) CloseHandle(s_net_ready);
    if (s_cfg_ready) CloseHandle(s_cfg_ready);
    s_net_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_cfg_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_thread = CreateThread(NULL, 0, net_thread, (LPVOID)s_spec, 0, NULL);
}

static void coop_start_net(void)
{
    const char *e;
    int role;
    e = getenv("AVP_COOP");
    if (!e || !*e) return;
    if (!strncmp(e, "host", 4)) role = ROLE_HOST;
    else if (!strncmp(e, "join:", 5)) role = ROLE_GUEST;
    else return;
    if (!wsa_ensure()) return;
    net_begin(e, role, 120, role == ROLE_HOST ? "Waiting for a player" : "Connecting to the host");
    fprintf(stderr, "[COOP] %s\n", s_role == ROLE_HOST ? "host" : "guest");
    if (s_role == ROLE_GUEST && (e = getenv("AVP_SKIRMISH")) && *e && *e != '0') {
        fprintf(stderr, "[COOP] waiting for the host's skirmish config\n");
        WaitForSingleObject(s_cfg_ready, 130000);
    }
}

static void net_stop(void)
{
    s_cancel = 1;
    if (s_thread) {
        close_listen();
        if (s_sock != INVALID_SOCKET) shutdown(s_sock, SD_BOTH);
        WaitForSingleObject(s_thread, 5000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    s_role = ROLE_NONE;
    s_coop_dead = 0;
    s_launch_t = 0;
    s_peer_name[0] = 0;
    InterlockedExchange(&s_guest_mission, -1);
    InterlockedExchange(&s_host_pick, -1);
    InterlockedExchange(&s_peer_ready, 0);
    InterlockedExchange(&s_self_ready, 0);
    s_peer_map[0] = 0;
    if (s_lock_ready) set_state(0, "");
    if (s_fixed != s_env_fixed) {
        s_fixed = s_env_fixed;
        s_rebase = 0;
        fprintf(stderr, "[SYNC] fixed step %s\n", s_fixed ? "on" : "off");
    }
}

static int net_allowed(void)
{
    coop_init();
    if (s_mission || s_mission_seen) {
        lock_init();
        if (!s_mission) set_state(3, "Restart the game to host or join after playing a mission");
        return 0;
    }
    return 1;
}

static void fixed_late(void)
{
    if (s_fixed) return;
    s_fixed = 1;
    s_rebase = 1;
    fprintf(stderr, "[SYNC] fixed step %d Hz (network match)\n", s_hz);
}

int avp_coop_host(int port)
{
    char spec[32], status[64];
    if (!net_allowed()) return 0;
    net_stop();
    if (port < 1 || port > 65535) port = 27015;
    snprintf(spec, sizeof spec, "host:%d", port);
    if (!wsa_ensure()) return 0;
    snprintf(status, sizeof status, "Waiting for a player on port %d", port);
    fixed_late();
    net_begin(spec, ROLE_HOST, 1, status);
    return 1;
}

int avp_coop_join(const char *ip, int port)
{
    char spec[160], status[160];
    if (!net_allowed()) return 0;
    net_stop();
    if (!ip || !*ip || strchr(ip, ':')) ip = "127.0.0.1";
    if (port < 1 || port > 65535) port = 27015;
    snprintf(spec, sizeof spec, "join:%.100s:%d", ip, port);
    if (!wsa_ensure()) return 0;
    snprintf(status, sizeof status, "Connecting to %.100s:%d", ip, port);
    fixed_late();
    net_begin(spec, ROLE_GUEST, 5, status);
    return 1;
}

void avp_coop_cancel(void)
{
    coop_init();
    if (s_mission || launch_busy()) return;
    net_stop();
}

int avp_coop_state(void)
{
    return s_state;
}

const char *avp_coop_state_text(void)
{
    static char t[sizeof s_text];
    if (!s_lock_ready) return "";
    EnterCriticalSection(&s_lock);
    memcpy(t, s_text, sizeof t);
    LeaveCriticalSection(&s_lock);
    return t;
}

const char *avp_coop_peer_name(void)
{
    return s_peer_name;
}

int avp_coop_take_start(void)
{
    if (s_role != ROLE_GUEST || s_state != 2 || s_coop_dead || s_cfg_n < 2) return 0;
    s_cfg_n = 1;
    return 1;
}

int avp_coop_send_cfg(void)
{
    if (s_role != ROLE_HOST || s_state != 2 || s_coop_dead || s_mission) return 0;
    if (send_cfg()) return 1;
    coop_fail("cfg");
    return 0;
}

static void send_turn(uint32_t turn, uint64_t hash, uint64_t whash)
{
    uint32_t head[6];
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
    head[4] = (uint32_t)whash;
    head[5] = (uint32_t)(whash >> 32);
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

#define UNIT_PLAYER 0x474u

static int s_vs, s_vs_slot[2];

static void versus_init(void)
{
    AvpSkirmishCfg c;
    int i, h = -1, g = -1;
    s_vs = 0;
    if (!avp_skirmish_active() || !avp_skirmish_config_load(&c)) return;
    for (i = 0; i < c.nplayers; i++) {
        if (c.player[i].kind == AVP_PK_HUMAN && h < 0) h = c.player[i].slot;
        if (c.player[i].kind == AVP_PK_REMOTE && g < 0) g = c.player[i].slot;
    }
    if (h < 0 || g < 0) return;
    s_vs = 1;
    s_vs_slot[0] = h;
    s_vs_slot[1] = g;
    fprintf(stderr, "[COOP] versus: host plays slot %d, guest plays slot %d\n", h, g);
}

static int owner_of(uint32_t handle)
{
    uint32_t i = handle >> 16;
    if (s_vs) {
        uint32_t obj = G32(AVP_OBJ_TABLE + i * 4u);
        int slot;
        if (!obj_ok(obj)) return -1;
        slot = (int)G32(obj + UNIT_PLAYER);
        return slot == s_vs_slot[0] ? 0 : slot == s_vs_slot[1] ? 1 : -1;
    }
    return s_owner_h[i] == handle ? s_owner[i] : 0;
}

static int coop_live(void)
{
    return s_role != ROLE_NONE && !s_coop_dead && s_mission;
}

enum { CMD_SELECT = 3, CMD_MOVE = 4, CMD_PCALL = 5, CMD_MARKER = 6 };

#define AVP_PTR_GLOBAL2  0x002F67F4u
#define AVP_WORLD        0x002F6AE4u
#define AVP_FACTORY      0x00027690u
#define AVP_SELECT_FN    0x000F0070u
#define AVP_DESELECT_FN  0x000F01A0u
#define AVP_MOVE_FN      0x0002D9F0u
#define AVP_NOWHERE      0x00304EDCu
#define PTR_BYTES        0x280u
#define SEL_MAX          64

extern uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax);
extern uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n);
extern uint32_t avp_guest_alloca(uint32_t bytes);
extern uint32_t avp_guest_esp(void);
extern void avp_guest_set_esp(uint32_t v);

#define AVP_LEVEL_RESET  0x0006A8E0u
#define AVP_LEVEL_RECORD 0x00306018u
#define SHELL_FLAGS      0x210u

static uint32_t shell_state(void)
{
    uint32_t sh = G32(SHELL_PTR);
    return sh >= 0x10000u && sh < 0x04000000u ? sh : 0;
}

static int level_is_mission(uint32_t sh)
{
    char c0 = (char)G8(sh), c3 = (char)G8(sh + 3);
    if (c0 >= 'a' && c0 <= 'z') c0 = (char)(c0 - 32);
    return (c0 == 'A' || c0 == 'M' || c0 == 'P' || c0 == 'T') && G8(sh + 1) >= '0' && G8(sh + 1) <= '9'
        && G8(sh + 2) >= '0' && G8(sh + 2) <= '9' && c3 == '/';
}

static void launch_apply(int mission)
{
    uint32_t sh = shell_state(), a[1], i;
    char name[40];
    a[0] = AVP_LEVEL_RECORD;
    avp_gcall(AVP_LEVEL_RESET, 0, a, 1, 0);
    avp_skirmish_disarm();
    snprintf(name, sizeof name, "%s/background.tbd", k_missions[mission].code);
    for (i = 0; i <= strlen(name); i++) G8(sh + i) = (uint8_t)name[i];
    G32(sh + SHELL_FLAGS) |= 1u;
    fprintf(stderr, "[COOP] loading mission %s (\"%s\")\n", k_missions[mission].code, name);
}

static void auto_drive(void)
{
    static int idx = -2;
    if (idx == -2) {
        const char *e = getenv("AVP_COOP_AUTO");
        int i;
        idx = -1;
        for (i = 0; e && *e && i < MISSIONS; i++)
            if (!strncmp(k_missions[i].code, e, 3)) idx = i;
    }
    if (idx < 0 || s_state != 2 || s_coop_dead || s_mission) return;
    if (s_role == ROLE_GUEST) {
        if (s_guest_mission >= 0 && !s_self_ready) avp_coop_set_ready(1);
    } else if (s_role == ROLE_HOST) {
        if (s_host_pick != idx) avp_coop_host_pick(idx);
        else if (s_peer_ready) avp_coop_host_launch(idx);
    }
}

static void launch_pump(void)
{
    LONG m = s_launch_req;
    uint32_t sh;
    auto_drive();
    if (m < 0 || m >= MISSIONS) return;
    sh = shell_state();
    if (!sh) return;
    if (G32(sh + SHELL_FLAGS) & 5u) return;
    InterlockedExchange(&s_launch_req, -1);
    if (level_is_mission(sh)) {
        fprintf(stderr, "[COOP] mission %s not loaded, a mission is already running\n", k_missions[m].code);
        return;
    }
    launch_apply((int)m);
}

static void hide_pointer(uint32_t p);
static uint32_t s_ptr[2];
static uint32_t s_sel_pending[SEL_MAX];
static int s_nsel_pending;

static int ptr_obj_ok(uint32_t p)
{
    return p >= 0x10000u && !(p & 3u) && p < LOCAL_HI - 0x1000u - PTR_BYTES;
}

static int ptr_class_ok(uint32_t p)
{
    uint32_t vt, fn;
    if (!ptr_obj_ok(p)) return 0;
    vt = G32(p);
    if (!ptr_obj_ok(vt) || vt < 0x002050C0u || vt >= 0x00310680u) return 0;
    fn = G32(vt + 0xB4u);
    return fn >= 0x00011000u && fn < 0x00170D60u;
}

static uint32_t player_pointer(int player)
{
    uint32_t h, obj;
    if (player < 0 || player > 1) return 0;
    h = s_ptr[player];
    if (!h) return 0;
    obj = G32(AVP_OBJ_TABLE + (h >> 16) * 4u);
    if (!ptr_obj_ok(obj) || G32(obj + 0x18) != h || !ptr_class_ok(obj)) return 0;
    return obj;
}

static void make_pointers(void)
{
    uint32_t world = G32(AVP_WORLD), pm, a[2], b, cur, cur2, own = G32(AVP_POINTER);
    s_ptr[0] = s_ptr[1] = 0;
    if (!ptr_class_ok(own)) { fprintf(stderr, "[COOP] no usable pointer object (%08X) for the local player\n", own); return; }
    s_ptr[0] = G32(own + 0x18);
    pm = ptr_obj_ok(world) ? G32(world + 0x78) : 0;
    if (!ptr_obj_ok(pm) || !G32(pm + 0x19C)) { fprintf(stderr, "[COOP] no player manager for a second pointer\n"); return; }
    a[0] = G32(pm + 0x19C);
    a[1] = G32(pm + 0x18) & 0xFFFFu;
    cur = G32(AVP_POINTER);
    cur2 = G32(AVP_PTR_GLOBAL2);
    b = avp_gcall(AVP_FACTORY, 0, a, 2, 0);
    G32(AVP_POINTER) = cur;
    G32(AVP_PTR_GLOBAL2) = cur2;
    if (!ptr_class_ok(b)) { fprintf(stderr, "[COOP] could not create player 2's pointer\n"); return; }
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

static int ptr_keep(uint32_t off)
{
    return off < 0x1C || (off >= 0x1B8 && off < 0x1C0) || (off >= 0x270 && off < 0x278)
        || (off >= 0x70 && off < 0xDC) || (off >= 0x10C && off < 0x178) || (off >= 0x21C && off < 0x22C);
}

typedef struct { uint8_t b[PTR_BYTES]; } PtrSave;

static void ptr_save(uint32_t p, PtrSave *s)
{
    uint32_t off;
    for (off = 0; off < PTR_BYTES; off += 4) {
        uint32_t v = G32(p + off);
        memcpy(s->b + off, &v, 4);
    }
}

static void ptr_restore(uint32_t p, const PtrSave *s)
{
    uint32_t off;
    for (off = 0; off < PTR_BYTES; off += 4) {
        uint32_t v;
        if (ptr_keep(off)) continue;
        memcpy(&v, s->b + off, 4);
        G32(p + off) = v;
    }
}

static void run_select(int player, const uint32_t *h, uint32_t n)
{
    uint32_t p = player_pointer(player), old, i, a[2];
    PtrSave keep;
    if (!p) return;
    ptr_save(p, &keep);
    old = swap_pointer(p);
    avp_gcall(G32(G32(p) + 0xB4), p, NULL, 0, 0);
    for (i = 0; i < n; i++) {
        if (owner_of(h[i]) != player) continue;
        a[0] = p;
        a[1] = h[i];
        avp_gcall(AVP_SELECT_FN, p, a, 2, 0);
    }
    swap_pointer(old);
    ptr_restore(p, &keep);
    if (player == 1) hide_pointer(p);
}

static void run_deselect(int player, uint32_t h)
{
    uint32_t p = player_pointer(player), old, a[1];
    if (!p) return;
    old = swap_pointer(p);
    a[0] = h;
    avp_gcall(AVP_DESELECT_FN, p, a, 1, 0);
    swap_pointer(old);
}

enum { ARG_RAW = 0, ARG_PTRFIELD = 1, ARG_BLOB = 2 };
#define ARG_BLOB_MAX 0x80u

static void run_move(int player, const uint8_t *d, uint32_t n)
{
    uint32_t p = player_pointer(player), old, off, a[4], esp, o = 0, i;
    PtrSave keep;
    if (!p || n < PTR_BYTES) return;
    ptr_save(p, &keep);
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
        if (o + 12 > n) {
            avp_guest_set_esp(esp);
            swap_pointer(old);
            ptr_restore(p, &keep);
            if (player == 1) hide_pointer(p);
            return;
        }
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
    ptr_restore(p, &keep);
    if (player == 1) hide_pointer(p);
}

static const struct { uint32_t fn, nstack; } k_pcall[] = {
    { 0x2C880u, 0 }, { 0x2C990u, 1 }, { 0x2CAC0u, 2 }, { 0x2CCD0u, 0 }, { 0x2CD60u, 0 }, { 0x2CE70u, 0 },
    { 0x2CF60u, 0 }, { 0x2D050u, 0 }, { 0x2D0E0u, 0 }, { 0x2D1D0u, 0 }, { 0x2D2C0u, 0 }, { 0x2D350u, 0 },
    { 0x2D440u, 0 }, { 0x2D580u, 0 }, { 0x2D670u, 0 }, { 0x2D760u, 0 }, { 0x2DD20u, 1 }, { 0x2DE70u, 0 },
    { 0x2DF90u, 0 }, { 0x2DFD0u, 0 }, { 0x2E130u, 0 }, { 0x2ED80u, 0 },
};

#define PCALL_REGS  6
#define PCALL_STACK 2

static int pcall_nstack(uint32_t fn)
{
    uint32_t i;
    for (i = 0; i < sizeof k_pcall / sizeof k_pcall[0]; i++)
        if (k_pcall[i].fn == fn) return (int)k_pcall[i].nstack;
    return -1;
}

static void run_pcall(int player, const uint8_t *d, uint32_t n)
{
    uint32_t p = player_pointer(player), old, off, fn, nstack, regs[PCALL_REGS], args[PCALL_STACK], esp, o, i;
    PtrSave keep;
    if (!p || n < 8) return;
    memcpy(&fn, d, 4);
    memcpy(&nstack, d + 4, 4);
    if (nstack > PCALL_STACK || pcall_nstack(fn) != (int)nstack) return;
    if (n != 8 + 8 * (PCALL_REGS + nstack) + PTR_BYTES) return;
    o = 8;
    for (i = 0; i < PCALL_REGS + nstack; i++) {
        uint32_t kind, val;
        memcpy(&kind, d + o, 4);
        memcpy(&val, d + o + 4, 4);
        o += 8;
        if (kind == ARG_PTRFIELD) val = p;
        if (i < PCALL_REGS) regs[i] = val;
        else args[i - PCALL_REGS] = val;
    }
    ptr_save(p, &keep);
    for (off = 0; off < PTR_BYTES; off += 4) {
        uint32_t v;
        if (ptr_keep(off)) continue;
        memcpy(&v, d + o + off, 4);
        G32(p + off) = v;
    }
    old = swap_pointer(p);
    esp = avp_guest_esp();
    avp_gcall_regs(fn, regs, args, (int)nstack);
    avp_guest_set_esp(esp);
    swap_pointer(old);
    ptr_restore(p, &keep);
    if (player == 1) hide_pointer(p);
}

#define AVP_MARKER_MAKE 0x0002EFF0u
#define MARKER_BYTES    16u

static int marker_crc_ok(uint32_t crc)
{
    return crc == 0x7715B5E5u || crc == 0x55EE21EFu || crc == 0x3C905527u;
}

static int marker_pos_ok(const float *v)
{
    int i;
    for (i = 0; i < 3; i++)
        if (!(v[i] > -1e7f && v[i] < 1e7f)) return 0;
    return 1;
}

static void run_marker(int player, const uint8_t *d, uint32_t n)
{
    uint32_t p = player_pointer(player), old, crc, esp, pos, regs[PCALL_REGS] = { 0 }, a[2];
    float v[3];
    PtrSave keep;
    if (!p || n != MARKER_BYTES) return;
    memcpy(&crc, d, 4);
    memcpy(v, d + 4, sizeof v);
    if (!marker_crc_ok(crc) || !marker_pos_ok(v)) return;
    ptr_save(p, &keep);
    old = swap_pointer(p);
    esp = avp_guest_esp();
    pos = avp_guest_alloca(32);
    memcpy((void *)((uintptr_t)pos + (uintptr_t)g_xbox_mem_offset), v, sizeof v);
    regs[0] = crc;
    a[0] = p;
    a[1] = pos;
    avp_gcall_regs(AVP_MARKER_MAKE, regs, a, 2);
    avp_guest_set_esp(esp);
    swap_pointer(old);
    ptr_restore(p, &keep);
    if (player == 1) hide_pointer(p);
}

int avp_coop_marked(uint32_t handle)
{
    return !s_vs && coop_live() && owner_of(handle) == 1;
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
    if (!ptr_obj_ok(ptr)) return 0;
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

int avp_coop_take_pcall(uint32_t fn, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx,
                        uint32_t esi, uint32_t edi)
{
    uint8_t buf[12 + 8 * (PCALL_REGS + PCALL_STACK) + PTR_BYTES];
    uint32_t regs[PCALL_REGS] = { eax, ecx, edx, ebx, esi, edi };
    uint32_t ptr, off, o, i, cmd = CMD_PCALL, ns;
    int nstack = pcall_nstack(fn);
    if (!coop_live() || s_executing || !s_local_input || nstack < 0) return 0;
    ptr = G32(AVP_POINTER);
    if (!ptr_obj_ok(ptr)) return 0;
    ns = (uint32_t)nstack;
    memcpy(buf, &cmd, 4);
    memcpy(buf + 4, &fn, 4);
    memcpy(buf + 8, &ns, 4);
    o = 12;
    for (i = 0; i < PCALL_REGS + ns; i++) {
        uint32_t v = i < PCALL_REGS ? regs[i] : G32(esp + 4 + 4 * (i - PCALL_REGS)), kind = ARG_RAW, val = v;
        if (v == ptr) {
            kind = ARG_PTRFIELD;
            val = 0;
        }
        memcpy(buf + o, &kind, 4);
        memcpy(buf + o + 4, &val, 4);
        o += 8;
    }
    for (off = 0; off < PTR_BYTES; off += 4) {
        uint32_t v = G32(ptr + off);
        memcpy(buf + o + off, &v, 4);
    }
    o += PTR_BYTES;
    if (!avp_coop_queue(buf, o)) return 0;
    {
        static int told;
        if (told++ < 10) fprintf(stderr, "[COOP] pointer call %08X sent\n", fn);
    }
    return 1;
}

int avp_coop_marker(uint32_t crc, const float *pos)
{
    uint8_t buf[4 + MARKER_BYTES];
    uint32_t cmd = CMD_MARKER;
    if (s_role == ROLE_NONE) return -1;
    if (!coop_live()) return -1;
    if (s_executing || !s_local_input) return 0;
    if (!marker_crc_ok(crc) || !marker_pos_ok(pos)) return 0;
    memcpy(buf, &cmd, 4);
    memcpy(buf + 4, &crc, 4);
    memcpy(buf + 8, pos, 12);
    return avp_coop_queue(buf, sizeof buf) ? 1 : 0;
}

static uint32_t s_fx_rng[2] = { 0x13579BDFu, 0x2468ACE0u };
static uint32_t s_fx_saved[2];
static int s_fx_depth;

static uint32_t s_fx_callers[16] = { 0x0005D413u };
static int s_fx_ncallers = 1;

void avp_coop_fx_rng(int enter, uint32_t obj)
{
    int i;
    if (!s_fixed && !s_fx_depth) return;
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
    launch_pump();
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
            } else if (kind == CMD_GIVE && from_host && !s_vs && n >= 8) {
                uint32_t cnt, i, given = 0;
                memcpy(&cnt, d + 4, 4);
                for (i = 0; i < cnt && 8 + 4 * (i + 1) <= n; i++) {
                    uint32_t h;
                    memcpy(&h, d + 8 + 4 * i, 4);
                    if (owner_of(h) != 0) continue;
                    run_deselect(0, h);
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
            } else if (kind == CMD_PCALL && n > 4) {
                run_pcall(from_host ? 0 : 1, d + 4, n - 4);
            } else if (kind == CMD_MARKER && n > 4) {
                run_marker(from_host ? 0 : 1, d + 4, n - 4);
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
    if (!coop_live() || s_role != ROLE_HOST || s_vs) return 0;
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
    uint64_t h = core_hash(), wh = world_hash();
    ULONGLONG t0, last_note;
    Turn *r, *l;
    s_hashes[turn % TURN_RING] = h;
    s_whashes[turn % TURN_RING] = wh;
    comp[turn % TURN_RING][0] = G32(AVP_CLOCK);
    comp[turn % TURN_RING][1] = G32(AVP_RNG_ACC);
    comp[turn % TURN_RING][2] = G32(AVP_RNG_SEED);
    send_turn(turn + TURN_DELAY, h, wh);
    if (turn < TURN_DELAY) return;
    t0 = last_note = GetTickCount64();
    for (;;) {
        int ok;
        EnterCriticalSection(&s_lock);
        r = &s_remote[turn % TURN_RING];
        ok = r->have && r->turn == turn;
        LeaveCriticalSection(&s_lock);
        if (ok || s_coop_dead) break;
        if (GetTickCount64() - t0 > wait_ms()) {
            fprintf(stderr, "[COOP] waited for turn %u; slot holds turn %u have %d\n", turn, r->turn, r->have);
            coop_fail("the other player stopped answering");
            return;
        }
        if (GetTickCount64() - last_note > 10000) {
            last_note = GetTickCount64();
            fprintf(stderr, "[COOP] waiting %llu s for the other player's turn %u (slot holds turn %u have %d), sync tick %llu\n",
                    (unsigned long long)((last_note - t0) / 1000u), turn, r->turn, r->have,
                    (unsigned long long)(s_tick - s_mission_tick));
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
    if (r->whash != s_whashes[(turn - TURN_DELAY) % TURN_RING]) {
        static int told, total;
        total++;
        if (told < 20 || total % 100 == 0) {
            told++;
            fprintf(stderr, "[COOP] world mismatch #%d: turn %u (sync tick %llu), ours %016llX, theirs %016llX\n", total,
                    turn - TURN_DELAY, (unsigned long long)(s_tick - s_mission_tick),
                    (unsigned long long)s_whashes[(turn - TURN_DELAY) % TURN_RING], (unsigned long long)r->whash);
            if (told == 1) net_dump("first world mismatch");
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

static int sync_addr_ok(uint32_t a)
{
    uint32_t r;
    if (a & (PAGE - 1u)) return 0;
    for (r = 0; r < sizeof k_sync_ranges / sizeof k_sync_ranges[0]; r++)
        if (a >= k_sync_ranges[r][0] && a < k_sync_ranges[r][1]) return 1;
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
    lib_refresh();
    if (s_role == ROLE_GUEST) {
        uint64_t *hashes = (uint64_t *)malloc((size_t)n * 8u);
        uint8_t *b;
        uint32_t applied = 0, still = 0, o, kept = 0, np = 0, k, kept_lib = 0, kept_obj = 0;
        if (!hashes) { coop_fail("memory"); return; }
        for (i = 0; i < n; i++) hashes[i] = page_hash(sync_page_addr(i));
        if (!send_msg(MSG_PAGEHASH, hashes, n * 8u)) { free(hashes); coop_fail("send hashes"); return; }
        b = wait_bulk(MSG_PAGES, &len);
        if (!b || len < 16) { free(hashes); return; }
        memcpy(&s_vcount, b, 8);
        memcpy(&applied, b + 8, 4);
        memcpy(&np, b + 12, 4);
        if (np > LIB_MAX || 16u + np * 8u > len) {
            free(hashes);
            free(b);
            s_bulk[MSG_PAGES] = NULL;
            coop_fail("state copy");
            return;
        }
        for (k = 0; k < np; k++) {
            uint32_t lo, sz;
            memcpy(&lo, b + 16 + k * 8u, 4);
            memcpy(&sz, b + 20 + k * 8u, 4);
            s_lib_peer[k].lo = lo;
            s_lib_peer[k].hi = lo + sz;
        }
        s_lib_peer_n = span_merge(s_lib_peer, np);
        for (o = 16u + np * 8u, i = 0; i < applied && o + 4 + PAGE <= len; i++, o += 4 + PAGE) {
            uint32_t a, w;
            uint32_t *dst, src[PAGE / 4];
            memcpy(&a, b + o, 4);
            if (!sync_addr_ok(a)) { fprintf(stderr, "[COOP] state copy: bad page address %08X\n", a); coop_fail("state copy"); break; }
            memcpy(src, b + o + 4, PAGE);
            dst = (uint32_t *)guest_page(a);
            for (w = 0; w < PAGE / 4; w++) {
                uint32_t wa = a + w * 4u;
                if (src[w] == dst[w]) continue;
                if (lib_own_addr(wa) || span_has(s_lib_peer, s_lib_peer_n, wa)) {
                    kept_lib++;
                    continue;
                }
                if (span_has(s_lib_peer, s_lib_peer_n, src[w])) {
                    kept_obj++;
                    continue;
                }
                if (!(src[w] & 3u) && !(dst[w] & 3u)
                    && src[w] >= LOCAL_LO && src[w] < LOCAL_HI && dst[w] >= LOCAL_LO && dst[w] < LOCAL_HI) {
                    if (kept++ < 8)
                        fprintf(stderr, "[COOP] kept local pointer at %08X: host %08X, here %08X\n",
                                wa, src[w], dst[w]);
                    continue;
                }
                dst[w] = src[w];
            }
        }
        free(b);
        s_bulk[MSG_PAGES] = NULL;
        for (i = 0; i < n; i++) if (page_hash(sync_page_addr(i)) != hashes[i]) still++;
        free(hashes);
        fprintf(stderr, "[COOP] state copied from the host: %u pages (%u KB); %u pages changed; %u thread-local pointers kept\n",
                applied, applied * 4u, still, kept);
        fprintf(stderr, "[COOP] library state kept: %u own heap spans, %u on the host, %u words not copied, %u pointers to library objects kept\n",
                s_lib_own_n, s_lib_peer_n, kept_lib, kept_obj);
    } else {
        uint8_t *b = wait_bulk(MSG_PAGEHASH, &len), *out;
        uint32_t diff = 0, o, k;
        if (!b) return;
        if (len != n * 8u) { free(b); coop_fail("the other game has a different memory layout"); return; }
        out = (uint8_t *)malloc(16u + LIB_MAX * 8u + (size_t)n * (4u + PAGE));
        if (!out) { free(b); coop_fail("memory"); return; }
        o = 16u + s_lib_own_n * 8u;
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
        memcpy(out + 12, &s_lib_own_n, 4);
        for (k = 0; k < s_lib_own_n; k++) {
            uint32_t sz = s_lib_own[k].hi - s_lib_own[k].lo;
            memcpy(out + 16 + k * 8u, &s_lib_own[k].lo, 4);
            memcpy(out + 20 + k * 8u, &sz, 4);
        }
        if (!send_msg(MSG_PAGES, out, o)) coop_fail("send pages");
        free(out);
        fprintf(stderr, "[COOP] state sent to the guest: %u pages (%u KB), %u library heap spans\n", diff, diff * 4u, s_lib_own_n);
    }
}

static char s_my_map[8];

static void wait_peer_start(void)
{
    ULONGLONG t0 = GetTickCount64();
    if (s_role == ROLE_NONE || s_coop_dead) return;
    WaitForSingleObject(s_net_ready, 120000);
    if (s_coop_dead) return;
    {
        char m[4] = { 0 };
        const char *cur = avp_skirmish_cur_map();
        if (cur) snprintf(m, sizeof m, "%s", cur);
        if (!send_msg(MSG_START, m, sizeof m)) { coop_fail("send start"); return; }
        snprintf(s_my_map, sizeof s_my_map, "%s", m);
    }
    fprintf(stderr, "[COOP] mission ready, waiting for the other player\n");
    while (!s_peer_started && !s_coop_dead) {
        if (GetTickCount64() - t0 > 120000) { coop_fail("the other player never started"); return; }
        Sleep(5);
    }
    if (!s_coop_dead && *s_my_map && *s_peer_map && strcmp(s_my_map, s_peer_map)) {
        char why[64];
        snprintf(why, sizeof why, "the games loaded different missions (%s and %s)", s_my_map, s_peer_map);
        coop_fail(why);
    }
    if (!s_coop_dead && !getenv("AVP_NO_STATE_SYNC"))
        state_sync();
    if (!s_coop_dead)
        make_pointers();
    if (!s_coop_dead)
        fprintf(stderr, "[COOP] both players in, lockstep on\n");
}

void avp_coop_mission_end(void)
{
    if (!s_mission) { s_mission_seen = 0; s_launch_t = 0; }
}

void avp_coop_mission_start(void)
{
    const char *e;
    coop_init();
    s_mission_seen = 1;
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
    versus_init();
    wait_peer_start();
    if (getenv("AVP_PAGE_DUMP")) {
        uint32_t a;
        for (a = 0; a < 0x04000000u; a += 0x1000u) {
            const uint8_t *p = (const uint8_t *)((uintptr_t)a + (uintptr_t)g_xbox_mem_offset);
            fprintf(stderr, "[PAGE] %08X %016llX\n", a, (unsigned long long)fnv(0xCBF29CE484222325ull, p, 0x1000u));
        }
    }
    s_mission = 1;
    s_launch_t = 0;
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
    if (s_rebase) {
        s_rebase = 0;
        s_vcount = real;
        s_real_at_tick = real;
    }
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

int avp_mission_count(void)
{
    return MISSIONS;
}

const char *avp_mission_code(int i)
{
    return i >= 0 && i < MISSIONS ? k_missions[i].code : "";
}

int avp_mission_race(int i)
{
    return i >= 0 && i < MISSIONS ? k_missions[i].race : 0;
}

const char *avp_mission_title(int i)
{
    static char t[MISSIONS][32];
    static const char *const race[3] = { "Marine", "Predator", "Alien" };
    int k, n = 0;
    if (i < 0 || i >= MISSIONS) return "";
    if (!t[i][0]) {
        for (k = 0; k <= i; k++)
            if (k_missions[k].race == k_missions[i].race) n++;
        snprintf(t[i], sizeof t[i], "%s mission %d", race[k_missions[i].race], n);
    }
    return t[i];
}

int avp_coop_host_launch(int mission)
{
    uint32_t sh;
    if (mission < 0 || mission >= MISSIONS) return 0;
    if (s_role != ROLE_HOST || s_state != 2 || s_coop_dead || s_mission) return 0;
    sh = shell_state();
    if (!sh || level_is_mission(sh)) return 0;
    if (s_host_pick != mission || !s_peer_ready) return 0;
    if (launch_busy() || s_launch_req >= 0) return 0;
    s_launch_t = GetTickCount64();
    if (!send_msg(MSG_LAUNCH, k_missions[mission].code, 4)) { s_launch_t = 0; coop_fail("send mission"); return 0; }
    InterlockedExchange(&s_launch_req, mission);
    fprintf(stderr, "[COOP] chose mission %s for both players\n", k_missions[mission].code);
    return 1;
}

int avp_coop_host_pick(int mission)
{
    if (mission < 0 || mission >= MISSIONS) return 0;
    if (s_role != ROLE_HOST || s_state != 2 || s_coop_dead || s_mission) return 0;
    if (s_host_pick == mission) return 1;
    if (!send_msg(MSG_PICK, k_missions[mission].code, 4)) { coop_fail("send pick"); return 0; }
    InterlockedExchange(&s_host_pick, mission);
    InterlockedExchange(&s_peer_ready, 0);
    fprintf(stderr, "[COOP] picked mission %s\n", k_missions[mission].code);
    return 1;
}

int avp_coop_peer_ready(void)
{
    return s_role == ROLE_HOST && s_state == 2 && s_peer_ready;
}

int avp_coop_self_ready(void)
{
    return s_role == ROLE_GUEST && s_self_ready;
}

int avp_coop_set_ready(int on)
{
    char r[4] = { 0 };
    if (s_role != ROLE_GUEST || s_state != 2 || s_coop_dead || s_mission || s_guest_mission < 0) return 0;
    r[0] = on ? 1 : 0;
    if (!send_msg(MSG_READY, r, 4)) { coop_fail("send ready"); return 0; }
    InterlockedExchange(&s_self_ready, on ? 1 : 0);
    return 1;
}

const char *avp_coop_guest_mission(void)
{
    LONG m = s_guest_mission;
    if (s_role != ROLE_GUEST || m < 0 || m >= MISSIONS) return NULL;
    return avp_mission_title((int)m);
}

void avp_load_mission(int mission)
{
    if (mission < 0 || mission >= MISSIONS) return;
    if (s_role == ROLE_GUEST) return;
    if (s_role == ROLE_HOST && s_state == 2 && !s_coop_dead) {
        avp_coop_host_launch(mission);
        return;
    }
    InterlockedExchange(&s_launch_req, mission);
}
