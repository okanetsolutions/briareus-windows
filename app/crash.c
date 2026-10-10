#include "crash.h"
#include "resource.h"
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

static wchar_t report_path[MAX_PATH], dump_path[MAX_PATH];
static HANDLE requested, finished;
static MINIDUMP_EXCEPTION_INFORMATION exception;
static volatile LONG handling;

static DWORD WINAPI write_report(void *unused) {
    (void)unused;
    WaitForSingleObject(requested, INFINITE);
    char text[1024];
    int length = snprintf(text, sizeof text,
        "Briareus " APP_VERSION_STRING "\r\nProcess: %lu\r\nThread: %lu\r\nException: 0x%08lX\r\nAddress: %p\r\n",
        GetCurrentProcessId(), exception.ThreadId,
        exception.ExceptionPointers->ExceptionRecord->ExceptionCode,
        exception.ExceptionPointers->ExceptionRecord->ExceptionAddress);
    HANDLE report = CreateFileW(report_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written;
    if (report != INVALID_HANDLE_VALUE && length > 0 && length < (int)sizeof text) {
        WriteFile(report, text, (DWORD)length, &written, NULL);
        FlushFileBuffers(report);
    }
    HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL dumped = FALSE;
    DWORD error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) {
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                  MiniDumpNormal, &exception, NULL, NULL);
        error = dumped ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!dumped) DeleteFileW(dump_path);
    }
    if (report != INVALID_HANDLE_VALUE) {
        length = snprintf(text, sizeof text, "Minidump: %s (error %lu)\r\n", dumped ? "saved" : "failed", error);
        if (length > 0 && length < (int)sizeof text) WriteFile(report, text, (DWORD)length, &written, NULL);
        CloseHandle(report);
    }
    SetEvent(finished);
    return 0;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *pointers) {
    if (InterlockedCompareExchange(&handling, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = pointers;
    exception.ClientPointers = FALSE;
    SetEvent(requested);
    // A damaged process can deadlock DbgHelp; let Windows terminate it after a bounded wait.
    WaitForSingleObject(finished, 15000);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_init(void) {
    wchar_t *base = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base) != S_OK) return;
    wchar_t directory[MAX_PATH];
    int length = swprintf(directory, MAX_PATH, L"%ls\\Okanet\\Briareus\\Crashes", base);
    CoTaskMemFree(base);
    if (length < 0 || length >= MAX_PATH) return;
    if (SHCreateDirectoryExW(NULL, directory, NULL) != ERROR_SUCCESS && GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES) return;
    SYSTEMTIME now; GetSystemTime(&now);
    length = swprintf(dump_path, MAX_PATH, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u-%03u-%lu.dmp",
        directory, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
    if (length < 0 || length >= MAX_PATH) return;
    wcscpy(report_path, dump_path);
    wcscpy(report_path + length - 3, L"txt");
    requested = CreateEventW(NULL, FALSE, FALSE, NULL);
    finished = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE thread = requested && finished ? CreateThread(NULL, 0, write_report, NULL, 0, NULL) : NULL;
    if (!thread) {
        if (requested) CloseHandle(requested);
        if (finished) CloseHandle(finished);
        return;
    }
    CloseHandle(thread);
    SetUnhandledExceptionFilter(unhandled);
}
