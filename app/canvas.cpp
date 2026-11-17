// Direct2D and DirectWrite behind the app's C drawing functions. This one file is C++ because the Windows SDK declares
// DirectWrite for C++ only; it keeps to C style (no exceptions, no RTTI, no standard library) and exports a C API.
// Coordinates are whole pixels, as the screens lay them out: every target runs at 96 DPI so a DIP is a pixel, and the
// fonts are sized in pixels for the real DPI (theme_set_dpi).
#include <d2d1.h>
#include <d2d1helper.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
extern "C" {
#include "str.h"
}

/// A part of a picture as the target holds it: pictures larger than a GPU texture are kept in tiles.
typedef struct { unsigned long long key; int x, y; ID2D1Bitmap *bitmap; size_t bytes; unsigned stamp; bool drawn; } PictureTile;
enum { PICTURE_TILES = 32, PICTURE_TILE_SIDE = 4096 };
static const size_t PICTURE_BUDGET = 384u * 1024 * 1024;

struct Canvas {
    PictureTile tiles[PICTURE_TILES];   // what draw_picture uploaded, least recently drawn first to go
    unsigned stamp;                     // counts draw_picture calls
    HWND hwnd;                          // a pane's window, or NULL for the borrowed GDI canvas
    ID2D1RenderTarget *rt;
    ID2D1DCRenderTarget *window_rt;     // owned; bound to the window's DC for each frame
    HDC window_dc;                      // the window's DC while a frame is open
    ID2D1SolidColorBrush *brush;
    bool color_fonts;                   // the target draws colour glyphs (emoji), Windows 8.1 and later
    int clips;
    unsigned layers;                    // bit n: clip n is a rounded layer rather than an axis-aligned clip
};

static ID2D1Factory *d2d;
static IDWriteFactory *dw;

typedef struct {
    IDWriteTextFormat *format;
    IDWriteInlineObject *ellipsis;
    int line_h, baseline;               // GDI's tmHeight and tmAscent, so lines keep the heights the layouts expect
    float space_w;
} Font;
static Font fonts[FONT_COUNT];

template <class T> static void release(T *&p) { if (p) { p->Release(); p = NULL; } }

bool canvas_startup(void) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), NULL, (void **)&d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&dw))) return false;
    return true;
}

// MARK: - Measuring

// Words repeat across a transcript and every layout measures them again, so their widths are kept.
typedef struct { unsigned hash; int font; unsigned len; wchar_t *text; int width; } WidthEntry;
enum { WIDTH_CACHE = 1 << 14, WIDTH_CACHE_MAX_LEN = 128 };
static WidthEntry width_cache[WIDTH_CACHE];

static void clear_width_cache(void) {
    for (int i = 0; i < WIDTH_CACHE; i++) { xfree_kept(width_cache[i].text); width_cache[i].text = NULL; }
}

static IDWriteTextLayout *layout(FontId f, const wchar_t *text, size_t len, float max_w, float max_h, bool wrap) {
    IDWriteTextLayout *l = NULL;
    if (!dw || !fonts[f].format) return NULL;
    if (FAILED(dw->CreateTextLayout(text, (UINT32)len, fonts[f].format, max_w, max_h, &l))) return NULL;
    l->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return l;
}
/// Widths are rounded up, so a box sized to its text never trims it.
static int whole(float w) { return (int)ceilf(w - 0.01f); }

static int measure_width(FontId f, const wchar_t *text, size_t len) {
    if (!len) return 0;
    IDWriteTextLayout *l = layout(f, text, len, 100000.0f, 100000.0f, false);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m;
    int w = SUCCEEDED(l->GetMetrics(&m)) ? whole(m.widthIncludingTrailingWhitespace) : 0;
    l->Release();
    return w;
}

extern "C" int textw_extent(FontId f, const wchar_t *text, size_t len) {
    if (!text || !len) return 0;
    if (len > WIDTH_CACHE_MAX_LEN) return measure_width(f, text, len);
    unsigned h = 2166136261u ^ (unsigned)f;
    for (size_t i = 0; i < len; i++) h = (h ^ text[i]) * 16777619u;
    WidthEntry *e = &width_cache[h & (WIDTH_CACHE - 1)];
    if (e->text && e->hash == h && e->font == (int)f && e->len == len && memcmp(e->text, text, len * sizeof *text) == 0) return e->width;
    int w = measure_width(f, text, len);
    xfree_kept(e->text);
    e->text = (wchar_t *)xmalloc_kept(len * sizeof *text); memcpy(e->text, text, len * sizeof *text);
    e->hash = h; e->font = (int)f; e->len = (unsigned)len; e->width = w;
    return w;
}

