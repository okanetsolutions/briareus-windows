#include "theme.h"
#include "canvas.h"
#include "board.h"
#include "str.h"
#include <dwmapi.h>
#include <math.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uxtheme.h>

Palette theme;
static HFONT fonts[FONT_COUNT];
static int edit_heights[FONT_COUNT];   // edit_line_height's answers, measured once per font
static int current_dpi = 96;

static COLORREF rgb_hex(unsigned hex) { return RGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF); }

/// Windows' "Choose your app mode": dark unless the user picked light (or the value is missing, as before Windows 10).
static bool system_prefers_dark(void) {
    DWORD light = 0, size = sizeof light;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, NULL, &light, &size) != ERROR_SUCCESS) return true;
    return light == 0;
}

bool theme_refresh(void) {
    bool dark = system_prefers_dark();
    bool changed = !theme.canvas || dark != theme.dark;
    theme.dark = dark;
    if (dark) {
        // The dashboard's `@theme` block, verbatim.
        theme.canvas = rgb_hex(0x262624);
        theme.sidebar = rgb_hex(0x1F1E1D);
        theme.raise = rgb_hex(0x30302E);
        theme.field = rgb_hex(0x3D3D3A);
        theme.sunken = rgb_hex(0x1C1C1A);
        theme.line = rgb_hex(0x3E3E3A);
        theme.line_strong = rgb_hex(0x5A5850);
        theme.ink = rgb_hex(0xE8E6E1);
        theme.muted = rgb_hex(0xA29E93);
        theme.accent = rgb_hex(0xD97757);
        theme.accent_dim = rgb_hex(0xB35C3E);
        theme.ok = rgb_hex(0x7FBF7F);
        theme.warn = rgb_hex(0xE0AF68);
        theme.danger = rgb_hex(0xE06C75);
        theme.on_accent = rgb_hex(0x1B1B19);
        theme.dot = rgb_hex(0x6B6862);
        theme.thumb = rgb_hex(0x3C3B38);
    } else {
        // The dashboard has no light mode; this is the same warm palette on paper, with the accents darkened to read on it.
        theme.canvas = rgb_hex(0xFAF9F5);
        theme.sidebar = rgb_hex(0xF0EEE6);
        theme.raise = rgb_hex(0xFFFFFF);
        theme.field = rgb_hex(0xE9E6DC);
        theme.sunken = rgb_hex(0xF3F1EA);
        theme.line = rgb_hex(0xE0DDD3);
        theme.line_strong = rgb_hex(0xC4C0B4);
        theme.ink = rgb_hex(0x1F1E1D);
        theme.muted = rgb_hex(0x6B675E);
        theme.accent = rgb_hex(0xC96442);
        theme.accent_dim = rgb_hex(0xE0A58E);
        theme.ok = rgb_hex(0x3D8B4A);
        theme.warn = rgb_hex(0xA86E12);
        theme.danger = rgb_hex(0xC2404B);
        theme.on_accent = rgb_hex(0xFFFFFF);
        theme.dot = rgb_hex(0xA29E93);
        theme.thumb = rgb_hex(0xCFCBC0);
    }
    theme.background = theme.canvas;
    theme.surface = theme.raise; theme.elevated = theme.raise; theme.bubble = theme.raise;
    theme.border = theme.line; theme.code = theme.sunken;
    theme.success = theme.ok; theme.warning = theme.warn;
    theme.text = theme.ink; theme.secondary = theme.muted;
    theme.tertiary = blend(theme.muted, theme.canvas, 0.7);   // `text-muted/70`
    theme.white = RGB(255, 255, 255);
    return changed;
}

static HFONT make_font(const wchar_t *face, int pixels, int weight, bool italic) {
    LOGFONTW lf; memset(&lf, 0, sizeof lf);
    lf.lfHeight = -MulDiv(pixels, current_dpi, 96);
    lf.lfWeight = weight; lf.lfItalic = italic;
    lf.lfCharSet = DEFAULT_CHARSET; lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}
/// A font for DirectWrite, which draws everything, and for GDI, which the Edit controls still use.
static void set_font(FontId id, const wchar_t *face, int pixels, int weight, bool italic) {
    fonts[id] = make_font(face, pixels, weight, italic);
    canvas_set_font(id, face, pixels * current_dpi / 96.0f, weight, italic);
}

static bool font_exists(const wchar_t *face) {
    HDC hdc = GetDC(NULL);
    HFONT f = make_font(face, 13, FW_NORMAL, false);
    HFONT old = SelectObject(hdc, f);
    wchar_t actual[LF_FACESIZE] = L"";
    GetTextFaceW(hdc, LF_FACESIZE, actual);
    SelectObject(hdc, old); DeleteObject(f); ReleaseDC(NULL, hdc);
    return _wcsicmp(actual, face) == 0;
}

