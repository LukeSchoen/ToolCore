#include "audio.h"
#include "common.h"
#include <objbase.h>

#define CPCALL __stdcall

static void *g_enumerator;
static void *g_volume;

// The object's first field is its vtable pointer.
#define VTBL(self) (*(void ***)(self))

static const GUID CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID IID_IMMDevice =
    { 0xD666063F, 0x1587, 0x4E43, { 0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F } };
static const GUID IID_IAudioEndpointVolume =
    { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };

// IMMDeviceEnumerator: QueryInterface AddRef Release EnumAudioEndpoints
//                     GetDefaultAudioEndpoint ...
static long CPCALL en_get_default(void *self, int flow, int role, void **out)
{
    typedef long (CPCALL *fn)(void *, int, int, void **);
    return ((fn)VTBL(self)[4])(self, flow, role, out);
}

// IMMDevice: QueryInterface AddRef Release Activate ...
static long CPCALL dev_activate(void *self, const GUID *iid, DWORD clsctx,
                                void *params, void **out)
{
    typedef long (CPCALL *fn)(void *, const GUID *, DWORD, void *, void **);
    return ((fn)VTBL(self)[3])(self, iid, clsctx, params, out);
}

static unsigned long CPCALL release(void *self)
{
    typedef unsigned long (CPCALL *fn)(void *);
    return ((fn)VTBL(self)[2])(self);
}

// IAudioEndpointVolume slot 7: SetMasterVolumeLevelScalar
static long CPCALL vol_set_scalar(void *self, float level, const GUID *ctx)
{
    typedef long (CPCALL *fn)(void *, float, const GUID *);
    return ((fn)VTBL(self)[7])(self, level, ctx);
}

// slot 9: GetMasterVolumeLevelScalar
static long CPCALL vol_get_scalar(void *self, float *level)
{
    typedef long (CPCALL *fn)(void *, float *);
    return ((fn)VTBL(self)[9])(self, level);
}

// slot 14: SetMute
static long CPCALL vol_set_mute(void *self, int mute, const GUID *ctx)
{
    typedef long (CPCALL *fn)(void *, int, const GUID *);
    return ((fn)VTBL(self)[14])(self, mute, ctx);
}

// slot 15: GetMute
static long CPCALL vol_get_mute(void *self, int *mute)
{
    typedef long (CPCALL *fn)(void *, int *);
    return ((fn)VTBL(self)[15])(self, mute);
}

static void *endpoint_volume(void)
{
    void *device = NULL;
    void *volume = NULL;

    if (g_volume) return g_volume;

    if (!g_enumerator) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);   // already-initialised is fine
        if (CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, 0x17 /*CLSCTX_ALL*/,
                             &IID_IMMDeviceEnumerator, &g_enumerator) < 0) {
            g_enumerator = NULL;
            return NULL;
        }
    }

    // 0 = eRender, 1 = eMultimedia
    if (en_get_default(g_enumerator, 0, 1, &device) < 0 || !device) return NULL;
    if (dev_activate(device, &IID_IAudioEndpointVolume, 0x17, NULL, &volume) < 0)
        volume = NULL;
    release(device);
    g_volume = volume;
    return g_volume;
}

int audio_set_volume(int percent)
{
    void *vol = endpoint_volume();

    if (!vol) return 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return vol_set_scalar(vol, (float)percent / 100.0f, NULL) >= 0;
}

int audio_get_volume(int *percent)
{
    void *vol = endpoint_volume();
    float level = 0.0f;

    if (!vol) return 0;
    if (vol_get_scalar(vol, &level) < 0) return 0;
    *percent = (int)(level * 100.0f + 0.5f);
    return 1;
}

int audio_set_mute(int on)
{
    void *vol = endpoint_volume();

    if (!vol) return 0;
    return vol_set_mute(vol, on ? 1 : 0, NULL) >= 0;
}

int audio_get_mute(int *on)
{
    void *vol = endpoint_volume();
    int mute = 0;

    if (!vol) return 0;
    if (vol_get_mute(vol, &mute) < 0) return 0;
    *on = mute;
    return 1;
}
