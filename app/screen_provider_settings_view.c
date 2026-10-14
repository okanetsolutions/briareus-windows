#include "screen_provider_settings_internal.h"

typedef struct { char *text; } SelectData;

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void hint(Doc *doc, int x, int w, const char *text);
static void field(FormScreen *s, Doc *doc, int x, int w, int f, const char *screen_provider_settings_note);
static void field_pair(FormScreen *s, Doc *doc, int x, int w, int a, const char *note_a, int b, const char *note_b);
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void select_free(void *p);
static void paint_select(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void select_box(Doc *doc, int x, int w, const char *label, const char *text, int action);
static void layout_connection(FormScreen *s, Doc *doc, int x, int w);

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}

static void hint(Doc *doc, int x, int w, const char *text) {
    doc_rich(doc, x, w, text, FONT_CAPTION, theme.muted);
}

/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
static void field(FormScreen *s, Doc *doc, int x, int w, int f, const char *screen_provider_settings_note) {
    if (!screen_provider_settings_field_shown(s, f)) return;
    const FieldDef *d = &screen_provider_settings_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, screen_provider_settings_note);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid);
    int h = screen_provider_settings_is_multiline(f) ? screen_provider_settings_shown_rows(s, f) * fh + px(16) : px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_box));
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    s->rects[f] = screen_provider_settings_is_multiline(f) ? (RECT){ x + px(10), box.top + px(8), x + w - px(3), box.bottom - px(8) }
                                  : (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

/// Two fields side by side, as the dashboard's `.field-row`; advances past the taller.
static void field_pair(FormScreen *s, Doc *doc, int x, int w, int a, const char *note_a, int b, const char *note_b) {
    int gap = px(14), half = (w - gap) / 2, top = doc->y;
    field(s, doc, x, half, a, note_a);
    int left_bottom = doc->y;
    doc->y = top;
    field(s, doc, x + half + gap, w - half - gap, b, note_b);
    if (doc->y < left_bottom) doc->y = left_bottom;
}

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    bool hovered = doc_item_hovered(doc, it);
    // A pixel under the row's middle, so the box sits on the label's capitals rather than its line box.
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2 + px(1);
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), s->active ? theme.accent : theme.field, s->active ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (s->active) draw_check_mark(cv, &b, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, "Active: new sessions may start on this provider", &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void select_free(void *p) { SelectData *d = p; free(d->text); free(d); }

static void paint_select(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SelectData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, px(6), theme.raise, hovered ? theme.accent_dim : theme.line);
    RECT t = { rc->left + px(12), rc->top, rc->right - px(28), rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT c = { rc->right - px(26), rc->top, rc->right - px(10), rc->bottom };
    draw_glyph(cv, 0xE70D, &c, FONT_ICON_SMALL, theme.ink);
}

/// A labelled select that opens a menu; advances.
static void select_box(Doc *doc, int x, int w, const char *label, const char *text, int action) {
    doc_text(doc, x, w, label, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(6));
    RECT box = { x, doc->y, x + w, doc->y + px(36) };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_select));
    SelectData *d = xcalloc(1, sizeof *d); d->text = xstrdup(text);
    it->data = d; it->free_data = select_free; it->action = action; it->hand = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

void screen_provider_settings_note(Doc *doc, int x, int w, const char *text) {
    hint(doc, x, w, text);
    doc_space(doc, px(12));
}

