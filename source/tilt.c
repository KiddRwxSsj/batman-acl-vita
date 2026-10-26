#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>

#include "java.h"
#include "loader.h"
#include "util.h"
#include "tilt.h"

// java feeds the rotation sensor into NativeCallback_SetAttitude as yaw/pitch/roll in radians
// (SensorManager.getOrientation, landscape). the engine hands that to the player controller's
// OnMobileMotion: the flying batarang steers with clamp(roll / pc+0x660, -1, 1), the tilt tutorial
// wants roll past +-0.78. roll and pitch come from the accelerometer here, yaw stays 0.
// tilting the right side down is positive roll, same as android. the vita reports the gravity
// direction in g (0 0 -1 lying face up), android the opposite, so up = -accel

int tilt_enabled = 1;
int tilt_sensitivity = 100; // percent

typedef void (*attitude_fn)(JNIEnv *, jobject, float yaw, float pitch, float roll);
static attitude_fn SetAttitude;

// java starts the sensor in onCreate and ToggleSensors only stops it after an earlier true
static int feeding = 1, toggled;
static float roll, pitch;
static int logged;

void tilt_init(void) {
    SetAttitude = (attitude_fn)ue3_sym("_Z26NativeCallback_SetAttitudeP7_JNIEnvP8_jobjectfff");
    int r = sceMotionStartSampling();
    LOG("tilt: sampling %x, tilt %d sensitivity %d", r, tilt_enabled, tilt_sensitivity);
}

void tilt_toggle(int on) {
    LOG("tilt: ToggleSensors %d", on);
    if (on != toggled) {
        feeding = on;
        toggled = on;
    }
}

void tilt_poll(const SceCtrlData *pad) {
    if (!feeding || !SetAttitude)
        return;
    float r = 0.0f, p = 0.0f;
    if (tilt_enabled) {
        SceMotionSensorState s;
        r = roll;
        p = pitch;
        if (sceMotionGetSensorState(&s, 1) >= 0) {
            float x = s.accelerometer.x, y = s.accelerometer.y, z = s.accelerometer.z;
            if (!logged) {
                logged = 1;
                LOG("tilt: accel %.2f %.2f %.2f", x, y, z);
            }
            float len = sqrtf(x * x + y * y + z * z);
            if (len > 0.1f) {
                float k = tilt_sensitivity / 100.0f;
                r = atan2f(x, -z) * k;
                p = asinf(fmaxf(-1.0f, fminf(1.0f, y / len))) * k;
            }
        }
    }
    // left stick steers too, overrides the tilt while it's pushed
    float lx = (pad->lx - 128) / 128.0f;
    if (fabsf(lx) > 0.25f)
        r = lx;
    // raw accel jitters, smooth it a little (polled every ~8 ms)
    roll += (r - roll) * 0.35f;
    pitch += (p - pitch) * 0.35f;
    SetAttitude(&jni, java_thiz, 0.0f, pitch, roll);
#ifdef PERF_TRACE
    static uint64_t last_log;
    static float last_roll;
    uint64_t now = uptime_ms();
    if (now - last_log > 500 && fabsf(roll - last_roll) > 0.1f) {
        last_log = now;
        last_roll = roll;
        LOG("tilt: roll %.2f pitch %.2f", roll, pitch);
    }
#endif
}
