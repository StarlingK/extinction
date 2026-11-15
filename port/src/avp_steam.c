#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "avp_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <wincrypt.h>
#include <wintrust.h>
#include <bcrypt.h>
#else
#include <dlfcn.h>
#endif
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include "avp_steam.h"

extern int avp_coop_version(void);

#define APPID_TEXT "480"
#define APPID_NUM 480ull
#define LOBBY_MAX 2
#define LOBBY_TYPE_PUBLIC 2
#define CH_COOP 0
#define SEND_RELIABLE 8
#define RES_OK 1
#define RES_LIMIT 25
#define CHUNK 131072u
#define RX_CAP (1u << 23)
#define RX_MSG_MAX 524288
#define FRIEND_FLAG_IMMEDIATE 4
#define PEND_MAX 8
#define FR_MAX 8
#define CODE_LEN 6

enum { CB_LOBBY_ENTER = 504, CB_LOBBY_LIST = 510, CB_LOBBY_CREATED = 513, CB_JOIN_REQ = 333, CB_CALL_DONE = 703,
       CB_SESS_REQ = 1251, CB_SESS_FAIL = 1252 };
enum { PK_NONE, PK_CREATE, PK_LIST, PK_JOIN };
enum { LK_NONE, LK_LOBBY, LK_UP, LK_DEAD };
enum { OP_IDLE, OP_RUN, OP_OK, OP_FAIL };

typedef struct { uint64_t w[17]; } Ident;

typedef struct {
    int32_t user;
    int32_t id;
    uint8_t *param;
    int32_t cub;
} CbMsg;

typedef struct { uint64_t call; int kind; uint32_t gen; } Pend;

typedef int (*fn_initflat)(char *err);
typedef int (*fn_initold)(void);
typedef int (*fn_isrunning)(void);
typedef void (*fn_void)(void);
typedef int (*fn_getpipe)(void);
typedef void (*fn_runframe)(int);
typedef int (*fn_nextcb)(int, CbMsg *);
typedef void (*fn_freecb)(int);
typedef int (*fn_callres)(int, uint64_t, void *, int, int, uint8_t *);
typedef void *(*fn_iface)(void);
typedef uint64_t (*fn_call_ii)(void *, int, int);
typedef uint64_t (*fn_call_u)(void *, uint64_t);
typedef uint64_t (*fn_call_0)(void *);
typedef void (*fn_leave)(void *, uint64_t);
typedef int (*fn_geti_u)(void *, uint64_t);
typedef uint64_t (*fn_get_ui)(void *, uint64_t, int);
typedef const char *(*fn_getdata)(void *, uint64_t, const char *);
typedef int (*fn_setdata)(void *, uint64_t, const char *, const char *);
typedef int (*fn_setjoin)(void *, uint64_t, int);
typedef int (*fn_reqdata)(void *, uint64_t);
typedef void (*fn_strfilter)(void *, const char *, const char *, int);
typedef void (*fn_intset)(void *, int);
typedef uint64_t (*fn_getbyidx)(void *, int);
typedef int (*fn_getcount)(void *, int);
typedef uint64_t (*fn_getfriend)(void *, int, int);
typedef const char *(*fn_getname)(void *, uint64_t);
typedef const char *(*fn_getmyname)(void *);
typedef int (*fn_gameplayed)(void *, uint64_t, void *);
typedef int (*fn_sendmsg)(void *, const Ident *, const void *, uint32_t, int, int);
typedef int (*fn_recvmsg)(void *, int, void **, int);
typedef int (*fn_acceptsess)(void *, const Ident *);
typedef void (*fn_setid64)(Ident *, uint64_t);
typedef uint64_t (*fn_getid64)(const Ident *);
typedef void (*fn_msgrelease)(void *);
typedef int (*fn_initrelay)(void *);

static struct {
    fn_initflat init_flat;
    fn_initold init_old;
    fn_isrunning is_running;
    fn_void man_init;
    fn_getpipe get_pipe;
    fn_runframe run_frame;
    fn_nextcb next_cb;
    fn_freecb free_cb;
    fn_callres call_res;
    fn_iface mm_get, fr_get, usr_get, nm_get, ut_get;
    fn_call_ii create_lobby;
    fn_call_u join_lobby;
    fn_call_0 request_list;
    fn_leave leave_lobby;
    fn_geti_u num_members, member_limit;
    fn_get_ui member_by_idx;
    fn_getdata get_data;
    fn_setdata set_data;
    fn_setjoin set_joinable;
    fn_reqdata req_data;
    fn_call_u get_owner;
    fn_strfilter str_filter;
    fn_intset dist_filter, count_filter, slots_filter;
    fn_getbyidx lobby_by_idx;
    fn_getcount friend_count;
    fn_getfriend friend_by_idx;
    fn_getname friend_name;
    fn_getmyname my_name;
    fn_gameplayed game_played;
    fn_call_0 my_id;
    fn_sendmsg send_msg;
    fn_recvmsg recv_msg;
    fn_acceptsess accept_sess, close_sess;
    fn_setid64 set_id;
    fn_getid64 get_id;
    fn_msgrelease msg_release;
    fn_initrelay init_relay;
} F;

