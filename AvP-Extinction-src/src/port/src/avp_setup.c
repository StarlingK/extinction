#include <windows.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SEC 2048
#define XBE_SIZE 2633728u

static const char k_xbe_sha256[] =
    "499C2277D4E00CC45402043FB6AE95AAE409BDAAEF291808187D0ABF4CB2E97D";

static const int64_t k_parts[] = { 0, 0x18300000LL, 0x0FD90000LL, 0x02080000LL };

typedef struct {
    char path[MAX_PATH];
    uint32_t sector, size;
    int dir;
} Entry;

static Entry *s_ent;
static int s_n, s_cap;
static HANDLE s_iso = INVALID_HANDLE_VALUE;
static int64_t s_part;

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void strip_last(char *p)
{
    char *s = strrchr(p, '\\');
    if (s) *s = 0;
}

static void remove_tree(const char *dir)
{
    char pat[MAX_PATH * 2], sub[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(sub);
            else {
                SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(sub);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static int sha256_hex(const void *data, size_t n, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char d[32];
    int ok = 0, i;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) >= 0
        && BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0) >= 0
        && BCryptFinishHash(h, d, sizeof d, 0) >= 0) {
        for (i = 0; i < 32; i++)
            sprintf(out + i * 2, "%02X", d[i]);
        ok = 1;
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static void *read_whole(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    void *b;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = sz > 0 ? malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) *n = (size_t)sz;
    return b;
}

static int xbe_ok(const void *data, size_t n)
{
    char hex[65];
    return n == XBE_SIZE && sha256_hex(data, n, hex) && !strcmp(hex, k_xbe_sha256);
}

static uint32_t xbe_title_id(const uint8_t *b, size_t n)
{
    uint32_t base, cert;
    if (n < 0x178 || memcmp(b, "XBEH", 4)) return 0;
    memcpy(&base, b + 0x104, 4);
    memcpy(&cert, b + 0x118, 4);
    cert -= base;
    if (cert + 12 > n) return 0;
    memcpy(&base, b + cert + 8, 4);
    return base;
}

static int folder_ok(const char *base)
{
    char p[MAX_PATH * 2];
    size_t n = 0;
    void *b;
    int ok;

    snprintf(p, sizeof p, "%s\\game\\default.xbe", base);
    if (!file_exists(p)) return -1;
    b = read_whole(p, &n);
    ok = b && xbe_ok(b, n);
    free(b);
    return ok;
}

static int iso_read(int64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = off;
    return SetFilePointerEx(s_iso, li, NULL, FILE_BEGIN)
        && ReadFile(s_iso, buf, n, &got, NULL) && got == n;
}

static int name_safe(const char *s)
{
    if (!*s || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (; *s; s++)
        if (*s == '\\' || *s == '/' || *s == ':' || (unsigned char)*s < 0x20)
            return 0;
    return 1;
}

static int walk(uint32_t sector, uint32_t size, const char *prefix, int depth);

static int walk_node(const uint8_t *t, uint32_t size, uint32_t off,
                     const char *prefix, int depth, int budget)
{
    uint16_t left, right;
    uint32_t sec, sz;
    uint8_t attr, len;
    char name[256];
    Entry *e;

    if (budget <= 0 || off + 14 > size) return 1;
    memcpy(&left, t + off, 2);
    memcpy(&right, t + off + 2, 2);
    if (left == 0xFFFF && right == 0xFFFF) return 1;
    memcpy(&sec, t + off + 4, 4);
    memcpy(&sz, t + off + 8, 4);
    attr = t[off + 12];
    len = t[off + 13];
    if (off + 14 + len > size) return 1;
    memcpy(name, t + off + 14, len);
    name[len] = 0;

    if (left && !walk_node(t, size, left * 4u, prefix, depth, budget - 1))
        return 0;

    if (name_safe(name) && !(!*prefix && !_stricmp(name, "cerbera.tvr"))) {
        if (s_n == s_cap) {
            s_cap = s_cap ? s_cap * 2 : 256;
            s_ent = realloc(s_ent, s_cap * sizeof *s_ent);
            if (!s_ent) return 0;
        }
        e = &s_ent[s_n++];
        snprintf(e->path, sizeof e->path, "%s%s%s", prefix, *prefix ? "\\" : "", name);
        e->sector = sec;
        e->size = sz;
        e->dir = (attr & 0x10) != 0;
        if (e->dir && sz) {
            char sub[MAX_PATH];
            snprintf(sub, sizeof sub, "%s", e->path);
            if (!walk(sec, sz, sub, depth + 1))
                return 0;
        }
    }

    if (right && !walk_node(t, size, right * 4u, prefix, depth, budget - 1))
        return 0;
    return 1;
}

static int walk(uint32_t sector, uint32_t size, const char *prefix, int depth)
{
    uint32_t n = (size + SEC - 1) / SEC * SEC;
    uint8_t *t;
    int ok;

    if (depth > 16 || n > 16u << 20) return 0;
    t = malloc(n);
    if (!t) return 0;
    ok = iso_read(s_part + (int64_t)sector * SEC, t, n)
      && walk_node(t, size, 0, prefix, depth, 4096);
    free(t);
    return ok;
}

static int iso_open(const char *path, const char **why)
{
    uint8_t vd[SEC];
    uint32_t root_sec = 0, root_size = 0;
    int i;

    s_n = 0;
    s_iso = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (s_iso == INVALID_HANDLE_VALUE) {
        *why = "The file could not be opened.";
        return -1;
    }
    for (i = 0; i < (int)(sizeof k_parts / sizeof k_parts[0]); i++) {
        if (iso_read(k_parts[i] + 32 * SEC, vd, SEC)
            && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) {
            s_part = k_parts[i];
            memcpy(&root_sec, vd + 20, 4);
            memcpy(&root_size, vd + 24, 4);
            break;
        }
    }
    if (!root_size) {
        *why = "This is not an Xbox disc image (no XDVDFS volume found).";
        return -1;
    }
    if (!walk(root_sec, root_size, "", 0)) {
        *why = "The disc image could not be read (its file table is damaged).";
        return -1;
    }
    for (i = 0; i < s_n; i++)
        if (!s_ent[i].dir && !_stricmp(s_ent[i].path, "default.xbe"))
            return i;
    *why = "The disc image has no default.xbe.";
    return -1;
}

static void iso_close(void)
{
    if (s_iso != INVALID_HANDLE_VALUE) CloseHandle(s_iso);
    s_iso = INVALID_HANDLE_VALUE;
}

static HWND s_wnd;
static volatile int s_cancel;
static char s_status[512];
static double s_frac;

static LRESULT CALLBACK prog_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, bar, fill;
        HBRUSH bg = CreateSolidBrush(RGB(10, 18, 34));
        HBRUSH edge = CreateSolidBrush(RGB(60, 190, 220));
        HBRUSH in = CreateSolidBrush(RGB(30, 120, 160));
        GetClientRect(h, &rc);
        FillRect(dc, &rc, bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 235, 245));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        { RECT t = { 16, 14, rc.right - 16, 60 };
          DrawTextA(dc, s_status, -1, &t, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS); }
        SetRect(&bar, 16, rc.bottom - 34, rc.right - 16, rc.bottom - 16);
        FrameRect(dc, &bar, edge);
        fill = bar;
        InflateRect(&fill, -3, -3);
        fill.right = fill.left + (LONG)((fill.right - fill.left) * s_frac);
        FillRect(dc, &fill, in);
        DeleteObject(bg); DeleteObject(edge); DeleteObject(in);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (MessageBoxA(h, "Stop copying the game files?", "Aliens versus Predator: Extinction",
                        MB_YESNO | MB_ICONQUESTION) == IDYES)
            s_cancel = 1;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void prog_open(void)
{
    WNDCLASSA wc = {0};
    RECT r = { 0, 0, 460, 110 };
    wc.lpfnWndProc = prog_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "AvpSetupProgress";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    s_wnd = CreateWindowA(wc.lpszClassName, "Setting up the game files",
                          WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                          (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                          (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2,
                          r.right - r.left, r.bottom - r.top,
                          NULL, NULL, wc.hInstance, NULL);
}

static void prog_update(const char *status, double frac)
{
    static DWORD last;
    MSG msg;
    DWORD now = GetTickCount();

    if (status) snprintf(s_status, sizeof s_status, "%s", status);
    s_frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    if (status || now - last > 50) {
        last = now;
        if (s_wnd) InvalidateRect(s_wnd, NULL, FALSE);
    }
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void prog_close(void)
{
    if (s_wnd) DestroyWindow(s_wnd);
    s_wnd = NULL;
}

static int extract_all(const char *dest, const char **why)
{
    uint64_t total = 0, done = 0;
    char p[MAX_PATH * 2], msg[512];
    uint8_t *buf;
    int i;

    for (i = 0; i < s_n; i++)
        if (!s_ent[i].dir) total += s_ent[i].size;

    { ULARGE_INTEGER freeb;
      char root[MAX_PATH * 2];
      snprintf(root, sizeof root, "%s\\", dest);
      strip_last(root); strip_last(root); strcat(root, "\\");
      if (GetDiskFreeSpaceExA(root, &freeb, NULL, NULL)
          && freeb.QuadPart < total + (64u << 20)) {
          snprintf(msg, sizeof msg, "Not enough free space: the game files need %.0f MB.",
                   total / 1048576.0 + 64);
          MessageBoxA(NULL, msg, "Aliens versus Predator: Extinction", MB_ICONERROR);
          *why = NULL;
          return 0;
      } }

    if (!CreateDirectoryA(dest, NULL)) {
        *why = "The game folder could not be created.";
        return 0;
    }
    buf = malloc(1u << 20);
    if (!buf) { *why = "Out of memory."; return 0; }

    prog_open();
    for (i = 0; i < s_n && !s_cancel; i++) {
        Entry *e = &s_ent[i];
        snprintf(p, sizeof p, "%s\\%s", dest, e->path);
        if (e->dir) {
            CreateDirectoryA(p, NULL);
            continue;
        }
        {
            HANDLE out = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, NULL);
            uint32_t left = e->size;
            int64_t off = s_part + (int64_t)e->sector * SEC;
            if (out == INVALID_HANDLE_VALUE) {
                *why = "A game file could not be written.";
                break;
            }
            snprintf(msg, sizeof msg, "Copying the game files from the disc image (%d of %d)\n%s",
                     i + 1, s_n, e->path);
            prog_update(msg, total ? (double)done / total : 0);
            while (left && !s_cancel) {
                DWORD n = left > (1u << 20) ? (1u << 20) : left, put = 0;
                if (!iso_read(off, buf, n)) { *why = "The disc image could not be read."; break; }
                if (!WriteFile(out, buf, n, &put, NULL) || put != n) {
                    *why = "A game file could not be written (is the disk full?).";
                    break;
                }
                left -= n; off += n; done += n;
                prog_update(NULL, total ? (double)done / total : 0);
            }
            CloseHandle(out);
            if (left) break;
        }
    }
    free(buf);
    prog_close();
    if (s_cancel) *why = NULL;
    return i == s_n && !s_cancel;
}

static int pick_iso(char *out, size_t n)
{
    OPENFILENAMEA ofn = {0};
    out[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "Xbox disc image (*.iso, *.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = (DWORD)n;
    ofn.lpstrTitle = "Choose your Aliens versus Predator: Extinction (USA) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) != 0;
}

static const char *cmdline_iso(void)
{
    static char buf[MAX_PATH * 2];
    const char *c = GetCommandLineA(), *s = strstr(c, "--iso");
    size_t i = 0;

    if (!s) return NULL;
    s += 5;
    while (*s == ' ' || *s == '=') s++;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && i < sizeof buf - 1) buf[i++] = *s++;
    } else {
        while (*s && *s != ' ' && i < sizeof buf - 1) buf[i++] = *s++;
    }
    buf[i] = 0;
    return i ? buf : NULL;
}

static int run_setup(const char *base)
{
    static const char title[] = "Aliens versus Predator: Extinction";
    char iso[MAX_PATH * 2], part[MAX_PATH * 2], game[MAX_PATH * 2], msg[1024];
    const char *given = getenv("AVP_ISO");
    int first = 1;

    if (!given) given = cmdline_iso();
    snprintf(part, sizeof part, "%s\\game.part", base);
    snprintf(game, sizeof game, "%s\\game", base);

    if (!given && MessageBoxA(NULL,
            "The game files are not set up yet.\n\n"
            "This port needs your own copy of Aliens versus Predator: Extinction "
            "(USA, Xbox) as a disc image (.iso). It copies the game files out of it "
            "(about 400 MB) into a folder called game next to the program. "
            "Nothing is downloaded.\n\nChoose the disc image now?",
            title, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return 0;

    for (;;) {
        const char *why = NULL;
        int xi;

        if (given && first)
            snprintf(iso, sizeof iso, "%s", given);
        else if (!pick_iso(iso, sizeof iso))
            return 0;
        first = 0;
        fprintf(stderr, "[SETUP] disc image %s\n", iso);

        xi = iso_open(iso, &why);
        if (xi >= 0) {
            Entry *e = &s_ent[xi];
            uint8_t *x = e->size <= (16u << 20) ? malloc(e->size) : NULL;
            if (!x || !iso_read(s_part + (int64_t)e->sector * SEC, x, e->size))
                why = "The disc image could not be read.";
            else if (!xbe_ok(x, e->size)) {
                uint32_t tid = xbe_title_id(x, e->size);
                snprintf(msg, sizeof msg,
                    "This disc image is not the release this port was built from.\n\n"
                    "It needs Aliens versus Predator: Extinction, USA release. "
                    "This image's title ID is %08X%s.",
                    tid, tid ? "" : " (not an XBE)");
                why = msg;
            }
            free(x);
        }
        if (why) {
            iso_close();
            fprintf(stderr, "[SETUP] rejected: %s\n", why);
            if (MessageBoxA(NULL, why, title, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
                return 0;
            continue;
        }

        fprintf(stderr, "[SETUP] partition 0x%llX, %d entries, copying to %s\n",
                (long long)s_part, s_n, game);
        if (dir_exists(part)) remove_tree(part);
        s_cancel = 0;
        {
            DWORD t0 = GetTickCount();
            int ok = extract_all(part, &why);
            iso_close();
            if (ok && dir_exists(game)) {

                char old[MAX_PATH * 2];
                snprintf(old, sizeof old, "%s\\game.old", base);
                if (dir_exists(old)) remove_tree(old);
                MoveFileA(game, old);
            }
            if (ok && !MoveFileA(part, game)) {
                ok = 0;
                why = "The game folder could not be put in place.";
            }
            if (!ok) {
                remove_tree(part);
                fprintf(stderr, "[SETUP] copy failed: %s\n", why ? why : "cancelled");
                if (!why) return 0;
                if (MessageBoxA(NULL, why, title, MB_RETRYCANCEL | MB_ICONERROR) != IDRETRY)
                    return 0;
                continue;
            }
            fprintf(stderr, "[SETUP] copied %d entries in %lu ms\n", s_n,
                    (unsigned long)(GetTickCount() - t0));
        }
        free(s_ent);
        s_ent = NULL;
        s_n = s_cap = 0;
        return 1;
    }
}

int avp_setup_locate(char *base, size_t n)
{
    char exe[MAX_PATH], cand[4][MAX_PATH];
    const char *home = getenv("AVP_HOME");
    int i, nc = 0, mismatch = -1;

    GetModuleFileNameA(NULL, exe, sizeof exe);
    strip_last(exe);
    if (home && *home) {
        snprintf(cand[nc++], MAX_PATH, "%s", home);
    } else {
        GetCurrentDirectoryA(MAX_PATH, cand[nc++]);
        snprintf(cand[nc++], MAX_PATH, "%s", exe);
        snprintf(cand[nc], MAX_PATH, "%s", exe);
        strip_last(cand[nc]);
        nc++;
    }
    for (i = 0; i < nc; i++) {
        int r = folder_ok(cand[i]);
        if (r == 1) {
            snprintf(base, n, "%s", cand[i]);
            SetCurrentDirectoryA(base);
            fprintf(stderr, "[SETUP] game files in %s\\game\n", base);
            return 1;
        }
        if (r == 0 && mismatch < 0) mismatch = i;
    }

    snprintf(base, n, "%s", home && *home ? home : exe);
    if (mismatch >= 0) {
        char msg[MAX_PATH * 3];
        snprintf(msg, sizeof msg,
                 "%s\\game\\default.xbe is not from the release this port was built "
                 "from (Aliens versus Predator: Extinction, USA).\n\n"
                 "Set the game files up again from your disc image?", cand[mismatch]);
        if (MessageBoxA(NULL, msg, "Aliens versus Predator: Extinction",
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return 0;
        snprintf(base, n, "%s", cand[mismatch]);
    }
    if (!dir_exists(base)) CreateDirectoryA(base, NULL);
    if (!run_setup(base)) return 0;
    if (folder_ok(base) != 1) {
        MessageBoxA(NULL, "The game files were copied but default.xbe does not check out.",
                    "Aliens versus Predator: Extinction", MB_ICONERROR);
        return 0;
    }
    SetCurrentDirectoryA(base);
    return 1;
}

void avp_setup_save_dir(const char *base, char *out, size_t n)
{
    const char *v = getenv("AVP_SAVE_DIR");
    char legacy[MAX_PATH * 2];

    if (v && *v) {
        snprintf(out, n, "%s", v);
        return;
    }
    snprintf(out, n, "%s\\userdata", base);
    snprintf(legacy, sizeof legacy, "%s\\..\\hdd", base);
    if (!dir_exists(out) && dir_exists(legacy)) {
        if (!GetFullPathNameA(legacy, (DWORD)n, out, NULL))
            snprintf(out, n, "%s", legacy);
        return;
    }
    CreateDirectoryA(out, NULL);
}

