// What Spotify (or, without it, whichever app Windows lists as playing) is playing, and its ⏮ ⏯ ⏭, through the system's
// media sessions (Windows.Media.Control), and its volume through Core Audio. No Spotify account or Web API: the desktop app publishes its session to Windows.
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
    bool has_volume;                    // the volume below can be read and set
    bool app_volume;                    // it is the app's own, in the volume mixer, rather than the speakers'
    bool muted;
    float volume;                       // 0...1
    char title[256], artist[256];
} MediaState;

/// MEDIA_MUTE flips the mute; MEDIA_VOLUME is media_set_volume's.
typedef enum { MEDIA_PREVIOUS, MEDIA_TOGGLE, MEDIA_NEXT, MEDIA_MUTE, MEDIA_VOLUME } MediaCommand;

/// Starts watching the media sessions on a thread of its own; nothing happens on a Windows without them.
void media_start(HWND notify);
void media_stop(void);
/// Reads every second while the app is in the foreground, every five otherwise.
void media_set_active(bool active);
/// The latest state, as copied by the watcher.
void media_state(MediaState *out);
/// Sends ⏮, ⏯ or ⏭ to the session shown; the state follows once the player has acted.
void media_command(MediaCommand command);
/// Sets the player's volume (0...1) and unmutes it: the app's own in the volume mixer, or the speakers' without one.
void media_set_volume(float level);

#endif
