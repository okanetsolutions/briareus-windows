// Files for the next message, as the dashboard's composer takes them: an image pasted from the clipboard, encoded as PNG,
// or files copied in Explorer and pasted or dropped, read from disk. The reading and encoding happen off the UI thread.
#ifndef BRIAREUS_ATTACH_H
#define BRIAREUS_ATTACH_H
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

// Selection owns metadata only; bytes are loaded for one reserved chip at a time.
typedef struct {
    char *name; void *bytes; size_t len;
    wchar_t *path;
    unsigned char *pixels; int width, height;
} AttachFile;
/// Runs on the UI thread with owned metadata/sources and grouped selection errors.
typedef void (*AttachDone)(void *ctx, AttachFile *files, size_t count, char *error);

typedef struct Attacher Attacher;
Attacher *attacher_new(AttachDone done, void *ctx);
/// Cancels metadata checks; any late selection finishes into nothing.
void attacher_free(Attacher *a);

/// True when the clipboard holds files copied in Explorer or an image.
bool attach_clipboard_has_files(void);
bool attach_clipboard_has_text(void);
/// Captures clipboard sources and checks metadata on a worker; false when it held neither.
bool attacher_from_clipboard(Attacher *a, HWND owner);
/// Checks metadata for the files of a drop. Takes the handle.
void attacher_from_drop(Attacher *a, HDROP drop);
/// Checks metadata by path, as the 📎 button's dialog names them. Copies the paths.
void attacher_from_paths(Attacher *a, const wchar_t *const *paths, size_t count);
void attach_file_clear(AttachFile *file);
void attach_files_free(AttachFile *files, size_t count);
/// Worker only: rechecks the current file, then reads bounded chunks; owned error or NULL.
char *attach_file_read(AttachFile *file, volatile LONG *cancelled);
/// UI thread: metadata jobs still in progress, including jobs whose callback is running.
bool attacher_pending(const Attacher *a);

/// "12 KB" or "1.3 MB", as a new string.
char *format_file_size(size_t bytes);

#endif
