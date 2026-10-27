#ifndef BRIAREUS_SCREEN_DASHBOARD_INTERNAL_H
#define BRIAREUS_SCREEN_DASHBOARD_INTERNAL_H
#include "screens.h"
#include "briareus_tokens.h"
#include "str.h"
#include "usage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// The 📊 Dashboard: what every project spent over a window, as the web dashboard's home pane draws it from the usage
// ledger (`GET /usage/all`). The totals as tiles, tokens and cost per day or month as bars, the most expensive sessions,
// and the spend by project, activity, provider and model, each with a ring of its share of the tokens. The window and
// six filters narrow the ledger; current account quotas are independent. Reading it needs an Admin token.

enum { ACT_PERIOD = 1000, ACT_REFRESH, ACT_FILTER, ACT_CLEAR, ACT_ROW, ACT_OPEN_SESSION, ACT_BAR };

enum { TIMER_POLL = 1 };

enum { ROW_STRIDE = 100000 };

// a breakdown row's argument is its filter times the stride plus its index
enum { MENU_CAP = 60 };

// the most options a picker lists; the window's sessions can run to thousands

typedef enum { F_PROJECT, F_MODEL, F_ACTIVITY, F_PROVIDER, F_ACCOUNT, F_SESSION, F_COUNT } FilterId;

extern UsageWindow screen_dashboard_preference;
typedef struct {
    Screen base;
    UsageWindow window;
    Json *data;                       // the payload on screen
    char *data_key;                   // the query it answered: a pick shows the loader until its own answer lands
    Json *options;                    // what the pickers offer, from the last payload: the whole window's, never the pick's
    char **picks[F_COUNT]; size_t pick_count[F_COUNT];
    char *error;
    Request *req;
    Json *accounts;
    char *accounts_error;
    Request *accounts_req, **status_reqs;
    size_t status_count;
    bool quota_fresh;
    double quota_retry_until;
    Poller poller;
} DashboardScreen;

// MARK: - Rings

// The categorical series colours (briareus_tokens.h), the only place a hue means which one: four slots and the tail's grey.
enum { SERIES_SLOTS = BRIAREUS_CHART_SLOTS };

/// shareSeries: the first four rows that spent get a colour each, every other row that spent is summed into Other, and
/// under three slices there is no ring at all (one is a circle, two a ratio the rows already say).
typedef struct { double share[SERIES_SLOTS + 1]; COLORREF color[SERIES_SLOTS + 1]; int count; } Ring;

// MARK: - Tables

enum { COLS_MAX = 8 };

/// One row of a table, painted whole: the cells in their columns, a swatch before the first, and in the project table a
/// bar of its share of the busiest project before the total.
typedef struct {
    int n, x[COLS_MAX], w[COLS_MAX]; UINT align[COLS_MAX];
    char *cell[COLS_MAX]; COLORREF color[COLS_MAX]; FontId font[COLS_MAX];
    char *sub;                 // a second line under the first cell (the session table's repository)
    COLORREF swatch;           // CLR_INVALID for none
    int bar_col; double bar; COLORREF bar_color;   // bar_col -1 for none
    bool head, on;
} RowData;

/// A table being built: its heads, then rows of owned cells, laid out once every width is known.
typedef struct {
    int n; const char *head[COLS_MAX]; UINT align[COLS_MAX]; int fixed[COLS_MAX];   // fixed: a share of the width in %, else 0
    RowData **rows; size_t count;
    FontId head_font; COLORREF head_color; int row_h;
    int flex;                  // the column that takes what the others leave
} Table;

const char *screen_dashboard_activity_label(const char *id);
double screen_dashboard_num(const Json *u, const char *key);
int screen_dashboard_count_of(const Json *u, const char *key);
const char *screen_dashboard_plural(int n);
char *screen_dashboard_tokens_text(double n);
char *screen_dashboard_cost_text(const Json *u);
char *screen_dashboard_cost_or_dash(const Json *u);
char *screen_dashboard_cost_note(const Json *u);
char *screen_dashboard_duration_or_dash(double ms);
char *screen_dashboard_bucket_name(const char *key, bool month);
char *screen_dashboard_window_name(const Json *u, const UsageWindow *window);
char *screen_dashboard_model_label(const Json *m);
bool screen_dashboard_any_pick(DashboardScreen *s);
bool screen_dashboard_is_picked(DashboardScreen *s, FilterId f, const char *key);
void screen_dashboard_picks_clear(DashboardScreen *s, FilterId f);
void screen_dashboard_pick_only(DashboardScreen *s, FilterId f, const char *key);
const char *screen_dashboard_option_key(const Json *o);
char *screen_dashboard_picks_label(DashboardScreen *s, FilterId f);
Json *screen_dashboard_query_args(DashboardScreen *s);
char *screen_dashboard_query_key(DashboardScreen *s);
const Json *screen_dashboard_shown(DashboardScreen *s);
void screen_dashboard_quotas_cancel(DashboardScreen *s);
void screen_dashboard_quotas_load(DashboardScreen *s, bool fresh);
void screen_dashboard_repick(DashboardScreen *s);
void screen_dashboard_quotas_layout(DashboardScreen *s, Doc *doc, int x, int w);
void screen_dashboard_tiles(Doc *doc, int x, int w, const char *const *labels, char *const *values, char *const *subs, int n);
bool screen_dashboard_chart(Doc *doc, int x, int w, const Json *u, int chart_id, const char *metric, const char *title);
bool screen_dashboard_share_series(const Json *rows, Ring *ring, COLORREF *colors);
void screen_dashboard_ring_at(Doc *doc, int x, const Ring *ring);
void screen_dashboard_table_init(Table *t, int n, const char *const *heads, const UINT *aligns);
RowData *screen_dashboard_table_row(Table *t);
void screen_dashboard_table_emit(Doc *doc, int x, int w, Table *t, const int *actions, const intptr_t *args);
void screen_dashboard_cell(RowData *d, int i, char *text);
void screen_dashboard_usage_cells(RowData *d, int from, const Json *u, bool total);
void screen_dashboard_project_card(DashboardScreen *s, Doc *doc, int x, int w, const Json *u);
void screen_dashboard_simple_card(DashboardScreen *s, Doc *doc, int x, int w, const Json *u, FilterId f);
void screen_dashboard_insights(Doc *doc, int x, int w, const Json *u);
char *screen_dashboard_filter_button_text(DashboardScreen *s, FilterId f);
bool screen_dashboard_apply_range(DashboardScreen *s, const UsageRange *range);
void screen_dashboard_pick_period(DashboardScreen *s, POINT pt);
void screen_dashboard_pick_filter(DashboardScreen *s, FilterId f, POINT pt);
Screen *dashboard_screen_new(void);

#endif
