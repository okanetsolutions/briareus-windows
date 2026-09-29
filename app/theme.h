// Warm neutrals and a clay accent, in the spirit of the Claude apps; fonts, DPI and the GDI helpers the screens share.
#ifndef BRIAREUS_THEME_H
#define BRIAREUS_THEME_H
#include <windows.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    COLORREF accent, background, surface, elevated, bubble, border, code, success, danger, warning;
    COLORREF text, secondary, tertiary, white;
    bool dark;
} Palette;

typedef enum {
    FONT_BODY, FONT_BODY_MEDIUM, FONT_BODY_SEMIBOLD, FONT_HEADLINE, FONT_SUBHEADLINE, FONT_SUBHEADLINE_SEMIBOLD, FONT_TITLE3,
    FONT_LARGE_TITLE, FONT_CALLOUT, FONT_FOOTNOTE, FONT_CAPTION, FONT_CAPTION_MEDIUM, FONT_CAPTION_SEMIBOLD, FONT_CAPTION2,
    FONT_BODY_ITALIC, FONT_CALLOUT_ITALIC, FONT_FOOTNOTE_SEMIBOLD, FONT_SUBHEADLINE_ITALIC,
    FONT_MONO, FONT_MONO_SMALL, FONT_MONO_CAPTION2, FONT_ICON, FONT_ICON_SMALL, FONT_ICON_LARGE, FONT_ICON_HUGE, FONT_SERIF_MONOGRAM,
    FONT_COUNT
} FontId;

extern Palette theme;

void theme_init(void);
/// Reads the system's light or dark preference again and rebuilds the palette.
void theme_refresh(void);
/// Rebuilds the fonts for a DPI; the app has one DPI at a time, the main window's.
void theme_set_dpi(int dpi);
int theme_dpi(void);
/// Scales a 96-DPI length.
int px(int units);
HFONT font(FontId id);
/// The status colours the dashboard uses: running green, queued amber, failed red.
COLORREF theme_status_color(const char *status);
/// `color` at `alpha` (0...1) over `background`, since GDI paints opaque.
COLORREF blend(COLORREF color, COLORREF background, double alpha);

// Drawing
void fill_rect(HDC hdc, const RECT *rc, COLORREF color);
void fill_round_rect(HDC hdc, const RECT *rc, int radius, COLORREF fill, COLORREF border);
void fill_circle(HDC hdc, int cx, int cy, int radius, COLORREF fill);
void stroke_circle(HDC hdc, int cx, int cy, int radius, COLORREF color, int width);
void draw_line(HDC hdc, int x1, int y1, int x2, int y2, COLORREF color);
/// Draws UTF-8 text; DT_ flags as for DrawText. Returns the height drawn.
int draw_text(HDC hdc, const char *text, RECT *rc, FontId f, COLORREF color, UINT flags);
int draw_textw(HDC hdc, const wchar_t *text, RECT *rc, FontId f, COLORREF color, UINT flags);
/// Height the text needs at `width`, wrapped.
int measure_text(HDC hdc, const char *text, int width, FontId f, UINT flags);
int text_width(HDC hdc, const char *text, FontId f);
int textw_width(HDC hdc, const wchar_t *text, FontId f);
int font_height(HDC hdc, FontId f);
/// A Segoe MDL2 Assets glyph centred in a rectangle.
void draw_glyph(HDC hdc, wchar_t glyph, const RECT *rc, FontId f, COLORREF color);
void draw_status_dot(HDC hdc, int cx, int cy, const char *status);
/// A capsule with a glyph and short text; returns its width. Pass a NULL hdc to measure only.
int draw_badge(HDC hdc, int x, int y, wchar_t glyph, const char *text, COLORREF color, COLORREF background, int *height);
/// A label chip: a colour dot and the name; returns its width.
int draw_chip(HDC hdc, int x, int y, const char *name, COLORREF color, COLORREF background, int *height);
/// The square monogram used for projects.
void draw_monogram(HDC hdc, int x, int y, int size, const char *text);
/// The Claude-style working glyph for a tick.
const wchar_t *working_glyph(int tick);
const char *working_verb(int tick);

// Formatting
/// When a message was logged: the time alone for today's, with its day for an older one.
char *format_event_time(time_t when);
char *format_relative(time_t when);
char *format_duration_ms(double ms);
char *format_date_abbrev(time_t when);
char *format_clock(int seconds);

/// Opens an https URL in the default browser; anything else is refused.
void open_web_url(const char *url);
void copy_to_clipboard(HWND owner, const char *text);
/// A dark title bar when the theme is dark.
void theme_apply_window(HWND hwnd);
/// Dark scrollbars and controls where Windows offers them.
void theme_apply_control(HWND hwnd);

#endif
