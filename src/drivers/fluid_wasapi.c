/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 * Copyright (C) 2021  Chris Xiong
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#include "fluid_synth.h"
#include "fluid_adriver.h"
#include "fluid_settings.h"

#if WASAPI_SUPPORT

#define CINTERFACE
#define COBJMACROS

#include <objbase.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiosessiontypes.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <oaidl.h>
#include <ksguid.h>
#include <ksmedia.h>

// these symbols are either never found in headers, or
// only defined but there are no library containing the actual symbol...

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif

#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static const CLSID _CLSID_MMDeviceEnumerator =
{
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}
};

static const IID   _IID_IMMDeviceEnumerator =
{
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}
};
static const IID   _IID_IAudioClient =
{
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}
};
static const IID   _IID_IAudioRenderClient =
{
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}
};
/*
 * WASAPI Driver for fluidsynth
 *
 * Current limitations:
 *  - Only one stereo audio output.
 *  - If audio.sample-format is "16bits", a convertion from float
 *    without dithering is used.
 *
 * Available settings:
 *  - audio.wasapi.exclusive-mode
 *    0 = shared mode, 1 = exclusive mode
 *  - audio.wasapi.device
 *    Used for device selection. Leave unchanged (or use the value "default")
 *    to use the default Windows audio device for media playback.
 *
 * Notes:
 *  - If exclusive mode is selected, audio.period-size is used as the periodicity
 *    of the IAudioClient stream, which is the sole factor of audio latency.
 *    audio.periods still determines the buffer size, but has no direct impact on
 *    the latency (at least according to Microsoft). The valid range for
 *    audio.period-size may vary depending on the driver and sample rate.
 *  - The sample rate and sample format of fluidsynth must be supported by the
 *    audio device in exclusive mode. Otherwise driver creation will fail.
 *  - In shared mode, if the sample rate of the synth doesn't match what is
 *    configured in the 'advanced' tab of the audio device properties dialog,
 *    Windows will automatically resample the output (obviously). Windows
 *    Vista doesn't seem to support the resampling method with better quality.
 *  - Under Windows 10, this driver may report a latency of 0ms in shared mode.
 *    This is nonsensical and should be disregarded.
 *
 */

#define FLUID_WASAPI_MAX_OUTPUTS 1

typedef void(*fluid_wasapi_devenum_callback_t)(IMMDevice *, void *);

static DWORD WINAPI fluid_wasapi_audio_run(void *p);
static int fluid_wasapi_write_processed_channels(void *data, int len,
        int channels_count,
        void *channels_out[], int channels_off[],
        int channels_incr[]);
static void fluid_wasapi_foreach_device(fluid_wasapi_devenum_callback_t callback, void *data);
static void fluid_wasapi_register_callback(IMMDevice *dev, void *data);
static void fluid_wasapi_finddev_callback(IMMDevice *dev, void *data);
static IMMDevice *fluid_wasapi_find_device(IMMDeviceEnumerator *denum, const char *name);
static FLUID_INLINE int16_t round_clip_to_i16(float x);

typedef struct
{
    const char *name;
    wchar_t *id;
} fluid_wasapi_finddev_data_t;

typedef struct
{
    fluid_audio_driver_t driver;

    void *user_pointer;
    fluid_audio_func_t func;
    fluid_audio_channels_callback_t write;
    float **drybuf;

    UINT32 nframes;
    double buffer_duration;
    int channels_count;
    int float_samples;

    HANDLE thread;
    DWORD thread_id;
    HANDLE quit_ev;

    IAudioClient *aucl;
    IAudioRenderClient *arcl;

} fluid_wasapi_audio_driver_t;

fluid_audio_driver_t *
new_fluid_wasapi_audio_driver(fluid_settings_t *settings,
                              fluid_synth_t *synth,
                              int flags)
{
    // FIXME: remove explicit cast to fluid_audio_func_t
    return new_fluid_wasapi_audio_driver2(
            settings,
            (fluid_audio_func_t)fluid_synth_process, synth,
            flags);
}

