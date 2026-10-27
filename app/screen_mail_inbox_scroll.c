#include "screen_mail_inbox_internal.h"

static int open_block_offset(const Inbox *s);
static int open_block_target(const Inbox *s);
static int stacked_open_target(const Inbox *s);
static void track_open_block(Inbox *s, Doc *doc, bool stack);

// Distance from the reader card top to the opened block, or -1 when this layout drew no block.
static int open_block_offset(const Inbox *s) {
    if (s->reader_block_y < 0 || s->reader_y < 0 || s->reader_block_y < s->reader_y) return -1;
    return s->reader_block_y - s->reader_y;
}

// A fresh open shows the block, and stays at the card top when that block is already first.
static int open_block_target(const Inbox *s) {
    int offset = open_block_offset(s);
    if (offset < 0 || s->reader_block_first) return 0;
    return offset;
}

// Keep the card heading when there is room, but reveal the body even for the first block. Long bodies show
// their beginning; short bodies fit in the space below the pinned controls. pane_scroll_to adds an 8px inset.
static int stacked_open_target(const Inbox *s) {
    int row = s->reader_block_y >= 0 && !s->reader_block_first ? s->reader_block_y : s->reader_y;
    int target = row - s->chrome_bottom;
    if (s->reader_body_y >= 0) {
        RECT view = pane_content_rect(s->base.pane);
        int height = view.bottom - view.top;
        int room = height - s->chrome_bottom - px(16);
        if (room < 0) room = 0;
        int shown = s->reader_body_bottom - s->reader_body_y;
        if (shown > room) shown = room;
        int body_target = s->reader_body_y + shown - height + px(16);
        if (target < body_target) target = body_target;
    }
    return target < 0 ? 0 : target;
}

// Older mail is laid out above the opened block, and a stacked list sits above the reader, so that block's y grows
// after the one-shot reveal. Add only that inserted height. Swapping wide and stacked layouts has no common origin,
// so the offset is taken from the block row_open matches. A throwaway layout must not record either: draw() lays
// the screen out into another document. A later paint in the same layout adds nothing, so a scroll the reader
// itself moved stays where the reader left it.
static void track_open_block(Inbox *s, Doc *doc, bool stack) {
    if (!s->base.pane || doc != pane_doc(s->base.pane)) return;
    int offset = open_block_offset(s);
    bool have = offset >= 0;
    bool revealing = s->reveal_focus || s->reveal_open || s->reveal_block;
    if (have && !revealing && s->block_held) {
        if (s->block_stacked != stack) {
            if (!stack) doc->sticky_scroll = open_block_target(s);
            else s->page_follow = 1;
        } else if (!stack) {
            int inserted = offset - s->block_offset;
            if (inserted > 0) doc->sticky_scroll += inserted;
        } else if (s->reader_block_y > s->block_page) {
            s->page_follow = 2;
            s->page_delta = s->reader_block_y - s->block_page;
        }
    }
    if (have) {
        s->block_held = true;
        s->block_offset = offset;
        s->block_page = s->reader_block_y;
        s->block_stacked = stack;
    }
}