/// The width before each character boundary, `out[0...len]`; the inside of a cluster has its start's width.
static void prefix_widths(FontId f, const wchar_t *text, size_t len, float *out) {
    for (size_t i = 0; i <= len; i++) out[i] = 0;
    IDWriteTextLayout *l = layout(f, text, len, 100000.0f, 100000.0f, false);
    if (!l) return;
    UINT32 count = 0;
    l->GetClusterMetrics(NULL, 0, &count);
    DWRITE_CLUSTER_METRICS *clusters = (DWRITE_CLUSTER_METRICS *)xmalloc((count ? count : 1) * sizeof *clusters);
    if (SUCCEEDED(l->GetClusterMetrics(clusters, count, &count))) {
        size_t pos = 0; float x = 0;
        for (UINT32 c = 0; c < count && pos < len; c++) {
            for (UINT16 k = 1; k < clusters[c].length && pos + k <= len; k++) out[pos + k] = x;
            x += clusters[c].width; pos += clusters[c].length;
            if (pos <= len) out[pos] = x;
        }
        for (; pos < len; pos++) out[pos + 1] = x;
    }
    free(clusters);
    l->Release();
}

extern "C" size_t textw_fit(FontId f, const wchar_t *text, size_t len, int width) {
    if (!text || !len) return 0;
    float *w = (float *)xmalloc((len + 1) * sizeof *w);
    prefix_widths(f, text, len, w);
    size_t k = len;
    while (k > 0 && whole(w[k]) > width) k--;
    free(w);
    return k;
}

extern "C" int textw_width(Canvas *cv, const wchar_t *text, FontId f) { (void)cv; return text ? textw_extent(f, text, wcslen(text)) : 0; }
extern "C" int text_width(Canvas *cv, const char *text, FontId f) {
    wchar_t *w = utf8_to_wide(text ? text : "");
    int width = textw_width(cv, w, f);
    free(w);
    return width;
}
extern "C" int font_height(Canvas *cv, FontId f) { (void)cv; return fonts[f].line_h; }

/// A layout of `text` as DrawText would lay it out in a box `w` wide.
static IDWriteTextLayout *flags_layout(FontId f, const wchar_t *text, size_t len, float w, UINT flags) {
    bool single = (flags & DT_SINGLELINE) != 0;
    IDWriteTextLayout *l = layout(f, text, len, w > 0 ? w : 0, 100000.0f, !single && (flags & DT_WORDBREAK));
    if (!l) return NULL;
    if (flags & DT_EXPANDTABS) l->SetIncrementalTabStop(fonts[f].space_w * 8);
    return l;
}

extern "C" int measure_text(Canvas *cv, const char *text, int width, FontId f, UINT flags) {
    (void)cv;
    if (flags & DT_SINGLELINE) return fonts[f].line_h;
    wchar_t *w = utf8_to_wide(text ? text : "");
    size_t len = wcslen(w);
    int h = fonts[f].line_h;
    IDWriteTextLayout *l = len ? flags_layout(f, w, len, width > 0 ? (float)width : 100000.0f, flags | (width > 0 ? DT_WORDBREAK : 0)) : NULL;
    DWRITE_TEXT_METRICS m;
    if (l && SUCCEEDED(l->GetMetrics(&m)) && m.height > 0) h = (int)lroundf(m.height);
    release(l);
    free(w);
    return h;
}

// MARK: - Fonts

