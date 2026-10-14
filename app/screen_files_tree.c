#include "screen_files_internal.h"

/// A row of the tree: a chevron and folder for a directory, the status mark for a file, then the name.
typedef struct { char *name; int depth; bool dir, open, selected; MarkData mark; } RowData;

static int node_add(PullFiles *f, const char *name, const char *path, int file, int depth);
static void node_link(PullFiles *f, int parent, int kid);
static int node_compare(const void *a, const void *b);
static void tree_sort(PullFiles *f, int index);
static void tree_compact(PullFiles *f, int index);
static void tree_depths(PullFiles *f, int index, int depth);
static void tree_build(PullFiles *f);
static bool dir_collapsed(PullFiles *f, const char *path);
static void row_free(void *p);
static void paint_tree_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void layout_tree_node(PullFiles *f, Doc *doc, int index, int x, int w);

static int node_add(PullFiles *f, const char *name, const char *path, int file, int depth) {
    if (f->node_count == f->node_cap) { f->node_cap = f->node_cap ? f->node_cap * 2 : 64; f->nodes = xrealloc(f->nodes, f->node_cap * sizeof *f->nodes); }
    TreeNode *n = &f->nodes[f->node_count];
    memset(n, 0, sizeof *n);
    n->name = xstrdup(name); n->path = xstrdup(path); n->file = file; n->depth = depth;
    return (int)f->node_count++;
}

static void node_link(PullFiles *f, int parent, int kid) {
    TreeNode *p = &f->nodes[parent];
    if (p->kid_count == p->kid_cap) { p->kid_cap = p->kid_cap ? p->kid_cap * 2 : 4; p->kids = xrealloc(p->kids, p->kid_cap * sizeof *p->kids); }
    p->kids[p->kid_count++] = kid;
}

void screen_files_tree_free(PullFiles *f) {
    for (size_t i = 0; i < f->node_count; i++) { free(f->nodes[i].name); free(f->nodes[i].path); free(f->nodes[i].kids); }
    free(f->nodes); f->nodes = NULL; f->node_count = f->node_cap = 0; f->tree_valid = false;
}

static PullFiles *sort_owner;

static int node_compare(const void *a, const void *b) {
    const TreeNode *x = &sort_owner->nodes[*(const int *)a], *y = &sort_owner->nodes[*(const int *)b];
    if ((x->file < 0) != (y->file < 0)) return x->file < 0 ? -1 : 1;   // directories first
    return _stricmp(x->name, y->name);
}

static void tree_sort(PullFiles *f, int index) {
    TreeNode *n = &f->nodes[index];
    sort_owner = f;
    if (n->kid_count > 1) qsort(n->kids, n->kid_count, sizeof *n->kids, node_compare);
    for (size_t i = 0; i < n->kid_count; i++) tree_sort(f, n->kids[i]);
}

/// A directory whose only child is a directory takes it in: `src/Services/Export`.
static void tree_compact(PullFiles *f, int index) {
    TreeNode *n = &f->nodes[index];
    while (n->file < 0 && n->kid_count == 1 && f->nodes[n->kids[0]].file < 0 && n->depth > 0) {
        TreeNode *only = &f->nodes[n->kids[0]];
        char *joined = xstrfmt("%s/%s", n->name, only->name);
        free(n->name); n->name = joined;
        free(n->path); n->path = xstrdup(only->path);
        free(n->kids); n->kids = only->kids; n->kid_count = only->kid_count; n->kid_cap = only->kid_cap;
        only->kids = NULL; only->kid_count = only->kid_cap = 0; only->file = -2;   // dropped
    }
    for (size_t i = 0; i < n->kid_count; i++) tree_compact(f, n->kids[i]);
}

static void tree_depths(PullFiles *f, int index, int depth) {
    TreeNode *n = &f->nodes[index];
    n->depth = depth;
    for (size_t i = 0; i < n->kid_count; i++) tree_depths(f, n->kids[i], depth + 1);
}

