// Picture files decoded by the Windows Imaging Component into premultiplied BGRA, scaled as they are read.
#include "picture.h"
#include "str.h"
#include <objidl.h>
#include <wincodec.h>
#include <stdlib.h>
#include <string.h>

// Windows Imaging Component, by identifier: no import library beyond ole32 is needed.
static const GUID clsid_wic_factory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID iid_wic_factory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID wic_pbgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

enum { PICTURE_MAX_SIDE = 30000 };

static volatile LONG64 next_key;

void picture_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h) {
    double scale = 1;
    if (max_w > 0 && w > max_w) scale = (double)max_w / w;
    if (max_h > 0 && h * scale > max_h) scale = (double)max_h / h;
    *out_w = (int)(w * scale + 0.5); *out_h = (int)(h * scale + 0.5);
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

/// The size to decode a `w` × `h` file at.
static void decoded_size(int w, int h, int max_w, int max_h, int *out_w, int *out_h) {
    picture_fit(w, h, max_w, max_h, out_w, out_h);
    while ((double)*out_w * *out_h > PICTURE_MAX_PIXELS) picture_fit(*out_w, *out_h, *out_w * 9 / 10, 0, out_w, out_h);
}

/// The frame's pixels through a converter and, when the size differs, a scaler; NULL when a step fails.
static unsigned char *read_pixels(IWICImagingFactory *wic, IWICBitmapFrameDecode *frame, int full_w, int full_h, int w, int h) {
    unsigned char *pixels = NULL;
    IWICFormatConverter *converter = NULL; IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *source = NULL;
    if (FAILED(wic->lpVtbl->CreateFormatConverter(wic, &converter))) goto out;
    if (FAILED(converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)frame, &wic_pbgra, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) goto out;
    source = (IWICBitmapSource *)converter;
    if (w != full_w || h != full_h) {
        if (FAILED(wic->lpVtbl->CreateBitmapScaler(wic, &scaler))) goto out;
        if (FAILED(scaler->lpVtbl->Initialize(scaler, source, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant))) goto out;
        source = (IWICBitmapSource *)scaler;
    }
    pixels = xmalloc((size_t)w * h * 4);
    if (FAILED(source->lpVtbl->CopyPixels(source, NULL, (UINT)w * 4, (UINT)w * (UINT)h * 4, pixels))) { free(pixels); pixels = NULL; }
out:
    if (scaler) scaler->lpVtbl->Release(scaler);
    if (converter) converter->lpVtbl->Release(converter);
    return pixels;
}

bool picture_decode(const void *bytes, size_t len, int max_w, int max_h, Picture *out, int *full_w, int *full_h) {
    memset(out, 0, sizeof *out); *full_w = *full_h = 0;
    if (!bytes || !len || len > 0xFFFFFFFFu) return false;
    // A pool thread may already be in an apartment of another kind: COM is up either way, and only our own start is undone.
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = NULL; IWICStream *stream = NULL; IWICBitmapDecoder *decoder = NULL; IWICBitmapFrameDecode *frame = NULL;
    UINT width = 0, height = 0;
    if (FAILED(CoCreateInstance(&clsid_wic_factory, NULL, CLSCTX_INPROC_SERVER, &iid_wic_factory, (void **)&wic))) goto out;
    if (FAILED(wic->lpVtbl->CreateStream(wic, &stream))) goto out;
    if (FAILED(stream->lpVtbl->InitializeFromMemory(stream, (BYTE *)bytes, (DWORD)len))) goto out;
    if (FAILED(wic->lpVtbl->CreateDecoderFromStream(wic, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder))) goto out;
    if (FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame))) goto out;
    if (FAILED(frame->lpVtbl->GetSize(frame, &width, &height)) || !width || !height) goto out;
    if (width > PICTURE_MAX_SIDE || height > PICTURE_MAX_SIDE) goto out;
    int w, h;
    decoded_size((int)width, (int)height, max_w, max_h, &w, &h);
    out->pixels = read_pixels(wic, frame, (int)width, (int)height, w, h);
    if (out->pixels) {
        out->width = w; out->height = h; out->key = (unsigned long long)InterlockedIncrement64(&next_key);
        *full_w = (int)width; *full_h = (int)height;
    }
out:
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    if (stream) stream->lpVtbl->Release(stream);
    if (wic) wic->lpVtbl->Release(wic);
    if (SUCCEEDED(co)) CoUninitialize();
    return out->pixels != NULL;
}

void picture_free(Picture *picture) {
    if (!picture) return;
    free(picture->pixels);
    memset(picture, 0, sizeof *picture);
}
