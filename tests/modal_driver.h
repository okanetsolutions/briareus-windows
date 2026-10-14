#ifndef BRIAREUS_TEST_MODAL_DRIVER_H
#define BRIAREUS_TEST_MODAL_DRIVER_H
#include "test.h"
#include <windows.h>
#include <wchar.h>

typedef struct { DWORD thread; int answer; bool found; } ModalDriver;
static inline BOOL CALLBACK modal_driver_find(HWND window, LPARAM arg) {
    ModalDriver *driver = (ModalDriver *)arg; wchar_t name[32];
    GetClassNameW(window, name, sizeof name / sizeof *name);
    if (wcscmp(name, L"#32770") || !IsWindowVisible(window)) return TRUE;
    driver->found = true;
    PostMessageW(window, WM_COMMAND, MAKEWPARAM(driver->answer, BN_CLICKED), 0);
    return FALSE;
}
static inline DWORD WINAPI modal_driver_thread(void *arg) {
    ModalDriver *driver = arg; ULONGLONG end = GetTickCount64() + 10000;
    while (!driver->found && GetTickCount64() < end) {
        EnumThreadWindows(driver->thread, modal_driver_find, (LPARAM)driver);
        if (!driver->found) Sleep(1);
    }
    if (!driver->found) PostThreadMessageW(driver->thread, WM_QUIT, 0, 0);
    return 0;
}
static inline void modal_driver_action(Screen *screen, int action, intptr_t arg, int answer) {
    // Tests do not initialize TaskDialogIndirect; app_confirm uses the standard Win32 message box.
    ModalDriver driver = {GetCurrentThreadId(), answer, false};
    HANDLE worker = CreateThread(NULL, 0, modal_driver_thread, &driver, 0, NULL);
    CHECK(worker != NULL);
    if (!worker) return;
    screen->vt->action(screen, action, arg, (POINT){0});
    MSG quit; if (!driver.found) PeekMessageW(&quit, NULL, WM_QUIT, WM_QUIT, PM_REMOVE);
    CHECK_INT(WaitForSingleObject(worker, 15000), WAIT_OBJECT_0); CHECK(driver.found); CloseHandle(worker);
}
#endif
