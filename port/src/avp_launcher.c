#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBE_SIZE 2633728u

enum { ID_SETUP = 101, ID_PLAY, ID_STATUS, ID_IN_MOUSE, ID_IN_PAD };

static HWND s_status, s_play, s_in_mouse, s_in_pad;
static char s_dir[MAX_PATH];
static HFONT s_font, s_font_head;
static HWND s_main;
static int s_dpi = 96;
#define S(v) MulDiv((v), s_dpi, 96)

static const char *const k_env[][2] = {
    { "RECOMP_FORCE_RETURN", "1" },
    { "RECOMP_PB_EXEC", "1" },
    { "RECOMP_VBLANK", "paced" },
    { "RECOMP_AC97_READY", "1" },
    { "RECOMP_TITLE_KEVENTS", "1" },
    { "RECOMP_USB", "1" },
    { "RECOMP_KEYBOARD", "1" },
    { "RECOMP_PB_WALK", "dma" },
    { "RECOMP_FB_WINDOW", "1" },
    { "RECOMP_GPU", "1" },
};

void avp_launcher_env(void)
{
    char exe[MAX_PATH], *slash, kv[160];
    size_t i;
    for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++) {
        if (getenv(k_env[i][0])) continue;
        snprintf(kv, sizeof kv, "%s=%s", k_env[i][0], k_env[i][1]);
        _putenv(kv);
    }
    if (GetModuleFileNameA(NULL, exe, sizeof exe) > 0 && (slash = strrchr(exe, '\\')) != NULL) {
        *slash = 0;
        SetCurrentDirectoryA(exe);
    }
}

static int game_exe(char *out, size_t n)
{
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    return k > 0 && k < n;
}

static int game_files_ok(void)
{
    char p[MAX_PATH * 2];
    WIN32_FILE_ATTRIBUTE_DATA a;
    snprintf(p, sizeof p, "%s\\game\\default.xbe", s_dir);
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a))
        return 0;
    return a.nFileSizeHigh == 0 && a.nFileSizeLow == XBE_SIZE;
}

