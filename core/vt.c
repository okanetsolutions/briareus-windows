// The terminal emulator's state machine: a VT500-style parser (ground, escape, CSI, OSC and the strings it skips) over a
// grid of cells, with the main and alternate screens and a ring of scrollback lines.
#include "vt.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

enum { MAX_PARAMS = 16, OSC_MAX = 4096 };
typedef enum { S_GROUND, S_ESC, S_ESC_INTER, S_CSI, S_OSC, S_OSC_ESC, S_STRING, S_STRING_ESC, S_CHARSET } State;

typedef struct { VtCell *cells; int width; } Line;
typedef struct { int x, y; VtCell pen; bool origin, autowrap, g0_lines, g1_lines, shift_out; } Saved;

struct Vt {
    int cols, rows;
    VtCell *main, *alt, *grid;      // `grid` is the screen in use: `main` or `alt`
    Line *sb; size_t sb_cap, sb_start, sb_count;
    int x, y; bool wrap_pending;
    VtCell pen;                      // the attributes new characters and erases take
    int top, bottom;                 // the scroll region, inclusive
    bool *tabs;
    bool cursor_visible, autowrap, origin, insert, app_cursor, app_keypad, bracketed, newline_mode;
    bool g0_lines, g1_lines, shift_out;   // DEC special graphics in G0 / G1, and SO selecting G1
    Saved saved, saved_alt;
    // The parser.
    State state;
    int params[MAX_PARAMS]; int nparams;   // -1 for a parameter left empty
    char private_mark; char inter;
    int charset_target;
    char *osc; size_t osc_len;
    uint32_t utf8_cp; int utf8_need;
    uint32_t last_char;              // for REP
    Str response;
    char *title; bool title_changed, bell;
};

static VtCell blank(const Vt *vt) { VtCell c = { ' ', VT_COLOR_DEFAULT, vt->pen.bg, 0 }; return c; }
static VtCell *row_at(Vt *vt, int y) { return vt->grid + (size_t)y * vt->cols; }
static void fill_cells(VtCell *c, int n, VtCell v) { for (int i = 0; i < n; i++) c[i] = v; }

static void reset_tabs(Vt *vt) { for (int i = 0; i < vt->cols; i++) vt->tabs[i] = i > 0 && i % 8 == 0; }
static void reset_modes(Vt *vt) {
    memset(&vt->pen, 0, sizeof vt->pen); vt->pen.ch = ' ';
    vt->top = 0; vt->bottom = vt->rows - 1;
    vt->cursor_visible = true; vt->autowrap = true; vt->origin = false; vt->insert = false;
    vt->app_cursor = false; vt->app_keypad = false; vt->bracketed = false; vt->newline_mode = false;
    vt->g0_lines = vt->g1_lines = vt->shift_out = false;
    vt->wrap_pending = false;
}

Vt *vt_new(int cols, int rows, size_t scrollback) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    Vt *vt = xcalloc(1, sizeof *vt);
    vt->cols = cols; vt->rows = rows;
    vt->main = xcalloc((size_t)cols * rows, sizeof *vt->main);
    vt->alt = xcalloc((size_t)cols * rows, sizeof *vt->alt);
    vt->grid = vt->main;
    vt->tabs = xcalloc((size_t)cols, sizeof *vt->tabs);
    vt->sb_cap = scrollback;
    vt->sb = scrollback ? xcalloc(scrollback, sizeof *vt->sb) : NULL;
    reset_modes(vt);
    reset_tabs(vt);
    fill_cells(vt->main, cols * rows, blank(vt));
    fill_cells(vt->alt, cols * rows, blank(vt));
    vt->saved.pen = vt->pen; vt->saved.autowrap = true;
    vt->saved_alt = vt->saved;
    str_init(&vt->response);
    return vt;
}

void vt_clear_scrollback(Vt *vt) {
    for (size_t i = 0; i < vt->sb_count; i++) free(vt->sb[(vt->sb_start + i) % vt->sb_cap].cells);
    vt->sb_start = vt->sb_count = 0;
}

void vt_free(Vt *vt) {
    if (!vt) return;
    vt_clear_scrollback(vt);
    free(vt->sb); free(vt->main); free(vt->alt); free(vt->tabs); free(vt->osc); free(vt->title);
    str_free(&vt->response);
    free(vt);
}

int vt_cols(const Vt *vt) { return vt->cols; }
int vt_rows(const Vt *vt) { return vt->rows; }
int vt_scrollback(const Vt *vt) { return (int)vt->sb_count; }
bool vt_app_cursor(const Vt *vt) { return vt->app_cursor; }
bool vt_app_keypad(const Vt *vt) { return vt->app_keypad; }
bool vt_bracketed_paste(const Vt *vt) { return vt->bracketed; }
bool vt_alt_screen(const Vt *vt) { return vt->grid == vt->alt; }
const char *vt_title(const Vt *vt) { return vt->title; }
bool vt_take_title_changed(Vt *vt) { bool c = vt->title_changed; vt->title_changed = false; return c; }
bool vt_take_bell(Vt *vt) { bool b = vt->bell; vt->bell = false; return b; }

