#ifndef GFX_H
#define GFX_H

#include <vitaGL.h>

#define SCREEN_W 960
#define SCREEN_H 544

extern int gfx_ready;
extern volatile int shot_req;

void gfx_init(void);
void gfx_swap(void);
void gfx_quad_begin(void);
void gfx_quad(GLuint tex, float x, float y, float w, float h);

const GLubyte *gl_get_string(GLenum name);
void gl_blend_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void gl_detach_shader(GLuint program, GLuint shader);
void gl_link_program(GLuint program);

void *egl_get_display(void *native);
void *egl_get_current_surface(int which);
int egl_swap_buffers(void *dpy, void *surface);

#endif
