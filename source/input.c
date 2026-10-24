#include "input.h"
#include "java.h"
#include "gfx.h"
#include "loader.h"
#include "util.h"
#include "tilt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>

#define MAX_FINGERS 5
#define VIRTUAL_ID MAX_FINGERS
#define CONTROLS_PATH DATA_PATH "controls.txt"

enum { ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2 };

typedef jboolean (*input_event_fn)(JNIEnv *, jobject, int action, int x, int y, int pointer, int64_t time);
static input_event_fn NativeCallback_InputEvent;

// vita touch ids keep counting up, android pointer ids are small and get reused
static struct {
    int active;
    int vita_id;
    int x, y;
} fingers[MAX_FINGERS];

// the game only knows touch, so buttons get played back as taps and swipes
// on a pointer id the real fingers never use.
// strike: swipe in the left stick's direction, or alternate right/left (the combos are chains like R L R up)
// counter: tap the next counter circle on screen, or the middle (that's also the dodge tap)
enum { G_TAP, G_SWIPE, G_STRIKE, G_COUNTER };

typedef struct {
    uint32_t button;
    int type;
    int x, y, dx, dy;
} mapping;

static mapping maps[32];
static int num_maps;

static struct {
    int busy, tick, steps, hold;
    int x0, y0, x1, y1;
} gesture;

static int stick_armed = 1, strike_left;
static uint64_t last_strike;

static const struct { const char *name; uint32_t bit; } button_names[] = {
    { "CROSS", SCE_CTRL_CROSS }, { "CIRCLE", SCE_CTRL_CIRCLE },
    { "SQUARE", SCE_CTRL_SQUARE }, { "TRIANGLE", SCE_CTRL_TRIANGLE },
    { "UP", SCE_CTRL_UP }, { "DOWN", SCE_CTRL_DOWN },
    { "LEFT", SCE_CTRL_LEFT }, { "RIGHT", SCE_CTRL_RIGHT },
    { "L", SCE_CTRL_LTRIGGER }, { "R", SCE_CTRL_RTRIGGER },
    { "START", SCE_CTRL_START }, { "SELECT", SCE_CTRL_SELECT },
};

// screen is 960x544, swipes are start point + offset. the right stick always
// swipes from the middle in whatever direction it gets flicked
#define CONTROLS_VERSION "# controls v2"

static const char *default_controls =
    CONTROLS_VERSION "\n"
    "# <button> tap <x> <y>\n"
    "# <button> swipe <x> <y> <dx> <dy>\n"
    "# <button> strike     swipe at the enemy: left stick direction if held, else right/left in turn\n"
    "# <button> counter    tap the counter circle if one is up, else the middle of the screen (dodge)\n"
    "# square, square, circle = right, left, down combo. right stick flicks swipe in any direction\n"
    "SQUARE strike\n"
    "TRIANGLE counter\n"
    "CIRCLE swipe 480 160 0 220\n"
    "CROSS swipe 480 380 0 -220\n"
    "UP swipe 480 380 0 -220\n"
    "DOWN swipe 480 160 0 220\n"
    "LEFT swipe 600 272 -240 0\n"
    "RIGHT swipe 360 272 240 0\n"
    "L tap 64 500\n"
    "R tap 177 500\n"
    "START tap 480 20\n";

static void parse_controls(const char *text) {
    char line[128], name[16], kind[16];
    num_maps = 0;
    while (*text && num_maps < (int)(sizeof(maps) / sizeof(maps[0]))) {
        int len = strcspn(text, "\n");
        if (len >= (int)sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, text, len);
        line[len] = 0;
        text += strcspn(text, "\n");
        if (*text)
            text++;

        mapping m = {0};
        int n = sscanf(line, "%15s %15s %d %d %d %d", name, kind, &m.x, &m.y, &m.dx, &m.dy);
        if (line[0] == '#' || n < 2)
            continue;
        if (!strcasecmp(kind, "strike"))
            m.type = G_STRIKE;
        else if (!strcasecmp(kind, "counter"))
            m.type = G_COUNTER;
        else if (n >= 4)
            m.type = !strcasecmp(kind, "swipe") ? G_SWIPE : G_TAP;
        else
            continue;
        for (int i = 0; i < (int)(sizeof(button_names) / sizeof(button_names[0])); i++)
            if (!strcasecmp(name, button_names[i].name))
                m.button = button_names[i].bit;
        if (!m.button)
            continue;
        maps[num_maps++] = m;
    }
}

static void load_controls(void) {
    void *buf;
    size_t size;
    // an older file gets set aside once so the new defaults show up
    if (file_load(CONTROLS_PATH, &buf, &size) && (size < strlen(CONTROLS_VERSION) || memcmp(buf, CONTROLS_VERSION, strlen(CONTROLS_VERSION)))) {
        file_save(DATA_PATH "controls.old.txt", buf, size);
        free(buf);
        buf = NULL;
        LOG("old controls.txt saved as controls.old.txt");
    }
    if (buf) {
        char *text = realloc(buf, size + 1);
        text[size] = 0;
        parse_controls(text);
        free(text);
    } else {
        file_save(CONTROLS_PATH, default_controls, strlen(default_controls));
        parse_controls(default_controls);
    }
    LOG("%d button mappings", num_maps);
}

void input_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    NativeCallback_InputEvent = (input_event_fn)ue3_sym("_Z25NativeCallback_InputEventP7_JNIEnvP8_jobjectiiiix");
    load_controls();
    tilt_init();
}

