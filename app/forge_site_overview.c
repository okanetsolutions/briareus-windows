#include "forge_site_internal.h"

typedef struct { char *text; bool on; } CheckData;

static void check_free(void *p);
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void layout_status(ForgeSite *v, Doc *doc, int x, int w, int ed);
static char *field_text(const Json *j);
static char *field_label(const char *key);
static char *masked_url(const char *text);
static void labeled(Doc *doc, int x, int w, const char *label, const Json *j, bool mask);

static void check_free(void *p) { CheckData *d = p; free(d->text); free(d); }

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    CheckData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), d->on ? theme.accent : theme.field, d->on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (d->on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void layout_status(ForgeSite *v, Doc *doc, int x, int w, int ed) {
    if (v->error[ed]) { doc_notice_box(doc, x, w, v->error[ed]); doc_space(doc, px(10)); }
    if (v->notice[ed]) { doc_text(doc, x, w, v->notice[ed], FONT_FOOTNOTE, theme.ok, DT_WORDBREAK); doc_space(doc, px(10)); }
}

/// A field read as text whatever its type.
static char *field_text(const Json *j) {
    const char *t = json_str_nonempty(j);
    if (t) return xstrdup(t);
    double n;
    if (json_num(j, &n) && isfinite(n)) return n == floor(n) ? xstrfmt("%.0f", n) : xstrfmt("%g", n);
    int b = json_bool_tristate(j);
    if (b >= 0) return xstrdup(b ? "Yes" : "No");
    if (json_is_array(j) && json_count(j)) {
        Str s; str_init(&s);
        for (size_t i = 0; i < json_count(j); i++) {
            char *one = field_text(json_at(j, i));
            if (one) { if (s.len) str_appendz(&s, ", "); str_appendz(&s, one); }
            free(one);
        }
        char *out = s.len ? xstrdup(s.data) : NULL;
        str_free(&s);
        return out;
    }
    return NULL;
}

/// Forge's snake_case name as words: "deployment_status" → "Deployment status", "php_version" → "PHP version".
static char *field_label(const char *key) {
    static const char *const upper[] = { "url", "php", "ip", "ssl", "id", "ssh", "dns", "http", "https", "tls" };
    Str s; str_init(&s);
    for (const char *p = key; *p;) {
        const char *end = strchr(p, '_');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char *word = xstrndup(p, n);
        bool caps = false;
        for (size_t k = 0; k < sizeof upper / sizeof *upper; k++) caps |= str_ieq(word, upper[k]);
        for (char *c = word; *c; c++) {
            if (caps && *c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
            else if (!caps && !s.len && c == word && *c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        }
        if (s.len) str_appendc(&s, ' ');
        str_appendz(&s, word);
        free(word);
        p += n;
        if (*p == '_') p++;
    }
    char *out = xstrdup(s.data ? s.data : "");
    str_free(&s);
    return out;
}

/// A URL with the value of its `token` parameter hidden: Forge's deployment trigger URL deploys the site to whoever has it.
static char *masked_url(const char *text) {
    const char *t = strstr(text, "token=");
    if (!t) return xstrdup(text);
    const char *end = t + 6;
    while (*end && *end != '&' && *end != '#') end++;
    return xstrfmt("%.*stoken=\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2%s", (int)(t - text), text, end);
}

static void labeled(Doc *doc, int x, int w, const char *label, const Json *j, bool mask) {
    char *text = field_text(j);
    if (!text) return;
    if (mask) { char *m = masked_url(text); free(text); text = m; }
    doc_space(doc, px(5));
    doc_labeled(doc, x, w, label, text, theme.text);
    free(text);
}

bool forge_site_site_url(const ForgeSite *v, char **out) {
    const char *url = json_str(json_get(v->site, "url"));
    *out = NULL;
    if (!url) return false;
    *out = !safe_web_url(url) && !strstr(url, "://") ? xstrfmt("https://%s", url) : xstrdup(url);
    if (safe_web_url(*out)) return true;
    free(*out); *out = NULL;
    return false;
}

void forge_site_layout_overview(ForgeSite *v, Doc *doc, int x, int w) {
    if (v->site_error) { doc_notice_box(doc, x, w, v->site_error); doc_space(doc, px(10)); }
    int box = doc_box_begin(doc, x, w, px(12), theme.elevated, theme.line, px(10));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(14), iw = w - px(28);
    doc_text(doc, ix, iw, "Site", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
    labeled(doc, ix, iw, "Domain", json_get(v->site, "name"), false);
    labeled(doc, ix, iw, "Status", json_get(v->site, "status"), false);
    labeled(doc, ix, iw, "URL", json_get(v->site, "url"), false);
    labeled(doc, ix, iw, "Deployment", json_get(v->site, "deployment_status"), false);
    labeled(doc, ix, iw, "Quick deploy", json_get(v->site, "quick_deploy"), false);
    const Json *repo = json_get(v->site, "repository");
    if (json_is_object(repo)) {
        doc_space(doc, px(14));
        doc_text(doc, ix, iw, "Repository", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
        for (size_t i = 0; i < json_count(repo); i++) {
            char *label = field_label(json_key(repo, i));
            labeled(doc, ix, iw, label, json_get(repo, json_key(repo, i)), false);
            free(label);
        }
    }
    doc_box_end(doc, box, px(12));
    // Everything else Forge says about the site, as Forge names it.
    static const char *const shown[] = { "name", "status", "url", "deployment_status", "quick_deploy", "repository", "id" };
    doc_space(doc, px(12));
    box = doc_box_begin(doc, x, w, px(12), theme.elevated, theme.line, px(10));
    doc_item(doc, box)->hover_fill = false;
    doc_text(doc, ix, iw, "Everything Forge reports", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
    labeled(doc, ix, iw, "Forge id", json_get(v->site, "id"), false);
    for (size_t i = 0; i < json_count(v->site); i++) {
        const char *key = json_key(v->site, i);
        bool skip = false;
        for (size_t k = 0; k < sizeof shown / sizeof *shown; k++) skip |= str_eq(key, shown[k]);
        if (skip || json_is_object(json_get(v->site, key))) continue;
        char *label = field_label(key);
        labeled(doc, ix, iw, label, json_get(v->site, key), str_icontains(key, "url"));
        free(label);
    }
    doc_box_end(doc, box, px(12));
    char *url;
    if (forge_site_site_url(v, &url)) {
        doc_space(doc, px(12));
        ButtonSpec b = { 0xE8A7, "Open site", BUTTON_BORDERED, v->base + A_OPEN_SITE, 0, true };
        doc_button_row(doc, x, w, &b, 1);
        free(url);
    }
    if (v->req_site && !v->site_error) { doc_space(doc, px(8)); doc_text(doc, x, w, "Reading the site from Forge\xE2\x80\xA6", FONT_CAPTION, theme.muted, DT_LEFT); }
}

void forge_site_layout_script(ForgeSite *v, Doc *doc, int x, int w) {
    layout_status(v, doc, x, w, ED_SCRIPT);
    if (!v->loaded[ED_SCRIPT]) { if (!v->error[ED_SCRIPT]) doc_loading(doc, x, w, "Reading the deploy script\xE2\x80\xA6"); return; }
    doc_text(doc, x, w, "The commands Forge runs on each deployment of this site, from the site's directory. $FORGE_ variables such as $FORGE_SITE_BRANCH and $FORGE_PHP are set.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    doc_space(doc, px(8));
    CheckData *d = xcalloc(1, sizeof *d);
    d->text = xstrdup("Run the script with the site's .env loaded");
    d->on = v->auto_source;
    int it = doc_custom(doc, x, w, px(26), paint_check, d, check_free, v->base + A_AUTO_SOURCE, 0);
    doc_item(doc, it)->hand = true;
    doc_space(doc, px(8));
    forge_site_layout_editor(v, doc, x, w, ED_SCRIPT);
}

void forge_site_layout_env(ForgeSite *v, Doc *doc, int x, int w) {
    layout_status(v, doc, x, w, ED_ENV);
    if (!v->env_wanted) {
        doc_space(doc, px(24));
        doc_empty_state(doc, x, w, 0xE72E, "The .env holds production secrets", "It is read from Forge only when you ask for it, and shown here in full.");
        doc_space(doc, px(12));
        ButtonSpec b = { 0xE7B3, "Show .env", BUTTON_PROMINENT, v->base + A_REVEAL, 0, true };
        doc_button_row(doc, x + px(8), w - px(8), &b, 1);
        return;
    }
    if (!v->loaded[ED_ENV]) { if (!v->error[ED_ENV]) doc_loading(doc, x, w, "Reading the .env\xE2\x80\xA6"); return; }
    // Once saved, the save's own note says what happens next.
    if (!v->notice[ED_ENV]) {
        doc_text(doc, x, w, "Saving replaces the whole file. Forge writes it to the server shortly after; it does not clear the config cache or restart queue workers.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
    }
    forge_site_layout_editor(v, doc, x, w, ED_ENV);
}
