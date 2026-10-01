// What Spotify (or, without it, whichever app Windows lists as playing) is playing, and its ⏮ ⏯ ⏭, through the system's
// media sessions (Windows.Media.Control). No Spotify account or Web API: the desktop app publishes its session to Windows.
#ifndef BRIAREUS_MEDIA_H
#define BRIAREUS_MEDIA_H
#include <windows.h>
#include <stdbool.h>

/// Posted to the window given to media_start when what is playing, or whether it plays, changes.
#define WM_APP_MEDIA_CHANGED (WM_APP + 4)

typedef struct {
    bool available;                     // a media session is open
    bool spotify;                       // and it is Spotify's
    bool playing;
    bool can_previous, can_toggle, can_next;
    char title[256], artist[256];
} MediaState;

typedef enum { MEDIA_PREVIOUS, MEDIA_TOGGLE, MEDIA_NEXT } MediaCommand;

/// Starts watching the media sessions on a thread of its own; nothing happens on a Windows without them.
void media_start(HWND notify);
void media_stop(void);
/// Reads every second while the app is in the foreground, every five otherwise.
void media_set_active(bool active);
/// The latest state, as copied by the watcher.
void media_state(MediaState *out);
/// Sends ⏮, ⏯ or ⏭ to the session shown; the state follows once the player has acted.
void media_command(MediaCommand command);

#endif
