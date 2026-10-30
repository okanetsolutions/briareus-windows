// Manual native Doc/Pane preview. No network: every response uses the test transport.
#include "app_pull_reviews_tests.c"

#ifdef BRIAREUS_WINE_PREVIEW_TIME
// Wine 9 lacks this existing app API. This fallback is linked ONLY into this harness,
// at UTC, and is never part of Briareus.exe or the authoritative Windows test builds.
static BOOL WINAPI preview_local_time(const DYNAMIC_TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *utc, SYSTEMTIME *local) {
    (void)tz; *local = *utc; return TRUE;
}
BOOL (WINAPI *__imp_SystemTimeToTzSpecificLocalTimeEx)(const DYNAMIC_TIME_ZONE_INFORMATION *, const SYSTEMTIME *, SYSTEMTIME *) = preview_local_time;
#endif

int main(void) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (getenv("PREVIEW_TESTS")) { app_pull_reviews_tests(); return test_summary(); }
    int width = getenv("PREVIEW_WIDTH") ? atoi(getenv("PREVIEW_WIDTH")) : 1652;
    ScreenFixture f; PullScreen *s = setup(&f);
    Json *payload = pull_review_polish_fixture(); char *fixture = json_serialize(payload, false);
    f.read_response = fixture;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    pane_set_root(f.pane, &s->base); poller_stop(&s->poller);
    // PREVIEW_REVIEW opens that review's findings; without it the table of reviews shows.
    if (getenv("PREVIEW_REVIEW")) s->base.vt->action(&s->base, ACT_REVIEW_ROW, atoi(getenv("PREVIEW_REVIEW")), (POINT){0});
    if (getenv("PREVIEW_SUMMARY")) s->reviews.summary_id = s->reviews.review_id;
    if (getenv("PREVIEW_DPI")) theme_set_dpi(atoi(getenv("PREVIEW_DPI")));
    MoveWindow(f.parent, 0, 0, width, 1009, TRUE);
    RECT bounds = {0, 0, width, 1009}; pane_set_bounds(f.pane, &bounds);
    UpdateWindow(pane_hwnd(f.pane));
    if (getenv("PREVIEW_USAGE")) SendMessageW(pane_hwnd(f.pane), WM_MOUSEHWHEEL, MAKEWPARAM(0, 32760), 0);
    DWORD until = GetTickCount() + 12000;
    while (GetTickCount() < until) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP_REQUEST_DONE) store_handle_message(msg.message, msg.wParam, msg.lParam);
            else { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        Sleep(10);
    }
    pane_set_root(f.pane, NULL); // Pane owns and destroys its root.
    screen_fixture_clear(&f); json_free(payload); free(fixture); CoUninitialize();
    return 0;
}
