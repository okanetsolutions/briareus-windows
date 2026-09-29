#include "theme.h"
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
static int current_dpi = 96;

static COLORREF rgb_hex(unsigned hex) { return RGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF); }

static bool system_prefers_dark(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD value = 1, size = sizeof value, type = 0;
    LSTATUS status = RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&value, &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD && value == 0;
}

void theme_refresh(void) {
    bool dark = system_prefers_dark();
    theme.dark = dark;
    theme.accent = dark ? rgb_hex(0xD97757) : rgb_hex(0xC96442);
    theme.background = dark ? rgb_hex(0x1F1E1D) : rgb_hex(0xFAF9F5);
    theme.surface = dark ? rgb_hex(0x2A2927) : rgb_hex(0xF0EEE6);
    theme.elevated = dark ? rgb_hex(0x302F2C) : rgb_hex(0xFFFFFF);
    theme.bubble = dark ? rgb_hex(0x3A3936) : rgb_hex(0xEAE7DD);
    theme.border = dark ? rgb_hex(0x3E3D39) : rgb_hex(0xDEDBD0);
    theme.code = dark ? rgb_hex(0x262523) : rgb_hex(0xF3F1EA);
    theme.success = dark ? rgb_hex(0x6FBF8B) : rgb_hex(0x3D8C5A);
    theme.danger = dark ? rgb_hex(0xE5776A) : rgb_hex(0xC0392B);
    theme.warning = dark ? rgb_hex(0xE3B25C) : rgb_hex(0xB7791F);
    theme.text = dark ? rgb_hex(0xECEAE3) : rgb_hex(0x28271F);
    theme.secondary = dark ? rgb_hex(0xA39F95) : rgb_hex(0x6F6B62);
    theme.tertiary = dark ? rgb_hex(0x767268) : rgb_hex(0xA29E94);
    theme.white = RGB(255, 255, 255);
}

