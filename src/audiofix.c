// Part 2: make a multi-channel interface look like the 1-channel Real Tone Cable.
// Targets the CoreAudio device named "Rocksmith USB Guitar Adapter" (an Aggregate
// Device wrapping your interface). The game sees 1 input channel; we feed it
// channel RS_INPUT_CHANNEL (default 1) of the real device.
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

extern void rsfix_log(const char *fmt, ...);
#define TARGET_NAME "Rocksmith USB Guitar Adapter"
#define MAXLOG 200
static int nlog;
#define LOGQ(...) do { if (nlog < MAXLOG) { nlog++; rsfix_log(__VA_ARGS__); } } while (0)

static AudioObjectID g_dev = kAudioObjectUnknown;
static AudioObjectID g_streams[8]; static UInt32 g_nstreams;
static UInt32 g_realch;      // real input channels
static int g_srcch = 0;      // 0-based channel to feed the game
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void find_target(void) {
    const char *e = getenv("RS_INPUT_CHANNEL");
    if (e && atoi(e) >= 1) g_srcch = atoi(e) - 1;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz)) return;
    AudioObjectID *d = malloc(sz);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, d);
    for (UInt32 i = 0; i < sz / sizeof *d; i++) {
        CFStringRef n = NULL; UInt32 s = sizeof n; char buf[256] = "";
        AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(d[i], &na, 0, NULL, &s, &n) == 0 && n) {
            CFStringGetCString(n, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(n);
        }
        if (strcmp(buf, TARGET_NAME) == 0) { g_dev = d[i]; break; }
    }
    free(d);
    if (g_dev == kAudioObjectUnknown) { rsfix_log("audio: no device named '%s' (audio fix inactive)\n", TARGET_NAME); return; }
    AudioObjectPropertyAddress sa = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    sz = sizeof g_streams;
    if (AudioObjectGetPropertyData(g_dev, &sa, 0, NULL, &sz, g_streams) == 0) g_nstreams = sz / sizeof(AudioObjectID);
    AudioObjectPropertyAddress ca = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyDataSize(g_dev, &ca, 0, NULL, &sz) == 0) {
        AudioBufferList *b = malloc(sz);
        if (AudioObjectGetPropertyData(g_dev, &ca, 0, NULL, &sz, b) == 0)
            for (UInt32 k = 0; k < b->mNumberBuffers; k++) g_realch += b->mBuffers[k].mNumberChannels;
        free(b);
    }
    if (g_srcch >= (int)g_realch) g_srcch = 0;
    rsfix_log("audio: target device %u, %u input channels in %u stream(s); feeding game channel %d\n",
              g_dev, g_realch, g_nstreams, g_srcch + 1);
}
static void init_once(void) { pthread_once(&g_once, find_target); }
static int active(void) { init_once(); return g_dev != kAudioObjectUnknown && g_realch > 1; }

