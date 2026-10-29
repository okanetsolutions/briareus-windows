// Files for the next message: the clipboard's image as PNG, or the files it or a drop names, read from disk off the UI thread.
#include "attach.h"
#include "api.h"
#include "store.h"
#include "str.h"
#include <objidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Windows Imaging Component, by identifier: no import library beyond ole32 is needed.
static const GUID clsid_wic_factory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID iid_wic_factory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID wic_png = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID wic_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

#define MAX_SIDE 16384

struct Attacher { AttachDone done; void *ctx; int refs; size_t pending; volatile LONG cancelled; };

typedef struct {
    Attacher *a;
    wchar_t **paths; size_t path_count;
    unsigned char *pixels; int width, height;   // an image: 32-bit BGRA rows, top-down
    AttachFile *files; size_t count;
    Str errors;
} AttachJob;

static void attacher_release(Attacher *a) { if (--a->refs == 0) free(a); }
Attacher *attacher_new(AttachDone done, void *ctx) { Attacher *a = xcalloc(1, sizeof *a); a->done = done; a->ctx = ctx; a->refs = 1; return a; }
void attacher_free(Attacher *a) { if (!a) return; a->done = NULL; a->ctx = NULL; InterlockedExchange(&a->cancelled, 1); attacher_release(a); }

bool attacher_pending(const Attacher *a) { return a && a->pending > 0; }
void attach_file_clear(AttachFile *f) { free(f->name); free(f->bytes); free(f->path); free(f->pixels); memset(f, 0, sizeof *f); }
void attach_files_free(AttachFile *files, size_t count) {
    for (size_t i = 0; i < count; i++) attach_file_clear(&files[i]);
    free(files);
}
char *format_file_size(size_t bytes) {
    if (bytes >= 1024 * 1024) return xstrfmt("%.1f MB", bytes / (1024.0 * 1024.0));
    size_t kb = (bytes + 1023) / 1024;
    return xstrfmt("%zu KB", kb ? kb : 1);
}

static AttachJob *job_new(Attacher *a) { AttachJob *job = xcalloc(1, sizeof *job); job->a = a; a->refs++; a->pending++; str_init(&job->errors); return job; }
static void job_free(AttachJob *job) {
    for (size_t i = 0; i < job->path_count; i++) free(job->paths[i]);
    free(job->paths); free(job->pixels); str_free(&job->errors);
    attacher_release(job->a);
    free(job);
}
static void push_file(AttachJob *job, char *name, void *bytes, size_t len) {
    job->files = xrealloc(job->files, (job->count + 1) * sizeof *job->files);
    memset(&job->files[job->count], 0, sizeof *job->files);
    job->files[job->count].name = name; job->files[job->count].bytes = bytes; job->files[job->count].len = len;
    job->count++;
}
static void refuse(AttachJob *job, const char *name, const char *why) { str_appendf(&job->errors, "%s%s %s.", job->errors.len ? "\n" : "", name, why); }

// MARK: - What the UI thread takes