void vt_cursor(const Vt *vt, int *x, int *y, bool *visible) {
    if (x) *x = vt->x < vt->cols ? vt->x : vt->cols - 1;
    if (y) *y = vt->y;
    if (visible) *visible = vt->cursor_visible;
}

const VtCell *vt_line(const Vt *vt, int y, int *width) {
    if (y >= 0) {
        if (y >= vt->rows) return NULL;
        if (width) *width = vt->cols;
        return vt->grid + (size_t)y * vt->cols;
    }
    if (-y > (int)vt->sb_count) return NULL;
    const Line *l = &vt->sb[(vt->sb_start + vt->sb_count + (size_t)y) % vt->sb_cap];
    if (width) *width = l->width;
    return l->cells;
}

char *vt_take_response(Vt *vt, size_t *len) {
    if (!vt->response.len) { if (len) *len = 0; return NULL; }
    if (len) *len = vt->response.len;
    return str_detach(&vt->response);
}

// MARK: - Character widths and colours

int vt_char_width(uint32_t c) {
    if (c == 0) return 0;
    // Combining marks and zero-width joiners ride on the cell before them.
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF)
        || (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFE0F || c == 0xFE0E) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF)
        || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF)
        || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6)
        || (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x1F680 && c <= 0x1F6FF)
        || (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}

uint32_t vt_index_rgb(int i) {
    if (i < 16) return 0;
    if (i < 232) {
        static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
        i -= 16;
        return (uint32_t)(steps[i / 36] << 16 | steps[(i / 6) % 6] << 8 | steps[i % 6]);
    }
    int g = 8 + (i - 232) * 10;
    return (uint32_t)(g << 16 | g << 8 | g);
}

// MARK: - Scrolling

static void push_scrollback(Vt *vt, const VtCell *row) {
    if (!vt->sb_cap) return;
    int width = vt->cols;
    while (width > 0 && row[width - 1].ch == ' ' && !row[width - 1].attr && VT_COLOR_KIND(row[width - 1].bg) == VT_COLOR_DEFAULT) width--;
    Line *slot;
    if (vt->sb_count == vt->sb_cap) {
        slot = &vt->sb[vt->sb_start];
        free(slot->cells);
        vt->sb_start = (vt->sb_start + 1) % vt->sb_cap;
    } else {
        slot = &vt->sb[(vt->sb_start + vt->sb_count) % vt->sb_cap];
        vt->sb_count++;
    }
    slot->width = width;
    slot->cells = width ? xmalloc((size_t)width * sizeof *row) : NULL;
    if (width) memcpy(slot->cells, row, (size_t)width * sizeof *row);
}

/// Moves the lines between `top` and `bottom` up by `n`, blanking the ones that come in at the bottom. Lines leaving the top
/// of the main screen's full-height region are kept in the scrollback.
static void scroll_up(Vt *vt, int top, int bottom, int n) {
    int height = bottom - top + 1;
    if (n <= 0 || height <= 0) return;
    if (n > height) n = height;
    if (top == 0 && vt->grid == vt->main)
        for (int i = 0; i < n; i++) push_scrollback(vt, row_at(vt, i));
    memmove(row_at(vt, top), row_at(vt, top + n), (size_t)(height - n) * vt->cols * sizeof(VtCell));
    fill_cells(row_at(vt, bottom - n + 1), n * vt->cols, blank(vt));
}
static void scroll_down(Vt *vt, int top, int bottom, int n) {
    int height = bottom - top + 1;
    if (n <= 0 || height <= 0) return;
    if (n > height) n = height;
    memmove(row_at(vt, top + n), row_at(vt, top), (size_t)(height - n) * vt->cols * sizeof(VtCell));
    fill_cells(row_at(vt, top), n * vt->cols, blank(vt));
}

static void linefeed(Vt *vt) {
    vt->wrap_pending = false;
    if (vt->y == vt->bottom) scroll_up(vt, vt->top, vt->bottom, 1);
    else if (vt->y < vt->rows - 1) vt->y++;
}
static void reverse_index(Vt *vt) {
    vt->wrap_pending = false;
    if (vt->y == vt->top) scroll_down(vt, vt->top, vt->bottom, 1);
    else if (vt->y > 0) vt->y--;
}