static CRITICAL_SECTION s_sl, s_rl, s_txl;
static int s_locks_ready;
static HANDLE s_thread, s_ev;
static volatile int s_status, s_link, s_op, s_quit;
static char s_text[96], s_err[96], s_code[CODE_LEN + 2], s_peer_name[32], s_my_name[32];
static void *s_mm, *s_fr, *s_usr, *s_nm;
static int s_pipe;
static uint64_t s_me, s_lobby, s_peer, s_req_id, s_join_lobby, s_cmd_lobby;
static int s_host, s_accepted, s_join_flag, s_cmd_flag, s_cmd_done;
static uint32_t s_gen;
static Ident s_ident;
static Pend s_pend[PEND_MAX];
static char s_want[CODE_LEN + 2];
static char s_ver[16];
static uint8_t *s_rx;
static uint64_t s_rx_w, s_rx_r;
static uint8_t *s_fb;
static int s_first_tx, s_first_rx;
static uint32_t s_fb_cap;
static AvpSteamFriend s_fl[FR_MAX];
static int s_fl_n, s_fl_want;
static ULONGLONG s_fl_t;

static void slog(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[STEAM] %s\n", b);
    fflush(stderr);
}

static const char k_alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

static void locks_init(void)
{
    if (s_locks_ready) return;
    InitializeCriticalSection(&s_sl);
    InitializeCriticalSection(&s_rl);
    InitializeCriticalSection(&s_txl);
    s_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_locks_ready = 1;
}

static void set_text(const char *t)
{
    EnterCriticalSection(&s_sl);
    snprintf(s_text, sizeof s_text, "%s", t);
    LeaveCriticalSection(&s_sl);
}

static void set_err(const char *t)
{
    EnterCriticalSection(&s_sl);
    snprintf(s_err, sizeof s_err, "%s", t);
    LeaveCriticalSection(&s_sl);
}

static void fail_init(const char *t)
{
    set_text(t);
    s_status = AVP_STEAM_FAILED;
    fprintf(stderr, "[STEAM] %s\n", t);
}

int avp_steam_valid_code(const char *c)
{
    int i;
    if (!c || (int)strlen(c) != CODE_LEN) return 0;
    for (i = 0; i < CODE_LEN; i++)
        if (!strchr(k_alpha, c[i]) || !c[i]) return 0;
    return 1;
}

static int random_bytes(uint8_t *b, int n)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, b, (ULONG)n, 2) == 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    int ok;
    if (!f) return 0;
    ok = fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
#endif
}

static int make_code(char *out)
{
    uint8_t b[CODE_LEN];
    int i;
    if (!random_bytes(b, CODE_LEN)) return 0;
    for (i = 0; i < CODE_LEN; i++) out[i] = k_alpha[b[i] & 31];
    out[CODE_LEN] = 0;
    return 1;
}

#ifdef _WIN32
typedef LONG (WINAPI *fn_wvt)(HWND, GUID *, void *);
typedef CRYPT_PROVIDER_DATA *(WINAPI *fn_wt_data)(HANDLE);
typedef CRYPT_PROVIDER_SGNR *(WINAPI *fn_wt_sgnr)(CRYPT_PROVIDER_DATA *, DWORD, BOOL, DWORD);
typedef CRYPT_PROVIDER_CERT *(WINAPI *fn_wt_cert)(CRYPT_PROVIDER_SGNR *, DWORD);
typedef DWORD (WINAPI *fn_cgns)(PCCERT_CONTEXT, DWORD, DWORD, void *, LPSTR, DWORD);

