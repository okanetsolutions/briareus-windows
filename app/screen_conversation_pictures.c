#include "screen_conversation_internal.h"
#include "canvas.h"
#include "picture.h"
#include "remote_image.h"

/// A picture under the reply that links to it: its preview once fetched, and until then a line saying what it waits for.
typedef struct { char *url, *label, *note; } Tile;

static void tile_free(void *p);
static char *tile_label(const MdImage *image);
static char *tile_note(const ImageInfo *info, const char *url);
static int tile_action(const ImageInfo *info);
static void paint_preview(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_note(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

static void tile_free(void *p) { Tile *t = p; free(t->url); free(t->label); free(t->note); free(t); }

/// The words that stand for the picture, or its file's name when the reply gave none of its own.
static char *tile_label(const MdImage *image) {
    char *alt = str_trim(image->alt ? image->alt : "");
    if (*alt && !str_eq(alt, image->url)) return alt;
    free(alt);
    return remote_image_name(image->url);
}

static char *tile_note(const ImageInfo *info, const char *url) {
    switch (info->state) {
    case IMAGE_LOADING: return xstrdup("Loading\xE2\x80\xA6");
    case IMAGE_FAILED: return xstrfmt("Click to try again \xC2\xB7 %s", info->error ? info->error : "The picture could not be loaded.");
    case IMAGE_READY: return NULL;
    case IMAGE_ASK: break;
    }
    if (info->host_allowed) return xstrdup("Click to show this picture");
    char *host = remote_image_host(url);
    char *note = xstrfmt("Click to always show pictures from %s", host ? host : "this address");
    free(host);
    return note;
}

static int tile_action(const ImageInfo *info) {
    switch (info->state) {
    case IMAGE_ASK: return ACT_PICTURE_ALLOW;
    case IMAGE_FAILED: return ACT_PICTURE_RETRY;
    case IMAGE_READY: return ACT_PICTURE_OPEN;
    case IMAGE_LOADING: break;
    }
    return ACTION_NONE;
}

static void paint_preview(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    Tile *t = it->data;
    fill_round_rect(cv, rc, px(8), theme.sunken, doc_item_hovered(doc, it) ? theme.accent : theme.line);
    // Drawing it is what keeps it: a preview that gave way to others comes back once it is in view again.
    const Picture *preview = image_cache_shown(t->url);
    if (!preview) return;
    RECT inner = { rc->left + 1, rc->top + 1, rc->right - 1, rc->bottom - 1 };
    canvas_clip_round(cv, &inner, px(7));
    draw_picture(cv, preview, &inner);
    canvas_unclip(cv);
}

static void paint_note(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    Tile *t = it->data;
    bool hovered = it->action && doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, px(8), theme.raise, hovered ? theme.accent : theme.line);
    RECT glyph = { rc->left + px(10), rc->top, rc->left + px(30), rc->bottom };
    draw_text(cv, "\xF0\x9F\x96\xBC", &glyph, FONT_EMOJI, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    int name_w = text_width(cv, t->label, FONT_FOOTNOTE_SEMIBOLD), room = rc->right - px(12) - glyph.right - px(6);
    if (name_w > room / 2) name_w = room / 2;
    RECT name = { glyph.right + px(6), rc->top, glyph.right + px(6) + name_w, rc->bottom };
    draw_text(cv, t->label, &name, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT note = { name.right + px(10), rc->top, rc->right - px(12), rc->bottom };
    draw_text(cv, t->note, &note, FONT_FOOTNOTE, it->action ? theme.accent : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_conversation_layout_picture(Doc *doc, int x, int w, const MdImage *image, void *ctx) {
    (void)ctx;
    ImageInfo info = image_cache_get(image->url);
    Tile *t = xcalloc(1, sizeof *t);
    t->url = xstrdup(image->url); t->label = tile_label(image); t->note = tile_note(&info, image->url);
    doc_space(doc, px(8));
    int item;
    if (info.state == IMAGE_READY) {
        int tw, th, room = w > px(40) ? w - 2 : px(40);
        picture_fit(info.preview_w, info.preview_h, room, 0, &tw, &th);
        item = doc_custom(doc, x, tw + 2, th + 2, paint_preview, t, tile_free, tile_action(&info), (intptr_t)t->url);
        doc_item(doc, item)->tip = xstrfmt("%s \xC2\xB7 %d \xC3\x97 %d", t->label, info.width, info.height);
    } else {
        int tw = w < px(520) ? w : px(520);
        item = doc_custom(doc, x, tw, px(36), paint_note, t, tile_free, tile_action(&info), (intptr_t)t->url);
        doc_item(doc, item)->tip = info.error ? xstrfmt("%s\n%s", info.error, t->url) : xstrdup(t->url);
    }
    doc_item(doc, item)->hover_fill = false;
    doc_space(doc, px(2));
}

void screen_conversation_pictures_changed(void *ctx) {
    ConversationScreen *s = ctx;
    if (s->base.pane) pane_relayout(s->base.pane);
}

bool screen_conversation_picture_action(ConversationScreen *s, int action, intptr_t arg) {
    if (action != ACT_PICTURE_OPEN && action != ACT_PICTURE_ALLOW && action != ACT_PICTURE_RETRY) return false;
    // The address belongs to the tile, which the next layout frees.
    char *url = xstrdup((const char *)arg);
    if (action == ACT_PICTURE_ALLOW) image_cache_allow(url);
    else if (action == ACT_PICTURE_RETRY) image_cache_retry(url);
    else if (s->base.pane && image_cache_get(url).preview) {
        // Back from the picture, the conversation is where it was left rather than at its end.
        s->return_scroll = s->at_bottom ? -1 : pane_scroll_y(s->base.pane);
        pane_push(s->base.pane, image_screen_new(url));
    }
    free(url);
    return true;
}