// MARK: - Cursor

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void move_to(Vt *vt, int x, int y) {
    vt->wrap_pending = false;
    int lo = vt->origin ? vt->top : 0, hi = vt->origin ? vt->bottom : vt->rows - 1;
    vt->x = clampi(x, 0, vt->cols - 1);
    vt->y = clampi(y, lo, hi);
}
/// Up or down without leaving the scroll region when the cursor starts inside it.
static void move_rows(Vt *vt, int dy) {
    int y = vt->y + dy, lo = 0, hi = vt->rows - 1;
    if (vt->y >= vt->top && vt->y <= vt->bottom) { lo = vt->top; hi = vt->bottom; }
    vt->wrap_pending = false;
    vt->y = clampi(y, lo, hi);
}

static void save_cursor(Vt *vt) {
    Saved *s = vt->grid == vt->alt ? &vt->saved_alt : &vt->saved;
    s->x = vt->x; s->y = vt->y; s->pen = vt->pen; s->origin = vt->origin; s->autowrap = vt->autowrap;
    s->g0_lines = vt->g0_lines; s->g1_lines = vt->g1_lines; s->shift_out = vt->shift_out;
}
static void restore_cursor(Vt *vt) {
    Saved *s = vt->grid == vt->alt ? &vt->saved_alt : &vt->saved;
    vt->pen = s->pen; vt->origin = s->origin; vt->autowrap = s->autowrap;
    vt->g0_lines = s->g0_lines; vt->g1_lines = s->g1_lines; vt->shift_out = s->shift_out;
    vt->x = clampi(s->x, 0, vt->cols - 1); vt->y = clampi(s->y, 0, vt->rows - 1);
    vt->wrap_pending = false;
}

static void set_alt(Vt *vt, bool on, bool save, bool clear) {
    if (on == (vt->grid == vt->alt)) return;
    if (on) {
        if (save) save_cursor(vt);
        vt->grid = vt->alt;
        if (clear) fill_cells(vt->alt, vt->cols * vt->rows, blank(vt));
    } else {
        if (clear) fill_cells(vt->alt, vt->cols * vt->rows, blank(vt));
        vt->grid = vt->main;
        if (save) restore_cursor(vt);
    }
    vt->wrap_pending = false;
}

// MARK: - Printing

/// DEC special graphics: the line-drawing set `ESC ( 0` selects, for 0x60...0x7E.
static uint32_t dec_graphic(uint32_t c) {
    static const uint16_t map[31] = {
        0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
        0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3,
        0x00B7,
    };
    return c >= 0x60 && c <= 0x7E ? map[c - 0x60] : c;
}

static void put_char(Vt *vt, uint32_t c) {
    if ((vt->shift_out ? vt->g1_lines : vt->g0_lines) && c < 0x80) c = dec_graphic(c);
    int w = vt_char_width(c);
    if (w == 0) return;
    if (vt->wrap_pending) {
        if (vt->autowrap) { vt->x = 0; linefeed(vt); }
        vt->wrap_pending = false;
    }
    if (w == 2 && vt->x == vt->cols - 1) {
        // A wide character never straddles the edge: it wraps whole, or overwrites the last cell when wrapping is off.
        if (vt->autowrap) { row_at(vt, vt->y)[vt->x] = blank(vt); vt->x = 0; linefeed(vt); }
        else w = 1;
    }
    VtCell *row = row_at(vt, vt->y);
    if (vt->insert) {
        int n = vt->cols - vt->x - w;
        if (n > 0) memmove(row + vt->x + w, row + vt->x, (size_t)n * sizeof *row);
    }
    // Overwriting half of a wide character blanks its other half.
    if (row[vt->x].attr & VT_WIDE_TAIL && vt->x > 0) row[vt->x - 1] = blank(vt);
    if (row[vt->x].attr & VT_WIDE && vt->x + 1 < vt->cols) row[vt->x + 1] = blank(vt);
    VtCell cell = vt->pen;
    cell.ch = c;
    cell.attr &= (uint16_t)~(VT_WIDE | VT_WIDE_TAIL);
    if (w == 2) {
        if (vt->x + 2 < vt->cols && row[vt->x + 1].attr & VT_WIDE) row[vt->x + 2] = blank(vt);
        cell.attr |= VT_WIDE;
        row[vt->x] = cell;
        VtCell tail = cell; tail.ch = ' '; tail.attr = (uint16_t)((cell.attr & ~VT_WIDE) | VT_WIDE_TAIL);
        row[vt->x + 1] = tail;
    } else row[vt->x] = cell;
    vt->last_char = c;
    if (vt->x + w >= vt->cols) { vt->x = vt->cols - 1; vt->wrap_pending = true; }
    else vt->x += w;
}

// MARK: - Erasing and editing

