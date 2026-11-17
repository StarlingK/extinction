#ifndef PLAT_H
#define PLAT_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PE_NONE, PE_KEY, PE_CHAR, PE_MOUSE_MOVE, PE_MOUSE_BTN, PE_WHEEL, PE_RESIZE, PE_CLOSE, PE_FOCUS_LOST
};

enum {
    PK_NONE = 0,
    PK_TAB = 1, PK_LEFT, PK_RIGHT, PK_UP, PK_DOWN, PK_PGUP, PK_PGDN, PK_HOME, PK_END, PK_INSERT,
    PK_DELETE, PK_BACKSPACE, PK_SPACE, PK_ENTER, PK_ESCAPE, PK_LSHIFT, PK_LCTRL, PK_LALT, PK_F1,
    PK_A = 40
};

typedef struct {
    int type;
    int a, b;
    float f;
} PlatEvent;

int  plat_open(const char *title, int w, int h);
int  plat_poll(PlatEvent *e);
void plat_swap(void);
void plat_close(void);
void *plat_getproc(const char *name);
void plat_size(int *w, int *h);
double plat_time(void);
void plat_sleep_ms(int ms);
int  plat_exe_dir(char *out, int cap);
int  plat_launch(const char *exe, const char *arg1, const char *arg2);
void plat_set_title(const char *t);

#ifdef __cplusplus
}
#endif

#endif