/// Under the label and binary: the mode, then the login, or the endpoint with its token and Test.
static void layout_connection(FormScreen *s, Doc *doc, int x, int w) {
    if (screen_provider_settings_mode_offered(s->binary)) select_box(doc, x, (w - px(14)) / 2, "Mode", s->token ? "API token" : "Login", ACT_MODE);
    else if (str_eq(s->binary, "grok")) screen_provider_settings_note(doc, x, w, "grok signs in with its own login; it takes no API token.");
    else screen_provider_settings_note(doc, x, w, "opencode authenticates with an API key per service and has no login of its own.");
    if (!screen_provider_settings_uses_token(s)) {
        if (!s->id) { screen_provider_settings_note(doc, x, w, "Save the provider first: its login is registered against the saved entry, so there is nothing to log in until one exists."); return; }
        screen_provider_settings_headline(s, doc, x, w);
        doc_space(doc, px(12));
        bool supported = store_supports(str_eq(s->binary, "claude") ? "provider_login_start" : "provider_login");
        doc_button(doc, x, 0, s->req_login ? "Logging in\xE2\x80\xA6" : "Log in", BUTTON_BORDERED, ACT_LOGIN, 0, supported && !s->req_login);
        doc_space(doc, px(10));
        if (s->device_code) {
            // codex's confirm page asks for the code its hidden CLI printed.
            doc_text(doc, x, w, "Enter this code on the page that opened; the login is picked up automatically.", FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_WORDBREAK);
            doc_space(doc, px(6));
            int top = doc->y;
            doc_text(doc, x, w, s->device_code, FONT_MONO, theme.ink, DT_LEFT | DT_SINGLELINE);
            int cw = text_width(doc->cv, s->device_code, FONT_MONO);
            doc->y = top;
            doc_button(doc, x + cw + px(14), 0, "Copy", BUTTON_PLAIN, ACT_COPY_CODE, 0, true);
            doc_space(doc, px(12));
        }
        if (screen_provider_settings_field_shown(s, F_LOGIN_CODE)) {
            screen_provider_settings_note(doc, x, w, "Approve in the browser, then paste the code it shows here.");
            field(s, doc, x, w, F_LOGIN_CODE, NULL);
            doc_button(doc, x, 0, "Finish", BUTTON_PROMINENT, ACT_FINISH, 0, !s->req_login && store_supports("provider_login_finish"));
            doc_space(doc, px(12));
            return;   // the hint above the code says it already
        }
        screen_provider_settings_note(doc, x, w, str_eq(s->binary, "claude")
            ? "The login happens in the browser: approve on claude.ai, then paste the code it shows back here."
            : "The login happens in the browser: the server runs the CLI's device login and picks it up once approved.");
        return;
    }
    field(s, doc, x, w, F_BASE_URL, screen_provider_settings_base_url_hint(s->binary));
    field(s, doc, x, w, F_API_KEY, screen_provider_settings_api_key_hint(s->binary));
    doc_button(doc, x, 0, s->req_test ? "Testing\xE2\x80\xA6" : "Test", BUTTON_BORDERED, ACT_TEST, 0, !s->req_test && store_supports("test_provider"));
    doc_space(doc, px(8));
    if (s->test_result && !s->req_test) { doc_text(doc, x, w, s->test_result, FONT_FOOTNOTE, s->test_failed ? theme.danger : theme.ok, DT_LEFT | DT_WORDBREAK); doc_space(doc, px(8)); }
    screen_provider_settings_note(doc, x, w, "Test probes the endpoint and token as the form holds them, no save needed, and puts the endpoint's models in the Models tab.");
}

void screen_provider_settings_form_layout(Screen *base, Doc *doc) {
    FormScreen *s = (FormScreen *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    if (!store_supports("settings_providers")) {
        doc_space(doc, px(10));
        doc_text(doc, x, col, "Provider settings need an Admin token on a server that offers them (GET /settings/providers).", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        return;
    }
    screen_provider_settings_layout_tabs(s, doc, x, col);
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    switch (screen_provider_settings_open_tab(s)) {
    case T_PROVIDER: {
        doc_item(doc, doc_custom(doc, x, col, px(26), paint_check, s, NULL, ACT_ACTIVE, 0))->hand = true;
        doc_space(doc, px(12));
        int gap = px(14), half = (col - gap) / 2, top = doc->y;
        field(s, doc, x, half, F_LABEL, NULL);
        int left_bottom = doc->y;
        doc->y = top;
        select_box(doc, x + half + gap, col - half - gap, "Binary", screen_provider_settings_binary_title(s->binary), ACT_BINARY);
        if (doc->y < left_bottom) doc->y = left_bottom;
        layout_connection(s, doc, x, col);
        break;
    }
    case T_MODELS:
        field_pair(s, doc, x, col, F_MODELS, "One model per line; empty offers the CLI's own list.", F_EFFORTS, "One effort per line; empty offers the CLI's own.");
        field_pair(s, doc, x, col, F_DEFAULT_MODEL, NULL, F_DEFAULT_EFFORT, NULL);
        break;
    case T_STATUS: screen_provider_settings_layout_status(s, doc, x, col); break;
    }
    doc_space(doc, px(40));
}

void screen_provider_settings_form_header(Screen *base, HeaderInfo *info) {
    FormScreen *s = (FormScreen *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : "Provider");
        snprintf(info->subtitle, sizeof info->subtitle, "runs the %s CLI", screen_provider_settings_row_binary(s->row));
    } else {
        snprintf(info->title, sizeof info->title, "New provider");
        snprintf(info->subtitle, sizeof info->subtitle, "A provider is a login or endpoint sessions can be started on.");
    }
    if (!store_supports("settings_providers")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save this provider (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_provider" : "create_provider");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_CLONE; c->enabled = !busy && store_supports("create_provider"); c->tip = "Clone into a new provider";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_provider"); d->tip = "Delete this provider";
}