static void erase_cells(Vt *vt, int y, int x0, int x1) {
    x0 = clampi(x0, 0, vt->cols); x1 = clampi(x1, 0, vt->cols);
    if (x1 > x0) fill_cells(row_at(vt, y) + x0, x1 - x0, blank(vt));
}
static void erase_display(Vt *vt, int mode) {
    switch (mode) {
    case 0:
        erase_cells(vt, vt->y, vt->x, vt->cols);
        for (int y = vt->y + 1; y < vt->rows; y++) erase_cells(vt, y, 0, vt->cols);
        break;
    case 1:
        for (int y = 0; y < vt->y; y++) erase_cells(vt, y, 0, vt->cols);
        erase_cells(vt, vt->y, 0, vt->x + 1);
        break;
    case 2: for (int y = 0; y < vt->rows; y++) erase_cells(vt, y, 0, vt->cols); break;
    case 3: vt_clear_scrollback(vt); break;
    }
}
static void erase_line(Vt *vt, int mode) {
    if (mode == 0) erase_cells(vt, vt->y, vt->x, vt->cols);
    else if (mode == 1) erase_cells(vt, vt->y, 0, vt->x + 1);
    else if (mode == 2) erase_cells(vt, vt->y, 0, vt->cols);
}
static void insert_chars(Vt *vt, int n) {
    VtCell *row = row_at(vt, vt->y);
    n = clampi(n, 1, vt->cols - vt->x);
    memmove(row + vt->x + n, row + vt->x, (size_t)(vt->cols - vt->x - n) * sizeof *row);
    fill_cells(row + vt->x, n, blank(vt));
    vt->wrap_pending = false;
}
static void delete_chars(Vt *vt, int n) {
    VtCell *row = row_at(vt, vt->y);
    n = clampi(n, 1, vt->cols - vt->x);
    memmove(row + vt->x, row + vt->x + n, (size_t)(vt->cols - vt->x - n) * sizeof *row);
    fill_cells(row + vt->cols - n, n, blank(vt));
    vt->wrap_pending = false;
}
static void insert_lines(Vt *vt, int n) {
    if (vt->y < vt->top || vt->y > vt->bottom) return;
    scroll_down(vt, vt->y, vt->bottom, n);
    vt->x = 0; vt->wrap_pending = false;
}
static void delete_lines(Vt *vt, int n) {
    if (vt->y < vt->top || vt->y > vt->bottom) return;
    // Deleting lines inside the screen never feeds the scrollback, even from its first line.
    int height = vt->bottom - vt->y + 1;
    if (n > height) n = height;
    memmove(row_at(vt, vt->y), row_at(vt, vt->y + n), (size_t)(height - n) * vt->cols * sizeof(VtCell));
    fill_cells(row_at(vt, vt->bottom - n + 1), n * vt->cols, blank(vt));
    vt->x = 0; vt->wrap_pending = false;
}

// MARK: - SGR

static uint32_t extended_color(const int *p, int n, int *used) {
    // 38;5;N or 38;2;R;G;B, the index past what was read in `*used`.
    if (n >= 3 && p[1] == 5) { *used = 2; return VT_COLOR_INDEX | (uint32_t)clampi(p[2], 0, 255); }
    if (n >= 2 && p[1] == 2) {
        *used = 4;
        int r = n > 2 ? clampi(p[2], 0, 255) : 0, g = n > 3 ? clampi(p[3], 0, 255) : 0, b = n > 4 ? clampi(p[4], 0, 255) : 0;
        return VT_COLOR_RGB | (uint32_t)(r << 16 | g << 8 | b);
    }
    *used = n - 1;
    return VT_COLOR_DEFAULT;
}
static void sgr(Vt *vt) {
    int n = vt->nparams ? vt->nparams : 1;
    for (int i = 0; i < n; i++) {
        int p = vt->nparams ? vt->params[i] : 0;
        VtCell *pen = &vt->pen;
        if (p < 0) p = 0;
        switch (p) {
        case 0: pen->attr = 0; pen->fg = pen->bg = VT_COLOR_DEFAULT; break;
        case 1: pen->attr |= VT_BOLD; break;
        case 2: pen->attr |= VT_DIM; break;
        case 3: pen->attr |= VT_ITALIC; break;
        case 4: case 21: pen->attr |= VT_UNDERLINE; break;
        case 7: pen->attr |= VT_INVERSE; break;
        case 8: pen->attr |= VT_HIDDEN; break;
        case 9: pen->attr |= VT_STRIKE; break;
        case 22: pen->attr &= (uint16_t)~(VT_BOLD | VT_DIM); break;
        case 23: pen->attr &= (uint16_t)~VT_ITALIC; break;
        case 24: pen->attr &= (uint16_t)~VT_UNDERLINE; break;
        case 27: pen->attr &= (uint16_t)~VT_INVERSE; break;
        case 28: pen->attr &= (uint16_t)~VT_HIDDEN; break;
        case 29: pen->attr &= (uint16_t)~VT_STRIKE; break;
        case 39: pen->fg = VT_COLOR_DEFAULT; break;
        case 49: pen->bg = VT_COLOR_DEFAULT; break;
        case 38: case 48: {
            int used;
            uint32_t c = extended_color(vt->params + i, n - i, &used);
            if (p == 38) pen->fg = c; else pen->bg = c;
            i += used;
            break;
        }
        default:
            if (p >= 30 && p <= 37) pen->fg = VT_COLOR_INDEX | (uint32_t)(p - 30);
            else if (p >= 40 && p <= 47) pen->bg = VT_COLOR_INDEX | (uint32_t)(p - 40);
            else if (p >= 90 && p <= 97) pen->fg = VT_COLOR_INDEX | (uint32_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) pen->bg = VT_COLOR_INDEX | (uint32_t)(p - 100 + 8);
            break;
        }
    }
}

