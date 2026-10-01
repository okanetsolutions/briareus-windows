// The dashboard's own look, dark only: its Tailwind palette (canvas, sidebar, raise, field, sunken, line, ink, muted,
// accent), its pixel sizes for Segoe UI and Cascadia Code, and the drawing helpers the screens share (see canvas.h).
#ifndef BRIAREUS_THEME_H
#define BRIAREUS_THEME_H
#include <windows.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // The dashboard's colour names.
    COLORREF canvas, sidebar, raise, field, sunken, line, line_strong, ink, muted, accent, accent_dim, ok, warn, danger, on_accent, dot;
    // The same colours under the names the screens grew up with.
    COLORREF background, surface, elevated, bubble, border, code, success, warning, text, secondary, tertiary, white;
    bool dark;
} Palette;

typedef enum {
    FONT_BODY, FONT_BODY_MEDIUM, FONT_BODY_SEMIBOLD, FONT_HEADLINE, FONT_SUBHEADLINE, FONT_SUBHEADLINE_SEMIBOLD, FONT_TITLE3,
    FONT_LARGE_TITLE, FONT_CALLOUT, FONT_FOOTNOTE, FONT_CAPTION, FONT_CAPTION_MEDIUM, FONT_CAPTION_SEMIBOLD, FONT_CAPTION2,
    FONT_BODY_ITALIC, FONT_CALLOUT_ITALIC, FONT_FOOTNOTE_SEMIBOLD, FONT_SUBHEADLINE_ITALIC,
    FONT_MONO, FONT_MONO_SMALL, FONT_MONO_CAPTION2, FONT_ICON, FONT_ICON_SMALL, FONT_ICON_LARGE, FONT_ICON_HUGE, FONT_SERIF_MONOGRAM,
    FONT_TINY_SEMIBOLD,   // 10px: severity tags, badge counts
    FONT_TITLE,           // 23px semibold: a pull request's title
    FONT_STAT,            // 22px semibold: dashboard tiles
    FONT_EMOJI, FONT_EMOJI_LARGE, FONT_EMOJI_HUGE,   // the dashboard's glyphs (📊 🏙 ⚑ ✎ ⚡ ▶ …)
    FONT_COUNT
} FontId;

extern Palette theme;
/// A surface to draw on: a pane's window, or a GDI paint borrowed for a moment (canvas.h).
typedef struct Canvas Canvas;

void theme_init(void);
/// The dashboard is dark whatever the system prefers; kept for the callers that ask.
void theme_refresh(void);
/// Rebuilds the fonts for a DPI; the app has one DPI at a time, the main window's.
void theme_set_dpi(int dpi);
int theme_dpi(void);
/// Scales a 96-DPI length.
int px(int units);
/// The GDI font, for the Edit controls that draw their own text.
HFONT font(FontId id);
/// The `.dot` colours: running/queued in the accent, idle green, waiting amber, failed red, the rest grey.
COLORREF theme_status_color(const char *status);
/// `color` at `alpha` (0...1) over `background`, as the dashboard's translucent tints come out on its opaque surfaces.
COLORREF blend(COLORREF color, COLORREF background, double alpha);

// Drawing
void fill_rect(Canvas *cv, const RECT *rc, COLORREF color);
void fill_round_rect(Canvas *cv, const RECT *rc, int radius, COLORREF fill, COLORREF border);
void fill_circle(Canvas *cv, int cx, int cy, int radius, COLORREF fill);
void stroke_circle(Canvas *cv, int cx, int cy, int radius, COLORREF color, int width);
/// An arc of a ring centred on the stroke: degrees clockwise from three o'clock.
void stroke_arc(Canvas *cv, int cx, int cy, int radius, COLORREF color, int width, double start, double sweep);
void draw_line(Canvas *cv, int x1, int y1, int x2, int y2, COLORREF color);
void draw_dashed_line(Canvas *cv, int x1, int y1, int x2, int y2, COLORREF color);
/// A line `width` pixels thick with round ends, through the centres of its end pixels.
void draw_thick_line(Canvas *cv, int x1, int y1, int x2, int y2, COLORREF color, int width);
/// A checked checkbox's tick, drawn in the middle of its box.
void draw_check_mark(Canvas *cv, const RECT *rc, COLORREF color);
/// A 1px dotted outline of a rounded rectangle.
void stroke_dotted_round_rect(Canvas *cv, const RECT *rc, int radius, COLORREF color);
/// Draws UTF-8 text; DT_ flags as for DrawText (alignment, single line, word break, ellipses, no clip). Returns the height drawn.
int draw_text(Canvas *cv, const char *text, RECT *rc, FontId f, COLORREF color, UINT flags);
int draw_textw(Canvas *cv, const wchar_t *text, RECT *rc, FontId f, COLORREF color, UINT flags);
/// Height the text needs at `width`, wrapped.
int measure_text(Canvas *cv, const char *text, int width, FontId f, UINT flags);
int text_width(Canvas *cv, const char *text, FontId f);
int textw_width(Canvas *cv, const wchar_t *text, FontId f);
int font_height(Canvas *cv, FontId f);
/// Width of the first `len` characters, measured by DirectWrite; no canvas is needed to measure.
int textw_extent(FontId f, const wchar_t *text, size_t len);
/// How many of the first `len` characters fit in `width`; a cluster (a surrogate pair, an accent) is never split.
size_t textw_fit(FontId f, const wchar_t *text, size_t len, int width);
/// A Segoe Fluent Icons glyph centred in a rectangle.
void draw_glyph(Canvas *cv, wchar_t glyph, const RECT *rc, FontId f, COLORREF color);
/// The dashboard's 7px `.dot`.
void draw_status_dot(Canvas *cv, int cx, int cy, const char *status);
/// A small bordered tag (`rounded-[5px] border px-1.5 text-[11px] font-semibold`) in a colour; returns its width. A NULL canvas measures.
int draw_badge(Canvas *cv, int x, int y, wchar_t glyph, const char *text, COLORREF color, COLORREF background, int *height);
/// A GitHub label as the board draws it: `rounded-full border px-1.5 text-[11px]` in the label's colour; returns its width.
int draw_chip(Canvas *cv, int x, int y, const char *name, COLORREF color, COLORREF background, int *height);
/// The square monogram used for projects.
void draw_monogram(Canvas *cv, int x, int y, int size, const char *text);
/// The Claude-style working glyph for a tick.
const wchar_t *working_glyph(int tick);
const char *working_verb(int tick);

// Formatting
/// When a message was logged, as the dashboard prints it: the clock time.
char *format_event_time(time_t when);
char *format_relative(time_t when);
char *format_duration_ms(double ms);
char *format_date_abbrev(time_t when);
char *format_clock(int seconds);
/// "19.1M", "839.1M", "42.9k": a token count as the dashboard abbreviates it.
char *format_tokens(double n);
/// "$33.35": a cost.
char *format_cost(double usd);

/// Opens an https URL in the default browser; anything else is refused.
void open_web_url(const char *url);
void copy_to_clipboard(HWND owner, const char *text);
/// A dark title bar.
void theme_apply_window(HWND hwnd);
/// Dark scrollbars and controls where Windows offers them.
void theme_apply_control(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif
