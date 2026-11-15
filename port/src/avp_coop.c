#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <time.h>
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
#include <math.h>
#include <string.h>
#include "avp_skirmish.h"
#include "avp_coop.h"

#include "avp_coop_proto.h"
#include "avp_steam.h"

extern ptrdiff_t g_xbox_mem_offset;
static void tx_stop(void);
static void coop_reset_all(void);
static void coop_fail(const char *why);
static int send_msg(uint32_t type, const void *body, uint32_t len);
static void spawn_enqueue(const CpSpawn *sp);
static volatile uint32_t s_step, s_step_a, s_step_b;
static volatile ULONGLONG s_tick_ms;
static volatile uint32_t s_last_esp;
static uint32_t s_tech_sent[64];
static int s_tech_nsent;
#ifdef _WIN32
static HANDLE s_game_thread;
#endif
extern volatile LONG g_avp_pcmenu_up;
#define STEP(id, a, b) (s_step = (id), s_step_a = (a), s_step_b = (b), (void)0)
static void spawn_reset(void);
static void req_push(uint32_t h);
static void unselect_everywhere(uint32_t h);
static void hide_local(uint32_t h);
static int is_hidden(uint32_t h);
static int sp_recent(uint32_t h);

extern uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax);
extern uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n);
extern uint32_t avp_guest_alloca(uint32_t bytes);
extern uint32_t avp_guest_esp(void);
extern void avp_guest_set_esp(uint32_t v);
extern void avp_run_order(int route, uint32_t handle, const uint8_t *msg, uint32_t len);
extern const char *avp_player_name(void);

#define AVP_OBJ_TABLE    CP_OBJ_TABLE
#define AVP_WORLD        CP_WORLD
#define AVP_POINTER      CP_POINTER_G
#define AVP_START_DEATH  0x0003C400u
#define AVP_PTR_GLOBAL2  0x002F67F4u
#define AVP_FACTORY      0x00027690u
#define AVP_SELECT_FN    0x000F0070u
#define AVP_DESELECT_FN  0x000F01A0u
#define AVP_MOVE_FN      0x0002D9F0u
#define AVP_NOWHERE      CP_NOWHERE
#define AVP_GROUND_FN    0x000F0420u
#define AVP_BUY_FN       0x000BF700u
#define AVP_UPG_FN       0x000BFBD0u
#define AVP_LEVEL_RESET  0x0006A8E0u
#define AVP_LEVEL_RECORD 0x00306018u
#define AVP_TECH_TREE    CP_TECH_TREE
#define AVP_TECH_INIT    0x00092E30u
#define AVP_TECH_VT      0x0022E184u
#define SHELL_PTR        0x002F67F0u
#define SHELL_FLAGS      0x210u
#define UNIT_PLAYER      0x474u
#define SEL_MAX          64
#define CFG_MAX          2048u
#define MISSIONS         21

enum { ROLE_NONE, ROLE_HOST, ROLE_GUEST };
enum { MSG_HELLO = 1, MSG_START = 2, MSG_CFG = 6, MSG_LAUNCH = 7, MSG_PICK = 8, MSG_READY = 9, MSG_STATE = 13, MSG_ORDER = 14, MSG_MIRROR = 15, MSG_SPAWN = 16, MSG_SPAWNREQ = 17, MSG_PING = 18, MSG_PONG = 19 };

#define COOP_MAGIC   0x43505641u
#define COOP_VERSION 9u
#define ST_RING      6u
#define ORD_RING     128u
#define STALL_MS     3000u

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int s_role;
static void coop_start_net(void);
static int s_mission_seen, s_mission, s_inited;
static int s_peer_started, s_coop_dead, s_env_skirm;
static SOCKET s_sock = INVALID_SOCKET;
enum { TR_TCP, TR_STEAM };
static int s_tr;
static CRITICAL_SECTION s_lock;
static CRITICAL_SECTION s_send_lock;
static void lk_enter(void);
static int s_send_lock_ready;
volatile LONG g_avp_coop_banner;
static HANDLE s_net_ready, s_cfg_ready, s_thread;
static SOCKET s_listen = INVALID_SOCKET;
static volatile int s_cancel, s_state;
static int s_lock_ready, s_tries = 120, s_wsa_ok;
static volatile int s_cfg_n;
static char s_spec[160], s_text[160], s_peer_name[24], s_peer_addr[48];
static volatile int s_last_net_err;
static volatile LONG s_netlog_n;
static struct { ULONGLONG ms; int dir; uint32_t type, len; } s_netlog[32];
static int s_local_input, s_executing, s_allow_death;
static uint32_t s_kill_q[64];
static int s_kill_n;
static char s_my_map[8];

static uint8_t *s_st_buf[ST_RING];
static uint32_t s_st_len[ST_RING], s_st_head, s_st_tail, s_st_dropseq;
static volatile LONG s_st_skip;
static volatile ULONGLONG s_st_last_ms;
static uint8_t *s_st_scratch;
static CpOrder s_ord[ORD_RING];
static uint32_t s_ord_head, s_ord_tail;
static uint8_t s_ord_raw[ORD_RING][CP_ORDER_WIRE_MAX];
static uint32_t s_ord_rawlen[ORD_RING];
static volatile LONG s_ord_in, s_ord_drop;

static uint64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000000ll / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static void coop_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    coop_start_net();
}

uint64_t avp_coop_now_ms(void)
{
    coop_init();
    return GetTickCount64();
}

static int kill_queue(uint32_t h)
{
    int i;
    for (i = 0; i < s_kill_n; i++)
        if (s_kill_q[i] == h) return 1;
    if (s_kill_n >= 64) return 0;
    s_kill_q[s_kill_n++] = h;
    return 1;
}

static void kill_pump(void)
{
    int i;
    s_allow_death = 1;
    for (i = 0; i < s_kill_n; i++) {
        uint32_t h = s_kill_q[i], obj = cp_obj_of(h);
        if (!obj || G8(obj + 0x26Du) || !cp_obj_ok(obj) || G32(obj + 0x18) != h) continue;
        if (sp_recent(h)) continue;
        STEP(20, h, G32(obj));
        unselect_everywhere(h);
        if (cp_obj_of(h) != obj) continue;
        avp_gcall(AVP_START_DEATH, 0, (uint32_t[]){ h, 1 }, 2, h);
    }
    s_allow_death = 0;
    s_kill_n = 0;
}

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
int avp_coop_role(void)
{
    return s_role;
}
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
    fprintf(stderr, "[COOP] net state (%s): role %d state %d mission %d dead %d last socket error %d\n",
            why, s_role, s_state, s_mission, s_coop_dead, s_last_net_err);
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

static volatile uint32_t s_rtt;
static int s_lag = -1, s_jit;
static volatile int s_lag_live, s_lag_eof;
static int recv_raw(void *p, int n);

typedef struct { uint8_t *d; uint32_t n; ULONGLONG due; } LagFrame;
#define LAG_Q 256u
static LagFrame s_lagq[LAG_Q];
static uint32_t s_lag_h, s_lag_t;
static uint8_t *s_lag_cur;
static uint32_t s_lag_cur_n, s_lag_cur_off;
static HANDLE s_lag_thread;
static ULONGLONG s_lag_last_due;

static void lag_init(void)
{
    if (s_lag < 0) {
        const char *a = getenv("AVP_COOP_TEST_LAG"), *b = getenv("AVP_COOP_TEST_JITTER");
        s_lag = a ? atoi(a) : 0;
        s_jit = b ? atoi(b) : 0;
        if (s_lag < 0) s_lag = 0;
        if (s_jit < 0) s_jit = 0;
        if (s_lag || s_jit) fprintf(stderr, "[COOP] test lag on: %d ms plus up to %d ms jitter on incoming messages\n", s_lag, s_jit);
    }
}

static DWORD WINAPI lag_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        uint32_t hdr[2];
        uint8_t *d;
        ULONGLONG due;
        if (!recv_raw(hdr, sizeof hdr)) break;
        if (hdr[1] > CP_STATE_MAX) break;
        d = (uint8_t *)malloc(sizeof hdr + hdr[1]);
        if (!d) break;
        memcpy(d, hdr, sizeof hdr);
        if (hdr[1] && !recv_raw(d + sizeof hdr, (int)hdr[1])) { free(d); break; }
        due = GetTickCount64() + (ULONGLONG)s_lag + (s_jit ? (ULONGLONG)(rand() % (s_jit + 1)) : 0);
        if (due < s_lag_last_due) due = s_lag_last_due;
        s_lag_last_due = due;
        for (;;) {
            int full;
            lk_enter();
            full = s_lag_t - s_lag_h >= LAG_Q;
            if (!full) {
                s_lagq[s_lag_t % LAG_Q].d = d;
                s_lagq[s_lag_t % LAG_Q].n = (uint32_t)sizeof hdr + hdr[1];
                s_lagq[s_lag_t % LAG_Q].due = due;
                s_lag_t++;
            }
            LeaveCriticalSection(&s_lock);
            if (!full) break;
            if (s_cancel) { free(d); s_lag_eof = 1; return 0; }
            Sleep(5);
        }
    }
    s_lag_eof = 1;
    return 0;
}

static int lag_recv_all(void *p, int n)
{
    uint8_t *c = (uint8_t *)p;
    while (n > 0) {
        if (s_lag_cur && s_lag_cur_off < s_lag_cur_n) {
            uint32_t k = s_lag_cur_n - s_lag_cur_off;
            if (k > (uint32_t)n) k = (uint32_t)n;
            memcpy(c, s_lag_cur + s_lag_cur_off, k);
            s_lag_cur_off += k;
            c += k;
            n -= (int)k;
            continue;
        }
        free(s_lag_cur);
        s_lag_cur = NULL;
        for (;;) {
            LagFrame f;
            int got = 0;
            lk_enter();
            if (s_lag_h != s_lag_t && s_lagq[s_lag_h % LAG_Q].due <= GetTickCount64()) {
                f = s_lagq[s_lag_h % LAG_Q];
                s_lag_h++;
                got = 1;
            }
            LeaveCriticalSection(&s_lock);
            if (got) {
                s_lag_cur = f.d;
                s_lag_cur_n = f.n;
                s_lag_cur_off = 0;
                break;
            }
            if (s_cancel || (s_lag_eof && s_lag_h == s_lag_t)) return 0;
            Sleep(2);
        }
    }
    return 1;
}

static int recv_all(void *p, int n)
{
    if (s_lag_live) return lag_recv_all(p, n);
    return recv_raw(p, n);
}

static int recv_raw(void *p, int n)
{
    char *c = (char *)p;
    if (s_tr == TR_STEAM) return avp_steam_recv(p, n);
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
    if (s_tr == TR_STEAM) {
        ok = avp_steam_send(hdr, sizeof hdr, body, len);
        net_note(0, type, len);
        return ok;
    }
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
    lk_enter();
    memcpy(s_text, t, sizeof s_text);
    s_state = st;
    LeaveCriticalSection(&s_lock);
}
static void lock_init(void);
static void lk_enter(void)
{
    lock_init();
    EnterCriticalSection(&s_lock);
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
    if (s_mission && !s_cancel) InterlockedExchange(&g_avp_coop_banner, 3);
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
    if (s_mission && !s_cancel) InterlockedExchange(&g_avp_coop_banner, 3);
    if (s_cfg_ready) SetEvent(s_cfg_ready);
    if (!s_cancel)
        set_state(3, "%s: %s", s_state == 2 ? "Connection lost" : s_role == ROLE_HOST ? "Could not host" : "Could not connect", why);
    if (s_cancel) return;
    fprintf(stderr, "[COOP] connection lost: %s; carrying on alone\n", why);
    net_dump(why);
}

static void proto_fail(const char *what, uint32_t type, uint32_t len)
{
    fprintf(stderr, "[COOP] malformed %s from the peer (message type %u, length %u); dropping the connection\n", what, type, len);
    if (s_tr == TR_STEAM) avp_steam_link_drop();
    else if (s_sock != INVALID_SOCKET) shutdown(s_sock, SD_BOTH);
    coop_fail("the other game sent malformed data");
}