// MARK: - Modes

static void set_mode(Vt *vt, bool on) {
    for (int i = 0; i < (vt->nparams ? vt->nparams : 0); i++) {
        int p = vt->params[i];
        if (vt->private_mark == '?') {
            switch (p) {
            case 1: vt->app_cursor = on; break;
            case 6: vt->origin = on; move_to(vt, 0, on ? vt->top : 0); break;
            case 7: vt->autowrap = on; if (!on) vt->wrap_pending = false; break;
            case 25: vt->cursor_visible = on; break;
            case 47: case 1047: set_alt(vt, on, false, p == 1047 && !on); break;
            case 1048: if (on) save_cursor(vt); else restore_cursor(vt); break;
            case 1049: set_alt(vt, on, true, true); break;
            case 2004: vt->bracketed = on; break;
            }
        } else if (!vt->private_mark) {
            if (p == 4) vt->insert = on;
            else if (p == 20) vt->newline_mode = on;
        }
    }
}

static void device_status(Vt *vt) {
    int p = vt->nparams ? vt->params[0] : 0;
    if (p == 5) str_appendz(&vt->response, "\x1b[0n");
    else if (p == 6) {
        int y = vt->y + 1 - (vt->origin ? vt->top : 0);
        str_appendf(&vt->response, "\x1b[%d;%dR", y, (vt->x < vt->cols ? vt->x : vt->cols - 1) + 1);
    }
}

static void soft_reset(Vt *vt) {
    bool alt = vt->grid == vt->alt;
    reset_modes(vt);
    vt->grid = alt ? vt->alt : vt->main;
    vt->saved.pen = vt->pen; vt->saved.x = vt->saved.y = 0;
}
static void full_reset(Vt *vt) {
    vt->grid = vt->main;
    reset_modes(vt);
    reset_tabs(vt);
    fill_cells(vt->main, vt->cols * vt->rows, blank(vt));
    fill_cells(vt->alt, vt->cols * vt->rows, blank(vt));
    vt_clear_scrollback(vt);
    vt->x = vt->y = 0;
    memset(&vt->saved, 0, sizeof vt->saved); vt->saved.pen = vt->pen; vt->saved.autowrap = true;
    vt->saved_alt = vt->saved;
}

// MARK: - CSI

