// Where the app draws: Direct2D on the GPU, with text laid out by DirectWrite. Panes own a canvas bound to their window;
// the few GDI windows left (the main window's background, the dialogs' frames and buttons) borrow one for a paint.
// The drawing and measuring functions themselves are declared in theme.h.
#ifndef BRIAREUS_CANVAS_H
#define BRIAREUS_CANVAS_H
#include "theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Creates the Direct2D and DirectWrite factories; false when Windows has neither.
bool canvas_startup(void);
/// The DirectWrite format behind a FontId; `size` is in pixels at the current DPI.
void canvas_set_font(FontId id, const wchar_t *face, float size, int weight, bool italic);

/// A canvas drawing straight to a window's client area; its GPU target is made on the first paint.
Canvas *canvas_for_window(HWND hwnd);
void canvas_free(Canvas *cv);
void canvas_resize(Canvas *cv, int width, int height);
/// Starts a frame; false when there is no target to draw on.
bool canvas_begin(Canvas *cv);
/// Ends the frame and shows it; a lost GPU device is dropped and made again on the next paint.
void canvas_end(Canvas *cv);

/// A canvas over a GDI device context for one paint, with (0, 0) at `rc`'s top left. End it with canvas_end_dc.
Canvas *canvas_begin_dc(HDC hdc, const RECT *rc);
void canvas_end_dc(Canvas *cv);

/// Clips drawing to a rectangle, in the canvas's own coordinates; clips nest.
void canvas_clip(Canvas *cv, const RECT *rc);
/// Clips to a rounded rectangle, anti-aliased, as `overflow-hidden rounded-full` does.
void canvas_clip_round(Canvas *cv, const RECT *rc, int radius);
/// Ends the innermost clip.
void canvas_unclip(Canvas *cv);
/// Moves the origin: what is drawn at (0, 0) lands at (dx, dy).
void canvas_offset(Canvas *cv, int dx, int dy);

#ifdef __cplusplus
}
#endif

#endif
