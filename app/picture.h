// Picture files read into pixels through the Windows Imaging Component, for draw_picture (theme.h).
#ifndef BRIAREUS_PICTURE_H
#define BRIAREUS_PICTURE_H
#include "theme.h"
#include <stdbool.h>
#include <stddef.h>

/// The pixels a decoded picture may have; a larger file is scaled down to that.
#define PICTURE_MAX_PIXELS (32 * 1000 * 1000)

/// Decodes a PNG, JPEG, GIF (its first frame), BMP or WebP file, on any thread. The picture is scaled down, keeping its
/// shape, to fit `max_w` × `max_h` (0: no such limit) and PICTURE_MAX_PIXELS. `*full_w` × `*full_h` is the file's own
/// size. False when Windows cannot read the file or a side of it is longer than 30000 pixels. Free with picture_free.
bool picture_decode(const void *bytes, size_t len, int max_w, int max_h, Picture *out, int *full_w, int *full_h);
void picture_free(Picture *picture);
/// The size a `w` × `h` picture takes inside `max_w` × `max_h` (0: no limit), keeping its shape and never enlarged.
void picture_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h);

#endif
