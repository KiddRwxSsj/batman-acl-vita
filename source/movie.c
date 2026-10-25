#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>

#include "movie.h"
#include "gfx.h"
#include "obb.h"
#include "util.h"

// decoded frames go straight into gxm textures, so keep a few in flight
#define FRAMES 5
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static SceAvPlayerHandle player;
static volatile int playing;
static uint32_t movie_off, movie_size;

static void *cpu_alloc(void *p, uint32_t align, uint32_t size) {
    return memalign(align, size);
}

static void cpu_free(void *p, void *ptr) {
    free(ptr);
}

// same as vitagl's video_playback sample: the decoder gets its own uncached phycont blocks,
// gfx_init leaves some phycont out of vitagl's pools for this
static void *gpu_alloc(void *p, uint32_t align, uint32_t size) {
    size = ALIGN(size, 1024 * 1024);
    SceUID blk = sceKernelAllocMemBlock("movie_frames", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);
    LOG("movie frame alloc %u align %x -> %08x", size, align, blk);
    if (blk < 0)
        return NULL;
    void *res;
    sceKernelGetMemBlockBase(blk, &res);
    sceGxmMapMemory(res, size, SCE_GXM_MEMORY_ATTRIB_RW);
    return res;
}

static void gpu_free(void *p, void *ptr) {
    glFinish();
    SceUID blk = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(blk);
}

// the movie is a stored entry in the obb, read it in place
static int f_open(void *p, const char *name) { return 0; }
static int f_close(void *p) { return 0; }
static uint64_t f_size(void *p) { return movie_size; }

static int f_read(void *p, uint8_t *buf, uint64_t pos, uint32_t len) {
    if (pos >= movie_size)
        return 0;
    if (pos + len > movie_size)
        len = movie_size - pos;
    return obb_read(movie_off + (uint32_t)pos, buf, len);
}

static int audio_thread(SceSize args, void *argp) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    while (playing) {
        SceAvPlayerFrameInfo frame;
        if (sceAvPlayerIsActive(player) && sceAvPlayerGetAudioData(player, &frame)) {
            sceAudioOutSetConfig(port, 1024, frame.details.audio.sampleRate,
                                 frame.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
            sceAudioOutOutput(port, frame.pData);
        } else {
            sceKernelDelayThread(1000);
        }
    }
    sceAudioOutReleasePort(port);
    return sceKernelExitThread(0);
}

static int skip_pressed(void) {
    static uint32_t old = ~0u;
    SceCtrlData pad;
    SceTouchData touch;
    uint32_t buttons = 0;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0)
        buttons = pad.buttons;
    // buttons still held from the bubble launch don't count
    int pressed = buttons & ~old;
    old = buttons;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum)
        pressed = 1;
    return pressed;
}

void movie_play(const char *name) {
    if (obb_find(name, &movie_off, &movie_size) < 0) {
        LOG("movie %s not in the obb", name);
        return;
    }
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = cpu_alloc;
    init.memoryReplacement.deallocate = cpu_free;
    init.memoryReplacement.allocateTexture = gpu_alloc;
    init.memoryReplacement.deallocateTexture = gpu_free;
    init.fileReplacement.open = f_open;
    init.fileReplacement.close = f_close;
    init.fileReplacement.readOffset = f_read;
    init.fileReplacement.size = f_size;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = FRAMES;
    init.autoStart = 1;
    player = sceAvPlayerInit(&init);
    // handles can look negative, the errors are 0x806axxxx
    if (((uint32_t)player & 0xffff0000) == 0x806a0000) {
        LOG("sceAvPlayerInit %08x", player);
        return;
    }

    GLuint tex[FRAMES];
    SceGxmTexture *gxm_tex[FRAMES];
    glGenTextures(FRAMES, tex);
    for (int i = 0; i < FRAMES; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gxm_tex[i] = vglGetGxmTexture(GL_TEXTURE_2D);
        vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
    }

    playing = 1;
    SceUID audio = sceKernelCreateThread("movie_audio", audio_thread, 0x10000100 - 10, 0x4000, 0, 0, NULL);
    sceKernelStartThread(audio, 0, NULL);
    int ret = sceAvPlayerAddSource(player, name);
    LOG("playing %s (%08x)", name, ret);

    int idx = 0, have_frame = 0;
    uint32_t w = 0, h = 0;
    uint64_t start = uptime_ms();
    while (1) {
        if (skip_pressed())
            break;
        if (sceAvPlayerIsActive(player)) {
            SceAvPlayerFrameInfo frame;
            if (sceAvPlayerGetVideoData(player, &frame)) {
                idx = (idx + 1) % FRAMES;
                w = frame.details.video.width;
                h = frame.details.video.height;
                sceGxmTextureInitLinear(gxm_tex[idx], frame.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, w, h, 0);
                sceGxmTextureSetMinFilter(gxm_tex[idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
                sceGxmTextureSetMagFilter(gxm_tex[idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
                have_frame = 1;
            }
        } else if (have_frame || uptime_ms() - start > 5000) {
            break;
        }

        gfx_quad_begin();
        if (have_frame) {
            // fit, keep the aspect
            float sw = SCREEN_W, sh = (float)SCREEN_W * h / w;
            if (sh > SCREEN_H) {
                sh = SCREEN_H;
                sw = (float)SCREEN_H * w / h;
            }
            gfx_quad(tex[idx], (SCREEN_W - sw) / 2, (SCREEN_H - sh) / 2, sw, sh);
        }
        vglSwapBuffers(GL_FALSE);
    }

    LOG("movie done after %u ms, frames %s", (unsigned)(uptime_ms() - start), have_frame ? "yes" : "none");
    sceAvPlayerStop(player);
    playing = 0;
    sceKernelWaitThreadEnd(audio, NULL, NULL);
    sceKernelDeleteThread(audio);
    sceAvPlayerClose(player);
    glFinish();
    // the textures still point at freed decoder memory, deleting them would free it again, so they just leak
    sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);
}