extern "C" void canvas_set_font(FontId id, const wchar_t *face, float size, int weight, bool italic) {
    Font *font = &fonts[id];
    release(font->format); release(font->ellipsis);
    clear_width_cache();
    if (!dw) return;
    DWRITE_FONT_STYLE style = italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    if (FAILED(dw->CreateTextFormat(face, NULL, (DWRITE_FONT_WEIGHT)weight, style, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &font->format))) return;
    // Lines as GDI measured them: the ascent and descent rounded apart, no line gap.
    float ascent = size * 0.9f, descent = size * 0.25f;
    IDWriteFontCollection *system = NULL;
    if (SUCCEEDED(dw->GetSystemFontCollection(&system, FALSE))) {
        UINT32 index = 0; BOOL exists = FALSE;
        IDWriteFontFamily *family = NULL; IDWriteFont *match = NULL;
        if (SUCCEEDED(system->FindFamilyName(face, &index, &exists)) && exists && SUCCEEDED(system->GetFontFamily(index, &family))
            && SUCCEEDED(family->GetFirstMatchingFont((DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STRETCH_NORMAL, style, &match))) {
            DWRITE_FONT_METRICS m;
            match->GetMetrics(&m);
            ascent = size * m.ascent / m.designUnitsPerEm; descent = size * m.descent / m.designUnitsPerEm;
        }
        release(match); release(family); release(system);
    }
    font->baseline = (int)lroundf(ascent);
    font->line_h = font->baseline + (int)lroundf(descent);
    font->format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, (float)font->line_h, (float)font->baseline);
    dw->CreateEllipsisTrimmingSign(font->format, &font->ellipsis);
    IDWriteTextLayout *l = layout(id, L" ", 1, 1000.0f, 1000.0f, false);
    DWRITE_TEXT_METRICS m;
    font->space_w = l && SUCCEEDED(l->GetMetrics(&m)) ? m.widthIncludingTrailingWhitespace : size / 4;
    release(l);
}

// MARK: - Targets

static void drop_pictures(Canvas *cv) {
    for (int i = 0; i < PICTURE_TILES; i++) { release(cv->tiles[i].bitmap); cv->tiles[i].key = 0; cv->tiles[i].bytes = 0; }
}
/// After a frame: a tile the frame did not draw is let go, so a picture scrolled away or closed holds no texture.
static void sweep_pictures(Canvas *cv) {
    for (int i = 0; i < PICTURE_TILES; i++) {
        PictureTile *t = &cv->tiles[i];
        if (t->bitmap && !t->drawn) { release(t->bitmap); t->bytes = 0; }
        t->drawn = false;
    }
}
static void drop_target(Canvas *cv) {
    drop_pictures(cv);
    release(cv->brush);
    cv->rt = NULL;
    release(cv->window_rt);
}
static bool make_brush(Canvas *cv) {
    if (FAILED(cv->rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &cv->brush))) return false;
    ID2D1DeviceContext *dc = NULL;
    cv->color_fonts = SUCCEEDED(cv->rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void **)&dc));
    release(dc);
    return true;
}
static D2D1_RENDER_TARGET_PROPERTIES target_properties(void) {
    // Opaque pixels, so text gets ClearType as on the rest of the desktop.
    return D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
}
static bool ensure_target(Canvas *cv) {
    if (cv->rt) return true;
    if (!d2d) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = target_properties();
    if (FAILED(d2d->CreateDCRenderTarget(&props, &cv->window_rt))) return false;
    cv->rt = cv->window_rt;
    if (!make_brush(cv)) { drop_target(cv); return false; }
    return true;
}

extern "C" Canvas *canvas_for_window(HWND hwnd) {
    Canvas *cv = (Canvas *)xcalloc(1, sizeof *cv);
    cv->hwnd = hwnd;
    return cv;
}
extern "C" void canvas_free(Canvas *cv) {
    if (!cv) return;
    drop_target(cv);
    free(cv);
}
/// Each frame binds the target at the window's current size, so a resize needs nothing more.
extern "C" void canvas_resize(Canvas *cv, int width, int height) { (void)cv; (void)width; (void)height; }
/// The frame reaches the window through its DC rather than an HWND render target: GDI clips the DC to the window's
/// visible region, which WS_CLIPCHILDREN keeps clear of the screens' controls, so a frame never paints over an Edit.
extern "C" bool canvas_begin(Canvas *cv) {
    if (!cv || !ensure_target(cv)) return false;
    RECT rc; GetClientRect(cv->hwnd, &rc);
    cv->window_dc = GetDC(cv->hwnd);
    if (!cv->window_dc) return false;
    if (FAILED(cv->window_rt->BindDC(cv->window_dc, &rc))) { ReleaseDC(cv->hwnd, cv->window_dc); cv->window_dc = NULL; return false; }
    cv->rt->BeginDraw();
    cv->rt->SetTransform(D2D1::Matrix3x2F::Identity());
    cv->clips = 0; cv->layers = 0;
    return true;
}
extern "C" void canvas_end(Canvas *cv) {
    if (!cv || !cv->rt) return;
    while (cv->clips > 0) canvas_unclip(cv);
    sweep_pictures(cv);
    HRESULT hr = cv->rt->EndDraw();
    ReleaseDC(cv->hwnd, cv->window_dc); cv->window_dc = NULL;
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        // The GPU was reset or changed: draw the frame again on a new target.
        drop_target(cv);
        InvalidateRect(cv->hwnd, NULL, FALSE);
    }
}