fluid_audio_driver_t *
new_fluid_wasapi_audio_driver2(fluid_settings_t *settings,
                               fluid_audio_func_t func, void *data,
                               int flags)
{
    fluid_wasapi_audio_driver_t *dev = NULL;
    double sample_rate;
    int periods, period_size;
    fluid_long_long_t buffer_duration_reftime;
    fluid_long_long_t periods_reftime;
    fluid_long_long_t latency_reftime;
    int audio_channels;
    int sample_size;
    int i;
    char *dname = NULL;
    DWORD flags = 0;
    //GUID guid_float = {DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_IEEE_FLOAT)};
    WAVEFORMATEXTENSIBLE wfx;
    WAVEFORMATEXTENSIBLE *rwfx = NULL;
    AUDCLNT_SHAREMODE share_mode;
    IMMDeviceEnumerator *denum = NULL;
    IMMDevice *mmdev = NULL;
    HRESULT ret;
    OSVERSIONINFOEXW vi = {sizeof(vi), 6, 0, 0, 0, {0}, 0, 0, 0, 0, 0};
    int err_level = (flags & FLUID_AUDRIVER_PROBE) ? FLUID_DBG : FLUID_ERR;

    if(!VerifyVersionInfoW(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR,
                           VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0,
                                   VER_MAJORVERSION, VER_GREATER_EQUAL),
                                   VER_MINORVERSION, VER_GREATER_EQUAL),
                                   VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL)))
    {
        FLUID_LOG(err_level, "wasapi: this driver requires Windows Vista or newer.");
        return NULL;
    }

    /* Retrieve the settings */
    fluid_settings_getnum(settings, "synth.sample-rate", &sample_rate);
    fluid_settings_getint(settings, "audio.periods", &periods);
    fluid_settings_getint(settings, "audio.period-size", &period_size);
    fluid_settings_getint(settings, "synth.audio-channels", &audio_channels);

    if(audio_channels > FLUID_WASAPI_MAX_OUTPUTS)
    {
        FLUID_LOG(err_level, "wasapi: channel configuration with more than one stereo pair is not supported.");
        return NULL;
    }

    /* Clear format structure */
    ZeroMemory(&wfx, sizeof(WAVEFORMATEXTENSIBLE));

    if(fluid_settings_str_equal(settings, "audio.sample-format", "16bits"))
    {
        sample_size = sizeof(int16_t);
        wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
    }
    else
    {
        sample_size = sizeof(float);
        wfx.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    }

    wfx.Format.nChannels  = 2;
    wfx.Format.wBitsPerSample  = sample_size * 8;
    wfx.Format.nBlockAlign     = sample_size * wfx.Format.nChannels;
    wfx.Format.nSamplesPerSec  = (DWORD)sample_rate;
    wfx.Format.nAvgBytesPerSec = (DWORD)sample_rate * wfx.Format.nBlockAlign;
    //wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    //wfx.Format.cbSize = 22;
    //wfx.SubFormat = guid_float;
    //wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    //wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;

    dev = FLUID_NEW(fluid_wasapi_audio_driver_t);

    if(dev == NULL)
    {
        FLUID_LOG(FLUID_ERR, "wasapi: out of memory.");
        return NULL;
    }

    FLUID_MEMSET(dev, 0, sizeof(fluid_wasapi_audio_driver_t));

    dev->func = func;
    dev->user_pointer = data;
    dev->buffer_duration = periods * period_size / sample_rate;
    dev->channels_count = audio_channels * 2;
    dev->float_samples = (wfx.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    buffer_duration_reftime = (fluid_long_long_t)(dev->buffer_duration * 1e7 + .5);
    periods_reftime = (fluid_long_long_t)(period_size / sample_rate * 1e7 + .5);

    ret = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if(FAILED(ret))
    {
        FLUID_LOG(err_level, "wasapi: cannot initialize COM. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    ret = CoCreateInstance(
              &_CLSID_MMDeviceEnumerator, NULL,
              CLSCTX_ALL, &_IID_IMMDeviceEnumerator,
              (void **)&denum);

    if(FAILED(ret))
    {
        FLUID_LOG(err_level, "wasapi: cannot create device enumerator. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    if(fluid_settings_dupstr(settings, "audio.wasapi.device", &dname) == FLUID_OK)
    {
        mmdev = fluid_wasapi_find_device(denum, dname);

        if(mmdev == NULL)
        {
            FLUID_FREE(dname);
            goto cleanup;
        }
    }
    else
    {
        FLUID_LOG(FLUID_ERR, "wasapi: out of memory.");
        goto cleanup;
    }

    FLUID_FREE(dname);

    ret = IMMDevice_Activate(mmdev,
                             &_IID_IAudioClient,
                             CLSCTX_ALL, NULL,
                             (void **)&dev->aucl);

    if(FAILED(ret))
    {
        FLUID_LOG(err_level, "wasapi: cannot activate audio client. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    share_mode = AUDCLNT_SHAREMODE_SHARED;

    if(fluid_settings_getint(settings, "audio.wasapi.exclusive-mode", &i) == FLUID_OK)
    {
        if(i)
        {
            share_mode = AUDCLNT_SHAREMODE_EXCLUSIVE;
        }
    }

    if(share_mode == AUDCLNT_SHAREMODE_SHARED)
    {
        FLUID_LOG(FLUID_DBG, "wasapi: using shared mode.");
        periods_reftime = 0;
    }
    else
    {
        FLUID_LOG(FLUID_DBG, "wasapi: using exclusive mode.");
    }

    ret = IAudioClient_IsFormatSupported(dev->aucl, share_mode, (const WAVEFORMATEX *)&wfx, (WAVEFORMATEX **)&rwfx);

    if(FAILED(ret))
    {
        FLUID_LOG(err_level, "wasapi: device doesn't support the mode we want. 0x%x", (unsigned)ret);
        goto cleanup;
    }
    else if(ret == S_FALSE)
    {
        //rwfx is non-null only in this case
        FLUID_LOG(FLUID_INFO, "wasapi: requested mode cannot be fully satisfied.");

        if(rwfx->Format.nSamplesPerSec != wfx.Format.nSamplesPerSec) // needs resampling
        {
            flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
            vi.dwMinorVersion = 1;

            if(VerifyVersionInfoW(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR,
                                  VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0,
                                          VER_MAJORVERSION, VER_GREATER_EQUAL),
                                          VER_MINORVERSION, VER_GREATER_EQUAL),
                                          VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL)))
                //IAudioClient::Initialize in Vista fails with E_INVALIDARG if this flag is set
            {
                flags |= AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            }
        }

        CoTaskMemFree(rwfx);
    }

    ret = IAudioClient_Initialize(dev->aucl, share_mode, flags,
                                  buffer_duration_reftime, periods_reftime, (WAVEFORMATEX *)&wfx, &GUID_NULL);

    if(FAILED(ret))
    {
        FLUID_LOG(err_level, "wasapi: failed to initialize audio client. 0x%x", (unsigned)ret);

        if(ret == AUDCLNT_E_INVALID_DEVICE_PERIOD)
        {
            fluid_long_long_t defp, minp;
            FLUID_LOG(err_level, "wasapi: the device period size is invalid.");

            if(SUCCEEDED(IAudioClient_GetDevicePeriod(dev->aucl, &defp, &minp)))
            {
                int defpf = (int)(defp / 1e7 * sample_rate);
                int minpf = (int)(minp / 1e7 * sample_rate);
                FLUID_LOG(err_level, "wasapi: minimum period is %d, default period is %d. selected %d.", minpf, defpf, period_size);
            }
        }

        goto cleanup;
    }

    ret = IAudioClient_GetBufferSize(dev->aucl, &dev->nframes);

    if(FAILED(ret))
    {
        FLUID_LOG(err_level, "wasapi: cannot get audio buffer size. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    FLUID_LOG(FLUID_DBG, "wasapi: requested %d frames of buffers, got %u.", periods * period_size, dev->nframes);
    dev->buffer_duration = dev->nframes / sample_rate;

    dev->drybuf = FLUID_ARRAY(float *, audio_channels * 2);

    if(dev->drybuf == NULL)
    {
        FLUID_LOG(FLUID_ERR, "wasapi: out of memory");
        goto cleanup;
    }

    FLUID_MEMSET(dev->drybuf, 0, sizeof(float *) * audio_channels * 2);

    for(i = 0; i < audio_channels * 2; ++i)
    {
        dev->drybuf[i] = FLUID_ARRAY(float, dev->nframes);

        if(dev->drybuf[i] == NULL)
        {
            FLUID_LOG(FLUID_ERR, "wasapi: out of memory");
            goto cleanup;
        }
    }

    ret = IAudioClient_GetService(dev->aucl, &_IID_IAudioRenderClient, (void **)&dev->arcl);

    if(FAILED(ret))
    {
        FLUID_LOG(err_level, "wasapi: cannot get audio render device. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    if(SUCCEEDED(IAudioClient_GetStreamLatency(dev->aucl, &latency_reftime)))
    {
        FLUID_LOG(FLUID_DBG, "wasapi: latency: %fms.", latency_reftime / 1e4);
    }

    dev->quit_ev = CreateEvent(NULL, FALSE, FALSE, NULL);

    if(dev->quit_ev == NULL)
    {
        FLUID_LOG(err_level, "wasapi: failed to create quit event.");
        goto cleanup;
    }

    dev->thread = CreateThread(NULL, 0, fluid_wasapi_audio_run, (void *)dev, 0, &dev->thread_id);

    if(dev->thread == NULL)
    {
        FLUID_LOG(err_level, "wasapi: failed to create audio thread.");
        goto cleanup;
    }

    IMMDevice_Release(mmdev);
    IMMDeviceEnumerator_Release(denum);
    return &dev->driver;

cleanup:

    if(mmdev != NULL)
    {
        IMMDevice_Release(mmdev);
    }

    if(denum != NULL)
    {
        IMMDeviceEnumerator_Release(denum);
    }

    delete_fluid_wasapi_audio_driver(&dev->driver);
    return NULL;
}

void delete_fluid_wasapi_audio_driver(fluid_audio_driver_t *p)
{
    fluid_wasapi_audio_driver_t *dev = (fluid_wasapi_audio_driver_t *) p;
    int i;

    fluid_return_if_fail(dev != NULL);

    if(dev->thread != NULL)
    {
        SetEvent(dev->quit_ev);

        if(WaitForSingleObject(dev->thread, 2000) != WAIT_OBJECT_0)
        {
            FLUID_LOG(FLUID_WARN, "wasapi: couldn't join the audio thread. killing it.");
            TerminateThread(dev->thread, 0);
        }

        CloseHandle(dev->thread);
    }

    if(dev->quit_ev != NULL)
    {
        CloseHandle(dev->quit_ev);
    }

    if(dev->aucl != NULL)
    {
        IAudioClient_Stop(dev->aucl);
        IAudioClient_Release(dev->aucl);
    }

    if(dev->arcl != NULL)
    {
        IAudioRenderClient_Release(dev->arcl);
    }

    CoUninitialize();

    if(dev->drybuf)
    {
        for(i = 0; i < dev->channels_count; ++i)
        {
            FLUID_FREE(dev->drybuf[i]);
        }
    }

    FLUID_FREE(dev->drybuf);

    FLUID_FREE(dev);
}

void fluid_wasapi_audio_driver_settings(fluid_settings_t *settings)
{
    fluid_settings_register_int(settings, "audio.wasapi.exclusive-mode", 0, 0, 1, FLUID_HINT_TOGGLED);
    fluid_settings_register_str(settings, "audio.wasapi.device", "default", 0);
    fluid_settings_add_option(settings, "audio.wasapi.device", "default");
    fluid_wasapi_foreach_device(fluid_wasapi_register_callback, settings);
}

static DWORD WINAPI fluid_wasapi_audio_run(void *p)
{
    fluid_wasapi_audio_driver_t *dev = (fluid_wasapi_audio_driver_t *)p;
    DWORD time_to_sleep = dev->buffer_duration * 1000 / 2;
    UINT32 pos;
    DWORD len;
    void *channels_out[2];
    int channels_off[2] = {0, 1};
    int channels_incr[2] = {2, 2};
    BYTE *pbuf;
    HRESULT ret;

    if(time_to_sleep < 1)
    {
        time_to_sleep = 1;
    }

    ret = IAudioClient_Start(dev->aucl);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: failed to start audio client. 0x%x", (unsigned)ret);
        return 0;
    }

    for(;;)
    {
        ret = IAudioClient_GetCurrentPadding(dev->aucl, &pos);

        if(FAILED(ret))
        {
            FLUID_LOG(FLUID_ERR, "wasapi: cannot get buffer padding. 0x%x", (unsigned)ret);
            return 0;
        }

        len = dev->nframes - pos;

        if(len == 0)
        {
            Sleep(0);
            continue;
        }

        ret = IAudioRenderClient_GetBuffer(dev->arcl, len, &pbuf);

        if(FAILED(ret))
        {
            FLUID_LOG(FLUID_ERR, "wasapi: cannot get buffer. 0x%x", (unsigned)ret);
            return 0;
        }

        channels_out[0] = channels_out[1] = (void *)pbuf;

        fluid_wasapi_write_processed_channels(dev, len, 2,
                                              channels_out, channels_off, channels_incr);

        ret = IAudioRenderClient_ReleaseBuffer(dev->arcl, len, 0);

        if(FAILED(ret))
        {
            FLUID_LOG(FLUID_ERR, "wasapi: failed to release buffer. 0x%x", (unsigned)ret);
            return 0;
        }

        if(WaitForSingleObject(dev->quit_ev, time_to_sleep) == WAIT_OBJECT_0)
        {
            break;
        }
    }

    return 0;
}

static int fluid_wasapi_write_processed_channels(void *data, int len,
        int channels_count,
        void *channels_out[], int channels_off[],
        int channels_incr[])
{
    int i, ch;
    int ret;
    fluid_wasapi_audio_driver_t *drv = (fluid_wasapi_audio_driver_t *) data;
    float *optr[FLUID_WASAPI_MAX_OUTPUTS * 2];
    int16_t *ioptr[FLUID_WASAPI_MAX_OUTPUTS * 2];

    for(ch = 0; ch < drv->channels_count; ++ch)
    {
        FLUID_MEMSET(drv->drybuf[ch], 0, len * sizeof(float));
        optr[ch] = (float *)channels_out[ch] + channels_off[ch];
        ioptr[ch] = (int16_t *)channels_out[ch] + channels_off[ch];
    }

    ret = drv->func(drv->user_pointer, len, 0, NULL, drv->channels_count, drv->drybuf);

    for(ch = 0; ch < drv->channels_count; ++ch)
    {
        for(i = 0; i < len; ++i)
        {
            if(drv->float_samples)
            {
                *optr[ch] = drv->drybuf[ch][i];
                optr[ch] += channels_incr[ch];
            }
            else //This code is taken from fluid_synth.c. No dithering yet.
            {
                *ioptr[ch] = round_clip_to_i16(drv->drybuf[ch][i] * 32766.0f);
                ioptr[ch] += channels_incr[ch];
            }
        }
    }

    return ret;
}

static void fluid_wasapi_foreach_device(fluid_wasapi_devenum_callback_t callback, void *data)
{
    IMMDeviceEnumerator *denum = NULL;
    IMMDeviceCollection *dcoll = NULL;
    UINT cnt, i;
    HRESULT ret;

    ret = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot initialize COM. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    ret = CoCreateInstance(
              &_CLSID_MMDeviceEnumerator, NULL,
              CLSCTX_ALL, &_IID_IMMDeviceEnumerator,
              (void **)&denum);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot create device enumerator. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    ret = IMMDeviceEnumerator_EnumAudioEndpoints(
              denum, eRender,
              DEVICE_STATE_ACTIVE, &dcoll);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot enumerate audio devices. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    ret = IMMDeviceCollection_GetCount(dcoll, &cnt);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot get device count. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    for(i = 0; i < cnt; ++i)
    {
        IMMDevice *dev = NULL;

        ret = IMMDeviceCollection_Item(dcoll, i, &dev);

        if(FAILED(ret))
        {
            FLUID_LOG(FLUID_ERR, "wasapi: cannot get device #%u. 0x%x", i, (unsigned)ret);
            continue;
        }

        callback(dev, data);

        IMMDevice_Release(dev);
    }

cleanup:

    if(dcoll != NULL)
    {
        IMMDeviceCollection_Release(dcoll);
    }

    if(denum != NULL)
    {
        IMMDeviceEnumerator_Release(denum);
    }

    CoUninitialize();
}

static void fluid_wasapi_register_callback(IMMDevice *dev, void *data)
{
    fluid_settings_t *settings = (fluid_settings_t *)data;
    IPropertyStore *prop = NULL;
    PROPVARIANT var;
    int ret;

    ret = IMMDevice_OpenPropertyStore(dev, STGM_READ, &prop);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot get properties of device. 0x%x", (unsigned)ret);
        return;
    }

    PropVariantInit(&var);

    ret = IPropertyStore_GetValue(prop, &PKEY_Device_FriendlyName, &var);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot get friendly name of device. 0x%x", (unsigned)ret);
    }
    else
    {
        int nsz;
        char *name;

        nsz = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, 0, 0, 0, 0);
        name = FLUID_ARRAY(char, nsz + 1);
        WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, name, nsz, 0, 0);
        fluid_settings_add_option(settings, "audio.wasapi.device", name);
        FLUID_FREE(name);
    }

    IPropertyStore_Release(prop);
    PropVariantClear(&var);
}

static void fluid_wasapi_finddev_callback(IMMDevice *dev, void *data)
{
    fluid_wasapi_finddev_data_t *d = (fluid_wasapi_finddev_data_t *)data;
    int nsz;
    char *name;
    wchar_t *id = NULL;
    IPropertyStore *prop = NULL;
    PROPVARIANT var;
    HRESULT ret;

    ret = IMMDevice_OpenPropertyStore(dev, STGM_READ, &prop);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot get properties of device. 0x%x", (unsigned)ret);
        return;
    }

    PropVariantInit(&var);

    ret = IPropertyStore_GetValue(prop, &PKEY_Device_FriendlyName, &var);

    if(FAILED(ret))
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot get friendly name of device. 0x%x", (unsigned)ret);
        goto cleanup;
    }

    nsz = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, 0, 0, 0, 0);
    name = FLUID_ARRAY(char, nsz + 1);
    WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, name, nsz, 0, 0);

    if(!FLUID_STRCASECMP(name, d->name))
    {
        ret = IMMDevice_GetId(dev, &id);

        if(FAILED(ret))
        {
            FLUID_LOG(FLUID_ERR, "wasapi: cannot get id of device. 0x%x", (unsigned)ret);
            goto cleanup;
        }

        nsz = wcslen(id);
        d->id = FLUID_ARRAY(wchar_t, nsz + 1);
        FLUID_MEMCPY(d->id, id, sizeof(wchar_t) * (nsz + 1));
    }

