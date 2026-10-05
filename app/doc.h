// A screen's content as a list of laid-out items: measured once per width, painted with a scroll offset, hit-tested for clicks.
#ifndef BRIAREUS_DOC_H
#define BRIAREUS_DOC_H
#include "theme.h"
#include "markdown.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Doc Doc;
typedef struct Item Item;
typedef struct Rich Rich;   // text laid out in runs, for painting and selecting
typedef void (*ItemPaint)(Doc *doc, Item *item, Canvas *cv, const RECT *rc);
typedef void (*ItemFree)(void *data);

struct Item {
    RECT rc;              // in content coordinates
    int action;           // 0 when the item is not clickable
    intptr_t arg;
    ItemPaint paint;
    void *data; ItemFree free_data;
    char *text;
    FontId font; COLORREF color; UINT flags;
    COLORREF fill, border; int radius;
    bool hover_fill;      // a hovered clickable item is tinted
    bool hand;            // hand cursor
    int id;               // a screen's own marker, such as the bottom anchor
    Rich *sel;            // the item's text in runs, relative to rc, when it can be selected
    bool sel_owned;       // `sel` is freed with the item (otherwise it is `data`)
    char *tip;            // shown in a tooltip while the item is hovered; freed with the item
    bool drag;            // a press that moves carries it to the screen's `drag` instead of clicking it (a board's card)
};

/// A character position: an item and an offset into its plain text.
typedef struct { int item, offset; } DocPos;
/// Items that scroll on their own inside a window of the page, as a board's column does. See doc_region.
typedef struct { int first, last; RECT view; int bottom, scroll, max; } DocRegion;

struct Doc {
    Item *items; size_t count, cap;
    Canvas *cv;           // while laying out; NULL, since text is measured without one
    int width;            // content width
    int y;                // the running cursor for stacked items
    int content_width;    // widest item, for horizontal scrolling
    int hover;            // index of the hovered item or -1
    int pressed;
    DocPos sel_anchor, sel_focus;   // the text selection's ends, in either order; item -1 when there is none
    bool selecting;                 // the mouse is dragging the selection
    int sticky_first, sticky_last;  // items that follow the scroll, as CSS `position: sticky`; none when equal
    int sticky_limit, sticky_shift; // the content y they stop at, and how far they are moved now
    int sticky_scroll, sticky_max;  // the group's own scroll inside its window, and how far it can go
    RECT sticky_view;               // the window the group shows through, in content coordinates
    int pin_last, pin_bottom, pin_shift;  // items [0, pin_last) pinned at the view's top over [0, pin_bottom); none when 0
    DocRegion *regions; size_t region_count, region_cap;
    size_t regions_kept;            // regions the last layout had, whose scroll the next one keeps
    int drag_first, drag_last;      // the carried box and the items inside it, painted as an empty slot; none when equal
};

void doc_init(Doc *doc);
void doc_free(Doc *doc);
/// Clears the items and starts a layout at the given width.
void doc_begin(Doc *doc, Canvas *cv, int width);
void doc_end(Doc *doc);
Item *doc_item(Doc *doc, int index);
/// True when `it` is the item under the mouse.
bool doc_item_hovered(const Doc *doc, const Item *it);
int doc_height(const Doc *doc);

/// Adds an item at an explicit rectangle without moving the cursor.
int doc_add(Doc *doc, const RECT *rc, ItemPaint paint);
/// Wrapped text stacked at the cursor; returns the item and advances.
int doc_text(Doc *doc, int x, int w, const char *text, FontId f, COLORREF color, UINT flags);
/// Text at an explicit rectangle.
int doc_text_at(Doc *doc, const RECT *rc, const char *text, FontId f, COLORREF color, UINT flags);
/// Inline Markdown (bold, code, links) wrapped in `w`; advances. Links open on click.
int doc_rich(Doc *doc, int x, int w, const char *inline_markdown, FontId base, COLORREF color);
/// The height inline Markdown would take, without adding it.
int doc_rich_height(Doc *doc, int w, const char *inline_markdown, FontId base);
/// Block Markdown: paragraphs, headings, bullets, quotes, code with a copy button, rules; advances.
void doc_markdown(Doc *doc, int x, int w, const char *source, FontId base);
/// A rounded box opened at the cursor and closed once its content is laid out; `pad` is inside it.
int doc_box_begin(Doc *doc, int x, int w, int pad, COLORREF fill, COLORREF border, int radius);
void doc_box_end(Doc *doc, int box, int pad);
/// Makes a box clickable as a whole (a row).
void doc_box_action(Doc *doc, int box, int action, intptr_t arg);
void doc_space(Doc *doc, int h);
int doc_rule(Doc *doc, int x, int w);
/// A custom-painted item stacked at the cursor.
int doc_custom(Doc *doc, int x, int w, int h, ItemPaint paint, void *data, ItemFree free_data, int action, intptr_t arg);
/// A form field's label in `color`; with `help`, a help glyph after it that shows `help` in a tooltip on hover. Advances.
int doc_field_label(Doc *doc, int x, int w, const char *label, COLORREF color, const char *help);
/// A glyph followed by text, in one colour; advances.
int doc_label(Doc *doc, int x, int w, wchar_t glyph, const char *text, FontId f, COLORREF color);
/// An error notice: warning glyph, danger colour, wrapped; advances.
int doc_notice(Doc *doc, int x, int w, const char *message);
/// A boxed error notice on a tinted background.
int doc_notice_box(Doc *doc, int x, int w, const char *message);
typedef enum { BUTTON_PROMINENT, BUTTON_BORDERED, BUTTON_PLAIN, BUTTON_DESTRUCTIVE } ButtonStyle;
/// A button sized to its text at (x, cursor); `w` 0 fits the text, -1 stretches to the doc width from x. Advances.
int doc_button(Doc *doc, int x, int w, const char *text, ButtonStyle style, int action, intptr_t arg, bool enabled);
/// A small button with a glyph before its text, as the board's errand buttons; each is its own clickable item.
typedef struct { wchar_t glyph; const char *text; ButtonStyle style; int action; intptr_t arg; bool enabled; } ButtonSpec;
/// A line of small buttons laid out left to right, wrapping within `w`; advances. Returns the first item or -1.
int doc_button_row(Doc *doc, int x, int w, const ButtonSpec *buttons, size_t count);
/// A row of small toggle buttons (a segmented picker); `selected` is the chosen index or -1. Advances.
int doc_segments(Doc *doc, int x, int w, const char *const *titles, size_t count, int selected, int action, intptr_t arg_base, bool enabled);
/// A centred progress note ("Loading…"); advances.
int doc_loading(Doc *doc, int x, int w, const char *text);
/// A tall empty-state note with a glyph; advances.
int doc_empty_state(Doc *doc, int x, int w, wchar_t glyph, const char *title, const char *detail);
/// A "Label: value" row with the value on the right; advances.
int doc_labeled(Doc *doc, int x, int w, const char *label, const char *value, COLORREF value_color);
/// A line of wrapped badges and chips from a list, laid out left to right; advances. See BadgeSpec.
typedef struct { wchar_t glyph; const char *text; COLORREF color; bool chip; } BadgeSpec;
int doc_badges(Doc *doc, int x, int w, const BadgeSpec *badges, size_t count, COLORREF background);
/// The width a line of badges takes, for text laid beside them.
int doc_badges_width(Doc *doc, const BadgeSpec *badges, size_t count);
/// A section header in small caps style, as an inset grouped list has; advances.
int doc_section(Doc *doc, int x, int w, const char *title);

