/*
 * platform_win32_video.c -- H.264/AAC MP4 recording on Windows.
 *
 * Video   : Media Foundation IMFSinkWriter (H.264 in MP4)
 * Audio   : WASAPI loopback -- "record what you hear" -- encoded to AAC
 *
 * Both are operating-system components, so this stays consistent with the
 * rest of the project: nothing to download, nothing to install, one binary.
 * ffmpeg would have been less code but it is not present on this machine and
 * would make the program depend on something the user has to go and get.
 *
 * Audio is deliberately optional at runtime. If there is no render endpoint,
 * or loopback fails, recording continues WITHOUT sound rather than failing --
 * a silent capture beats no capture.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define COBJMACROS                 /* C-friendly COM: IFoo_Method(p, ...) */

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

/* ---- tuning ----------------------------------------------------------- */
#define VID_FPS_DEFAULT   30
#define VID_BITRATE       6000000       /* 6 Mbit -- good for screen content */
#define AUD_SAMPLE_RATE   48000
#define AUD_CHANNELS      2
#define AUD_BITS          16
#define AUD_AAC_BYTES_SEC 24000         /* 192 kbit AAC */

/* 100-nanosecond units: Media Foundation's native timebase. */
#define HNS_PER_MS        10000LL

typedef struct {
    bool              active;
    IMFSinkWriter*    writer;
    DWORD             vstream;
    DWORD             astream;
    bool              has_audio;
    int               w, h, fps;
    LONGLONG          frame_dur_hns;
    uint64_t          start_ms;

    /* audio capture -- up to two endpoints, summed */
    int                  audio_src;     /* PLAT_AUDIO_* */
    IMMDeviceEnumerator* enumr;

    IMMDevice*           dev_sys;       /* render endpoint, opened LOOPBACK */
    IAudioClient*        cl_sys;
    IAudioCaptureClient* cap_sys;
    WAVEFORMATEX*        fmt_sys;

    IMMDevice*           dev_mic;       /* capture endpoint */
    IAudioClient*        cl_mic;
    IAudioCaptureClient* cap_mic;
    WAVEFORMATEX*        fmt_mic;

    /* Mic samples land here and are drained against the master clock. */
    int16_t*             mic_ring;
    size_t               mic_cap;       /* in int16 samples */
    size_t               mic_head, mic_tail;
    CRITICAL_SECTION     mic_lock;

    HANDLE               athread;
    volatile LONG        astop;
    CRITICAL_SECTION     lock;          /* the sink writer is not re-entrant */
    LONGLONG             audio_pos_hns;
    HRESULT finalize_hr;   /* why a file ended up with no moov */
} VidRec;

static VidRec V;

/* WASAPI GUIDs are not carried by any of MinGW's link libraries, so define
 * them here rather than adding a library that may or may not exist on
 * someone else's toolchain. Values are from mmdeviceapi.h / audioclient.h. */