void theme_set_dpi(int dpi) {
    current_dpi = dpi > 0 ? dpi : 96;
    for (int i = 0; i < FONT_COUNT; i++) if (fonts[i]) { DeleteObject(fonts[i]); fonts[i] = NULL; }
    memset(edit_heights, 0, sizeof edit_heights);
    // The dashboard's type at its own pixel sizes: `--font-sans` is Segoe UI on Windows and `--font-mono` Cascadia Code.
    // Body text is 15px (`text-sm`), the smallest chrome 13px (`text-xs`), the sidebar rows 14px, metadata 12px and 11px.
    const wchar_t *ui = L"Segoe UI";
    const wchar_t *mono = font_exists(L"Cascadia Code") ? L"Cascadia Code" : font_exists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    const wchar_t *icons = font_exists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const wchar_t *emoji = font_exists(L"Segoe UI Emoji") ? L"Segoe UI Emoji" : L"Segoe UI Symbol";
    set_font(FONT_BODY, ui, 15, FW_NORMAL, false);
    set_font(FONT_BODY_MEDIUM, ui, 15, FW_MEDIUM, false);
    set_font(FONT_BODY_SEMIBOLD, ui, 15, FW_SEMIBOLD, false);
    set_font(FONT_HEADLINE, ui, 15, FW_SEMIBOLD, false);
    set_font(FONT_SUBHEADLINE, ui, 14, FW_NORMAL, false);
    set_font(FONT_SUBHEADLINE_SEMIBOLD, ui, 14, FW_SEMIBOLD, false);
    set_font(FONT_TITLE3, ui, 17, FW_SEMIBOLD, false);
    set_font(FONT_LARGE_TITLE, ui, 26, FW_SEMIBOLD, false);
    set_font(FONT_CALLOUT, ui, 14, FW_NORMAL, false);
    set_font(FONT_FOOTNOTE, ui, 13, FW_NORMAL, false);
    set_font(FONT_CAPTION, ui, 12, FW_NORMAL, false);
    set_font(FONT_CAPTION_MEDIUM, ui, 12, FW_MEDIUM, false);
    set_font(FONT_CAPTION_SEMIBOLD, ui, 12, FW_SEMIBOLD, false);
    set_font(FONT_CAPTION2, ui, 11, FW_NORMAL, false);
    set_font(FONT_BODY_ITALIC, ui, 15, FW_NORMAL, true);
    set_font(FONT_CALLOUT_ITALIC, ui, 14, FW_NORMAL, true);
    set_font(FONT_FOOTNOTE_SEMIBOLD, ui, 13, FW_SEMIBOLD, false);
    set_font(FONT_SUBHEADLINE_ITALIC, ui, 14, FW_NORMAL, true);
    set_font(FONT_MONO, mono, 13, FW_NORMAL, false);
    set_font(FONT_MONO_SMALL, mono, 12, FW_NORMAL, false);
    set_font(FONT_MONO_CAPTION2, mono, 11, FW_NORMAL, false);
    set_font(FONT_ICON, icons, 14, FW_NORMAL, false);
    set_font(FONT_ICON_SMALL, icons, 12, FW_NORMAL, false);
    set_font(FONT_ICON_LARGE, icons, 20, FW_NORMAL, false);
    set_font(FONT_ICON_HUGE, icons, 34, FW_NORMAL, false);
    set_font(FONT_SERIF_MONOGRAM, ui, 17, FW_SEMIBOLD, false);
    set_font(FONT_TINY_SEMIBOLD, ui, 10, FW_SEMIBOLD, false);
    set_font(FONT_TITLE, ui, 23, FW_SEMIBOLD, false);
    set_font(FONT_STAT, ui, 22, FW_SEMIBOLD, false);
    set_font(FONT_EMOJI, emoji, 13, FW_NORMAL, false);
    set_font(FONT_EMOJI_LARGE, emoji, 15, FW_NORMAL, false);
    set_font(FONT_EMOJI_HUGE, emoji, 26, FW_NORMAL, false);
}

