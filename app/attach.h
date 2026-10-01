// Files for the next message, as the dashboard's composer takes them: an image pasted from the clipboard, encoded as PNG,
// or files copied in Explorer and pasted or dropped, read from disk. The reading and encoding happen off the UI thread.
#ifndef BRIAREUS_ATTACH_H
#define BRIAREUS_ATTACH_H
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { char *name; void *bytes; size_t len; } AttachFile;
/// Runs on the UI thread with the files read, which the callee owns, and one line per file that could not be, or NULL.
typedef void (*AttachDone)(void *ctx, AttachFile *files, size_t count, char *error);

typedef struct Attacher Attacher;
Attacher *attacher_new(AttachDone done, void *ctx);
/// A read still under way finishes into nothing.
void attacher_free(Attacher *a);

/// True when the clipboard holds files copied in Explorer or an image.
bool attach_clipboard_has_files(void);
bool attach_clipboard_has_text(void);
/// Starts reading the clipboard's files or image; false when it held neither.
bool attacher_from_clipboard(Attacher *a, HWND owner);
/// Starts reading the files of a drop. Takes the handle.
void attacher_from_drop(Attacher *a, HDROP drop);
/// Starts reading files by path, as the 📎 button's dialog names them. Copies the paths.
void attacher_from_paths(Attacher *a, const wchar_t *const *paths, size_t count);
void attach_files_free(AttachFile *files, size_t count);

/// "12 KB" or "1.3 MB", as a new string.
char *format_file_size(size_t bytes);

#endif
