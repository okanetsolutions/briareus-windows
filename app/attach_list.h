// The files for a composer's next message or first prompt, as the dashboard's `.attach-list`: each taken from a paste,
// a drop or the 📎 button's dialog, uploaded at once, and shown as a chip with ✕ above the text until it is sent.
#ifndef BRIAREUS_ATTACH_LIST_H
#define BRIAREUS_ATTACH_LIST_H
#include "pane.h"
#include "theme.h"
#include "json.h"
#include <shellapi.h>

enum { ATTACHMENTS_MAX = 10 };   // the dashboard's "At most 10 files per message"

typedef struct AttachList AttachList;
/// `call` is what the files go with (`message` or `start_session`); the screen's pane hears of every change.
AttachList *attach_list_new(Screen *screen, const char *call);
/// Cancels the uploads under way and forgets the files.
void attach_list_free(AttachList *l);
/// Whether the server takes files with `call` from this token.
bool attach_list_supported(const AttachList *l);
/// A paste that holds files or an image: true when it was taken as attachments and the text, if any, is not to be pasted.
bool attach_list_paste(AttachList *l);
/// Takes the handle.
void attach_list_drop(AttachList *l, HDROP drop);
/// The 📎 button: the open dialog, several files at once.
void attach_list_pick(AttachList *l);

size_t attach_list_count(const AttachList *l);
bool attach_list_uploading(const AttachList *l);
/// The uploaded files' ids, as the call's `attachments`; NULL when there are none.
Json *attach_list_ids(const AttachList *l);
/// Drops the files whose ids went with a call that succeeded; one attached since stays.
void attach_list_sent(AttachList *l, const Json *ids);

/// The chips' height at `width`, with the gap under them; 0 with no files. A NULL canvas measures.
int attach_list_height(AttachList *l, Canvas *cv, int width);
/// Paints the chips from (`left`, `top`) and keeps their ✕ for `attach_list_click`.
void attach_list_paint(AttachList *l, Canvas *cv, int left, int top, int width);
/// True when the point was a chip's ✕, and that file is gone.
bool attach_list_click(AttachList *l, POINT pt);

/// The 📎 button in a composer's row of buttons, at `rc`.
void attach_button_paint(Canvas *cv, const RECT *rc, bool enabled);

#endif