static int param(const Vt *vt, int i, int fallback) {
    int p = i < vt->nparams ? vt->params[i] : -1;
    return p <= 0 ? fallback : p;
}
static void csi_dispatch(Vt *vt, char final) {
    if (vt->private_mark == '?' && final != 'h' && final != 'l') return;
    if (vt->private_mark == '>' ) {
        // Secondary device attributes: a VT220-class terminal, version 10.
        if (final == 'c') str_appendz(&vt->response, "\x1b[>1;10;0c");
        return;
    }
    if (vt->private_mark && vt->private_mark != '?') return;
    if (vt->inter == '!' && final == 'p') { soft_reset(vt); return; }
    if (vt->inter) return;   // DECSCUSR (` q`) and the rest change nothing that is drawn
    int n = param(vt, 0, 1);
    switch (final) {
    case '@': insert_chars(vt, n); break;
    case 'A': move_rows(vt, -n); break;
    case 'B': case 'e': move_rows(vt, n); break;
    case 'C': case 'a': move_to(vt, vt->x + n, vt->y); break;
    case 'D': move_to(vt, (vt->x < vt->cols ? vt->x : vt->cols - 1) - n, vt->y); break;
    case 'E': move_rows(vt, n); vt->x = 0; break;
    case 'F': move_rows(vt, -n); vt->x = 0; break;
    case 'G': case '`': move_to(vt, n - 1, vt->y); break;
    case 'H': case 'f': move_to(vt, param(vt, 1, 1) - 1, n - 1 + (vt->origin ? vt->top : 0)); break;
    case 'd': move_to(vt, vt->x, n - 1 + (vt->origin ? vt->top : 0)); break;
    case 'I': for (int i = 0; i < n; i++) { int x = vt->x + 1; while (x < vt->cols - 1 && !vt->tabs[x]) x++; move_to(vt, x, vt->y); } break;
    case 'Z': for (int i = 0; i < n; i++) { int x = vt->x - 1; while (x > 0 && !vt->tabs[x]) x--; move_to(vt, x, vt->y); } break;
    case 'J': erase_display(vt, param(vt, 0, 0)); break;
    case 'K': erase_line(vt, param(vt, 0, 0)); break;
    case 'L': insert_lines(vt, n); break;
    case 'M': delete_lines(vt, n); break;
    case 'P': delete_chars(vt, n); break;
    case 'S': scroll_up(vt, vt->top, vt->bottom, n); break;
    case 'T': scroll_down(vt, vt->top, vt->bottom, n); break;
    case 'X': erase_cells(vt, vt->y, vt->x, vt->x + n); vt->wrap_pending = false; break;
    case 'b': if (vt->last_char) for (int i = 0; i < n && i < 65535; i++) put_char(vt, vt->last_char); break;
    case 'c': if (param(vt, 0, 0) == 0) str_appendz(&vt->response, "\x1b[?62;22c"); break;
    case 'g': if (param(vt, 0, 0) == 3) memset(vt->tabs, 0, (size_t)vt->cols); else if (vt->x < vt->cols) vt->tabs[vt->x] = false; break;
    case 'h': set_mode(vt, true); break;
    case 'l': set_mode(vt, false); break;
    case 'm': sgr(vt); break;
    case 'n': device_status(vt); break;
    case 'r': {
        int top = param(vt, 0, 1) - 1, bottom = param(vt, 1, vt->rows) - 1;
        if (bottom >= vt->rows) bottom = vt->rows - 1;
        if (top < bottom) { vt->top = top; vt->bottom = bottom; move_to(vt, 0, vt->origin ? top : 0); }
        break;
    }
    case 's': save_cursor(vt); break;
    case 'u': restore_cursor(vt); break;
    }
}

// MARK: - OSC

static void osc_dispatch(Vt *vt) {
    if (!vt->osc) return;
    vt->osc[vt->osc_len] = 0;
    char *semi = strchr(vt->osc, ';');
    if (!semi) return;
    *semi = 0;
    if (str_eq(vt->osc, "0") || str_eq(vt->osc, "2")) {
        if (!str_eq(vt->title, semi + 1)) { free(vt->title); vt->title = xstrdup(semi + 1); vt->title_changed = true; }
    }
}
static void osc_put(Vt *vt, char c) {
    if (!vt->osc) vt->osc = xmalloc(OSC_MAX + 1);
    if (vt->osc_len < OSC_MAX) vt->osc[vt->osc_len++] = c;
}

// MARK: - The parser

static void csi_begin(Vt *vt) {
    vt->state = S_CSI;
    vt->nparams = 0; vt->private_mark = 0; vt->inter = 0;
}

static void control(Vt *vt, uint32_t c) {
    switch (c) {
    case 0x07: vt->bell = true; break;
    case 0x08: if (vt->wrap_pending) vt->wrap_pending = false; else if (vt->x > 0) vt->x--; break;
    case 0x09: { int x = vt->x + 1; while (x < vt->cols - 1 && !vt->tabs[x]) x++; vt->x = clampi(x, 0, vt->cols - 1); vt->wrap_pending = false; break; }
    case 0x0A: case 0x0B: case 0x0C: linefeed(vt); if (vt->newline_mode) vt->x = 0; break;
    case 0x0D: vt->x = 0; vt->wrap_pending = false; break;
    case 0x0E: vt->shift_out = true; break;
    case 0x0F: vt->shift_out = false; break;
    }
}

static void esc_dispatch(Vt *vt, uint32_t c) {
    vt->state = S_GROUND;
    switch (c) {
    case '[': csi_begin(vt); break;
    case ']': vt->state = S_OSC; vt->osc_len = 0; break;
    case 'P': case 'X': case '^': case '_': vt->state = S_STRING; break;
    case '(': case ')': vt->state = S_CHARSET; vt->charset_target = c == '(' ? 0 : 1; break;
    case '*': case '+': vt->state = S_CHARSET; vt->charset_target = 2; break;
    case '#': case '%': case ' ': vt->state = S_ESC_INTER; break;
    case '7': save_cursor(vt); break;
    case '8': restore_cursor(vt); break;
    case 'D': linefeed(vt); break;
    case 'E': vt->x = 0; linefeed(vt); break;
    case 'H': if (vt->x < vt->cols) vt->tabs[vt->x] = true; break;
    case 'M': reverse_index(vt); break;
    case 'c': full_reset(vt); break;
    case '=': vt->app_keypad = true; break;
    case '>': vt->app_keypad = false; break;
    }
}

