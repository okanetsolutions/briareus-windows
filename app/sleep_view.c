#include "sleep_mode.h"
#include "canvas.h"
#include "repo.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static COLORREF ink(bool replay) { return replay ? RGB(120, 126, 138) : RGB(220, 224, 232); }
// Matches the existing Files tab's dark syntax palette, independent of the app's light mode.
static COLORREF syntax(CodeKind kind, bool replay) {
    if (replay) return ink(true);
    switch (kind) {
    case CODE_KEYWORD: return RGB(0xCC, 0x78, 0x32);
    case CODE_STRING: return RGB(0x6A, 0x87, 0x59);
    case CODE_COMMENT: return RGB(0x80, 0x80, 0x80);
    case CODE_NUMBER: return RGB(0x68, 0x97, 0xBB);
    case CODE_TYPE: return RGB(0xE8, 0xBF, 0x6A);
    case CODE_VARIABLE: return RGB(0x98, 0x76, 0xAA);
    default: return ink(false);
    }
}
static void label(Canvas *cv, const char *text, RECT *r, FontId font_id, COLORREF color) {
    draw_text(cv, text ? text : "", r, font_id, color, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}
static void code(Canvas *cv, const SleepPane *p, RECT rc) {
    if (!p->content) {
        label(cv, !p->path ? "Waiting for a file…" : !p->loaded ? "Reading file…" : "File unavailable", &rc, FONT_MONO_SMALL, ink(true)); return;
    }
    size_t count; RepoLine *lines = repo_lines(p->content, &count);
    size_t last = 0;
    while (last + 1 < count && lines[last + 1].start < p->typed) last++;
    int height = font_height(cv, FONT_MONO_SMALL), rows = (rc.bottom - rc.top) / height;
    size_t start = last + 1 > (size_t)rows ? last + 1 - (size_t)rows : 0;
    CodeLexer lexer; code_lexer_init(&lexer, code_language_of(p->path));
    canvas_clip(cv, &rc);
    for (size_t i = 0; i <= last && i < count; i++) {
        RepoLine l = lines[i]; size_t n; CodeToken *tokens = code_lexer_line(&lexer, p->content + l.start, l.len, &n);
        if (i >= start) {
            RECT text = rc; text.top += (int)(i - start) * height; text.bottom = text.top + height;
            char *number = xstrfmt("%3zu  ", i + 1); label(cv, number, &text, FONT_MONO_SMALL, ink(true)); text.left += text_width(cv, number, FONT_MONO_SMALL); free(number);
            size_t visible = p->typed > l.start ? p->typed - l.start : 0;
            for (size_t t = 0; t < n; t++) {
                CodeToken token = tokens[t]; if (token.start >= visible) break;
                size_t len = token.len; if (token.start + len > visible) len = visible - token.start;
                char *part = xstrndup(p->content + l.start + token.start, len);
                label(cv, part, &text, FONT_MONO_SMALL, syntax(token.kind, p->replay));
                text.left += text_width(cv, part, FONT_MONO_SMALL); free(part);
            }
        }
        free(tokens);
    }
    canvas_unclip(cv); free(lines);
}
static void pane(Canvas *cv, const SleepPane *p, RECT rc) {
    fill_rect(cv, &rc, RGB(17, 20, 27));
    rc.left += px(16); rc.right -= px(16); rc.top += px(12); rc.bottom -= px(12);
    RECT line = rc; label(cv, session_display_title(&p->session), &line, FONT_HEADLINE, ink(p->replay));
    line.top += px(30);
    const char *branch = json_str_or(json_get(p->session.raw, "branch"), "default branch");
    char *place = xstrfmt("%s · %s · %s%s", session_repo(&p->session), branch, session_status(&p->session), p->replay ? " · Replay" : "");
    label(cv, place, &line, FONT_CAPTION, ink(true)); free(place);
    line.top += px(25);
    char *file = xstrfmt("%s %s", p->writes ? "Editing" : "Reading", p->path ? p->path : "");
    label(cv, file, &line, FONT_MONO_CAPTION2, ink(p->replay)); free(file);
    RECT source = rc; source.top = line.top + px(25); source.bottom -= px(27);
    if (source.bottom > source.top) code(cv, p, source);
    line.top = rc.bottom - px(20); label(cv, p->command, &line, FONT_MONO_SMALL, ink(p->replay));
}
void sleep_paint(HWND hwnd, const SleepWall *wall) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
    Canvas *cv = canvas_begin_dc(dc, &rc);
    if (cv) {
        fill_rect(cv, &rc, RGB(9, 11, 16));
        RECT header = {px(24), px(18), rc.right - px(24), px(54)};
        bool replay = false; int count = 0;
        for (int i = 0; i < SLEEP_PANES; i++) if (wall->panes[i].session.raw) { count++; replay |= wall->panes[i].replay; }
        label(cv, replay ? "Recent sessions · Replay" : count ? "Session activity" : "Session activity · Quiet", &header, FONT_TITLE3, ink(false));
        RECT hint = {px(24), rc.bottom - px(30), rc.right - px(24), rc.bottom};
        label(cv, "Key, click or scroll to wake · Files show last pushed content", &hint, FONT_CAPTION, ink(true));
        int columns = rc.right >= px(850) && count > 1 ? 2 : 1;
        int rows = (count + columns - 1) / columns; if (!rows) rows = 1;
        int ticker = px(90), top = px(65), bottom = rc.bottom - px(42) - ticker;
        int w = (rc.right - px(36)) / columns, h = (bottom - top) / rows, index = 0;
        for (int i = 0; i < SLEEP_PANES; i++) if (wall->panes[i].session.raw) {
            int x = px(18) + index % columns * w, y = top + index / columns * h;
            RECT box = {x, y, x + w - px(8), y + h - px(8)};
            if (h > px(100)) pane(cv, &wall->panes[i], box);
            index++;
        }
        if (!count) { RECT empty = {px(24), px(90), rc.right - px(24), bottom}; label(cv, "No recent activity to show", &empty, FONT_BODY, ink(true)); }
        canvas_clip(cv, &rc);
        size_t start = wall->commands > 3 ? wall->commands - 3 : 0;
        for (size_t i = start; i < wall->commands; i++) {
            const SleepCommand *c = &wall->ticker[i];
            RECT line = {px(24), bottom + (int)(i - start) * px(25), rc.right - px(24), rc.bottom - px(36)};
            char *text = xstrfmt("%s%s  ›  %s", c->replay ? "Replay · " : "", c->project, c->text);
            label(cv, text, &line, FONT_MONO_SMALL, ink(c->replay)); free(text);
        }
        canvas_unclip(cv); canvas_end_dc(cv);
    }
    EndPaint(hwnd, &ps);
}
