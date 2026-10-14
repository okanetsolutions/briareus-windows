#include "project_sftp_internal.h"

/// One line over the tree, always there so the rows do not move: what runs now, how the last transfer or change went
/// (with "Show in folder" after a download), else how to move files.
typedef struct { char *text; wchar_t glyph; COLORREF color; bool busy, show; } StatusData;

static void remember_row(ProjectSftp *p, const char *path, int top, int bottom);
static void layout_node(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w, SftpNode *n, int depth);
static void status_free(void *v);
static void paint_status(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void layout_status(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w);
static void layout_session(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w);

void project_sftp_forget_rows(ProjectSftp *p) {
    for (size_t i = 0; i < p->path_count; i++) free(p->paths[i]);
    free(p->paths); free(p->tops); free(p->bottoms);
    p->paths = NULL; p->tops = p->bottoms = NULL; p->path_count = 0;
}

static void remember_row(ProjectSftp *p, const char *path, int top, int bottom) {
    p->paths = xrealloc(p->paths, (p->path_count + 1) * sizeof *p->paths);
    p->tops = xrealloc(p->tops, (p->path_count + 1) * sizeof *p->tops);
    p->bottoms = xrealloc(p->bottoms, (p->path_count + 1) * sizeof *p->bottoms);
    p->paths[p->path_count] = xstrdup(path); p->tops[p->path_count] = top; p->bottoms[p->path_count] = bottom;
    p->path_count++;
}

/// A folder's open entries under it, depth first.
static void layout_node(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w, SftpNode *n, int depth) {
    bool folder = n->dir || (n->link && (!n->loaded || n->kid_count));
    RowData *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(n->name); d->depth = depth; d->folder = folder; d->open = folder && n->expanded; d->link = n->link;
    d->selected = str_eq(sftp_selected(s), n->path);
    d->home = str_eq(n->path, sftp_home(s));
    if (!folder) d->size = sftp_format_size(n->size);
    d->when = n->when ? xstrdup(n->when) : NULL;
    int top = doc->y;
    size_t index = p->path_count;
    remember_row(p, n->path, top, top + px(ROW_H));
    doc_custom(doc, x, w, px(ROW_H), project_sftp_paint_row, d, project_sftp_row_free, p->base + A_ROW, (intptr_t)index);
    if (folder) {
        // The twisty opens and closes without selecting.
        RECT tw = { project_sftp_twisty_left(&(RECT){ x, top, x + w, top }, depth) - px(4), top, project_sftp_twisty_left(&(RECT){ x, top, x + w, top }, depth) + px(18), top + px(ROW_H) };
        Item *t = doc_item(doc, doc_add(doc, &tw, NULL));
        t->action = p->base + A_TWISTY; t->arg = (intptr_t)index; t->hand = true;
    }
    if (!folder || !n->expanded) return;
    int inner = x + (depth + 1) * px(INDENT) + px(28);
    if (n->loading && !n->kid_count) {
        doc_text(doc, inner, w - (inner - x), "Loading\xE2\x80\xA6", FONT_CAPTION, theme.muted, DT_SINGLELINE);
        doc_space(doc, px(4));
    }
    if (n->error) {
        doc_text(doc, inner, w - (inner - x), n->error, FONT_CAPTION, theme.danger, DT_WORDBREAK);
        doc_space(doc, px(4));
    } else if (n->loaded && !n->loading && !n->kid_count) {
        doc_text(doc, inner, w - (inner - x), "Empty folder", FONT_CAPTION, theme.tertiary, DT_SINGLELINE);
        doc_space(doc, px(4));
    }
    for (size_t i = 0; i < n->kid_count; i++) layout_node(p, s, doc, x, w, n->kids[i], depth + 1);
}

static void status_free(void *v) { StatusData *d = v; free(d->text); free(d); }

static void paint_status(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    StatusData *d = it->data;
    int right = rc->right - px(4);
    if (d->show) {
        const char *link = "Show in folder";
        int lw = text_width(cv, link, FONT_CAPTION_SEMIBOLD);
        RECT lr = { right - lw, rc->top, right, rc->bottom };
        draw_text(cv, link, &lr, FONT_CAPTION_SEMIBOLD, doc_item_hovered(doc, it) ? theme.ink : theme.accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        right = lr.left - px(16);
    }
    RECT g = { rc->left, rc->top, rc->left + px(18), rc->bottom };
    if (d->glyph) draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, d->color);
    RECT t = { d->glyph ? g.right + px(6) : rc->left + px(6), rc->top, right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_CAPTION, d->color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void layout_status(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w) {
    StatusData *d = xcalloc(1, sizeof *d);
    size_t queued = 0;
    const char *busy = sftp_busy(s, &queued);
    bool failed = false;
    const char *status = sftp_status(s, &failed);
    if (busy) {
        d->text = queued ? xstrfmt("%s \xC2\xB7 %zu more waiting", busy, queued) : xstrdup(busy);
        d->glyph = 0xE895; d->color = theme.accent; d->busy = true;
    } else if (status) {
        // Several complaints share the line.
        d->text = str_replace(status, "\n", " \xC2\xB7 ");
        d->glyph = failed ? 0xE783 : 0xE73E; d->color = failed ? theme.danger : theme.secondary;
        d->show = !failed && sftp_last_download(s) && str_has_prefix(status, "Downloaded ");
    } else {
        d->text = xstrdup(sftp_state(s) == SFTP_READY ? "Drop files or folders from Explorer onto a folder to upload them there; double-click a file to download it; right-click for more."
                                                      : "Not connected.");
        d->color = theme.tertiary;
    }
    doc_custom(doc, x, w, px(24), paint_status, d, status_free, d->show ? p->base + A_SHOW_DOWNLOAD : 0, 0);
    doc_space(doc, px(4));
}

static void layout_session(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w) {
    SftpState state = sftp_state(s);
    if (state != SFTP_CONNECTING) layout_status(p, s, doc, x, w);
    if (state == SFTP_CONNECTING) {
        doc_space(doc, px(30));
        char *line = xstrfmt("Connecting to %s\xE2\x80\xA6", sftp_target(s));
        doc_loading(doc, x, w, line);
        free(line);
        doc_space(doc, px(8));
        doc_text(doc, x, w, "A password or an unknown host key is asked for in a window of its own.", FONT_CAPTION, theme.muted, DT_CENTER | DT_WORDBREAK);
        return;
    }
    SftpNode *root = sftp_root(s);
    if (!root) {
        const char *log = sftp_connect_log(s);
        char *message = xstrfmt("Could not connect to %s.%s%s", sftp_target(s), log ? "\n" : "", log ? log : "");
        doc_notice(doc, x, w, message);
        free(message);
        doc_space(doc, px(8));
        doc_button(doc, x, 0, "Connect again", BUTTON_BORDERED, p->base + A_RECONNECT, 0, true);
        return;
    }
    if (state == SFTP_CLOSED) {
        doc_text(doc, x, w, "The session is closed; Reconnect opens it again.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(6));
    }
    doc_custom(doc, x, w, px(22), project_sftp_paint_columns, NULL, NULL, 0, 0);
    doc_rule(doc, x, w);
    doc_space(doc, px(2));
    layout_node(p, s, doc, x, w, root, 0);
    doc_space(doc, px(10));
}

void project_sftp_layout(ProjectSftp *p, Doc *doc, int w) {
    project_sftp_forget_rows(p);
    if (!project_sftp_offered()) {
        doc_text(doc, 0, w, "The SSH servers are read from Settings, which needs an Admin token. Create one on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
    project_sftp_load(p);
    RECT view = pane_content_rect(p->host->pane);
    int top = doc->y, bottom = top + (view.bottom - view.top) - top - px(14);
    if (bottom < top + px(240)) bottom = top + px(240);
    int lw = px(LIST_W);
    if (lw > w / 3) lw = w / 3;
    project_sftp_layout_list(p, doc, 0, lw);
    int list_bottom = doc->y;
    int rx = lw + px(18), rw = w - rx;
    doc->y = top;
    SftpSession *s = project_sftp_shown_session(p);
    if (!s) {
        doc_space(doc, px(40));
        doc_empty_state(doc, rx, rw, 0xE8B7, "No open sessions",
                        "Click a server on the left to browse its files here, and upload and download them. It connects from this PC straight to the "
                        "server with your own OpenSSH client, so your keys, ssh-agent and ~/.ssh/config apply; passwords are asked for in a window.");
    } else {
        project_sftp_layout_tabs(p, doc, rx, rw);
        doc_space(doc, px(10));
        layout_session(p, s, doc, rx, rw);
    }
    int right_bottom = doc->y;
    if (right_bottom > bottom) bottom = right_bottom;
    // A rule between the list and the sessions.
    RECT rule = { lw + px(8), top, lw + px(9), bottom };
    doc_add(doc, &rule, project_sftp_paint_rule);
    doc->y = bottom > list_bottom ? bottom : list_bottom;
}