static void take_drop(HDROP drop, AttachJob *job) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < n; i++) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        if (!len) continue;
        wchar_t *path = xmalloc(((size_t)len + 1) * sizeof *path);
        DragQueryFileW(drop, i, path, len + 1);
        job->paths = xrealloc(job->paths, (job->path_count + 1) * sizeof *job->paths);
        job->paths[job->path_count++] = path;
    }
}
static bool dibv5_has_rgba(const BITMAPV5HEADER *hdr, SIZE_T size) {
    if (size < sizeof *hdr || hdr->bV5Size < sizeof *hdr) return false;
    if (hdr->bV5BitCount != 32 || hdr->bV5Planes != 1 || hdr->bV5Width <= 0 || hdr->bV5Height == 0) return false;
    if (hdr->bV5AlphaMask != 0xFF000000) return false;
    if (hdr->bV5Compression == BI_RGB) return true;
    return hdr->bV5Compression == BI_BITFIELDS && hdr->bV5RedMask == 0x00FF0000
        && hdr->bV5GreenMask == 0x0000FF00 && hdr->bV5BlueMask == 0x000000FF;
}
// A 32-bit DIB with an alpha channel keeps it: an image copied from a browser may have a transparent background.
static bool take_dibv5(HGLOBAL h, AttachJob *job) {
    SIZE_T size = GlobalSize(h);
    const BITMAPV5HEADER *hdr = GlobalLock(h);
    if (!hdr) return false;
    bool ok = false;
    if (dibv5_has_rgba(hdr, size)) {
        int w = hdr->bV5Width, hgt = hdr->bV5Height < 0 ? -hdr->bV5Height : hdr->bV5Height;
        bool bottom_up = hdr->bV5Height > 0;
        size_t offset = hdr->bV5Size + (size_t)hdr->bV5ClrUsed * 4, stride = (size_t)w * 4;
        if (w <= MAX_SIDE && hgt <= MAX_SIDE && offset + stride * hgt <= size) {
            const unsigned char *bits = (const unsigned char *)hdr + offset;
            unsigned char *px = xmalloc(stride * hgt);
            for (int y = 0; y < hgt; y++) memcpy(px + (size_t)y * stride, bits + (size_t)(bottom_up ? hgt - 1 - y : y) * stride, stride);
            // An alpha channel that is zero throughout is no channel: the image is opaque.
            bool any = false;
            for (size_t i = 3; i < stride * hgt && !any; i += 4) any = px[i] != 0;
            if (!any) for (size_t i = 3; i < stride * hgt; i += 4) px[i] = 255;
            job->pixels = px; job->width = w; job->height = hgt; ok = true;
        }
    }
    GlobalUnlock(h);
    return ok;
}
// Any other bitmap, opaque, through GDI.
static bool take_bitmap(HBITMAP hbm, AttachJob *job) {
    BITMAP bm;
    if (!GetObjectW(hbm, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth > MAX_SIDE || bm.bmHeight > MAX_SIDE) return false;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = bm.bmWidth; bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    size_t stride = (size_t)bm.bmWidth * 4;
    unsigned char *px = xmalloc(stride * bm.bmHeight);
    HDC hdc = GetDC(NULL);
    int lines = GetDIBits(hdc, hbm, 0, bm.bmHeight, px, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);
    if (lines != bm.bmHeight) { free(px); return false; }
    for (size_t i = 3; i < stride * bm.bmHeight; i += 4) px[i] = 255;   // GetDIBits leaves the fourth byte as it found it
    job->pixels = px; job->width = bm.bmWidth; job->height = bm.bmHeight;
    return true;
}

// MARK: - The worker

static bool cancelled(volatile LONG *flag) { return InterlockedCompareExchange(flag, 0, 0) != 0; }

// Check using the opened handle too: the path can change between enumeration and opening.
static const char *open_path(const wchar_t *path, HANDLE *out, size_t *len) {
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return "could not be read";
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return "is a folder; attach its files";
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return "could not be opened";
    BY_HANDLE_FILE_INFORMATION info = {0};
    LARGE_INTEGER size = {0};
    const char *why = NULL;
    if (!GetFileInformationByHandle(f, &info) || !GetFileSizeEx(f, &size)) why = "could not be read";
    else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) why = "is a folder; attach its files";
    else if (GetFileType(f) != FILE_TYPE_DISK || size.QuadPart < 0) why = "is not a regular file";
    else if (size.QuadPart == 0) why = "is empty";
    else if (size.QuadPart > API_UPLOAD_LIMIT) why = "is larger than 500 MiB, the most a message takes";
    if (why) { CloseHandle(f); return why; }
    *out = f; *len = (size_t)size.QuadPart;
    return NULL;
}
static void check_path(AttachJob *job, const wchar_t *path) {
    const wchar_t *slash = wcsrchr(path, L'\\'), *fwd = wcsrchr(path, L'/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    char *name = wide_to_utf8(slash ? slash + 1 : path);
    HANDLE f; size_t len;
    const char *why = open_path(path, &f, &len);
    if (why) { refuse(job, name, why); free(name); return; }
    CloseHandle(f);
    push_file(job, name, NULL, len);
    job->files[job->count - 1].path = _wcsdup(path);
}

static bool encode_png(const unsigned char *pixels, int w, int h, void **out, size_t *out_len) {
    bool ok = false;
    IWICImagingFactory *f = NULL; IStream *stream = NULL; IWICBitmapEncoder *enc = NULL; IWICBitmapFrameEncode *frame = NULL; IPropertyBag2 *props = NULL;
    WICPixelFormatGUID fmt = wic_bgra;
    UINT stride = (UINT)w * 4;
    LARGE_INTEGER zero = { { 0, 0 } }; ULARGE_INTEGER end = { { 0, 0 } };
    unsigned char *bytes = NULL; ULONG got = 0;
    if (FAILED(CoCreateInstance(&clsid_wic_factory, NULL, CLSCTX_INPROC_SERVER, &iid_wic_factory, (void **)&f))) goto out;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) goto out;
    if (FAILED(f->lpVtbl->CreateEncoder(f, &wic_png, NULL, &enc))) goto out;
    if (FAILED(enc->lpVtbl->Initialize(enc, stream, WICBitmapEncoderNoCache))) goto out;
    if (FAILED(enc->lpVtbl->CreateNewFrame(enc, &frame, &props))) goto out;
    if (FAILED(frame->lpVtbl->Initialize(frame, props))) goto out;
    if (FAILED(frame->lpVtbl->SetSize(frame, (UINT)w, (UINT)h))) goto out;
    if (FAILED(frame->lpVtbl->SetPixelFormat(frame, &fmt)) || !IsEqualGUID(&fmt, &wic_bgra)) goto out;
    if (FAILED(frame->lpVtbl->WritePixels(frame, (UINT)h, stride, stride * (UINT)h, (BYTE *)pixels))) goto out;
    if (FAILED(frame->lpVtbl->Commit(frame)) || FAILED(enc->lpVtbl->Commit(enc))) goto out;
    if (FAILED(stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_END, &end)) || end.QuadPart == 0 || end.QuadPart > API_UPLOAD_LIMIT) goto out;
    if (FAILED(stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL))) goto out;
    bytes = xmalloc((size_t)end.QuadPart);
    if (FAILED(stream->lpVtbl->Read(stream, bytes, (ULONG)end.QuadPart, &got)) || got != end.QuadPart) { free(bytes); goto out; }
    *out = bytes; *out_len = (size_t)end.QuadPart; ok = true;