// One canvas over GDI device contexts serves every borrowed paint; they never nest.
static Canvas dc_canvas;
static ID2D1DCRenderTarget *dc_rt;
extern "C" Canvas *canvas_begin_dc(HDC hdc, const RECT *rc) {
    if (!dc_rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = target_properties();
        if (!d2d || FAILED(d2d->CreateDCRenderTarget(&props, &dc_rt))) return NULL;
        dc_canvas.rt = dc_rt;
        if (!make_brush(&dc_canvas)) { release(dc_rt); dc_canvas.rt = NULL; return NULL; }
    }
    if (FAILED(dc_rt->BindDC(hdc, rc))) return NULL;
    dc_rt->BeginDraw();
    dc_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_canvas.clips = 0; dc_canvas.layers = 0;
    return &dc_canvas;
}
extern "C" void canvas_end_dc(Canvas *cv) {
    if (!cv || !dc_rt) return;
    while (cv->clips > 0) canvas_unclip(cv);
    sweep_pictures(cv);
    if (dc_rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) { drop_pictures(&dc_canvas); release(dc_canvas.brush); release(dc_rt); dc_canvas.rt = NULL; }
}

static D2D1_RECT_F rectf(const RECT *rc) { return D2D1::RectF((float)rc->left, (float)rc->top, (float)rc->right, (float)rc->bottom); }

extern "C" void canvas_clip(Canvas *cv, const RECT *rc) {
    if (!cv || !cv->rt) return;
    if (cv->clips >= 32) return;
    cv->rt->PushAxisAlignedClip(rectf(rc), D2D1_ANTIALIAS_MODE_ALIASED);
    cv->clips++;
}
extern "C" void canvas_clip_round(Canvas *cv, const RECT *rc, int radius) {
    if (!cv || !cv->rt || cv->clips >= 32) return;
    ID2D1RoundedRectangleGeometry *shape = NULL;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rectf(rc), (float)radius, (float)radius);
    if (FAILED(d2d->CreateRoundedRectangleGeometry(&rr, &shape))) { canvas_clip(cv, rc); return; }
    cv->rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape), NULL);
    shape->Release();   // the layer holds its own reference
    cv->layers |= 1u << cv->clips;
    cv->clips++;
}
extern "C" void canvas_unclip(Canvas *cv) {
    if (!cv || !cv->rt || cv->clips <= 0) return;
    cv->clips--;
    if (cv->layers & (1u << cv->clips)) { cv->rt->PopLayer(); cv->layers &= ~(1u << cv->clips); }
    else cv->rt->PopAxisAlignedClip();
}
extern "C" void canvas_offset(Canvas *cv, int dx, int dy) {
    if (cv && cv->rt) cv->rt->SetTransform(D2D1::Matrix3x2F::Translation((float)dx, (float)dy));
}

// MARK: - Shapes

static ID2D1SolidColorBrush *paint(Canvas *cv, COLORREF c) {
    cv->brush->SetColor(D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f));
    return cv->brush;
}
static bool ready(Canvas *cv) { return cv && cv->rt && cv->brush; }

