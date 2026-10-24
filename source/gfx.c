#include "gfx.h"
#include <psp2/kernel/sysmem.h>
#include "perf.h"
#include <psp2/kernel/processmgr.h>
#include "util.h"
#include "bionic.h"

#include <stdio.h>
#include <psp2/io/fcntl.h>

int gfx_ready;

// the engine picks CookedAndroid_<format> off this list and only PVRTC data ships
// in the obb, so anything that would make it go looking for DXT/ATITC stays hidden
static const char *extensions =
    "GL_IMG_texture_compression_pvrtc "
    "GL_OES_depth24 "
    "GL_OES_packed_depth_stencil "
    "GL_OES_rgb8_rgba8 "
    "GL_OES_texture_npot";

void gfx_init(void) {
    if (gfx_ready)
        return;
    if (!file_exists("ur0:data/libshacccg.suprx") && !file_exists("ur0:data/external/libshacccg.suprx"))
        fatal_error("libshacccg.suprx is missing.\nInstall it with ShaRKBR33D first.");

    // shaders get translated and compiled at link time, the gxp cache skips that on later boots
    vglSetShaderCachePath(DATA_PATH "shader_cache");
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
    // same as vglInitExtended, except 20MB of phycont stays out of vitagl for the intro movie decoder
    // and 40MB more user ram for the big blocks the heap gave up (bionic.c big_alloc)
    vglInitWithCustomThreshold(0, SCREEN_W, SCREEN_H, 56 * 1024 * 1024, 0, 20 * 1024 * 1024, 0x8C6000, SCE_GXM_MULTISAMPLE_NONE);
    gfx_ready = 1;
    LOG("vitaGL up");
}

#ifdef DEBUG_SHOTS
static void debug_frame(void) {
    static int frames;
    static uint64_t last;
    frames++;
    uint64_t now = uptime_ms();
    if (now - last < 5000)
        return;
    LOG("%d fps", frames * 1000 / (int)(now - last));
    frames = 0;
    last = now;
}

// press select and the next frame lands in shot.raw (rgba, bottom-up)
volatile int shot_req;

static void shot_on_request(void) {
    static uint8_t pixels[SCREEN_W * SCREEN_H * 4];
    if (!shot_req)
        return;
    shot_req = 0;
    glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    file_save(DATA_PATH "shot.raw", pixels, sizeof(pixels));
    LOG("shot saved");
}
#endif

void gfx_swap(void) {
    progress_ticks++;
    swap_ticks++;
    // a frame over 2 s is a level load (or boot) ending, memory is most interesting right there
    static uint64_t last_swap;
    uint64_t now = uptime_ms();
    if (last_swap && now - last_swap > 2000) {
        char what[32];
        snprintf(what, sizeof(what), "after %u ms frame", (unsigned)(now - last_swap));
        mem_log(what);
    }
    last_swap = now;
#ifdef DEBUG_SHOTS
    debug_frame();
    shot_on_request();
#endif
#ifdef GL_TRACE
    extern int gl_trace_on;
    static int frame;
    frame++;
    gl_trace_on = frame >= 120 && frame < 122;
    if (gl_trace_on)
        LOG("---- frame %d", frame);
#endif
#ifdef PERF_TRACE
    perf_swap_begin();
    uint64_t swap_start = sceKernelGetProcessTimeWide();
    vglSwapBuffers(GL_FALSE);
    perf_swap_end(swap_start);
#else
    vglSwapBuffers(GL_FALSE);
#endif
}

const GLubyte *gl_get_string(GLenum name) {
    if (name == GL_EXTENSIONS)
        return (const GLubyte *)extensions;
    return glGetString(name);
}

// gxm has no constant blend colour, and vitaGL never needs shaders detached
void gl_blend_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
}

void gl_detach_shader(GLuint program, GLuint shader) {
}

// first boot compiles every shader through libshacccg, this shows it's still going
void gl_link_program(GLuint program) {
    static int links;
    uint64_t t = uptime_ms();
    glLinkProgram(program);
    progress_ticks++;
    if (++links % 25 == 0 || uptime_ms() - t > 1000)
        LOG("%d program links, last took %u ms", links, (unsigned)(uptime_ms() - t));
}

void *egl_get_display(void *native) {
    return (void *)1;
}

void *egl_get_current_surface(int which) {
    return (void *)1;
}

int egl_swap_buffers(void *dpy, void *surface) {
    gfx_swap();
    return 1;
}

// plain textured quad in screen pixels, for the intro movie and the comic viewer.
// only used while the engine isn't touching gl (before Init, or while interrupted)
static GLuint quad_prog;
static GLint quad_tex_loc;

static const char *quad_vs =
    "attribute vec2 pos;\n"
    "attribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); v_uv = uv; }\n";

static const char *quad_fs =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(tex, v_uv); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    return sh;
}

void gfx_quad_begin(void) {
    if (!quad_prog) {
        quad_prog = glCreateProgram();
        glAttachShader(quad_prog, compile(GL_VERTEX_SHADER, quad_vs));
        glAttachShader(quad_prog, compile(GL_FRAGMENT_SHADER, quad_fs));
        glBindAttribLocation(quad_prog, 0, "pos");
        glBindAttribLocation(quad_prog, 1, "uv");
        glLinkProgram(quad_prog);
        quad_tex_loc = glGetUniformLocation(quad_prog, "tex");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(quad_prog);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(quad_tex_loc, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gfx_quad(GLuint tex, float x, float y, float w, float h) {
    float x0 = x / SCREEN_W * 2 - 1, x1 = (x + w) / SCREEN_W * 2 - 1;
    float y0 = 1 - y / SCREEN_H * 2, y1 = 1 - (y + h) / SCREEN_H * 2;
    float pos[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    static const float uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
