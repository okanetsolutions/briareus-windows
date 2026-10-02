#include "browser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// MARK: - Server-sent events

void sse_init(SseParser *p) { memset(p, 0, sizeof *p); str_init(&p->line); str_init(&p->data); }
void sse_free(SseParser *p) { str_free(&p->line); str_free(&p->data); free(p->event); memset(p, 0, sizeof *p); }

static void sse_reset_event(SseParser *p) {
    str_free(&p->data); str_init(&p->data);
    free(p->event); p->event = NULL;
    p->has_data = false; p->overflow = false;
}
static void sse_line(SseParser *p, SseEmit emit, void *ctx) {
    const char *line = p->line.data ? p->line.data : "";
    size_t len = p->line.len;
    if (len == 0) {
        if (p->has_data && !p->overflow) emit(ctx, p->event ? p->event : "message", p->data.data ? p->data.data : "", p->data.len);
        sse_reset_event(p);
        return;
    }
    if (line[0] == ':') return;
    const char *colon = memchr(line, ':', len);
    size_t name_len = colon ? (size_t)(colon - line) : len;
    const char *value = colon ? colon + 1 : line + len;
    if (colon && *value == ' ') value++;
    size_t value_len = (size_t)(line + len - value);
    if (name_len == 4 && memcmp(line, "data", 4) == 0) {
        if (p->has_data) str_appendc(&p->data, '\n');
        str_append(&p->data, value, value_len);
        p->has_data = true;
        if (p->data.len > SSE_MAX_EVENT) { p->overflow = true; str_free(&p->data); str_init(&p->data); }
    } else if (name_len == 5 && memcmp(line, "event", 5) == 0) {
        free(p->event); p->event = xstrndup(value, value_len);
    }
    // `id:` and `retry:` are for reconnecting, which the app does on its own terms.
}
void sse_feed(SseParser *p, const char *bytes, size_t len, SseEmit emit, void *ctx) {
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        // A CR ends a line; an LF straight after it belongs to the same line end.
        if (c == '\n' && p->after_cr) { p->after_cr = false; continue; }
        p->after_cr = c == '\r';
        if (c == '\r' || c == '\n') {
            sse_line(p, emit, ctx);
            str_free(&p->line); str_init(&p->line);
            continue;
        }
        if (p->line.len >= SSE_MAX_EVENT) { p->overflow = true; continue; }
        // Runs of ordinary bytes go in at once: a frame's line is hundreds of kilobytes.
        size_t run = 1;
        while (i + run < len && bytes[i + run] != '\r' && bytes[i + run] != '\n') run++;
        str_append(&p->line, bytes + i, run);
        i += run - 1;
    }
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
unsigned char *base64_decode(const char *text, size_t text_len, size_t *len) {
    *len = 0;
    if (!text) return NULL;
    while (text_len && text[text_len - 1] == '=') text_len--;
    if (text_len % 4 == 1) return NULL;
    unsigned char *out = xmalloc(text_len / 4 * 3 + 3);
    size_t n = 0; unsigned buffer = 0; int bits = 0;
    for (size_t i = 0; i < text_len; i++) {
        int v = base64_value((unsigned char)text[i]);
        if (v < 0) { free(out); return NULL; }
        buffer = (buffer << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (unsigned char)(buffer >> bits); buffer &= (1u << bits) - 1; }
    }
    *len = n;
    return out;
}

// MARK: - State

static void tabs_free(BrowserState *s) {
    for (size_t i = 0; i < s->count; i++) { free(s->tabs[i].id); free(s->tabs[i].url); free(s->tabs[i].title); }
    free(s->tabs); s->tabs = NULL; s->count = 0;
}
void browser_state_free(BrowserState *s) { tabs_free(s); free(s->active); memset(s, 0, sizeof *s); }
void browser_state_read(BrowserState *s, const Json *record) {
    if (!json_is_object(record)) return;
    if (json_bool_tristate(json_get(record, "on")) >= 0) s->on = json_bool_is(json_get(record, "on"), true);
    if (json_bool_tristate(json_get(record, "running")) >= 0) s->running = json_bool_is(json_get(record, "running"), true);
    const Json *tabs = json_get(record, "tabs");
    if (json_is_array(tabs)) {
        tabs_free(s);
        s->tabs = json_count(tabs) ? xcalloc(json_count(tabs), sizeof *s->tabs) : NULL;
        for (size_t i = 0; i < json_count(tabs); i++) {
            const Json *t = json_at(tabs, i);
            const char *id = json_str_nonempty(json_get(t, "id"));
            if (!id) continue;
            BrowserTab *tab = &s->tabs[s->count++];
            tab->id = xstrdup(id);
            tab->url = xstrdup(json_str_or(json_get(t, "url"), ""));
            tab->title = xstrdup(json_str_or(json_get(t, "title"), ""));
        }
    }
    const Json *active = json_get(record, "active");
    if (active) { free(s->active); s->active = json_dup_str(active); }
}
const BrowserTab *browser_active_tab(const BrowserState *s) {
    for (size_t i = 0; s->active && i < s->count; i++) if (str_eq(s->tabs[i].id, s->active)) return &s->tabs[i];
    return NULL;
}
bool browser_session_on(const Json *session, bool *running) {
    const Json *b = json_get(session, "browser");
    if (running) *running = json_is_object(b) && json_bool_is(json_get(b, "running"), true);
    return json_is_object(b);
}

// MARK: - Drawing and pointing