static void close_listen(void)
{
    lk_enter();
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
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                if (getsockopt(c, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) || soerr) break;
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

static int skip_body(uint32_t len)
{
    uint8_t tmp[512];
    while (len) {
        uint32_t k = len > sizeof tmp ? (uint32_t)sizeof tmp : len;
        if (!recv_all(tmp, (int)k)) return 0;
        len -= k;
    }
    return 1;
}

static void skip_note(const char *what, uint32_t len)
{
    static int told;
    if (told++ < 16) fprintf(stderr, "[COOP] skipped an invalid %s message (length %u)\n", what, len);
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
    if (s_tr == TR_STEAM) {
        char code[16];
        int ok;
        if (s_role == ROLE_HOST) {
            ok = avp_steam_host(&s_cancel, code, sizeof code);
            if (ok) {
                set_state(1, "Your code is %s. Waiting for a player", code);
                ok = avp_steam_wait_peer(&s_cancel);
            }
        } else if (!strncmp(spec, "steam:code:", 11)) {
            ok = avp_steam_join_code(spec + 11, &s_cancel);
        } else {
            ok = avp_steam_join_lobby(strtoull(spec + 12, NULL, 10), &s_cancel);
        }
        if (!ok || s_cancel) {
            if (!s_cancel) coop_fail_text(avp_steam_error(), "%s", *avp_steam_error() ? avp_steam_error() : "Could not connect");
            else coop_fail("no connection");
            SetEvent(s_net_ready);
            return 0;
        }
        {
            char pn[sizeof s_peer_name];
            snprintf(pn, sizeof pn, "%s", avp_steam_peer_name());
            lk_enter();
            snprintf(s_peer_name, sizeof s_peer_name, "%s", pn);
            LeaveCriticalSection(&s_lock);
            snprintf(s_peer_addr, sizeof s_peer_addr, "%s", pn);
        }
        goto link_up;
    }
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
        lk_enter();
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
        fprintf(stderr, "[COOP] %s the host on port %d\n", s_sock == INVALID_SOCKET ? "could not reach" : "joined", port);
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
    {
#ifdef _WIN32
        DWORD to = 4000;
#else
        struct timeval to = { 4, 0 };
#endif
        setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    }
link_up:
    memset(&hello, 0, sizeof hello);
    hello.magic = COOP_MAGIC;
    hello.version = COOP_VERSION;
    hello.role = (uint32_t)s_role;
    snprintf(hello.name, sizeof hello.name, "%s", avp_player_name());
    if (!send_msg(MSG_HELLO, &hello, sizeof hello)) { coop_fail("hello"); SetEvent(s_net_ready); return 0; }
    if (s_role == ROLE_HOST && !send_cfg()) { coop_fail("cfg"); SetEvent(s_net_ready); return 0; }
    set_state(2, "Connected to %s", *s_peer_addr ? s_peer_addr : "the other player");
    SetEvent(s_net_ready);
    lag_init();
    if (s_lag || s_jit) {
        s_lag_eof = 0;
        s_lag_h = s_lag_t = 0;
        s_lag_last_due = 0;
        s_lag_live = 1;
        s_lag_thread = CreateThread(NULL, 0, lag_thread, NULL, 0, NULL);
    }
    for (;;) {
        uint32_t hdr[2];
        if (!recv_all(hdr, sizeof hdr)) { if (s_tr == TR_STEAM && *avp_steam_error()) coop_fail_text(avp_steam_error(), "%s", avp_steam_error()); else coop_fail("closed"); return 0; }
        net_note(1, hdr[0], hdr[1]);
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
                    s_env_skirm = 1;
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
        if (hdr[0] != MSG_STATE && hdr[1] > 4096u) { proto_fail("oversized message", hdr[0], hdr[1]); return 0; }
        if (hdr[0] == MSG_HELLO) {
            if (hdr[1] != sizeof peer || !recv_all(&peer, sizeof peer)) { coop_fail("hello"); return 0; }
            if (peer.magic != COOP_MAGIC || peer.version != COOP_VERSION || peer.role == (uint32_t)s_role) {
                coop_fail("the other game is a different version or the same role");
                return 0;
            }
            peer.name[sizeof peer.name - 1] = 0;
            lk_enter();
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
        } else if (hdr[0] == MSG_STATE) {
            uint32_t slot, full;
            uint8_t *dst;
            if (hdr[1] > CP_STATE_MAX) { proto_fail("state size", hdr[0], hdr[1]); return 0; }
            if (s_role != ROLE_GUEST || hdr[1] < CP_HDR) { if (!skip_body(hdr[1])) { coop_fail("state data"); return 0; } skip_note("state size", hdr[1]); continue; }
            if (!s_st_scratch) s_st_scratch = (uint8_t *)malloc(CP_STATE_MAX);
            lk_enter();
            full = s_st_tail - s_st_head >= ST_RING;
            slot = s_st_tail % ST_RING;
            LeaveCriticalSection(&s_lock);
            if (!s_st_buf[slot]) s_st_buf[slot] = (uint8_t *)malloc(CP_STATE_MAX);
            dst = full ? s_st_scratch : s_st_buf[slot];
            if (!dst) { coop_fail("memory"); return 0; }
            if (!recv_all(dst, (int)hdr[1])) { coop_fail("state data"); return 0; }
            if (!cp_state_validate(dst, hdr[1])) { skip_note("state", hdr[1]); continue; }
            s_st_last_ms = GetTickCount64();
            lk_enter();
            if (full) {
                memcpy(&s_st_dropseq, dst + 4, 4);
                s_st_skip = 1;
            } else {
                s_st_len[slot] = hdr[1];
                s_st_tail++;
            }
            LeaveCriticalSection(&s_lock);
        } else if (hdr[0] == MSG_ORDER) {
            uint8_t body[CP_ORDER_WIRE_MAX];
            CpOrder ord;
            if (s_role != ROLE_HOST || hdr[1] < 4 || hdr[1] > CP_ORDER_WIRE_MAX) { if (!skip_body(hdr[1])) { coop_fail("order data"); return 0; } skip_note("order size", hdr[1]); continue; }
            if (!recv_all(body, (int)hdr[1])) { coop_fail("order data"); return 0; }
            if (!cp_order_parse(body, hdr[1], &ord) || ord.cmd == CP_CMD_REINF) { skip_note("order", hdr[1]); continue; }
            lk_enter();
            if (s_ord_tail - s_ord_head < ORD_RING) {
                ord.player = 1;
                s_ord[s_ord_tail % ORD_RING] = ord;
                memcpy(s_ord_raw[s_ord_tail % ORD_RING], body, hdr[1]);
                s_ord_rawlen[s_ord_tail % ORD_RING] = hdr[1];
                s_ord_tail++;
                InterlockedIncrement(&s_ord_in);
            } else {
                InterlockedIncrement(&s_ord_drop);
            }
            LeaveCriticalSection(&s_lock);
        } else if (hdr[0] == MSG_MIRROR) {
            uint8_t body[CP_ORDER_WIRE_MAX + 4];
            CpOrder ord;
            uint32_t pl;
            if (s_role != ROLE_GUEST || hdr[1] < 8 || hdr[1] > CP_ORDER_WIRE_MAX + 4) { if (!skip_body(hdr[1])) { coop_fail("mirror data"); return 0; } skip_note("mirror size", hdr[1]); continue; }
            if (!recv_all(body, (int)hdr[1])) { coop_fail("mirror data"); return 0; }
            memcpy(&pl, body, 4);
            if (pl > 1u || !cp_order_parse(body + 4, hdr[1] - 4, &ord) || ord.cmd == CP_CMD_GIVE) { skip_note("mirror", hdr[1]); continue; }
            ord.player = pl;
            lk_enter();
            if (s_ord_tail - s_ord_head < ORD_RING) {
                s_ord[s_ord_tail % ORD_RING] = ord;
                s_ord_rawlen[s_ord_tail % ORD_RING] = 0;
                s_ord_tail++;
            }
            LeaveCriticalSection(&s_lock);
        } else if (hdr[0] == MSG_SPAWN) {
            uint8_t body[CP_SPAWN_WIRE_MAX];
            CpSpawn sp;
            if (s_role != ROLE_GUEST || hdr[1] < CP_SPAWN_HDR || hdr[1] > CP_SPAWN_WIRE_MAX) { if (!skip_body(hdr[1])) { coop_fail("spawn data"); return 0; } skip_note("spawn size", hdr[1]); continue; }
            if (!recv_all(body, (int)hdr[1])) { coop_fail("spawn data"); return 0; }
            if (cp_spawn_parse(body, hdr[1], &sp)) {
                spawn_enqueue(&sp);
            } else {
                static int bad;
                if (bad++ < 8)
                    fprintf(stderr, "[COOP] spawn skipped (len %u kind %u n %u w0 %08X w3 %08X w4 %08X w6 %08X w9 %08X)\n", hdr[1],
                            rd_u32(body), rd_u32(body + 12), hdr[1] >= 32 ? rd_u32(body + 28) : 0, hdr[1] >= 44 ? rd_u32(body + 40) : 0,
                            hdr[1] >= 48 ? rd_u32(body + 44) : 0, hdr[1] >= 56 ? rd_u32(body + 52) : 0, hdr[1] >= 68 ? rd_u32(body + 64) : 0);
            }
        } else if (hdr[0] == MSG_PING || hdr[0] == MSG_PONG) {
            uint32_t ts = 0;
            if (hdr[1] != 4 || !recv_all(&ts, 4)) { coop_fail("ping"); return 0; }
            if (hdr[0] == MSG_PING) {
                send_msg(MSG_PONG, &ts, 4);
            } else {
                uint32_t r = (uint32_t)GetTickCount() - ts;
                if (r < 60000u) s_rtt = s_rtt ? (s_rtt * 7u + r) / 8u : r;
            }
        } else if (hdr[0] == MSG_SPAWNREQ) {
            uint32_t rh = 0;
            if (hdr[1] != 4 || !recv_all(&rh, 4)) { if (hdr[1] != 4 && skip_body(hdr[1])) { skip_note("spawn request", hdr[1]); continue; } coop_fail("spawn request"); return 0; }
            if (s_role == ROLE_HOST) req_push(rh);
        } else {
            if (!skip_body(hdr[1])) { coop_fail("unknown message"); return 0; }
            skip_note("unknown", hdr[1]);
        }
    }
}

static void net_begin(const char *spec, int role, int tries, const char *status)
{
    lock_init();
    snprintf(s_spec, sizeof s_spec, "%s", spec);
    lk_enter();
    s_st_head = s_st_tail = 0;
    s_ord_head = s_ord_tail = 0;
    s_st_skip = 0;
    LeaveCriticalSection(&s_lock);
    s_coop_dead = 0;
    s_peer_started = 0;
    s_peer_map[0] = 0;
    s_my_map[0] = 0;
    s_cancel = 0;
    s_tries = tries;
    s_peer_name[0] = 0;
    s_peer_addr[0] = 0;
    s_sock = INVALID_SOCKET;
    s_tr = !strncmp(spec, "steam:", 6) ? TR_STEAM : TR_TCP;
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
    int i;
    if (s_sock != INVALID_SOCKET) fprintf(stderr, "[COOP] closing the connection on purpose (role %d, mission %d)\n", s_role, s_mission);
    s_cancel = 1;
    close_listen();
    if (s_sock != INVALID_SOCKET) shutdown(s_sock, SD_BOTH);
    if (s_tr == TR_STEAM) avp_steam_link_drop();
    tx_stop();
    if (s_thread) {
        for (i = 0; i < 100 && WaitForSingleObject(s_thread, 100) != WAIT_OBJECT_0; i++) {
        }
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    if (s_lag_thread) {
        WaitForSingleObject(s_lag_thread, 3000);
        CloseHandle(s_lag_thread);
        s_lag_thread = NULL;
    }
    s_lag_live = 0;
    lk_enter();
    while (s_lag_h != s_lag_t) free(s_lagq[s_lag_h++ % LAG_Q].d);
    LeaveCriticalSection(&s_lock);
    free(s_lag_cur);
    s_lag_cur = NULL;
    s_lag_cur_n = s_lag_cur_off = 0;
    s_rtt = 0;
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    if (s_tr == TR_STEAM) avp_steam_leave();
    s_tr = TR_TCP;
    close_listen();
    if (s_net_ready) {
        CloseHandle(s_net_ready);
        s_net_ready = NULL;
    }
    if (s_cfg_ready) {
        CloseHandle(s_cfg_ready);
        s_cfg_ready = NULL;
    }
    s_role = ROLE_NONE;
    s_coop_dead = 0;
    s_launch_t = 0;
    s_peer_name[0] = 0;
    s_peer_addr[0] = 0;
    s_cfg_n = 0;
    InterlockedExchange(&s_launch_req, -1);
    InterlockedExchange(&s_guest_mission, -1);
    InterlockedExchange(&s_host_pick, -1);
    InterlockedExchange(&s_peer_ready, 0);
    InterlockedExchange(&s_self_ready, 0);
    s_peer_map[0] = 0;
    if (s_lock_ready) set_state(0, "");
}

extern volatile LONG g_avp_mainmenu;

static int net_allowed(void)
{
    coop_init();
    if (s_mission || s_mission_seen) {
        if (g_avp_mainmenu) return 1;
        lock_init();
        set_state(3, "Leave the current mission before hosting or joining");
        return 0;
    }
    return 1;
}

int avp_coop_host(int port)
{
    char spec[32], status[64];
    if (!net_allowed()) return 0;
    coop_reset_all();
    if (port < 1 || port > 65535) port = 27015;
    snprintf(spec, sizeof spec, "host:%d", port);
    if (!wsa_ensure()) return 0;
    snprintf(status, sizeof status, "Waiting for a player on port %d", port);
    net_begin(spec, ROLE_HOST, 1, status);
    return 1;
}

int avp_coop_join(const char *ip, int port)
{
    char spec[160], status[160];
    if (!net_allowed()) return 0;
    coop_reset_all();
    if (!ip || !*ip || strchr(ip, ':')) ip = "127.0.0.1";
    if (port < 1 || port > 65535) port = 27015;
    snprintf(spec, sizeof spec, "join:%.100s:%d", ip, port);
    if (!wsa_ensure()) return 0;
    snprintf(status, sizeof status, "Connecting to %.100s:%d", ip, port);
    net_begin(spec, ROLE_GUEST, 5, status);
    return 1;
}

int avp_coop_version(void)
{
    return (int)COOP_VERSION;
}

int avp_coop_host_steam(void)
{
    if (!net_allowed()) return 0;
    coop_reset_all();
    if (avp_steam_status() != AVP_STEAM_READY) {
        lock_init();
        set_state(3, "Connect to Steam first");
        return 0;
    }
    net_begin("steam:host", ROLE_HOST, 1, "Creating the lobby");
    return 1;
}

int avp_coop_join_steam_code(const char *code)
{
    char spec[48];
    if (!net_allowed()) return 0;
    coop_reset_all();
    if (avp_steam_status() != AVP_STEAM_READY) {
        lock_init();
        set_state(3, "Connect to Steam first");
        return 0;
    }
    if (!avp_steam_valid_code(code)) {
        set_state(3, "Enter the 6 character code");
        return 0;
    }
    snprintf(spec, sizeof spec, "steam:code:%s", code);
    net_begin(spec, ROLE_GUEST, 1, "Looking for the lobby");
    return 1;
}

int avp_coop_join_steam_lobby(unsigned long long lobby)
{
    char spec[48];
    if (!net_allowed()) return 0;
    coop_reset_all();
    if (avp_steam_status() != AVP_STEAM_READY) {
        lock_init();
        set_state(3, "Connect to Steam first");
        return 0;
    }
    snprintf(spec, sizeof spec, "steam:lobby:%llu", lobby);
    net_begin(spec, ROLE_GUEST, 1, "Joining the lobby");
    return 1;
}

void avp_coop_cancel(void)
{
    coop_init();
    if (s_mission || launch_busy()) return;
    coop_reset_all();
}

int avp_coop_state(void)
{
    return s_state;
}

const char *avp_coop_state_text(void)
{
    static char t[sizeof s_text];
    if (!s_lock_ready) return "";
    lk_enter();
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

static uint8_t s_owner[0x10000];
static uint32_t s_owner_h[0x10000];
static int s_vs, s_vs_slot[2];

static int local_player(void)
{
    return s_role == ROLE_GUEST ? 1 : 0;
}

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
    uint32_t i = handle >> 16, obj;
    if (!i) return -1;
    if (s_vs) {
        int slot;
        obj = cp_obj_of(handle);
        if (!obj) return -1;
        slot = (int)G32(obj + UNIT_PLAYER);
        return slot == s_vs_slot[0] ? 0 : slot == s_vs_slot[1] ? 1 : -1;
    }
    return s_owner_h[i] == handle ? s_owner[i] : 0;
}

static void owner_set(uint32_t handle, int owner)
{
    uint32_t i = handle >> 16;
    if (s_vs || !i) return;
    if (owner) {
        s_owner_h[i] = handle;
        s_owner[i] = 1;
    } else if (s_owner_h[i] == handle) {
        s_owner[i] = 0;
    }
}

static int coop_live(void)
{
    return s_role != ROLE_NONE && !s_coop_dead && s_mission;
}

static int guest_live(void)
{
    return s_role == ROLE_GUEST && !s_coop_dead && s_mission;
}

static int hk_owner(uint32_t h)
{
    return owner_of(h);
}

static void hk_owner_set(uint32_t h, int o)
{
    owner_set(h, o);
}

static void hk_kill(uint32_t h)
{
    if (sp_recent(h)) return;
    if (g_cp_kill_orphan) { hide_local(h); return; }
    kill_queue(h);
}

static void vt_name(uint32_t vt, char *nm, size_t cap)
{
    uint32_t col, td, k = 0;
    nm[0] = 0;
    if (vt < 0x10020u || vt >= CP_LOCAL_HI - 0x100u) return;
    col = G32(vt - 4u);
    td = col >= 0x10000u && col < CP_LOCAL_HI - 0x100u ? G32(col + 12u) : 0;
    if (td >= 0x10000u && td < CP_LOCAL_HI - 0x100u)
        while (k + 1 < cap && G8(td + 8u + k)) { nm[k] = (char)G8(td + 8u + k); k++; }
    nm[k] = 0;
}

static void um_note(uint32_t handle, uint32_t vt, const float *pos);

static void hk_unmatched(uint32_t vt, uint32_t handle, const float *pos)
{
    static uint32_t seen[32];
    static int n;
    int i;
    char nm[48];
    if (!(cp_class(vt) & CF_UNIT) || (cp_class(vt) & CF_HELPER)) return;
    um_note(handle, vt, pos);
    for (i = 0; i < n; i++)
        if (seen[i] == vt) return;
    if (n < 32) seen[n++] = vt;
    vt_name(vt, nm, sizeof nm);
    fprintf(stderr, "[COOP] no object here for host class %08X %s; waiting for its spawn event\n", vt, nm);
}

static void call_vec(uint32_t obj, uint32_t slot, const float *v)
{
    uint32_t esp, va, i, vt;
    uint32_t fn;
    if (!cp_obj_ok(obj) || G8(obj + 0x26Du)) return;
    esp = avp_guest_esp();
    va = avp_guest_alloca(16);
    vt = G32(obj);
    fn = vt >= 0x10000u && vt < CP_LOCAL_HI - 0x400u ? G32(vt + slot * 4u) : 0;
    if (fn < 0x00011000u || fn >= 0x00170D60u) { avp_guest_set_esp(esp); return; }
    for (i = 0; i < 3; i++) GF(va + 4 * i) = v[i];
    avp_gcall(fn, obj, (uint32_t[]){ va }, 1, 0);
    avp_guest_set_esp(esp);
}

static void hk_setpos(uint32_t obj, const float *v)
{
    call_vec(obj, 22, v);
}

static uint32_t s_hidden[128];
static int s_hidden_n;

static int is_hidden(uint32_t h)
{
    int i;
    for (i = 0; i < s_hidden_n; i++)
        if (s_hidden[i] == h) return 1;
    return 0;
}

static void hide_local(uint32_t h)
{
    int i;
    uint32_t obj = cp_obj_of(h);
    static const float far_away[3] = { -20000.0f, -20000.0f, -20000.0f };
    if (!obj || G8(obj + 0x26Du)) return;
    for (i = 0; i < s_hidden_n; i++)
        if (s_hidden[i] == h) return;
    if (s_hidden_n < 128) s_hidden[s_hidden_n++] = h;
    unselect_everywhere(h);
    hk_setpos(obj, far_away);
    {
        static int told;
        if (told++ < 12) fprintf(stderr, "[COOP] a local object the host does not have was moved out of the map instead of being destroyed\n");
    }
}

static void hk_setrot(uint32_t obj, const float *v)
{
    call_vec(obj, 23, v);
}

static const CpHooks s_hooks = { hk_owner, hk_owner_set, hk_kill, hk_unmatched, hk_setpos, hk_setrot };

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

static volatile LONG s_ord_sent, s_ord_exec, s_ord_rej;

static uint32_t rd32le(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int hostauth_block(void)
{
    return guest_live() && !s_allow_death;
}

int avp_coop_block_death(uint32_t unit)
{
    (void)unit;
    return hostauth_block();
}

int avp_coop_block_death_h(uint32_t handle)
{
    (void)handle;
    return hostauth_block();
}

int avp_coop_skip_ai(uint32_t ai)
{
    static int on = -1;
    uint32_t npc;
    if (on < 0) {
        const char *e = getenv("AVP_COOP_SKIP_AI");
        on = e && *e && *e != '0';
    }
    if (!on || !guest_live() || !cp_obj_ok(ai)) return 0;
    npc = G32(ai + 0x188);
    if (!cp_obj_ok(npc)) return 0;
    return owner_of(G32(npc + 0x18)) != 1;
}

static int sane_guest_ptr(uint32_t v)
{
    return v >= 0x10000u && v < CP_LOCAL_HI - 0x100u;
}

static int friend_player(void)
{
    uint32_t world = G32(AVP_WORLD), pm;
    if (!sane_guest_ptr(world)) return -1;
    pm = G32(world + 0x78);
    if (!sane_guest_ptr(pm)) return -1;
    return (int)(G32(pm + 0x18) & 0xFFFFu);
}

int avp_coop_marked(uint32_t handle)
{
    uint32_t obj;
    int o, fs;
    if (!coop_live()) return 0;
    obj = cp_obj_of(handle);
    if (!obj || !(cp_class(G32(obj)) & CF_UNIT) || (cp_class(G32(obj)) & CF_STATIC) || is_hidden(handle)) return 0;
    o = owner_of(handle);
    if (s_vs) {
        if (o < 0) return 0;
    } else {
        fs = friend_player();
        if (fs < 0 || (int)(handle & 0xFFFFu) != fs) return 0;
    }
    return o == local_player() ? 2 : 1;
}

int avp_coop_is_static(uint32_t handle)
{
    uint32_t obj = cp_obj_of(handle);
    return obj && ((cp_class(G32(obj)) & CF_STATIC) || is_hidden(handle)) ? 1 : 0;
}

int avp_coop_may_select(uint32_t handle)
{
    if (avp_coop_is_static(handle) || is_hidden(handle)) return 0;
    if (!coop_live() || s_executing) return 1;
    return owner_of(handle) == local_player();
}

int avp_coop_replay_ground(void)
{
    return coop_live() && s_executing;
}

int avp_coop_live(void)
{
    return coop_live();
}

static void ground_refresh(uint32_t ptr)
{
    uint32_t regs[6] = { 0 };
    regs[5] = ptr;
    avp_gcall_regs(AVP_GROUND_FN, regs, NULL, 0);
}

static void send_order(const uint8_t *buf, uint32_t len)
{
    if (!send_msg(MSG_ORDER, buf, len)) {
        coop_fail("send order");
        return;
    }
    InterlockedIncrement(&s_ord_sent);
}

static void send_mirror(uint32_t player, const uint8_t *buf, uint32_t len)
{
    uint8_t m[CP_ORDER_WIRE_MAX + 4];
    if (len > CP_ORDER_WIRE_MAX) return;
    memcpy(m, &player, 4);
    memcpy(m + 4, buf, len);
    if (!send_msg(MSG_MIRROR, m, len + 4)) coop_fail("send mirror");
}

static void emit(const uint8_t *buf, uint32_t len)
{
    if (s_role == ROLE_GUEST) send_order(buf, len);
    else send_mirror(0, buf, len);
}

static uint32_t map_h(uint32_t h)
{
    return s_role == ROLE_HOST ? h : cp_map_to_host(h);
}

static void pointer_aim(uint32_t ptr, uint32_t *under, float *world)
{
    uint32_t i, h = G32(ptr + 0xDC);
    *under = h ? map_h(h) : 0;
    for (i = 0; i < 3; i++) {
        float v = GF(ptr + 0x1A8 + 4 * i);
        world[i] = (v == v && v > -1.0e6f && v < 1.0e6f) ? v : 0.0f;
    }
}

static uint32_t s_sel_sent[SEL_MAX], s_nsel_sent;

static int cap_mode(void)
{
    if (!s_local_input || s_executing) return 0;
    if (guest_live()) return 1;
    if (s_role == ROLE_HOST && coop_live()) return 2;
    return 0;
}

static void sel_sync(void)
{
    uint32_t cl[SEL_MAX], hh[SEL_MAX], n, i, m = 0;
    uint8_t buf[8 + 4 * SEL_MAX];
    uint32_t ptr = G32(AVP_POINTER);
    if (!coop_live() || !sane_guest_ptr(ptr)) return;
    n = cp_read_selection(ptr, cl, SEL_MAX);
    for (i = 0; i < n; i++) {
        uint32_t h = map_h(cl[i]);
        if (h) hh[m++] = h;
    }
    if (m == s_nsel_sent && !memcmp(hh, s_sel_sent, m * 4u)) return;
    emit(buf, cp_enc_select(buf, hh, m));
    memcpy(s_sel_sent, hh, m * 4u);
    s_nsel_sent = m;
}

static int order_has_no_dest(uint32_t order)
{
    return G32(order + 0x10) == G32(AVP_NOWHERE) && G32(order + 0x14) == G32(AVP_NOWHERE + 4u)
        && G32(order + 0x18) == G32(AVP_NOWHERE + 8u);
}

static int ord_stack_ok(uint32_t v, uint32_t esp)
{
    return sane_guest_ptr(v) || (v >= esp - 0x4000u && v < esp + 0x4000u);
}

int avp_coop_take_move(uint32_t esp)
{
    uint8_t buf[64];
    uint32_t ptr, ord, part, under, nowhere, mtype, particle = 0xFFu, queue;
    float pos[3] = { 0, 0, 0 }, world[3];
    int pi, cap = cap_mode();
    if (!cap) return 0;
    ptr = G32(esp + 4);
    ord = G32(esp + 8);
    part = G32(esp + 12);
    queue = (G32(esp + 16) & 0xFFu) != 0;
    if (!sane_guest_ptr(ptr) || ptr != G32(AVP_POINTER) || !ord_stack_ok(ord, esp)) {
        static int told;
        if (told++ < 5) fprintf(stderr, "[COOP] move not captured: pointer %08X (native %08X) message %08X esp %08X\n", ptr, G32(AVP_POINTER), ord, esp);
        return cap == 1;
    }
    mtype = G32(ord);
    if (mtype != 0x60039u && mtype != 0x6007Cu) {
        static int told;
        if (told++ < 5) fprintf(stderr, "[COOP] move order type %08X is not forwarded\n", mtype);
        return cap == 1;
    }
    nowhere = order_has_no_dest(ord) ? 1u : 0u;
    if (nowhere) {
        ground_refresh(ptr);
    } else {
        uint32_t i;
        for (i = 0; i < 3; i++) {
            float v = GF(ord + 0x10 + 4 * i);
            pos[i] = (v == v && v > -1.0e6f && v < 1.0e6f) ? v : 0.0f;
        }
    }
    if (part) {
        pi = cp_particle_index(part - ptr);
        if (pi < 0) return cap == 1;
        particle = (uint32_t)pi;
    }
    sel_sync();
    pointer_aim(ptr, &under, world);
    emit(buf, cp_enc_move(buf, mtype == 0x6007Cu ? 1u : 0u, nowhere, pos, queue, particle, under, world));
    return cap == 1;
}

int avp_coop_take_pcall(uint32_t fn, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx,
                        uint32_t esi, uint32_t edi)
{
    uint8_t buf[32];
    uint32_t ptr, param = 0, under;
    float world[3];
    int idx = cp_pcall_find(fn), cap;
    (void)eax; (void)ecx; (void)edx; (void)ebx; (void)esi;
    if (idx < 0) return 0;
    cap = cap_mode();
    if (!cap) return 0;
    ptr = G32(AVP_POINTER);
    if (!sane_guest_ptr(ptr)) return 0;
    if (fn == 0x0002CAC0u) {
        param = (G32(esp + 8) & 0xFFu) != 0;
        ground_refresh(ptr);
    } else if (fn == 0x0002E130u) {
        param = edi;
    }
    if (!cp_pcall_param_ok((uint32_t)idx, param)) return cap == 1;
    sel_sync();
    pointer_aim(ptr, &under, world);
    emit(buf, cp_enc_pcall(buf, (uint32_t)idx, param, under, world));
    return cap == 1;
}

int avp_coop_take_order(uint32_t unit, uint32_t msg)
{
    uint8_t raw[CP_MSG_MAX], buf[24 + CP_MSG_MAX];
    uint32_t type, size, h, hh, i, entry;
    int cap = cap_mode();
    if (!cap || !sane_guest_ptr(unit) || !sane_guest_ptr(msg)) return 0;
    h = G32(unit + 0x18);
    if (cap == 2 && owner_of(h) == 1) return 0;
    if (cap == 1 && owner_of(h) != 1) return 1;
    type = G32(msg);
    if (!cp_order_type_ok(type)) {
        static int told;
        if (told++ < 8) {
            uint32_t e = CP_MSG_TYPES + (type & 0xFFu) * 12u, sz = G32(e) ? G32(e + 4u) : 0, w;
            fprintf(stderr, "[COOP] order type %08X is not forwarded (size %u):", type, sz);
            for (w = 0; w < 12u && w * 4u < sz; w++) fprintf(stderr, " %08X", G32(msg + 4u * w));
            fprintf(stderr, "\n");
        }
        return cap == 1;
    }
    entry = CP_MSG_TYPES + (type & 0xFFu) * 12u;
    size = G32(entry) ? G32(entry + 4u) : 0;
    if (size < 12u || size > CP_MSG_MAX || (size & 3u)) return cap == 1;
    for (i = 0; i < size; i++) raw[i] = G8(msg + i);
    if (cap == 1 && !cp_order_xlate_out(type, raw, size)) return 1;
    hh = map_h(h);
    if (!hh) return cap == 1;
    sel_sync();
    emit(buf, cp_enc_order(buf, hh, type, raw[8] & 15u, raw, size));
    return cap == 1;
}

int avp_coop_take_reinf(uint32_t who, uint32_t esp)
{
    uint8_t buf[48];
    uint32_t types[7], i, drop, dh = 0;
    int cap = cap_mode();
    (void)who;
    if (!cap) return 0;
    for (i = 0; i < 7; i++) types[i] = G32(0x002F6A90u + 4 * i);
    if (cap == 1) {
        static int told;
        if (told++ < 3) fprintf(stderr, "[COOP] reinforcements are ordered by the host; the guest's request is dropped\n");
        for (i = 0; i < 7; i++) G32(0x002F6A90u + 4 * i) = 0;
        return 1;
    }
    drop = G32(esp + 12);
    if (sane_guest_ptr(drop) && cp_obj_ok(drop)) dh = G32(drop + 0x18);
    emit(buf, cp_enc_reinf(buf, G32(esp + 4), G32(esp + 8), dh, types));
    return 0;
}

int avp_coop_take_buy(uint32_t self)
{
    uint8_t buf[8];
    uint32_t who, world, pm, i;
    int cap = cap_mode();
    if (!cap || !sane_guest_ptr(self)) return 0;
    who = G32(self + 0x18) & 0xFFFFu;
    world = G32(AVP_WORLD);
    if (who >= 0x20u || !sane_guest_ptr(world)) return 0;
    pm = G32(world + CP_PM_ARRAY + 4u * who);
    if (!sane_guest_ptr(pm)) return 0;
    for (i = 0; i < 0x14u; i++)
        if (G32(pm + 0x1D4 + 4 * i) == self) {
            emit(buf, cp_enc_buy(buf, i));
            return cap == 1;
        }
    return 0;
}

static int text_ptr(uint32_t v)
{
    return v >= 0x00011000u && v < 0x00170000u;
}

static int rdata_ptr(uint32_t v)
{
    return v >= 0x002050C0u && v < 0x00310680u && !(v & 3u);
}

int avp_coop_parent_bad(uint32_t obj)
{
    uint32_t p, hops;
    static int told;
    if (!coop_live() || !obj) return 0;
    if (!cp_obj_ok(obj)) return 0;
    p = G32(obj + 8u);
    for (hops = 0; hops < 64u; hops++) {
        if (!p) return 0;
        if (!cp_obj_ok(p) || p == obj) goto bad;
        p = G32(p + 8u);
    }
bad:
    if (told++ < 10) fprintf(stderr, "[COOP] skipped an object whose parent chain is cyclic or damaged (%08X)\n", obj);
    return 1;
}

int avp_coop_mover_pre(uint32_t m)
{
    uint32_t vt, own, ov;
    static int told;
    if (!coop_live()) return 0;
    if (!cp_obj_ok(m)) goto bad;
    vt = G32(m);
    if (!rdata_ptr(vt) || !text_ptr(G32(vt + 0x9Cu))) goto bad;
    own = G32(m + 0x104u);
    if (own) {
        if (!cp_obj_ok(own)) goto bad;
        ov = G32(own);
        if (!rdata_ptr(ov) || !text_ptr(G32(ov + 0x4Cu)) || !text_ptr(G32(ov + 0x50u))) goto bad;
    }
    return 0;
bad:
    if (told++ < 10) fprintf(stderr, "[COOP] skipped a mover update whose unit or class data is damaged (mover %08X)\n", m);
    return 1;
}

int avp_rtti_bad(uint32_t esp)
{
    uint32_t obj = G32(esp + 4), vt, col, chd, n, bca, i;
    static int told;
    if (!coop_live() || !obj) return 0;
    if (!cp_obj_ok(obj)) goto bad;
    vt = G32(obj);
    if (!rdata_ptr(vt)) goto bad;
    col = G32(vt - 4u);
    if (!rdata_ptr(col)) goto bad;
    chd = G32(col + 16u);
    if (!rdata_ptr(chd)) goto bad;
    n = G32(chd + 8u);
    bca = G32(chd + 12u);
    if (n == 0 || n > 64u || !rdata_ptr(bca)) goto bad;
    for (i = 0; i < n; i++) {
        uint32_t bcd = G32(bca + 4u * i);
        if (!rdata_ptr(bcd) || !rdata_ptr(G32(bcd))) goto bad;
    }
    if (G32(vt) < 0x00011000u || G32(vt) >= 0x00170000u || G32(vt + 4u) < 0x00011000u || G32(vt + 4u) >= 0x00170000u) goto bad;
    return 0;
bad:
    if (told++ < 10)
        fprintf(stderr, "[COOP] dynamic cast on a damaged object %08X (class data invalid), returning null instead of looping; caller %08X\n", obj, G32(esp));
    return 1;
}

int avp_coop_tech_pre(uint32_t esp)
{
    uint8_t buf[12 + 4 * 16];
    uint32_t obj = G32(esp + 4), ids, n, i, tmp[16];
    if (s_role != ROLE_HOST || !coop_live() || s_executing) return 0;
    if (!sane_guest_ptr(obj)) return 0;
    n = G32(obj + 0x3C);
    ids = G32(obj + 0x38);
    if (n == 0 || n > 16u || !sane_guest_ptr(ids)) return 0;
    for (i = 0; i < n; i++) tmp[i] = G32(ids + 4u * i);
    {
        uint32_t tt = G32(AVP_TECH_TREE), r, pending = G8(obj + 0x85) ? 0u : 1u;
        int k;
        if (tt >= 0x10000u && tt < CP_LOCAL_HI - 0x1000u)
            for (r = 0; G32(tt + 0x38 + r * 8u) && r < 256u; r++)
                for (i = 0; i < n; i++)
                    if (G32(tt + 0x38 + r * 8u) == tmp[i] && !G8(tt + 0x3Eu + r * 8u)) pending = 1;
        if (!pending) return 0;
        for (k = 0; k < s_tech_nsent; k++)
            if (s_tech_sent[k] == obj) return 0;
        if (s_tech_nsent < 64) s_tech_sent[s_tech_nsent++] = obj;
    }
    fprintf(stderr, "[COOP] host applies a tech upgrade (%u rows, first %08X), mirrored to the guest\n", n, tmp[0]);
    emit(buf, cp_enc_tech(buf, G32(obj), tmp, n));
    return 0;
}

static uint32_t tech_row_of(uint32_t id)
{
    uint32_t tt = G32(AVP_TECH_TREE), r, first = 0;
    if (tt < 0x10000u || tt >= CP_LOCAL_HI - 0x1000u) return 0;
    for (r = 0; G32(tt + 0x38 + r * 8u) && r < 256u; r++) {
        uint32_t row = tt + 0x38u + r * 8u;
        if (G32(row) != id) continue;
        if (G8(row + 7u)) return row;
        if (!first) first = row;
    }
    return first;
}

static int tech_row_up(uint32_t id)
{
    uint32_t row = tech_row_of(id);
    return row ? (G8(row + 6u) ? 1 : 0) : -1;
}

static uint32_t s_t1_sent[64];
static int s_t1_n;

int avp_coop_tech1_pre(uint32_t esp, uint32_t id)
{
    uint8_t buf[16];
    uint32_t obj = G32(esp + 4), ret = G32(esp);
    int k;
    if (s_role != ROLE_HOST || !coop_live() || s_executing) return 0;
    if (tech_row_up(id) != 0) return 0;
    for (k = 0; k < s_t1_n; k++)
        if (s_t1_sent[k] == id) return 0;
    if (s_t1_n < 64) s_t1_sent[s_t1_n++] = id;
    {
        uint32_t h = sane_guest_ptr(obj) ? G32(obj + 0x18) : 0, vt = sane_guest_ptr(obj) ? G32(obj) : 0;
        fprintf(stderr, "[COOP] host applies tech row %08X as upgraded (caller %08X), mirrored to the guest\n", id, ret);
        emit(buf, cp_enc_tech1(buf, id, h, vt));
    }
    return 0;
}

int avp_coop_tech2_pre(uint32_t self, uint32_t fn)
{
    uint8_t buf[20];
    uint32_t id, h, vt;
    if (!sane_guest_ptr(self) || !coop_live() || s_executing) return 0;
    id = G32(self + 0x280);
    h = G32(self + 0x18);
    vt = G32(self);
    if (s_role == ROLE_HOST) {
        int k;
        if (tech_row_up(id) != 0) return 0;
        for (k = 0; k < s_t1_n; k++)
            if (s_t1_sent[k] == id) return 0;
        if (s_t1_n < 64) s_t1_sent[s_t1_n++] = id;
        fprintf(stderr, "[COOP] host runs the tech action %05X for row %08X, mirrored to the guest\n", fn, id);
        emit(buf, cp_enc_tech2(buf, fn, h, vt, id));
        return 0;
    }
    if (s_role == ROLE_GUEST && s_local_input) {
        uint32_t hh = cp_map_to_host(h);
        fprintf(stderr, "[COOP] guest pressed the tech action %05X for row %08X, forwarded to the host\n", fn, id);
        emit(buf, cp_enc_tech2(buf, fn, hh ? hh : h, vt, id));
        return 1;
    }
    return 0;
}

static uint32_t s_t_applied[64];
static int s_t_applied_n;

static int tech_was_applied(uint32_t id)
{
    int i;
    for (i = 0; i < s_t_applied_n; i++)
        if (s_t_applied[i] == id) return 1;
    return 0;
}

static void tech_mark_row(uint32_t id)
{
    uint32_t row = tech_row_of(id);
    if (!row) return;
    G8(row + 6u) = 1;
    G8(row + 5u) = 0;
}

static void tech_apply_direct(uint32_t fn, uint32_t id)
{
    uint32_t idx, n = 0, i, hit = 0, seen = 0, esp = avp_guest_esp(), list[512];
    tech_mark_row(id);
    for (idx = 1; idx < 0x10000u && n < 512u; idx++) {
        uint32_t obj = G32(AVP_OBJ_TABLE + idx * 4u), vt, cls, f1;
        if (!cp_obj_ok(obj)) continue;
        vt = G32(obj);
        if (vt < 0x002050C0u || vt >= 0x00265D60u || (vt & 3u)) continue;
        cls = cp_class(vt);
        if (!(cls & CF_AVP) || (cls & CF_HELPER)) continue;
        if (G8(obj + 0x26Du) || G8(obj + 0x26Au)) continue;
        seen++;
        if (fn == 0x00052FE0u) {
            uint32_t a[1];
            f1 = G32(vt + 0x1Cu);
            if (f1 < 0x00011000u || f1 >= 0x00170000u || G32(vt + 0x110u) < 0x00011000u || G32(vt + 0x110u) >= 0x00170000u) continue;
            a[0] = id;
            if (!(avp_gcall(f1, obj, a, 1, 0) & 0xFFu)) { avp_guest_set_esp(esp); continue; }
            avp_guest_set_esp(esp);
        } else if (!((cls & CF_UNIT) && G32(obj + 0xA20u))) {
            continue;
        }
        list[n++] = G32(obj + 0x18);
    }
    s_allow_death = 1;
    for (i = 0; i < n; i++) {
        uint32_t obj = cp_obj_of(list[i]), vt;
        if (!obj || G8(obj + 0x26Du) || G8(obj + 0x26Au)) continue;
        vt = G32(obj);
        if (vt < 0x002050C0u || vt >= 0x00265D60u) continue;
        if (fn == 0x00052FE0u) {
            uint32_t f2 = G32(vt + 0x110u);
            if (f2 < 0x00011000u || f2 >= 0x00170000u) continue;
            avp_gcall(f2, obj, NULL, 0, 0);
        } else {
            uint32_t a[1];
            if (!G32(obj + 0xA20u)) continue;
            a[0] = obj;
            avp_gcall(0x000460D0u, 0, a, 1, 0);
        }
        avp_guest_set_esp(esp);
        hit++;
    }
    s_allow_death = 0;
    fprintf(stderr, "[COOP] tech action %05X for row %08X applied to %u of %u local objects\n", fn, id, hit, seen);
}

static void exec_tech2(const CpOrder *o)
{
    if (tech_was_applied(o->d)) return;
    if (s_t_applied_n < 64) s_t_applied[s_t_applied_n++] = o->d;
    tech_apply_direct(o->a, o->d);
}

static void exec_tech2_by_object(const CpOrder *o)
{
    uint32_t h = o->b, obj, c;
    if (s_role == ROLE_GUEST) {
        c = cp_map_to_client(h);
        obj = c ? cp_obj_of(c) : cp_obj_of(h);
    } else {
        obj = cp_obj_of(h);
    }
    if (!obj || G32(obj) != o->c || !sane_guest_ptr(obj)) {
        fprintf(stderr, "[COOP] tech action %05X: no matching local object\n", o->a);
        return;
    }
    if (tech_row_up(G32(obj + 0x280)) == 1) return;
    fprintf(stderr, "[COOP] tech action %05X for row %08X: running the game's own function on the local object\n", o->a, G32(obj + 0x280));
    avp_gcall(o->a, obj, NULL, 0, 0);
}

static void exec_tech1(const CpOrder *o)
{
    uint32_t id = o->a, h = o->b, vt = o->c, obj = 0, idx, cnt = 0, a[1], c;
    if (s_role != ROLE_GUEST) return;
    if (h) {
        c = cp_map_to_client(h);
        obj = c ? cp_obj_of(c) : cp_obj_of(h);
        if (obj && G32(obj) != vt) obj = 0;
    }
    if (!obj && vt)
        for (idx = 1; idx < 0x10000u; idx++) {
            uint32_t ob = G32(AVP_OBJ_TABLE + idx * 4u);
            if (cp_obj_ok(ob) && G32(ob) == vt) { obj = ob; cnt++; }
        }
    if (!obj || (!h && cnt != 1)) {
        fprintf(stderr, "[COOP] tech row %08X: no matching local object for the apply, left to the flag fallback\n", id);
        return;
    }
    if (tech_row_up(id) == 1) return;
    a[0] = obj;
    fprintf(stderr, "[COOP] tech row %08X: running the game's own apply on the local object\n", id);
    avp_gcall(0x00017FE0u, 0, a, 1, id);
}

static struct { uint32_t id; ULONGLONG t; } s_flip[16];

static void tech_flip_note(uint32_t id)
{
    int i, fr = -1;
    for (i = 0; i < 16; i++) {
        if (s_flip[i].id == id) return;
        if (!s_flip[i].id && fr < 0) fr = i;
    }
    if (fr >= 0) {
        s_flip[fr].id = id;
        s_flip[fr].t = GetTickCount64();
    }
}

static void tech_flip_tick(ULONGLONG now)
{
    uint32_t tt = G32(AVP_TECH_TREE), r;
    int i;
    for (i = 0; i < 16; i++) {
        uint32_t id = s_flip[i].id;
        if (!id) continue;
        if (tech_row_up(id) == 1) { s_flip[i].id = 0; continue; }
        if (now - s_flip[i].t < 3000u + 2u * s_rtt) continue;
        s_flip[i].id = 0;
        {
            uint32_t row = tech_row_of(id);
            if (row) {
                G8(row + 6u) = 1;
                fprintf(stderr, "[COOP] tech row %08X: upgrade applied by flag only (no apply event arrived)\n", id);
            }
            (void)tt;
            (void)r;
        }
    }
}

int avp_coop_take_upgrade(uint32_t self)
{
    uint8_t buf[8];
    uint32_t who, world, pm, i;
    int cap = cap_mode();
    if (!cap || !sane_guest_ptr(self)) return 0;
    who = G32(self + 0x18) & 0xFFFFu;
    world = G32(AVP_WORLD);
    if (who >= 0x20u || !sane_guest_ptr(world)) return 0;
    pm = G32(world + CP_PM_ARRAY + 4u * who);
    if (!sane_guest_ptr(pm)) return 0;
    for (i = 0; i < 0x14u; i++)
        if (G32(pm + 0x1D4 + 4 * i) == self) {
            fprintf(stderr, "[COOP] upgrade button %u pressed (%s)\n", i, cap == 1 ? "forwarded to the host" : "host, mirrored to the guest");
            emit(buf, cp_enc_upg(buf, i));
            return cap == 1;
        }
    return 0;
}

static void hide_pointer(uint32_t p);
static uint32_t s_ptr[2];

static int ptr_obj_ok(uint32_t p)
{
    return p >= 0x10000u && !(p & 3u) && p < CP_LOCAL_HI - 0x1000u - 0x280u;
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
    obj = cp_obj_of(h);
    if (!ptr_obj_ok(obj) || !ptr_class_ok(obj)) return 0;
    return obj;
}

static void make_pointers(void)
{
    uint32_t world = G32(AVP_WORLD), pm, a[2], b, cur, cur2, own = G32(AVP_POINTER);
    s_ptr[0] = s_ptr[1] = 0;
    if (!ptr_class_ok(own)) { fprintf(stderr, "[COOP] no usable pointer object (%08X) for the local player\n", own); return; }
    s_ptr[local_player()] = G32(own + 0x18);
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
    s_ptr[1 - local_player()] = G32(b + 0x18);
    G32(b + 0x22C) = 0;
    hide_pointer(b);
    fprintf(stderr, "[COOP] pointers: host %08X, player 2 %08X (hidden object %08X, local player %d)\n", s_ptr[0], s_ptr[1], b, local_player());
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
    if (player != local_player()) hide_pointer(p);
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

static void unselect_everywhere(uint32_t h)
{
    int pl;
    for (pl = 0; pl < 2; pl++) {
        uint32_t p = player_pointer(pl), sel[SEL_MAX], n, i;
        if (!p) continue;
        n = cp_read_selection(p, sel, SEL_MAX);
        for (i = 0; i < n; i++)
            if (sel[i] == h) { run_deselect(pl, h); break; }
    }
}

static void prune_selection(int player, uint32_t p)
{
    uint32_t sel[SEL_MAX], n = cp_read_selection(p, sel, SEL_MAX), i;
    for (i = 0; i < n; i++)
        if (owner_of(sel[i]) != player) run_deselect(player, sel[i]);
}

#define HIDE_MAX 256u
static uint32_t s_hide_h[HIDE_MAX], s_hide_head;
static uint32_t s_scan[0x10000];
static int s_show_other_marks = -1;

static void marks_begin(void)
{
    uint32_t i;
    for (i = 1; i < 0x10000u; i++) s_scan[i] = G32(AVP_OBJ_TABLE + i * 4u);
    s_scan[0] = 1;
}

static void marks_end(void)
{
    uint32_t i;
    if (!s_scan[0]) return;
    s_scan[0] = 0;
    for (i = 1; i < 0x10000u; i++) {
        uint32_t obj = G32(AVP_OBJ_TABLE + i * 4u);
        if (obj == s_scan[i] || !cp_obj_ok(obj)) continue;
        s_hide_h[s_hide_head++ % HIDE_MAX] = G32(obj + 0x18);
    }
}

int avp_coop_hide_marker(uint32_t obj)
{
    uint32_t h, i;
    if (s_show_other_marks < 0) {
        const char *e = getenv("AVP_COOP_SHOW_OTHER_MARKERS");
        s_show_other_marks = e && *e && *e != '0';
    }
    if (s_show_other_marks || s_role == ROLE_NONE || !s_hide_head) return 0;
    h = G32(obj + 0x18);
    if (is_hidden(h)) return 1;
    for (i = 0; i < HIDE_MAX; i++)
        if (s_hide_h[i] == h) return 1;
    return 0;
}

static int reject(const char *why, const CpOrder *o)
{
    static int told;
    InterlockedIncrement(&s_ord_rej);
    if (told++ < 20) fprintf(stderr, "[COOP] rejected order (cmd %u): %s\n", o->cmd, why);
    return 0;
}

static int unit_alive(uint32_t h)
{
    uint32_t obj = cp_obj_of(h);
    return obj && cp_unit_ok(h) && !G8(obj + 0x26Du);
}

static void exec_select(const CpOrder *o, int player)
{
    uint32_t hs[SEL_MAX], n = 0, i;
    for (i = 0; i < o->n && i < SEL_MAX; i++) {
        if (!unit_alive(o->h[i])) continue;
        if (owner_of(o->h[i]) != player) continue;
        hs[n++] = o->h[i];
    }
    run_select(player, hs, n);
    {
        static int told;
        if (s_role == ROLE_HOST && told++ < 20) {
            uint32_t sel[SEL_MAX], m = cp_read_selection(player_pointer(player), sel, SEL_MAX), k;
            fprintf(stderr, "[COOP] select: %u sent, %u passed owner filter, %u now selected on the host pointer; owners:", o->n, n, m);
            for (k = 0; k < o->n && k < 8; k++) fprintf(stderr, " %08X=%d", o->h[k], owner_of(o->h[k]));
            fprintf(stderr, "\n");
        }
    }
}

typedef struct { uint32_t under; float w[3]; } AimSave;

static void aim_pointer(uint32_t p, uint32_t under, const float *world, AimSave *sv)
{
    uint32_t i;
    sv->under = G32(p + 0xDC);
    for (i = 0; i < 3; i++) sv->w[i] = GF(p + 0x1A8 + 4 * i);
    G32(p + 0xDC) = (under && cp_target_ok(under) && !G8(cp_obj_of(under) + 0x26Du)) ? under : 0;
    for (i = 0; i < 3; i++) GF(p + 0x1A8 + 4 * i) = world[i];
}

static void aim_restore(uint32_t p, int player, const AimSave *sv)
{
    uint32_t i;
    if (player != local_player()) return;
    G32(p + 0xDC) = sv->under;
    for (i = 0; i < 3; i++) GF(p + 0x1A8 + 4 * i) = sv->w[i];
}

static void exec_move(const CpOrder *o, int player)
{
    uint32_t p = player_pointer(player), esp, msg, a[4], type, i, part, old;
    AimSave sv;
    float before[3], after[3];
    if (!p) { reject("no pointer for the player", o); return; }
    type = o->a ? 0x6007Cu : 0x60039u;
    prune_selection(player, p);
    aim_pointer(p, o->h[0], o->f + 3, &sv);
    for (i = 0; i < 3; i++) before[i] = GF(p + 0x1A8 + 4 * i);
    esp = avp_guest_esp();
    msg = avp_guest_alloca(0x30);
    for (i = 0; i < 0x30; i += 4) G32(msg + i) = 0;
    G32(msg) = type;
    G8(msg + 8) = 5;
    if (o->b) {
        for (i = 0; i < 3; i++) G32(msg + 0x10 + 4 * i) = G32(AVP_NOWHERE + 4 * i);
    } else {
        for (i = 0; i < 3; i++) GF(msg + 0x10 + 4 * i) = o->f[i];
    }
    G8(msg + 0x24) = (uint8_t)o->c;
    part = o->d == 0xFFu ? 0 : p + cp_particle_off(o->d);
    a[0] = p;
    a[1] = msg;
    a[2] = part;
    a[3] = o->c;
    old = swap_pointer(p);
    if (player != local_player()) marks_begin();
    avp_gcall(AVP_MOVE_FN, p, a, 4, 0);
    if (player != local_player()) marks_end();
    avp_guest_set_esp(esp);
    swap_pointer(old);
    for (i = 0; i < 3; i++) after[i] = GF(p + 0x1A8 + 4 * i);
    {
        static int told;
        if (s_role == ROLE_HOST && told++ < 20) {
            uint32_t sel[SEL_MAX], n = cp_read_selection(p, sel, SEL_MAX), k;
            fprintf(stderr, "[COOP] move: %u selected, nowhere %u, dest %.1f %.1f %.1f -> %.1f %.1f %.1f, owners:", n, o->b,
                    (double)before[0], (double)before[1], (double)before[2], (double)after[0], (double)after[1], (double)after[2]);
            for (k = 0; k < n && k < 8; k++) fprintf(stderr, " %08X=%d moveto %.1f %.1f %.1f", sel[k], owner_of(sel[k]),
                    (double)GF(cp_obj_of(sel[k]) + 0x4A4), (double)GF(cp_obj_of(sel[k]) + 0x4A8), (double)GF(cp_obj_of(sel[k]) + 0x4AC));
            fprintf(stderr, "\n");
        }
    }
    aim_restore(p, player, &sv);
    if (player != local_player()) hide_pointer(p);
}

static void exec_pcall(const CpOrder *o, int player)
{
    uint32_t p = player_pointer(player), fn = cp_pcall_fn(o->a), regs[6] = { 0 }, args[2] = { 0 }, esp, old;
    int reg = cp_pcall_this_reg(o->a);
    uint32_t ns = cp_pcall_nstack(o->a);
    AimSave sv;
    if (!p || !fn) { reject("no pointer for the player", o); return; }
    prune_selection(player, p);
    aim_pointer(p, o->h[0], o->f + 3, &sv);
    if (reg >= 0) regs[reg] = p;
    if (ns >= 1) args[0] = p;
    if (ns >= 2) args[1] = o->b;
    if (fn == 0x0002E130u) regs[5] = o->b;
    old = swap_pointer(p);
    esp = avp_guest_esp();
    if (player != local_player()) marks_begin();
    avp_gcall_regs(fn, regs, args, (int)ns);
    if (player != local_player()) marks_end();
    avp_guest_set_esp(esp);
    swap_pointer(old);
    aim_restore(p, player, &sv);
    if (player != local_player()) hide_pointer(p);
}

static void exec_order_msg(const CpOrder *o, int player)
{
    uint8_t msg[CP_MSG_MAX];
    uint32_t h = o->h[0], size;
    if (!unit_alive(h)) { reject("unit does not exist", o); return; }
    if (owner_of(h) != player) { reject("unit is not the player's", o); return; }
    size = cp_order_msg_build(o, msg, sizeof msg);
    if (!size) { reject("message failed validation", o); return; }
    avp_run_order(1, h, msg, size);
}

static void exec_buy(const CpOrder *o)
{
    uint32_t world = G32(AVP_WORLD), pm, btn;
    if (!sane_guest_ptr(world)) return;
    pm = G32(world + 0x78);
    if (!sane_guest_ptr(pm) || o->a >= 0x14u) { reject("no player manager", o); return; }
    btn = G32(pm + 0x1D4 + 4 * o->a);
    if (!cp_obj_ok(btn) || cp_obj_of(G32(btn + 0x18)) != btn || (G32(btn + 0x18) & 0xFFFFu) != (G32(pm + 0x18) & 0xFFFFu)) {
        reject("not a purchase button", o);
        return;
    }
    avp_gcall(AVP_BUY_FN, 0, NULL, 0, btn);
}

static void exec_upgrade(const CpOrder *o, int player)
{
    uint32_t world = G32(AVP_WORLD), pm, btn, a[1];
    if (!sane_guest_ptr(world) || player < 0 || player > 1) return;
    pm = G32(world + CP_PM_ARRAY + 4u * (uint32_t)player);
    if (!sane_guest_ptr(pm) || o->a >= 0x14u) { reject("no player manager", o); return; }
    btn = G32(pm + 0x1D4 + 4 * o->a);
    if (!cp_obj_ok(btn) || cp_obj_of(G32(btn + 0x18)) != btn || (G32(btn + 0x18) & 0xFFFFu) != (G32(pm + 0x18) & 0xFFFFu)) {
        reject("not an upgrade button", o);
        return;
    }
    a[0] = 0;
    fprintf(stderr, "[COOP] upgrade button %u executed for player %d\n", o->a, player);
    avp_gcall(AVP_UPG_FN, btn, a, 1, 0);
}

static void exec_tech(const CpOrder *o)
{
    uint32_t idx, n = o->b, found = 0, i, tt = G32(AVP_TECH_TREE), r, rows = 0;
    if (s_role != ROLE_GUEST || n == 0 || n > 16u) return;
    for (idx = 1; idx < 0x10000u && !found; idx++) {
        uint32_t obj = G32(AVP_OBJ_TABLE + idx * 4u), ids;
        int same = 1;
        if (!cp_obj_ok(obj) || G32(obj) != o->a || G32(obj + 0x3C) != n) continue;
        ids = G32(obj + 0x38);
        if (!sane_guest_ptr(ids)) continue;
        for (i = 0; i < n; i++)
            if (G32(ids + 4u * i) != o->h[i]) same = 0;
        if (!same) continue;
        found = obj;
    }
    if (found) {
        uint32_t a[1];
        if (G8(found + 0x85)) return;
        a[0] = found;
        fprintf(stderr, "[COOP] tech upgrade (%u rows, first %08X): running the game's own apply on the matching local object\n", n, o->h[0]);
        avp_gcall(0x00017340u, 0, a, 1, 0);
        return;
    }
    if (tt >= 0x10000u && tt < CP_LOCAL_HI - 0x1000u) {
        for (r = 0; G32(tt + 0x38 + r * 8u) && r < 256u; r++)
            for (i = 0; i < n; i++)
                if (G32(tt + 0x38 + r * 8u) == o->h[i]) {
                    G8(tt + 0x3Eu + r * 8u) = 1;
                    G8(tt + 0x3Du + r * 8u) = 0;
                    rows++;
                }
    }
    fprintf(stderr, "[COOP] tech upgrade (%u rows, first %08X): no matching local object, marked %u rows upgraded directly\n", n, o->h[0], rows);
}

static void exec_give(const CpOrder *o)
{
    uint32_t i, cnt = 0;
    for (i = 0; i < o->n && i < SEL_MAX; i++) {
        uint32_t h = o->h[i];
        if (s_vs || !cp_unit_ok(h) || owner_of(h) != 1) continue;
        run_deselect(1, h);
        s_owner_h[h >> 16] = h;
        s_owner[h >> 16] = 0;
        cnt++;
    }
    if (cnt) fprintf(stderr, "[COOP] %u unit%s given back to the host\n", cnt, cnt == 1 ? "" : "s");
}

static void exec_reinf(const CpOrder *o)
{
    uint32_t world = G32(AVP_WORLD), pm, who, drop = 0, i, a[3], ch;
    if (s_role != ROLE_GUEST || !sane_guest_ptr(world)) return;
    pm = G32(world + 0x78);
    if (!sane_guest_ptr(pm)) return;
    who = G32(pm + 0x18) & 0xFFFFu;
    if (o->h[0]) {
        ch = cp_map_to_client(o->h[0]);
        drop = ch ? cp_obj_of(ch) : 0;
    }
    for (i = 0; i < 7; i++) G32(0x002F6A90u + 4 * i) = o->h[1 + i];
    a[0] = o->a;
    a[1] = o->b;
    a[2] = drop;
    avp_gcall(0x00050360u, 0, a, 3, who);
}

static int ord_pop(CpOrder *o, uint8_t *raw, uint32_t *rawlen)
{
    int ok = 0;
    lk_enter();
    if (s_ord_head != s_ord_tail) {
        uint32_t k = s_ord_head % ORD_RING;
        *o = s_ord[k];
        *rawlen = s_ord_rawlen[k];
        memcpy(raw, s_ord_raw[k], *rawlen);
        s_ord_head++;
        ok = 1;
    }
    LeaveCriticalSection(&s_lock);
    return ok;
}

static int xlate_in(CpOrder *o)
{
    uint32_t i, n = 0;
    switch (o->cmd) {
    case CP_CMD_SELECT:
        for (i = 0; i < o->n; i++) {
            uint32_t c = cp_map_to_client(o->h[i]);
            if (c) o->h[n++] = c;
        }
        o->n = n;
        return 1;
    case CP_CMD_MOVE:
    case CP_CMD_PCALL:
        o->h[0] = o->h[0] ? cp_map_to_client(o->h[0]) : 0;
        return 1;
    case CP_CMD_ORDER:
        o->h[0] = cp_map_to_client(o->h[0]);
        return o->h[0] && cp_order_xlate_in(o->b, o->msg, o->n);
    case CP_CMD_BUY:
    case CP_CMD_UPG:
    case CP_CMD_TECH:
    case CP_CMD_TECH1:
    case CP_CMD_TECH2:
    case CP_CMD_REINF:
        return 1;
    }
    return 0;
}

static void orders_exec(void)
{
    CpOrder o;
    uint8_t raw[CP_ORDER_WIRE_MAX];
    uint32_t rawlen;
    int n = 0;
    while (n++ < 32 && ord_pop(&o, raw, &rawlen)) {
        int player = (int)o.player;
        LONG rej = s_ord_rej;
        if (s_role == ROLE_GUEST && !xlate_in(&o)) continue;
        s_executing = 1;
        switch (o.cmd) {
        case CP_CMD_SELECT: exec_select(&o, player); break;
        case CP_CMD_MOVE: exec_move(&o, player); break;
        case CP_CMD_PCALL: exec_pcall(&o, player); break;
        case CP_CMD_ORDER: exec_order_msg(&o, player); break;
        case CP_CMD_BUY: exec_buy(&o); break;
        case CP_CMD_UPG: exec_upgrade(&o, player); break;
        case CP_CMD_TECH: exec_tech(&o); break;
        case CP_CMD_TECH1: exec_tech1(&o); break;
        case CP_CMD_TECH2: exec_tech2(&o); break;
        case CP_CMD_REINF: exec_reinf(&o); break;
        case CP_CMD_GIVE: exec_give(&o); break;
        }
        s_executing = 0;
        InterlockedIncrement(&s_ord_exec);
        if (s_role == ROLE_HOST && rej == s_ord_rej && rawlen && o.cmd != CP_CMD_SELECT)
            send_mirror(1, raw, rawlen);
    }
}

int avp_coop_give_selection(void)
{
    uint32_t sel[SEL_MAX], n, i, cnt = 0;
    if (guest_live() && !s_vs) {
        uint32_t hh[SEL_MAX], m = 0;
        uint8_t buf[8 + 4 * SEL_MAX];
        n = cp_read_selection(G32(AVP_POINTER), sel, SEL_MAX);
        for (i = 0; i < n; i++) {
            uint32_t h;
            if (owner_of(sel[i]) != 1) continue;
            h = cp_map_to_host(sel[i]);
            if (!h) continue;
            hh[m++] = h;
            run_deselect(1, sel[i]);
            owner_set(sel[i], 0);
        }
        if (m) {
            send_order(buf, cp_enc_give(buf, hh, m));
            fprintf(stderr, "[COOP] giving %u unit%s back to the host\n", m, m == 1 ? "" : "s");
        }
        return (int)m;
    }
    if (!coop_live() || s_role != ROLE_HOST || s_vs) return 0;
    n = cp_read_selection(G32(AVP_POINTER), sel, SEL_MAX);
    for (i = 0; i < n; i++) {
        if (owner_of(sel[i]) != 0 || !cp_unit_ok(sel[i])) continue;
        run_deselect(0, sel[i]);
        s_owner_h[sel[i] >> 16] = sel[i];
        s_owner[sel[i] >> 16] = 1;
        cnt++;
    }
    if (cnt) fprintf(stderr, "[COOP] %u unit%s given to player 2\n", cnt, cnt == 1 ? "" : "s");
    return (int)cnt;
}

static uint8_t *s_sbuf;
static HANDLE s_tx_thread, s_tx_event;
static volatile LONG s_tx_quit, s_tx_pending;
static uint32_t s_tx_len, s_tx_seq;
static ULONGLONG s_tx_last, s_tx_full_last, s_gt_last, s_log_last;
static volatile LONG s_tx_bytes, s_tx_msgs;
static struct {
    uint32_t sent, full, skipped, recs, objs, words, trunc;
    uint64_t build_us, build_max;
    uint32_t rx, bad, rx_dropped;
    uint64_t apply_us, apply_max;
    CpApplyStats ap;
} s_st;
static int s_hz = 10, s_full_ms = 2000;
static uint32_t s_skip_run;

#define SP_RING   256u
#define SP_DEPTH  8
#define SP_KIDS   8
#define SP_REG_LIST   0x002C9E40u
#define SP_ALLOC_FN   0x000DC4D0u
#define SP_ALLOC_POOL 0x002CC2E4u
#define SP_HANDLE_FN  0x000D6FE0u
#define SP_CKEY_FN    0x000D6DC0u
#define SP_WORLD_OBJ  0x002F6AF0u
#define SP_PLAYERS    12u
#define SP_SCRATCH_MAX 1024u

enum { SPF_FACT = 1, SPF_CKEY = 2, SPF_MSG = 3 };

typedef struct {
    int kind;
    uint32_t rec[5], arg2, key, a, b, nk;
    CpSpawn kid[SP_KIDS];
} SpFrame;

#define SP_SENT 256u
static uint32_t s_bl[16];
static int s_bl_n;
#define REQ_MAX 16u
static uint32_t s_req[REQ_MAX], s_req_head, s_req_tail;
static CpSpawn s_sent[SP_SENT];
static uint32_t s_sent_n;
static ULONGLONG s_resent_t[SP_SENT];

#define UM_MAX 32u
static struct { uint32_t h, vt; ULONGLONG first, seen; int tries; ULONGLONG last; int logged, haspos, bound_try; float pos[3]; } s_um[UM_MAX];
#define HIST_MAX 64u
static struct { uint32_t h, kind; ULONGLONG t; const char *what; } s_hist[HIST_MAX];
static uint32_t s_hist_n;
static CpSpawn s_sp_ring[SP_RING];
static uint32_t s_sp_head, s_sp_tail, s_sp_scratch_n;
static volatile LONG s_sp_drop;
static SpFrame s_spf[SP_DEPTH];
static int s_spn, s_sp_over, s_sp_replay, s_sp_msgdepth;
static uint32_t s_sp_obj;
static LONG s_sp_sent, s_sp_done, s_sp_fail, s_sp_bound, s_sp_reaped;
static ULONGLONG s_sp_t[SP_RING];

#define SP_LOC      64u
#define SP_BIND_BASE 5000u
#define SP_GRACE_BASE 300u
#define SP_BIND_MS  (SP_BIND_BASE + 2u * s_rtt)
#define SP_GRACE_MS (SP_GRACE_BASE + s_rtt / 2u)
#define SP_BIND_DIST 64.0f

typedef struct { uint32_t h; ULONGLONG t; } SpLoc;
static SpLoc s_loc[SP_LOC];
static uint32_t s_loc_n;

static void spawn_reset(void)
{
    if (s_lock_ready) {
        lk_enter();
        s_sp_head = s_sp_tail = 0;
        LeaveCriticalSection(&s_lock);
    }
    s_spn = s_sp_over = s_sp_replay = s_sp_msgdepth = 0;
    s_sp_obj = 0;
    s_sp_scratch_n = 0;
    s_sp_sent = s_sp_done = s_sp_fail = s_sp_bound = s_sp_reaped = 0;
    memset(s_loc, 0, sizeof s_loc);
    s_loc_n = 0;
    memset(s_sent, 0, sizeof s_sent);
    memset(s_resent_t, 0, sizeof s_resent_t);
    memset(s_um, 0, sizeof s_um);
    memset(s_hist, 0, sizeof s_hist);
    s_hist_n = 0;
    s_sent_n = 0;
    s_req_head = s_req_tail = 0;
    s_bl_n = 0;
    s_tech_nsent = 0;
    s_hidden_n = 0;
    s_t1_n = 0;
    s_t_applied_n = 0;
    memset(s_flip, 0, sizeof s_flip);
    g_cp_tech_flip = tech_flip_note;
    s_sp_drop = 0;
}

static void spawn_enqueue(const CpSpawn *sp)
{
    lk_enter();
    if (s_sp_tail - s_sp_head < SP_RING) {
        s_sp_ring[s_sp_tail % SP_RING] = *sp;
        s_sp_t[s_sp_tail % SP_RING] = GetTickCount64();
        s_sp_tail++;
    } else {
        InterlockedIncrement(&s_sp_drop);
    }
    LeaveCriticalSection(&s_lock);
}

static int sp_ptr(uint32_t v)
{
    return v >= 0x10000u && v < CP_LOCAL_HI - 0x400u;
}

static int sp_has(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (strstr(name, *list)) return 1;
    return 0;
}

static const char *const k_sp_deny[] = { "Pointer", "Select", "Marker", "Decal", "Particle", "Button", "Cursor", "Squad", "MiniMap", "Minimap", NULL };
static const char *const k_sp_root[] = { "GAvPObject", "GUnit", "GTrigger", NULL };

static int sp_name_at(uint32_t p, char *out, size_t cap)
{
    size_t k = 0;
    out[0] = 0;
    if (!sp_ptr(p)) return 0;
    while (k + 1 < cap && G8(p + k)) { out[k] = (char)G8(p + k); k++; }
    out[k] = 0;
    return k > 0;
}

static uint32_t sp_find_desc(uint32_t key)
{
    uint32_t list = G32(SP_REG_LIST), data, cnt, i;
    if (!sp_ptr(list)) return 0;
    data = G32(list);
    cnt = G32(list + 4);
    if (!sp_ptr(data) || cnt > 8192u) return 0;
    for (i = 0; i < cnt; i++) {
        uint32_t d = G32(data + 4u * i);
        if (sp_ptr(d) && G32(d) == key) return d;
    }
    return 0;
}

static struct { uint32_t key; uint8_t st; } s_sp_kc[256];

static int sp_key_gameplay(uint32_t key)
{
    uint32_t h = (key * 2654435761u) >> 24, i, d, hops;
    int root = 0, deny = 0;
    for (i = 0; i < 8u; i++) {
        uint32_t k = (h + i) & 255u;
        if (s_sp_kc[k].st && s_sp_kc[k].key == key) return s_sp_kc[k].st == 1;
        if (!s_sp_kc[k].st) break;
    }
    d = sp_find_desc(key);
    for (hops = 0; d && hops < 24u; hops++) {
        char nm[64];
        if (sp_name_at(G32(d + 4), nm, sizeof nm)) {
            if (sp_has(nm, k_sp_deny)) deny = 1;
            if (sp_has(nm, k_sp_root)) root = 1;
        }
        d = G32(d + 12);
        if (!sp_ptr(d)) break;
    }
    if (d || hops) {
        for (i = 0; i < 8u; i++) {
            uint32_t k = (h + i) & 255u;
            if (!s_sp_kc[k].st) {
                s_sp_kc[k].key = key;
                s_sp_kc[k].st = (uint8_t)(root && !deny ? 1 : 2);
                break;
            }
        }
    }
    return root && !deny;
}

static int sp_obj_ok(uint32_t obj)
{
    uint32_t h;
    if (!sp_ptr(obj) || (obj & 3u) || !cp_obj_ok(obj)) return 0;
    h = G32(obj + 0x18);
    return (h >> 16) && cp_obj_of(h) == obj;
}

static int sp_obj_gameplay(uint32_t obj)
{
    char nm[48];
    uint32_t vt;
    if (!sp_obj_ok(obj)) return 0;
    vt = G32(obj);
    if (!cp_class_replicable(vt) || (cp_class(vt) & CF_HELPER)) return 0;
    vt_name(vt, nm, sizeof nm);
    return !sp_has(nm, k_sp_deny);
}

static void sp_class_log(const char *what, uint32_t vt)
{
    static uint32_t seen[96];
    static int n;
    int i;
    char nm[48];
    for (i = 0; i < n; i++)
        if (seen[i] == vt) return;
    if (n >= 96) return;
    seen[n++] = vt;
    vt_name(vt, nm, sizeof nm);
    fprintf(stderr, "[COOP] %s: %s\n", what, nm);
}

static void sp_fail(const char *why, const CpSpawn *sp)
{
    LONG n = InterlockedIncrement(&s_sp_fail);
    if (n <= 24 || (n & 63) == 0)
        fprintf(stderr, "[COOP] spawn replay failed (%ld): %s (kind %u, host handle %08X, class %08X)\n", (long)n, why, sp->kind, sp->host_h, sp->host_vt);
}

static void sp_send(const CpSpawn *sp)
{
    uint8_t buf[CP_SPAWN_WIRE_MAX];
    uint32_t n = cp_enc_spawn(buf, sp);
    if (!n) return;
    s_sent[s_sent_n++ % SP_SENT] = *sp;
    if (!send_msg(MSG_SPAWN, buf, n)) { coop_fail("send spawn"); return; }
    InterlockedIncrement(&s_sp_sent);
    sp_class_log("spawn sent", sp->host_vt);
}

static int host_spawning(void)
{
    return s_role == ROLE_HOST && coop_live();
}

static SpFrame *sp_push(int kind)
{
    SpFrame *f;
    if (s_spn >= SP_DEPTH) { s_sp_over++; return NULL; }
    f = &s_spf[s_spn++];
    f->kind = kind;
    f->nk = 0;
    return f;
}

static void sp_fill(CpSpawn *s, uint32_t b)
{
    s->fill_ptr = b;
    s->fill_len = 0;
    s->fill_hash = 0;
    if (sp_ptr(b) && !(b & 3u) && b + CP_SPAWN_FILL < CP_LOCAL_HI - 0x100u) {
        uint32_t i;
        for (i = 0; i < CP_SPAWN_FILL; i++) s->fill[i] = G8(b + i);
        s->fill_len = CP_SPAWN_FILL;
        s->fill_hash = cp_fnv32(s->fill, CP_SPAWN_FILL);
    }
}

static int sp_build(const SpFrame *f, uint32_t obj, uint32_t msg, CpSpawn *s)
{
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->host_h = G32(obj + 0x18);
    s->host_vt = G32(obj);
    switch (f->kind) {
    case SPF_MSG: {
        uint32_t entry = CP_MSG_TYPES + 0x15u * 12u, size = G32(entry) ? G32(entry + 4u) : 0;
        if (!sp_ptr(msg) || size < 48u || size > CP_SPAWN_WORDS * 4u || (size & 3u)) return 0;
        s->kind = CP_SPAWN_MSG15;
        s->n = size / 4u;
        for (i = 0; i < s->n; i++) s->w[i] = G32(msg + 4u * i);
        return s->w[0] == 0x15u && (s->w[3] || s->w[4]);
    }
    case SPF_FACT:
        s->kind = CP_SPAWN_FACT;
        s->n = 6;
        for (i = 0; i < 5; i++) s->w[i] = f->rec[i];
        s->w[5] = f->arg2;
        sp_fill(s, f->rec[3]);
        return f->rec[1] != 0;
    case SPF_CKEY:
        s->kind = CP_SPAWN_CKEY;
        s->n = 3;
        s->w[0] = f->key;
        s->w[1] = f->a;
        s->w[2] = f->b;
        return f->key != 0;
    }
    return 0;
}

static void sp_deliver(SpFrame *f, const CpSpawn *mine)
{
    const CpSpawn *list = mine ? mine : f->kid;
    uint32_t n = mine ? 1u : f->nk, i;
    if (!n) return;
    if (s_spn == 0) {
        for (i = 0; i < n; i++) sp_send(&list[i]);
    } else {
        SpFrame *p = &s_spf[s_spn - 1];
        for (i = 0; i < n && p->nk < SP_KIDS; i++) p->kid[p->nk++] = list[i];
    }
}

static void sp_post(int kind, uint32_t obj, uint32_t msg)
{
    SpFrame *f;
    CpSpawn mine;
    int have = 0;
    if (s_sp_over > 0) { s_sp_over--; return; }
    if (!s_spn) return;
    f = &s_spf[--s_spn];
    if (f->kind != kind) {
        static int told;
        if (told++ < 8) fprintf(stderr, "[COOP] spawn capture lost its place (frame kind %d, finished kind %d); pending spawns dropped\n", f->kind, kind);
        s_spn = 0;
        return;
    }
    if (sp_obj_gameplay(obj)) have = sp_build(f, obj, msg, &mine);
    sp_deliver(f, have ? &mine : NULL);
}

static int sp_recent(uint32_t h)
{
    uint32_t i;
    ULONGLONG now = GetTickCount64();
    for (i = 0; i < SP_LOC; i++)
        if (s_loc[i].h == h && now - s_loc[i].t <= SP_BIND_MS && !cp_map_to_host(h)) return 1;
    return 0;
}

static void sp_note_local(uint32_t obj)
{
    uint32_t h, i;
    if (!sp_obj_gameplay(obj)) return;
    sp_class_log("object created by the guest's own game code", G32(obj));
    h = G32(obj + 0x18);
    for (i = 0; i < SP_LOC; i++)
        if (s_loc[i].h == h) return;
    s_loc[s_loc_n++ % SP_LOC].h = h;
    s_loc[(s_loc_n - 1) % SP_LOC].t = GetTickCount64();
}

static int sp_host_pos(const CpSpawn *sp, float *p)
{
    uint32_t i;
    if (sp->kind != CP_SPAWN_MSG15 || sp->n < 9u) return 0;
    for (i = 0; i < 3; i++) memcpy(&p[i], &sp->w[6 + i], 4);
    return 1;
}

static int sp_try_bind(const CpSpawn *sp, ULONGLONG now)
{
    uint32_t i, best = SP_LOC, cnt = 0;
    float hp[3], bd = 0.0f;
    int haspos = sp_host_pos(sp, hp);
    for (i = 0; i < SP_LOC; i++) {
        uint32_t obj;
        float d2 = 0.0f;
        if (!s_loc[i].h || now - s_loc[i].t > SP_BIND_MS) continue;
        obj = cp_obj_of(s_loc[i].h);
        if (!sp_obj_ok(obj) || G32(obj) != sp->host_vt || G8(obj + 0x26Du) || is_hidden(s_loc[i].h) || cp_map_to_host(s_loc[i].h)) continue;
        cnt++;
        if (haspos) {
            uint32_t a;
            for (a = 0; a < 3; a++) {
                float d = GF(obj + 0x40u + a * 4u) - hp[a];
                d2 += d * d;
            }
        }
        if (best == SP_LOC || (haspos ? d2 < bd : s_loc[i].t < s_loc[best].t)) {
            best = i;
            bd = d2;
        }
    }
    if (best == SP_LOC) return 0;
    if (haspos && cnt > 1 && bd > SP_BIND_DIST * SP_BIND_DIST) return 0;
    cp_map_bind(sp->host_h, s_loc[best].h);
    s_loc[best].h = 0;
    InterlockedIncrement(&s_sp_bound);
    sp_class_log("spawn bound to the guest's own object", sp->host_vt);
    return 1;
}

static void sp_loc_expire(ULONGLONG now)
{
    uint32_t i;
    for (i = 0; i < SP_LOC; i++) {
        uint32_t h = s_loc[i].h, obj;
        if (!h || now - s_loc[i].t <= SP_BIND_MS) continue;
        obj = cp_obj_of(h);
        if (sp_obj_gameplay(obj) && !G8(obj + 0x26Du) && !cp_map_to_host(h) && s_st_last_ms && now - s_st_last_ms < 3000u) {
            if (!sp_recent(h) && (hide_local(h), 1) && InterlockedIncrement(&s_sp_reaped) <= 8)
                fprintf(stderr, "[COOP] removing a local object the host does not have\n");
        }
        s_loc[i].h = 0;
    }
}

int avp_coop_factory_pre(uint32_t rec, uint32_t arg2)
{
    uint32_t key = sp_ptr(rec) ? G32(rec + 4) : 0, i;
    SpFrame *f;
    if (!host_spawning()) return 0;
    f = sp_push(SPF_FACT);
    if (f) {
        for (i = 0; i < 5; i++) f->rec[i] = sp_ptr(rec) ? G32(rec + 4u * i) : 0;
        f->arg2 = arg2;
        f->key = key;
    }
    return 0;
}

void avp_coop_factory_post(uint32_t obj)
{
    if (guest_live()) {
        if (!s_sp_replay) sp_note_local(obj);
        return;
    }
    if (host_spawning() || s_spn || s_sp_over) sp_post(SPF_FACT, obj, 0);
}

int avp_coop_classkey_pre(uint32_t key, uint32_t a, uint32_t b)
{
    SpFrame *f;
    if (!host_spawning()) return 0;
    f = sp_push(SPF_CKEY);
    if (f) {
        f->key = key;
        f->a = a;
        f->b = b;
    }
    return 0;
}

void avp_coop_classkey_post(uint32_t obj)
{
    if (guest_live()) {
        if (!s_sp_replay) sp_note_local(obj);
        return;
    }
    if (host_spawning() || s_spn || s_sp_over) sp_post(SPF_CKEY, obj, 0);
}

void avp_coop_msg15_begin(uint32_t msg)
{
    (void)msg;
    s_sp_msgdepth++;
    if (host_spawning()) sp_push(SPF_MSG);
}

void avp_coop_msg15_end(uint32_t obj, uint32_t msg)
{
    if (s_sp_msgdepth > 0) s_sp_msgdepth--;
    if (guest_live()) {
        if (s_sp_replay) s_sp_obj = obj;
        else sp_note_local(obj);
        return;
    }
    if (host_spawning() || s_spn || s_sp_over) sp_post(SPF_MSG, obj, msg);
}

static uint32_t sp_next_handle(uint32_t player)
{
    uint32_t esp = avp_guest_esp(), out = avp_guest_alloca(16), regs[6] = { 0 }, h;
    if (player >= SP_PLAYERS) { avp_guest_set_esp(esp); return 0; }
    G32(out) = 0;
    regs[0] = out;
    regs[2] = player;
    avp_gcall_regs(SP_HANDLE_FN, regs, NULL, 0);
    h = G32(out);
    avp_guest_set_esp(esp);
    if (!(h >> 16) || G32(CP_OBJ_TABLE + (h >> 16) * 4u) > 1u) return 0;
    return h;
}

static uint32_t sp_scratch(uint32_t size)
{
    uint32_t esp = avp_guest_esp(), a[3], r;
    if (s_sp_scratch_n >= SP_SCRATCH_MAX) return 0;
    a[0] = SP_ALLOC_POOL;
    a[1] = size;
    a[2] = 0x10;
    r = avp_gcall(SP_ALLOC_FN, 0, a, 3, 0);
    avp_guest_set_esp(esp);
    if (!sp_ptr(r)) return 0;
    s_sp_scratch_n++;
    return r;
}

static uint32_t sp_fill_ptr(const CpSpawn *sp)
{
    uint32_t i, b;
    if (!sp->fill_len) return sp->fill_ptr;
    if (sp_ptr(sp->fill_ptr) && !(sp->fill_ptr & 3u) && sp->fill_ptr + sp->fill_len < CP_LOCAL_HI - 0x100u) {
        uint8_t tmp[CP_SPAWN_FILL];
        for (i = 0; i < sp->fill_len; i++) tmp[i] = G8(sp->fill_ptr + i);
        if (cp_fnv32(tmp, sp->fill_len) == sp->fill_hash) return sp->fill_ptr;
    }
    b = sp_scratch(sp->fill_len + 16u);
    if (!b) return 0;
    for (i = 0; i < sp->fill_len; i++) G8(b + i) = sp->fill[i];
    return b;
}

static uint32_t sp_xlate_handle_arg(uint32_t v, uint32_t host_h, int *bad)
{
    if (!(v >> 16)) return v;
    if (v == host_h) {
        uint32_t h = sp_next_handle(v & 0xFFFFu);
        if (!h) *bad = 1;
        return h;
    }
    {
        uint32_t c = cp_map_to_client(v);
        return c ? c : v;
    }
}

static int sp_blacklisted(uint32_t vt)
{
    int i;
    for (i = 0; i < s_bl_n; i++)
        if (s_bl[i] == vt) return 1;
    return 0;
}

static int sp_chain_ok(uint32_t obj)
{
    uint32_t p = G32(obj + 8), hops;
    for (hops = 0; hops < 64u; hops++) {
        if (!p) return 1;
        if (!cp_obj_ok(p) || p == obj) return 0;
        p = G32(p + 8);
    }
    return 0;
}

static int sp_new_ok(uint32_t obj)
{
    uint32_t vt, first;
    if (!sp_obj_ok(obj)) return 0;
    vt = G32(obj);
    if (vt < 0x002050C0u || vt >= 0x00265D60u || (vt & 3u)) return 0;
    first = G32(vt);
    return first >= 0x00011000u && first < 0x00170000u;
}

static void sp_finish(uint32_t obj, const CpSpawn *sp)
{
    if (!sp_obj_ok(obj)) { sp_fail("the creation call returned no object", sp); return; }
    if (G32(obj) != sp->host_vt) { sp_fail("created class differs from the host's", sp); return; }
    if (!sp_new_ok(obj) || !sp_chain_ok(obj)) {
        char nm[48];
        if (G32(obj + 8) && !sp_chain_ok(obj)) G32(obj + 8) = 0;
        vt_name(G32(obj), nm, sizeof nm);
        fprintf(stderr, "[COOP] a replayed %s failed validation; removing it and not replicating that class again\n", nm);
        if (s_bl_n < 16) s_bl[s_bl_n++] = G32(obj);
        hide_local(G32(obj + 0x18));
        return;
    }
    cp_map_bind(sp->host_h, G32(obj + 0x18));
    InterlockedIncrement(&s_sp_done);
    sp_class_log("spawn mirrored", sp->host_vt);
}

static void replay_msg15(const CpSpawn *sp)
{
    uint8_t buf[CP_SPAWN_WORDS * 4u];
    uint32_t w[CP_SPAWN_WORDS], i, entry = CP_MSG_TYPES + 0x15u * 12u, world = G32(SP_WORLD_OBJ), wh, c;
    int bad = 0;
    if (!sp->w[3] && !sp->w[4]) return;
    if (!G32(entry) || G32(entry + 4u) != sp->n * 4u) { sp_fail("message size differs on this machine", sp); return; }
    wh = sp_ptr(world) ? G32(world + 0x18) : 0;
    if (!wh || cp_obj_of(wh) != world) { sp_fail("spawn receiver object not found", sp); return; }
    for (i = 0; i < sp->n; i++) w[i] = sp->w[i];
    c = w[1] ? cp_map_to_client(w[1]) : 0;
    w[1] = c ? c : wh;
    w[5] = sp_xlate_handle_arg(w[5], sp->host_h, &bad);
    if (bad) { sp_fail("no free handle for the new object", sp); return; }
    for (i = 0; i < sp->n; i++) memcpy(buf + 4u * i, &w[i], 4);
    s_sp_obj = 0;
    s_sp_replay = 1;
    avp_run_order(3, wh, buf, sp->n * 4u);
    s_sp_replay = 0;
    sp_finish(s_sp_obj, sp);
}

static int sp_class_ok(uint32_t key, const CpSpawn *sp)
{
    if (!sp_find_desc(key)) { sp_fail("class key is not registered", sp); return 0; }
    if (!sp_key_gameplay(key)) { sp_fail("class is not a replicated gameplay class", sp); return 0; }
    return 1;
}

static void replay_fact(const CpSpawn *sp)
{
    uint32_t esp = avp_guest_esp(), va, fp, arg2, a[2], i, obj;
    int bad = 0;
    if (!sp_class_ok(sp->w[1], sp)) return;
    fp = sp_fill_ptr(sp);
    if (sp->fill_len && !fp) { sp_fail("no room for the fill record", sp); return; }
    arg2 = sp_xlate_handle_arg(sp->w[5], sp->host_h, &bad);
    if (bad) { sp_fail("no free handle for the new object", sp); return; }
    va = avp_guest_alloca(32);
    for (i = 0; i < 5; i++) G32(va + 4u * i) = sp->w[i];
    G32(va + 12) = fp;
    a[0] = va;
    a[1] = arg2;
    s_sp_replay = 1;
    obj = avp_gcall(AVP_FACTORY, 0, a, 2, 0);
    s_sp_replay = 0;
    avp_guest_set_esp(esp);
    sp_finish(obj, sp);
}

static void replay_ckey(const CpSpawn *sp)
{
    uint32_t esp = avp_guest_esp(), regs[6] = { 0 }, a[2], obj;
    int bad = 0;
    if (!sp_class_ok(sp->w[0], sp)) return;
    a[0] = sp_xlate_handle_arg(sp->w[1], sp->host_h, &bad);
    if (bad) { sp_fail("no free handle for the new object", sp); return; }
    a[1] = sp->w[2];
    regs[5] = sp->w[0];
    s_sp_replay = 1;
    obj = avp_gcall_regs(SP_CKEY_FN, regs, a, 2);
    s_sp_replay = 0;
    avp_guest_set_esp(esp);
    sp_finish(obj, sp);
}

static int spawn_mapped(const CpSpawn *sp)
{
    uint32_t cm = cp_map_to_client(sp->host_h), obj;
    if (!cm) return 0;
    obj = cp_obj_of(cm);
    if (obj && G32(obj) == sp->host_vt) return 1;
    {
        char a[48], b[48];
        vt_name(sp->host_vt, a, sizeof a);
        vt_name(obj ? G32(obj) : 0, b, sizeof b);
        fprintf(stderr, "[COOP] the host replaced a %s with a %s; the old local object is released and reaped later if the host no longer has it\n", b, a);
    }
    cp_map_unbind(sp->host_h);
    return 0;
}

static void sp_trace(const char *what, const CpSpawn *sp)
{
    static int n;
    char nm[48];
    s_hist[s_hist_n++ % HIST_MAX].h = sp->host_h;
    s_hist[(s_hist_n - 1) % HIST_MAX].kind = sp->kind;
    s_hist[(s_hist_n - 1) % HIST_MAX].t = GetTickCount64();
    s_hist[(s_hist_n - 1) % HIST_MAX].what = what;
    if (n++ >= 80) return;
    vt_name(sp->host_vt, nm, sizeof nm);
    fprintf(stderr, "[COOP] spawn event kind %u %s: %s\n", sp->kind, nm, what);
}

static void um_note(uint32_t handle, uint32_t vt, const float *pos)
{
    uint32_t i, free_i = UM_MAX;
    ULONGLONG now = GetTickCount64();
    if (!(handle >> 16)) return;
    for (i = 0; i < UM_MAX; i++) {
        if (s_um[i].h == handle) { s_um[i].seen = now; return; }
        if (!s_um[i].h && free_i == UM_MAX) free_i = i;
    }
    if (free_i == UM_MAX) return;
    s_um[free_i].h = handle;
    s_um[free_i].vt = vt;
    s_um[free_i].haspos = pos != NULL;
    if (pos) memcpy(s_um[free_i].pos, pos, sizeof s_um[free_i].pos);
    s_um[free_i].logged = 0;
    s_um[free_i].first = s_um[free_i].seen = now;
    s_um[free_i].tries = 0;
    s_um[free_i].last = 0;
}

static int class_key_of(uint32_t vt);
static void replay_ckey(const CpSpawn *sp);
static int sp_blacklisted(uint32_t vt);

static int morph_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("AVP_COOP_MORPH");
        on = !(e && *e == '0');
    }
    return on;
}

static void morph_local(uint32_t h, uint32_t vt, uint32_t lo)
{
    uint32_t key, old_vt = G32(lo), cur, nobj, i, hw;
    float pos[3];
    CpSpawn sp;
    char a[48], b[48];
    uint32_t fl_new = cp_class(vt), fl_old = cp_class(old_vt);
    if (!(fl_new & CF_UNIT) || !(fl_old & CF_UNIT) || ((fl_new | fl_old) & (CF_STATIC | CF_HELPER)) || sp_blacklisted(vt)) {
        char x[48], y[48];
        vt_name(vt, x, sizeof x);
        vt_name(old_vt, y, sizeof y);
        fprintf(stderr, "[COOP] class mismatch %s (host) vs %s (local) not morphed: not two replicable units or the class is blacklisted\n", x, y);
        return;
    }
    key = (uint32_t)class_key_of(vt);
    vt_name(vt, a, sizeof a);
    vt_name(old_vt, b, sizeof b);
    if (!key) {
        sp_class_log("no class key for a local morph", vt);
        return;
    }
    for (i = 0; i < 3; i++) pos[i] = GF(lo + 0x40u + 4u * i);
    {
        uint32_t bi, bobj = 0;
        float bd = 1.0e30f;
        for (bi = 1; bi < 0x10000u; bi++) {
            uint32_t ob = G32(AVP_OBJ_TABLE + bi * 4u), ch, a3;
            float d2 = 0.0f;
            if (ob == lo || !cp_obj_ok(ob) || G32(ob) != vt || G8(ob + 0x26Du) || G8(ob + 0x26Au) || is_hidden(G32(ob + 0x18))) continue;
            ch = G32(ob + 0x18);
            if (cp_map_to_host(ch)) continue;
            for (a3 = 0; a3 < 3; a3++) {
                float dd = GF(ob + 0x40u + 4u * a3) - pos[a3];
                d2 += dd * dd;
            }
            if (d2 < bd) { bd = d2; bobj = ob; }
        }
        if (bobj) {
            cp_map_unbind(h);
            cp_map_bind(h, G32(bobj + 0x18));
            fprintf(stderr, "[COOP] class mismatch %s (host) vs %s (local): rebound the host handle to the nearest local %s instead of creating one\n", a, b, a);
            return;
        }
    }
    if (!cp_classes_related(vt, old_vt)) {
        fprintf(stderr, "[COOP] class mismatch %s (host) vs %s (local): different families, not a morph; waiting for the spawn\n", a, b);
        return;
    }
    memset(&sp, 0, sizeof sp);
    sp.kind = CP_SPAWN_CKEY;
    sp.host_h = h;
    sp.host_vt = vt;
    sp.n = 3;
    sp.w[0] = key;
    sp.w[1] = h;
    sp.w[2] = 0;
    cp_map_unbind(h);
    hide_local(h);
    replay_ckey(&sp);
    cur = cp_map_to_client(h);
    nobj = cur ? cp_obj_of(cur) : 0;
    hw = nobj ? 1u : 0u;
    if (hw) hk_setpos(nobj, pos);
    fprintf(stderr, "[COOP] morphed a local %s into a %s to match the host: %s\n", b, a, hw ? "done" : "failed");
}

static void um_tick(ULONGLONG now)
{
    uint32_t i;
    for (i = 0; i < UM_MAX; i++) {
        uint32_t h = s_um[i].h;
        if (!h) continue;
        if (cp_map_to_client(h) || now - s_um[i].seen > 3000u + 2u * s_rtt || now - s_um[i].first > 20000u + 4u * s_rtt) { s_um[i].h = 0; continue; }
        if (!s_um[i].bound_try && now - s_um[i].first >= 1000u + s_rtt && s_um[i].haspos) {
            uint32_t bi, bobj = 0;
            float bd = 1.0e30f;
            s_um[i].bound_try = 1;
            for (bi = 1; bi < 0x10000u; bi++) {
                uint32_t ob = G32(AVP_OBJ_TABLE + bi * 4u), a3;
                float d2 = 0.0f;
                if (!cp_obj_ok(ob) || G8(ob + 0x26Du) || G8(ob + 0x26Au) || cp_map_to_host(G32(ob + 0x18)) || is_hidden(G32(ob + 0x18))) continue;
                if (G32(ob) != s_um[i].vt && !((cp_class(s_um[i].vt) & CF_NOREPLAY) && (cp_class(G32(ob)) & CF_NOREPLAY) && cp_classes_related(G32(ob), s_um[i].vt))) continue;
                for (a3 = 0; a3 < 3; a3++) {
                    float dd = GF(ob + 0x40u + 4u * a3) - s_um[i].pos[a3];
                    d2 += dd * dd;
                }
                if (d2 < bd) { bd = d2; bobj = ob; }
            }
            if (bobj) {
                char nm[48];
                cp_map_bind(h, G32(bobj + 0x18));
                vt_name(s_um[i].vt, nm, sizeof nm);
                fprintf(stderr, "[COOP] bound the host's %s to the nearest free local one (%.0f units away)\n", nm, (double)sqrtf(bd));
                s_um[i].h = 0;
                continue;
            }
        }
        {
            uint32_t lo = cp_obj_of(h), lc = lo ? cp_class(G32(lo)) : 0;
            if (lo && (!(lc & CF_UNIT) || (lc & (CF_STATIC | CF_HELPER)))) lo = 0;
            if (lo && G32(lo) != s_um[i].vt && !s_um[i].logged && now - s_um[i].first < 2000u + 2u * s_rtt) {
                char x[48], y[48];
                vt_name(s_um[i].vt, x, sizeof x);
                vt_name(G32(lo), y, sizeof y);
                fprintf(stderr, "[COOP] class mismatch on a handle: host %s, local %s; %s after %u ms\n", x, y, morph_on() ? (cp_classes_related(s_um[i].vt, G32(lo)) ? "will rebind or morph locally" : "will try to rebind a local unit, never morph") : "morph is disabled", 2000u + 2u * s_rtt);
                s_um[i].logged = 1;
            }
            if (lo && morph_on() && G32(lo) != s_um[i].vt && now - s_um[i].first >= 2000u + 2u * s_rtt && !s_um[i].tries) {
                s_um[i].tries = 3;
                STEP(30, h, s_um[i].vt);
                morph_local(h, s_um[i].vt, lo);
                s_um[i].h = 0;
                continue;
            }
        }
        if (!s_um[i].logged && now - s_um[i].first >= 3000u + 2u * s_rtt) {
            char nm[48], cn[48] = "none";
            uint32_t k, hits = 0;
            uint32_t cm = 0;
            s_um[i].logged = 1;
            vt_name(s_um[i].vt, nm, sizeof nm);
            if (cp_obj_of(h)) vt_name(G32(cp_obj_of(h)), cn, sizeof cn);
            fprintf(stderr, "[COOP] host unit still missing after 3 s: %s (a local object with that handle is %s)\n", nm, cn);
            (void)cm;
            for (k = 0; k < HIST_MAX; k++)
                if (s_hist[k].h == h) {
                    hits++;
                    fprintf(stderr, "[COOP]   spawn history: kind %u %s, %llu ms ago\n", s_hist[k].kind, s_hist[k].what, (unsigned long long)(now - s_hist[k].t));
                }
            if (!hits) fprintf(stderr, "[COOP]   no spawn event was ever received for it\n");
        }
        if (!(cp_class(s_um[i].vt) & CF_NOREPLAY) && s_um[i].tries < 3 && now - s_um[i].first >= 2000u + 2u * s_rtt + 4000u * (ULONGLONG)s_um[i].tries) {
            s_um[i].tries++;
            s_um[i].last = now;
            send_msg(MSG_SPAWNREQ, &h, 4);
            fprintf(stderr, "[COOP] asked the host to resend a spawn (try %d)\n", s_um[i].tries);
        }
    }
}

static int rebuild_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("AVP_COOP_REBUILD");
        on = e && *e == '1';
    }
    return on;
}

