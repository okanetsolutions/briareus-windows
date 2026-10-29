#ifndef BRIAREUS_SLEEP_INTERNAL_H
#define BRIAREUS_SLEEP_INTERNAL_H
#include "sleep_mode.h"
typedef struct SleepFollow SleepFollow;
SleepFollow *sleep_follow_new(void);
void sleep_follow_cancel(SleepFollow *f);
typedef struct { bool ended, reconcile; int status; double retry_after; } SleepFollowInfo;
// Owned FIFO batch, at most SLEEP_BACKLOG; excess session records request /sessions reconciliation without disconnecting.
Json *sleep_follow_drain(SleepFollow *f, SleepFollowInfo *info);
void sleep_follow_release(SleepFollow *f);
#endif
