#include "screen_browser_internal.h"

static void paint_view(BrowserScreen *s, HWND hwnd, HDC dc);
static void drain_feed(BrowserScreen *s);
static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void register_view_class(void);

// MARK: - The view

static const wchar_t VIEW_CLASS[] = L"BriareusBrowserView";

static void paint_view(BrowserScreen *s, HWND hwnd, HDC dc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h), old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(theme.sunken);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    if (s->pixels) {
        s->drawn = browser_fit(w - 2, h - 2, s->pw, s->ph);
        s->drawn.left += 1; s->drawn.right += 1; s->drawn.top += 1; s->drawn.bottom += 1;
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = s->pw; bi.bmiHeader.biHeight = -s->ph;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(mem, HALFTONE); SetBrushOrgEx(mem, 0, 0, NULL);
        StretchDIBits(mem, s->drawn.left, s->drawn.top, s->drawn.right - s->drawn.left, s->drawn.bottom - s->drawn.top,
                      0, 0, s->pw, s->ph, s->pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        memset(&s->drawn, 0, sizeof s->drawn);
        const wchar_t *note = s->feed ? L"Waiting for the first picture\x2026" : L"Connecting to the browser\x2026";
        HFONT old_font = SelectObject(mem, font(FONT_BODY));
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, theme.muted);
        DrawTextW(mem, note, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, old_font);
    }
    // A ring in the accent while keys go to the page.
    HBRUSH ring = CreateSolidBrush(GetFocus() == hwnd ? theme.accent : theme.line);
    FrameRect(mem, &rc, ring);
    DeleteObject(ring);
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
}