static void tree_build(PullFiles *f) {
    screen_files_tree_free(f);
    int root = node_add(f, "", "", -1, 0);
    for (size_t i = 0; i < f->list.file_count; i++) {
        const char *path = f->list.files[i].filename;
        int parent = root;
        for (const char *p = path; *p;) {
            const char *slash = strchr(p, '/');
            size_t n = slash ? (size_t)(slash - p) : strlen(p);
            char *part = xstrndup(p, n), *full = xstrndup(path, (size_t)(p - path) + n);
            int found = -1;
            if (slash) for (size_t k = 0; k < f->nodes[parent].kid_count; k++) { int kid = f->nodes[parent].kids[k]; if (f->nodes[kid].file < 0 && str_eq(f->nodes[kid].name, part)) { found = kid; break; } }
            if (found < 0) { found = node_add(f, part, full, slash ? -1 : (int)i, f->nodes[parent].depth + 1); node_link(f, parent, found); }
            free(part); free(full);
            parent = found;
            p = slash ? slash + 1 : p + n;
        }
    }
    tree_compact(f, root);
    tree_depths(f, root, 0);
    tree_sort(f, root);
    f->tree_files = f->list.file_count; f->tree_valid = true;
}

static bool dir_collapsed(PullFiles *f, const char *path) { for (size_t i = 0; i < f->collapsed_count; i++) if (str_eq(f->collapsed[i], path)) return true; return false; }

void screen_files_dir_toggle(PullFiles *f, const char *path) {
    for (size_t i = 0; i < f->collapsed_count; i++) if (str_eq(f->collapsed[i], path)) { free(f->collapsed[i]); f->collapsed[i] = f->collapsed[--f->collapsed_count]; return; }
    f->collapsed = xrealloc(f->collapsed, (f->collapsed_count + 1) * sizeof *f->collapsed);
    f->collapsed[f->collapsed_count++] = xstrdup(path);
}

static void row_free(void *p) { RowData *d = p; free(d->name); free(d); }

static void paint_tree_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (d->selected || hovered) { RECT h = { rc->left, rc->top, rc->right, rc->bottom }; fill_round_rect(cv, &h, px(6), d->selected ? blend(theme.accent, theme.canvas, 0.16) : theme.raise, d->selected ? blend(theme.accent, theme.canvas, 0.16) : theme.raise); }
    int x = rc->left + px(6) + d->depth * px(16);
    if (d->dir) {
        RECT c = { x, rc->top, x + px(14), rc->bottom }; draw_glyph(cv, d->open ? 0xE70D : 0xE76C, &c, FONT_ICON_SMALL, theme.muted); x += px(16);
        RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, 0xE8B7, &g, FONT_ICON_SMALL, theme.accent); x += px(20);
    } else {
        x += px(16);
        RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, d->mark.glyph, &g, FONT_ICON_SMALL, d->mark.color); x += px(20);
    }
    RECT t = { x, rc->top, rc->right - px(6), rc->bottom };
    draw_text(cv, d->name, &t, d->selected ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
}

static void layout_tree_node(PullFiles *f, Doc *doc, int index, int x, int w) {
    TreeNode *n = &f->nodes[index];
    if (n->file == -2) return;
    if (n->depth > 0) {
        RowData *d = xcalloc(1, sizeof *d);
        d->name = xstrdup(n->name); d->depth = n->depth - 1; d->dir = n->file < 0;
        d->open = d->dir && !dir_collapsed(f, n->path);
        d->selected = !d->dir && n->file == f->selected;
        if (!d->dir) screen_files_file_mark(&f->list.files[n->file], &d->mark);
        int i = doc_custom(doc, x, w, px(26), paint_tree_row, d, row_free, f->action_base + (d->dir ? ACT_TOGGLE : ACT_SELECT), d->dir ? (intptr_t)index : (intptr_t)n->file);
        doc_item(doc, i)->hover_fill = false;
        doc_space(doc, px(1));
        if (d->dir && !d->open) return;
    }
    for (size_t k = 0; k < n->kid_count; k++) layout_tree_node(f, doc, n->kids[k], x, w);
}

void screen_files_layout_tree(PullFiles *f, Doc *doc, int x, int w) {
    if (!f->tree_valid || f->tree_files != f->list.file_count) tree_build(f);
    char *title = xstrfmt("%zu file%s", f->list.file_count, f->list.file_count == 1 ? "" : "s");
    doc_text(doc, x + px(6), w - px(12), title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(title);
    doc_space(doc, px(6));
    if (f->node_count) layout_tree_node(f, doc, 0, x, w);
}