void theme_init(void) {
    canvas_startup();
    theme_refresh(); theme_set_dpi(96);
}
int theme_dpi(void) { return current_dpi; }
int px(int units) { return MulDiv(units, current_dpi, 96); }
HFONT font(FontId id) { return fonts[id]; }
int edit_line_height(FontId id) {
    if (!edit_heights[id] && fonts[id]) {
        HDC hdc = GetDC(NULL);
        HFONT old = SelectObject(hdc, fonts[id]);
        TEXTMETRICW tm;
        if (GetTextMetricsW(hdc, &tm)) edit_heights[id] = tm.tmHeight;
        SelectObject(hdc, old); ReleaseDC(NULL, hdc);
    }
    int dw = font_height(NULL, id);
    return edit_heights[id] > dw ? edit_heights[id] : dw;
}

COLORREF theme_status_color(const char *status) {
    if (str_eq(status, "running") || str_eq(status, "queued") || str_eq(status, "preparing") || str_eq(status, "starting")) return theme.accent;
    if (str_eq(status, "idle")) return theme.ok;
    if (str_eq(status, "waiting") || str_eq(status, "interrupted") || str_eq(status, "cancelled")) return theme.warn;
    if (str_eq(status, "failed") || str_eq(status, "error")) return theme.danger;
    return theme.dot;
}

COLORREF blend(COLORREF color, COLORREF background, double alpha) {
    if (alpha < 0) alpha = 0;
    if (alpha > 1) alpha = 1;
    int r = (int)lround(GetRValue(color) * alpha + GetRValue(background) * (1 - alpha));
    int g = (int)lround(GetGValue(color) * alpha + GetGValue(background) * (1 - alpha));
    int b = (int)lround(GetBValue(color) * alpha + GetBValue(background) * (1 - alpha));
    return RGB(r, g, b);
}

// MARK: - Drawing
// The primitives (rectangles, curves, lines, text) are Direct2D's, in canvas.cpp; these are the dashboard's pieces built on them.