static int is_input_stream(AudioObjectID o) {
    for (UInt32 i = 0; i < g_nstreams; i++) if (g_streams[i] == o) return 1;
    return 0;
}
static int is_fmt_sel(AudioObjectPropertySelector s) {
    return s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat ||
           s == kAudioDevicePropertyStreamFormat;
}
static int is_fmtlist_sel(AudioObjectPropertySelector s) {
    return s == kAudioStreamPropertyAvailableVirtualFormats || s == kAudioStreamPropertyAvailablePhysicalFormats ||
           s == kAudioDevicePropertyStreamFormats;
}
// Is this query about the target's input side?
static int targets_input(AudioObjectID o, const AudioObjectPropertyAddress *a) {
    if (!active()) return 0;
    if (o == g_dev && a->mScope == kAudioObjectPropertyScopeInput) return 1;
    if (is_input_stream(o)) return 1;
    return 0;
}
static void mono_asbd(AudioStreamBasicDescription *f) {
    if (f->mChannelsPerFrame > 1 && !(f->mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
        UInt32 bpc = f->mBytesPerFrame / f->mChannelsPerFrame;
        f->mBytesPerFrame = bpc; f->mBytesPerPacket = bpc * f->mFramesPerPacket;
    }
    f->mChannelsPerFrame = 1;
}
static const char *fourcc(UInt32 v, char *b) { b[0]=v>>24; b[1]=v>>16; b[2]=v>>8; b[3]=v; b[4]=0; return b; }

// ---- property interposers ----
static OSStatus my_GetPropertyDataSize(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 *sz) {
    OSStatus r = AudioObjectGetPropertyDataSize(o, a, q, qd, sz);
    if (r == 0 && a && targets_input(o, a)) {
        if (a->mSelector == kAudioDevicePropertyStreamConfiguration && o == g_dev) *sz = sizeof(AudioBufferList);
    }
    return r;
}
static OSStatus my_GetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 *sz, void *data) {
    if (a && targets_input(o, a) && a->mSelector == kAudioDevicePropertyStreamConfiguration && o == g_dev) {
        if (!data || *sz < sizeof(AudioBufferList)) return kAudioHardwareBadPropertySizeError;
        AudioBufferList *b = data; b->mNumberBuffers = 1; b->mBuffers[0].mNumberChannels = 1;
        b->mBuffers[0].mDataByteSize = 0; b->mBuffers[0].mData = NULL; *sz = sizeof(AudioBufferList);
        LOGQ("audio: reported 1-channel stream configuration\n");
        return 0;
    }
    OSStatus r = AudioObjectGetPropertyData(o, a, q, qd, sz, data);
    if (r == 0 && a && data && targets_input(o, a)) {
        char c[5]; fourcc(a->mSelector, c);
        if (is_fmt_sel(a->mSelector) && *sz >= sizeof(AudioStreamBasicDescription)) {
            mono_asbd(data); LOGQ("audio: get '%s' on %u -> mono\n", c, o);
        } else if (is_fmtlist_sel(a->mSelector)) {
            if (a->mSelector == kAudioStreamPropertyAvailableVirtualFormats || a->mSelector == kAudioStreamPropertyAvailablePhysicalFormats) {
                AudioStreamRangedDescription *l = data;
                for (UInt32 i = 0; i < *sz / sizeof *l; i++) mono_asbd(&l->mFormat + 0), l++;
            } else {
                AudioStreamBasicDescription *l = data;
                for (UInt32 i = 0; i < *sz / sizeof *l; i++) mono_asbd(&l[i]);
            }
            LOGQ("audio: get list '%s' on %u -> mono\n", c, o);
        } else {
            (void)c;
        }
    }
    return r;
}
static OSStatus my_SetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 sz, const void *data) {
    if (a && data && targets_input(o, a) && is_fmt_sel(a->mSelector) && sz >= sizeof(AudioStreamBasicDescription)) {
        const AudioStreamBasicDescription *f = data;
        rsfix_log("audio: game sets format %.0f Hz, %u ch, %u bits -> applying sample rate only\n",
                  f->mSampleRate, f->mChannelsPerFrame, f->mBitsPerChannel);
        Float64 sr = f->mSampleRate;
        AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (sr > 0) AudioObjectSetPropertyData(g_dev, &ra, 0, NULL, sizeof sr, &sr);
        return 0;
    }
    return AudioObjectSetPropertyData(o, a, q, qd, sz, data);
}

// ---- IOProc wrapper: hand the game one channel ----
typedef struct { AudioDeviceIOProc proc; void *client; float *buf; UInt32 cap; int logged; AudioDeviceIOProcID id; } Wrap;
static Wrap *g_wraps[16]; static int g_nwraps;

