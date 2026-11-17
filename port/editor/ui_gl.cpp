#include "imgui.h"
#include "glmin.h"
#include "ui_gl.h"
#include <stdio.h>

static GLuint s_prog, s_vao, s_vbo, s_ebo, s_font;
static GLint s_u_proj, s_u_tex;

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"
    "layout(location=1) in vec2 uv;\n"
    "layout(location=2) in vec4 c;\n"
    "uniform mat4 proj;\n"
    "out vec2 vuv;\n"
    "out vec4 vc;\n"
    "void main(){ vuv=uv; vc=c; gl_Position=proj*vec4(p,0.0,1.0); }\n";

static const char *k_fs =
    "#version 330 core\n"
    "in vec2 vuv;\n"
    "in vec4 vc;\n"
    "uniform sampler2D tex;\n"
    "out vec4 o;\n"
    "void main(){ o=vc*texture(tex,vuv); }\n";

GLuint gl_program(const char *vs, const char *fs)
{
    GLuint a = glCreateShader(GL_VERTEX_SHADER), b = glCreateShader(GL_FRAGMENT_SHADER), p = glCreateProgram();
    GLint ok = 0;
    char log[1024];
    glShaderSource(a, 1, &vs, NULL);
    glCompileShader(a);
    glGetShaderiv(a, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(a, sizeof log, NULL, log);
        fprintf(stderr, "vertex shader: %s\n", log);
        return 0;
    }
    glShaderSource(b, 1, &fs, NULL);
    glCompileShader(b);
    glGetShaderiv(b, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(b, sizeof log, NULL, log);
        fprintf(stderr, "fragment shader: %s\n", log);
        return 0;
    }
    glAttachShader(p, a);
    glAttachShader(p, b);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "link: %s\n", log);
        return 0;
    }
    glDeleteShader(a);
    glDeleteShader(b);
    return p;
}

bool ui_gl_init()
{
    ImGuiIO &io = ImGui::GetIO();
    unsigned char *px;
    int w, h;
    s_prog = gl_program(k_vs, k_fs);
    if (!s_prog)
        return false;
    s_u_proj = glGetUniformLocation(s_prog, "proj");
    s_u_tex = glGetUniformLocation(s_prog, "tex");
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ebo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ebo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, uv));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, col));
    glBindVertexArray(0);
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    glGenTextures(1, &s_font);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)s_font);
    return true;
}

void ui_gl_render(ImDrawData *dd, int fbw, int fbh)
{
    if (dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0)
        return;
    float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x;
    float T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
    float proj[16] = { 2.0f / (R - L), 0, 0, 0, 0, 2.0f / (T - B), 0, 0, 0, 0, -1.0f, 0, (R + L) / (L - R), (T + B) / (B - T), 0, 1.0f };
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(s_prog);
    glUniform1i(s_u_tex, 0);
    glUniformMatrix4fv(s_u_proj, 1, GL_FALSE, proj);
    glBindVertexArray(s_vao);
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *cl = dd->CmdLists[n];
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cl->VtxBuffer.Size * (GLsizeiptr)sizeof(ImDrawVert), cl->VtxBuffer.Data, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)cl->IdxBuffer.Size * (GLsizeiptr)sizeof(ImDrawIdx), cl->IdxBuffer.Data, GL_STREAM_DRAW);
        for (int i = 0; i < cl->CmdBuffer.Size; i++) {
            const ImDrawCmd *cmd = &cl->CmdBuffer[i];
            if (cmd->UserCallback) {
                cmd->UserCallback(cl, cmd);
                continue;
            }
            float cx0 = (cmd->ClipRect.x - dd->DisplayPos.x) * dd->FramebufferScale.x;
            float cy0 = (cmd->ClipRect.y - dd->DisplayPos.y) * dd->FramebufferScale.y;
            float cx1 = (cmd->ClipRect.z - dd->DisplayPos.x) * dd->FramebufferScale.x;
            float cy1 = (cmd->ClipRect.w - dd->DisplayPos.y) * dd->FramebufferScale.y;
            if (cx1 <= cx0 || cy1 <= cy0)
                continue;
            glScissor((GLint)cx0, (GLint)((float)fbh - cy1), (GLsizei)(cx1 - cx0), (GLsizei)(cy1 - cy0));
            glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->GetTexID());
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd->ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                (const void *)(intptr_t)(cmd->IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    (void)fbw;
    glBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);
}