static int class_key_of(uint32_t vt)
{
    char nm[64], core[64];
    uint32_t list = G32(SP_REG_LIST), data, cnt, i;
    size_t m;
    vt_name(vt, nm, sizeof nm);
    if (strncmp(nm, ".?AV", 4)) return 0;
    snprintf(core, sizeof core, "%s", nm + 4);
    m = strlen(core);
    if (m > 2 && !strcmp(core + m - 2, "@@")) core[m - 2] = 0;
    if (!sp_ptr(list)) return 0;
    data = G32(list);
    cnt = G32(list + 4);
    if (!sp_ptr(data) || cnt > 8192u) return 0;
    for (i = 0; i < cnt; i++) {
        uint32_t d = G32(data + 4u * i);
        char dn[64];
        if (!sp_ptr(d) || !sp_name_at(G32(d + 4), dn, sizeof dn)) continue;
        if (!strcmp(dn, core)) return (int)G32(d);
    }
    return 0;
}

static void spawn_resend(uint32_t handle)
{
    uint32_t i, obj, vt, key;
    ULONGLONG now = GetTickCount64();
    CpSpawn sp;
    uint8_t buf[CP_SPAWN_WIRE_MAX];
    uint32_t n;
    if (!(handle >> 16)) return;
    for (i = 0; i < SP_SENT; i++)
        if (s_sent[i].host_h == handle && s_sent[i].kind && now - s_resent_t[i] > 1500u + 2u * s_rtt) {
            n = cp_enc_spawn(buf, &s_sent[i]);
            s_resent_t[i] = now;
            if (n) send_msg(MSG_SPAWN, buf, n);
            return;
        } else if (s_sent[i].host_h == handle && s_sent[i].kind) {
            return;
        }
    obj = cp_obj_of(handle);
    if (!obj || !sp_obj_gameplay(obj)) return;
    vt = G32(obj);
    {
        static int told;
        char nm[48];
        if (told++ < 12) {
            vt_name(vt, nm, sizeof nm);
            fprintf(stderr, "[COOP] the guest asked for a spawn of %s that was never captured on this machine (created by a path with no spawn hook)\n", nm);
        }
    }
    if (!rebuild_on() || !(cp_class(vt) & CF_UNIT) || (cp_class(vt) & (CF_STATIC | CF_HELPER)) || sp_blacklisted(vt)) return;
    key = (uint32_t)class_key_of(vt);
    if (!key) {
        sp_class_log("no class key for a requested spawn", vt);
        return;
    }
    memset(&sp, 0, sizeof sp);
    sp.kind = CP_SPAWN_CKEY;
    sp.host_h = handle;
    sp.host_vt = vt;
    sp.n = 3;
    sp.w[0] = key;
    sp.w[1] = handle;
    sp.w[2] = 0;
    n = cp_enc_spawn(buf, &sp);
    if (n) send_msg(MSG_SPAWN, buf, n);
    sp_class_log("spawn rebuilt from the class key", vt);
}

