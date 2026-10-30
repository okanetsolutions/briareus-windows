// Synthetic native Pane/Doc preview; no live server or provider actions.
#include "app_ci_tests.c"
#ifdef BRIAREUS_WINE_PREVIEW_TIME
// Wine 9 lacks this existing app API. Harness-only UTC fallback, never in the app or Windows CI.
static BOOL WINAPI preview_local_time(const DYNAMIC_TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *utc, SYSTEMTIME *local) {
    (void)tz; *local = *utc; return TRUE;
}
BOOL (WINAPI *__imp_SystemTimeToTzSpecificLocalTimeEx)(const DYNAMIC_TIME_ZONE_INFORMATION *, const SYSTEMTIME *, SYSTEMTIME *) = preview_local_time;
#endif
int main(void) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (getenv("PREVIEW_TESTS")) { app_ci_tests(); core_ci_tests(); return test_summary(); }
    Fixture f; CIScreen *s = setup(&f); load(&f, s);
    int width = getenv("PREVIEW_WIDTH") ? atoi(getenv("PREVIEW_WIDTH")) : 1400;
    pane_set_root(f.screen.pane, &s->base); poller_stop(&s->poller);
    MoveWindow(f.screen.parent, 0, 0, width, 1000, TRUE);
    RECT bounds = {0, 0, width, 1000}; pane_set_bounds(f.screen.pane, &bounds);
    UpdateWindow(pane_hwnd(f.screen.pane));
    DWORD until = GetTickCount() + 12000;
    while (GetTickCount() < until) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP_REQUEST_DONE) store_handle_message(msg.message, msg.wParam, msg.lParam);
            else { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        Sleep(10);
    }
    pane_set_root(f.screen.pane, NULL); free(f.queries[0]); free(f.queries[1]); screen_fixture_clear(&f.screen); CoUninitialize(); return 0;
}