void screen_mail_inbox_layout(Screen *base, Doc *doc) {
    Inbox *s = (Inbox *)base; int x = px(16), w = doc->width - x * 2;
    s->page_follow = 0;
    if (w < px(120)) w = px(120);
    s->focus_y = -1; s->open_y = -1; s->reader_y = -1; s->reader_block_y = -1; s->reader_blocks = 0; s->reader_block_first = false; s->stacked = false;
    s->reader_body_y = s->reader_body_bottom = -1;
    doc_space(doc, px(12));
    if (s->mail_client && s->mail_client != g_store.client) screen_mail_inbox_clear_private(s);
    if (!mail_inbox_offered()) { screen_mail_inbox_clear_private(s); doc_notice(doc, x, w, "Mail needs an Admin token and the deployed account and message-list routes."); return; }
    if (!store_supports("mail_message")) screen_mail_inbox_clear_selection(s);
    bool available = screen_mail_inbox_ready(s);
    MailGroups groups = mail_groups_build(&s->messages, s->group == GROUP_THREADS);
    MailGroup *rows = groups.groups; size_t nrow = groups.count;
    Anchors anchors; memset(&anchors, 0, sizeof anchors);
    screen_mail_inbox_layout_chrome(s, doc, x, w, rows, nrow, available, &anchors);
    if (s->notice) { doc_notice(doc, x, w, s->notice); doc_space(doc, px(8)); }
    if (g_store.mail_retry_until > GetTickCount64()) { doc_notice(doc, x, w, "Mail cooldown is active. Retry buttons become available when it ends."); doc_space(doc, px(8)); }
    if (s->account_error) { doc_notice(doc, x, w, s->account_error); doc_button(doc, x, 0, "Retry account status", BUTTON_BORDERED, ACT_RETRY_ACCOUNTS, 0, available && !s->account_read); doc_space(doc, px(8)); }
    int pin_at = (int)doc->count; s->chrome_bottom = doc->y;
    if (!s->accounts_loaded) {
        if (s->account_read) doc_loading(doc, x, w, "Loading mail accounts...");
        int menu_bottom = screen_mail_inbox_layout_menu(s, doc, &anchors);
        if (doc->y < menu_bottom) doc->y = menu_bottom;
        doc_pin(doc, pin_at, s->chrome_bottom);
        mail_groups_free(&groups);
        return;
    }
    if (!s->accounts.count && !s->account_error) {
        doc_text(doc, x, w, "No mailbox is connected. Add Gmail or Outlook in Settings, under Mail.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(8));
    }
    bool stack = w < px(680);
    int list_w = w, reader_x = x, reader_w = w, top = doc->y;
    if (!stack) {
        list_w = w * 34 / 100;
        if (list_w < px(250)) list_w = px(250);
        if (list_w > px(380)) list_w = px(380);
        if (list_w > w - px(280)) list_w = w - px(280);
        reader_x = x + list_w + px(14);
        reader_w = w - list_w - px(14);
    }
    screen_mail_inbox_layout_list(s, doc, rows, nrow, x, list_w, available);
    int list_bottom = doc->y;
    if (!stack) doc->y = top;
    s->stacked = stack;
    s->reader_y = doc->y;
    int reader_first = (int)doc->count;
    screen_mail_inbox_layout_reader(s, doc, rows, nrow, reader_x, reader_w, available);
    if (!stack) {
        // The reader stays beside the list and, once it is taller than the view, scrolls on its own. Opening or
        // moving in the list can no longer carry a short message above the viewport. The page is as long as the
        // list, or as the reader's window when the reader needs that.
        if (s->base.pane) {
            RECT view = pane_content_rect(s->base.pane);
            int room = (view.bottom - view.top) - top - px(12);
            if (room < 0) room = 0;
            int shown = doc->y - top;
            if (shown > room) shown = room;
            int page = top + shown;
            if (page < list_bottom) page = list_bottom;
            doc->y = page;
            // Members are oldest-first, so the message just opened is below older previews. Scroll to that block, and
            // keep the top when it is already first. The loading paint has no block yet; this stays set until the body
            // is laid out.
            if (s->reveal_block) doc->sticky_scroll = open_block_target(s);
            track_open_block(s, doc, false);
            doc_sticky(doc, reader_first, (int)doc->count, doc->y);
        } else if (list_bottom > doc->y) doc->y = list_bottom;
    } else track_open_block(s, doc, true);
    int content_bottom = doc->y;
    int menu_bottom = screen_mail_inbox_layout_menu(s, doc, &anchors);
    if (content_bottom > doc->y) doc->y = content_bottom;
    if (menu_bottom > doc->y) doc->y = menu_bottom;
    doc_pin(doc, pin_at, s->chrome_bottom);
    mail_groups_free(&groups);
}

void screen_mail_inbox_scrolled(Screen *base, bool at_bottom) {
    (void)at_bottom; Inbox *s = (Inbox *)base;
    if (s->in_scroll) return;
    s->in_scroll = true;
    if (s->reveal_focus || s->reveal_open || s->reveal_block) {
        // j/k keeps the focused list row under the chrome. Beside the list the reader scrolls to the opened block on
        // its own. A stacked reader has no sticky scroll, so opening one scrolls the page to that block, or to the
        // card top while the body is loading. Once loaded, ensure the body fits below the pinned controls.
        bool to_block = s->reveal_block && s->reader_block_y >= 0 && !s->reader_block_first;
        int row = s->reveal_focus ? s->focus_y : s->stacked ? (to_block ? s->reader_block_y : s->reader_y) : s->open_y;
        bool move = s->reveal_focus || s->reveal_open || (s->stacked && s->reveal_block && s->reader_block_y >= 0);
        bool reveal_reader = s->stacked && !s->reveal_focus;
        s->reveal_focus = false; s->reveal_open = false;
        if (s->reveal_block && (s->reader_block_y >= 0 || !s->body_read)) s->reveal_block = false;
        if (move && s->base.pane && row >= 0) {
            int target = row - s->chrome_bottom;
            if (reveal_reader) target = stacked_open_target(s);
            if (target < 0) target = 0;
            pane_scroll_to(s->base.pane, target);
        }
    }
    if (s->page_follow && s->base.pane) {
        int follow = s->page_follow, delta = s->page_delta;
        s->page_follow = 0;
        if (follow == 1) {
            if (s->reader_y >= 0) pane_scroll_to(s->base.pane, stacked_open_target(s));
        } else if (follow == 2 && delta > 0) pane_scroll_to(s->base.pane, pane_scroll_y(s->base.pane) + delta + px(8));
    }
    if (s->base.pane && s->menu) {
        int y = pane_scroll_y(s->base.pane);
        if (y != s->scroll_seen) { s->scroll_seen = y; s->menu = MENU_NONE; screen_mail_inbox_repaint(s); }
    }
    s->in_scroll = false;
}
