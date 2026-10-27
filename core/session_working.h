#ifndef BRIAREUS_SESSION_WORKING_H
#define BRIAREUS_SESSION_WORKING_H
#include "models.h"

/// Borrowed strings from a backgroundTasks entry; a nonempty name is required.
typedef struct { const char *name, *summary; } BackgroundTask;
bool background_task_parse(const Json *value, BackgroundTask *out);
/// True after the latest turn's result, past server logs, until user/agent activity or a status boundary.
bool transcript_answered(const Transcript *transcript);
/// Owned working-line text, or NULL for inactive sessions or a question awaiting an answer.
/// `verb` is the presentation's current animated verb; background tasks remain in Session.raw.
char *session_working_text(const Session *session, const Transcript *transcript, const char *verb);
#endif
