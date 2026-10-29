#ifndef BRIAREUS_APP_SLEEP_H
#define BRIAREUS_APP_SLEEP_H
#include "store.h"
#include "../core/sleep.h"
// All lifecycle functions run on the UI thread. The overlay never changes pane visibility or activation.
void sleep_start(HWND main);
// Timer entry point with monotonic milliseconds, also used by deterministic lifecycle tests.
void sleep_pulse(uint64_t now);
void sleep_stop(void);
void sleep_scope_changed(void);
void sleep_deactivated(void);
void sleep_modal(bool begin);
int sleep_after(void);
void sleep_set_after(int minutes);
void sleep_settings_action(void);
// Shared dark drawing surface; borrowed wall, no controls or actions.
void sleep_paint(HWND hwnd, const SleepWall *wall);
#endif
