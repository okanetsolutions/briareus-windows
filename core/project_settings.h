#ifndef BRIAREUS_PROJECT_SETTINGS_H
#define BRIAREUS_PROJECT_SETTINGS_H
#include "json.h"

/// Optional per-project setting; key absence is distinct from a supported false value.
/// Read from the saved project, server defaults, or a clone body, never global capabilities.
typedef struct { bool supported, on, saved; } ProjectBabysit;
ProjectBabysit project_babysit_read(const Json *row);
void project_babysit_toggle(ProjectBabysit *state);
bool project_babysit_changed(const ProjectBabysit *state);
/// Updates only babysitPrs in the borrowed body, removing it when unsupported.
void project_babysit_write(const ProjectBabysit *state, Json *body);
/// Borrowed note, NULL when the setting is hidden or off.
const char *project_babysit_note(const ProjectBabysit *state);
#endif
