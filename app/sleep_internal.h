#ifndef BRIAREUS_SLEEP_INTERNAL_H
#define BRIAREUS_SLEEP_INTERNAL_H
#include "sleep_mode.h"
typedef struct SleepFollow SleepFollow;
SleepFollow *sleep_follow_new(void);
void sleep_follow_cancel(SleepFollow *f);
// Owned queued records, at most 128; overflow reconnects to recover session state.
Json *sleep_follow_drain(SleepFollow *f, bool *ended, int *delay);
void sleep_follow_release(SleepFollow *f);
#endif
