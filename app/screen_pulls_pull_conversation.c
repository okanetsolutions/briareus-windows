#include "screen_pulls_internal.h"

static void comment_head_free(void *p);
static void paint_comment_head(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static const char *review_verdict(const Json *review, wchar_t *glyph, COLORREF *color);
static intptr_t conv_arg(int feed, size_t i);
static CommentHeadData *conv_head(const Json *v, const char *did, const char *time_field);
static bool layout_comment_body(Doc *doc, int x, int w, const Json *v);
static void layout_thread(PullScreen *s, Doc *doc, int x, int w, size_t root);

static void comment_head_free(void *p) { CommentHeadData *d = p; free(d->author); free(d->when); free(d); }

static void paint_comment_head(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    CommentHeadData *d = it->data;
    COLORREF tint = blend(theme.accent, theme.raise, 0.10);
    RECT top = *rc; fill_round_rect(cv, &top, px(7), tint, tint);
    RECT low = { rc->left, (rc->top + rc->bottom) / 2, rc->right, rc->bottom }; fill_rect(cv, &low, tint);
    draw_line(cv, rc->left, rc->bottom - 1, rc->right, rc->bottom - 1, theme.line);
    int x = rc->left + px(16);
    if (d->glyph) { RECT g = { x, rc->top, x + px(18), rc->bottom }; draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, d->glyph_color); x += px(24); }
    if (d->author) { int aw = text_width(cv, d->author, FONT_FOOTNOTE_SEMIBOLD); RECT a = { x, rc->top, x + aw + px(2), rc->bottom }; draw_text(cv, d->author, &a, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); x += aw + px(5); }
    RECT t = { x, rc->top, rc->right - px(16), rc->bottom };
    draw_text(cv, d->when, &t, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/// A timeline comment: a box opened with its header (`h`, which is taken). Returns the box for doc_box_end, with the
/// inner column in `ix`/`iw`. A header with an action opens the comment on GitHub.
int screen_pulls_comment_begin(Doc *doc, Col c, CommentHeadData *h, int action, intptr_t arg, int *ix, int *iw) {
    int x = c.x, w = c.w;
    int box = doc_box_begin(doc, x, w, 0, theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    int hi = doc_custom(doc, x + 1, w - 2, px(38), paint_comment_head, h, comment_head_free, action, arg);
    doc_item(doc, hi)->hover_fill = false;
    doc_space(doc, px(14));
    *ix = x + px(16); *iw = w - px(32);
    return box;
}

/// The opening comment: the pull request's description as GitHub shows it, read from the server's file list when `pull` leaves it out.
void screen_pulls_layout_description(PullScreen *s, Doc *doc, Col c) {
    const char *body = json_str(json_get(s->pr, "body"));
    if (!body) body = s->body;
    bool loading = !body && !s->body_read && store_supports("pull_description");
    if (!body && !loading) return;
    const char *author = screen_pulls_pull_author(s);
    const PullSummary *row = screen_pulls_board_row(s);
    CommentHeadData *h = xcalloc(1, sizeof *h);
    h->author = xstrdup(author);
    if (row && row->has_updated) { char *rel = format_relative(row->updated_at); h->when = xstrfmt("%s \xC2\xB7 updated %s", author ? "commented" : "Description", rel); free(rel); }
    else h->when = xstrdup(author ? "commented" : "Description");
    int ix, iw, box = screen_pulls_comment_begin(doc, c, h, 0, 0, &ix, &iw);
    if (loading) doc_text(doc, ix, iw, "Loading the description\xE2\x80\xA6", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    else {
        char *trimmed = visible_markdown(body);
        if (*trimmed) doc_markdown(doc, ix, iw, trimmed, FONT_CALLOUT);
        else doc_text(doc, ix, iw, "No description provided.", FONT_CALLOUT_ITALIC, theme.secondary, DT_WORDBREAK);
        free(trimmed);
    }
    doc_box_end(doc, box, px(16));
}

// The Conversation tab: GitHub's timeline of conversation comments, reviews and the line comments they carry.

/// A review's verdict as its header words it, with its glyph; NULL for a plain comment review.
static const char *review_verdict(const Json *review, wchar_t *glyph, COLORREF *color) {
    const char *state = json_str(json_get(review, "state"));
    if (str_ieq(state, "approved")) { *glyph = 0xE73E; *color = theme.success; return "approved these changes"; }
    if (str_ieq(state, "changes_requested")) { *glyph = 0xE7BA; *color = theme.danger; return "requested changes"; }
    if (str_ieq(state, "dismissed")) { *glyph = 0xE711; *color = theme.secondary; return "reviewed (dismissed)"; }
    *glyph = 0xE8BD; *color = theme.muted;
    return NULL;
}

static intptr_t conv_arg(int feed, size_t i) { return ((intptr_t)feed << 24) | (intptr_t)i; }

/// The header of a timeline entry: `author did · 3h ago`.
static CommentHeadData *conv_head(const Json *v, const char *did, const char *time_field) {
    CommentHeadData *h = xcalloc(1, sizeof *h);
    const char *author = json_str(json_get(v, "author"));
    h->author = xstrdup(author ? author : "Someone");
    time_t at = pull_conversation_time(v, time_field);
    if (at) { char *rel = format_relative(at); h->when = xstrfmt("%s \xC2\xB7 %s", did, rel); free(rel); }
    else h->when = xstrdup(did);
    return h;
}

/// A comment's markdown as GitHub shows it; whether there was any.
static bool layout_comment_body(Doc *doc, int x, int w, const Json *v) {
    const char *body = json_str(json_get(v, "body"));
    char *t = visible_markdown(body ? body : "");
    bool any = *t != 0;
    if (any) doc_markdown(doc, x, w, t, FONT_CALLOUT);
    free(t);
    return any;
}

/// A line comment thread: the file and line, then each comment in it with its author and time.
static void layout_thread(PullScreen *s, Doc *doc, int x, int w, size_t root) {
    const Json *lines = s->conv[CONV_REVIEW_COMMENTS].items, *first = json_at(lines, root);
    int box = doc_box_begin(doc, x, w, 0, theme.canvas, theme.line, px(6));
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(8));
    const char *path = json_str(json_get(first, "path"));
    double line;
    bool has_line = json_num(json_get(first, "line"), &line) || json_num(json_get(first, "originalLine"), &line);
    char *where = has_line ? xstrfmt("%s:%d", path ? path : "", (int)line) : xstrdup(path ? path : "");
    int pi = doc_text(doc, x + px(12), w - px(24), where, FONT_MONO_SMALL, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
    if (safe_web_url(json_str(json_get(first, "url")))) { doc_item(doc, pi)->action = ACT_CONV_URL; doc_item(doc, pi)->arg = conv_arg(CONV_REVIEW_COMMENTS, root); doc_item(doc, pi)->hand = true; }
    free(where);
    doc_space(doc, px(8));
    doc_rule(doc, x, w);
    for (size_t i = 0; i < json_count(lines); i++) {
        if (pull_thread_root(lines, i) != root) continue;
        const Json *cm = json_at(lines, i);
        doc_space(doc, px(10));
        int y = doc->y, h = px(20), ix = x + px(12), iw = w - px(24);
        const char *author = json_str(json_get(cm, "author")); if (!author) author = "Someone";
        int aw = text_width(doc->cv, author, FONT_FOOTNOTE_SEMIBOLD); if (aw > iw) aw = iw;
        RECT a = { ix, y, ix + aw + px(2), y + h }; doc_text_at(doc, &a, author, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        time_t at = pull_conversation_time(cm, "createdAt");
        if (at) { char *rel = format_relative(at); RECT t = { ix + aw + px(8), y, ix + iw, y + h }; doc_text_at(doc, &t, rel, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); free(rel); }
        doc->y = y + h + px(4);
        layout_comment_body(doc, ix, iw, cm);
    }
    doc_space(doc, px(10));
    doc_box_end(doc, box, 0);
}

void screen_pulls_layout_conversation(PullScreen *s, Doc *doc, Col c) {
    for (int k = 0; k < CONV_FEEDS; k++) if (s->conv[k].error) { doc_notice(doc, c.x, c.w, s->conv[k].error); doc_space(doc, px(12)); break; }
    if (!screen_pulls_conv_read(s)) { doc_loading(doc, c.x, c.w, "Loading the conversation\xE2\x80\xA6"); return; }
    size_t n;
    PullConversationEntry *entries = pull_conversation_timeline(s->conv[CONV_COMMENTS].items, s->conv[CONV_REVIEWS].items, s->conv[CONV_REVIEW_COMMENTS].items, &n);
    const Json *lines = s->conv[CONV_REVIEW_COMMENTS].items;
    for (size_t e = 0; e < n; e++) {
        const PullConversationEntry *en = &entries[e];
        const Json *v = json_at(s->conv[en->feed].items, en->i);
        int ix, iw, box, action = safe_web_url(json_str(json_get(v, "url"))) ? ACT_CONV_URL : 0;
        if (e) doc_space(doc, px(16));
        if (en->feed == CONV_COMMENTS) {
            box = screen_pulls_comment_begin(doc, c, conv_head(v, "commented", "createdAt"), action, conv_arg(en->feed, en->i), &ix, &iw);
            if (!layout_comment_body(doc, ix, iw, v)) doc_text(doc, ix, iw, "No description provided.", FONT_CALLOUT_ITALIC, theme.secondary, DT_WORDBREAK);
        } else if (en->feed == CONV_REVIEWS) {
            wchar_t glyph; COLORREF color;
            const char *verdict = review_verdict(v, &glyph, &color);
            CommentHeadData *h = conv_head(v, verdict ? verdict : "reviewed", "submittedAt");
            h->glyph = glyph; h->glyph_color = color;
            box = screen_pulls_comment_begin(doc, c, h, action, conv_arg(en->feed, en->i), &ix, &iw);
            bool any = layout_comment_body(doc, ix, iw, v);
            size_t rv;
            for (size_t i = 0; i < json_count(lines); i++) {
                if (pull_thread_root(lines, i) != i || !pull_thread_review(lines, s->conv[CONV_REVIEWS].items, i, &rv) || rv != en->i) continue;
                if (any) doc_space(doc, px(10));
                layout_thread(s, doc, ix, iw, i);
                any = true;
            }
            if (!any) doc_space(doc, -px(14));
        } else {
            box = screen_pulls_comment_begin(doc, c, conv_head(v, "commented on a line", "createdAt"), action, conv_arg(en->feed, en->i), &ix, &iw);
            layout_thread(s, doc, ix, iw, en->i);
        }
        doc_box_end(doc, box, px(14));
    }
    if (!n) doc_text(doc, c.x, c.w, "No comments yet", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    free(entries);
}