static void drain_feed(BrowserScreen *s) {
    Feed *f = s->feed;
    if (!f) return;
    EnterCriticalSection(&f->lock);
    Json *tabs = f->tabs; f->tabs = NULL;
    unsigned char *pixels = f->pixels; f->pixels = NULL;
    int pw = f->pw, ph = f->ph, fw = f->fw, fh = f->fh;
    bool heard = f->heard, closed = f->closed, ended = f->ended;
    ApiError error; api_error_init(&error); api_error_copy(&error, &f->error);
    f->posted = false;
    LeaveCriticalSection(&f->lock);
    if (heard) {
        s->failures = 0;
        if (s->state_error) { set_string(&s->state_error, NULL); screen_browser_changed(s); }
    }
    if (pixels) screen_browser_show_frame(s, pixels, pw, ph, fw, fh);
    if (tabs) {
        browser_state_read(&s->state, tabs);
        json_free(tabs);
        screen_browser_set_address(s);
        screen_browser_changed(s);
    }
    if (closed) {
        screen_browser_went_down(s);
        screen_browser_read_state(s);
        screen_browser_changed(s);
    } else if (ended) {
        screen_browser_stop_feed(s);
        if (api_error_unauthorized(&error)) store_invalidate_credentials(&error);
        else if (error.kind == API_HTTP && error.status == 409) { screen_browser_went_down(s); screen_browser_read_state(s); screen_browser_changed(s); }
        else if (s->shown && s->base.pane) {
            // A dropped connection: back after a pause that grows while it keeps failing.
            SetTimer(pane_hwnd(s->base.pane), TIMER_RECONNECT, (UINT)poll_delay_ms(1000, s->failures++, error.retry_after), NULL);
        }
    }
    api_error_clear(&error);
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    BrowserScreen *s = (BrowserScreen *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg) {
    case WM_BROWSER_FEED: drain_feed(s); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); paint_view(s, hwnd, dc); EndPaint(hwnd, &ps); return 0; }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW)); return TRUE;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        SetFocus(hwnd);
        if (s->pressing) return 0;
        s->pressing = true; s->dragging = false; s->press_x = x; s->press_y = y;
        s->clicks = msg == WM_LBUTTONDBLCLK ? 2 : 1;
        s->button = msg == WM_RBUTTONDOWN ? "right" : msg == WM_MBUTTONDOWN ? "middle" : "left";
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (!s->pressing || !str_eq(s->button, "left")) return 0;
        if (!s->dragging && abs(x - s->press_x) + abs(y - s->press_y) > GetSystemMetrics(SM_CXDRAG)) {
            Json *down = screen_browser_pointer_input(s, "down", s->press_x, s->press_y);
            if (down) { s->dragging = true; screen_browser_send_input(s, down); }
        }
        if (s->dragging) screen_browser_send_input(s, screen_browser_pointer_input(s, "move", x, y));
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        if (!s->pressing) return 0;
        s->pressing = false;
        ReleaseCapture();
        if (s->dragging) { Json *up = screen_browser_pointer_input(s, "up", x, y); if (!up) { up = screen_browser_simple_input("up"); } screen_browser_send_input(s, up); return 0; }
        Json *click = screen_browser_pointer_input(s, "click", s->press_x, s->press_y);
        if (click) {
            if (!str_eq(s->button, "left")) json_set_str(click, "button", s->button);
            if (s->clicks > 1) json_set_num(click, "clickCount", s->clicks);
            screen_browser_send_input(s, click);
        }
        return 0;
    }
    case WM_CAPTURECHANGED: s->pressing = false; return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT pt = { x, y }; ScreenToClient(hwnd, &pt);
        Json *wheel = screen_browser_pointer_input(s, "wheel", pt.x, pt.y);
        if (!wheel) return 0;
        // A notch scrolls 100 pixels, as Chromium does; shift turns it sideways.
        double delta = -(double)GET_WHEEL_DELTA_WPARAM(wp) * 100.0 / WHEEL_DELTA;
        bool sideways = msg == WM_MOUSEHWHEEL || GetKeyState(VK_SHIFT) < 0;
        if (msg == WM_MOUSEHWHEEL) delta = -delta;
        json_set_num(wheel, sideways ? "deltaX" : "deltaY", delta);
        screen_browser_send_input(s, wheel);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (ctrl && wp == 'V') { screen_browser_paste(s); return 0; }
        if (ctrl && wp == 'L') { SetFocus(s->address); SendMessageW(s->address, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_F5) { screen_browser_send_input(s, screen_browser_simple_input("reload")); return 0; }
        const char *name = browser_key_name((unsigned)wp);
        bool character = (wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9');
        // Letters and digits are typed through WM_CHAR, unless a shortcut holds ctrl or alt.
        if (name && (!character || ctrl || alt)) {
            Json *key = screen_browser_simple_input("key"); json_set_str(key, "key", name);
            screen_browser_add_modifiers(key, true);
            screen_browser_send_input(s, key);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) break;
        return 0;
    }
    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if (c >= 0xD800 && c <= 0xDBFF) { s->high_surrogate = c; return 0; }
        // Control characters came as keys already (Enter, Tab, Backspace, ctrl shortcuts).
        if (c < 0x20 || c == 0x7F) return 0;
        wchar_t text[3] = { 0 };
        if (c >= 0xDC00 && c <= 0xDFFF) { if (!s->high_surrogate) return 0; text[0] = s->high_surrogate; text[1] = c; }
        else text[0] = c;
        s->high_surrogate = 0;
        char *utf8 = wide_to_utf8(text);
        Json *type = screen_browser_simple_input("type"); json_set_str(type, "text", utf8);
        free(utf8);
        screen_browser_send_input(s, type);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_view_class(void) {
    static bool registered;
    if (registered) return;
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    // Redrawn whole on a resize: the picture is centred, so the pixels it had are in the wrong place.
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = view_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.lpszClassName = VIEW_CLASS;
    registered = RegisterClassW(&wc) != 0;
}

/// The controls are children of the pane, which is known once the screen is shown in it.
void screen_browser_ensure_controls(BrowserScreen *s) {
    if (s->view || !s->base.pane) return;
    register_view_class();
    HWND parent = pane_hwnd(s->base.pane);
    s->view = CreateWindowExW(0, VIEW_CLASS, L"", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(s->view, GWLP_USERDATA, (LONG_PTR)s);
    s->address = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ID_ADDRESS, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->address, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->address, EM_SETCUEBANNER, TRUE, (LPARAM)L"Enter a web address");
    SetWindowSubclass(s->address, screen_browser_address_proc, ID_ADDRESS, (DWORD_PTR)s);
    theme_apply_control(s->address);
    screen_browser_set_address(s);
}