static void feed(Vt *vt, uint32_t c) {
    // CAN and SUB abandon a sequence; ESC starts a new one from anywhere but inside a string, where it may begin ST.
    if (c == 0x18 || c == 0x1A) { vt->state = S_GROUND; return; }
    switch (vt->state) {
    case S_GROUND:
        if (c == 0x1B) vt->state = S_ESC;
        else if (c < 0x20 || c == 0x7F) control(vt, c);
        else if (c >= 0x80 && c < 0xA0) { if (c == 0x9B) csi_begin(vt); else if (c == 0x9D) { vt->state = S_OSC; vt->osc_len = 0; } }
        else put_char(vt, c);
        break;
    case S_ESC:
        if (c == 0x1B) break;
        if (c < 0x20) { control(vt, c); break; }
        esc_dispatch(vt, c);
        break;
    case S_ESC_INTER:
        if (c >= 0x30) vt->state = S_GROUND;
        break;
    case S_CHARSET:
        if (vt->charset_target == 0) vt->g0_lines = c == '0';
        else if (vt->charset_target == 1) vt->g1_lines = c == '0';
        vt->state = S_GROUND;
        break;
    case S_CSI:
        if (c == 0x1B) { vt->state = S_ESC; break; }
        if (c < 0x20) { control(vt, c); break; }
        if (c >= '0' && c <= '9') {
            if (!vt->nparams) { vt->nparams = 1; vt->params[0] = -1; }
            int *p = &vt->params[vt->nparams - 1];
            if (*p < 0) *p = 0;
            if (*p < 100000) *p = *p * 10 + (int)(c - '0');
        } else if (c == ';' || c == ':') {
            // An empty parameter counts, so `CSI ;5H` is row default, column 5; -1 marks one left empty.
            if (!vt->nparams) { vt->nparams = 1; vt->params[0] = -1; }
            if (vt->nparams < MAX_PARAMS) vt->params[vt->nparams++] = -1;
        } else if (c >= 0x3C && c <= 0x3F) { if (!vt->nparams) vt->private_mark = (char)c; }
        else if (c >= 0x20 && c <= 0x2F) vt->inter = (char)c;
        else if (c >= 0x40 && c <= 0x7E) {
            for (int i = 0; i < vt->nparams; i++) if (vt->params[i] < 0) vt->params[i] = 0;
            vt->state = S_GROUND;
            csi_dispatch(vt, (char)c);
        } else vt->state = S_GROUND;
        break;
    case S_OSC:
        if (c == 0x07 || c == 0x9C) { osc_dispatch(vt); vt->state = S_GROUND; }
        else if (c == 0x1B) vt->state = S_OSC_ESC;
        else if (c >= 0x20) {
            char buf[4]; int n = 0;
            if (c < 0x80) buf[n++] = (char)c;
            else if (c < 0x800) { buf[n++] = (char)(0xC0 | c >> 6); buf[n++] = (char)(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { buf[n++] = (char)(0xE0 | c >> 12); buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (c & 0x3F)); }
            else { buf[n++] = (char)(0xF0 | c >> 18); buf[n++] = (char)(0x80 | ((c >> 12) & 0x3F)); buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (c & 0x3F)); }
            for (int i = 0; i < n; i++) osc_put(vt, buf[i]);
        }
        break;
    case S_OSC_ESC:
        if (c == '\\') { osc_dispatch(vt); vt->state = S_GROUND; }
        else { vt->state = S_ESC; feed(vt, c); }
        break;
    case S_STRING:
        if (c == 0x1B) vt->state = S_STRING_ESC;
        else if (c == 0x9C || c == 0x07) vt->state = S_GROUND;
        break;
    case S_STRING_ESC:
        vt->state = c == '\\' ? S_GROUND : S_STRING;
        break;
    }
}

void vt_write(Vt *vt, const char *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = p[i];
        if (vt->utf8_need) {
            if ((b & 0xC0) == 0x80) {
                vt->utf8_cp = vt->utf8_cp << 6 | (b & 0x3F);
                if (--vt->utf8_need == 0) feed(vt, vt->utf8_cp >= 0x80 && vt->utf8_cp <= 0x10FFFF && !(vt->utf8_cp >= 0xD800 && vt->utf8_cp <= 0xDFFF) ? vt->utf8_cp : 0xFFFD);
                continue;
            }
            // A sequence cut short: what it had becomes one replacement character and this byte starts afresh.
            vt->utf8_need = 0;
            feed(vt, 0xFFFD);
        }
        if (b < 0x80) feed(vt, b);
        else if ((b & 0xE0) == 0xC0) { vt->utf8_cp = b & 0x1F; vt->utf8_need = 1; }
        else if ((b & 0xF0) == 0xE0) { vt->utf8_cp = b & 0x0F; vt->utf8_need = 2; }
        else if ((b & 0xF8) == 0xF0) { vt->utf8_cp = b & 0x07; vt->utf8_need = 3; }
        else feed(vt, 0xFFFD);
    }
}

