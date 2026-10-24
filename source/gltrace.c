#ifdef GL_TRACE
#include <vitaGL.h>
#include <so_util/so_util.h>
#include "util.h"
#include "gfx.h"

int gl_trace_on;

#define T(fmt, ...) do { if (gl_trace_on) LOG("gl " fmt, ##__VA_ARGS__); } while (0)

static void t_glBindFramebuffer(GLenum t, GLuint fb) { T("BindFramebuffer %x %u", t, fb); glBindFramebuffer(t, fb); }
static void t_glBindRenderbuffer(GLenum t, GLuint rb) { T("BindRenderbuffer %u", rb); glBindRenderbuffer(t, rb); }
static void t_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { T("FramebufferTexture2D %x %x %u", a, tt, tex); glFramebufferTexture2D(t, a, tt, tex, l); }
static void t_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) { T("FramebufferRenderbuffer %x %u", a, rb); glFramebufferRenderbuffer(t, a, rt, rb); }
static void t_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { T("RenderbufferStorage %x %dx%d", f, w, h); glRenderbufferStorage(t, f, w, h); }
static void t_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { T("Viewport %d %d %d %d", x, y, w, h); glViewport(x, y, w, h); }
static void t_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { T("Scissor %d %d %d %d", x, y, w, h); glScissor(x, y, w, h); }
static void t_glClear(GLbitfield m) { T("Clear %x", m); glClear(m); }
static void t_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { T("ClearColor %.2f %.2f %.2f %.2f", r, g, b, a); glClearColor(r, g, b, a); }
static void t_glDrawArrays(GLenum m, GLint f, GLsizei c) { T("DrawArrays %x %d %d", m, f, c); glDrawArrays(m, f, c); }
static void t_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { T("DrawElements %x %d %x %p", m, c, t, i); glDrawElements(m, c, t, i); }
static void t_glUseProgram(GLuint p) { T("UseProgram %u", p); glUseProgram(p); }
static void t_glEnable(GLenum c) { T("Enable %x", c); glEnable(c); }
static void t_glDisable(GLenum c) { T("Disable %x", c); glDisable(c); }
static void t_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { T("ColorMask %d%d%d%d", r, g, b, a); glColorMask(r, g, b, a); }
static void t_glBindTexture(GLenum t, GLuint tex) { T("BindTexture %x %u", t, tex); glBindTexture(t, tex); }
static void t_glLinkProgram(GLuint p) { T("LinkProgram %u", p); gl_link_program(p); }
static void t_glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p) { T("VertexAttribPointer %u %d %x %d %d %p", i, s, t, n, st, p); glVertexAttribPointer(i, s, t, n, st, p); }
static void t_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { T("BlendFuncSeparate %x %x %x %x", a, b, c, d); glBlendFuncSeparate(a, b, c, d); }
static void t_glDepthFunc(GLenum f) { T("DepthFunc %x", f); glDepthFunc(f); }
static void t_glUniform4fv(GLint l, GLsizei c, const GLfloat *v) {
    T("Uniform4fv %d %d [%.3f %.3f %.3f %.3f]", l, c, v[0], v[1], v[2], v[3]);
    glUniform4fv(l, c, v);
}
static void t_glUniformMatrix4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) {
    T("UniformMatrix4fv %d %d %d [%.3f %.3f %.3f %.3f]", l, c, t, v[0], v[5], v[10], v[15]);
    glUniformMatrix4fv(l, c, t, v);
}
static GLint t_glGetAttribLocation(GLuint p, const GLchar *n) {
    GLint r = glGetAttribLocation(p, n);
    T("GetAttribLocation %u %s -> %d", p, n, r);
    return r;
}
static GLint t_glGetUniformLocation(GLuint p, const GLchar *n) {
    GLint r = glGetUniformLocation(p, n);
    T("GetUniformLocation %u %s -> %d", p, n, r);
    return r;
}

so_default_dynlib gl_trace_imports[] = {
    { "glBindFramebuffer", (uintptr_t)&t_glBindFramebuffer },
    { "glBindRenderbuffer", (uintptr_t)&t_glBindRenderbuffer },
    { "glFramebufferTexture2D", (uintptr_t)&t_glFramebufferTexture2D },
    { "glFramebufferRenderbuffer", (uintptr_t)&t_glFramebufferRenderbuffer },
    { "glRenderbufferStorage", (uintptr_t)&t_glRenderbufferStorage },
    { "glViewport", (uintptr_t)&t_glViewport },
    { "glScissor", (uintptr_t)&t_glScissor },
    { "glClear", (uintptr_t)&t_glClear },
    { "glClearColor", (uintptr_t)&t_glClearColor },
    { "glDrawArrays", (uintptr_t)&t_glDrawArrays },
    { "glDrawElements", (uintptr_t)&t_glDrawElements },
    { "glUseProgram", (uintptr_t)&t_glUseProgram },
    { "glEnable", (uintptr_t)&t_glEnable },
    { "glDisable", (uintptr_t)&t_glDisable },
    { "glColorMask", (uintptr_t)&t_glColorMask },
    { "glBindTexture", (uintptr_t)&t_glBindTexture },
    { "glLinkProgram", (uintptr_t)&t_glLinkProgram },
    { "glVertexAttribPointer", (uintptr_t)&t_glVertexAttribPointer },
    { "glBlendFuncSeparate", (uintptr_t)&t_glBlendFuncSeparate },
    { "glDepthFunc", (uintptr_t)&t_glDepthFunc },
    { "glUniform4fv", (uintptr_t)&t_glUniform4fv },
    { "glUniformMatrix4fv", (uintptr_t)&t_glUniformMatrix4fv },
    { "glGetAttribLocation", (uintptr_t)&t_glGetAttribLocation },
    { "glGetUniformLocation", (uintptr_t)&t_glGetUniformLocation },
};
int gl_trace_imports_size = sizeof(gl_trace_imports);
#endif
