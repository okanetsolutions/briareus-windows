// A pane hosts a stack of screens: a header with back and actions, a scrolling document and an optional footer.
#ifndef BRIAREUS_PANE_H
#define BRIAREUS_PANE_H
#include "doc.h"
#include <windows.h>
#include <stdbool.h>

typedef struct Pane Pane;
typedef struct Screen Screen;

/// A header button: a glyph alone, or a labelled pill when `label` is set and the header has room for the labels.
/// `prominent` fills it with the accent, as the dashboard's `.btn-primary` (Save).
typedef struct { wchar_t glyph; int action; bool enabled; const char *tip; char label[40]; bool destructive, prominent; } HeaderButton;
enum { HEADER_BUTTONS = 8 };
typedef struct {
    char title[512]; char subtitle[512]; char status[48];   // status draws a dot before the subtitle
    HeaderButton buttons[HEADER_BUTTONS]; int button_count;
    bool large;         // unused: the dashboard has one title size
    int title_action;   // when set, a ✎ after the title fires it (the dashboard's Edit session title)
} HeaderInfo;

typedef struct ScreenVTable {
    void (*destroy)(Screen *s);
    void (*layout)(Screen *s, Doc *doc);
    void (*header)(Screen *s, HeaderInfo *info);
    /// A click on an item or header button; `pt` is in screen coordinates, for menus.
    void (*action)(Screen *s, int action, intptr_t arg, POINT pt);
    /// A right click on an item; optional.
    void (*context)(Screen *s, int action, intptr_t arg, POINT pt);
    void (*timer)(Screen *s, UINT id);
    int (*footer_height)(Screen *s, int width);
    void (*footer_layout)(Screen *s, const RECT *rc);
    void (*footer_paint)(Screen *s, Canvas *cv, const RECT *rc);
    void (*footer_click)(Screen *s, POINT pt);
    /// Moves content child controls after a layout or scroll; optional.
    void (*place)(Screen *s, const RECT *content, int scroll_y);
    /// The screen is (or stops being) the pane's top: start or stop polling, show or hide controls.
    void (*visible)(Screen *s, bool shown);
    void (*command)(Screen *s, int id, int code, HWND control);
    /// A key press while the pane has focus; return true when handled.
    bool (*key)(Screen *s, WPARAM vk, bool ctrl, bool shift);
    /// F5 or the refresh button: read everything again.
    void (*refresh)(Screen *s);
    void (*scrolled)(Screen *s, bool at_bottom);
    /// The application came to the foreground or left it.
    void (*activated)(Screen *s, bool active);
    /// Another screen is about to replace this one as the detail pane's root: false keeps it (unsaved changes); optional.
    bool (*can_leave)(Screen *s);
    /// The detail pane keeps this screen over a fresh one with the same id: take what the fresh one asks for; optional.
    void (*adopt)(Screen *s, Screen *fresh);
} ScreenVTable;

struct Screen {
    const ScreenVTable *vt;
    Pane *pane;
    char *id;   // what the screen shows, such as "conversation:<id>", so a repeated choice is not reopened
};

Pane *pane_create(HWND parent, bool sidebar);
void pane_destroy(Pane *pane);
HWND pane_hwnd(Pane *pane);
void pane_set_bounds(Pane *pane, const RECT *rc);
void pane_show(Pane *pane, bool shown);
bool pane_is_sidebar(Pane *pane);

void pane_push(Pane *pane, Screen *screen);
void pane_pop(Pane *pane);
void pane_pop_to_root(Pane *pane);
/// Replaces the whole stack with one screen (NULL empties the pane).
void pane_set_root(Pane *pane, Screen *screen);
Screen *pane_top(Pane *pane);
Screen *pane_root(Pane *pane);
size_t pane_depth(Pane *pane);

/// The document is stale: lay out and paint again.
void pane_relayout(Pane *pane);
void pane_repaint(Pane *pane);
void pane_header_changed(Pane *pane);
void pane_footer_changed(Pane *pane);
void pane_scroll_to_bottom(Pane *pane);
void pane_scroll_to_top(Pane *pane);
bool pane_at_bottom(Pane *pane);
/// Keeps the view on the bottom while the content grows, as a transcript does.
void pane_stick_to_bottom(Pane *pane, bool stick);
/// Shows a floating "latest" button when the view is not at the bottom.
void pane_show_bottom_button(Pane *pane, bool show);
/// In one column the root screen shows a back button that returns to the sidebar.
void pane_set_root_back(Pane *pane, bool show, void (*callback)(void *ctx), void *ctx);
/// The row the sidebar highlights: the detail pane's root screen id.
void pane_set_selected_id(Pane *pane, const char *id);
const char *pane_selected_id(Pane *pane);
int pane_content_width(Pane *pane);
RECT pane_content_rect(Pane *pane);
/// Content y of the top of the visible area.
int pane_scroll_y(Pane *pane);
/// Scrolls so a content y is visible near the top.
void pane_scroll_to(Pane *pane, int content_y);
/// A screen asks the app to go to the foreground of the other pane, or to close itself.
void pane_activate_all(bool active);

/// Frees a screen's base fields; a screen's destroy calls it last.
void screen_release(Screen *screen);

#endif
