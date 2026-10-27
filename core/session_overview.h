// Status decisions shared by the sidebar, overview and transcript readers.
#ifndef BRIAREUS_SESSION_OVERVIEW_H
#define BRIAREUS_SESSION_OVERVIEW_H
#include "models.h"

typedef enum { SESSION_WAITING, SESSION_FINDINGS, SESSION_WORKING, SESSION_READY, SESSION_FAILED, SESSION_CLOSED } SessionState;
SessionState session_state(const Session *session);
bool session_needs_you(const Session *session);
const char *session_state_label(SessionState state);
// Owned index arrays, borrowing the input sessions; stable for equal wait times/titles.
typedef struct { size_t *waiting, waiting_count, *working, working_count, ready; } SessionOverview;
SessionOverview session_overview(const Session *sessions, size_t count);
void session_overview_free(SessionOverview *overview);
// Borrowed last unanswered ask; a later user event answers it, including an empty message.
const Event *transcript_pending_question(const Transcript *transcript);
// Owned identity of the turn that asked. Changes invalidate a question read in flight.
Json *session_question_identity(const Session *session);
#endif