static void req_push(uint32_t h)
{
    lk_enter();
    if (s_req_tail - s_req_head < REQ_MAX) s_req[s_req_tail++ % REQ_MAX] = h;
    LeaveCriticalSection(&s_lock);
}

static void req_exec(void)
{
    for (;;) {
        uint32_t h = 0;
        int ok = 0;
        lk_enter();
        if (s_req_head != s_req_tail) { h = s_req[s_req_head++ % REQ_MAX]; ok = 1; }
        LeaveCriticalSection(&s_lock);
        if (!ok) break;
        spawn_resend(h);
    }
}

static void spawns_exec(void)
{
    int n = 0;
    ULONGLONG now = GetTickCount64();
    sp_loc_expire(now);
    um_tick(now);
    tech_flip_tick(now);
    while (n < 16) {
        CpSpawn sp;
        ULONGLONG t;
        int ok = 0, consume = 1;
        lk_enter();
        if (s_sp_head != s_sp_tail) {
            sp = s_sp_ring[s_sp_head % SP_RING];
            t = s_sp_t[s_sp_head % SP_RING];
            ok = 1;
        }
        LeaveCriticalSection(&s_lock);
        if (!ok) break;
        n++;
        if (sp.kind == CP_SPAWN_MSG15 && !sp.w[3] && !sp.w[4]) {
            sp_trace("placement only, ignored", &sp);
        } else if (STEP(11, sp.host_h, sp.host_vt), spawn_mapped(&sp)) {
            sp_trace("already mapped", &sp);
        } else if (STEP(12, sp.host_h, sp.host_vt), sp_try_bind(&sp, now)) {
            sp_trace("bound to a local object", &sp);
        } else if (now - t < ((cp_class(sp.host_vt) & CF_NOREPLAY) && sp.kind != CP_SPAWN_MSG15 ? 3000u + 2u * s_rtt : SP_GRACE_MS)) {
            consume = 0;
        } else if ((cp_class(sp.host_vt) & CF_NOREPLAY) && sp.kind != CP_SPAWN_MSG15) {
            sp_trace("no local object appeared, not replayed", &sp);
        } else {
            STEP(10, sp.host_h, sp.host_vt);
            sp_trace("replaying", &sp);
            if (sp_blacklisted(sp.host_vt)) { consume = 1; goto next_spawn; }
            switch (sp.kind) {
            case CP_SPAWN_MSG15: replay_msg15(&sp); break;
            case CP_SPAWN_FACT: replay_fact(&sp); break;
            case CP_SPAWN_CKEY: replay_ckey(&sp); break;
            }
        }
next_spawn:
        if (!consume) break;
        lk_enter();
        s_sp_head++;
        LeaveCriticalSection(&s_lock);
    }
}

