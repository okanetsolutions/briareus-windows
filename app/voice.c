// Voice notes: the microphone through waveIn, AAC through Media Foundation, and the server's transcription.
#include "voice.h"
#include "store.h"
#include "str.h"
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define BUFFER_COUNT 4
#define BUFFER_SAMPLES (SAMPLE_RATE / 10)
#define VOICE_CLASS L"BriareusVoice"

struct VoiceNote {
    VoiceChanged changed; void *ctx;
    VoiceState state;
    DWORD started_tick;
    char *text, *error;
    HWND window;
    HWAVEIN wave;
    WAVEHDR headers[BUFFER_COUNT];
    short *buffers[BUFFER_COUNT];
    short *pcm; size_t pcm_len, pcm_cap;   // samples
    Request *req;
    unsigned generation;
};

typedef struct { VoiceNote *note; unsigned generation; short *pcm; size_t samples; void *bytes; size_t len; char *type; char *error; } EncodeJob;

static void notify(VoiceNote *v) { if (v->changed) v->changed(v->ctx); }
static void set_state(VoiceNote *v, VoiceState s) { v->state = s; }

static void close_wave(VoiceNote *v) {
    if (!v->wave) return;
    waveInStop(v->wave);
    waveInReset(v->wave);
    for (int i = 0; i < BUFFER_COUNT; i++) if (v->headers[i].dwFlags & WHDR_PREPARED) waveInUnprepareHeader(v->wave, &v->headers[i], sizeof(WAVEHDR));
    waveInClose(v->wave);
    v->wave = NULL;
    for (int i = 0; i < BUFFER_COUNT; i++) { free(v->buffers[i]); v->buffers[i] = NULL; }
}

static void finish(VoiceNote *v) {
    close_wave(v);
    request_cancel(&v->req);
    free(v->pcm); v->pcm = NULL; v->pcm_len = v->pcm_cap = 0;
    v->generation++;
    set_state(v, VOICE_IDLE);
}

