#include "screen_dashboard_internal.h"

static void pick_add(DashboardScreen *s, FilterId f, const char *key);
static void pick_remove(DashboardScreen *s, FilterId f, const char *key);
static char *option_label(FilterId f, const Json *o);
static char *pick_label(DashboardScreen *s, FilterId f, const char *key);

static const struct { const char *param, *one, *plural, *options; bool multi; } FILTERS[F_COUNT] = {
    { "project", "project", "projects", "projects", true },
    { "model", "model", "models", "models", true },
    { "activity", "activity", "activities", "activities", false },
    { "provider", "provider", "providers", "providers", false },
    { "account", "account", "accounts", "accounts", false },
    { "session", "session", "sessions", "sessions", false },
};

// MARK: - Filters

bool screen_dashboard_any_pick(DashboardScreen *s) { for (int f = 0; f < F_COUNT; f++) if (s->pick_count[f]) return true; return false; }

bool screen_dashboard_is_picked(DashboardScreen *s, FilterId f, const char *key) {
    for (size_t i = 0; i < s->pick_count[f]; i++) if (str_eq(s->picks[f][i], key)) return true;
    return false;
}

void screen_dashboard_picks_clear(DashboardScreen *s, FilterId f) { str_array_free(s->picks[f], s->pick_count[f]); s->picks[f] = NULL; s->pick_count[f] = 0; }

static void pick_add(DashboardScreen *s, FilterId f, const char *key) {
    s->picks[f] = xrealloc(s->picks[f], (s->pick_count[f] + 1) * sizeof *s->picks[f]);
    s->picks[f][s->pick_count[f]++] = xstrdup(key);
}

static void pick_remove(DashboardScreen *s, FilterId f, const char *key) {
    for (size_t i = 0; i < s->pick_count[f]; i++) {
        if (!str_eq(s->picks[f][i], key)) continue;
        free(s->picks[f][i]);
        memmove(&s->picks[f][i], &s->picks[f][i + 1], (s->pick_count[f] - i - 1) * sizeof *s->picks[f]);
        s->pick_count[f]--;
        return;
    }
}

/// A breakdown row's pick: the same one twice comes back out, so a row is a toggle.
void screen_dashboard_pick_only(DashboardScreen *s, FilterId f, const char *key) {
    bool same = s->pick_count[f] == 1 && str_eq(s->picks[f][0], key);
    screen_dashboard_picks_clear(s, f);
    if (!same) pick_add(s, f, key);
}

/// An option's key and the words a picker shows for it.
const char *screen_dashboard_option_key(const Json *o) { const char *k = json_str(json_get(o, "key")); return k ? k : ""; }

static char *option_label(FilterId f, const Json *o) {
    if (f == F_MODEL) return screen_dashboard_model_label(o);
    const char *label = json_str_nonempty(json_get(o, "label"));
    if (f == F_ACTIVITY) return xstrdup(screen_dashboard_activity_label(screen_dashboard_option_key(o)));
    if (f == F_PROJECT && json_bool_is(json_get(o, "gone"), true)) return xstrfmt("%s (removed)", label ? label : screen_dashboard_option_key(o));
    return xstrdup(label ? label : screen_dashboard_option_key(o));
}

/// What a pick is called: whatever the payload calls it now, else its key.
static char *pick_label(DashboardScreen *s, FilterId f, const char *key) {
    const Json *options = json_get(s->options, FILTERS[f].options);
    for (size_t i = 0; i < json_count(options); i++) if (str_eq(screen_dashboard_option_key(json_at(options, i)), key)) return option_label(f, json_at(options, i));
    return xstrdup(f == F_ACTIVITY ? screen_dashboard_activity_label(key) : key);
}

char *screen_dashboard_picks_label(DashboardScreen *s, FilterId f) {
    Str out; str_init(&out);
    for (size_t i = 0; i < s->pick_count[f]; i++) {
        char *l = pick_label(s, f, s->picks[f][i]);
        if (i) str_appendz(&out, ", ");
        str_appendz(&out, l); free(l);
    }
    return str_detach(&out);
}