static const GUID CLSID_MMDeviceEnumerator_ =
    {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID IID_IMMDeviceEnumerator_ =
    {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID IID_IAudioClient_ =
    {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID IID_IAudioCaptureClient_ =
    {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};

/* MFSetAttributeSize / MFSetAttributeRatio are C++-only inlines in the
 * MinGW headers. They do nothing but pack two 32-bit values into a UINT64,
 * so define the same thing for C. */
static HRESULT mf_set_pair(IMFAttributes* a, const GUID* key, UINT32 hi, UINT32 lo) {
    return IMFAttributes_SetUINT64(a, key, ((UINT64)hi << 32) | (UINT64)lo);
}

/* ---- audio: WASAPI loopback ------------------------------------------- */

/* Convert whatever the mixer hands us (normally 32-bit float) to the 16-bit
 * stereo PCM the AAC encoder expects. Returns bytes written. */
static size_t audio_to_pcm16(const BYTE* src, UINT32 frames,
                             const WAVEFORMATEX* fmt, int16_t* dst, size_t dst_cap) {
    int ch = fmt->nChannels;
    bool is_float = false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) is_float = true;
    else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ex = (const WAVEFORMATEXTENSIBLE*)fmt;
        if (IsEqualGUID(&ex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            is_float = true;
    }
    size_t out = 0;
    for (UINT32 i = 0; i < frames; i++) {
        for (int c = 0; c < AUD_CHANNELS; c++) {
            int sc = (c < ch) ? c : ch - 1;         /* mono -> both sides */
            int16_t v = 0;
            if (is_float) {
                float f = ((const float*)src)[i * ch + sc];
                if (f >  1.0f) f =  1.0f;
                if (f < -1.0f) f = -1.0f;
                v = (int16_t)(f * 32767.0f);
            } else if (fmt->wBitsPerSample == 16) {
                v = ((const int16_t*)src)[i * ch + sc];
            }
            if (out + 1 > dst_cap) return out * sizeof(int16_t);
            dst[out++] = v;
        }
    }
    return out * sizeof(int16_t);
}

/* Drain everything currently queued on the mic into the ring buffer. */
static void mic_pump(void) {
    if (!V.cap_mic || !V.mic_ring) return;
    int16_t tmp[19200];
    UINT32 packet = 0;
    if (FAILED(IAudioCaptureClient_GetNextPacketSize(V.cap_mic, &packet))) return;
    while (packet > 0) {
        BYTE* d = NULL; UINT32 fr = 0; DWORD fl = 0;
        if (FAILED(IAudioCaptureClient_GetBuffer(V.cap_mic, &d, &fr, &fl, NULL, NULL)))
            return;
        size_t n = 0;
        if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT))
            n = audio_to_pcm16(d, fr, V.fmt_mic, tmp, sizeof tmp / sizeof tmp[0])
                / sizeof(int16_t);
        else
            n = (size_t)fr * AUD_CHANNELS;
        IAudioCaptureClient_ReleaseBuffer(V.cap_mic, fr);

        EnterCriticalSection(&V.mic_lock);
        for (size_t i = 0; i < n; i++) {
            size_t next = (V.mic_head + 1) % V.mic_cap;
            if (next == V.mic_tail) {          /* full: drop oldest */
                V.mic_tail = (V.mic_tail + 1) % V.mic_cap;
            }
            V.mic_ring[V.mic_head] = (fl & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : tmp[i];
            V.mic_head = next;
        }
        LeaveCriticalSection(&V.mic_lock);

        if (FAILED(IAudioCaptureClient_GetNextPacketSize(V.cap_mic, &packet))) return;
    }
}

/* Sum `count` mic samples into pcm[], padding with silence if the mic is
 * behind. Mixing against the master's clock is what keeps the two streams
 * from drifting -- they are independent devices with independent timing. */
static void mic_mix_into(int16_t* pcm, size_t count) {
    if (!V.mic_ring) return;
    EnterCriticalSection(&V.mic_lock);
    for (size_t i = 0; i < count; i++) {
        if (V.mic_tail == V.mic_head) break;        /* nothing queued */
        int32_t sum = (int32_t)pcm[i] + (int32_t)V.mic_ring[V.mic_tail];
        V.mic_tail = (V.mic_tail + 1) % V.mic_cap;
        if (sum >  32767) sum =  32767;             /* clamp, do not wrap */
        if (sum < -32768) sum = -32768;
        pcm[i] = (int16_t)sum;
    }
    LeaveCriticalSection(&V.mic_lock);
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int16_t pcm[19200];                              /* 100 ms headroom */

    /* Whichever endpoint we have drives the timeline. Loopback is preferred
     * because it ticks steadily even in silence; a mic can go quiet. */
    bool sys_master = (V.cap_sys != NULL);
    IAudioCaptureClient* master = sys_master ? V.cap_sys : V.cap_mic;
    const WAVEFORMATEX*  mfmt   = sys_master ? V.fmt_sys : V.fmt_mic;
    bool mix_mic = (V.audio_src == PLAT_AUDIO_BOTH) && sys_master;
    if (!master) { CoUninitialize(); return 0; }

    while (!InterlockedCompareExchange(&V.astop, 0, 0)) {
        if (mix_mic) mic_pump();

        UINT32 packet = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(master, &packet))) break;
        if (packet == 0) { Sleep(5); continue; }

        while (packet > 0) {
            BYTE* data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(IAudioCaptureClient_GetBuffer(master, &data, &frames,
                                                     &flags, NULL, NULL))) break;
            size_t bytes;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                bytes = (size_t)frames * AUD_CHANNELS * sizeof(int16_t);
                if (bytes > sizeof pcm) bytes = sizeof pcm;
                memset(pcm, 0, bytes);
            } else {
                bytes = audio_to_pcm16(data, frames, mfmt, pcm,
                                       sizeof pcm / sizeof pcm[0]);
            }
            IAudioCaptureClient_ReleaseBuffer(master, frames);
            if (mix_mic) mic_mix_into(pcm, bytes / sizeof(int16_t));

            if (bytes > 0) {
                IMFMediaBuffer* mbuf = NULL;
                if (SUCCEEDED(MFCreateMemoryBuffer((DWORD)bytes, &mbuf))) {
                    BYTE* p = NULL;
                    if (SUCCEEDED(IMFMediaBuffer_Lock(mbuf, &p, NULL, NULL))) {
                        memcpy(p, pcm, bytes);
                        IMFMediaBuffer_Unlock(mbuf);
                        IMFMediaBuffer_SetCurrentLength(mbuf, (DWORD)bytes);
                        IMFSample* smp = NULL;
                        if (SUCCEEDED(MFCreateSample(&smp))) {
                            IMFSample_AddBuffer(smp, mbuf);
                            LONGLONG dur =
                                (LONGLONG)(bytes / (AUD_CHANNELS * sizeof(int16_t)))
                                * 10000000LL / AUD_SAMPLE_RATE;
                            EnterCriticalSection(&V.lock);
                            IMFSample_SetSampleTime(smp, V.audio_pos_hns);
                            IMFSample_SetSampleDuration(smp, dur);
                            if (V.writer && V.has_audio)
                                IMFSinkWriter_WriteSample(V.writer, V.astream, smp);
                            V.audio_pos_hns += dur;
                            LeaveCriticalSection(&V.lock);
                            IMFSample_Release(smp);
                        }
                    }
                    IMFMediaBuffer_Release(mbuf);
                }
            }
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(master, &packet))) break;
        }
    }
    CoUninitialize();
    return 0;
}

/* Open one endpoint. `loopback` picks a render device recorded backwards
 * (system sound) versus a real capture device (a microphone). */
