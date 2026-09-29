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
typedef void (*ItemPaint)(Doc *doc, Item *item, HDC hdc, const RECT *rc);
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
};

struct Doc {
    Item *items; size_t count, cap;
    HDC hdc;              // the measuring DC while laying out
    int width;            // content width
    int y;                // the running cursor for stacked items
    int content_width;    // widest item, for horizontal scrolling
    int hover;            // index of the hovered item or -1
    int pressed;
};

void doc_init(Doc *doc);
void doc_free(Doc *doc);
/// Clears the items and starts a layout at the given width.
void doc_begin(Doc *doc, HDC hdc, int width);
void doc_end(Doc *doc);
Item *doc_item(Doc *doc, int index);
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
/// A glyph followed by text, in one colour; advances.
int doc_label(Doc *doc, int x, int w, wchar_t glyph, const char *text, FontId f, COLORREF color);
/// An error notice: warning glyph, danger colour, wrapped; advances.
int doc_notice(Doc *doc, int x, int w, const char *message);
/// A boxed error notice on a tinted background.
int doc_notice_box(Doc *doc, int x, int w, const char *message);
typedef enum { BUTTON_PROMINENT, BUTTON_BORDERED, BUTTON_PLAIN, BUTTON_DESTRUCTIVE } ButtonStyle;
/// A button sized to its text at (x, cursor); `w` 0 fits the text, -1 stretches to the doc width from x. Advances.
int doc_button(Doc *doc, int x, int w, const char *text, ButtonStyle style, int action, intptr_t arg, bool enabled);
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
/// A section header in small caps style, as an inset grouped list has; advances.
int doc_section(Doc *doc, int x, int w, const char *title);

void doc_paint(Doc *doc, HDC hdc, int scroll_x, int scroll_y, const RECT *clip);
/// The topmost clickable item at a content point, or -1.
int doc_hit(Doc *doc, int x, int y);
/// The URL under a content point inside a rich item, or NULL.
const char *doc_link_at(Doc *doc, int index, int x, int y);
/// Plain text of an item, for copying; NULL when it has none.
char *doc_item_plain_text(Doc *doc, int index);
/// The first item with this id, or -1.
int doc_find(Doc *doc, int id);

/// Standard actions items may carry; screens use values from 1000 up.
enum { ACTION_NONE = 0, ACTION_OPEN_LINK = 1, ACTION_COPY_CODE = 2 };

#endif