static int verify_valve(const wchar_t *path, HANDLE *keep)
{
    static GUID action = { 0x00AAC56B, 0xCD44, 0x11D0, { 0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE } };
    HANDLE f;
    HMODULE wt, cr;
    fn_wvt verify;
    fn_wt_data pdata;
    fn_wt_sgnr psgnr;
    fn_wt_cert pcert;
    fn_cgns cgns;
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    LONG rc;
    int ok = 0;
    *keep = NULL;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    wt = LoadLibraryExW(L"wintrust.dll", NULL, 0x800);
    cr = LoadLibraryExW(L"crypt32.dll", NULL, 0x800);
    if (!wt || !cr) { CloseHandle(f); return 0; }
    verify = (fn_wvt)(void *)GetProcAddress(wt, "WinVerifyTrust");
    pdata = (fn_wt_data)(void *)GetProcAddress(wt, "WTHelperProvDataFromStateData");
    psgnr = (fn_wt_sgnr)(void *)GetProcAddress(wt, "WTHelperGetProvSignerFromChain");
    pcert = (fn_wt_cert)(void *)GetProcAddress(wt, "WTHelperGetProvCertFromChain");
    cgns = (fn_cgns)(void *)GetProcAddress(cr, "CertGetNameStringA");
    if (!verify || !pdata || !psgnr || !pcert || !cgns) { CloseHandle(f); return 0; }
    memset(&fi, 0, sizeof fi);
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path;
    fi.hFile = f;
    memset(&wd, 0, sizeof wd);
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    rc = verify((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (rc == 0) {
        CRYPT_PROVIDER_DATA *pd = pdata(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR *sg = pd ? psgnr(pd, 0, FALSE, 0) : NULL;
        CRYPT_PROVIDER_CERT *pc = sg ? pcert(sg, 0) : NULL;
        if (pc && pc->pCert) {
            char name[512];
            DWORD flag = 3;
            name[0] = 0;
            if (cgns(pc->pCert, 2, 0, &flag, name, sizeof name) && strstr(name, "Valve Corp.")) ok = 1;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    verify((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (!ok) { CloseHandle(f); return 0; }
    *keep = f;
    return 1;
}

static int exe_dir(wchar_t *d, size_t n)
{
    DWORD k = GetModuleFileNameW(NULL, d, (DWORD)n), i;
    if (!k || k >= n) return 0;
    for (i = k; i > 0; i--)
        if (d[i - 1] == L'\\' || d[i - 1] == L'/') { d[i] = 0; return 1; }
    return 0;
}
#else
static int exe_dir(char *d, size_t n)
{
    ssize_t k = readlink("/proc/self/exe", d, n - 1), i;
    if (k <= 0) return 0;
    d[k] = 0;
    for (i = k; i > 0; i--)
        if (d[i - 1] == '/') { d[i] = 0; return 1; }
    return 0;
}
#endif

static int exe_dir_a(char *d, size_t n)
{
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, d, (DWORD)n), i;
    if (!k || k >= n) return 0;
    for (i = k; i > 0; i--)
        if (d[i - 1] == '\\' || d[i - 1] == '/') { d[i] = 0; return 1; }
    return 0;
#else
    return exe_dir(d, n);
#endif
}

static int appid_at(const char *prefix)
{
    char p[1200];
    FILE *f;
    snprintf(p, sizeof p, "%ssteam_appid.txt", prefix);
    f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    f = fopen(p, "wb");
    if (!f) return 0;
    fputs(APPID_TEXT, f);
    fclose(f);
    return 1;
}

static void write_appid(void)
{
    char d[1100];
    int ok = appid_at("");
    if (exe_dir_a(d, sizeof d) && appid_at(d)) ok = 1;
    if (!ok && !getenv("SteamAppId")) {
#ifdef _WIN32
        _putenv("SteamAppId=" APPID_TEXT);
        _putenv("SteamGameId=" APPID_TEXT);
#else
        setenv("SteamAppId", APPID_TEXT, 0);
        setenv("SteamGameId", APPID_TEXT, 0);
#endif
    }
}

#ifdef _WIN32
#define LIB_NAME "steam_api64.dll"
#else
#define LIB_NAME "libsteam_api.so"
#endif

static int make_dir(const char *p)
{
#ifdef _WIN32
    return CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}

#ifdef AVP_STEAM_EMBED
#ifndef _WIN32
extern const uint8_t avp_steam_blob_start[];
extern const uint8_t avp_steam_blob_end[];
#endif

static int embedded(const uint8_t **p, size_t *n)
{
#ifdef _WIN32
    HRSRC r = FindResourceA(NULL, "AVPSTEAM", (LPCSTR)RT_RCDATA);
    HGLOBAL g;
    if (!r) return 0;
    g = LoadResource(NULL, r);
    if (!g) return 0;
    *p = (const uint8_t *)LockResource(g);
    *n = (size_t)SizeofResource(NULL, r);
    return *p && *n;
#else
    *p = avp_steam_blob_start;
    *n = (size_t)(avp_steam_blob_end - avp_steam_blob_start);
    return *n > 0;
#endif
}
#else
static int embedded(const uint8_t **p, size_t *n)
{
    (void)p;
    (void)n;
    return 0;
}
#endif

static int same_file(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[8192];
    size_t got, off = 0;
    int ok = 1;
    if (!f) return 0;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (off + got > n || memcmp(buf, d + off, got)) { ok = 0; break; }
        off += got;
    }
    fclose(f);
    return ok && off == n;
}

static int unpack_into(const char *steamdir, char *out, size_t cap)
{
    const uint8_t *d;
    size_t n;
    char tmp[1200];
    FILE *f;
    if (!embedded(&d, &n)) return 0;
    snprintf(out, cap, "%s" AVP_SEP LIB_NAME, steamdir);
    if (same_file(out, d, n)) return 1;
    if (!make_dir(steamdir)) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    f = fopen(tmp, "wb");
    if (!f) return 0;
    if (fwrite(d, 1, n, f) != n) { fclose(f); remove(tmp); return 0; }
    fclose(f);
#ifdef _WIN32
    if (!MoveFileExA(tmp, out, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return 0; }
#else
    if (rename(tmp, out) != 0) { remove(tmp); return 0; }
#endif
    slog("Steam library unpacked");
    return same_file(out, d, n);
}

int avp_steam_unpack(const char *gamedir)
{
    char sd[1200], out[1300];
    if (!gamedir || !*gamedir) return 0;
    snprintf(sd, sizeof sd, "%s" AVP_SEP "steam", gamedir);
    return unpack_into(sd, out, sizeof out);
}

static int lib_path(char *out, size_t cap)
{
    char cwd[1100], sd[1200];
    const uint8_t *d;
    size_t n;
    if (embedded(&d, &n)) {
#ifdef _WIN32
        if (!GetCurrentDirectoryA(sizeof cwd, cwd)) return 0;
#else
        if (!getcwd(cwd, sizeof cwd)) return 0;
#endif
        snprintf(sd, sizeof sd, "%s" AVP_SEP "game", cwd);
        make_dir(sd);
        snprintf(sd, sizeof sd, "%s" AVP_SEP "game" AVP_SEP "steam", cwd);
        return unpack_into(sd, out, cap);
    }
    if (exe_dir_a(sd, sizeof sd)) {
        FILE *f;
        snprintf(out, cap, "%s" LIB_NAME, sd);
        f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

#ifdef _WIN32
#define GETSYM(h, n) ((void *)(uintptr_t)GetProcAddress((HMODULE)(h), n))
#else
#define GETSYM(h, n) dlsym(h, n)
#endif
#define SYM(h, f, n) ((f) = (void *)GETSYM(h, n))

static void *load_library(const char **why)
{
    void *lib = NULL;
    char ap[1400];
    if (!lib_path(ap, sizeof ap)) { *why = "Steam library missing"; return NULL; }
#ifdef _WIN32
    {
        wchar_t p[1400];
        HANDLE keep = NULL;
        int v;
        if (!MultiByteToWideChar(CP_ACP, 0, ap, -1, p, 1400)) { *why = "Steam library missing"; return NULL; }
        v = verify_valve(p, &keep);
        if (v < 0) { *why = "Steam library missing"; return NULL; }
        if (!v) { *why = "Steam library failed the signature check"; return NULL; }
        lib = (void *)LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        CloseHandle(keep);
        if (!lib) *why = "Steam library could not be loaded";
    }
#else
    lib = dlopen(ap, RTLD_NOW | RTLD_LOCAL);
    if (!lib) *why = "Steam library could not be loaded";
#endif
    return lib;
}

static int resolve(void *h)
{
    memset(&F, 0, sizeof F);
    SYM(h, F.init_flat, "SteamAPI_InitFlat");
    SYM(h, F.init_old, "SteamAPI_Init");
    SYM(h, F.is_running, "SteamAPI_IsSteamRunning");
    SYM(h, F.man_init, "SteamAPI_ManualDispatch_Init");
    SYM(h, F.get_pipe, "SteamAPI_GetHSteamPipe");
    SYM(h, F.run_frame, "SteamAPI_ManualDispatch_RunFrame");
    SYM(h, F.next_cb, "SteamAPI_ManualDispatch_GetNextCallback");
    SYM(h, F.free_cb, "SteamAPI_ManualDispatch_FreeLastCallback");
    SYM(h, F.call_res, "SteamAPI_ManualDispatch_GetAPICallResult");
    SYM(h, F.mm_get, "SteamAPI_SteamMatchmaking_v009");
    SYM(h, F.fr_get, "SteamAPI_SteamFriends_v018");
    if (!F.fr_get) SYM(h, F.fr_get, "SteamAPI_SteamFriends_v017");
    SYM(h, F.usr_get, "SteamAPI_SteamUser_v023");
    SYM(h, F.nm_get, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    SYM(h, F.ut_get, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    SYM(h, F.create_lobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    SYM(h, F.join_lobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    SYM(h, F.request_list, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    SYM(h, F.leave_lobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    SYM(h, F.num_members, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    SYM(h, F.member_limit, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit");
    SYM(h, F.member_by_idx, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    SYM(h, F.get_data, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    SYM(h, F.set_data, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    SYM(h, F.set_joinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    SYM(h, F.req_data, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    SYM(h, F.get_owner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    SYM(h, F.str_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    SYM(h, F.dist_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    SYM(h, F.count_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    SYM(h, F.slots_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable");
    SYM(h, F.lobby_by_idx, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    SYM(h, F.friend_count, "SteamAPI_ISteamFriends_GetFriendCount");
    SYM(h, F.friend_by_idx, "SteamAPI_ISteamFriends_GetFriendByIndex");
    SYM(h, F.friend_name, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    SYM(h, F.my_name, "SteamAPI_ISteamFriends_GetPersonaName");
    SYM(h, F.game_played, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    SYM(h, F.my_id, "SteamAPI_ISteamUser_GetSteamID");
    SYM(h, F.send_msg, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    SYM(h, F.recv_msg, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    SYM(h, F.accept_sess, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    SYM(h, F.close_sess, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    SYM(h, F.set_id, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    SYM(h, F.get_id, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    SYM(h, F.msg_release, "SteamAPI_SteamNetworkingMessage_t_Release");
    SYM(h, F.init_relay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    if (!F.init_flat && !F.init_old) return 0;
    return F.man_init && F.get_pipe && F.run_frame && F.next_cb && F.free_cb && F.call_res && F.mm_get && F.fr_get &&
           F.usr_get && F.nm_get && F.create_lobby && F.join_lobby && F.request_list && F.leave_lobby &&
           F.num_members && F.member_by_idx && F.get_data && F.set_data && F.get_owner && F.str_filter &&
           F.lobby_by_idx && F.friend_count && F.friend_by_idx && F.friend_name && F.my_name && F.game_played &&
           F.my_id && F.send_msg && F.recv_msg && F.accept_sess && F.close_sess && F.set_id && F.get_id &&
           F.msg_release;
}

static void pend_add(uint64_t call, int kind)
{
    int i;
    if (!call) return;
    for (i = 0; i < PEND_MAX; i++)
        if (!s_pend[i].kind) {
            s_pend[i].call = call;
            s_pend[i].kind = kind;
            s_pend[i].gen = s_gen;
            return;
        }
}

static void op_done(int ok, const char *err)
{
    if (!ok) { set_err(err); slog("failed: %s", err); }
    s_op = ok ? OP_OK : OP_FAIL;
}

static void lobby_leave_locked(void)
{
    if (s_lobby) F.leave_lobby(s_mm, s_lobby);
    if (s_peer) F.close_sess(s_nm, &s_ident);
    s_lobby = 0;
    s_peer = 0;
    s_accepted = 0;
    s_req_id = 0;
    s_host = 0;
    s_code[0] = 0;
    s_peer_name[0] = 0;
    s_link = LK_NONE;
}

static void bind_peer(uint64_t id)
{
    const char *n;
    s_peer = id;
    memset(&s_ident, 0, sizeof s_ident);
    F.set_id(&s_ident, id);
    n = F.friend_name(s_fr, id);
    snprintf(s_peer_name, sizeof s_peer_name, "%s", n && *n ? n : "Player");
    F.accept_sess(s_nm, &s_ident);
    s_accepted = 1;
    s_link = LK_UP;
}

static int ver_ok(uint64_t lobby)
{
    const char *v = F.get_data(s_mm, lobby, "avpx_ver");
    return v && !strcmp(v, s_ver);
}

static void on_created(const uint8_t *b, int cub, uint32_t gen)
{
    int32_t res;
    uint64_t lobby;
    if (cub < 12) { op_done(0, "Could not create the lobby"); return; }
    memcpy(&res, b, 4);
    memcpy(&lobby, b + (cub >= 16 ? 8 : 4), 8);
    slog("lobby create result %d", (int)res);
    if (gen != s_gen) {
        if (res == RES_OK && lobby) F.leave_lobby(s_mm, lobby);
        return;
    }
    if (res != RES_OK || !lobby) { op_done(0, "Could not create the lobby"); return; }
    s_lobby = lobby;
    s_host = 1;
    F.set_data(s_mm, lobby, "avpx_code", s_code);
    F.set_data(s_mm, lobby, "avpx_ver", s_ver);
    s_link = LK_LOBBY;
    slog("lobby created, code %s", s_code);
    op_done(1, NULL);
}

static void on_list(const uint8_t *b, int cub, uint32_t gen)
{
    uint32_t n, i;
    if (gen != s_gen) return;
    if (cub < 4) { op_done(0, "Could not search for lobbies"); return; }
    memcpy(&n, b, 4);
    slog("lobby list: %u matching", n);
    if (n > 50) n = 50;
    for (i = 0; i < n; i++) {
        uint64_t l = F.lobby_by_idx(s_mm, (int)i);
        const char *c = l ? F.get_data(s_mm, l, "avpx_code") : NULL;
        if (c && !strcmp(c, s_want) && ver_ok(l)) {
            slog("lobby found, joining");
            pend_add(F.join_lobby(s_mm, l), PK_JOIN);
            return;
        }
    }
    op_done(0, "No lobby with that code");
}

static void on_enter(const uint8_t *b, int cub, uint32_t gen)
{
    uint64_t lobby, owner;
    uint32_t resp;
    if (cub < 20) { op_done(0, "Could not join the lobby"); return; }
    memcpy(&lobby, b, 8);
    memcpy(&resp, b + 16, 4);
    slog("join response %u", resp);
    if (gen != s_gen) {
        if (resp == 1 && lobby) F.leave_lobby(s_mm, lobby);
        return;
    }
    if (resp != 1) {
        op_done(0, resp == 2 ? "No lobby with that code" : resp == 4 ? "Lobby is full" : "Could not join the lobby");
        return;
    }
    owner = F.get_owner(s_mm, lobby);
    {
        int vok = ver_ok(lobby), mem = F.num_members(s_mm, lobby), own = owner == s_me;
        slog("owner lookup: %s, version %s, %d members", !owner ? "none" : own ? "this account" : "other account", vok ? "ok" : "differs", mem);
        if (!owner || own || !vok || mem > LOBBY_MAX) {
            F.leave_lobby(s_mm, lobby);
            op_done(0, own ? "Can't join your own lobby (needs a second Steam account)" : !vok ? "Different game version" : mem > LOBBY_MAX ? "Lobby is full" : "Could not join the lobby");
            return;
        }
    }
    s_lobby = lobby;
    s_host = 0;
    bind_peer(owner);
    op_done(1, NULL);
}

static void on_call_done(const uint8_t *b, int cub)
{
    uint64_t call;
    int32_t cb;
    uint32_t sz;
    uint8_t buf[256], failed = 0;
    int i, kind = 0;
    uint32_t gen = 0;
    if (cub < 16) return;
    memcpy(&call, b, 8);
    memcpy(&cb, b + 8, 4);
    memcpy(&sz, b + 12, 4);
    for (i = 0; i < PEND_MAX; i++)
        if (s_pend[i].kind && s_pend[i].call == call) {
            kind = s_pend[i].kind;
            gen = s_pend[i].gen;
            s_pend[i].kind = 0;
            break;
        }
    if (!kind) return;
    if (sz > sizeof buf || !F.call_res(s_pipe, call, buf, (int)sz, cb, &failed) || failed) {
        if (gen == s_gen) op_done(0, kind == PK_CREATE ? "Could not create the lobby" : kind == PK_LIST ? "Could not search for lobbies" : "Could not join the lobby");
        return;
    }
    if (kind == PK_CREATE && cb == CB_LOBBY_CREATED) on_created(buf, (int)sz, gen);
    else if (kind == PK_LIST && cb == CB_LOBBY_LIST) on_list(buf, (int)sz, gen);
    else if (kind == PK_JOIN && cb == CB_LOBBY_ENTER) on_enter(buf, (int)sz, gen);
}

static void dispatch(const CbMsg *m)
{
    if (!m->param) return;
    switch (m->id) {
    case CB_CALL_DONE:
        on_call_done(m->param, m->cub);
        break;
    case CB_JOIN_REQ:
        if (m->cub >= 8) {
            memcpy(&s_join_lobby, m->param, 8);
            s_join_flag = s_join_lobby != 0;
        }
        break;
    case CB_SESS_REQ:
        if (m->cub >= (int)sizeof(Ident)) {
            Ident id;
            memcpy(&id, m->param, sizeof id);
            s_req_id = F.get_id(&id);
            if (s_peer && s_req_id == s_peer) {
                F.accept_sess(s_nm, &s_ident);
                s_accepted = 1;
                slog("session request accepted");
            } else {
                slog("session request %s", s_peer ? "rejected (not the lobby peer)" : "held until the lobby peer is known");
            }
        }
        break;
    case CB_SESS_FAIL:
        if (m->cub >= (int)sizeof(Ident) && s_peer) {
            Ident id;
            memcpy(&id, m->param, sizeof id);
            if (F.get_id(&id) == s_peer) {
                int32_t code = 0;
                char t[64];
                if (m->cub >= 188) memcpy(&code, m->param + 184, 4);
                snprintf(t, sizeof t, "Steam relay failed (code %d)", (int)code);
                set_err(t);
                slog("session failed, end reason %d", (int)code);
                s_link = LK_DEAD;
            }
        }
        break;
    }
}

static void rx_push(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    EnterCriticalSection(&s_rl);
    if (!s_rx || s_rx_w - s_rx_r + n > RX_CAP) {
        LeaveCriticalSection(&s_rl);
        fprintf(stderr, "[STEAM] receive buffer overflow, dropping the link\n");
        s_link = LK_DEAD;
        return;
    }
    for (i = 0; i < n; i++) s_rx[(s_rx_w + i) & (RX_CAP - 1u)] = p[i];
    s_rx_w += n;
    if (!s_first_rx) { s_first_rx = 1; slog("first message received (%u bytes)", n); }
    LeaveCriticalSection(&s_rl);
    SetEvent(s_ev);
}

static void pump_messages(void)
{
    void *msgs[16];
    int n, i;
    if (!s_peer) {
        n = F.recv_msg(s_nm, CH_COOP, msgs, 16);
        for (i = 0; i < n; i++) F.msg_release(msgs[i]);
        return;
    }
    do {
        n = F.recv_msg(s_nm, CH_COOP, msgs, 16);
        for (i = 0; i < n; i++) {
            uint8_t *m = (uint8_t *)msgs[i];
            void *data;
            int32_t sz;
            memcpy(&data, m, sizeof data);
            memcpy(&sz, m + 8, 4);
            if (s_link == LK_UP && sz > 0 && sz <= RX_MSG_MAX && F.get_id((const Ident *)(m + 16)) == s_peer)
                rx_push((const uint8_t *)data, (uint32_t)sz);
            F.msg_release(m);
        }
    } while (n == 16);
}

static void pump_lobby(void)
{
    int i, n, found = 0;
    uint64_t other = 0;
    if (!s_lobby) return;
    n = F.num_members(s_mm, s_lobby);
    for (i = 0; i < n; i++) {
        uint64_t m = F.member_by_idx(s_mm, s_lobby, i);
        if (s_peer && m == s_peer) found = 1;
        else if (m && m != s_me && !other) other = m;
    }
    if (s_host && s_link == LK_LOBBY && other) {
        bind_peer(other);
        if (F.set_joinable) F.set_joinable(s_mm, s_lobby, 0);
        slog("a player joined the lobby, session accept sent");
        return;
    }
    if (s_link == LK_UP && s_peer && !found) {
        s_link = LK_DEAD;
        set_err("Host left");
        slog("the other player left the lobby");
    }
    if (s_link == LK_UP && s_peer && !s_accepted && s_req_id == s_peer) {
        F.accept_sess(s_nm, &s_ident);
        s_accepted = 1;
        slog("session request accepted");
    }
}

static void friends_refresh(ULONGLONG now)
{
    int n, i, cnt = 0;
    AvpSteamFriend tmp[FR_MAX];
    if (!s_fl_want || now - s_fl_t < 3000) return;
    s_fl_t = now;
    n = F.friend_count(s_fr, FRIEND_FLAG_IMMEDIATE);
    if (n > 600) n = 600;
    for (i = 0; i < n && cnt < FR_MAX; i++) {
        uint64_t sid = F.friend_by_idx(s_fr, i, FRIEND_FLAG_IMMEDIATE), lobby, gid;
        uint64_t info[4];
        const char *nm;
        memset(info, 0, sizeof info);
        if (!sid || !F.game_played(s_fr, sid, info)) continue;
        gid = info[0];
        lobby = info[2];
        if (gid != APPID_NUM || !lobby || lobby == s_lobby) continue;
        if (F.req_data) F.req_data(s_mm, lobby);
        if (!ver_ok(lobby)) continue;
        nm = F.friend_name(s_fr, sid);
        snprintf(tmp[cnt].name, sizeof tmp[cnt].name, "%s", nm && *nm ? nm : "Friend");
        tmp[cnt].lobby = lobby;
        cnt++;
    }
    memcpy(s_fl, tmp, sizeof(AvpSteamFriend) * (size_t)cnt);
    s_fl_n = cnt;
}

static DWORD WINAPI steam_thread(LPVOID arg)
{
    const char *why = "Steam library missing";
    void *lib;
    int res = 0;
    char err[1024];
    (void)arg;
    set_text("Connecting to Steam");
    lib = load_library(&why);
    if (!lib) { fail_init(why); return 0; }
    if (!resolve(lib)) { fail_init("Steam library is not compatible"); return 0; }
    write_appid();
    if (F.is_running && !F.is_running()) { fail_init("Steam is not running"); return 0; }
    err[0] = 0;
    if (F.init_flat) {
        res = F.init_flat(err);
        if (res != 0) {
            fprintf(stderr, "[STEAM] init result %d\n", res);
            fail_init(res == 2 ? "Steam is not running" : "Could not connect to Steam");
            return 0;
        }
    } else if (!F.init_old()) {
        fail_init("Steam is not running");
        return 0;
    }
    F.man_init();
    s_pipe = F.get_pipe();
    s_mm = F.mm_get();
    s_fr = F.fr_get();
    s_usr = F.usr_get();
    s_nm = F.nm_get();
    if (!s_pipe || !s_mm || !s_fr || !s_usr || !s_nm) { fail_init("Could not connect to Steam"); return 0; }
    if (F.ut_get && F.init_relay) {
        void *ut = F.ut_get();
        if (ut) F.init_relay(ut);
    }
    s_me = F.my_id(s_usr);
    {
        const char *n = F.my_name(s_fr);
        snprintf(s_my_name, sizeof s_my_name, "%s", n && *n ? n : "Player");
    }
    snprintf(s_ver, sizeof s_ver, "%d", avp_coop_version());
    EnterCriticalSection(&s_rl);
    if (!s_rx) s_rx = (uint8_t *)malloc(RX_CAP);
    LeaveCriticalSection(&s_rl);
    set_text("Steam connected");
    s_status = AVP_STEAM_READY;
    fprintf(stderr, "[STEAM] connected\n");
    while (!s_quit) {
        CbMsg m;
        ULONGLONG now = GetTickCount64();
        EnterCriticalSection(&s_sl);
        F.run_frame(s_pipe);
        while (F.next_cb(s_pipe, &m)) {
            dispatch(&m);
            F.free_cb(s_pipe);
        }
        pump_lobby();
        pump_messages();
        friends_refresh(now);
        LeaveCriticalSection(&s_sl);
        Sleep(10);
    }
    return 0;
}

int avp_steam_connect(void)
{
    locks_init();
    if (s_status == AVP_STEAM_BUSY || s_status == AVP_STEAM_READY) return 1;
    if (s_thread) {
        WaitForSingleObject(s_thread, 2000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    s_status = AVP_STEAM_BUSY;
    s_thread = CreateThread(NULL, 0, steam_thread, NULL, 0, NULL);
    if (!s_thread) { fail_init("Could not start Steam"); return 0; }
    return 1;
}

int avp_steam_status(void)
{
    return s_status;
}

const char *avp_steam_text(void)
{
    static char t[sizeof s_text];
    if (!s_locks_ready) return "";
    EnterCriticalSection(&s_sl);
    memcpy(t, s_text, sizeof t);
    LeaveCriticalSection(&s_sl);
    return t;
}

const char *avp_steam_error(void)
{
    static char t[sizeof s_err];
    if (!s_locks_ready) return "";
    EnterCriticalSection(&s_sl);
    memcpy(t, s_err, sizeof t);
    LeaveCriticalSection(&s_sl);
    return t;
}

const char *avp_steam_peer_name(void)
{
    static char t[sizeof s_peer_name];
    if (!s_locks_ready) return "";
    EnterCriticalSection(&s_sl);
    memcpy(t, s_peer_name, sizeof t);
    LeaveCriticalSection(&s_sl);
    return t;
}

const char *avp_steam_code(void)
{
    static char t[sizeof s_code];
    if (!s_locks_ready) return "";
    EnterCriticalSection(&s_sl);
    memcpy(t, s_code, sizeof t);
    LeaveCriticalSection(&s_sl);
    return t;
}

static int op_wait(volatile int *cancel, int ms)
{
    int t;
    for (t = 0; t < ms / 10; t++) {
        if (s_op == OP_OK) return 1;
        if (s_op == OP_FAIL) return 0;
        if ((cancel && *cancel) || s_status != AVP_STEAM_READY) { set_err("Cancelled"); return 0; }
        Sleep(10);
    }
    set_err("Steam did not answer in time");
    return 0;
}

static int op_begin(void)
{
    if (s_status != AVP_STEAM_READY) { set_err("Steam is not connected"); return 0; }
    EnterCriticalSection(&s_sl);
    lobby_leave_locked();
    s_gen++;
    s_first_tx = s_first_rx = 0;
    s_op = OP_RUN;
    s_err[0] = 0;
    EnterCriticalSection(&s_rl);
    s_rx_r = s_rx_w = 0;
    LeaveCriticalSection(&s_rl);
    return 1;
}

int avp_steam_host(volatile int *cancel, char *code, int cap)
{
    if (s_status != AVP_STEAM_READY) { set_err("Steam is not connected"); return 0; }
    if (!op_begin()) return 0;
    if (!make_code(s_code)) {
        LeaveCriticalSection(&s_sl);
        set_err("Could not make a code");
        s_op = OP_FAIL;
        return 0;
    }
    slog("creating a lobby");
    pend_add(F.create_lobby(s_mm, LOBBY_TYPE_PUBLIC, LOBBY_MAX), PK_CREATE);
    snprintf(code, (size_t)cap, "%s", s_code);
    LeaveCriticalSection(&s_sl);
    return op_wait(cancel, 20000);
}

int avp_steam_join_code(const char *code, volatile int *cancel)
{
    if (!avp_steam_valid_code(code)) { set_err("Enter the 6 character code"); return 0; }
    if (!op_begin()) return 0;
    snprintf(s_want, sizeof s_want, "%s", code);
    F.str_filter(s_mm, "avpx_code", s_want, 0);
    F.str_filter(s_mm, "avpx_ver", s_ver, 0);
    if (F.dist_filter) F.dist_filter(s_mm, 3);
    if (F.slots_filter) F.slots_filter(s_mm, 1);
    if (F.count_filter) F.count_filter(s_mm, 20);
    slog("requesting the lobby list");
    pend_add(F.request_list(s_mm), PK_LIST);
    LeaveCriticalSection(&s_sl);
    return op_wait(cancel, 30000);
}

int avp_steam_join_lobby(uint64_t lobby, volatile int *cancel)
{
    if (!lobby) { set_err("Could not join the lobby"); return 0; }
    if (!op_begin()) return 0;
    slog("joining a lobby");
    pend_add(F.join_lobby(s_mm, lobby), PK_JOIN);
    LeaveCriticalSection(&s_sl);
    return op_wait(cancel, 30000);
}

int avp_steam_wait_peer(volatile int *cancel)
{
    while (!(cancel && *cancel)) {
        if (s_link == LK_UP) return 1;
        if (s_link == LK_DEAD || s_link == LK_NONE || s_status != AVP_STEAM_READY) break;
        Sleep(20);
    }
    set_err("The lobby closed");
    return 0;
}

int avp_steam_link_up(void)
{
    return s_link == LK_UP;
}

void avp_steam_link_drop(void)
{
    if (s_link != LK_NONE) s_link = LK_DEAD;
    if (s_locks_ready) SetEvent(s_ev);
}

void avp_steam_leave(void)
{
    if (!s_locks_ready) return;
    if (s_status == AVP_STEAM_READY) {
        EnterCriticalSection(&s_sl);
        lobby_leave_locked();
        s_gen++;
        s_op = OP_IDLE;
        LeaveCriticalSection(&s_sl);
    } else {
        s_link = LK_NONE;
    }
    EnterCriticalSection(&s_rl);
    s_rx_r = s_rx_w = 0;
    LeaveCriticalSection(&s_rl);
    SetEvent(s_ev);
}

int avp_steam_send(const void *head, uint32_t hn, const void *body, uint32_t bn)
{
    uint32_t total = hn + bn, off = 0;
    int ok = 1;
    if (s_link != LK_UP) return 0;
    EnterCriticalSection(&s_txl);
    if (!s_first_tx) { s_first_tx = 1; slog("first message sent (%u bytes)", total); }
    if (total > s_fb_cap) {
        uint8_t *nb = (uint8_t *)realloc(s_fb, total);
        if (!nb) { LeaveCriticalSection(&s_txl); return 0; }
        s_fb = nb;
        s_fb_cap = total;
    }
    memcpy(s_fb, head, hn);
    if (bn) memcpy(s_fb + hn, body, bn);
    while (off < total && ok) {
        uint32_t n = total - off > CHUNK ? CHUNK : total - off;
        int tries, r = 0;
        for (tries = 0; tries < 400; tries++) {
            EnterCriticalSection(&s_sl);
            if (s_link != LK_UP) { LeaveCriticalSection(&s_sl); r = -1; break; }
            r = F.send_msg(s_nm, &s_ident, s_fb + off, n, SEND_RELIABLE, CH_COOP);
            LeaveCriticalSection(&s_sl);
            if (r != RES_LIMIT) break;
            Sleep(5);
        }
        if (r != RES_OK) { ok = 0; slog("send failed, result %d", r); }
        off += n;
    }
    LeaveCriticalSection(&s_txl);
    return ok;
}

int avp_steam_recv(void *p, int n)
{
    uint8_t *d = (uint8_t *)p;
    int i;
    for (;;) {
        EnterCriticalSection(&s_rl);
        if (s_rx && s_rx_w - s_rx_r >= (uint64_t)n) {
            for (i = 0; i < n; i++) d[i] = s_rx[(s_rx_r + (uint64_t)i) & (RX_CAP - 1u)];
            s_rx_r += (uint64_t)n;
            LeaveCriticalSection(&s_rl);
            return 1;
        }
        LeaveCriticalSection(&s_rl);
        if (s_link != LK_UP) return 0;
        WaitForSingleObject(s_ev, 20);
    }
}

void avp_steam_want_friends(int on)
{
    s_fl_want = on;
    if (!on) s_fl_t = 0;
}

int avp_steam_friends(AvpSteamFriend *out, int max)
{
    int n;
    if (!s_locks_ready || s_status != AVP_STEAM_READY) return 0;
    EnterCriticalSection(&s_sl);
    n = s_fl_n < max ? s_fl_n : max;
    memcpy(out, s_fl, sizeof(AvpSteamFriend) * (size_t)n);
    LeaveCriticalSection(&s_sl);
    return n;
}

void avp_steam_scan_cmdline(void)
{
    char buf[2048];
    char *p;
    if (s_cmd_done) return;
    s_cmd_done = 1;
    buf[0] = 0;
#ifdef _WIN32
    snprintf(buf, sizeof buf, "%s", GetCommandLineA());
#else
    {
        FILE *f = fopen("/proc/self/cmdline", "rb");
        size_t n, i;
        if (!f) return;
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        for (i = 0; i < n; i++)
            if (!buf[i]) buf[i] = ' ';
        buf[n] = 0;
    }
#endif
    p = strstr(buf, "+connect_lobby");
    if (!p) return;
    p += 14;
    while (*p == ' ' || *p == '=') p++;
    s_cmd_lobby = strtoull(p, NULL, 10);
    s_cmd_flag = s_cmd_lobby != 0;
}

int avp_steam_cmd_pending(void)
{
    return s_cmd_flag;
}

int avp_steam_take_join(uint64_t *lobby)
{
    if (s_join_flag) {
        s_join_flag = 0;
        *lobby = s_join_lobby;
        return 1;
    }
    if (s_cmd_flag && s_status == AVP_STEAM_READY) {
        s_cmd_flag = 0;
        *lobby = s_cmd_lobby;
        return 1;
    }
    return 0;
}