static bool open_endpoint(bool loopback, IMMDevice** pdev, IAudioClient** pcl,
                          IAudioCaptureClient** pcap, WAVEFORMATEX** pfmt) {
    HRESULT hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        V.enumr, loopback ? eRender : eCapture, eConsole, pdev);
    if (FAILED(hr) || !*pdev) return false;
    hr = IMMDevice_Activate(*pdev, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void**)pcl);
    if (FAILED(hr) || !*pcl) return false;
    hr = IAudioClient_GetMixFormat(*pcl, pfmt);
    if (FAILED(hr) || !*pfmt) return false;
    hr = IAudioClient_Initialize(*pcl, AUDCLNT_SHAREMODE_SHARED,
                                 loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                 5000000 /* 500 ms */, 0, *pfmt, NULL);
    if (FAILED(hr)) return false;
    hr = IAudioClient_GetService(*pcl, &IID_IAudioCaptureClient_, (void**)pcap);
    if (FAILED(hr) || !*pcap) return false;
    return SUCCEEDED(IAudioClient_Start(*pcl));
}

static bool audio_start(int src) {
    if (src == PLAT_AUDIO_NONE) return false;
    V.audio_src = src;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator_, (void**)&V.enumr);
    if (FAILED(hr) || !V.enumr) return false;

    bool want_sys = (src == PLAT_AUDIO_SYSTEM || src == PLAT_AUDIO_BOTH);
    bool want_mic = (src == PLAT_AUDIO_MIC    || src == PLAT_AUDIO_BOTH);
    bool got_sys = false, got_mic = false;

    if (want_sys)
        got_sys = open_endpoint(true,  &V.dev_sys, &V.cl_sys, &V.cap_sys, &V.fmt_sys);
    if (want_mic)
        got_mic = open_endpoint(false, &V.dev_mic, &V.cl_mic, &V.cap_mic, &V.fmt_mic);

    /* Take whatever we actually got -- a missing mic should not silence the
     * system track, and vice versa. */
    if (!got_sys && !got_mic) return false;
    V.audio_src = (got_sys && got_mic) ? PLAT_AUDIO_BOTH
                 : got_sys ? PLAT_AUDIO_SYSTEM : PLAT_AUDIO_MIC;

    if (V.audio_src == PLAT_AUDIO_BOTH) {
        InitializeCriticalSection(&V.mic_lock);
        V.mic_cap  = AUD_SAMPLE_RATE * AUD_CHANNELS * 2;   /* 2 s of slack */
        V.mic_ring = (int16_t*)calloc(V.mic_cap, sizeof(int16_t));
        V.mic_head = V.mic_tail = 0;
        if (!V.mic_ring) V.audio_src = got_sys ? PLAT_AUDIO_SYSTEM : PLAT_AUDIO_MIC;
    }

    V.astop = 0;
    V.athread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    return V.athread != NULL;
}

static void audio_stop(void) {
    if (V.athread) {
        InterlockedExchange(&V.astop, 1);
        WaitForSingleObject(V.athread, 2000);
        CloseHandle(V.athread);
        V.athread = NULL;
    }
    if (V.cl_sys)  IAudioClient_Stop(V.cl_sys);
    if (V.cl_mic)  IAudioClient_Stop(V.cl_mic);
    if (V.cap_sys) { IAudioCaptureClient_Release(V.cap_sys); V.cap_sys = NULL; }
    if (V.cap_mic) { IAudioCaptureClient_Release(V.cap_mic); V.cap_mic = NULL; }
    if (V.fmt_sys) { CoTaskMemFree(V.fmt_sys); V.fmt_sys = NULL; }
    if (V.fmt_mic) { CoTaskMemFree(V.fmt_mic); V.fmt_mic = NULL; }
    if (V.cl_sys)  { IAudioClient_Release(V.cl_sys); V.cl_sys = NULL; }
    if (V.cl_mic)  { IAudioClient_Release(V.cl_mic); V.cl_mic = NULL; }
    if (V.dev_sys) { IMMDevice_Release(V.dev_sys); V.dev_sys = NULL; }
    if (V.dev_mic) { IMMDevice_Release(V.dev_mic); V.dev_mic = NULL; }
    if (V.enumr)   { IMMDeviceEnumerator_Release(V.enumr); V.enumr = NULL; }
    if (V.mic_ring) {
        free(V.mic_ring); V.mic_ring = NULL;
        DeleteCriticalSection(&V.mic_lock);
    }
}

/* ---- video ------------------------------------------------------------ */

