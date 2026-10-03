// Per-app loopback capture (Windows 10 2004 and later): the sound one app and its child processes play, or every app's
// but one, as 24 kHz mono 16-bit, through WASAPI's process loopback (ActivateAudioInterfaceAsync on the virtual
// "VAD\Process_Loopback" device). miniaudio offers it only in Store builds, so the desktop app does it here.
#ifndef BRIAREUS_LOOPBACK_H
#define BRIAREUS_LOOPBACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Called on the capture thread with each packet, already 24 kHz mono.
typedef void (*LoopbackFrames)(void *ctx, const int16_t *pcm, size_t samples);
typedef struct Loopback Loopback;
/// Captures process `pid`'s tree, or with `exclude` every process but its tree. NULL with `*error` when Windows refuses.
Loopback *loopback_start(unsigned pid, bool exclude, LoopbackFrames frames, void *ctx, char **error);
void loopback_stop(Loopback *l);

#endif