static DWORD WINAPI tx_thread(LPVOID arg)
{
    (void)arg;
    while (!s_tx_quit) {
        WaitForSingleObject(s_tx_event, 250);
        if (s_tx_pending) {
            uint32_t n = s_tx_len;
            int ok = !s_coop_dead && send_msg(MSG_STATE, s_sbuf, n);
            InterlockedExchangeAdd(&s_tx_bytes, (LONG)n);
            InterlockedIncrement(&s_tx_msgs);
            s_tx_pending = 0;
            if (!ok) {
                if (!s_coop_dead) coop_fail("send state");
                break;
            }
        }
    }
    return 0;
}

static void tx_start(void)
{
    if (s_tx_thread || s_role != ROLE_HOST) return;
    if (!s_sbuf) s_sbuf = (uint8_t *)malloc(CP_STATE_MAX);
    if (!s_sbuf) { coop_fail("memory"); return; }
    s_tx_quit = 0;
    s_tx_pending = 0;
    s_tx_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_tx_thread = CreateThread(NULL, 0, tx_thread, NULL, 0, NULL);
}

static void tx_stop(void)
{
    if (!s_tx_thread) return;
    s_tx_quit = 1;
    SetEvent(s_tx_event);
    WaitForSingleObject(s_tx_thread, 3000);
    CloseHandle(s_tx_thread);
    CloseHandle(s_tx_event);
    s_tx_thread = NULL;
    s_tx_event = NULL;
}