BrowserRect browser_fit(int view_w, int view_h, int frame_w, int frame_h) {
    BrowserRect r = { 0, 0, 0, 0 };
    if (view_w <= 0 || view_h <= 0 || frame_w <= 0 || frame_h <= 0) return r;
    int w = view_w, h = (int)((long long)frame_h * view_w / frame_w);
    if (h > view_h) { h = view_h; w = (int)((long long)frame_w * view_h / frame_h); }
    if (w > frame_w && h > frame_h) { w = frame_w; h = frame_h; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    r.left = (view_w - w) / 2; r.top = (view_h - h) / 2; r.right = r.left + w; r.bottom = r.top + h;
    return r;
}
bool browser_page_point(const BrowserRect *drawn, int frame_w, int frame_h, int x, int y, double *page_x, double *page_y) {
    int w = drawn->right - drawn->left, h = drawn->bottom - drawn->top;
    if (w <= 0 || h <= 0 || frame_w <= 0 || frame_h <= 0) return false;
    if (x < drawn->left || x >= drawn->right || y < drawn->top || y >= drawn->bottom) return false;
    *page_x = (x - drawn->left + 0.5) * frame_w / w;
    *page_y = (y - drawn->top + 0.5) * frame_h / h;
    return true;
}

const char *browser_key_name(unsigned vk) {
    switch (vk) {
    case VK_RETURN: return "Enter";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_ESCAPE: return "Escape";
    case VK_LEFT: return "ArrowLeft";
    case VK_UP: return "ArrowUp";
    case VK_RIGHT: return "ArrowRight";
    case VK_DOWN: return "ArrowDown";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    default: break;
    }
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz", digits[] = "0123456789";
    static char one[36][2];
    if (vk >= 'A' && vk <= 'Z') { char *k = one[vk - 'A']; k[0] = letters[vk - 'A']; k[1] = 0; return k; }
    if (vk >= '0' && vk <= '9') { char *k = one[26 + vk - '0']; k[0] = digits[vk - '0']; k[1] = 0; return k; }
    return NULL;
}

// MARK: - Input

void browser_inputs_free(BrowserInputs *q) {
    for (size_t i = 0; i < q->count; i++) json_free(q->items[i]);
    free(q->items); memset(q, 0, sizeof *q);
}
void browser_inputs_push(BrowserInputs *q, Json *input) {
    if (!input) return;
    const char *type = json_str(json_get(input, "type"));
    Json *last = q->count ? q->items[q->count - 1] : NULL;
    const char *last_type = last ? json_str(json_get(last, "type")) : NULL;
    if (last && str_eq(type, "type") && str_eq(last_type, "type")) {
        char *joined = xstrfmt("%s%s", json_str_or(json_get(last, "text"), ""), json_str_or(json_get(input, "text"), ""));
        json_set_str(last, "text", joined);
        free(joined); json_free(input);
        return;
    }
    if (last && str_eq(type, "move") && str_eq(last_type, "move")) { json_free(last); q->items[q->count - 1] = input; return; }
    if (last && str_eq(type, "wheel") && str_eq(last_type, "wheel")) {
        json_set_num(input, "deltaX", json_num_or(json_get(last, "deltaX"), 0) + json_num_or(json_get(input, "deltaX"), 0));
        json_set_num(input, "deltaY", json_num_or(json_get(last, "deltaY"), 0) + json_num_or(json_get(input, "deltaY"), 0));
        json_free(last); q->items[q->count - 1] = input;
        return;
    }
    if (q->count == q->cap) { q->cap = q->cap ? q->cap * 2 : 8; q->items = xrealloc(q->items, q->cap * sizeof *q->items); }
    q->items[q->count++] = input;
}
Json *browser_inputs_pop(BrowserInputs *q) {
    if (!q->count) return NULL;
    Json *first = q->items[0];
    memmove(q->items, q->items + 1, (q->count - 1) * sizeof *q->items);
    q->count--;
    return first;
}

char *browser_address(const char *typed) {
    char *t = str_trim(typed ? typed : "");
    char *result = NULL;
    if (!*t || strpbrk(t, " \t\r\n")) goto done;
    if (str_ieq(t, "about:blank")) { result = xstrdup("about:blank"); goto done; }
    char *folded = str_fold(t);
    bool web = str_has_prefix(folded, "http://") || str_has_prefix(folded, "https://");
    // Another scheme ("file://", "chrome://") is one before the path; "://" further on is part of the page's address.
    const char *slashes = strstr(folded, "://");
    bool other = !web && slashes && (size_t)(slashes - folded) < strcspn(folded, "/?#");
    // A local address has no certificate to show for itself: it is asked for over plain HTTP.
    bool local = str_has_prefix(folded, "localhost") || str_has_prefix(folded, "127.0.0.1") || str_has_prefix(folded, "[::1]");
    free(folded);
    if (other) goto done;
    if (web) {
        const char *host = strstr(t, "://") + 3;
        if (*host && *host != '/') result = xstrdup(t);
        goto done;
    }
    // "mailto:", "javascript:" and the like: a scheme without slashes is not an address. Only a colon before the path
    // counts: one after it ("/wiki/Special:Search") is part of the page.
    size_t host_len = strcspn(t, "/?#");
    const char *colon = memchr(t, ':', host_len);
    if (colon && !local) {
        const char *digits = colon + 1;
        while (isdigit((unsigned char)*digits)) digits++;
        if (digits == colon + 1 || (*digits && *digits != '/' && *digits != '?' && *digits != '#')) goto done;
    }
    result = xstrfmt("%s://%s", local ? "http" : "https", t);
done:
    free(t);
    return result;
}
