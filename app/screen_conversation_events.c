#include "screen_conversation_internal.h"

/// A `<details>` summary: 13px muted, `▸`/`▾` before it, ink on hover.
typedef struct { char *text; bool open; } SummaryData;

/// One step of a tool block: the tool's name in a chip, then what it did, in mono, on one line.
typedef struct { char *name, *summary; bool error; } StepData;

static void summary_free(void *p);
static void paint_summary(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void step_free(void *p);
static void paint_step(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static char *first_line(const char *text);
static void add_time(Doc *doc, int x, int w, const Event *e, bool right);
static void paint_turn_footer(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void layout_turn_footer(Doc *doc, int x, int w, const Event *e);

// The dashboard's `#messages`: `mx-auto max-w-[860px] px-6 pt-[18px] pb-[30px]`.
int screen_conversation_column_x(int width) { int col = width < px(860) ? width : px(860); return (width - col) / 2 + px(24); }

int screen_conversation_column_w(int width) { int col = width < px(860) ? width : px(860); return col - px(48); }

/// `.ev-log`: a 12px mono line in the muted colour.
void screen_conversation_ev_log(Doc *doc, int x, int w, const char *text, COLORREF color) {
    doc_space(doc, px(1));
    doc_text(doc, x, w, text ? text : "", FONT_MONO_SMALL, color, DT_WORDBREAK | DT_EDITCONTROL);
    doc_space(doc, px(1));
}

static void summary_free(void *p) { SummaryData *d = p; free(d->text); free(d); }

static void paint_summary(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SummaryData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    RECT r = *rc;
    draw_text(cv, d->text, &r, FONT_FOOTNOTE, hovered ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_conversation_doc_summary(Doc *doc, int x, int w, const char *text, bool open, int action, intptr_t arg) {
    SummaryData *d = xcalloc(1, sizeof *d);
    d->text = xstrfmt("%s %s", open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", text); d->open = open;
    doc_custom(doc, x, w, px(18), paint_summary, d, summary_free, action, arg);
}

void screen_conversation_paint_left_border(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { RECT r = { rc->left, rc->top, rc->left + px(2), rc->bottom }; fill_rect(cv, &r, theme.line); }

static void step_free(void *p) { StepData *d = p; free(d->name); free(d->summary); free(d); }

static void paint_step(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    StepData *d = it->data;
    int cw = px(6) * 2 + text_width(cv, d->name, FONT_CAPTION) + 2, ch = px(19);
    RECT chip = { rc->left, rc->top + (rc->bottom - rc->top - ch) / 2, rc->left + cw, rc->top + (rc->bottom - rc->top - ch) / 2 + ch };
    fill_round_rect(cv, &chip, px(5), theme.raise, d->error ? theme.danger : theme.line);
    RECT n = { chip.left + px(6), chip.top, chip.right - px(6) + 2, chip.bottom };
    draw_text(cv, d->name, &n, FONT_CAPTION, d->error ? theme.danger : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT s = { chip.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->summary, &s, FONT_MONO_SMALL, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

const char *screen_conversation_tool_title(const Event *e) {
    if (!str_empty(e->name)) return e->name;
    if (str_eq(e->kind, "cmd")) return "Command";
    if (str_eq(e->kind, "git")) return "Git";
    if (str_eq(e->kind, "tool_error")) return "Tool error";
    return "Tool";
}

static char *first_line(const char *text) {
    char *trimmed = str_trim(text);
    const char *nl = strchr(trimmed, '\n');
    char *first = nl ? xstrndup(trimmed, (size_t)(nl - trimmed)) : xstrdup(trimmed);
    char *out = str_trim(first);
    free(first); free(trimmed);
    return out;
}

void screen_conversation_layout_step(Doc *doc, int x, int w, const Event *e) {
    StepData *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(screen_conversation_tool_title(e)); d->summary = first_line(event_detail(e)); d->error = str_eq(e->kind, "tool_error");
    doc_space(doc, px(3));
    doc_custom(doc, x, w, px(19), paint_step, d, step_free, 0, 0);
    doc_space(doc, px(3));
}

bool screen_conversation_is_tool(const Event *e) { return str_eq(e->kind, "tool") || str_eq(e->kind, "tool_error"); }

/// `setup` is a setup command's own output; a failed step's last lines are there.
bool screen_conversation_is_prep_line(const Event *e) { return (str_eq(e->kind, "info") || str_eq(e->kind, "cmd") || str_eq(e->kind, "git") || str_eq(e->kind, "setup") || str_eq(e->kind, "stdout")) && e->text; }

/// Info lines that belong to the conversation rather than to workspace preparation.
bool screen_conversation_prep_closed(const Event *e) {
    const char *t = e->text ? e->text : "";
    if (strncmp(t, "Starting ", 9) == 0 || strncmp(t, "Started worker ", 15) == 0 || strncmp(t, "Worker ", 7) == 0) return true;
    return str_icontains(t, "session started");
}

static void add_time(Doc *doc, int x, int w, const Event *e, bool right) {
    time_t when;
    if (!event_time(e, &when)) return;
    char *text = format_event_time(when);
    doc_space(doc, px(6));
    doc_text(doc, x, w, text, FONT_CAPTION2, theme.muted, (right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE);
    free(text);
}

void screen_conversation_paint_queued_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    stroke_dotted_round_rect(cv, rc, px(12), theme.line);
}

void screen_conversation_paint_working(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ConversationScreen *s = it->data;
    char *text = session_working_text(screen_conversation_session(s), &s->transcript, working_verb(s->tick));
    if (!text) return;
    RECT g = { rc->left, rc->top, rc->left + px(16), rc->bottom };
    draw_textw(cv, working_glyph(s->tick), &g, FONT_BODY_SEMIBOLD, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT t = { rc->left + px(24), rc->top, rc->right, rc->bottom };
    draw_text(cv, text, &t, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(text);
}

/// "— $2.9565 · 455s · 57 turns · 5.6M in / 36.4k out · 151.6k context", above a dashed rule.
static void paint_turn_footer(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    draw_dashed_line(cv, rc->left, rc->top, rc->right, rc->top, theme.line);
    RECT t = { rc->left, rc->top + px(8), rc->right, rc->bottom };
    draw_text(cv, it->text, &t, FONT_CAPTION, it->color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void layout_turn_footer(Doc *doc, int x, int w, const Event *e) {
    Str bits; str_init(&bits);
    if (e->has_cost) { char *c = xstrfmt("$%.4f", e->cost_usd); str_appendz(&bits, c); free(c); }
    if (e->has_duration) str_appendf(&bits, "%s%ds", bits.len ? " \xC2\xB7 " : "", (int)(e->duration_ms / 1000));
    double n;
    if (json_num(json_get(e->raw, "numTurns"), &n)) str_appendf(&bits, "%s%d turns", bits.len ? " \xC2\xB7 " : "", (int)n);
    double in = 0, out = 0;
    bool has_in = json_num(json_get(e->raw, "inputTokens"), &in), has_out = json_num(json_get(e->raw, "outputTokens"), &out);
    if (has_in || has_out) { char *a = format_tokens(in), *b = format_tokens(out); str_appendf(&bits, "%s%s in / %s out", bits.len ? " \xC2\xB7 " : "", a, b); free(a); free(b); }
    if (json_num(json_get(e->raw, "tokens"), &n)) { char *c = format_tokens(n); str_appendf(&bits, "%s%s context", bits.len ? " \xC2\xB7 " : "", c); free(c); }
    char *text = xstrfmt("\xE2\x80\x94 %s", bits.len ? bits.data : "turn done");
    str_free(&bits);
    doc_space(doc, px(10));
    int i = doc_custom(doc, x, w, px(8) + px(20), paint_turn_footer, NULL, NULL, 0, 0);
    Item *it = doc_item(doc, i);
    it->text = text; it->color = e->is_error == 1 ? theme.danger : theme.muted;
    doc_space(doc, px(4));
}

void screen_conversation_layout_event(ConversationScreen *s, Doc *doc, int x, int w, const Event *e) {
    if (str_eq(e->kind, "user")) {
        // `rounded-xl border border-line bg-raise px-3.5 py-2.5`, the time right-aligned under the text.
        doc_space(doc, px(18));
        int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(12));
        doc_item(doc, box)->hover_fill = false;
        doc_text(doc, x + px(14), w - px(28), e->text, FONT_BODY, theme.ink, DT_WORDBREAK | DT_EDITCONTROL);
        size_t an = json_count(e->attachments);
        for (size_t i = 0; i < an; i++) {
            const char *name = json_str(json_get(json_at(e->attachments, i), "name"));
            doc_space(doc, px(6));
            char *label = xstrfmt("\xF0\x9F\x93\x8E %s", name ? name : "Attachment");
            doc_text(doc, x + px(14), w - px(28), label, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
            free(label);
        }
        time_t when;
        if (event_time(e, &when)) { char *tm = format_event_time(when); doc_space(doc, px(6)); doc_text(doc, x + px(14), w - px(28), tm, FONT_CAPTION2, theme.muted, DT_RIGHT | DT_SINGLELINE); free(tm); }
        doc_box_end(doc, box, px(10));
        doc_space(doc, px(14));
    } else if (str_eq(e->kind, "text")) {
        doc_space(doc, px(10));
        doc_markdown(doc, x, w, e->text ? e->text : "", FONT_BODY);
        add_time(doc, x, w, e, false);
        doc_space(doc, px(10));
    } else if (str_eq(e->kind, "ask")) {
        doc_space(doc, px(10));
        int box = doc_box_begin(doc, x, w, px(12), theme.raise, theme.accent, px(12));
        doc_item(doc, box)->hover_fill = false;
        int ix = x + px(14), iw = w - px(28);
        doc_text(doc, ix, iw, "Your input is needed", FONT_CAPTION, theme.accent, DT_SINGLELINE);
        doc_space(doc, px(8));
        doc_markdown(doc, ix, iw, e->question ? e->question : (e->text ? e->text : ""), FONT_BODY);
        if (screen_conversation_can_message(s)) {
            size_t on = json_count(e->options);
            if (on) doc_space(doc, px(8));
            ButtonSpec *buttons = xcalloc(on, sizeof *buttons); size_t bn = 0;
            for (size_t i = 0; i < on; i++) {
                const char *label = json_str(json_get(json_at(e->options, i), "label"));
                if (!label) continue;
                ButtonSpec b = { 0, label, BUTTON_BORDERED, ACT_ANSWER, (intptr_t)((e->seq << 8) | (int)i), true };
                buttons[bn++] = b;
            }
            if (bn) doc_button_row(doc, ix, iw, buttons, bn);
            free(buttons);
        }
        doc_box_end(doc, box, px(12));
        doc_space(doc, px(10));
    } else if (str_eq(e->kind, "result")) {
        layout_turn_footer(doc, x, w, e);
    } else if (e->text) {
        if (str_eq(e->kind, "cmd")) { char *line = xstrfmt("$ %s", e->text); screen_conversation_ev_log(doc, x, w, line, theme.muted); free(line); }
        else screen_conversation_ev_log(doc, x, w, e->text, str_eq(e->kind, "stderr") ? theme.danger : theme.muted);
    }
}
