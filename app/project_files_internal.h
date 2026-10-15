#ifndef BRIAREUS_PROJECT_FILES_INTERNAL_H
#define BRIAREUS_PROJECT_FILES_INTERNAL_H
#include "repo.h"
#include "screens.h"
#include "sftp.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A project's Files tab: its repository as PhpStorm's project view shows it, read through the server's GitHub token
// (`repo_tree`, `repo_file`), so no checkout or token of the user's own is needed. The tree is on the left, folders
// first, at the branch the header's picker names (the default one until another is picked). Files open as tabs on the
// right, each read at the commit the tree was read at, so what opens matches the tree shown while the branch moves on;
// their lines are numbered and coloured by language, and select and copy as text. A binary file, or one over 1 MB,
// shows its size and a link to GitHub.
//
// Over the tree, PhpStorm's four finders: Go to Class (Ctrl+N), Go to File (Ctrl+Shift+N), Go to Symbol
// (Ctrl+Shift+Alt+N) and Find in Files (Ctrl+Shift+F). Files need only the tree; the other three search an index of
// the repository's source kept on this PC, in the encrypted response cache: every indexable file (repo_indexable) read
// through the API at the tree's commit, a few at a time. When the branch moves on, the index walks back through the
// new commits (`GET /commits/{sha}`) and reads again only the files they changed; one too far behind is read anew.

// The actions, from the host's `action_base` up.
enum { A_ROW, A_RESULT, A_TAB, A_TAB_CLOSE, A_BRANCH, A_OPEN_GITHUB, A_COPY, A_RETRY, A_FOCUS_FIND, A_CLEAR_FIND, A_RETRY_FILE, A_MODE, A_INDEX_RETRY };

enum { ID_FIND = 0x5F30 };

// the finder's edit control id
enum { MAX_TABS = 20, MAX_RESULTS = 50, MAX_HITS = 200, GUTTER_BLOCK = 64 };

/// The finders, in the order their picker shows them.
enum { FIND_CLASSES, FIND_FILES, FIND_SYMBOLS, FIND_TEXT, FIND_MODES };

/// Files read at once while indexing; commits walked back before reading the index anew; symbols worked out again at
/// most this often (ms) while a search waits on an index being filled.
enum { FETCH_PARALLEL = 4, WALK_MAX = 50, SYMBOLS_EVERY_MS = 3000 };

/// A file open as a tab, and its lines once read.
typedef struct ProjectFiles ProjectFiles;

typedef struct {
    ProjectFiles *owner;
    char *path;
    RepoFile file; bool loaded;     // `file` holds the server's answer
    char *read_at;                  // the ref it was read at; a tree read at another commit reads it again
    char *error;
    Request *req;
    // Worked out once per answer: the lines, and each one's runs (line i's are tokens[first[i]] up to tokens[first[i + 1]]).
    RepoLine *lines; size_t line_count;
    CodeToken *tokens; size_t *first;
    size_t widest;                  // the longest line, in columns
} FileTab;

struct ProjectFiles {
    char *repo;
    Screen *host;
    int base;
    char *ref;                      // the branch picked; NULL for the default one
    RepoTree tree; bool has_tree;
    bool loaded;                    // the server answered since the tab was first opened
    char *error;
    Request *req;
    // The branch picker's choices, read once.
    char **branches; size_t branch_count; char *default_branch; bool branches_read; Request *req_branches;
    FileTab **tabs; size_t tab_count; int active;   // -1 for none
    // The finder: its edit, where the last layout put it, and what it finds: tree entries (Files), the index's symbols
    // (Classes, Symbols) or lines (Text, `hits`).
    HWND find; RECT find_rc; bool find_laid, clipped;
    int mode;
    char *query; size_t *results; RepoTextHit *hits; size_t result_count, text_total; int pick;
    // The index: read from disk once per branch, then brought to the tree's commit (`target`) by walking back through
    // the commits since its own (`walk_at`, gathering `changed`) and reading the files in `queue`.
    RepoIndex index; bool index_read, index_dirty, symbols_stale; ULONGLONG symbols_at;
    struct IndexLoad *loading;
    char *target, *walk_at; char **changed; size_t changed_count; int walk_steps; Request *req_walk;
    char **queue; size_t queue_count, queue_next, fetched; Request *fetch[FETCH_PARALLEL];
    char *index_error;
    // A line to show once its file is laid out: a symbol's or a text hit's.
    char *goto_path; int goto_line, goto_y; bool goto_scroll; UINT timer;
};

/// Reading the index from disk, off the UI thread; `cancelled` when its tab went first.
typedef struct IndexLoad { ProjectFiles *p; char *key; RepoIndex index; bool found, cancelled; } IndexLoad;

bool project_files_offered(void);
void project_files_relayout(ProjectFiles *p);
FileTab *project_files_active_file(ProjectFiles *p);
const char *project_files_files_ref(const ProjectFiles *p);
const char *project_files_shown_ref(const ProjectFiles *p);
char *project_files_state_key(const ProjectFiles *p);
void project_files_state_save(const ProjectFiles *p);
void project_files_file_free(FileTab *f);
void project_files_file_ensure(ProjectFiles *p, FileTab *f, bool retry);
void project_files_activate(ProjectFiles *p, int index);
FileTab *project_files_tab_add(ProjectFiles *p, const char *path);
void project_files_tab_close(ProjectFiles *p, size_t index);
void project_files_open_path(ProjectFiles *p, const char *path);
const char *project_files_index_base(const ProjectFiles *p);
void project_files_symbols_refresh(ProjectFiles *p);
void project_files_index_sync(ProjectFiles *p);
void project_files_index_drop(ProjectFiles *p);
void project_files_load(ProjectFiles *p);
void project_files_open(ProjectFiles *p);
void project_files_refresh(ProjectFiles *p);
void project_files_find_update(ProjectFiles *p);
void project_files_find_clear(ProjectFiles *p);
void project_files_open_at(ProjectFiles *p, const char *path, int line);
void project_files_open_result(ProjectFiles *p, size_t k);
void project_files_set_mode(ProjectFiles *p, int mode);
int project_files_mode_for_key(WPARAM vk, bool ctrl, bool shift);
bool project_files_command(ProjectFiles *p, int id, int code);
bool project_files_key(ProjectFiles *p, WPARAM vk, bool ctrl, bool shift);
bool project_files_timer(ProjectFiles *p, UINT id);
void project_files_place(ProjectFiles *p, const RECT *content, int scroll_y, bool shown);
void project_files_paint_clear(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void project_files_paint_lit(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void project_files_layout_find(ProjectFiles *p, Doc *doc, int x, int w);
void project_files_layout_results(ProjectFiles *p, Doc *doc, int x, int w);
void project_files_layout_modes(ProjectFiles *p, Doc *doc, int x, int w);
void project_files_layout_tree(ProjectFiles *p, Doc *doc, int x, int w);
void project_files_layout_tabs(ProjectFiles *p, Doc *doc, int x, int w);
void project_files_layout_file(ProjectFiles *p, Doc *doc, int x, int w);
void project_files_layout(ProjectFiles *p, Doc *doc, int w);
void project_files_header(ProjectFiles *p, HeaderInfo *info);
void project_files_pick_branch(ProjectFiles *p, POINT pt);
bool project_files_action(ProjectFiles *p, int action, intptr_t arg, POINT pt);
bool project_files_context(ProjectFiles *p, int action, intptr_t arg, POINT pt);
ProjectFiles *project_files_new(const char *repo, Screen *host, int action_base, UINT timer);
void project_files_free(ProjectFiles *p);

#endif
