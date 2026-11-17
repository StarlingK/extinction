#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "plat.h"

typedef GLXContext (*CreateCtxFn)(Display *, GLXFBConfig, GLXContext, Bool, const int *);

static Display *s_dpy;
static Window s_win;
static GLXContext s_ctx;
static Atom s_wm_delete;
static int s_w, s_h;
static XIM s_im;
static XIC s_ic;

static PlatEvent s_q[16];
static int s_qn;

static void push(const PlatEvent *e)
{
    if (s_qn < 16)
        s_q[s_qn++] = *e;
}

static PlatEvent pop(void)
{
    PlatEvent e = s_q[0];
    memmove(s_q, s_q + 1, (size_t)(--s_qn) * sizeof s_q[0]);
    return e;
}

static int map_key(KeySym ks)
{
    if (ks >= XK_a && ks <= XK_z)
        return PK_A + (int)(ks - XK_a);
    if (ks >= XK_A && ks <= XK_Z)
        return PK_A + (int)(ks - XK_A);
    if (ks >= XK_0 && ks <= XK_9)
        return PK_A + 26 + (int)(ks - XK_0);
    if (ks >= XK_F1 && ks <= XK_F12)
        return PK_F1 + (int)(ks - XK_F1);
    switch (ks) {
    case XK_Tab: return PK_TAB;
    case XK_Left: return PK_LEFT;
    case XK_Right: return PK_RIGHT;
    case XK_Up: return PK_UP;
    case XK_Down: return PK_DOWN;
    case XK_Page_Up: return PK_PGUP;
    case XK_Page_Down: return PK_PGDN;
    case XK_Home: return PK_HOME;
    case XK_End: return PK_END;
    case XK_Insert: return PK_INSERT;
    case XK_Delete: return PK_DELETE;
    case XK_BackSpace: return PK_BACKSPACE;
    case XK_space: return PK_SPACE;
    case XK_Return: case XK_KP_Enter: return PK_ENTER;
    case XK_Escape: return PK_ESCAPE;
    case XK_Shift_L: case XK_Shift_R: return PK_LSHIFT;
    case XK_Control_L: case XK_Control_R: return PK_LCTRL;
    case XK_Alt_L: case XK_Alt_R: return PK_LALT;
    }
    return PK_NONE;
}

