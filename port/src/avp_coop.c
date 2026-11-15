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
#include <string.h>
#include "avp_skirmish.h"
#include "avp_coop.h"

#include "avp_coop_proto.h"

extern ptrdiff_t g_xbox_mem_offset;
static void tx_stop(void);
static void coop_fail(const char *why);
static int send_msg(uint32_t type, const void *body, uint32_t len);

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
enum { MSG_HELLO = 1, MSG_START = 2, MSG_CFG = 6, MSG_LAUNCH = 7, MSG_PICK = 8, MSG_READY = 9, MSG_STATE = 13, MSG_ORDER = 14, MSG_MIRROR = 15 };

#define COOP_MAGIC   0x43505641u
#define COOP_VERSION 8u
#define ST_RING      6u
#define ORD_RING     128u
#define STALL_MS     3000u

static int s_role;
static void coop_start_net(void);
static int s_mission_seen, s_mission, s_inited;
static int s_peer_started, s_coop_dead;
static SOCKET s_sock = INVALID_SOCKET;
static CRITICAL_SECTION s_lock;
static CRITICAL_SECTION s_send_lock;
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
        if (!obj || G8(obj + 0x26Du)) continue;
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
    if (s_sock != INVALID_SOCKET) shutdown(s_sock, SD_BOTH);
    coop_fail("the other game sent malformed data");
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
    {
#ifdef _WIN32
        DWORD to = 4000;
#else
        struct timeval to = { 4, 0 };
#endif
        setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    }
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
        if (hdr[0] != MSG_STATE && hdr[1] > 4096u) { proto_fail("oversized message", hdr[0], hdr[1]); return 0; }
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
        } else if (hdr[0] == MSG_STATE) {
            uint32_t slot, full;
            uint8_t *dst;
            if (s_role != ROLE_GUEST || hdr[1] < CP_HDR || hdr[1] > CP_STATE_MAX) { proto_fail("state size", hdr[0], hdr[1]); return 0; }
            if (!s_st_scratch) s_st_scratch = (uint8_t *)malloc(CP_STATE_MAX);
            EnterCriticalSection(&s_lock);
            full = s_st_tail - s_st_head >= ST_RING;
            slot = s_st_tail % ST_RING;
            LeaveCriticalSection(&s_lock);
            if (!s_st_buf[slot]) s_st_buf[slot] = (uint8_t *)malloc(CP_STATE_MAX);
            dst = full ? s_st_scratch : s_st_buf[slot];
            if (!dst) { coop_fail("memory"); return 0; }
            if (!recv_all(dst, (int)hdr[1])) { coop_fail("state data"); return 0; }
            if (!cp_state_validate(dst, hdr[1])) { proto_fail("state contents", hdr[0], hdr[1]); return 0; }
            s_st_last_ms = GetTickCount64();
            EnterCriticalSection(&s_lock);
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
            if (s_role != ROLE_HOST || hdr[1] < 4 || hdr[1] > CP_ORDER_WIRE_MAX) { proto_fail("order size", hdr[0], hdr[1]); return 0; }
            if (!recv_all(body, (int)hdr[1])) { coop_fail("order data"); return 0; }
            if (!cp_order_parse(body, hdr[1], &ord) || ord.cmd == CP_CMD_REINF) { proto_fail("order contents", hdr[0], hdr[1]); return 0; }
            EnterCriticalSection(&s_lock);
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
            if (s_role != ROLE_GUEST || hdr[1] < 8 || hdr[1] > CP_ORDER_WIRE_MAX + 4) { proto_fail("mirror size", hdr[0], hdr[1]); return 0; }
            if (!recv_all(body, (int)hdr[1])) { coop_fail("mirror data"); return 0; }
            memcpy(&pl, body, 4);
            if (pl > 1u || !cp_order_parse(body + 4, hdr[1] - 4, &ord) || ord.cmd == CP_CMD_GIVE) { proto_fail("mirror contents", hdr[0], hdr[1]); return 0; }
            ord.player = pl;
            EnterCriticalSection(&s_lock);
            if (s_ord_tail - s_ord_head < ORD_RING) {
                s_ord[s_ord_tail % ORD_RING] = ord;
                s_ord_rawlen[s_ord_tail % ORD_RING] = 0;
                s_ord_tail++;
            }
            LeaveCriticalSection(&s_lock);
        } else {
            coop_fail("unknown message");
            return 0;
        }
    }
}