extern "C" void fill_rect(Canvas *cv, const RECT *rc, COLORREF color) {
    if (!ready(cv) || rc->right <= rc->left || rc->bottom <= rc->top) return;
    cv->rt->FillRectangle(rectf(rc), paint(cv, color));
}
static void fill_round(Canvas *cv, float l, float t, float r, float b, float radius, COLORREF color) {
    if (r <= l || b <= t) return;
    float max = fminf(r - l, b - t) / 2;
    if (radius > max) radius = max;
    if (radius <= 0) cv->rt->FillRectangle(D2D1::RectF(l, t, r, b), paint(cv, color));
    else cv->rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, b), radius, radius), paint(cv, color));
}
extern "C" void fill_round_rect(Canvas *cv, const RECT *rc, int radius, COLORREF fill, COLORREF border) {
    if (!ready(cv)) return;
    // A 1px border is the outer shape in the border colour with the inner shape filled over it, so it stays crisp.
    float l = (float)rc->left, t = (float)rc->top, r = (float)rc->right, b = (float)rc->bottom;
    if (border == fill) fill_round(cv, l, t, r, b, (float)radius, fill);
    else {
        fill_round(cv, l, t, r, b, (float)radius, border);
        fill_round(cv, l + 1, t + 1, r - 1, b - 1, (float)radius - 1, fill);
    }
}
// Circles are centred on the middle of pixel (cx, cy) and span 2 × radius + 1 pixels, as the GDI+ ones did.
extern "C" void fill_circle(Canvas *cv, int cx, int cy, int radius, COLORREF fill) {
    if (!ready(cv)) return;
    float r = radius + 0.5f;
    cv->rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 0.5f, cy + 0.5f), r, r), paint(cv, fill));
}
extern "C" void stroke_circle(Canvas *cv, int cx, int cy, int radius, COLORREF color, int width) {
    if (!ready(cv)) return;
    float r = radius + 0.5f - width / 2.0f;
    cv->rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 0.5f, cy + 0.5f), r, r), paint(cv, color), (float)width);
}
extern "C" void stroke_arc(Canvas *cv, int cx, int cy, int radius, COLORREF color, int width, double start, double sweep) {
    if (!ready(cv) || sweep <= 0) return;
    if (sweep >= 360) {
        cv->rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)cx, (float)cy), (float)radius, (float)radius), paint(cv, color), (float)width);
        return;
    }
    const double rad = 3.14159265358979323846 / 180;
    D2D1_POINT_2F a = D2D1::Point2F((float)(cx + radius * cos(start * rad)), (float)(cy + radius * sin(start * rad)));
    D2D1_POINT_2F b = D2D1::Point2F((float)(cx + radius * cos((start + sweep) * rad)), (float)(cy + radius * sin((start + sweep) * rad)));
    ID2D1PathGeometry *path = NULL; ID2D1GeometrySink *sink = NULL;
    if (FAILED(d2d->CreatePathGeometry(&path))) return;
    if (SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(b, D2D1::SizeF((float)radius, (float)radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        release(sink);
        cv->rt->DrawGeometry(path, paint(cv, color), (float)width);
    }
    release(path);
}
// Lines are 1px and leave out their last pixel, as GDI's LineTo does; straight ones are filled rectangles, so they stay sharp.
extern "C" void draw_line(Canvas *cv, int x1, int y1, int x2, int y2, COLORREF color) {
    if (!ready(cv)) return;
    if (y1 == y2) { RECT r = { x1 < x2 ? x1 : x2 + 1, y1, x1 < x2 ? x2 : x1 + 1, y1 + 1 }; fill_rect(cv, &r, color); return; }
    if (x1 == x2) { RECT r = { x1, y1 < y2 ? y1 : y2 + 1, x1 + 1, y1 < y2 ? y2 : y1 + 1 }; fill_rect(cv, &r, color); return; }
    cv->rt->DrawLine(D2D1::Point2F(x1 + 0.5f, y1 + 0.5f), D2D1::Point2F(x2 + 0.5f, y2 + 0.5f), paint(cv, color), 1.0f);
}
extern "C" void draw_dashed_line(Canvas *cv, int x1, int y1, int x2, int y2, COLORREF color) {
    (void)y2;
    // Three on, three off, as a browser dashes a 1px border.
    int step = px(3);
    for (int x = x1; x < x2; x += step * 2) { RECT r = { x, y1, x + step < x2 ? x + step : x2, y1 + 1 }; fill_rect(cv, &r, color); }
}

static ID2D1StrokeStyle *stroke_style(bool dotted) {
    static ID2D1StrokeStyle *round, *dots;
    ID2D1StrokeStyle **style = dotted ? &dots : &round;
    if (!*style && d2d) {
        if (dotted) {
            static const float dashes[] = { 1, 1 };
            d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                               D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM), dashes, 2, style);
        } else d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), NULL, 0, style);
    }
    return *style;
}
extern "C" void draw_thick_line(Canvas *cv, int x1, int y1, int x2, int y2, COLORREF color, int width) {
    if (!ready(cv)) return;
    cv->rt->DrawLine(D2D1::Point2F(x1 + 0.5f, y1 + 0.5f), D2D1::Point2F(x2 + 0.5f, y2 + 0.5f), paint(cv, color), (float)width, stroke_style(false));
}
extern "C" void draw_check_mark(Canvas *cv, const RECT *rc, COLORREF color) {
    if (!ready(cv)) return;
    // A tick in the middle of the box, as the dashboard's checked checkbox draws it: the font's ✓ sits by its line box.
    float w = (float)(rc->right - rc->left), h = (float)(rc->bottom - rc->top), x = (float)rc->left, y = (float)rc->top;
    D2D1_POINT_2F a = D2D1::Point2F(x + w * 0.24f, y + h * 0.52f), b = D2D1::Point2F(x + w * 0.42f, y + h * 0.70f), c = D2D1::Point2F(x + w * 0.76f, y + h * 0.32f);
    ID2D1PathGeometry *path = NULL; ID2D1GeometrySink *sink = NULL;
    if (FAILED(d2d->CreatePathGeometry(&path))) return;
    if (SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(b);
        sink->AddLine(c);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        release(sink);
        cv->rt->DrawGeometry(path, paint(cv, color), w * 0.13f > 1.5f ? w * 0.13f : 1.5f, stroke_style(false));
    }
    release(path);
}
extern "C" void stroke_dotted_round_rect(Canvas *cv, const RECT *rc, int radius, COLORREF color) {
    if (!ready(cv)) return;
    D2D1_RECT_F r = D2D1::RectF(rc->left + 0.5f, rc->top + 0.5f, rc->right - 0.5f, rc->bottom - 0.5f);
    cv->rt->DrawRoundedRectangle(D2D1::RoundedRect(r, (float)radius, (float)radius), paint(cv, color), 1.0f, stroke_style(true));
}