int plat_open(const char *title, int w, int h)
{
    int fb_attr[] = { GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER, True, None };
    int ctx_attr[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None };
    int n = 0;
    GLXFBConfig *fbc;
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    CreateCtxFn create;
    s_dpy = XOpenDisplay(NULL);
    if (!s_dpy)
        return 0;
    fbc = glXChooseFBConfig(s_dpy, DefaultScreen(s_dpy), fb_attr, &n);
    if (!fbc || n < 1)
        return 0;
    vi = glXGetVisualFromFBConfig(s_dpy, fbc[0]);
    if (!vi)
        return 0;
    swa.colormap = XCreateColormap(s_dpy, RootWindow(s_dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask
        | StructureNotifyMask | FocusChangeMask | ExposureMask;
    s_win = XCreateWindow(s_dpy, RootWindow(s_dpy, vi->screen), 0, 0, (unsigned)w, (unsigned)h, 0, vi->depth,
        InputOutput, vi->visual, CWBorderPixel | CWColormap | CWEventMask, &swa);
    XFree(vi);
    if (!s_win)
        return 0;
    XStoreName(s_dpy, s_win, title);
    s_wm_delete = XInternAtom(s_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s_dpy, s_win, &s_wm_delete, 1);
    create = (CreateCtxFn)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    if (!create)
        return 0;
    s_ctx = create(s_dpy, fbc[0], 0, True, ctx_attr);
    XFree(fbc);
    if (!s_ctx)
        return 0;
    XMapWindow(s_dpy, s_win);
    glXMakeCurrent(s_dpy, s_win, s_ctx);
    {
        typedef void (*SwapFn)(Display *, GLXDrawable, int);
        SwapFn sw = (SwapFn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
        if (sw)
            sw(s_dpy, s_win, 1);
    }
    s_im = XOpenIM(s_dpy, NULL, NULL, NULL);
    if (s_im)
        s_ic = XCreateIC(s_im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, s_win, NULL);
    s_w = w;
    s_h = h;
    return 1;
}

int plat_poll(PlatEvent *e)
{
    XEvent ev;
    memset(e, 0, sizeof *e);
    if (s_qn) {
        *e = pop();
        return 1;
    }
    while (XPending(s_dpy)) {
        XNextEvent(s_dpy, &ev);
        if (XFilterEvent(&ev, None))
            continue;
        switch (ev.type) {
        case ConfigureNotify:
            s_w = ev.xconfigure.width;
            s_h = ev.xconfigure.height;
            e->type = PE_RESIZE;
            e->a = s_w;
            e->b = s_h;
            return 1;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == s_wm_delete) {
                e->type = PE_CLOSE;
                return 1;
            }
            break;
        case FocusOut:
            e->type = PE_FOCUS_LOST;
            return 1;
        case MotionNotify:
            e->type = PE_MOUSE_MOVE;
            e->a = ev.xmotion.x;
            e->b = ev.xmotion.y;
            return 1;
        case ButtonPress:
        case ButtonRelease:
            if (ev.xbutton.button >= 4 && ev.xbutton.button <= 5) {
                if (ev.type == ButtonPress) {
                    e->type = PE_WHEEL;
                    e->f = ev.xbutton.button == 4 ? 1.0f : -1.0f;
                    return 1;
                }
                break;
            }
            e->type = PE_MOUSE_BTN;
            e->a = ev.xbutton.button == 1 ? 0 : ev.xbutton.button == 3 ? 1 : 2;
            e->b = ev.type == ButtonPress;
            return 1;
        case KeyPress: {
            char buf[16];
            KeySym ks = 0;
            int len = s_ic ? Xutf8LookupString(s_ic, &ev.xkey, buf, (int)sizeof buf - 1, &ks, NULL)
                           : XLookupString(&ev.xkey, buf, (int)sizeof buf - 1, &ks, NULL);
            int key = map_key(XLookupKeysym(&ev.xkey, 0));
            if (key) {
                PlatEvent k;
                memset(&k, 0, sizeof k);
                k.type = PE_KEY;
                k.a = key;
                k.b = 1;
                push(&k);
            }
            if (len > 0 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127) {
                PlatEvent k;
                memset(&k, 0, sizeof k);
                k.type = PE_CHAR;
                k.a = (unsigned char)buf[0];
                push(&k);
            }
            if (s_qn) {
                *e = pop();
                return 1;
            }
            break;
        }
        case KeyRelease: {
            if (XPending(s_dpy)) {
                XEvent nx;
                XPeekEvent(s_dpy, &nx);
                if (nx.type == KeyPress && nx.xkey.time == ev.xkey.time && nx.xkey.keycode == ev.xkey.keycode)
                    break;
            }
            e->type = PE_KEY;
            e->a = map_key(XLookupKeysym(&ev.xkey, 0));
            e->b = 0;
            if (!e->a)
                break;
            return 1;
        }
        }
    }
    return 0;
}

void plat_swap(void)
{
    glXSwapBuffers(s_dpy, s_win);
}

void plat_close(void)
{
    if (s_dpy) {
        glXMakeCurrent(s_dpy, None, NULL);
        if (s_ctx)
            glXDestroyContext(s_dpy, s_ctx);
        if (s_win)
            XDestroyWindow(s_dpy, s_win);
        XCloseDisplay(s_dpy);
    }
    s_dpy = NULL;
}

void *plat_getproc(const char *name)
{
    return (void *)glXGetProcAddressARB((const GLubyte *)name);
}

void plat_size(int *w, int *h)
{
    *w = s_w;
    *h = s_h;
}

double plat_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void plat_sleep_ms(int ms)
{
    usleep((useconds_t)ms * 1000u);
}

int plat_exe_dir(char *out, int cap)
{
    ssize_t k = readlink("/proc/self/exe", out, (size_t)cap - 1);
    char *s;
    if (k <= 0)
        return 0;
    out[k] = 0;
    s = strrchr(out, '/');
    if (!s)
        return 0;
    *s = 0;
    return 1;
}

int plat_launch(const char *exe, const char *arg1, const char *arg2)
{
    pid_t p;
    if (access(exe, X_OK) != 0)
        return 0;
    p = fork();
    if (p < 0)
        return 0;
    if (p == 0) {
        execl(exe, exe, arg1, arg2, (char *)NULL);
        _exit(1);
    }
    return 1;
}

void plat_set_title(const char *t)
{
    if (s_dpy)
        XStoreName(s_dpy, s_win, t);
}