cleanup:
    PropVariantClear(&var);
    IPropertyStore_Release(prop);
    CoTaskMemFree(id);
    FLUID_FREE(name);
}

static IMMDevice *fluid_wasapi_find_device(IMMDeviceEnumerator *denum, const char *name)
{
    fluid_wasapi_finddev_data_t d;
    IMMDevice *dev;
    HRESULT ret;
    d.name = name;
    d.id = NULL;

    if(!FLUID_STRCASECMP(name, "default"))
    {
        ret = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
                  denum,
                  eRender,
                  eMultimedia,
                  &dev);

        if(FAILED(ret))
        {
            FLUID_LOG(FLUID_ERR, "wasapi: cannot get default audio device. 0x%x", (unsigned)ret);
            return NULL;
        }
        else
        {
            return dev;
        }
    }

    fluid_wasapi_foreach_device(fluid_wasapi_finddev_callback, &d);

    if(d.id != NULL)
    {
        ret = IMMDeviceEnumerator_GetDevice(denum, d.id, &dev);
        FLUID_FREE(d.id);

        if(FAILED(ret))
        {
            FLUID_LOG(FLUID_ERR, "wasapi: cannot find device with id. 0x%x", (unsigned)ret);
            return NULL;
        }

        return dev;
    }
    else
    {
        FLUID_LOG(FLUID_ERR, "wasapi: cannot find device \"%s\".", name);
        return NULL;
    }
}

static FLUID_INLINE int16_t
round_clip_to_i16(float x)
{
    long i;

    if(x >= 0.0f)
    {
        i = (long)(x + 0.5f);

        if(FLUID_UNLIKELY(i > 32767))
        {
            i = 32767;
        }
    }
    else
    {
        i = (long)(x - 0.5f);

        if(FLUID_UNLIKELY(i < -32768))
        {
            i = -32768;
        }
    }

    return (int16_t)i;
}
#endif /* WASAPI_SUPPORT */