int draw_text(Canvas *cv, const char *text, RECT *rc, FontId f, COLORREF color, UINT flags) {
    wchar_t *w = utf8_to_wide(text ? text : "");
    int h = draw_textw(cv, w, rc, f, color, flags);
    free(w);
    return h;
}
void draw_glyph(Canvas *cv, wchar_t glyph, const RECT *rc, FontId f, COLORREF color) {
    wchar_t text[2] = { glyph, 0 };
    RECT r = *rc;
    draw_textw(cv, text, &r, f, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void draw_status_dot(Canvas *cv, int cx, int cy, const char *status) {
    COLORREF color = theme_status_color(status);
    int r = px(7) / 2;
    fill_circle(cv, cx, cy, r, color);
}
int draw_badge(Canvas *cv, int x, int y, wchar_t glyph, const char *text, COLORREF color, COLORREF background, int *height) {
    int h = font_height(cv, FONT_CAPTION2) + px(4);
    int glyph_w = glyph ? px(11) : 0;
    int text_w = text_width(cv, text, FONT_CAPTION2);
    int w = px(6) * 2 + glyph_w + (glyph && text && *text ? px(3) : 0) + text_w + 2;
    if (cv) {
        RECT rc = { x, y, x + w, y + h };
        fill_round_rect(cv, &rc, px(5), background, color == theme.muted ? theme.line : color);
        int cx = x + px(6);
        if (glyph) { RECT g = { cx, y, cx + glyph_w, y + h }; draw_glyph(cv, glyph, &g, FONT_ICON_SMALL, color); cx += glyph_w + px(3); }
        RECT t = { cx, y, cx + text_w + 2, y + h };
        draw_text(cv, text, &t, FONT_CAPTION2, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (height) *height = h;
    return w;
}
int draw_chip(Canvas *cv, int x, int y, const char *name, COLORREF color, COLORREF background, int *height) {
    int h = font_height(cv, FONT_CAPTION2) + px(4);
    int text_w = text_width(cv, name, FONT_CAPTION2);
    int w = px(6) * 2 + text_w + 2;
    if (cv) {
        RECT rc = { x, y, x + w, y + h };
        fill_round_rect(cv, &rc, h / 2, background, color);
        RECT t = { x + px(6), y, x + w - px(4), y + h };
        draw_text(cv, name, &t, FONT_CAPTION2, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (height) *height = h;
    return w;
}
void draw_monogram(Canvas *cv, int x, int y, int size, const char *text) {
    const char *slash = text ? strrchr(text, '/') : NULL;
    const char *start = slash && slash[1] ? slash + 1 : (text ? text : "?");
    wchar_t *w = utf8_to_wide(start);
    wchar_t letter[2] = { w[0] ? towupper(w[0]) : L'?', 0 };
    free(w);
    RECT rc = { x, y, x + size, y + size };
    COLORREF fill = blend(theme.accent, theme.background, 0.15);
    fill_round_rect(cv, &rc, size * 28 / 100, fill, fill);
    draw_textw(cv, letter, &rc, FONT_SERIF_MONOGRAM, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static const wchar_t *const glyphs[] = { L"·", L"✢", L"✳", L"✶", L"✻", L"✽", L"✻", L"✶", L"✳", L"✢" };
static const char *const verbs[] = { "Working", "Thinking", "Reasoning", "Tinkering", "Crafting", "Pondering" };
const wchar_t *working_glyph(int tick) { return glyphs[((tick % 10) + 10) % 10]; }
const char *working_verb(int tick) { return verbs[((tick / 25) % 6 + 6) % 6]; }

// MARK: - Formatting

static void to_local(time_t when, SYSTEMTIME *out) {
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)when * 10000000ULL + 116444736000000000ULL;
    FILETIME ft; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME utc; FileTimeToSystemTime(&ft, &utc);
    // The offset of that date, not today's: daylight saving moves it.
    DYNAMIC_TIME_ZONE_INFORMATION tz; GetDynamicTimeZoneInformation(&tz);
    if (!SystemTimeToTzSpecificLocalTimeEx(&tz, &utc, out)) *out = utc;
}
static char *time_part(const SYSTEMTIME *st) {
    wchar_t buf[64];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, st, NULL, buf, 64);
    return wide_to_utf8(buf);
}
char *format_event_time(time_t when) {
    SYSTEMTIME st, now_st; to_local(when, &st); to_local(time(NULL), &now_st);
    char *clock = time_part(&st);
    if (st.wYear == now_st.wYear && st.wMonth == now_st.wMonth && st.wDay == now_st.wDay) return clock;
    wchar_t day[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"MMM d", day, 64, NULL);
    char *d = wide_to_utf8(day);
    char *out = xstrfmt("%s, %s", d, clock);
    free(d); free(clock);
    return out;
}
char *format_relative(time_t when) {
    double diff = difftime(time(NULL), when);
    const char *suffix = diff >= 0 ? " ago" : "";
    double d = fabs(diff);
    if (d < 60) return xstrdup("now");
    if (d < 3600) return xstrfmt("%dm%s", (int)(d / 60), suffix);
    if (d < 86400) return xstrfmt("%dh%s", (int)(d / 3600), suffix);
    if (d < 7 * 86400) return xstrfmt("%dd%s", (int)(d / 86400), suffix);
    if (d < 30 * 86400) return xstrfmt("%dw%s", (int)(d / (7 * 86400)), suffix);
    if (d < 365 * 86400) return xstrfmt("%dmo%s", (int)(d / (30 * 86400)), suffix);
    return xstrfmt("%dy%s", (int)(d / (365 * 86400)), suffix);
}
char *format_duration_ms(double ms) {
    long long total = (long long)(ms / 1000);
    if (total < 0) total = 0;
    long long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h) return xstrfmt("%lldh %lldm", h, m);
    if (m) return xstrfmt("%lldm %llds", m, s);
    return xstrfmt("%llds", s);
}
char *format_date_abbrev(time_t when) {
    SYSTEMTIME st; to_local(when, &st);
    wchar_t buf[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"MMM d, yyyy", buf, 64, NULL);
    return wide_to_utf8(buf);
}
char *format_clock(int seconds) { if (seconds < 0) seconds = 0; return xstrfmt("%d:%02d", seconds / 60, seconds % 60); }
char *format_tokens(double n) {
    // From where one decimal rounds up to 1000 of the smaller unit.
    if (n >= 999.95e6) return xstrfmt("%.1fB", n / 1e9);
    if (n >= 999.95e3) return xstrfmt("%.1fM", n / 1e6);
    if (n >= 1e3) return xstrfmt("%.1fk", n / 1e3);
    return xstrfmt("%d", (int)n);
}
char *format_cost(double usd) { return xstrfmt("$%.2f", usd); }

void open_web_url(const char *url) {
    if (!safe_web_url(url)) return;
    wchar_t *w = utf8_to_wide(url);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}
void copy_to_clipboard(HWND owner, const char *text) {
    wchar_t *w = utf8_to_wide(text ? text : "");
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *p = GlobalLock(mem);
        if (p) { memcpy(p, w, bytes); GlobalUnlock(mem); }
        if (OpenClipboard(owner)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, mem); CloseClipboard(); }
        else GlobalFree(mem);
    }
    free(w);
}
void theme_apply_window(HWND hwnd) {
    BOOL dark = theme.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF caption = theme.sidebar;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    COLORREF text = theme.ink;
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof text);
}
void theme_apply_control(HWND hwnd) {
    SetWindowTheme(hwnd, theme.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
}