static void state_cfg(void)
{
    const char *e = getenv("AVP_COOP_STATE_HZ");
    if (e && atoi(e) >= 1) s_hz = atoi(e) > 60 ? 60 : atoi(e);
    e = getenv("AVP_COOP_FULL_MS");
    if (e && atoi(e) >= 250) s_full_ms = atoi(e);
}

static void state_tick_host(ULONGLONG now)
{
    CpBuildStats bs;
    uint64_t t0, t1;
    uint32_t len;
    int full;
    if (now - s_tx_last < (ULONGLONG)(1000 / s_hz) || !s_tx_thread) return;
    if (s_tx_pending) {
        s_st.skipped++;
        s_skip_run++;
        return;
    }
    full = !s_tx_seq || now - s_tx_full_last >= (ULONGLONG)s_full_ms || s_skip_run >= (uint32_t)s_hz;
    s_skip_run = 0;
    t0 = now_us();
    len = cp_state_build(s_sbuf, CP_STATE_MAX, ++s_tx_seq, full, (uint32_t)now, &s_hooks, &bs);
    t1 = now_us();
    if (!len) return;
    s_tx_len = len;
    s_tx_pending = 1;
    SetEvent(s_tx_event);
    s_tx_last = now;
    if (full) {
        s_tx_full_last = now;
        s_st.full++;
    }
    s_st.sent++;
    s_st.recs += bs.records;
    s_st.objs += bs.objects;
    s_st.words += bs.words;
    s_st.trunc += bs.truncated;
    s_st.build_us += t1 - t0;
    if (t1 - t0 > s_st.build_max) s_st.build_max = t1 - t0;
}

