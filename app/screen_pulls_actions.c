#include "screen_pulls_internal.h"

static const char *assign_me_label(char *const *assignees, size_t count);
static int assignees_menu(HWND hwnd, POINT pt, char *const *assignees, size_t count);
static Json *assignees_chosen(int chosen, char *const *assignees, size_t count, int number);

// The project board: open pull requests and issues, one pull request in full, and one issue.

// MARK: - Errands, shared by the board and one pull request

const char *screen_pulls_action_icon(const char *id) {
    static const struct { const char *id; const char *icon; } icons[] = {
        { "run", "\xE2\x96\xB6" }, { "review", "\xE2\x8C\x95" }, { "solve-conflicts", "\xF0\x9F\x94\x80" }, { "fix-checks", "\xF0\x9F\xA7\xAA" }, { "implement-feedback", "\xF0\x9F\x92\xAC" },
        { "custom-feedback", "\xE2\x9C\x8D" }, { "test-sheet", "\xF0\x9F\x93\x8B" }, { "test-run", "\xF0\x9F\x8E\xAC" }, { "pr-body-summary", "\xE2\x9C\x8E" }, { "delete-self-comments", "\xF0\x9F\xA7\xB9" },
    };
    for (size_t k = 0; k < sizeof icons / sizeof *icons; k++) if (str_eq(icons[k].id, id)) return icons[k].icon;
    return "";
}

/// Asks for an errand's input when it takes one; the others start straight away. True to go ahead.
bool screen_pulls_action_prompt(Screen *source, const char *repo, const BoardAction *a, int number, char **input) {
    *input = NULL;
    if (a->has_input) return dialog_action_input(app_dialog_owner(), source, repo, a, number, input);
    return true;
}

/// The errands this app can start on a row: what the server offers for it, less what the token or server lacks.
BoardAction *screen_pulls_row_actions(const Json *catalog, const PullSummary *pull, int failed_checks, size_t *count) {
    *count = 0;
    if (!store_can_manage()) return NULL;
    size_t n; BoardAction *offered = board_actions_offered(catalog, pull, failed_checks, &n);
    BoardAction *out = xcalloc(n, sizeof *out);
    for (size_t i = 0; i < n; i++) {
        char *op = board_action_operation(&offered[i]);
        if (store_supports(op)) board_action_copy(&out[(*count)++], &offered[i]);
        free(op);
    }
    board_actions_free(offered, n);
    return out;
}

// MARK: - Edits, shared by one pull request and one issue

// The user's own GitHub login, which "Assign me" adds: asked the first time and kept in the registry, since the API does
// not say whose the token is.
static char *my_login;

static bool my_login_read;

const char *screen_pulls_github_login(void) {
    if (!my_login_read) { my_login = settings_read_github_login(); my_login_read = true; }
    return my_login;
}

/// Asks for the login, the saved one filled in; false when cancelled or left blank.
bool screen_pulls_github_login_ask(void) {
    char *typed = dialog_text(app_dialog_owner(), "Your GitHub login", "The GitHub user \xE2\x80\x9C" "Assign me\xE2\x80\x9D adds", "Save", screen_pulls_github_login() ? screen_pulls_github_login() : "");
    if (!typed) return false;
    Json *names = board_names_parse(typed, true);
    const char *login = json_str(json_at(names, 0));
    if (login) { set_string(&my_login, login); settings_write_github_login(login); }
    free(typed); json_free(names);
    return login != NULL;
}

/// "Assign me" or "Unassign me" as the item's assignees stand.
static const char *assign_me_label(char *const *assignees, size_t count) {
    const char *me = screen_pulls_github_login();
    for (size_t i = 0; me && i < count; i++) if (str_ieq(assignees[i], me)) return "Unassign me";
    return "Assign me";
}

/// The ▾ beside the assignees: assign or unassign the user, edit the list, or change which login "me" is.
static int assignees_menu(HWND hwnd, POINT pt, char *const *assignees, size_t count) {
    HMENU menu = CreatePopupMenu();
    wchar_t *me = utf8_to_wide(assign_me_label(assignees, count));
    AppendMenuW(menu, MF_STRING, EDIT_ASSIGN_ME, me); free(me);
    AppendMenuW(menu, MF_STRING, EDIT_ASSIGNEES, L"Edit assignees\x2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    char *change = screen_pulls_github_login() ? xstrfmt("Change my GitHub login (%s)\xE2\x80\xA6", screen_pulls_github_login()) : xstrdup("Set my GitHub login\xE2\x80\xA6");
    wchar_t *w = utf8_to_wide(change); AppendMenuW(menu, MF_STRING, EDIT_LOGIN, w); free(w); free(change);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    return chosen;
}

/// What a pick from that menu sends as `assignees`, or NULL for nothing to send. The list replaces GitHub's whole.
static Json *assignees_chosen(int chosen, char *const *assignees, size_t count, int number) {
    if (chosen == EDIT_LOGIN) { screen_pulls_github_login_ask(); return NULL; }
    if (chosen == EDIT_ASSIGN_ME) {
        if (!screen_pulls_github_login() && !screen_pulls_github_login_ask()) return NULL;
        return board_assignees_toggle(assignees, count, screen_pulls_github_login(), NULL);
    }
    if (chosen != EDIT_ASSIGNEES) return NULL;
    char *current = board_names_join(assignees, count), *caption = xstrfmt("Assignees of #%d", number);
    char *typed = dialog_text(app_dialog_owner(), caption, "GitHub logins, comma separated (ten at most)", "Save", current);
    free(current); free(caption);
    if (!typed) return NULL;
    Json *names = board_names_parse(typed, true);
    free(typed);
    return names;
}

/// The menu, then what its pick sends. The assignees are copied first: the screen may read them again while it is up.
Json *screen_pulls_assignees_edit(HWND hwnd, POINT pt, char *const *assignees, size_t count, int number) {
    char **mine = xcalloc(count + 1, sizeof *mine);
    for (size_t i = 0; i < count; i++) mine[i] = xstrdup(assignees[i]);
    Json *out = assignees_chosen(assignees_menu(hwnd, pt, mine, count), mine, count, number);
    str_array_free(mine, count);
    return out;
}

/// The labels typed over the current ones (read before the dialog opens), or NULL when cancelled.
Json *screen_pulls_labels_edit(const PullLabel *labels, size_t count, int number) {
    char *current = board_label_names_join(labels, count), *caption = xstrfmt("Labels of #%d", number);
    char *typed = dialog_text(app_dialog_owner(), caption, "Label names, comma separated", "Save", current);
    free(current); free(caption);
    if (!typed) return NULL;
    Json *names = board_names_parse(typed, false);
    free(typed);
    return names;
}

/// The title and description after the edit dialog, as only the fields that changed; NULL when cancelled or unchanged.
Json *screen_pulls_details_edit(const char *what, int number, const char *title, const char *body) {
    char *caption = xstrfmt("Edit %s #%d", what, number);
    char *t = xstrdup(title ? title : ""), *b = xstrdup(body ? body : "");
    bool ok = dialog_edit_item(app_dialog_owner(), caption, &t, &b);
    free(caption);
    Json *fields = NULL;
    if (ok) {
        char *was = str_replace(body ? body : "", "\r\n", "\n");
        fields = json_object();
        if (!str_eq(t, title)) json_set_str(fields, "title", t);
        if (!str_eq(b, was)) json_set_str(fields, "body", b);
        free(was);
        if (!json_count(fields)) { json_free(fields); fields = NULL; }
    }
    free(t); free(b);
    return fields;
}
