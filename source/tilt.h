#ifndef TILT_H
#define TILT_H

#include <psp2/ctrl.h>

extern int tilt_enabled, tilt_sensitivity;

void tilt_init(void);
void tilt_toggle(int on);
void tilt_poll(const SceCtrlData *pad);

#endif