Json *screen_dashboard_query_args(DashboardScreen *s) {
    Json *args = json_object();
    json_set_str(args, "period", screen_dashboard_PERIODS[screen_dashboard_g_period].id);
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->pick_count[f]) continue;
        if (!FILTERS[f].multi) { json_set_str(args, FILTERS[f].param, s->picks[f][0]); continue; }
        Json *list = json_array();
        for (size_t i = 0; i < s->pick_count[f]; i++) json_array_push(list, json_string(s->picks[f][i]));
        json_object_set(args, FILTERS[f].param, list);
    }
    return args;
}

char *screen_dashboard_query_key(DashboardScreen *s) { Json *a = screen_dashboard_query_args(s); char *k = json_serialize(a, true); json_free(a); return k; }

/// The payload drawn: only the one that answered the current window and picks.
const Json *screen_dashboard_shown(DashboardScreen *s) {
    if (!s->data) return NULL;
    char *k = screen_dashboard_query_key(s);
    bool same = str_eq(k, s->data_key);
    free(k);
    return same ? s->data : NULL;
}

// MARK: - Screen

char *screen_dashboard_filter_button_text(DashboardScreen *s, FilterId f) {
    size_t n = s->pick_count[f];
    if (!n) return xstrfmt("All %s \xE2\x96\xBE", FILTERS[f].plural);
    if (FILTERS[f].multi && n > 1) return xstrfmt("%zu %s \xE2\x96\xBE", n, FILTERS[f].plural);
    char *l = screen_dashboard_picks_label(s, f), *out = xstrfmt("%s \xE2\x96\xBE", l);
    free(l);
    return out;
}

void screen_dashboard_pick_period(DashboardScreen *s, POINT pt) {
    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < PERIOD_COUNT; i++) {
        wchar_t *w = utf8_to_wide(screen_dashboard_PERIODS[i].label);
        AppendMenuW(menu, MF_STRING | (i == screen_dashboard_g_period ? MF_CHECKED : 0), (UINT_PTR)(1 + i), w);
        free(w);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1 || chosen - 1 == screen_dashboard_g_period) return;
    screen_dashboard_g_period = chosen - 1;
    screen_dashboard_repick(s);
}

/// One picker's menu: All, then the window's options. The project and model pickers tick several; the others take one.
void screen_dashboard_pick_filter(DashboardScreen *s, FilterId f, POINT pt) {
    const Json *options = json_get(s->options, FILTERS[f].options);
    size_t n = json_count(options), listed = n < MENU_CAP ? n : MENU_CAP;
    HMENU menu = CreatePopupMenu();
    char *all = xstrfmt("All %s", FILTERS[f].plural); wchar_t *wall = utf8_to_wide(all);
    AppendMenuW(menu, MF_STRING | (s->pick_count[f] ? 0 : MF_CHECKED), 1, wall);
    free(all); free(wall);
    if (n) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    for (size_t i = 0; i < listed; i++) {
        const Json *o = json_at(options, i);
        char *label = option_label(f, o); wchar_t *wl = utf8_to_wide(label);
        AppendMenuW(menu, MF_STRING | (screen_dashboard_is_picked(s, f, screen_dashboard_option_key(o)) ? MF_CHECKED : 0), (UINT_PTR)(2 + i), wl);
        free(label); free(wl);
    }
    if (listed < n) {
        char *more = xstrfmt("%zu more %s; a shorter window lists them", n - listed, FILTERS[f].plural); wchar_t *wm = utf8_to_wide(more);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, wm);
        free(more); free(wm);
    }
    if (!n) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No usage in this period");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen == 1) { if (!s->pick_count[f]) return; screen_dashboard_picks_clear(s, f); }
    else if (chosen >= 2 && (size_t)(chosen - 2) < listed) {
        const char *key = screen_dashboard_option_key(json_at(options, (size_t)(chosen - 2)));
        if (FILTERS[f].multi) { if (screen_dashboard_is_picked(s, f, key)) pick_remove(s, f, key); else pick_add(s, f, key); }
        else { if (screen_dashboard_is_picked(s, f, key)) return; screen_dashboard_picks_clear(s, f); pick_add(s, f, key); }
    } else return;
    screen_dashboard_repick(s);
}