static LRESULT CALLBACK voice_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    VoiceNote *v = (VoiceNote *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == MM_WIM_DATA && v) {
        WAVEHDR *h = (WAVEHDR *)lp;
        if (h->dwBytesRecorded) {
            size_t samples = h->dwBytesRecorded / sizeof(short);
            if (v->pcm_len + samples > v->pcm_cap) { v->pcm_cap = (v->pcm_len + samples) * 2; v->pcm = xrealloc(v->pcm, v->pcm_cap * sizeof(short)); }
            memcpy(v->pcm + v->pcm_len, h->lpData, samples * sizeof(short));
            v->pcm_len += samples;
        }
        if (v->state == VOICE_RECORDING && v->wave) waveInAddBuffer(v->wave, h, sizeof(WAVEHDR));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool open_wave(VoiceNote *v, char **why) {
    WAVEFORMATEX fmt; memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 1; fmt.nSamplesPerSec = SAMPLE_RATE; fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2; fmt.nAvgBytesPerSec = SAMPLE_RATE * 2;
    MMRESULT r = waveInOpen(&v->wave, WAVE_MAPPER, &fmt, (DWORD_PTR)v->window, 0, CALLBACK_WINDOW);
    if (r != MMSYSERR_NOERROR) {
        v->wave = NULL;
        if (r == MMSYSERR_NODRIVER || r == MMSYSERR_BADDEVICEID) *why = xstrdup("No microphone was found.");
        else if (r == MMSYSERR_ALLOCATED) *why = xstrdup("The microphone is in use by another application.");
        else *why = xstrdup("Allow Briareus to use the microphone in Windows Settings \xE2\x86\x92 Privacy \xE2\x86\x92 Microphone to record voice notes.");
        return false;
    }
    for (int i = 0; i < BUFFER_COUNT; i++) {
        v->buffers[i] = xcalloc(BUFFER_SAMPLES, sizeof(short));
        memset(&v->headers[i], 0, sizeof(WAVEHDR));
        v->headers[i].lpData = (LPSTR)v->buffers[i]; v->headers[i].dwBufferLength = BUFFER_SAMPLES * sizeof(short);
        waveInPrepareHeader(v->wave, &v->headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(v->wave, &v->headers[i], sizeof(WAVEHDR));
    }
    if (waveInStart(v->wave) != MMSYSERR_NOERROR) { close_wave(v); *why = xstrdup("The microphone could not be started."); return false; }
    return true;
}

// MARK: - Encoding

static bool encode_aac(const short *pcm, size_t samples, void **out, size_t *out_len, char **error) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { *error = xstrdup("Media Foundation is unavailable."); return false; }
    bool ok = false;
    IMFSinkWriter *writer = NULL; IMFMediaType *out_type = NULL, *in_type = NULL; IMFAttributes *attributes = NULL;
    wchar_t path[MAX_PATH]; wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(path, MAX_PATH, L"%lsbriareus-voice-%lu-%lu.mp4", temp, (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    DWORD stream = 0;
    if (FAILED(MFCreateAttributes(&attributes, 2))) goto done;
    attributes->lpVtbl->SetGUID(attributes, &MF_TRANSCODE_CONTAINERTYPE, &MFTranscodeContainerType_MPEG4);
    if (FAILED(MFCreateSinkWriterFromURL(path, NULL, attributes, &writer))) { *error = xstrdup("The MP4 writer could not be created."); goto done; }
    if (FAILED(MFCreateMediaType(&out_type))) goto done;
    out_type->lpVtbl->SetGUID(out_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    out_type->lpVtbl->SetGUID(out_type, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    out_type->lpVtbl->SetUINT32(out_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out_type->lpVtbl->SetUINT32(out_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, SAMPLE_RATE);
    out_type->lpVtbl->SetUINT32(out_type, &MF_MT_AUDIO_NUM_CHANNELS, 1);
    // Speech needs no more than this: 96 kbit/s keeps five minutes near three megabytes.
    out_type->lpVtbl->SetUINT32(out_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 12000);
    if (FAILED(writer->lpVtbl->AddStream(writer, out_type, &stream))) { *error = xstrdup("The AAC encoder is unavailable."); goto done; }
    if (FAILED(MFCreateMediaType(&in_type))) goto done;
    in_type->lpVtbl->SetGUID(in_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    in_type->lpVtbl->SetGUID(in_type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, SAMPLE_RATE);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_AUDIO_NUM_CHANNELS, 1);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, SAMPLE_RATE * 2);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
    if (FAILED(writer->lpVtbl->SetInputMediaType(writer, stream, in_type, NULL))) { *error = xstrdup("The AAC encoder rejected the recording."); goto done; }
    if (FAILED(writer->lpVtbl->BeginWriting(writer))) { *error = xstrdup("The MP4 writer could not start."); goto done; }
    const size_t chunk = SAMPLE_RATE / 5;
    LONGLONG position = 0;
    for (size_t offset = 0; offset < samples; offset += chunk) {
        size_t n = samples - offset < chunk ? samples - offset : chunk;
        IMFMediaBuffer *buffer = NULL; IMFSample *sample = NULL;
        if (FAILED(MFCreateMemoryBuffer((DWORD)(n * sizeof(short)), &buffer))) goto done;
        BYTE *data = NULL; DWORD max_len = 0;
        if (SUCCEEDED(buffer->lpVtbl->Lock(buffer, &data, &max_len, NULL))) { memcpy(data, pcm + offset, n * sizeof(short)); buffer->lpVtbl->Unlock(buffer); }
        buffer->lpVtbl->SetCurrentLength(buffer, (DWORD)(n * sizeof(short)));
        if (FAILED(MFCreateSample(&sample))) { buffer->lpVtbl->Release(buffer); goto done; }
        sample->lpVtbl->AddBuffer(sample, buffer);
        LONGLONG duration = (LONGLONG)n * 10000000LL / SAMPLE_RATE;
        sample->lpVtbl->SetSampleTime(sample, position); sample->lpVtbl->SetSampleDuration(sample, duration);
        position += duration;
        HRESULT wr = writer->lpVtbl->WriteSample(writer, stream, sample);
        sample->lpVtbl->Release(sample); buffer->lpVtbl->Release(buffer);
        if (FAILED(wr)) { *error = xstrdup("The recording could not be encoded."); goto done; }
    }
    if (FAILED(writer->lpVtbl->Finalize(writer))) { *error = xstrdup("The recording could not be finished."); goto done; }
    writer->lpVtbl->Release(writer); writer = NULL;
    // Read the file back and remove it.
    HANDLE h = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size; GetFileSizeEx(h, &size);
        char *bytes = xmalloc((size_t)size.QuadPart + 1);
        DWORD total = 0, got = 0;
        while (total < size.QuadPart && ReadFile(h, bytes + total, (DWORD)(size.QuadPart - total), &got, NULL) && got) total += got;
        CloseHandle(h);
        *out = bytes; *out_len = total; ok = total > 0;
        if (!ok) free(bytes);
    }
done:
    if (writer) writer->lpVtbl->Release(writer);
    if (out_type) out_type->lpVtbl->Release(out_type);
    if (in_type) in_type->lpVtbl->Release(in_type);
    if (attributes) attributes->lpVtbl->Release(attributes);
    DeleteFileW(path);
    MFShutdown();
    if (!ok && !*error) *error = xstrdup("The recording could not be encoded.");
    return ok;
}

static void encode_wav(const short *pcm, size_t samples, void **out, size_t *out_len) {
    size_t data_len = samples * sizeof(short);
    unsigned char *bytes = xmalloc(44 + data_len);
    unsigned rate = SAMPLE_RATE, byte_rate = SAMPLE_RATE * 2, total = (unsigned)(36 + data_len), dl = (unsigned)data_len;
    memcpy(bytes, "RIFF", 4); memcpy(bytes + 4, &total, 4); memcpy(bytes + 8, "WAVEfmt ", 8);
    unsigned fmt_len = 16; unsigned short pcm_tag = 1, channels = 1, align = 2, bits = 16;
    memcpy(bytes + 16, &fmt_len, 4); memcpy(bytes + 20, &pcm_tag, 2); memcpy(bytes + 22, &channels, 2); memcpy(bytes + 24, &rate, 4);
    memcpy(bytes + 28, &byte_rate, 4); memcpy(bytes + 32, &align, 2); memcpy(bytes + 34, &bits, 2);
    memcpy(bytes + 36, "data", 4); memcpy(bytes + 40, &dl, 4);
    memcpy(bytes + 44, pcm, data_len);
    *out = bytes; *out_len = 44 + data_len;
}

static void encode_work(void *p) {
    EncodeJob *job = p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (encode_aac(job->pcm, job->samples, &job->bytes, &job->len, &job->error)) job->type = xstrdup("audio/mp4");
    else { free(job->error); job->error = NULL; encode_wav(job->pcm, job->samples, &job->bytes, &job->len); job->type = xstrdup("audio/wav"); }
    CoUninitialize();
}
static void transcribe_done(void *owner, Request *req) {
    VoiceNote *v = owner;
    if (v->state != VOICE_TRANSCRIBING) return;
    if (req->ok) { free(v->text); v->text = xstrdup(req->text ? req->text : ""); finish(v); }
    else {
        char *why = request_error_text(req);
        free(v->error); v->error = xstrfmt("The voice note could not be transcribed: %s", why);
        free(why); finish(v);
    }
    notify(v);
}
static void encode_done(void *p) {
    EncodeJob *job = p;
    VoiceNote *v = job->note;
    if (v && v->generation == job->generation && v->state == VOICE_TRANSCRIBING) {
        if (job->bytes) store_transcribe(job->bytes, job->len, job->type, v, transcribe_done, 0, &v->req);
        else { free(v->error); v->error = xstrdup("The voice note could not be recorded."); finish(v); notify(v); }
    }
    free(job->pcm); free(job->bytes); free(job->type); free(job->error); free(job);
}

// MARK: - API

VoiceNote *voice_new(VoiceChanged changed, void *ctx) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = voice_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = VOICE_CLASS;
        RegisterClassW(&wc); registered = true;
    }
    VoiceNote *v = xcalloc(1, sizeof *v);
    v->changed = changed; v->ctx = ctx;
    v->window = CreateWindowExW(0, VOICE_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(v->window, GWLP_USERDATA, (LONG_PTR)v);
    return v;
}
void voice_free(VoiceNote *v) {
    if (!v) return;
    v->changed = NULL;
    finish(v);
    if (v->window) { SetWindowLongPtrW(v->window, GWLP_USERDATA, 0); DestroyWindow(v->window); }
    free(v->text); free(v->error); free(v);
}
VoiceState voice_state(const VoiceNote *v) { return v->state; }
int voice_elapsed(const VoiceNote *v) { return v->state == VOICE_RECORDING ? (int)((GetTickCount() - v->started_tick) / 1000) : 0; }

static void permitted(void *ctx, const char *reason) {
    VoiceNote *v = ctx;
    if (v->state != VOICE_STARTING) return;   // dropped while the server was asked
    if (reason) { set_state(v, VOICE_IDLE); free(v->error); v->error = xstrdup(reason); notify(v); return; }
    if (!store_can_transcribe()) { set_state(v, VOICE_IDLE); notify(v); return; }
    char *why = NULL;
    if (!open_wave(v, &why)) { set_state(v, VOICE_IDLE); free(v->error); v->error = why; notify(v); return; }
    free(v->pcm); v->pcm = NULL; v->pcm_len = v->pcm_cap = 0;
    v->started_tick = GetTickCount();
    set_state(v, VOICE_RECORDING);
    notify(v);
}
void voice_record(VoiceNote *v) {
    if (v->state != VOICE_IDLE) return;
    set_state(v, VOICE_STARTING); free(v->error); v->error = NULL;
    notify(v);
    store_voice_notes_off(permitted, v);
}
void voice_stop(VoiceNote *v) {
    if (v->state != VOICE_RECORDING) return;
    set_state(v, VOICE_TRANSCRIBING);
    close_wave(v);
    if (!v->pcm_len) { free(v->error); v->error = xstrdup("The voice note could not be recorded."); finish(v); notify(v); return; }
    EncodeJob *job = xcalloc(1, sizeof *job);
    job->note = v; job->generation = v->generation;
    job->pcm = v->pcm; job->samples = v->pcm_len;
    v->pcm = NULL; v->pcm_len = v->pcm_cap = 0;
    async_run(encode_work, encode_done, job);
    notify(v);
}
void voice_drop(VoiceNote *v) {
    if (v->state == VOICE_IDLE) return;
    finish(v);
    notify(v);
}
void voice_tick(VoiceNote *v) { if (v->state == VOICE_RECORDING && voice_elapsed(v) >= VOICE_LIMIT_SECONDS) voice_stop(v); }
char *voice_take_text(VoiceNote *v) { char *t = v->text; v->text = NULL; return t; }
char *voice_take_error(VoiceNote *v) { char *e = v->error; v->error = NULL; return e; }