// MARK: - Pictures

/// The slot for a tile: the one that holds it, else a free one, else the least recently drawn; over the budget, older
/// tiles go as well. A slot that held another tile comes back empty.
static PictureTile *picture_slot(Canvas *cv, unsigned long long key, int x, int y, size_t bytes) {
    PictureTile *found = NULL, *oldest = &cv->tiles[0];
    size_t held = 0;
    for (int i = 0; i < PICTURE_TILES; i++) {
        PictureTile *t = &cv->tiles[i];
        if (t->bitmap && t->key == key && t->x == x && t->y == y) return t;
        if (!t->bitmap) { if (!found) found = t; continue; }
        held += t->bytes;
        if (!oldest->bitmap || cv->stamp - t->stamp > cv->stamp - oldest->stamp) oldest = t;
    }
    for (int i = 0; i < PICTURE_TILES && held + bytes > PICTURE_BUDGET; i++) {
        PictureTile *t = &cv->tiles[i];
        if (!t->bitmap || t->stamp == cv->stamp) continue;
        held -= t->bytes;
        release(t->bitmap); t->bytes = 0;
        if (!found) found = t;
    }
    if (!found) { found = oldest; release(found->bitmap); found->bytes = 0; }
    return found;
}

static bool has_pixels(const Picture *p) { return p && p->pixels && p->width > 0 && p->height > 0; }

