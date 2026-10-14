#ifndef BRIAREUS_SCREEN_FILES_INTERNAL_H
#define BRIAREUS_SCREEN_FILES_INTERNAL_H
#include "diff.h"
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A pull request's changed files as GitHub's Files changed tab: the tree of paths on the left, the chosen file's diff beside it.
// The component is laid out inside the pull request screen, and the standalone screen wraps it for the conversation's menu.

// MARK: - Tree

/// A directory or file of the tree. Directories hold only one child directory are shown as one row, `a/b/c`, as GitHub does.
typedef struct {
    char *name, *path;   // the row's text and the full directory path (files: the filename)
    int file;            // index into the list, or -1 for a directory
    int *kids; size_t kid_count, kid_cap;
    int depth;
} TreeNode;

struct PullFiles {
    Screen *host; int action_base; UINT page_timer;
    Project project; int number;
    PullFileList list;
    bool loading, changed, confirmed, retried;
    char *error;
    Request *req;
    // The tree, rebuilt when the list changes.
    TreeNode *nodes; size_t node_count, node_cap; size_t tree_files; bool tree_valid;
    char **collapsed; size_t collapsed_count;   // directory paths folded by the user
    // The chosen file and its parsed patch.
    int selected; char *selected_path;
    DiffLine *lines; size_t line_count; int lines_for;
    bool wrap;
};

enum { ACT_SELECT, ACT_TOGGLE, ACT_RETRY, ACT_WRAP, ACT_OPEN_URL };

// MARK: - Painting

typedef struct { wchar_t glyph; COLORREF color; } MarkData;

// MARK: - Standalone screen

enum { ACT_FILES_BASE = 1000 };

enum { TIMER_NEXT_PAGE = 5 };

void screen_files_tree_free(PullFiles *f);
void screen_files_dir_toggle(PullFiles *f, const char *path);
Pane *screen_files_files_pane(PullFiles *f);
void pull_files_load(PullFiles *f);
void pull_files_cancel(PullFiles *f);
void pull_files_refresh(PullFiles *f);
bool pull_files_started(PullFiles *f);
bool pull_files_timer(PullFiles *f, UINT id);
void screen_files_file_mark(const PullFile *file, MarkData *m);
void screen_files_layout_tree(PullFiles *f, Doc *doc, int x, int w);
void pull_files_layout(PullFiles *f, Doc *doc, int x, int w);
bool pull_files_action(PullFiles *f, int action, intptr_t arg);
PullFiles *pull_files_new(const Project *project, int number, Screen *host, int action_base, UINT page_timer);
void pull_files_free(PullFiles *f);
Screen *pull_files_screen_new(const Project *project, int number);

#endif