static OSStatus wrap_proc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                          AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    Wrap *w = ctx;
    if (!w->logged) { w->logged = 1; LOGQ("audio: input is flowing (%u buffer(s))\n", in ? in->mNumberBuffers : 0); }
    if (!in || in->mNumberBuffers == 0) return w->proc(dev, now, in, inTime, out, outTime, w->client);
    // locate source channel across (possibly several) interleaved float32 buffers
    UInt32 base = 0; const AudioBuffer *src = NULL; UInt32 off = 0;
    for (UInt32 k = 0; k < in->mNumberBuffers; k++) {
        UInt32 n = in->mBuffers[k].mNumberChannels;
        if ((UInt32)g_srcch < base + n) { src = &in->mBuffers[k]; off = g_srcch - base; break; }
        base += n;
    }
    if (!src || src->mNumberChannels == 0) return w->proc(dev, now, in, inTime, out, outTime, w->client);
    UInt32 frames = src->mDataByteSize / (sizeof(float) * src->mNumberChannels);
    if (frames > w->cap) frames = w->cap;
    const float *s = src->mData;
    for (UInt32 i = 0; i < frames; i++) w->buf[i] = s[i * src->mNumberChannels + off];
    AudioBufferList mono; mono.mNumberBuffers = 1;
    mono.mBuffers[0].mNumberChannels = 1; mono.mBuffers[0].mDataByteSize = frames * sizeof(float); mono.mBuffers[0].mData = w->buf;
    return w->proc(dev, now, &mono, inTime, out, outTime, w->client);
}
static OSStatus my_CreateIOProcID(AudioObjectID dev, AudioDeviceIOProc proc, void *client, AudioDeviceIOProcID *id) {
    if (active() && dev == g_dev && proc) {
        Wrap *w = calloc(1, sizeof *w);
        w->proc = proc; w->client = client; w->cap = 16384; w->buf = calloc(w->cap, sizeof(float));
        OSStatus r = AudioDeviceCreateIOProcID(dev, wrap_proc, w, &w->id);
        if (id) *id = w->id;
        if (r == 0 && g_nwraps < 16) g_wraps[g_nwraps++] = w;
        rsfix_log("audio: game opened input IOProc on target (game proc %p -> id %p) = %d; wrapping to 1 channel\n", proc, w->id, (int)r);
        return r;
    }
    return AudioDeviceCreateIOProcID(dev, proc, client, id);
}

#define INTERPOSE(n, o) __attribute__((used)) static struct { const void *r, *orig; } _ip_##o \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)n, (const void *)o };
INTERPOSE(my_GetPropertyDataSize, AudioObjectGetPropertyDataSize)
INTERPOSE(my_GetPropertyData, AudioObjectGetPropertyData)
INTERPOSE(my_SetPropertyData, AudioObjectSetPropertyData)
INTERPOSE(my_CreateIOProcID, AudioDeviceCreateIOProcID)

// The game passes its own IOProc function pointer to Start/Stop (legacy style).
// Map that to the ID of our wrapper on the target device.
static AudioDeviceIOProcID map_id(AudioObjectID dev, AudioDeviceIOProcID p) {
    if (dev != g_dev || !p) return p;
    for (int i = 0; i < g_nwraps; i++)
        if (g_wraps[i] && (void *)g_wraps[i]->proc == (void *)p) return g_wraps[i]->id;
    return p;
}
static OSStatus my_Start(AudioObjectID d, AudioDeviceIOProcID p) {
    AudioDeviceIOProcID m = map_id(d, p); OSStatus r = AudioDeviceStart(d, m);
    LOGQ("audio: AudioDeviceStart(%u,%p->%p) = %d\n", d, p, m, (int)r); return r; }
static OSStatus my_Stop(AudioObjectID d, AudioDeviceIOProcID p) {
    AudioDeviceIOProcID m = map_id(d, p); OSStatus r = AudioDeviceStop(d, m);
    LOGQ("audio: AudioDeviceStop(%u,%p->%p) = %d\n", d, p, m, (int)r); return r; }
static OSStatus my_Destroy(AudioObjectID d, AudioDeviceIOProcID p) {
    AudioDeviceIOProcID m = map_id(d, p); OSStatus r = AudioDeviceDestroyIOProcID(d, m);
    if (r == 0 && d == g_dev) for (int i = 0; i < g_nwraps; i++)
        if (g_wraps[i] && g_wraps[i]->id == m) g_wraps[i] = NULL;   // leak the small Wrap: callbacks may still be draining
    LOGQ("audio: DestroyIOProcID(%u,%p->%p) = %d\n", d, p, m, (int)r); return r; }
INTERPOSE(my_Start, AudioDeviceStart)
INTERPOSE(my_Stop, AudioDeviceStop)
INTERPOSE(my_Destroy, AudioDeviceDestroyIOProcID)
