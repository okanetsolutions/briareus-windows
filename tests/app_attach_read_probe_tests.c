// Exercise the public reader with a real disk handle, stopping exactly after its first chunk.
#include <windows.h>
#include "test.h"
#include "suites.h"
#include <stdlib.h>

static volatile LONG *stop_after_chunk;
static DWORD largest_read;
static int chunk_reads;
static BOOL WINAPI observed_read(HANDLE file, LPVOID bytes, DWORD count, LPDWORD got, LPOVERLAPPED overlapped) {
    chunk_reads++; if (count > largest_read) largest_read = count;
    BOOL ok = ReadFile(file, bytes, count, got, overlapped);
    if (stop_after_chunk) InterlockedExchange(stop_after_chunk, 1);
    return ok;
}
#define ReadFile observed_read
#define attacher_new probe_attacher_new
#define attacher_free probe_attacher_free
#define attacher_pending probe_attacher_pending
#define attach_file_clear probe_attach_file_clear
#define attach_files_free probe_attach_files_free
#define attach_file_read probe_attach_file_read
#define attach_clipboard_has_files probe_attach_clipboard_has_files
#define attach_clipboard_has_text probe_attach_clipboard_has_text
#define attacher_from_clipboard probe_attacher_from_clipboard
#define attacher_from_drop probe_attacher_from_drop
#define attacher_from_paths probe_attacher_from_paths
#define format_file_size probe_format_file_size
#include "../app/attach.c"

static void bounded_reads_stop_on_cancellation(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH]; CHECK(GetTempPathW(MAX_PATH, dir) != 0);
    CHECK(GetTempFileNameW(dir, L"arc", 0, path) != 0);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE); LARGE_INTEGER size; size.QuadPart = 1024 * 1024 + 7;
    CHECK(SetFilePointerEx(h, size, NULL, FILE_BEGIN)); CHECK(SetEndOfFile(h)); CloseHandle(h);
    AttachFile file = {.name = xstrdup("chunks"), .path = _wcsdup(path)}; volatile LONG stop = 0;
    stop_after_chunk = &stop; chunk_reads = 0; largest_read = 0;
    char *error = attach_file_read(&file, &stop); free(error);
    CHECK_INT(chunk_reads, 1); CHECK_INT(largest_read, 1048576); CHECK(file.bytes == NULL);
    stop_after_chunk = NULL; InterlockedExchange(&stop, 0); chunk_reads = 0;
    CHECK(attach_file_read(&file, &stop) == NULL); CHECK_INT(chunk_reads, 2); CHECK_INT(file.len, 1048583);
    CHECK_INT(largest_read, 1048576); attach_file_clear(&file); DeleteFileW(path);
}
void app_attach_read_probe_tests(void) {
    test_run("attachment reads bounded chunks and stop during read", bounded_reads_stop_on_cancellation);
}