// MARK: - Resizing

static VtCell *regrid(const Vt *vt, const VtCell *old, int cols, int rows, int shift) {
    VtCell *grid = xmalloc((size_t)cols * rows * sizeof *grid);
    VtCell b = { ' ', VT_COLOR_DEFAULT, VT_COLOR_DEFAULT, 0 };
    fill_cells(grid, cols * rows, b);
    int keep = cols < vt->cols ? cols : vt->cols;
    for (int y = 0; y < rows; y++) {
        int from = y + shift;
        if (from < 0 || from >= vt->rows) continue;
        memcpy(grid + (size_t)y * cols, old + (size_t)from * vt->cols, (size_t)keep * sizeof *grid);
        // A wide character cut in half at the new edge goes.
        if (keep < vt->cols && keep > 0 && grid[(size_t)y * cols + keep - 1].attr & VT_WIDE) grid[(size_t)y * cols + keep - 1] = b;
    }
    return grid;
}

void vt_resize(Vt *vt, int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == vt->cols && rows == vt->rows) return;
    // Lines above a cursor that would fall off the bottom move into the scrollback, as the screen shrinks from the top.
    int shift = vt->y >= rows ? vt->y - rows + 1 : 0;
    if (shift && vt->grid == vt->main) {
        for (int y = 0; y < shift; y++) push_scrollback(vt, vt->main + (size_t)y * vt->cols);
    }
    int alt_shift = vt->grid == vt->alt ? shift : 0, main_shift = vt->grid == vt->main ? shift : 0;
    VtCell *main = regrid(vt, vt->main, cols, rows, main_shift), *alt = regrid(vt, vt->alt, cols, rows, alt_shift);
    bool on_alt = vt->grid == vt->alt;
    free(vt->main); free(vt->alt);
    vt->main = main; vt->alt = alt; vt->grid = on_alt ? alt : main;
    free(vt->tabs);
    vt->tabs = xcalloc((size_t)cols, sizeof *vt->tabs);
    vt->cols = cols; vt->rows = rows;
    reset_tabs(vt);
    vt->top = 0; vt->bottom = rows - 1;
    vt->y = clampi(vt->y - shift, 0, rows - 1);
    vt->x = clampi(vt->x, 0, cols - 1);
    vt->wrap_pending = false;
    vt->saved.x = clampi(vt->saved.x, 0, cols - 1); vt->saved.y = clampi(vt->saved.y - main_shift, 0, rows - 1);
    vt->saved_alt.x = clampi(vt->saved_alt.x, 0, cols - 1); vt->saved_alt.y = clampi(vt->saved_alt.y - alt_shift, 0, rows - 1);
}

// MARK: - Text

static void append_utf8(Str *s, uint32_t c) {
    char buf[4]; int n = 0;
    if (c < 0x80) buf[n++] = (char)c;
    else if (c < 0x800) { buf[n++] = (char)(0xC0 | c >> 6); buf[n++] = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { buf[n++] = (char)(0xE0 | c >> 12); buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (c & 0x3F)); }
    else { buf[n++] = (char)(0xF0 | c >> 18); buf[n++] = (char)(0x80 | ((c >> 12) & 0x3F)); buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (c & 0x3F)); }
    str_append(s, buf, (size_t)n);
}

char *vt_text(const Vt *vt, int y0, int x0, int y1, int x1) {
    Str s; str_init(&s);
    str_appendz(&s, "");
    for (int y = y0; y <= y1; y++) {
        int width = 0;
        const VtCell *row = vt_line(vt, y, &width);
        int from = y == y0 ? x0 : 0, to = y == y1 ? x1 : width;
        if (to > width) to = width;
        size_t line_start = s.len;
        for (int x = from < 0 ? 0 : from; row && x < to; x++) {
            if (row[x].attr & VT_WIDE_TAIL) continue;
            append_utf8(&s, row[x].ch ? row[x].ch : ' ');
        }
        while (s.len > line_start && s.data[s.len - 1] == ' ') s.len--;
        if (s.data) s.data[s.len] = 0;
        if (y < y1) str_appendz(&s, "\r\n");
    }
    return str_detach(&s);
}