static void icon_c565(unsigned c, unsigned char *o)
{
    o[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
    o[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
    o[2] = (unsigned char)((c & 31) * 255 / 31);
}

HICON avp_game_icon(void)
{
    static HICON s_icon;
    static int s_tried;
    char p[MAX_PATH * 2], dir[MAX_PATH], *slash;
    unsigned char *buf, *rgba;
    FILE *f;
    long n;
    unsigned hs, fmt, w, h, bx, by, i, o;
    BITMAPV5HEADER bi;
    void *bits = NULL;
    HDC dc;
    HBITMAP color, mask;
    ICONINFO ii;
    if (s_icon || s_tried) return s_icon;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    snprintf(p, sizeof p, "%s\\game\\default.xbe", dir);
    f = fopen(p, "rb");
    if (!f) return NULL;
    s_tried = 1;
    buf = (unsigned char *)malloc(0x2800);
    if (!buf) { fclose(f); return NULL; }
    n = fseek(f, 0x27E000, SEEK_SET) ? 0 : (long)fread(buf, 1, 0x2800, f);
    fclose(f);
    if (n != 0x2800 || memcmp(buf, "XPR0", 4)) { free(buf); return NULL; }
    memcpy(&hs, buf + 8, 4);
    memcpy(&fmt, buf + 24, 4);
    w = 1u << ((fmt >> 20) & 15);
    h = 1u << ((fmt >> 24) & 15);
    if (((fmt >> 8) & 0xFF) != 0x0C || w != 128 || h != 128 || hs + w * h / 2 > 0x2800) { free(buf); return NULL; }
    rgba = (unsigned char *)calloc(w * h, 4);
    if (!rgba) { free(buf); return NULL; }
    for (o = hs, by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4, o += 8) {
            unsigned c0 = buf[o] | buf[o + 1] << 8, c1 = buf[o + 2] | buf[o + 3] << 8;
            unsigned sel = buf[o + 4] | buf[o + 5] << 8 | buf[o + 6] << 16 | (unsigned)buf[o + 7] << 24;
            unsigned char c[4][4];
            int k;
            icon_c565(c0, c[0]);
            icon_c565(c1, c[1]);
            for (k = 0; k < 3; k++) {
                if (c0 > c1) {
                    c[2][k] = (unsigned char)((2 * c[0][k] + c[1][k]) / 3);
                    c[3][k] = (unsigned char)((c[0][k] + 2 * c[1][k]) / 3);
                } else {
                    c[2][k] = (unsigned char)((c[0][k] + c[1][k]) / 2);
                    c[3][k] = 0;
                }
            }
            c[0][3] = c[1][3] = c[2][3] = 255;
            c[3][3] = c0 > c1 ? 255 : 0;
            for (i = 0; i < 16; i++) {
                unsigned char *d = rgba + ((by + i / 4) * w + bx + i % 4) * 4, *s = c[(sel >> (2 * i)) & 3];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
            }
        }
    free(buf);
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = (LONG)w;
    bi.bV5Height = -(LONG)h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) { free(rgba); return NULL; }
    memcpy(bits, rgba, w * h * 4);
    free(rgba);
    mask = CreateBitmap((int)w, (int)h, 1, 1, NULL);
    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    s_icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return s_icon;
}
static void refresh(void)
{
    char exe[MAX_PATH * 2];
    int has_exe = game_exe(exe, sizeof exe), ok = has_exe && game_files_ok();
    SetWindowTextA(s_status,
                   !has_exe ? "The game program is missing from this folder."
                   : ok ? "Game files found. Ready to play."
                        : "No game files yet. Set them up from your own Aliens versus Predator: Extinction "
                          "(Xbox, USA) disc image. Nothing from the game is included with this port.");
    EnableWindow(s_play, ok);
    if (ok && s_main && avp_game_icon()) {
        SendMessageA(s_main, WM_SETICON, ICON_BIG, (LPARAM)avp_game_icon());
        SendMessageA(s_main, WM_SETICON, ICON_SMALL, (LPARAM)avp_game_icon());
    }
}

static void ini_path(char *out, size_t n)
{
    snprintf(out, n, "%s\\avp_settings.ini", s_dir);
}

static int ini_get(const char *key, char *out, size_t n)
{
    char p[MAX_PATH * 2], line[256];
    size_t kl = strlen(key);
    FILE *f;
    int found = 0;
    ini_path(p, sizeof p);
    f = fopen(p, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out, n, "%s", line + kl + 1);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

static void ini_set(const char *const *keys, const char *const *vals, int n)
{
    char p[MAX_PATH * 2];
    char *old = NULL;
    long len = 0;
    int done[4] = { 0 }, i;
    FILE *f;
    ini_path(p, sizeof p);
    f = fopen(p, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        old = malloc((size_t)len + 1);
        if (old) { len = (long)fread(old, 1, (size_t)len, f); old[len] = 0; }
        fclose(f);
    }
    f = fopen(p, "wb");
    if (!f) { free(old); return; }
    if (old) {
        char *cur = old;
        while (*cur) {
            char *nl = strchr(cur, '\n');
            size_t ll = nl ? (size_t)(nl - cur) + 1 : strlen(cur);
            int hit = -1;
            for (i = 0; i < n; i++) {
                size_t kl = strlen(keys[i]);
                if (!strncmp(cur, keys[i], kl) && cur[kl] == '=') hit = i;
            }
            if (hit >= 0) {
                if (!done[hit]) fprintf(f, "%s=%s\r\n", keys[hit], vals[hit]);
                done[hit] = 1;
            } else {
                fwrite(cur, 1, ll, f);
                if (!nl) fwrite("\r\n", 1, 2, f);
            }
            cur += ll;
        }
    }
    for (i = 0; i < n; i++)
        if (!done[i]) fprintf(f, "%s=%s\r\n", keys[i], vals[i]);
    fclose(f);
    free(old);
}

static void save_prefs(void)
{
    const char *keys[1] = { "input" };
    const char *vals[1];
    vals[0] = SendMessageA(s_in_pad, BM_GETCHECK, 0, 0) == BST_CHECKED ? "controller" : "mouse";
    ini_set(keys, vals, 1);
}

static void launch(const char *iso)
{
    char exe[MAX_PATH * 2], cmd[MAX_PATH * 6];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    size_t i;

    save_prefs();
    if (!game_exe(exe, sizeof exe)) {
        MessageBoxA(NULL, "The game program is missing from this folder.", "AvP: Extinction", MB_ICONERROR);
        return;
    }
    for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
        SetEnvironmentVariableA(k_env[i][0], k_env[i][1]);
    if (iso)
        snprintf(cmd, sizeof cmd, "\"%s\" --play --iso \"%s\"", exe, iso);
    else
        snprintf(cmd, sizeof cmd, "\"%s\" --play", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, s_dir, &si, &pi)) {
        MessageBoxA(NULL, "The game could not be started.", "AvP: Extinction", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (s_main) DestroyWindow(s_main);
}

static void pick_image(HWND h)
{
    char path[MAX_PATH * 2] = "";
    OPENFILENAMEA o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = h;
    o.lpstrFilter = "Disc images (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    o.lpstrFile = path;
    o.nMaxFile = sizeof path;
    o.lpstrTitle = "Select your Aliens versus Predator: Extinction disc image";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&o))
        launch(path);
}

static HWND add(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int hh, int id)
{
    HWND c = CreateWindowExA(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(hh), parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static LRESULT CALLBACK wnd(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        char v[96];
        int pad = ini_get("input", v, sizeof v) && !strcmp(v, "controller");
        s_main = h;
        s_font = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        s_font_head = CreateFontA(-S(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        SendMessageA(add(h, "STATIC", "Aliens versus Predator: Extinction", 0, 20, 14, 480, 34, 0),
                     WM_SETFONT, (WPARAM)s_font_head, TRUE);
        add(h, "STATIC", "PC port launcher", 0, 20, 50, 480, 24, 0);
        s_status = add(h, "STATIC", "", 0, 20, 82, 480, 66, ID_STATUS);
        add(h, "BUTTON", "Play", BS_GROUPBOX, 14, 156, 492, 140, 0);
        add(h, "BUTTON", "Set up from my disc image...", BS_PUSHBUTTON | WS_TABSTOP, 30, 184, 226, 38, ID_SETUP);
        s_play = add(h, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 268, 184, 222, 38, ID_PLAY);
        add(h, "STATIC", "Input", 0, 30, 240, 60, 26, 0);
        s_in_mouse = add(h, "BUTTON", "Mouse && keyboard", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 96, 238, 168, 28, ID_IN_MOUSE);
        s_in_pad = add(h, "BUTTON", "Controller", BS_AUTORADIOBUTTON | WS_TABSTOP, 272, 238, 160, 28, ID_IN_PAD);
        SendMessageA(pad ? s_in_pad : s_in_mouse, BM_SETCHECK, BST_CHECKED, 0);
        refresh();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) refresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_SETUP: pick_image(h); return 0;
        case ID_PLAY: launch(NULL); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        save_prefs();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int avp_launcher_main(HINSTANCE inst, int show)
{
    WNDCLASSA wc;
    MSG msg;
    HWND h;
    RECT r;
    char *slash;
    HDC dc;
    SetProcessDPIAware();
    dc = GetDC(NULL);
    if (dc) { s_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
    if (s_dpi < 96) s_dpi = 96;
    r.left = 0; r.top = 0; r.right = S(520); r.bottom = S(312);
    GetModuleFileNameA(NULL, s_dir, sizeof s_dir);
    slash = strrchr(s_dir, '\\');
    if (slash) *slash = 0;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = "AvPLauncher";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    h = CreateWindowA("AvPLauncher", "AvP: Extinction Launcher", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                      CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(h, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(h, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