static int st_pop(uint32_t *slot, uint32_t *len)
{
    int ok = 0;
    lk_enter();
    if (s_st_head != s_st_tail) {
        *slot = s_st_head % ST_RING;
        *len = s_st_len[*slot];
        ok = 1;
    }
    LeaveCriticalSection(&s_lock);
    return ok;
}

static void st_done(void)
{
    lk_enter();
    s_st_head++;
    LeaveCriticalSection(&s_lock);
}

static void state_tick_guest(ULONGLONG now)
{
    uint32_t slot, len, n = 0;
    ULONGLONG dt = s_gt_last ? now - s_gt_last : 16;
    while (n++ < ST_RING && st_pop(&slot, &len)) {
        const uint8_t *p = s_st_buf[slot];
        uint32_t flags = rd32le(p + 8), seq = rd32le(p + 4);
        if (s_st_skip && (!(flags & CP_FLAG_FULL) || (int32_t)(seq - s_st_dropseq) <= 0)) {
            s_st.rx_dropped++;
        } else {
            uint64_t t0, t1;
            if (flags & CP_FLAG_FULL) s_st_skip = 0;
            t0 = now_us();
            cp_state_apply(p, len, &s_hooks, &s_st.ap, (uint32_t)now);
            t1 = now_us();
            s_st.apply_us += t1 - t0;
            if (t1 - t0 > s_st.apply_max) s_st.apply_max = t1 - t0;
            s_st.rx++;
        }
        st_done();
    }
    cp_smooth_step((uint32_t)now, (uint32_t)dt, &s_hooks);
    if (s_st_last_ms && now - s_st_last_ms > STALL_MS) {
        if (g_avp_coop_banner == 0) {
            fprintf(stderr, "[COOP] no state from the host for %llu ms\n", (unsigned long long)(now - s_st_last_ms));
            InterlockedExchange(&g_avp_coop_banner, 2);
        }
    } else if (g_avp_coop_banner == 2) {
        InterlockedExchange(&g_avp_coop_banner, 0);
    }
}

