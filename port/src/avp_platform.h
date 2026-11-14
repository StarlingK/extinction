#ifndef AVP_PLATFORM_H
#define AVP_PLATFORM_H

#ifdef _WIN32

#include <windows.h>

#define AVP_SEP "\\"
#define AVP_SEPC '\\'
#define AVP_MAX_PATH MAX_PATH

#else

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <platform/win32_compat.h>

#define AVP_SEP "/"
#define AVP_SEPC '/'
#define AVP_MAX_PATH 1024

#ifndef VK_LSHIFT
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#endif
#ifndef VK_NUMPAD0
#define VK_NUMPAD0 0x60
#define VK_NUMPAD9 0x69
#define VK_NUMPAD2 0x62
#define VK_NUMPAD4 0x64
#define VK_NUMPAD6 0x66
#define VK_NUMPAD8 0x68
#endif
#ifndef VK_DECIMAL
#define VK_DECIMAL 0x6E
#endif
#ifndef VK_F6
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F24 0x87
#endif
#ifndef WM_MOUSEMOVE
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_CLOSE 0x0010
#endif

#ifndef VK_OEM_4
#define VK_OEM_4 0xDB
#define VK_OEM_6 0xDD
#endif

extern int xbox_FramebufferKeyDown(int vk);
#define GetKeyState(vk) ((SHORT)(xbox_FramebufferKeyDown(vk) ? -1 : 0))

#ifndef SW_SHOW
#define SW_SHOW 5
#endif
#ifndef MB_YESNO
#define MB_YESNO 0x00000004u
#define MB_RETRYCANCEL 0x00000005u
#define MB_ICONQUESTION 0x00000020u
#define IDYES 6
#define IDRETRY 4
#define IDOK 1
#endif

static inline int avp_putenv(const char *kv)
{
    char name[128];
    const char *eq = strchr(kv, '=');
    size_t n;
    if (!eq)
        return -1;
    n = (size_t)(eq - kv);
    if (n == 0 || n >= sizeof name)
        return -1;
    memcpy(name, kv, n);
    name[n] = 0;
    if (!eq[1])
        return unsetenv(name);
    return setenv(name, eq + 1, 1);
}

#define _putenv avp_putenv
#define SetEnvironmentVariableA(name, value) setenv((name), (value), 1)

#endif

#endif
