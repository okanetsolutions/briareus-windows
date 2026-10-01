// A terminal emulator's state, without a window: the xterm subset that ConPTY and the programs behind ssh write (cursor
// moves, erases, scroll regions, SGR colours, the alternate screen, DEC line drawing, OSC titles), into a grid of cells
// with a scrollback above it. The app's terminal window paints the grid and feeds it what the pseudoconsole prints.
#ifndef BRIAREUS_VT_H
#define BRIAREUS_VT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// A colour: the terminal's default, one of the 256 indexed colours, or 24-bit RGB (0xRRGGBB in the low bits).
enum { VT_COLOR_DEFAULT = 0, VT_COLOR_INDEX = 0x01000000u, VT_COLOR_RGB = 0x02000000u };
#define VT_COLOR_KIND(c) ((c) & 0xFF000000u)

enum {
    VT_BOLD = 1, VT_DIM = 2, VT_ITALIC = 4, VT_UNDERLINE = 8, VT_INVERSE = 16, VT_HIDDEN = 32, VT_STRIKE = 64,
    VT_WIDE = 128,        // the first half of a double-width character
    VT_WIDE_TAIL = 256,   // the cell a double-width character's second half covers; never painted on its own
};

typedef struct { uint32_t ch; uint32_t fg, bg; uint16_t attr; } VtCell;

typedef struct Vt Vt;

Vt *vt_new(int cols, int rows, size_t scrollback);
void vt_free(Vt *vt);
/// Feeds bytes as the program printed them; UTF-8 and escape sequences may be split across calls.
void vt_write(Vt *vt, const char *data, size_t len);
/// A new size; the cursor's line stays on screen, lines pushed off the top go to the scrollback.
void vt_resize(Vt *vt, int cols, int rows);
int vt_cols(const Vt *vt);
int vt_rows(const Vt *vt);
/// Lines held above the screen.
int vt_scrollback(const Vt *vt);
/// A line: 0...rows-1 on screen, -1...-scrollback above it. `*width` is how many cells it holds (a scrollback line keeps
/// the width it was written at). NULL past either end.
const VtCell *vt_line(const Vt *vt, int y, int *width);
void vt_cursor(const Vt *vt, int *x, int *y, bool *visible);
/// Bytes the terminal answers with (cursor position and device attribute reports), to write back to the program; the
/// caller frees them. NULL when there is nothing to send.
char *vt_take_response(Vt *vt, size_t *len);
/// The title an OSC 0 or 2 set; NULL when none was.
const char *vt_title(const Vt *vt);
/// Whether the title changed or BEL rang since the last call; clears the flag.
bool vt_take_title_changed(Vt *vt);
bool vt_take_bell(Vt *vt);
/// The modes the keyboard encoding depends on.
bool vt_app_cursor(const Vt *vt);
bool vt_app_keypad(const Vt *vt);
bool vt_bracketed_paste(const Vt *vt);
bool vt_alt_screen(const Vt *vt);
/// Empties the scrollback, keeping the screen.
void vt_clear_scrollback(Vt *vt);
/// The text of a range, row by row, trailing blanks dropped and rows joined with CRLF; rows as for `vt_line`, the end
/// column exclusive. The caller frees it.
char *vt_text(const Vt *vt, int y0, int x0, int y1, int x1);
/// How many cells a code point takes: 0 for combining marks, 2 for East Asian wide characters and emoji, else 1.
int vt_char_width(uint32_t ch);
/// The 256-colour palette's RGB for an index from 16 up (the cube and the greys); the first 16 are the app's to choose.
uint32_t vt_index_rgb(int index);

#endif
