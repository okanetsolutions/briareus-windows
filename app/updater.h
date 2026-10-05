// Keeps Briareus up to date from its GitHub releases (core/update.c): a check shortly after launch and every six hours,
// and, unless turned off, the new executable downloaded and swapped in to run from the next start. The sidebar's version
// shows when one is waiting and opens the menu to check now, install, restart or turn the automatic updates off.
#ifndef BRIAREUS_UPDATER_H
#define BRIAREUS_UPDATER_H
#include "pane.h"
#include <stdbool.h>

/// Called once the main window exists: removes what the last update left behind and schedules the first check.
void updater_start(void);
/// The sidebar's version label: this build's, or the waiting release's with `*highlight` set.
const char *updater_label(bool *highlight);
/// The updates menu, at a point in `pane`'s client coordinates.
void updater_menu(Pane *pane, POINT pt);
/// After the main window closed: starts the updated executable when a restart was asked for.
void updater_relaunch_if_asked(void);
/// The close the restart asked for was turned down.
void updater_restart_cancelled(void);

#endif
