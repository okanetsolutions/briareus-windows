#include "project_ssh_internal.h"
#include "str.h"
#include <stdlib.h>

char *project_ssh_tab_label(ProjectSsh *p, Term *t) {
    if (!p->global) return xstrdup(term_label(t));
    bool clash = false;
    for (size_t i = 0; i < term_count(); i++) {
        Term *other = term_at(i);
        if (str_eq(term_label(t), term_label(other)) && !str_eq(term_group(t), term_group(other))) clash = true;
    }
    if (!clash) return xstrdup(term_label(t));
    // Include the repository too: two projects may also share a display name.
    const char *repo = term_group(t);
    size_t count; const Project *projects = projects_list(&count);
    for (size_t i = 0; i < count; i++)
        if (str_eq(projects[i].repo, repo)) return xstrfmt("%s · %s (%s)", term_label(t), project_title(&projects[i]), repo);
    return xstrfmt("%s · %s", term_label(t), *repo ? repo : "No project");
}
void project_ssh_global_list(ProjectSsh *p, Doc *doc, int w) {
    size_t count; const Project *projects = projects_list(&count);
    Json *folders = ssh_server_folders(p->rows, projects, count, "");
    for (size_t f = 0; f < json_count(folders); f++) {
        const Json *folder = json_at(folders, f), *indices = json_get(folder, "indices");
        const char *repo = json_str(json_get(folder, "repo"));
        bool open = ssh_server_folder_open(p->folded, repo, "");
        char *label = xstrfmt("%s %s (%u)", open ? "▾" : "▸", json_str(json_get(folder, "title")), (unsigned)json_count(indices));
        if (f) doc_space(doc, px(10));
        int item = doc_box_begin(doc, 0, w, px(8), theme.raise, theme.line, px(7));
        doc_text(doc, px(8), w - px(16), label, FONT_SUBHEADLINE_SEMIBOLD, theme.secondary, DT_WORDBREAK);
        doc_box_end(doc, item, px(8)); doc_box_action(doc, item, p->base + A_FOLDER, (intptr_t)f);
        doc_item(doc, item)->tip = xstrfmt("%s, %s", json_str(json_get(folder, "title")), open ? "Expanded" : "Collapsed");
        free(label);
        doc_space(doc, px(4));
        if (open) for (size_t i = 0; i < json_count(indices); i++)
            project_ssh_layout_server(p, doc, px(10), w - px(10), (size_t)json_int_or(json_at(indices, i), 0));
    }
    if (!json_count(folders) && !p->error) {
        doc_text(doc, 0, w, "No SSH servers yet. Register one in Settings.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    }
    json_free(folders);
}