static void send(int action, int x, int y, int pointer) {
#ifdef INPUT_TRACE
    if (action != ACTION_MOVE)
        LOG("touch %s %d,%d id %d", action == ACTION_DOWN ? "down" : "up", x, y, pointer);
#endif
    NativeCallback_InputEvent(&jni, java_thiz, action, x, y, pointer, uptime_ms());
}

static int start_gesture(int x0, int y0, int x1, int y1, int steps) {
    if (gesture.busy)
        return 0;
    gesture.busy = 1;
    gesture.tick = 0;
    gesture.steps = steps;
    gesture.hold = steps ? 2 : 6;
    gesture.x0 = x0;
    gesture.y0 = y0;
    gesture.x1 = x1;
    gesture.y1 = y1;
    return 1;
}

// one step per poll (8ms), so a swipe lasts ~80ms and a tap is held ~50ms
static void run_gesture(void) {
    if (!gesture.busy)
        return;
    int t = gesture.tick++;
    if (t == 0) {
        send(ACTION_DOWN, gesture.x0, gesture.y0, VIRTUAL_ID);
    } else if (t <= gesture.steps) {
        int x = gesture.x0 + (gesture.x1 - gesture.x0) * t / gesture.steps;
        int y = gesture.y0 + (gesture.y1 - gesture.y0) * t / gesture.steps;
        send(ACTION_MOVE, x, y, VIRTUAL_ID);
    } else if (t >= gesture.steps + gesture.hold) {
        send(ACTION_UP, gesture.x1, gesture.y1, VIRTUAL_ID);
        gesture.busy = 0;
    }
}

static void poll_buttons(void) {
    static uint32_t old;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;
    tilt_poll(&pad);

    uint32_t pressed = pad.buttons & ~old;
    old = pad.buttons;
#ifdef DEBUG_SHOTS
    if (pressed & SCE_CTRL_SELECT)
        shot_req = 1;
#endif
    for (int i = 0; i < num_maps; i++) {
        mapping *m = &maps[i];
        if (!(pressed & m->button))
            continue;
        if (m->type == G_SWIPE) {
            start_gesture(m->x, m->y, m->x + m->dx, m->y + m->dy, 10);
        } else if (m->type == G_STRIKE) {
            float lx = (pad.lx - 128) / 128.0f, ly = (pad.ly - 128) / 128.0f;
            float len = sqrtf(lx * lx + ly * ly);
            int dx, dy;
            if (len > 0.5f) {
                dx = (int)(lx / len * 240);
                dy = (int)(ly / len * 220);
            } else {
                // a fresh chain always opens with a right swipe
                if (uptime_ms() - last_strike > 1500)
                    strike_left = 0;
                dx = strike_left ? -240 : 240;
                dy = 0;
            }
            if (start_gesture(SCREEN_W / 2 - dx / 2, SCREEN_H / 2 - dy / 2, SCREEN_W / 2 + dx / 2, SCREEN_H / 2 + dy / 2, 10) && len <= 0.5f) {
                strike_left = !strike_left;
                last_strike = uptime_ms();
            }
        } else if (m->type == G_COUNTER) {
            int x = SCREEN_W / 2, y = SCREEN_H / 2;
            counter_target(&x, &y);
            start_gesture(x, y, x, y, 0);
        } else {
            start_gesture(m->x, m->y, m->x, m->y, 0);
        }
    }

    // fire once per flick, re-arm when the stick comes back to the middle
    float sx = (pad.rx - 128) / 128.0f, sy = (pad.ry - 128) / 128.0f;
    float len = sqrtf(sx * sx + sy * sy);
    if (len < 0.3f) {
        stick_armed = 1;
    } else if (stick_armed && len > 0.7f) {
        stick_armed = 0;
        int dx = (int)(sx / len * 240), dy = (int)(sy / len * 220);
        start_gesture(SCREEN_W / 2 - dx / 2, SCREEN_H / 2 - dy / 2, SCREEN_W / 2 + dx / 2, SCREEN_H / 2 + dy / 2, 10);
    }
}

static void poll_touch(void) {
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 1)
        return;

    int seen[MAX_FINGERS] = {0};

    for (int i = 0; i < touch.reportNum && i < MAX_FINGERS; i++) {
        int id = touch.report[i].id;
        int x = touch.report[i].x * SCREEN_W / 1920;
        int y = touch.report[i].y * SCREEN_H / 1088;

        int slot = -1;
        for (int s = 0; s < MAX_FINGERS; s++) {
            if (fingers[s].active && fingers[s].vita_id == id) {
                slot = s;
                break;
            }
        }

        if (slot < 0) {
            for (int s = 0; s < MAX_FINGERS; s++) {
                if (!fingers[s].active) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0)
                continue;
            fingers[slot].active = 1;
            fingers[slot].vita_id = id;
            fingers[slot].x = x;
            fingers[slot].y = y;
            send(ACTION_DOWN, x, y, slot);
        } else if (fingers[slot].x != x || fingers[slot].y != y) {
            fingers[slot].x = x;
            fingers[slot].y = y;
            send(ACTION_MOVE, x, y, slot);
        }
        seen[slot] = 1;
    }

    for (int s = 0; s < MAX_FINGERS; s++) {
        if (fingers[s].active && !seen[s]) {
            send(ACTION_UP, fingers[s].x, fingers[s].y, s);
            fingers[s].active = 0;
        }
    }
}

void input_poll(void) {
    poll_touch();
    poll_buttons();
    run_gesture();
}