static void state_log(ULONGLONG now)
{
    ULONGLONG dt;
    if (!s_log_last) { s_log_last = now; return; }
    dt = now - s_log_last;
    if (dt < 5000) return;
    s_log_last = now;
    if (s_role == ROLE_HOST) {
        uint32_t n = s_st.sent ? s_st.sent : 1;
        fprintf(stderr, "[COOP] host: %.1f states/s (%u full, %u skipped), %.1f KB/s, per state %u objects %u records %u words, build avg %llu us max %llu us, truncated %u; orders in %ld run %ld rejected %ld\n",
                s_st.sent * 1000.0 / (double)dt, s_st.full, s_st.skipped, s_tx_bytes / 1024.0 * 1000.0 / (double)dt,
                s_st.objs / n, s_st.recs / n, s_st.words / n, (unsigned long long)(s_st.build_us / n),
                (unsigned long long)s_st.build_max, s_st.trunc, s_ord_in, s_ord_exec, s_ord_rej);
        s_tx_bytes = 0;
    } else {
        uint32_t n = s_st.rx ? s_st.rx : 1;
        fprintf(stderr, "[COOP] guest: %.1f states/s (%u dropped), apply avg %llu us max %llu us; records %u exact %u nearby %u unmatched %u orphans %u killed %u bad %u; orders sent %ld\n",
                s_st.rx * 1000.0 / (double)dt, s_st.rx_dropped, (unsigned long long)(s_st.apply_us / n),
                (unsigned long long)s_st.apply_max, s_st.ap.records, s_st.ap.exact, s_st.ap.nearby, s_st.ap.unmatched,
                s_st.ap.orphans, s_st.ap.kills, s_st.ap.bad, s_ord_sent);
    }
    fprintf(stderr, "[COOP] round trip %u ms\n", s_rtt);
    memset(&s_st, 0, sizeof s_st);
}

void avp_coop_input_begin(void)
{
    ULONGLONG now;
    s_tick_ms = GetTickCount64();
    s_last_esp = avp_guest_esp();
#ifdef _WIN32
    if (!s_game_thread) {
        HANDLE dup = NULL;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            s_game_thread = dup;
    }
#endif
    STEP(1, 0, 0);
    launch_pump();
    s_local_input = 1;
    if (!coop_live()) return;
    now = GetTickCount64();
    g_cp_orphan_need = 2u + s_rtt / 700u;
    {
        static ULONGLONG last_ping;
        if (now - last_ping >= 1000u) {
            uint32_t ts = (uint32_t)GetTickCount();
            last_ping = now;
            send_msg(MSG_PING, &ts, 4);
        }
    }
    STEP(2, 0, 0);
    kill_pump();
    STEP(3, 0, 0);
    if (guest_live()) spawns_exec();
    STEP(4, 0, 0);
    orders_exec();
    STEP(5, 0, 0);
    if (s_role == ROLE_HOST) { req_exec(); state_tick_host(now); }
    else state_tick_guest(now);
    STEP(6, 0, 0);
    s_gt_last = now;
    state_log(now);
    STEP(0, 0, 0);
}

static volatile LONG s_wd_quit;
static HANDLE s_wd_thread;

#ifdef _WIN32
static void wp_arm(void);
static int s_watch = -1;
#endif

static DWORD WINAPI wd_thread(LPVOID arg)
{
    int logged = 0;
    float prev_clock = 0.0f;
    (void)arg;
    while (!s_wd_quit) {
        ULONGLONG now = GetTickCount64(), t = s_tick_ms;
        float clock_now = GF(CP_CLOCK);
        Sleep(250);
#ifdef _WIN32
        if (s_watch < 0) {
            const char *e = getenv("AVP_COOP_WATCH");
            s_watch = e && *e == '1';
        }
        if (s_watch && s_role == ROLE_HOST && s_mission && !g_avp_mainmenu) wp_arm();
#endif
        if (!s_mission || g_avp_mainmenu || g_avp_pcmenu_up || !t) { logged = 0; prev_clock = clock_now; continue; }
        if (now - t > 3000u) {
            if (!logged) {
                uint32_t esp = s_last_esp, i, n = 0;
                logged = 1;
                fprintf(stderr, "[COOP] game thread stalled for %llu ms; last step %u (%08X %08X); esp at the last tick %08X\n",
                        (unsigned long long)(now - t), s_step, s_step_a, s_step_b, esp);
                fprintf(stderr, "[COOP]   timer paused %u/%u, game clock %.2f (250 ms ago %.2f), shell %08X, mission %d dead %d role %d, tick age %llu ms\n",
                        (unsigned)G8(0x002F6884u), (unsigned)G8(0x002F6885u), (double)clock_now, (double)prev_clock, G32(SHELL_PTR),
                        s_mission, s_coop_dead, s_role, (unsigned long long)(now - t));
                if (esp >= 0x10000u && esp < CP_LOCAL_HI - 0x2000u)
                    for (i = 0; i < 512u && n < 10u; i++) {
                        uint32_t v = G32(esp + 4u * i);
                        if (v >= 0x00011000u && v < 0x00170000u) {
                            fprintf(stderr, "[COOP]   return address %08X at esp+%X\n", v, 4u * i);
                            n++;
                        }
                    }
#ifdef _WIN32
                if (s_game_thread) {
                    int smp;
                    uintptr_t modbase = (uintptr_t)GetModuleHandleA(NULL);
                    for (smp = 0; smp < 3; smp++) {
                        if (SuspendThread(s_game_thread) != (DWORD)-1) {
                            CONTEXT cx;
                            memset(&cx, 0, sizeof cx);
                            cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                            if (GetThreadContext(s_game_thread, &cx))
                                fprintf(stderr, "[COOP]   native sample %d: rip RVA %llX (rip %llX, module base %llX)\n", smp + 1,
                                        (unsigned long long)(cx.Rip - modbase), (unsigned long long)cx.Rip, (unsigned long long)modbase);
                            ResumeThread(s_game_thread);
                        }
                        Sleep(100);
                    }
                }
#endif
                fflush(stderr);
            }
        } else {
            logged = 0;
        }
        prev_clock = clock_now;
    }
    return 0;
}

#ifdef _WIN32
static uintptr_t s_wp_addr[4];
static uint32_t s_wp_id[4];
static int s_wp_on[4];
static void *s_veh;

static LONG CALLBACK wp_veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *cx;
    int i, hit = 0;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    cx = ep->ContextRecord;
    for (i = 0; i < 4; i++)
        if ((cx->Dr6 >> i) & 1u) {
            uint32_t esp = avp_guest_esp(), k, n = 0;
            uintptr_t modbase = (uintptr_t)GetModuleHandleA(NULL);
            hit = 1;
            fprintf(stderr, "[COOP] watch: row %08X upgraded byte written at rip RVA %llX (guest esp %08X)\n", s_wp_id[i],
                    (unsigned long long)(cx->Rip - modbase), esp);
            if (esp >= 0x10000u && esp < CP_LOCAL_HI - 0x2000u)
                for (k = 0; k < 256u && n < 12u; k++) {
                    uint32_t v = G32(esp + 4u * k);
                    if (v >= 0x00011000u && v < 0x00170000u) {
                        fprintf(stderr, "[COOP]   guest return address %08X at esp+%X\n", v, 4u * k);
                        n++;
                    }
                }
            fflush(stderr);
            cx->Dr7 &= ~(((uint64_t)3 << (2 * i)) | ((uint64_t)0xF << (16 + 4 * i)));
            s_wp_on[i] = 0;
        }
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;
    cx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void wp_arm(void)
{
    uint32_t tt = G32(AVP_TECH_TREE), r;
    CONTEXT cx;
    int i, want = 0, need = 0;
    uintptr_t cand[4];
    uint32_t cid[4];
    if (!s_game_thread || tt < 0x10000u || tt >= CP_LOCAL_HI - 0x1000u) return;
    for (i = 0; i < 4; i++)
        if (!s_wp_on[i]) need++;
    if (!need) return;
    for (r = 0; G32(tt + 0x38 + r * 8u) && r < 256u && want < need; r++) {
        uintptr_t a = (uintptr_t)(tt + 0x3Eu + r * 8u) + (uintptr_t)g_xbox_mem_offset;
        int dup = 0;
        if (G8(tt + 0x3Eu + r * 8u)) continue;
        for (i = 0; i < 4; i++)
            if (s_wp_on[i] && s_wp_addr[i] == a) dup = 1;
        if (dup) continue;
        cand[want] = a;
        cid[want] = G32(tt + 0x38 + r * 8u);
        want++;
    }
    if (!want) return;
    if (!s_veh) s_veh = AddVectoredExceptionHandler(1, wp_veh);
    if (SuspendThread(s_game_thread) == (DWORD)-1) return;
    memset(&cx, 0, sizeof cx);
    cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(s_game_thread, &cx)) {
        int w = 0;
        for (i = 0; i < 4 && w < want; i++) {
            if (s_wp_on[i]) continue;
            switch (i) {
            case 0: cx.Dr0 = cand[w]; break;
            case 1: cx.Dr1 = cand[w]; break;
            case 2: cx.Dr2 = cand[w]; break;
            default: cx.Dr3 = cand[w]; break;
            }
            cx.Dr7 |= ((uint64_t)1 << (2 * i)) | ((uint64_t)1 << (16 + 4 * i));
            cx.Dr7 &= ~((uint64_t)0xC << (16 + 4 * i));
            s_wp_addr[i] = cand[w];
            s_wp_id[i] = cid[w];
            s_wp_on[i] = 1;
            w++;
        }
        SetThreadContext(s_game_thread, &cx);
    }
    ResumeThread(s_game_thread);
}
#endif

static void wd_start(void)
{
    if (s_wd_thread) return;
    s_wd_quit = 0;
    s_wd_thread = CreateThread(NULL, 0, wd_thread, NULL, 0, NULL);
}

void avp_coop_input_end(void)
{
    s_local_input = 0;
    if (coop_live()) sel_sync();
}

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
    InterlockedExchange(&g_avp_coop_banner, 1);
    while (!s_peer_started && !s_coop_dead) {
        if (GetTickCount64() - t0 > 120000) {
            InterlockedExchange(&g_avp_coop_banner, 0);
            coop_fail("the other player never started");
            return;
        }
        Sleep(5);
    }
    InterlockedExchange(&g_avp_coop_banner, 0);
    if (!s_coop_dead && *s_my_map && *s_peer_map && strcmp(s_my_map, s_peer_map)) {
        char why[64];
        snprintf(why, sizeof why, "the games loaded different missions (%s and %s)", s_my_map, s_peer_map);
        coop_fail(why);
    }
    if (!s_coop_dead) make_pointers();
    if (!s_coop_dead)
        fprintf(stderr, "[COOP] both players in, host-authoritative state stream on\n");
}

static void coop_reset_all(void)
{
    uint32_t i;
    net_stop();
#ifdef _WIN32
    if (s_wsa_ok) {
        WSACleanup();
        s_wsa_ok = 0;
    }
#endif
    if (s_lock_ready) {
        lk_enter();
        s_st_head = s_st_tail = s_st_dropseq = 0;
        s_ord_head = s_ord_tail = 0;
        s_st_skip = 0;
        LeaveCriticalSection(&s_lock);
    }
    spawn_reset();
    s_ord_in = 0;
    s_ord_drop = 0;
    s_ord_sent = s_ord_exec = s_ord_rej = 0;
    for (i = 0; i < ST_RING; i++) {
        free(s_st_buf[i]);
        s_st_buf[i] = NULL;
        s_st_len[i] = 0;
    }
    free(s_st_scratch);
    s_st_scratch = NULL;
    free(s_sbuf);
    s_sbuf = NULL;
    s_st_last_ms = 0;
    s_tx_quit = 0;
    s_tx_pending = 0;
    s_tx_len = s_tx_seq = 0;
    s_tx_last = s_tx_full_last = s_gt_last = s_log_last = 0;
    s_tx_bytes = s_tx_msgs = 0;
    memset(&s_st, 0, sizeof s_st);
    s_mission = 0;
    s_mission_seen = 0;
    s_peer_started = 0;
    s_my_map[0] = 0;
    s_local_input = s_executing = s_allow_death = 0;
    s_kill_n = 0;
    s_nsel_sent = 0;
    s_last_net_err = 0;
    s_netlog_n = 0;
    memset(s_netlog, 0, sizeof s_netlog);
    s_ptr[0] = s_ptr[1] = 0;
    s_vs = 0;
    s_vs_slot[0] = s_vs_slot[1] = 0;
    memset(s_owner, 0, sizeof s_owner);
    memset(s_owner_h, 0, sizeof s_owner_h);
    memset(s_hide_h, 0, sizeof s_hide_h);
    s_hide_head = 0;
    s_scan[0] = 0;
    InterlockedExchange(&g_avp_coop_banner, 0);
    cp_reset();
    avp_skirmish_disarm();
    if (s_env_skirm) {
        _putenv("AVP_SKIRMISH=");
        s_env_skirm = 0;
    }
}

void avp_coop_reset(void)
{
    coop_reset_all();
}

void avp_coop_mission_end(void)
{
    if (s_mission) {
        coop_reset_all();
        return;
    }
    s_mission_seen = 0;
    s_launch_t = 0;
}

static void tech_repair(void)
{
    uint32_t tt = G32(AVP_TECH_TREE), world = G32(AVP_WORLD), pm, species, row, h, n = 0, have = 0;
    if (tt < 0x10000u || tt >= CP_LOCAL_HI - 0x1000u || world < 0x10000u || world >= CP_LOCAL_HI - 0x100u) return;
    pm = G32(world + 0x78);
    if (pm < 0x10000u || pm >= CP_LOCAL_HI - 0x520u) return;
    species = G32(pm + 0x114);
    for (row = tt + 0x38; n < 256 && G32(row); row += 8, n++)
        if (G8(row + 7) == 1) have = 1;
    if (have || !n) return;
    for (h = 0; h < 0x10000u; h++) {
        uint32_t obj = G32(AVP_OBJ_TABLE + h * 4u), arr, a[3];
        if (!cp_obj_ok(obj) || G32(obj) != AVP_TECH_VT) continue;
        arr = G32(obj + 0x34);
        if (arr < 0x10000u || !(G32(obj + 0x21C) & species)) continue;
        a[0] = tt;
        a[1] = arr;
        a[2] = 1;
        avp_gcall(AVP_TECH_INIT, 0, a, 3, 0);
        fprintf(stderr, "[COOP] player tech rows rebuilt from tree %08X (species %u)\n", obj, species);
    }
}

static void log_unit_classes(void)
{
    uint32_t list = G32(SP_REG_LIST), data, cnt, i, shown = 0, total = 0;
    char line[400];
    size_t len = 0;
    if (!sp_ptr(list)) return;
    data = G32(list);
    cnt = G32(list + 4);
    if (!sp_ptr(data) || cnt > 8192u) return;
    line[0] = 0;
    for (i = 0; i < cnt; i++) {
        uint32_t d = G32(data + 4u * i), c, hops;
        char nm[64];
        int unit = 0;
        if (!sp_ptr(d) || !sp_name_at(G32(d + 4), nm, sizeof nm)) continue;
        c = d;
        for (hops = 0; c && hops < 24u; hops++) {
            char pn[64];
            if (sp_name_at(G32(c + 4), pn, sizeof pn) && !strcmp(pn, "GUnit")) { unit = 1; break; }
            c = G32(c + 12);
            if (!sp_ptr(c)) break;
        }
        if (!unit) continue;
        total++;
        if (len + strlen(nm) + 2 >= sizeof line) {
            fprintf(stderr, "[COOP] unit classes:%s\n", line);
            line[0] = 0;
            len = 0;
        }
        len += (size_t)snprintf(line + len, sizeof line - len, " %s", nm);
        shown++;
    }
    if (len) fprintf(stderr, "[COOP] unit classes:%s\n", line);
    fprintf(stderr, "[COOP] %u unit classes are replicated by class, morphs and hatches replay as source to new class\n", total);
    (void)shown;
}

void avp_coop_mission_start(void)
{
    ULONGLONG now;
    coop_init();
    s_mission_seen = 1;
    if (s_role == ROLE_NONE || s_mission) return;
    state_cfg();
    tech_repair();
    cp_reset();
    spawn_reset();
    memset(&s_st, 0, sizeof s_st);
    memset(s_owner, 0, sizeof s_owner);
    memset(s_owner_h, 0, sizeof s_owner_h);
    s_tx_seq = 0;
    s_nsel_sent = 0;
    lk_enter();
    s_st_head = s_st_tail = 0;
    s_ord_head = s_ord_tail = 0;
    s_st_skip = 0;
    LeaveCriticalSection(&s_lock);
    versus_init();
    log_unit_classes();
    wait_peer_start();
    now = GetTickCount64();
    s_st_last_ms = now;
    s_gt_last = 0;
    s_log_last = 0;
    s_tx_last = 0;
    s_tx_full_last = 0;
    s_mission = 1;
    s_launch_t = 0;
    if (!s_coop_dead) tx_start();
    s_tick_ms = GetTickCount64();
    wd_start();
    if (s_role != ROLE_NONE)
        fprintf(stderr, "[COOP] mission start as the %s: %d Hz state, full every %d ms\n",
                s_role == ROLE_HOST ? "host" : "guest", s_hz, s_full_ms);
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