static HFONT make_font(const wchar_t *face, double points, int weight, bool italic) {
    LOGFONTW lf; memset(&lf, 0, sizeof lf);
    lf.lfHeight = -(LONG)lround(points * current_dpi / 72.0);
    lf.lfWeight = weight; lf.lfItalic = italic;
    lf.lfCharSet = DEFAULT_CHARSET; lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

static bool font_exists(const wchar_t *face) {
    HDC hdc = GetDC(NULL);
    HFONT f = make_font(face, 10, FW_NORMAL, false);
    HFONT old = SelectObject(hdc, f);
    wchar_t actual[LF_FACESIZE] = L"";
    GetTextFaceW(hdc, LF_FACESIZE, actual);
    SelectObject(hdc, old); DeleteObject(f); ReleaseDC(NULL, hdc);
    return _wcsicmp(actual, face) == 0;
}

void theme_set_dpi(int dpi) {
    current_dpi = dpi > 0 ? dpi : 96;
    for (int i = 0; i < FONT_COUNT; i++) if (fonts[i]) { DeleteObject(fonts[i]); fonts[i] = NULL; }
    const wchar_t *ui = L"Segoe UI";
    const wchar_t *mono = font_exists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    const wchar_t *icons = font_exists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const wchar_t *serif = font_exists(L"Georgia") ? L"Georgia" : L"Cambria";
    fonts[FONT_BODY] = make_font(ui, 10.5, FW_NORMAL, false);
    fonts[FONT_BODY_MEDIUM] = make_font(ui, 10.5, FW_MEDIUM, false);
    fonts[FONT_BODY_SEMIBOLD] = make_font(ui, 10.5, FW_SEMIBOLD, false);
    fonts[FONT_HEADLINE] = make_font(ui, 11, FW_SEMIBOLD, false);
    fonts[FONT_SUBHEADLINE] = make_font(ui, 9.75, FW_NORMAL, false);
    fonts[FONT_SUBHEADLINE_SEMIBOLD] = make_font(ui, 9.75, FW_SEMIBOLD, false);
    fonts[FONT_TITLE3] = make_font(ui, 14, FW_SEMIBOLD, false);
    fonts[FONT_LARGE_TITLE] = make_font(serif, 22, FW_SEMIBOLD, false);
    fonts[FONT_CALLOUT] = make_font(ui, 10, FW_NORMAL, false);
    fonts[FONT_FOOTNOTE] = make_font(ui, 9.25, FW_NORMAL, false);
    fonts[FONT_CAPTION] = make_font(ui, 8.75, FW_NORMAL, false);
    fonts[FONT_CAPTION_MEDIUM] = make_font(ui, 8.75, FW_MEDIUM, false);
    fonts[FONT_CAPTION_SEMIBOLD] = make_font(ui, 8.75, FW_SEMIBOLD, false);
    fonts[FONT_CAPTION2] = make_font(ui, 8, FW_NORMAL, false);
    fonts[FONT_BODY_ITALIC] = make_font(ui, 10.5, FW_NORMAL, true);
    fonts[FONT_CALLOUT_ITALIC] = make_font(ui, 10, FW_NORMAL, true);
    fonts[FONT_FOOTNOTE_SEMIBOLD] = make_font(ui, 9.25, FW_SEMIBOLD, false);
    fonts[FONT_SUBHEADLINE_ITALIC] = make_font(ui, 9.75, FW_NORMAL, true);
    fonts[FONT_MONO] = make_font(mono, 9.25, FW_NORMAL, false);
    fonts[FONT_MONO_SMALL] = make_font(mono, 8.5, FW_NORMAL, false);
    fonts[FONT_MONO_CAPTION2] = make_font(mono, 8, FW_NORMAL, false);
    fonts[FONT_ICON] = make_font(icons, 11, FW_NORMAL, false);
    fonts[FONT_ICON_SMALL] = make_font(icons, 8.5, FW_NORMAL, false);
    fonts[FONT_ICON_LARGE] = make_font(icons, 15, FW_NORMAL, false);
    fonts[FONT_ICON_HUGE] = make_font(icons, 26, FW_NORMAL, false);
    fonts[FONT_SERIF_MONOGRAM] = make_font(serif, 12, FW_SEMIBOLD, false);
}

void theme_init(void) { theme_refresh(); theme_set_dpi(96); }
int theme_dpi(void) { return current_dpi; }
int px(int units) { return MulDiv(units, current_dpi, 96); }
HFONT font(FontId id) { return fonts[id]; }

COLORREF theme_status_color(const char *status) {
    if (str_eq(status, "running")) return theme.success;
    if (str_eq(status, "queued") || str_eq(status, "preparing") || str_eq(status, "starting")) return theme.warning;
    if (str_eq(status, "failed") || str_eq(status, "error") || str_eq(status, "cancelled")) return theme.danger;
    if (str_eq(status, "closed")) return blend(theme.secondary, theme.background, 0.6);
    return theme.secondary;
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

void fill_rect(HDC hdc, const RECT *rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, rc, brush);
    DeleteObject(brush);
}
void fill_round_rect(HDC hdc, const RECT *rc, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = border == fill ? CreatePen(PS_SOLID, 1, fill) : CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(hdc, brush), old_pen = SelectObject(hdc, pen);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, radius * 2, radius * 2);
    SelectObject(hdc, old_brush); SelectObject(hdc, old_pen);
    DeleteObject(brush); DeleteObject(pen);
}
void fill_circle(HDC hdc, int cx, int cy, int radius, COLORREF fill) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ old_brush = SelectObject(hdc, brush), old_pen = SelectObject(hdc, pen);
    Ellipse(hdc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(hdc, old_brush); SelectObject(hdc, old_pen);
    DeleteObject(brush); DeleteObject(pen);
}
void stroke_circle(HDC hdc, int cx, int cy, int radius, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH)), old_pen = SelectObject(hdc, pen);
    Ellipse(hdc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(hdc, old_brush); SelectObject(hdc, old_pen);
    DeleteObject(pen);
}
void draw_line(HDC hdc, int x1, int y1, int x2, int y2, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, NULL); LineTo(hdc, x2, y2);
    SelectObject(hdc, old); DeleteObject(pen);
}