static void net_begin(const char *spec, int role, int tries, const char *status)
{
    lock_init();
    snprintf(s_spec, sizeof s_spec, "%s", spec);
    EnterCriticalSection(&s_lock);
    s_st_head = s_st_tail = 0;
    s_ord_head = s_ord_tail = 0;
    s_st_skip = 0;
    LeaveCriticalSection(&s_lock);
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
    if (s_sock != INVALID_SOCKET) fprintf(stderr, "[COOP] closing the connection on purpose (role %d, mission %d)\n", s_role, s_mission);
    tx_stop();
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

int avp_coop_host(int port)
{
    char spec[32], status[64];
    if (!net_allowed()) return 0;
    net_stop();
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
    net_stop();
    if (!ip || !*ip || strchr(ip, ':')) ip = "127.0.0.1";
    if (port < 1 || port > 65535) port = 27015;
    snprintf(spec, sizeof spec, "join:%.100s:%d", ip, port);
    if (!wsa_ensure()) return 0;
    snprintf(status, sizeof status, "Connecting to %.100s:%d", ip, port);
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
    kill_queue(h);
}

static void hk_unmatched(uint32_t vt)
{
    if (!(cp_class(vt) & CF_UNIT)) return;
    static uint32_t seen[32];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (seen[i] == vt) return;
    if (n < 32) seen[n++] = vt;
    {
        char nm[48] = "";
        uint32_t col = G32(vt - 4u), td = col >= 0x10000u && col < CP_LOCAL_HI - 0x100u ? G32(col + 12u) : 0, k = 0;
        if (td >= 0x10000u && td < CP_LOCAL_HI - 0x100u)
            while (k + 1 < sizeof nm && G8(td + 8u + k)) { nm[k] = (char)G8(td + 8u + k); k++; }
        nm[k] = 0;
        fprintf(stderr, "[COOP] no object here for host class %08X %s; no safe generic spawn path, not created\n", vt, nm);
    }
}

static void call_vec(uint32_t obj, uint32_t slot, const float *v)
{
    uint32_t esp = avp_guest_esp(), va = avp_guest_alloca(16), i, vt = G32(obj);
    uint32_t fn = vt >= 0x10000u && vt < CP_LOCAL_HI - 0x400u ? G32(vt + slot * 4u) : 0;
    if (fn < 0x00011000u || fn >= 0x00170D60u) { avp_guest_set_esp(esp); return; }
    for (i = 0; i < 3; i++) GF(va + 4 * i) = v[i];
    avp_gcall(fn, obj, (uint32_t[]){ va }, 1, 0);
    avp_guest_set_esp(esp);
}

static void hk_setpos(uint32_t obj, const float *v)
{
    call_vec(obj, 22, v);
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
    if (!obj || !(cp_class(G32(obj)) & CF_UNIT)) return 0;
    o = owner_of(handle);
    if (s_vs) {
        if (o < 0) return 0;
    } else {
        fs = friend_player();
        if (fs < 0 || (int)(handle & 0xFFFFu) != fs) return 0;
    }
    return o == local_player() ? 2 : 1;
}

int avp_coop_may_select(uint32_t handle)
{
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
        if (told++ < 8) fprintf(stderr, "[COOP] order type %08X is not forwarded\n", type);
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

static void exec_select(const CpOrder *o, int player)
{
    uint32_t hs[SEL_MAX], n = 0, i;
    for (i = 0; i < o->n && i < SEL_MAX; i++) {
        if (!cp_unit_ok(o->h[i])) continue;
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
    G32(p + 0xDC) = (under && cp_target_ok(under)) ? under : 0;
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
    if (!cp_unit_ok(h)) { reject("unit does not exist", o); return; }
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
    EnterCriticalSection(&s_lock);
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
        return;
    }
    full = !s_tx_seq || now - s_tx_full_last >= (ULONGLONG)s_full_ms;
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
    EnterCriticalSection(&s_lock);
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
    EnterCriticalSection(&s_lock);
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
    memset(&s_st, 0, sizeof s_st);
}

void avp_coop_input_begin(void)
{
    ULONGLONG now;
    launch_pump();
    s_local_input = 1;
    if (!coop_live()) return;
    now = GetTickCount64();
    kill_pump();
    orders_exec();
    if (s_role == ROLE_HOST) state_tick_host(now);
    else state_tick_guest(now);
    s_gt_last = now;
    state_log(now);
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

void avp_coop_mission_end(void)
{
    if (!s_mission) { s_mission_seen = 0; s_launch_t = 0; }
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

void avp_coop_mission_start(void)
{
    ULONGLONG now;
    coop_init();
    s_mission_seen = 1;
    if (s_role == ROLE_NONE || s_mission) return;
    state_cfg();
    tech_repair();
    cp_reset();
    memset(&s_st, 0, sizeof s_st);
    memset(s_owner, 0, sizeof s_owner);
    memset(s_owner_h, 0, sizeof s_owner_h);
    s_tx_seq = 0;
    s_nsel_sent = 0;
    EnterCriticalSection(&s_lock);
    s_st_head = s_st_tail = 0;
    s_ord_head = s_ord_tail = 0;
    s_st_skip = 0;
    LeaveCriticalSection(&s_lock);
    versus_init();
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
