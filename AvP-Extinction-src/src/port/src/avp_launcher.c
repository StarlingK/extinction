#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>

#define XBE_SIZE 2633728u

enum { ID_SETUP = 101, ID_PLAY, ID_HOST, ID_JOIN, ID_IP, ID_PORT, ID_STATUS };

static HWND s_status, s_play, s_host, s_join, s_ip, s_port;
static char s_dir[MAX_PATH];
static HFONT s_font;

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
    { "RECOMP_KB_UNBIND", "WASD" },
};

static int game_exe(char *out, size_t n)
{
    static const char *const names[] = { "AvPExtinction.exe", "extinction.exe" };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(out, n, "%s\\%s", s_dir, names[i]);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    return 0;
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
    EnableWindow(s_host, ok);
    EnableWindow(s_join, ok);
}

static void launch(const char *coop, const char *iso)
{
    char exe[MAX_PATH * 2], cmd[MAX_PATH * 6];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    size_t i;

    if (!game_exe(exe, sizeof exe)) {
        MessageBoxA(NULL, "The game program is missing from this folder.", "AvP: Extinction", MB_ICONERROR);
        return;
    }
    for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
        SetEnvironmentVariableA(k_env[i][0], k_env[i][1]);
    SetEnvironmentVariableA("AVP_COOP", coop && *coop ? coop : NULL);
    if (iso)
        snprintf(cmd, sizeof cmd, "\"%s\" --iso \"%s\"", exe, iso);
    else
        snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, s_dir, &si, &pi)) {
        MessageBoxA(NULL, "The game could not be started.", "AvP: Extinction", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
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
        launch(NULL, path);
}

static void port_text(char *out, size_t n)
{
    char p[16];
    int v;
    GetWindowTextA(s_port, p, sizeof p);
    v = atoi(p);
    if (v <= 0 || v > 65535) v = 27015;
    snprintf(out, n, "%d", v);
}

static HWND add(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int hh, int id)
{
    HWND c = CreateWindowExA(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, hh, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static LRESULT CALLBACK wnd(HWND h, UINT m, WPARAM w, LPARAM l)
{
    char coop[160], port[16], ip[96];
    switch (m) {
    case WM_CREATE:
        s_font = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        add(h, "STATIC", "Aliens versus Predator: Extinction - PC port", 0, 20, 16, 440, 22, 0);
        s_status = add(h, "STATIC", "", 0, 20, 46, 440, 60, ID_STATUS);
        add(h, "BUTTON", "Set up from my disc image...", BS_PUSHBUTTON, 20, 114, 220, 32, ID_SETUP);
        s_play = add(h, "BUTTON", "Play", BS_DEFPUSHBUTTON, 250, 114, 210, 32, ID_PLAY);
        add(h, "STATIC", "Co-op campaign", 0, 20, 164, 440, 22, 0);
        add(h, "STATIC", "Port", 0, 20, 194, 40, 22, 0);
        s_port = add(h, "EDIT", "27015", ES_NUMBER, 64, 191, 70, 24, ID_PORT);
        s_host = add(h, "BUTTON", "Host co-op", BS_PUSHBUTTON, 250, 188, 210, 30, ID_HOST);
        add(h, "STATIC", "Host IP", 0, 20, 232, 60, 22, 0);
        s_ip = add(h, "EDIT", "127.0.0.1", ES_AUTOHSCROLL, 84, 229, 150, 24, ID_IP);
        s_join = add(h, "BUTTON", "Join co-op", BS_PUSHBUTTON, 250, 226, 210, 30, ID_JOIN);
        add(h, "STATIC", "The host starts with the whole army. Select units and press G to give them to "
                         "player 2 for the rest of the mission.", 0, 20, 268, 440, 40, 0);
        refresh();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) refresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_SETUP: pick_image(h); return 0;
        case ID_PLAY: launch(NULL, NULL); return 0;
        case ID_HOST:
            port_text(port, sizeof port);
            snprintf(coop, sizeof coop, "host:%s", port);
            launch(coop, NULL);
            return 0;
        case ID_JOIN:
            port_text(port, sizeof port);
            GetWindowTextA(s_ip, ip, sizeof ip);
            snprintf(coop, sizeof coop, "join:%s:%s", *ip ? ip : "127.0.0.1", port);
            launch(coop, NULL);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    HWND h;
    RECT r = { 0, 0, 480, 320 };
    char *slash;
    (void)prev; (void)cmd;
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