/// Items [first, last) follow the scroll down to `limit`, as a sidebar beside a long column: they stay in view, and
/// one taller than the view scrolls on its own inside a window the view's height, as GitHub's file tree does. The page
/// needs room for that window: `limit` at least the group's top plus the view's height less 24px. One group per
/// document; its own scroll survives layouts.
void doc_sticky(Doc *doc, int first, int last, int limit);
/// Items [0, last), laid out above `bottom`, stay at the top of the view as a fixed header: the rest scrolls beneath
/// them, clipped at `bottom`, and a sticky group's window starts below them. One band per document.
void doc_pin(Doc *doc, int last, int bottom);
/// Items [first, last) scroll on their own inside `view`, in content coordinates, as a board's column does: clipped to
/// it, with a thin bar at its right edge while they overflow. `bottom` is the content y they end at, padding included.
/// A region keeps its scroll through layouts that lay it out at the same index; their rectangles move with it.
void doc_region(Doc *doc, int first, int last, const RECT *view, int bottom);
/// Scrolls the region under a content point by `dy`; false when there is none or it does not overflow.
bool doc_region_wheel(Doc *doc, int x, int y, int dy);
/// The region whose bar is under a content point, or -1; `thumb_top` is the bar's thumb top, for dragging it.
int doc_region_thumb_at(Doc *doc, int x, int y, int *thumb_top);
/// Scrolls region `r` so its thumb's top is at content y `thumb_top`, as a drag of the bar moves it.
void doc_region_drag(Doc *doc, int r, int thumb_top);
/// Moves the pinned and sticky items for the scroll offset and the visible height; their rectangles stay in content coordinates.
void doc_set_view(Doc *doc, int scroll_y, int view_height);
/// Scrolls the sticky group by `dy` when the content point is over its window and it overflows; false otherwise.
bool doc_sticky_wheel(Doc *doc, int x, int y, int dy);

/// Starts carrying `box` (a doc_box_begin item) with the items inside it: its place shows an empty slot until the
/// next layout or doc_drag(doc, -1).
void doc_drag(Doc *doc, int box);
/// Paints the carried box and what is inside it with its top left at (x, y), in the canvas's coordinates.
void doc_paint_dragged(Doc *doc, Canvas *cv, int x, int y);
void doc_paint(Doc *doc, Canvas *cv, int scroll_x, int scroll_y, const RECT *clip);
/// The topmost clickable item at a content point, or -1.
int doc_hit(Doc *doc, int x, int y);
/// The URL under a content point inside a rich item, or NULL.
const char *doc_link_at(Doc *doc, int index, int x, int y);
/// Plain text of an item, for copying; NULL when it has none.
char *doc_item_plain_text(Doc *doc, int index);
/// The first item with this id, or -1.
int doc_find(Doc *doc, int id);

// Selection: text items are selected with the mouse across items, as in a browser. The selection survives a layout at the
// same items; the pane clears it when a screen changes.
/// The text position nearest a content point; false when the document has no text.
bool doc_position_at(Doc *doc, Canvas *cv, int x, int y, DocPos *pos);
/// The selectable text item under a content point, or -1.
int doc_text_item_at(Doc *doc, int x, int y);
bool doc_has_selection(Doc *doc);
void doc_clear_selection(Doc *doc);
void doc_select_all(Doc *doc);
/// Selects the word around a position.
void doc_select_word(Doc *doc, DocPos pos);
/// The selected text as UTF-8, items on separate lines; NULL when nothing is selected.
char *doc_selection_text(Doc *doc);

/// Standard actions items may carry; screens use values from 1000 up.
enum { ACTION_NONE = 0, ACTION_OPEN_LINK = 1, ACTION_COPY_CODE = 2, ACTION_TIP = 3 };   // ACTION_TIP: hovered for its tip, a click does nothing

#endif