int draw_textw(HDC hdc, const wchar_t *text, RECT *rc, FontId f, COLORREF color, UINT flags) {
    HFONT old = SelectObject(hdc, fonts[f]);
    SetTextColor(hdc, color); SetBkMode(hdc, TRANSPARENT);
    int h = DrawTextW(hdc, text, -1, rc, flags | DT_NOPREFIX);
    SelectObject(hdc, old);
    return h;
}
int draw_text(HDC hdc, const char *text, RECT *rc, FontId f, COLORREF color, UINT flags) {
    wchar_t *w = utf8_to_wide(text ? text : "");
    int h = draw_textw(hdc, w, rc, f, color, flags);
    free(w);
    return h;
}
int measure_text(HDC hdc, const char *text, int width, FontId f, UINT flags) {
    wchar_t *w = utf8_to_wide(text ? text : "");
    RECT rc = { 0, 0, width > 0 ? width : 100000, 0 };
    HFONT old = SelectObject(hdc, fonts[f]);
    int h = DrawTextW(hdc, *w ? w : L" ", -1, &rc, flags | DT_CALCRECT | DT_NOPREFIX | (width > 0 ? DT_WORDBREAK | DT_EDITCONTROL : 0));
    SelectObject(hdc, old);
    free(w);
    return h > 0 ? h : rc.bottom - rc.top;
}
int textw_width(HDC hdc, const wchar_t *text, FontId f) {
    HFONT old = SelectObject(hdc, fonts[f]);
    SIZE size = { 0, 0 };
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &size);
    SelectObject(hdc, old);
    return size.cx;
}
int text_width(HDC hdc, const char *text, FontId f) {
    wchar_t *w = utf8_to_wide(text ? text : "");
    int width = textw_width(hdc, w, f);
    free(w);
    return width;
}
int font_height(HDC hdc, FontId f) {
    HFONT old = SelectObject(hdc, fonts[f]);
    TEXTMETRICW tm; GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    return tm.tmHeight;
}
void draw_glyph(HDC hdc, wchar_t glyph, const RECT *rc, FontId f, COLORREF color) {
    wchar_t text[2] = { glyph, 0 };
    RECT r = *rc;
    draw_textw(hdc, text, &r, f, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void draw_status_dot(HDC hdc, int cx, int cy, const char *status) {
    COLORREF color = theme_status_color(status);
    int r = px(4);
    fill_circle(hdc, cx, cy, r - 1, color);
    if (str_eq(status, "running")) stroke_circle(hdc, cx, cy, r + 2, blend(color, theme.background, 0.35), 1);
}
int draw_badge(HDC hdc, int x, int y, wchar_t glyph, const char *text, COLORREF color, COLORREF background, int *height) {
    HDC measure = hdc ? hdc : GetDC(NULL);
    int h = font_height(measure, FONT_CAPTION2) + px(7);
    int glyph_w = glyph ? px(11) : 0;
    int text_w = text_width(measure, text, FONT_CAPTION_SEMIBOLD);
    int w = px(8) * 2 + glyph_w + (glyph && text && *text ? px(3) : 0) + text_w;
    if (hdc) {
        RECT rc = { x, y, x + w, y + h };
        fill_round_rect(hdc, &rc, h / 2, blend(color, background, 0.12), blend(color, background, 0.12));
        int cx = x + px(8);
        if (glyph) { RECT g = { cx, y, cx + glyph_w, y + h }; draw_glyph(hdc, glyph, &g, FONT_ICON_SMALL, color); cx += glyph_w + px(3); }
        RECT t = { cx, y, cx + text_w + 2, y + h };
        draw_text(hdc, text, &t, FONT_CAPTION_SEMIBOLD, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else ReleaseDC(NULL, measure);
    if (height) *height = h;
    return w;
}
int draw_chip(HDC hdc, int x, int y, const char *name, COLORREF color, COLORREF background, int *height) {
    HDC measure = hdc ? hdc : GetDC(NULL);
    int h = font_height(measure, FONT_CAPTION2) + px(6);
    int text_w = text_width(measure, name, FONT_CAPTION_MEDIUM);
    int w = px(7) * 2 + px(6) + px(4) + text_w;
    if (hdc) {
        RECT rc = { x, y, x + w, y + h };
        fill_round_rect(hdc, &rc, h / 2, blend(color, background, 0.14), blend(color, background, 0.45));
        fill_circle(hdc, x + px(7) + px(3), y + h / 2, px(3), color);
        RECT t = { x + px(7) + px(6) + px(4), y, x + w, y + h };
        draw_text(hdc, name, &t, FONT_CAPTION_MEDIUM, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else ReleaseDC(NULL, measure);
    if (height) *height = h;
    return w;
}
void draw_monogram(HDC hdc, int x, int y, int size, const char *text) {
    const char *slash = text ? strrchr(text, '/') : NULL;
    const char *start = slash && slash[1] ? slash + 1 : (text ? text : "?");
    wchar_t *w = utf8_to_wide(start);
    wchar_t letter[2] = { w[0] ? towupper(w[0]) : L'?', 0 };
    free(w);
    RECT rc = { x, y, x + size, y + size };
    COLORREF fill = blend(theme.accent, theme.background, 0.14);
    fill_round_rect(hdc, &rc, size * 28 / 100, fill, fill);
    draw_textw(hdc, letter, &rc, FONT_SERIF_MONOGRAM, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static const wchar_t *const glyphs[] = { L"·", L"✢", L"✳", L"✶", L"✻", L"✽", L"✻", L"✶", L"✳", L"✢" };
static const char *const verbs[] = { "Working", "Thinking", "Reasoning", "Tinkering", "Crafting", "Pondering" };
const wchar_t *working_glyph(int tick) { return glyphs[((tick % 10) + 10) % 10]; }
const char *working_verb(int tick) { return verbs[((tick / 25) % 6 + 6) % 6]; }

// MARK: - Formatting

static void to_local(time_t when, SYSTEMTIME *out) {
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)when * 10000000ULL + 116444736000000000ULL;
    FILETIME ft, local; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, out);
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
    if (h) return xstrfmt("%lldh %lldm %llds", h, m, s);
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
    COLORREF caption = theme.background;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    COLORREF text = theme.text;
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof text);
}
void theme_apply_control(HWND hwnd) {
    SetWindowTheme(hwnd, theme.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
}