extern "C" void draw_picture(Canvas *cv, const Picture *p, const RECT *rc) {
    if (!ready(cv) || !has_pixels(p) || rc->right <= rc->left || rc->bottom <= rc->top) return;
    cv->stamp++;
    int side = (int)cv->rt->GetMaximumBitmapSize();
    if (side <= 0 || side > PICTURE_TILE_SIDE) side = PICTURE_TILE_SIDE;
    float sx = (float)(rc->right - rc->left) / p->width, sy = (float)(rc->bottom - rc->top) / p->height;
    ID2D1DeviceContext *dc = NULL;
    if (FAILED(cv->rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void **)&dc))) dc = NULL;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    for (int y = 0; y < p->height; y += side) {
        for (int x = 0; x < p->width; x += side) {
            int w = p->width - x < side ? p->width - x : side, h = p->height - y < side ? p->height - y : side;
            size_t bytes = (size_t)w * h * 4;
            PictureTile *t = picture_slot(cv, p->key, x, y, bytes);
            if (!t->bitmap) {
                const unsigned char *first = p->pixels + ((size_t)y * p->width + x) * 4;
                if (FAILED(cv->rt->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), first, (UINT32)p->width * 4, &props, &t->bitmap))) { t->bitmap = NULL; continue; }
                t->key = p->key; t->x = x; t->y = y; t->bytes = bytes;
            }
            t->stamp = cv->stamp; t->drawn = true;
            // Tiles meet on whole source pixels, so their scaled edges are computed from the same numbers.
            D2D1_RECT_F to = D2D1::RectF(rc->left + x * sx, rc->top + y * sy, rc->left + (x + w) * sx, rc->top + (y + h) * sy);
            if (dc && (sx < 0.99f || sy < 0.99f)) dc->DrawBitmap(t->bitmap, &to, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, NULL, NULL);
            else cv->rt->DrawBitmap(t->bitmap, &to, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
        }
    }
    release(dc);
}

// MARK: - Text

/// Emoji proper (📊 🗑, past the basic plane, or asked for with U+FE0F) draw in colour, as a browser draws them; the
/// symbols the app tints as glyphs (✳ ⏹ ✎ ☑) keep the text's colour, since emoji is not their default presentation.
static bool wants_color(const wchar_t *text) {
    for (; *text; text++) if ((*text >= 0xD800 && *text <= 0xDBFF) || *text == 0xFE0F) return true;
    return false;
}

extern "C" int draw_textw(Canvas *cv, const wchar_t *text, RECT *rc, FontId f, COLORREF color, UINT flags) {
    if (!ready(cv) || !text || !*text) return 0;
    size_t len = wcslen(text);
    bool single = (flags & DT_SINGLELINE) != 0;
    wchar_t *one = NULL;
    if (single && wcspbrk(text, L"\r\n")) {
        one = (wchar_t *)xmalloc((len + 1) * sizeof *one);
        for (size_t i = 0; i <= len; i++) one[i] = text[i] == L'\r' || text[i] == L'\n' ? L' ' : text[i];
        text = one;
    }
    float w = (float)(rc->right - rc->left), h = (float)(rc->bottom - rc->top);
    IDWriteTextLayout *l = flags_layout(f, text, len, w, flags);
    if (!l) { free(one); return 0; }
    l->SetTextAlignment((flags & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER : (flags & DT_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (flags & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS)) {
        DWRITE_TRIMMING trim = { (flags & DT_WORD_ELLIPSIS) ? DWRITE_TRIMMING_GRANULARITY_WORD : DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        // A path keeps its last part (the file name) after the ellipsis.
        if (flags & DT_PATH_ELLIPSIS) { trim.delimiter = L'/'; trim.delimiterCount = 1; }
        l->SetTrimming(&trim, fonts[f].ellipsis);
    }
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    int text_h = (int)lroundf(m.height);
    float y = 0;
    if (single && (flags & DT_VCENTER)) y = (float)(((int)h - text_h) / 2);
    else if (single && (flags & DT_BOTTOM)) y = h - text_h;
    bool overflows = m.left < -0.5f || m.left + m.widthIncludingTrailingWhitespace > w + 0.5f || y < 0 || y + text_h > h + 0.5f;
    bool clip = !(flags & DT_NOCLIP) && overflows;
    if (clip) canvas_clip(cv, rc);
    D2D1_DRAW_TEXT_OPTIONS options = cv->color_fonts && wants_color(text) ? (D2D1_DRAW_TEXT_OPTIONS)4 /* ENABLE_COLOR_FONT */ : D2D1_DRAW_TEXT_OPTIONS_NONE;
    cv->rt->DrawTextLayout(D2D1::Point2F((float)rc->left, rc->top + y), l, paint(cv, color), options);
    if (clip) canvas_unclip(cv);
    l->Release();
    free(one);
    return (int)y + text_h;
}