bool plat_video_start(const char* path, int w, int h, int fps, int audio_src) {
    if (V.active) return false;
    memset(&V, 0, sizeof V);
    InitializeCriticalSection(&V.lock);

    if (fps <= 0) fps = VID_FPS_DEFAULT;
    /* H.264 wants even dimensions. */
    w &= ~1; h &= ~1;
    if (w < 16 || h < 16) return false;

    V.w = w; V.h = h; V.fps = fps;
    V.frame_dur_hns = 10000000LL / fps;

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;

    wchar_t wpath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) == 0) {
        MFShutdown(); return false;
    }

    IMFAttributes* attr = NULL;
    if (SUCCEEDED(MFCreateAttributes(&attr, 2))) {
        IMFAttributes_SetUINT32(attr, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1);
        IMFAttributes_SetUINT32(attr, &MF_SINK_WRITER_DISABLE_THROTTLING, 1);
    }
    HRESULT hr = MFCreateSinkWriterFromURL(wpath, NULL, attr, &V.writer);
    if (attr) IMFAttributes_Release(attr);
    if (FAILED(hr) || !V.writer) { MFShutdown(); return false; }

    /* --- video: H.264 out, RGB32 in --- */
    IMFMediaType* vout = NULL;
    MFCreateMediaType(&vout);
    IMFMediaType_SetGUID(vout, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(vout, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    /* Scale the bitrate with the frame, rather than spending 6 Mbit on
     * whatever size turns up. 6 Mbit was chosen for full-screen capture; at
     * 640x480 it is roughly four times what the picture can use -- big files,
     * no better image -- and at 1080p it would be thin. ~0.1 bit per pixel
     * per frame, clamped so a tiny region still gets enough and a large one
     * does not run away. */
    {
        unsigned long long px  = (unsigned long long)w * (unsigned long long)h;
        unsigned long long rate = px * (unsigned long long)fps / 4ULL;
        /* Floor at 4 Mbit deliberately. The arithmetic alone would give a
         * 640x480 camera frame about 2.3 Mbit, and cutting what a small frame
         * already receives is the wrong direction -- camera sensor noise is
         * expensive to encode, and the complaint that started this was
         * quality, not file size. The point of scaling is that a 1080p
         * capture is no longer starved on the 6 Mbit that was chosen for
         * screen content, not that VGA should get less. */
        if (rate < 4000000ULL)  rate = 4000000ULL;
        if (rate > 16000000ULL) rate = 16000000ULL;
        IMFMediaType_SetUINT32(vout, &MF_MT_AVG_BITRATE, (UINT32)rate);
    }
    IMFMediaType_SetUINT32(vout, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mf_set_pair((IMFAttributes*)vout, &MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
    mf_set_pair((IMFAttributes*)vout, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    mf_set_pair((IMFAttributes*)vout, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = IMFSinkWriter_AddStream(V.writer, vout, &V.vstream);
    IMFMediaType_Release(vout);
    if (FAILED(hr)) goto fail;

    IMFMediaType* vin = NULL;
    MFCreateMediaType(&vin);
    IMFMediaType_SetGUID(vin, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(vin, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    IMFMediaType_SetUINT32(vin, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mf_set_pair((IMFAttributes*)vin, &MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
    mf_set_pair((IMFAttributes*)vin, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    mf_set_pair((IMFAttributes*)vin, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = IMFSinkWriter_SetInputMediaType(V.writer, V.vstream, vin, NULL);
    IMFMediaType_Release(vin);
    if (FAILED(hr)) goto fail;

    /* --- audio: AAC out, PCM16 in (optional) --- */
    if (audio_src != PLAT_AUDIO_NONE) {
        IMFMediaType* aout = NULL;
        MFCreateMediaType(&aout);
        IMFMediaType_SetGUID(aout, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(aout, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
        IMFMediaType_SetUINT32(aout, &MF_MT_AUDIO_BITS_PER_SAMPLE, AUD_BITS);
        IMFMediaType_SetUINT32(aout, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AUD_SAMPLE_RATE);
        IMFMediaType_SetUINT32(aout, &MF_MT_AUDIO_NUM_CHANNELS, AUD_CHANNELS);
        IMFMediaType_SetUINT32(aout, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUD_AAC_BYTES_SEC);
        if (SUCCEEDED(IMFSinkWriter_AddStream(V.writer, aout, &V.astream))) {
            IMFMediaType* ain = NULL;
            MFCreateMediaType(&ain);
            IMFMediaType_SetGUID(ain, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
            IMFMediaType_SetGUID(ain, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
            IMFMediaType_SetUINT32(ain, &MF_MT_AUDIO_BITS_PER_SAMPLE, AUD_BITS);
            IMFMediaType_SetUINT32(ain, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AUD_SAMPLE_RATE);
            IMFMediaType_SetUINT32(ain, &MF_MT_AUDIO_NUM_CHANNELS, AUD_CHANNELS);
            IMFMediaType_SetUINT32(ain, &MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                   AUD_CHANNELS * AUD_BITS / 8);
            IMFMediaType_SetUINT32(ain, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                   AUD_SAMPLE_RATE * AUD_CHANNELS * AUD_BITS / 8);
            if (SUCCEEDED(IMFSinkWriter_SetInputMediaType(V.writer, V.astream, ain, NULL)))
                V.has_audio = true;
            IMFMediaType_Release(ain);
        }
        IMFMediaType_Release(aout);
    }

    if (FAILED(IMFSinkWriter_BeginWriting(V.writer))) goto fail;

    /* Sound is a bonus, never a blocker: if loopback will not start we keep
     * recording video and simply have no audio track. */
    if (V.has_audio && !audio_start(audio_src)) {
        audio_stop();
        V.has_audio = false;
    }

    V.start_ms = plat_now_ms();
    V.active = true;
    return true;

fail:
    if (V.writer) { IMFSinkWriter_Release(V.writer); V.writer = NULL; }
    MFShutdown();
    DeleteCriticalSection(&V.lock);
    return false;
}

bool plat_video_has_audio(void) { return V.active && V.has_audio; }

bool plat_video_write_frame(const uint8_t* rgba, uint64_t t_ms) {
    if (!V.active || !V.writer) return false;

    DWORD stride = (DWORD)V.w * 4;
    DWORD size   = stride * (DWORD)V.h;
    IMFMediaBuffer* buf = NULL;
    if (FAILED(MFCreateMemoryBuffer(size, &buf))) return false;

    BYTE* dst = NULL;
    if (FAILED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        IMFMediaBuffer_Release(buf); return false;
    }
    /* RGBA (top-down) -> BGRA bottom-up, which is what RGB32 means here. */
    for (int y = 0; y < V.h; y++) {
        const uint8_t* srow = rgba + (size_t)y * stride;
        BYTE* drow = dst + (size_t)(V.h - 1 - y) * stride;
        for (int x = 0; x < V.w; x++) {
            drow[x*4 + 0] = srow[x*4 + 2];   /* B */
            drow[x*4 + 1] = srow[x*4 + 1];   /* G */
            drow[x*4 + 2] = srow[x*4 + 0];   /* R */
            drow[x*4 + 3] = 255;
        }
    }
    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_SetCurrentLength(buf, size);

    IMFSample* smp = NULL;
    bool ok = false;
    if (SUCCEEDED(MFCreateSample(&smp))) {
        IMFSample_AddBuffer(smp, buf);
        /* Timestamp from the wall clock so video tracks reality even when a
         * frame is late -- fixed increments would drift against the audio. */
        LONGLONG t = (LONGLONG)(t_ms - V.start_ms) * HNS_PER_MS;
        IMFSample_SetSampleTime(smp, t);
        IMFSample_SetSampleDuration(smp, V.frame_dur_hns);
        EnterCriticalSection(&V.lock);
        ok = SUCCEEDED(IMFSinkWriter_WriteSample(V.writer, V.vstream, smp));
        LeaveCriticalSection(&V.lock);
        IMFSample_Release(smp);
    }
    IMFMediaBuffer_Release(buf);
    return ok;
}

void plat_video_stop(void) {
    if (!V.active) return;
    audio_stop();
    EnterCriticalSection(&V.lock);
    if (V.writer) {
        /* Finalize writes the moov box -- the index without which the file
         * is unplayable no matter how much sample data reached mdat. Its
         * result was being thrown away, so a 3 MB file with no moov looked
         * like a successful recording. */
        V.finalize_hr = IMFSinkWriter_Finalize(V.writer);
        IMFSinkWriter_Release(V.writer);
        V.writer = NULL;
    }
    LeaveCriticalSection(&V.lock);
    MFShutdown();
    DeleteCriticalSection(&V.lock);
    V.active = false;
}

/* ======================================================================
 * Camera capture -- recorded DIRECTLY, not composited over the screen.
 *
 * Joe's clarification ("camera would only record directly not be added to
 * the recording of the screen") is what makes this small. Compositing a
 * webcam into screen frames would need a second live pipeline, per-frame
 * blending, and position/size UI. Recording the camera on its own is just
 * a source reader feeding the sink writer that already exists.
 * ==================================================================== */

#include <mfobjects.h>

/* Not carried by MinGW's link libraries -- same story as the WASAPI GUIDs. */
static const GUID MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_ =
    {0xC60AC5FE,0x252A,0x478F,{0xA0,0xEF,0xBC,0x8F,0xA5,0xF7,0xCA,0xD3}};
static const GUID MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID_ =
    {0x8AC3587A,0x4AE7,0x42D8,{0x99,0xE0,0x0A,0x60,0x13,0xEE,0xF9,0x0F}};
static const GUID MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME_ =
    {0x60D0E559,0x52F8,0x4FA2,{0xBB,0xCE,0xAC,0xDB,0x34,0xA8,0xEC,0x01}};
static const GUID IID_IMFMediaSource_ =
    {0x279A808D,0xAEC7,0x40C8,{0x9C,0x6B,0xA6,0xB4,0x92,0xC7,0x8A,0x66}};

static struct {
    IMFSourceReader* reader;
    int  w, h;
    bool open;
    LONG   stride;      /* signed: negative means the rows
                         * arrive bottom-up */
} CAM;

static HRESULT mf_get_pair(IMFAttributes* a, const GUID* key,
                           UINT32* hi, UINT32* lo) {
    UINT64 v = 0;
    HRESULT hr = IMFAttributes_GetUINT64(a, key, &v);
    if (SUCCEEDED(hr)) { *hi = (UINT32)(v >> 32); *lo = (UINT32)(v & 0xFFFFFFFF); }
    return hr;
}

int plat_camera_list(char names[][64], int max) {
    if (max <= 0) return 0;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 0;

    IMFAttributes* attr = NULL;
    int n = 0;
    if (SUCCEEDED(MFCreateAttributes(&attr, 1))) {
        IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_,
                              &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID_);
        IMFActivate** devs = NULL;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attr, &devs, &count)) && devs) {
            for (UINT32 i = 0; i < count && n < max; i++) {
                WCHAR* wname = NULL; UINT32 len = 0;
                if (SUCCEEDED(IMFActivate_GetAllocatedString(
                        devs[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME_,
                        &wname, &len)) && wname) {
                    WideCharToMultiByte(CP_UTF8, 0, wname, -1,
                                        names[n], 64, NULL, NULL);
                    CoTaskMemFree(wname);
                } else {
                    snprintf(names[n], 64, "Camera %u", i + 1);
                }
                n++;
            }
            for (UINT32 i = 0; i < count; i++) IMFActivate_Release(devs[i]);
            CoTaskMemFree(devs);
        }
        IMFAttributes_Release(attr);
    }
    MFShutdown();
    return n;
}

/* Set by plat_camera_open() so the failure can be explained rather than
 * guessed at. */
/* Without this the source reader will NOT insert a format converter, and
 * asking a YUY2/MJPG webcam for RGB32 simply fails. The old code asked for
 * RGB32 with a comment saying MF "inserts a converter when the camera only
 * speaks NV12/MJPG" -- it does, but only when this is set, and it was not.
 * Three cameras that work perfectly in every other application therefore
 * refused to record. */
static const GUID MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING_ =
  {0xfb394f3d,0xccf1,0x42ee,{0xbb,0xb3,0xf9,0xb8,0x45,0xd5,0x68,0x1d}};

static HRESULT g_cam_last_hr = S_OK;

/* What the mode picker settled on, for diagnosis. */
static struct { UINT32 w,h,num,den; HRESULT set_hr; int pass; } cam_pick;


void plat_camera_last_error(char* out, size_t sz) {
    if (!out || sz == 0) return;
    switch ((unsigned long)g_cam_last_hr) {
    case 0UL:
        out[0] = 0; return;
    case 0xC00D3704UL:   /* MF_E_HW_MFT_FAILED_START_STREAMING */
        snprintf(out, sz,
                 "the camera would not start -- no hardware resources. On USB "
                 "this usually means other cameras on the same controller have "
                 "used up its bandwidth; try unplugging one, or use a camera on "
                 "a different USB port.");
        return;
    case 0x80070020UL:   /* ERROR_SHARING_VIOLATION */
    case 0x800700AAUL:   /* ERROR_BUSY */
    case 0x80070005UL:   /* E_ACCESSDENIED */
        snprintf(out, sz, "another application has it open.");
        return;
    case 0xC00D36B3UL:   /* MF_E_INVALIDMEDIATYPE */
    case 0xC00D5212UL:   /* MF_E_TOPO_CODEC_NOT_FOUND */
        snprintf(out, sz,
                 "the camera offers no video format this can record.");
        return;
    case 0x80070002UL:   /* ERROR_FILE_NOT_FOUND -- unplugged mid-use */
        snprintf(out, sz, "the camera is no longer connected.");
        return;
    default:
        snprintf(out, sz, "it would not open (error 0x%08lX).",
                 (unsigned long)g_cam_last_hr);
        return;
    }
}

bool plat_camera_in_use(int index) {
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;

    IMFAttributes* attr = NULL;
    bool busy = false;
    if (SUCCEEDED(MFCreateAttributes(&attr, 1))) {
        IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_,
                              &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID_);
        IMFActivate** devs = NULL;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attr, &devs, &count)) && devs) {
            if (index >= 0 && (UINT32)index < count) {
                IMFMediaSource* src = NULL;
                HRESULT hr = IMFActivate_ActivateObject(
                    devs[index], &IID_IMFMediaSource_, (void**)&src);
                if (SUCCEEDED(hr) && src) {
                    IMFMediaSource_Shutdown(src);
                    IMFMediaSource_Release(src);
                } else {
                    /* ONLY the sharing-violation family counts as in-use.
                     * Anything else is a different fault, and calling it
                     * "another app has it" would be a guess dressed up as a
                     * diagnosis. */
                    busy = (hr == (HRESULT)0x80070020L)   /* SHARING_VIOLATION */
                        || (hr == E_ACCESSDENIED)
                        || (hr == (HRESULT)0x800700AAL);  /* ERROR_BUSY      */
                }
            }
            for (UINT32 i = 0; i < count; i++) IMFActivate_Release(devs[i]);
            CoTaskMemFree(devs);
        }
        IMFAttributes_Release(attr);
    }
    MFShutdown();
    return busy;
}

bool plat_camera_open(int index, int* out_w, int* out_h) {
    if (CAM.open) return false;
    g_cam_last_hr = S_OK;
    memset(&CAM, 0, sizeof CAM);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;

    IMFAttributes* attr = NULL;
    if (FAILED(MFCreateAttributes(&attr, 1))) { MFShutdown(); return false; }
    IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_,
                          &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID_);

    IMFActivate** devs = NULL; UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attr, &devs, &count);
    IMFAttributes_Release(attr);
    if (FAILED(hr) || !devs || count == 0) {
        g_cam_last_hr = FAILED(hr) ? hr : E_FAIL;
        MFShutdown(); return false;
    }
    if (index < 0 || (UINT32)index >= count) index = 0;

    IMFMediaSource* src = NULL;
    hr = IMFActivate_ActivateObject(devs[index], &IID_IMFMediaSource_, (void**)&src);
    for (UINT32 i = 0; i < count; i++) IMFActivate_Release(devs[i]);
    CoTaskMemFree(devs);
    if (FAILED(hr) || !src) {
        g_cam_last_hr = FAILED(hr) ? hr : E_FAIL;
        MFShutdown();
        return false;
    }

    /* Enable video processing, or the reader will not convert formats and a
     * request for RGB32 fails outright on any camera that speaks YUY2 or
     * MJPG -- which is most of them. */
    IMFAttributes* rattr = NULL;
    if (SUCCEEDED(MFCreateAttributes(&rattr, 1)))
        IMFAttributes_SetUINT32(rattr,
            &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING_, TRUE);

    hr = MFCreateSourceReaderFromMediaSource(src, rattr, &CAM.reader);
    if (rattr) IMFAttributes_Release(rattr);
    IMFMediaSource_Release(src);
    if (FAILED(hr) || !CAM.reader) {
        g_cam_last_hr = FAILED(hr) ? hr : E_FAIL;
        MFShutdown(); return false;
    }

    /* Choose a RESOLUTION as well as a format.
     *
     * Asking only for a subtype lets Media Foundation pick the frame size,
     * and it picks the camera's default -- which is its smallest. A LifeCam
     * HD-3000 that does 1280x720 was recording 640x480 at 6.4 Mbit: a lavish
     * bitrate spent on far too few pixels, which is what "the video is really
     * bad" actually was.
     *
     * So enumerate what the camera natively offers and take the LARGEST it
     * has, rather than aiming at a number we picked. Detecting beats
     * hardcoding: a 720p target would short-change a 1080p camera and fail
     * outright on one whose modes are all smaller.
     *
     * The 1920x1080 ceiling is a cost guard, not a preference -- every frame
     * is converted to RGB32 and copied on the CPU, and a 4K webcam would
     * spend far more per frame than the picture is worth here. */
    /* Choose a RESOLUTION *and* a FRAME RATE, and reject what the bus cannot
     * carry.
     *
     * Asking only for a subtype lets Media Foundation pick both, and it picks
     * the camera's default -- the smallest. Asking only for a size is not
     * enough either: a LifeCam HD-3000 offers 1280x720 at 30 fps, but
     * uncompressed YUY2 at that rate is 442 Mbit/s and a USB 2.0 segment
     * carries about 320, so the request fails and the driver falls back to
     * VGA. The camera was never the limit; the cable was.
     *
     * 1280x720 at 15 fps is 221 Mbit/s. It fits, and it is four times the
     * detail of 640x480. So: take the largest frame size that has ANY usable
     * rate, then the highest rate at that size which fits the budget.
     *
     * Compressed formats (MJPG, H.264) are exempt -- the camera has already
     * shrunk them and the uncompressed arithmetic does not apply. */
    UINT32 best_w = 0, best_h = 0, best_num = 0, best_den = 0;
    int    best_index = -1;
    memset(&cam_pick, 0, sizeof cam_pick);
    {
        const unsigned long long BUDGET_BPS = 300000000ULL;  /* USB 2.0, usable */

        for (DWORD ti = 0; ti < 128; ti++) {
            IMFMediaType* nat = NULL;
            if (FAILED(IMFSourceReader_GetNativeMediaType(
                    CAM.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, ti, &nat))
                || !nat) break;

            UINT32 nw = 0, nh = 0, rn = 0, rd = 0;
            GUID sub = {0};
            IMFMediaType_GetGUID(nat, &MF_MT_SUBTYPE, &sub);
            bool have_size = SUCCEEDED(mf_get_pair((IMFAttributes*)nat,
                                                   &MF_MT_FRAME_SIZE, &nw, &nh));
            bool have_rate = SUCCEEDED(mf_get_pair((IMFAttributes*)nat,
                                                   &MF_MT_FRAME_RATE, &rn, &rd));
            IMFMediaType_Release(nat);
            if (!have_size || !nw || !nh) continue;
            if (nw > 1920 || nh > 1080) continue;      /* CPU-cost guard */
            if (!have_rate || !rd) { rn = 30; rd = 1; }

            /* Bytes per pixel for the uncompressed families we might get.
             * Anything else is assumed compressed and cheap. */
            unsigned bpp = 0;
            if (sub.Data1 == 0x32595559UL) bpp = 2;        /* YUY2 */
            else if (sub.Data1 == 0x3231564EUL) bpp = 3;   /* NV12 -- 12bpp, x2 */
            else if (sub.Data1 == 0x00000016UL) bpp = 4;   /* RGB32 */

            if (bpp) {
                unsigned long long bits = (unsigned long long)nw * nh;
                bits = bits * (bpp == 3 ? 3 : bpp * 2) / 2;   /* NV12 is 1.5 */
                bits = bits * 8ULL * rn / (rd ? rd : 1);
                if (bits > BUDGET_BPS) continue;           /* will not fit */
            }

            unsigned long long area = (unsigned long long)nw * nh;
            unsigned long long best_area = (unsigned long long)best_w * best_h;
            double rate      = (double)rn / (rd ? rd : 1);
            double best_rate = best_den ? (double)best_num / best_den : 0.0;

            /* Bigger picture wins; at equal size, the faster affordable rate.
             * Anything past 30 fps is spent on smoothness we do not need. */
            if (area > best_area ||
                (area == best_area && rate > best_rate && rate <= 30.0)) {
                best_w = nw; best_h = nh;
                best_index = (int)ti;
                if (rate <= 30.0) { best_num = rn; best_den = rd; }
            }
        }
    }

    /* Configure the CAMERA first, then ask for a convenient output format.
     *
     * Asking for "RGB32 at 1280x720 at 15fps" in one request makes Media
     * Foundation satisfy all three at once, and it declines -- then the
     * fallback path asks for RGB32 alone and gets the camera's default, which
     * is VGA. That is why the resolution never moved.
     *
     * The documented order works: set the camera's own NATIVE type (which
     * fixes size and rate on the device), then set the output subtype WITHOUT
     * a size, so the converter keeps the size already negotiated. Two steps,
     * each of which the driver can actually satisfy. */
    hr = E_FAIL;
    if (best_index >= 0) {
        IMFMediaType* nat = NULL;
        if (SUCCEEDED(IMFSourceReader_GetNativeMediaType(
                CAM.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                (DWORD)best_index, &nat)) && nat) {
            hr = IMFSourceReader_SetCurrentMediaType(
                    CAM.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, nat);
            IMFMediaType_Release(nat);
        }
    }
    cam_pick.set_hr = hr;

    /* Now the output format, size deliberately unspecified. */
    static const GUID* WANTED[] = { &MFVideoFormat_RGB32, &MFVideoFormat_NV12 };
    HRESULT ohr = E_FAIL;
    for (int t = 0; t < 2 && FAILED(ohr); t++) {
        IMFMediaType* want = NULL;
        if (FAILED(MFCreateMediaType(&want)) || !want) continue;
        IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, WANTED[t]);
        ohr = IMFSourceReader_SetCurrentMediaType(
                CAM.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, want);
        IMFMediaType_Release(want);
    }
    cam_pick.pass = SUCCEEDED(ohr) ? 1 : 0;

    /* If the output conversion refused, the native type is still set and the
     * camera still works -- we simply read whatever it gives. Only a total
     * failure of both is fatal. */
    if (FAILED(hr) && FAILED(ohr)) {
        g_cam_last_hr = hr;
        IMFSourceReader_Release(CAM.reader); CAM.reader = NULL;
        MFShutdown(); return false;
    }

    IMFMediaType* cur = NULL;
    UINT32 cw = 0, ch = 0;
    CAM.stride = 0;
    if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(
            CAM.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
        mf_get_pair((IMFAttributes*)cur, &MF_MT_FRAME_SIZE, &cw, &ch);
        /* Ask which way up the rows come, rather than assuming.
         *
         * A negative default stride means bottom-up. The old code hardcoded
         * bottom-up, which was right while the raw camera type came straight
         * through -- and became wrong the moment video processing was enabled,
         * because the converter hands back top-down. Result: the picture came
         * out upside down. Reading the sign covers both. */
        UINT32 st = 0;
        if (SUCCEEDED(IMFAttributes_GetUINT32((IMFAttributes*)cur,
                                              &MF_MT_DEFAULT_STRIDE, &st)))
            CAM.stride = (LONG)(INT32)st;
        IMFMediaType_Release(cur);
    }
    if (cw == 0 || ch == 0) { cw = 640; ch = 480; }

    CAM.w = (int)cw & ~1;
    CAM.h = (int)ch & ~1;
    CAM.open = true;
    if (out_w) *out_w = CAM.w;
    if (out_h) *out_h = CAM.h;
    return true;
}

/* One frame as RGBA at capW x capH, malloc'd. NULL if nothing is ready. */
/* Diagnostics for the read path, so a silent NULL can be explained. */
static struct { HRESULT hr; DWORD flags; DWORD curlen; size_t need;
                int calls, samples, sized; GUID sub; } cam_diag;
void plat_camera_pick(char* out, size_t sz) {
    snprintf(out, sz, "chose %ux%u @%.1ffps  setHR=0x%08lX pass=%d",
             cam_pick.w, cam_pick.h,
             cam_pick.den ? (double)cam_pick.num/cam_pick.den : 0.0,
             (unsigned long)cam_pick.set_hr, cam_pick.pass);
}

void plat_camera_diag(char* out, size_t sz) {
    snprintf(out, sz,
             "calls=%d samples=%d sized_ok=%d lastHR=0x%08lX flags=0x%lX "
             "curlen=%lu need=%llu sub=%08lX",
             cam_diag.calls, cam_diag.samples, cam_diag.sized,
             (unsigned long)cam_diag.hr, (unsigned long)cam_diag.flags,
             (unsigned long)cam_diag.curlen,
             (unsigned long long)cam_diag.need,
             (unsigned long)cam_diag.sub.Data1);
}

uint8_t* plat_camera_read(int capW, int capH) {
    cam_diag.calls++;
    if (!CAM.open || !CAM.reader) return NULL;

    DWORD stream = 0, flags = 0;
    LONGLONG ts = 0;
    IMFSample* smp = NULL;
    HRESULT rhr = IMFSourceReader_ReadSample(
            CAM.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
            &stream, &flags, &ts, &smp);
    cam_diag.hr = rhr; cam_diag.flags = flags;
    if (FAILED(rhr)) return NULL;
    if (!smp) return NULL;                       /* no frame this instant */
    cam_diag.samples++;

    IMFMediaBuffer* buf = NULL;
    uint8_t* out = NULL;
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &buf)) && buf) {
        BYTE* data = NULL; DWORD maxlen = 0, curlen = 0;
        if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &data, &maxlen, &curlen))) {
            size_t need = (size_t)CAM.w * CAM.h * 4;
            cam_diag.curlen = curlen; cam_diag.need = need;
            if (curlen >= need) {
                cam_diag.sized++;
                out = (uint8_t*)malloc((size_t)capW * capH * 4);
                if (out) {
                    /* BGRA. Row order follows the stride sign reported by
                     * the media type -- negative is bottom-up. Assuming
                     * either way is how the image ended up inverted. */
                    LONG pitch = CAM.stride ? CAM.stride : (LONG)(CAM.w * 4);
                    bool bottom_up = (pitch < 0);
                    size_t abs_pitch = (size_t)(pitch < 0 ? -pitch : pitch);
                    for (int y = 0; y < capH; y++) {
                        int sy = y * CAM.h / capH;
                        if (bottom_up) sy = CAM.h - 1 - sy;
                        if (sy < 0) sy = 0;
                        if (sy >= CAM.h) sy = CAM.h - 1;
                        const BYTE* srow = data + (size_t)sy * abs_pitch;
                        uint8_t* drow = out + (size_t)y * capW * 4;
                        for (int x = 0; x < capW; x++) {
                            int sx = x * CAM.w / capW;
                            drow[x*4 + 0] = srow[sx*4 + 2];   /* R */
                            drow[x*4 + 1] = srow[sx*4 + 1];   /* G */
                            drow[x*4 + 2] = srow[sx*4 + 0];   /* B */
                            drow[x*4 + 3] = 255;
                        }
                    }
                }
            }
            IMFMediaBuffer_Unlock(buf);
        }
        IMFMediaBuffer_Release(buf);
    }
    IMFSample_Release(smp);
    return out;
}

void plat_camera_close(void) {
    if (!CAM.open) return;
    if (CAM.reader) { IMFSourceReader_Release(CAM.reader); CAM.reader = NULL; }
    CAM.open = false;
    MFShutdown();
}
