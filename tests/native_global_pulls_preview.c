// Synthetic native Pane/Doc fixture. No live server or provider actions.
#include "app_global_pulls_tests.c"
#ifdef BRIAREUS_WINE_PREVIEW_TIME
static BOOL WINAPI preview_local_time(const DYNAMIC_TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *utc, SYSTEMTIME *local) {
    (void)tz; *local = *utc; return TRUE;
}
BOOL (WINAPI *__imp_SystemTimeToTzSpecificLocalTimeEx)(const DYNAMIC_TIME_ZONE_INFORMATION *, const SYSTEMTIME *, SYSTEMTIME *) = preview_local_time;
#endif
int main(void) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (getenv("PREVIEW_TESTS")) { app_global_pulls_tests(); core_global_pulls_tests(); return test_summary(); }
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f);
    f.projects = "{\"projects\":[{\"repo\":\"o/a\",\"label\":\"Synthetic storefront\"},{\"repo\":\"o/b\",\"label\":\"Synthetic mobile\"}]}";
    f.boards[0] = "{\"pulls\":[{\"number\":42,\"title\":\"Keep checkout totals consistent when quantities change\",\"branch\":\"fix/cart-totals\",\"author\":\"alice\",\"checks\":\"success\",\"updatedAt\":\"2026-10-30T12:00:00Z\",\"labels\":[{\"name\":\"ready\",\"color\":\"22aa66\"}],\"recommended\":\"review\"},{\"number\":38,\"title\":\"Show delivery estimates before checkout\",\"author\":\"sam\",\"draft\":true,\"checks\":\"pending\"}],\"syncedAt\":\"2026-10-30T12:00:00Z\"}";
    f.boards[1] = "{\"pulls\":[{\"number\":42,\"title\":\"Retain the selected workspace after reconnecting\",\"branch\":\"fix/workspace\",\"author\":\"bob\",\"checks\":\"failure\",\"updatedAt\":\"2026-10-30T11:00:00Z\",\"labels\":[{\"name\":\"needs-fix\",\"color\":\"dd6644\"}]}]}";
    global_load(&f, s); set_string(&s->state.projects[1].error, "Synthetic rate limit; last successful snapshot retained. Retry after the project cooldown.");
    int width = getenv("PREVIEW_WIDTH") ? atoi(getenv("PREVIEW_WIDTH")) : 1400;
    pane_set_root(f.screen.pane, &s->base); poller_stop(&s->poller);
    MoveWindow(f.screen.parent, 0, 0, width, 1000, TRUE);
    RECT bounds = {0, 0, width, 1000}; pane_set_bounds(f.screen.pane, &bounds); UpdateWindow(pane_hwnd(f.screen.pane));
    DWORD until = GetTickCount() + 12000;
    while (GetTickCount() < until) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP_REQUEST_DONE) store_handle_message(msg.message, msg.wParam, msg.lParam);
            else { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        Sleep(10);
    }
    pane_take_root(f.screen.pane); s->base.pane = f.screen.pane;
    global_cleanup(&f, s); CoUninitialize(); return 0;
}
