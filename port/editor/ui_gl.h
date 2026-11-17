#ifndef UI_GL_H
#define UI_GL_H
#include "imgui.h"
#include "glmin.h"
bool ui_gl_init();
void ui_gl_render(ImDrawData *dd, int fbw, int fbh);
#endif
