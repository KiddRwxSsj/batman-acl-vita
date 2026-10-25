#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>

#include <psp2/kernel/clib.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb/stb_image.h>

#include "comic.h"
#include "java.h"
#include "gfx.h"
#include "loader.h"
#include "obb.h"
#include "util.h"

// android swaps the gl view for a java comic reader (HVS_ComicView) and pauses the engine.
// same here: interrupt the engine so it lets go of gl, draw the pages ourselves, then hand back
#define PAGE_FMT OBB_ROOT "HVS_Comic/BM_AC_01_300-%d.png"

volatile int comic_requested;

static GLuint page_tex;
static int page_w, page_h;

static int load_page(int n) {
    char name[96];
    uint32_t size;
    snprintf(name, sizeof(name), PAGE_FMT, n);
    void *png = obb_load(name, &size);
    if (!png)
        return 0;
    int w, h, comp;
    uint8_t *pixels = stbi_load_from_memory(png, size, &w, &h, &comp, 4);
    free(png);
    if (!pixels)
        return 0;
    if (!page_tex)
        glGenTextures(1, &page_tex);
    glBindTexture(GL_TEXTURE_2D, page_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(pixels);
    page_w = w;
    page_h = h;
    return 1;
}

static int count_pages(void) {
    char name[96];
    uint32_t off, size;
    int n = 0;
    do {
        snprintf(name, sizeof(name), PAGE_FMT, ++n);
    } while (obb_find(name, &off, &size) == 0);
    return n - 1;
}

static void run_viewer(void) {
    int pages = count_pages(), page = 1, zoom = 0, loaded = 0;
    float scroll = 0;
    uint32_t old = ~0u;
    int touch_x = -1, touch_y = -1, touch_x0 = 0, touch_y0 = 0;
    float touch_scroll0 = 0;
    LOG("comic: %d pages", pages);
    if (!pages)
        return;

    while (1) {
        if (loaded != page) {
            if (!load_page(page))
                LOG("comic: page %d failed", page);
            loaded = page;
            scroll = 0;
        }

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~old;
        old = pad.buttons;
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
            break;
        if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER) && page < pages)
            page++;
        if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER) && page > 1)
            page--;
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE))
            zoom = !zoom;

        // zoomed in the page is screen wide, sticks and d-pad scroll it
        float zh = (float)SCREEN_W * page_h / page_w;
        float max_scroll = zh > SCREEN_H ? zh - SCREEN_H : 0;
        float sy = (pad.ly - 128) / 128.0f + (pad.ry - 128) / 128.0f;
        if (fabsf(sy) > 0.2f)
            scroll += sy * 14;
        if (pad.buttons & SCE_CTRL_DOWN)
            scroll += 10;
        if (pad.buttons & SCE_CTRL_UP)
            scroll -= 10;

        // touch: drag scrolls when zoomed, a short tap on either side turns the page
        SceTouchData touch;
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum) {
            int x = touch.report[0].x * SCREEN_W / 1920, y = touch.report[0].y * SCREEN_H / 1088;
            if (touch_x < 0) {
                touch_x0 = x;
                touch_y0 = y;
                touch_scroll0 = scroll;
            }
            touch_x = x;
            touch_y = y;
            if (zoom)
                scroll = touch_scroll0 - (y - touch_y0) * 1.5f;
        } else if (touch_x >= 0) {
            int dx = touch_x - touch_x0, dy = touch_y - touch_y0;
            if (abs(dx) > 80 && abs(dx) > abs(dy)) {
                if (dx < 0 && page < pages)
                    page++;
                else if (dx > 0 && page > 1)
                    page--;
            } else if (abs(dx) < 15 && abs(dy) < 15) {
                if (touch_x0 > SCREEN_W * 2 / 3 && page < pages)
                    page++;
                else if (touch_x0 < SCREEN_W / 3 && page > 1)
                    page--;
                else
                    zoom = !zoom;
            }
            touch_x = -1;
        }
        if (scroll < 0)
            scroll = 0;
        if (scroll > max_scroll)
            scroll = max_scroll;

        gfx_quad_begin();
        if (page_tex && page_w) {
            if (zoom) {
                gfx_quad(page_tex, 0, -scroll, SCREEN_W, zh);
            } else {
                float w = (float)SCREEN_H * page_w / page_h;
                gfx_quad(page_tex, (SCREEN_W - w) / 2, 0, w, SCREEN_H);
            }
        }
        vglSwapBuffers(GL_FALSE);
    }

    // don't let the button that closed it reach the game
    SceCtrlData pad;
    do {
        sceKernelDelayThread(16000);
        sceCtrlPeekBufferPositive(0, &pad, 1);
    } while (pad.buttons);
}

void comic_run(void) {
    jboolean (*interruption)(JNIEnv *, jobject, jboolean) =
        (void *)ue3_sym("_Z34NativeCallback_InterruptionChangedP7_JNIEnvP8_jobjecth");
    void (*close_viewer)(void) = (void *)ue3_sym("_Z38NativeCallback_AndroidCloseComicViewerv");

    comic_requested = 0;
    LOG("comic: pausing engine");
    interruption(&jni, java_thiz, JNI_TRUE);
    run_viewer();
    if (page_tex) {
        glDeleteTextures(1, &page_tex);
        page_tex = 0;
    }
    glFinish();
    LOG("comic: closed");
    close_viewer();
    interruption(&jni, java_thiz, JNI_FALSE);
}