out:
    if (props) props->lpVtbl->Release(props);
    if (frame) frame->lpVtbl->Release(frame);
    if (enc) enc->lpVtbl->Release(enc);
    if (stream) stream->lpVtbl->Release(stream);
    if (f) f->lpVtbl->Release(f);
    return ok;
}

char *attach_file_read(AttachFile *file, volatile LONG *stop) {
    if (cancelled(stop)) return NULL;
    const char *why = NULL;
    if (file->path) {
        HANDLE f; size_t len;
        why = open_path(file->path, &f, &len);
        if (!why) {
            size_t got = 0;
            unsigned char *bytes = xmalloc(len);
            while (got < len && !cancelled(stop)) {
                DWORD chunk = 0, want = (DWORD)(len - got > (1u << 20) ? (1u << 20) : len - got);
                if (!ReadFile(f, bytes + got, want, &chunk, NULL) || !chunk) break;
                got += chunk;
            }
            // Writers/deletion are excluded for the lifetime of this handle; detect an unexpected length anyway.
            LARGE_INTEGER end;
            bool same = GetFileSizeEx(f, &end) && end.QuadPart == (LONGLONG)len;
            CloseHandle(f);
            if (got == len && same && !cancelled(stop)) { file->bytes = bytes; file->len = len; }
            else { free(bytes); why = "could not be read"; }
        }
    } else if (file->pixels) {
        if (!encode_png(file->pixels, file->width, file->height, &file->bytes, &file->len)) why = "could not be encoded as PNG";
        free(file->pixels); file->pixels = NULL;
    }
    return why ? xstrfmt("%s %s.", file->name, why) : NULL;
}

static void attach_work(void *p) {
    AttachJob *job = p;
    for (size_t i = 0; i < job->path_count && !cancelled(&job->a->cancelled); i++) check_path(job, job->paths[i]);
    if (job->pixels && !cancelled(&job->a->cancelled)) {
        char name[48]; time_t now = time(NULL); struct tm *lt = localtime(&now);
        strftime(name, sizeof name, "pasted-%Y%m%d%H%M%S.png", lt);
        push_file(job, xstrdup(name), NULL, 0);
        AttachFile *file = &job->files[job->count - 1];
        file->pixels = job->pixels; job->pixels = NULL;
        file->width = job->width; file->height = job->height;
    }
}
static void attach_done(void *p) {
    AttachJob *job = p;
    Attacher *a = job->a;
    a->pending--;
    if (a->done) a->done(a->ctx, job->files, job->count, job->errors.len ? xstrdup(job->errors.data) : NULL);
    else attach_files_free(job->files, job->count);
    job_free(job);
}

static void start_selection(AttachJob *job) {
    if (async_try_run(attach_work, attach_done, job)) return;
    refuse(job, "The files", "could not be checked because the worker could not be started");
    attach_done(job);
}

// MARK: - API

bool attach_clipboard_has_files(void) {
    return IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP);
}
bool attach_clipboard_has_text(void) { return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT); }

bool attacher_from_clipboard(Attacher *a, HWND owner) {
    if (!attach_clipboard_has_files() || !OpenClipboard(owner)) return false;
    AttachJob *job = job_new(a);
    bool got = false;
    HANDLE h;
    // Files copied in Explorer come first: an Explorer copy carries no image, and a browser's image no files.
    if ((h = GetClipboardData(CF_HDROP)) != NULL) { take_drop((HDROP)h, job); got = job->path_count > 0; }
    else {
        if ((h = GetClipboardData(CF_DIBV5)) != NULL) got = take_dibv5(h, job);
        if (!got && (h = GetClipboardData(CF_BITMAP)) != NULL) got = take_bitmap((HBITMAP)h, job);
    }
    CloseClipboard();
    if (!got) { a->pending--; job_free(job); return false; }
    start_selection(job);
    return true;
}
void attacher_from_drop(Attacher *a, HDROP drop) {
    AttachJob *job = job_new(a);
    take_drop(drop, job);
    DragFinish(drop);
    if (!job->path_count) { a->pending--; job_free(job); return; }
    start_selection(job);
}
void attacher_from_paths(Attacher *a, const wchar_t *const *paths, size_t count) {
    if (!count) return;
    AttachJob *job = job_new(a);
    job->paths = xmalloc(count * sizeof *job->paths);
    for (size_t i = 0; i < count; i++) job->paths[job->path_count++] = _wcsdup(paths[i]);
    start_selection(job);
}
