/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "sun4i_audio_hardware"
#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>

#include "ril_interface.h"

//#define F_LOG LOGV("%s, line: %d", __FUNCTION__, __LINE__);
#define F_LOG LOGV("########## %s ##########", __FUNCTION__);

/* Sun4i Mixer Controls. TODO: implement Mic mute and Master Volume control
ctl type    num name                                     value range
0   INT     1   Master Playback Volume                   (0->63) 59
1   BOOL    1   Playback Switch                          (0/1) off
2   INT     1   Capture Volume                           (0->7) 3
3   INT     1   Fm Volume                                (0->7) 3
4   BOOL    1   Line Volume                              (0/1) on
5   INT     1   MicL Volume                              (0->3) 2
6   INT     1   MicR Volume                              (0->3) 2
7   BOOL    1   FmL Switch                               (0/1) off
8   BOOL    1   FmR Switch                               (0/1) off
9   BOOL    1   LineL Switch                             (0/1) off
10  BOOL    1   LineR Switch                             (0/1) off
11  BOOL    1   Ldac Left Mixer                          (0/1) off
12  BOOL    1   Rdac Right Mixer                         (0/1) off
13  BOOL    1   Ldac Right Mixer                         (0/1) off
14  INT     1   Mic Input Mux                            (0->15) 0
15  INT     1   ADC Input Mux                            (0->15) 2
*/

/* Mixer control names for A10 */
#define MIXER_MASTER_PLAYBACK_VOLUME       "Master Playback Volume"
#define MIXER_PLAYBACK_SWITCH              "Playback Switch"
#define MIXER_CAPTURE_VOLUME               "Capture Volume"
#define MIXER_FM_VOLUME                    "Fm Volume"
#define MIXER_LINE_VOLUME                  "Line Volume"
#define MIXER_MICL_VOLUME                  "MicL Volume"
#define MIXER_MICR_VOLUME                  "MicR Volume"
#define MIXER_FML_SWITCH                   "FmL Switch"
#define MIXER_FMR_SWITCH                   "FmR Switch"
#define MIXER_LINEL_SWITCH                 "LineL Switch"
#define MIXER_LINER_SWITCH                 "LineR Switch"
#define MIXER_LDAC_LEFT_MIXER              "Ldac Left Mixer"
#define MIXER_RDAC_RIGHT_MIXER             "Rdac Right Mixer"
#define MIXER_LDAC_RIGHT_MIXER             "Ldac Right Mixer"
#define MIXER_MIC_INPUT_MUX                "Mic Input Mux"
#define MIXER_ADC_INPUT_MUX                "ADC Input Mux"

/* Master Playback Volume 0dB */
#define MASTER_VOLUME                      0

/* ALSA cards for A10 */
#define CARD_A10_ABE 0
#define CARD_A10_HDMI 1
#define CARD_DEFAULT CARD_A10_ABE

/* ALSA ports for A10 */
#define PORT_MM 0
#define PORT_MM_UL 0
#define PORT_MODEM 5
#define PORT_SPDIF 9
#define PORT_HDMI 0

/* EXTERNAL USB DAC */
#define OUT_CARD_CID_PROPERTY  "usb.audio.out.device"
#define OUT_CARD_FREQ_PROPERTY  "usb.audio.out.freq"
/* Define first device after HDMI as default */
#define OUT_CARD_CID  "pcmC2D0p"
#define CAP_CARD_CID_PROPERTY  "usb.audio.cap.device"
#define CAP_CARD_FREQ_PROPERTY  "usb.audio.cap.freq"
/* Define internal MIC as default input source */
#define CAP_CARD_CID  "pcmC0D0c"

/* constraint imposed by ABE: all period sizes must be multiples of 24 */
#define ABE_BASE_FRAME_COUNT 24
/* number of base blocks in a short period (low latency) */
#define SHORT_PERIOD_MULTIPLIER 80  /* 40 ms */		//ex.44  /* 22 ms */
/* number of frames per short period (low latency) */
#define SHORT_PERIOD_SIZE (ABE_BASE_FRAME_COUNT * SHORT_PERIOD_MULTIPLIER)
/* number of short periods in a long period (low power) */
#define LONG_PERIOD_MULTIPLIER 6  /* 240 ms */		//ex.14  /* 308 ms */
/* number of frames per long period (low power) */
#define LONG_PERIOD_SIZE (SHORT_PERIOD_SIZE * LONG_PERIOD_MULTIPLIER)
/* number of periods for low power playback */
#define PLAYBACK_LONG_PERIOD_COUNT 2
/* number of pseudo periods for low latency playback */
#define PLAYBACK_SHORT_PERIOD_COUNT 4
/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 5000

// add for capture
#define CAPTURE_PERIOD_SIZE 4096	// not less than 8192
/* number of periods for capture */
#define CAPTURE_PERIOD_COUNT 4		//ex.2

#define RESAMPLER_BUFFER_FRAMES (SHORT_PERIOD_SIZE * 2)
#define RESAMPLER_BUFFER_SIZE (4 * RESAMPLER_BUFFER_FRAMES)

#define DEFAULT_OUT_SAMPLING_RATE 44100

/* sampling rate when using MM low power port */
#define MM_44100_SAMPLING_RATE 44100
/* sampling rate when using MM full power port */
#define MM_48000_SAMPLING_RATE 48000
/* sampling rate when using VX port for narrow band */
#define VX_NB_SAMPLING_RATE 8000
/* sampling rate when using VX port for wide band */
#define VX_WB_SAMPLING_RATE 16000

#define MIXER_ABE_GAIN_0DB 120
/* conversions from dB to ABE and codec gains */
#define DB_TO_ABE_GAIN(x) ((x) + MIXER_ABE_GAIN_0DB)
#define DB_TO_CAPTURE_PREAMPLIFIER_VOLUME(x) (((x) + 6) / 6)
#define DB_TO_CAPTURE_VOLUME(x) (((x) - 6) / 6)
#define DB_TO_HEADSET_VOLUME(x) (((x) + 30) / 2)
#define DB_TO_SPEAKER_VOLUME(x) (((x) + 52) / 2)
#define DB_TO_EARPIECE_VOLUME(x) (((x) + 24) / 2)
/* conversions from codec and ABE gains to dB */
#define DB_FROM_SPEAKER_VOLUME(x) ((x) * 2 - 52)

/* use-case specific mic volumes, all in dB */
#define CAPTURE_MAIN_MIC_VOLUME 2
#define VOICE_RECOGNITION_MAIN_MIC_VOLUME 3
#define CAMCORDER_MAIN_MIC_VOLUME 2
#define VOIP_MAIN_MIC_VOLUME 2
#define VOICE_CALL_MAIN_MIC_VOLUME 2

/* use-case specific output volumes */
#define NORMAL_SPEAKER_VOLUME 60
#define NORMAL_HEADSET_VOLUME 50
#define NORMAL_HEADPHONE_VOLUME 50
#define NORMAL_EARPIECE_VOLUME 50

#define VOICE_CALL_SPEAKER_VOLUME 60
#define VOICE_CALL_HEADSET_VOLUME 50
#define VOICE_CALL_EARPIECE_VOLUME 50

#define VOIP_SPEAKER_VOLUME 60
#define VOIP_HEADSET_VOLUME 50
#define VOIP_EARPIECE_VOLUME 50

#define HEADPHONE_VOLUME_TTY -2
#define RINGTONE_HEADSET_VOLUME_OFFSET -14

/* product-specific defines */
#define PRODUCT_DEVICE_PROPERTY "ro.product.board"
#define PRODUCT_NAME_PROPERTY   "ro.product.name"
#define PRODUCT_DEVICE_A10      "crane"

enum tty_modes {
    TTY_MODE_OFF,
    TTY_MODE_VCO,
    TTY_MODE_HCO,
    TTY_MODE_FULL
};

struct pcm_config pcm_config_mm = {
    .channels = 2,
    .rate = MM_48000_SAMPLING_RATE,
    .period_size = SHORT_PERIOD_SIZE,
    .period_count = PLAYBACK_SHORT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_mm_ul = {
    .channels = 2,
    .rate = MM_48000_SAMPLING_RATE,
    .period_size = 1024,
    .period_count = CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_vx = {
    .channels = 2,
    .rate = VX_NB_SAMPLING_RATE,
    .period_size = 160,
    .period_count = 2,
    .format = PCM_FORMAT_S16_LE,
};

#define MIN(x, y) ((x) > (y) ? (y) : (x))

struct route_setting
{
    char *ctl_name;
    int intval;
    char *strval;
};

/* These are initial mixer values */
struct route_setting defaults[] = {
    {
        .ctl_name = MIXER_MASTER_PLAYBACK_VOLUME,
        .intval = MASTER_VOLUME,
    },
    {
        .ctl_name = MIXER_LINE_VOLUME,
        .intval = 0,
    },
    {
        .ctl_name = MIXER_CAPTURE_VOLUME,
        .intval = 0,
    },
    {
        .ctl_name = MIXER_FM_VOLUME,
        .intval = 0,
    },
    {
        .ctl_name = MIXER_ADC_INPUT_MUX,
        .intval = 2, /* ADC value must be 2 for internal mic input */
    },
};

struct mixer_ctls
{
    struct mixer_ctl *master_vol;
    struct mixer_ctl *playback_sw;
    struct mixer_ctl *capture_vol;
    struct mixer_ctl *fm_vol;
    struct mixer_ctl *line_vol;
    struct mixer_ctl *micl_vol;
    struct mixer_ctl *micr_vol;
    struct mixer_ctl *fml_sw;
    struct mixer_ctl *fmr_sw;
    struct mixer_ctl *linel_sw;
    struct mixer_ctl *liner_sw;
    struct mixer_ctl *ldac_left;
    struct mixer_ctl *rdac_right;
    struct mixer_ctl *ldac_right;
    struct mixer_ctl *mic_input_mux;
    struct mixer_ctl *adc_input_mux;
};

struct sun4i_audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct mixer *mixer;
    struct mixer_ctls mixer_ctls;
    int mode;
    int devices;
    struct pcm *pcm_modem_dl;
    struct pcm *pcm_modem_ul;
    int in_call;
    float voice_volume;
    struct sun4i_stream_in *active_input;
    struct sun4i_stream_out *active_output;
    bool mic_mute;
    int tty_mode;
    struct echo_reference_itfe *echo_reference;
    bool bluetooth_nrec;
    bool device_is_a10;
    int wb_amr;
    bool low_power;
#ifdef __ENABLE_RIL
    /* RIL */
    struct ril_handle ril;
#endif
};

struct sun4i_stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct pcm_config config;
    struct pcm *pcm;
    struct resampler_itfe *resampler;
    char *buffer;
    int standby;
    struct echo_reference_itfe *echo_reference;
    struct sun4i_audio_device *dev;
    int write_threshold;
    bool low_power;
};

#define MAX_PREPROCESSORS 3 /* maximum one AGC + one NS + one AEC per input stream */

struct sun4i_stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct pcm_config config;
    struct pcm *pcm;
    int device;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t frames_in;
    unsigned int requested_rate;
    int standby;
    int source;
    struct echo_reference_itfe *echo_reference;
    bool need_echo_reference;
    effect_handle_t preprocessors[MAX_PREPROCESSORS];
    int num_preprocessors;
    int16_t *proc_buf;
    size_t proc_buf_size;
    size_t proc_frames_in;
    int16_t *ref_buf;
    size_t ref_buf_size;
    size_t ref_frames_in;
    int read_status;

    struct sun4i_audio_device *dev;
};

/**
 * NOTE: when multiple mutexes have to be acquired, always respect the following order:
 *        hw device > in stream > out stream
 */


static void select_output_device(struct sun4i_audio_device *adev);
static void select_input_device(struct sun4i_audio_device *adev);
static int adev_set_voice_volume(struct audio_hw_device *dev, float volume);
static int do_input_standby(struct sun4i_stream_in *in);
static int do_output_standby(struct sun4i_stream_out *out);

/* Returns true on devices that are a10 (crane board), false otherwise */
static int is_device_a10(void)
{
    char property[PROPERTY_VALUE_MAX];

    property_get(PRODUCT_DEVICE_PROPERTY, property, PRODUCT_DEVICE_A10);

    /* return true if the property matches the given value */
    return strcmp(property, PRODUCT_DEVICE_A10) == 0;
}

/* Returns true for external DAC (if present), false otherwise */
static int is_device_usb_dac(void)
{
    char property[PROPERTY_VALUE_MAX];
    property_get(OUT_CARD_CID_PROPERTY, property, OUT_CARD_CID);
    struct stat info;
    if (strcmp(&property[7], "p") == 0) {
        char path[18]="/dev/snd/";
        strcat(path, property);
        int ret = stat(path, &info);
        if (ret == 0) {
            LOGV("# property: %s, dev: %s, present", OUT_CARD_CID_PROPERTY, property);
        } else {
            LOGV("# property: %s, dev: %s, device not exist! use default output", OUT_CARD_CID_PROPERTY, property);
        }
        return(ret == -1 ? 0 : 1);
    } else {
        LOGV("# property: %s, dev: %s, not a playback device! use default output", OUT_CARD_CID_PROPERTY, property);
    }
    return(0);
}

/* Returns true for external ADC (if present), false otherwise */
static int is_device_usb_cap(void)
{
    char property[PROPERTY_VALUE_MAX];
    property_get(CAP_CARD_CID_PROPERTY, property, CAP_CARD_CID);
    struct stat info;
    /* property value: pcmC[4]D[6]c */    
    /* Special case for internal MIC */
    if (strcmp(property, "pcmC0D0c") == 0) {
    	LOGV("# internal mic input selected");
    } else if (strcmp(&property[7], "c") == 0) {
    	char path[18]="/dev/snd/";
        strcat(path, property);
        int ret = stat(path, &info);
        if (ret == 0) {
            LOGV("# property: %s, dev: %s, present", CAP_CARD_CID_PROPERTY, property);
        } else {
            LOGV("# property: %s, dev: %s, device not exists! use default source (internal mic)", CAP_CARD_CID_PROPERTY, property);
        }
        return(ret == -1 ? 0 : 1);
    } else {
        LOGV("# property: %s, dev: %s, not a capture device! use default source (internal mic)", CAP_CARD_CID_PROPERTY, property);
    }
    return(0);
}

/* The enable flag when 0 makes the assumption that enums are disabled by
 * "Off" and integers/booleans by 0 */
static int set_route_by_array(struct mixer *mixer, struct route_setting *route,
                              int enable)
{
    struct mixer_ctl *ctl;
    unsigned int i, j;

    /* Go through the route array and set each value */
    i = 0;
    while (route[i].ctl_name) {
        ctl = mixer_get_ctl_by_name(mixer, route[i].ctl_name);
        if (!ctl)
            return -EINVAL;

        if (route[i].strval) {
            if (enable)
                mixer_ctl_set_enum_by_string(ctl, route[i].strval);
            else
                mixer_ctl_set_enum_by_string(ctl, "Off");
        } else {
            /* This ensures multiple (i.e. stereo) values are set jointly */
            for (j = 0; j < mixer_ctl_get_num_values(ctl); j++) {
                if (enable)
                    mixer_ctl_set_value(ctl, j, route[i].intval);
                else
                    mixer_ctl_set_value(ctl, j, 0);
            }
        }
        i++;
    }

    return 0;
}

static int start_call(struct sun4i_audio_device *adev)
{
    LOGE("Opening modem PCMs");

    pcm_config_vx.rate = adev->wb_amr ? VX_WB_SAMPLING_RATE : VX_NB_SAMPLING_RATE;

    /* Open modem PCM channels */
    if (adev->pcm_modem_dl == NULL) {
        adev->pcm_modem_dl = pcm_open(0, PORT_MODEM, PCM_OUT, &pcm_config_vx);
        if (!pcm_is_ready(adev->pcm_modem_dl)) {
            LOGE("cannot open PCM modem DL stream: %s", pcm_get_error(adev->pcm_modem_dl));
            goto err_open_dl;
        }
    }

    if (adev->pcm_modem_ul == NULL) {
        adev->pcm_modem_ul = pcm_open(0, PORT_MODEM, PCM_IN, &pcm_config_vx);
        if (!pcm_is_ready(adev->pcm_modem_ul)) {
            LOGE("cannot open PCM modem UL stream: %s", pcm_get_error(adev->pcm_modem_ul));
            goto err_open_ul;
        }
    }

    pcm_start(adev->pcm_modem_dl);
    pcm_start(adev->pcm_modem_ul);

    return 0;

err_open_ul:
    pcm_close(adev->pcm_modem_ul);
    adev->pcm_modem_ul = NULL;
err_open_dl:
    pcm_close(adev->pcm_modem_dl);
    adev->pcm_modem_dl = NULL;

    return -ENOMEM;
}

static void end_call(struct sun4i_audio_device *adev)
{
    LOGE("Closing modem PCMs");

    pcm_stop(adev->pcm_modem_dl);
    pcm_stop(adev->pcm_modem_ul);
    pcm_close(adev->pcm_modem_dl);
    pcm_close(adev->pcm_modem_ul);
    adev->pcm_modem_dl = NULL;
    adev->pcm_modem_ul = NULL;
}

static void set_eq_filter(struct sun4i_audio_device *adev)
{
	return; /* not implemented */
}

void audio_set_wb_amr_callback(void *data, int enable)
{
    struct sun4i_audio_device *adev = (struct sun4i_audio_device *)data;

    pthread_mutex_lock(&adev->lock);
    if (adev->wb_amr != enable) {
        adev->wb_amr = enable;

        /* reopen the modem PCMs at the new rate */
        if (adev->in_call) {
            end_call(adev);
            set_eq_filter(adev);
            start_call(adev);
        }
    }
    pthread_mutex_unlock(&adev->lock);
}

static void set_incall_device(struct sun4i_audio_device *adev)
{
    int device_type;

    switch(adev->devices & AUDIO_DEVICE_OUT_ALL) {
        case AUDIO_DEVICE_OUT_EARPIECE:
            device_type = SOUND_AUDIO_PATH_HANDSET;
            break;
        case AUDIO_DEVICE_OUT_SPEAKER:
        case AUDIO_DEVICE_OUT_AUX_DIGITAL:
            device_type = SOUND_AUDIO_PATH_SPEAKER;
            break;
        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
            device_type = SOUND_AUDIO_PATH_HEADSET;
            break;
        case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
            device_type = SOUND_AUDIO_PATH_HEADPHONE;
            break;
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            if (adev->bluetooth_nrec)
                device_type = SOUND_AUDIO_PATH_BLUETOOTH;
            else
                device_type = SOUND_AUDIO_PATH_BLUETOOTH_NO_NR;
            break;
        default:
            device_type = SOUND_AUDIO_PATH_HANDSET;
            break;
    }
#ifdef __ENABLE_RIL
    /* if output device isn't supported, open modem side to handset by default */
    ril_set_call_audio_path(&adev->ril, device_type);
#endif
}

static void set_input_volumes(struct sun4i_audio_device *adev, int main_mic_on,
                              int headset_mic_on, int sub_mic_on)
{
    unsigned int channel;
    int volume = 0;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        /* special case: don't look at input source for IN_CALL state */
        volume = VOICE_CALL_MAIN_MIC_VOLUME;
    } else if (adev->active_input) {
        /* determine input volume by use case */
        switch (adev->active_input->source) {
        case AUDIO_SOURCE_MIC: /* general capture */
            volume = CAPTURE_MAIN_MIC_VOLUME;
            break;

        case AUDIO_SOURCE_CAMCORDER:
            volume = CAMCORDER_MAIN_MIC_VOLUME;
            break;

        case AUDIO_SOURCE_VOICE_RECOGNITION:
            volume = VOICE_RECOGNITION_MAIN_MIC_VOLUME;
            break;

        case AUDIO_SOURCE_VOICE_COMMUNICATION: /* VoIP */
            volume = VOIP_MAIN_MIC_VOLUME;
            break;

        default:
            /* nothing to do */
            break;
        }
    }
	LOGV("# set_input_volumes: volume: %u", volume);
	mixer_ctl_set_value(adev->mixer_ctls.micl_vol, 0, volume);
	mixer_ctl_set_value(adev->mixer_ctls.micr_vol, 0, volume);
}

static void set_output_volumes(struct sun4i_audio_device *adev, bool tty_volume)
{
    unsigned int channel;
    int speaker_volume;
    int headset_volume;
    int earpiece_volume;
    bool a10 = adev->device_is_a10;
    int headphone_on = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    int speaker_on = adev->devices & AUDIO_DEVICE_OUT_SPEAKER;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        /* Voice call */
        speaker_volume = VOICE_CALL_SPEAKER_VOLUME;
        headset_volume = VOICE_CALL_HEADSET_VOLUME;
        earpiece_volume = VOICE_CALL_EARPIECE_VOLUME;
    } else if (adev->mode == AUDIO_MODE_IN_COMMUNICATION) {
        /* VoIP */
        speaker_volume = VOIP_SPEAKER_VOLUME;
        headset_volume = VOIP_HEADSET_VOLUME;
        earpiece_volume = VOIP_EARPIECE_VOLUME;
    } else {
        /* Media */
        speaker_volume = NORMAL_SPEAKER_VOLUME;
        if (headphone_on)
            headset_volume = NORMAL_HEADPHONE_VOLUME;
        else
            headset_volume = NORMAL_HEADSET_VOLUME;
        earpiece_volume = NORMAL_EARPIECE_VOLUME;
    }
    if (tty_volume)
        headset_volume = HEADPHONE_VOLUME_TTY;
    else if (adev->mode == AUDIO_MODE_RINGTONE)
        headset_volume += RINGTONE_HEADSET_VOLUME_OFFSET;

/*
    for (channel = 0; channel < 2; channel++) {
        mixer_ctl_set_value(adev->mixer_ctls.speaker_volume, channel,
            DB_TO_SPEAKER_VOLUME(speaker_volume));
        mixer_ctl_set_value(adev->mixer_ctls.headset_volume, channel,
            DB_TO_HEADSET_VOLUME(headset_volume));
    }
    mixer_ctl_set_value(adev->mixer_ctls.earpiece_volume, 0,
        DB_TO_EARPIECE_VOLUME(earpiece_volume));
*/
	LOGV("# set_output_volumes: speaker volume: %u, headset_volume: %u, earpiece_volume: %u", speaker_volume, headset_volume, earpiece_volume);
    mixer_ctl_set_value(adev->mixer_ctls.master_vol, 0, speaker_volume);
}

static void force_all_standby(struct sun4i_audio_device *adev)
{
    struct sun4i_stream_in *in;
    struct sun4i_stream_out *out;

    if (adev->active_output) {
        out = adev->active_output;
        pthread_mutex_lock(&out->lock);
        do_output_standby(out);
        pthread_mutex_unlock(&out->lock);
    }

    if (adev->active_input) {
        in = adev->active_input;
        pthread_mutex_lock(&in->lock);
        do_input_standby(in);
        pthread_mutex_unlock(&in->lock);
    }
}

static void select_mode(struct sun4i_audio_device *adev)
{
    if (adev->mode == AUDIO_MODE_IN_CALL) {
        LOGE("Entering IN_CALL state, in_call=%d", adev->in_call);
        if (!adev->in_call) {
            force_all_standby(adev);
            /* force earpiece route for in call state if speaker is the
            only currently selected route. This prevents having to tear
            down the modem PCMs to change route from speaker to earpiece
            after the ringtone is played, but doesn't cause a route
            change if a headset or bt device is already connected. If
            speaker is not the only thing active, just remove it from
            the route. We'll assume it'll never be used initally during
            a call. This works because we're sure that the audio policy
            manager will update the output device after the audio mode
            change, even if the device selection did not change. */
            if ((adev->devices & AUDIO_DEVICE_OUT_ALL) == AUDIO_DEVICE_OUT_SPEAKER)
                adev->devices = AUDIO_DEVICE_OUT_EARPIECE |
                                AUDIO_DEVICE_IN_BUILTIN_MIC;
            else
                adev->devices &= ~AUDIO_DEVICE_OUT_SPEAKER;
            select_output_device(adev);
            start_call(adev);
#ifdef __ENABLE_RIL
            ril_set_call_clock_sync(&adev->ril, SOUND_CLOCK_START);
#endif
            adev_set_voice_volume(&adev->hw_device, adev->voice_volume);
            adev->in_call = 1;
        }
    } else {
        LOGE("Leaving IN_CALL state, in_call=%d, mode=%d",
             adev->in_call, adev->mode);
        if (adev->in_call) {
            adev->in_call = 0;
            end_call(adev);
            force_all_standby(adev);
            select_output_device(adev);
            select_input_device(adev);
        }
    }
}

static void select_output_device(struct sun4i_audio_device *adev)
{
    int headset_on;
    int headphone_on;
    int speaker_on;
    int earpiece_on;
    int bt_on;
    bool tty_volume = false;
    unsigned int channel;

    /* Mute VX_UL to avoid pop noises in the tx path
     * during call before switch changes.
     */
/*
    if (adev->mode == AUDIO_MODE_IN_CALL) {
        for (channel = 0; channel < 2; channel++)
            mixer_ctl_set_value(adev->mixer_ctls.voice_ul_volume,
                                channel, 0);
    }
*/
    headset_on = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADSET;
    headphone_on = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    speaker_on = adev->devices & AUDIO_DEVICE_OUT_SPEAKER;
    earpiece_on = adev->devices & AUDIO_DEVICE_OUT_EARPIECE;
    bt_on = adev->devices & AUDIO_DEVICE_OUT_ALL_SCO;

    /* force rx path according to TTY mode when in call */
    if (adev->mode == AUDIO_MODE_IN_CALL && !bt_on) {
        switch(adev->tty_mode) {
            case TTY_MODE_FULL:
            case TTY_MODE_VCO:
                /* rx path to headphones */
                headphone_on = 1;
                headset_on = 0;
                speaker_on = 0;
                earpiece_on = 0;
                tty_volume = true;
                break;
            case TTY_MODE_HCO:
                /* rx path to device speaker */
                headphone_on = 0;
                headset_on = 0;
                speaker_on = 1;
                earpiece_on = 0;
                break;
            case TTY_MODE_OFF:
            default:
                /* force speaker on when in call and HDMI is selected as voice DL audio
                 * cannot be routed to HDMI by ABE */
                if (adev->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)
                    speaker_on = 1;
                break;
        }
    }
	LOGV("# select_output_device: headset_on: %u, headphone_on: %u, speaker_on: %u, earpiece_on: %u, bt_on: %u, tty_volume: %u",
	headset_on, headphone_on, speaker_on, earpiece_on, bt_on, tty_volume);
    set_output_volumes(adev, tty_volume);

    /* Special case: select input path if in a call, otherwise
       in_set_parameters is used to update the input route
       todo: use sub mic for handsfree case */
    if (adev->mode == AUDIO_MODE_IN_CALL) {
		/* force tx path according to TTY mode when in call */
		switch(adev->tty_mode) {
			case TTY_MODE_FULL:
			case TTY_MODE_HCO:
				/* tx path from headset mic */
				headphone_on = 0;
				headset_on = 1;
				speaker_on = 0;
				earpiece_on = 0;
				break;
			case TTY_MODE_VCO:
				/* tx path from device sub mic */
				headphone_on = 0;
				headset_on = 0;
				speaker_on = 1;
				earpiece_on = 0;
				break;
			case TTY_MODE_OFF:
			default:
				break;
		}

		set_incall_device(adev);
	
		/* Unmute VX_UL after the switch */
/*
		for (channel = 0; channel < 2; channel++) {
			mixer_ctl_set_value(adev->mixer_ctls.voice_ul_volume,
								channel, MIXER_ABE_GAIN_0DB);
		}
*/
    }
//    mixer_ctl_set_value(adev->mixer_ctls.sidetone_capture, 0, sidetone_capture_on);
}

static void select_input_device(struct sun4i_audio_device *adev)
{
    int headset_on = 0;
    int main_mic_on = 0;
    int sub_mic_on = 0;
    int bt_on = adev->devices & AUDIO_DEVICE_IN_ALL_SCO;

    if (!bt_on) {
        if ((adev->mode != AUDIO_MODE_IN_CALL) && (adev->active_input != 0)) {
            /* sub mic is used for camcorder or VoIP on speaker phone */
            sub_mic_on = (adev->active_input->source == AUDIO_SOURCE_CAMCORDER) ||
                         ((adev->devices & AUDIO_DEVICE_OUT_SPEAKER) &&
                          (adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION));
        }
        if (!sub_mic_on) {
            headset_on = adev->devices & AUDIO_DEVICE_IN_WIRED_HEADSET;
            main_mic_on = adev->devices & AUDIO_DEVICE_IN_BUILTIN_MIC;
        }
    }

   /* TODO: check how capture is possible during voice calls or if
    * both use cases are mutually exclusive.
    */
/*
    if (bt_on)
        set_route_by_array(adev->mixer, mm_ul2_bt, 1);
    else {
*/
        /* Select front end */
/*
        if (main_mic_on || headset_on)
            set_route_by_array(adev->mixer, mm_ul2_amic_left, 1);
        else if (sub_mic_on)
            set_route_by_array(adev->mixer, mm_ul2_amic_right, 1);
        else
            set_route_by_array(adev->mixer, mm_ul2_amic_left, 0);
*/
        /* Select back end */
/*
        mixer_ctl_set_enum_by_string(adev->mixer_ctls.right_capture,
                                     sub_mic_on ? MIXER_SUB_MIC : "Off");
        mixer_ctl_set_enum_by_string(adev->mixer_ctls.left_capture,
                                     main_mic_on ? MIXER_MAIN_MIC :
                                     (headset_on ? MIXER_HS_MIC : "Off"));
    }
*/
	LOGV("# select_input_device: main_mic_on: %u, headset_on: %u, sub_mic_on: %u", main_mic_on, headset_on, sub_mic_on);
    set_input_volumes(adev, main_mic_on, headset_on, sub_mic_on);
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct sun4i_stream_out *out)
{
    F_LOG;
    struct sun4i_audio_device *adev = out->dev;
    unsigned int card = CARD_DEFAULT;
    unsigned int port = PORT_MM;

    adev->active_output = out;

    if (adev->mode != AUDIO_MODE_IN_CALL) {
        /* FIXME: only works if only one output can be active at a time */
        select_output_device(adev);
    }
    /* S/PDIF takes priority over HDMI audio. In the case of multiple
     * devices, this will cause use of S/PDIF or HDMI only */
    out->config.rate = MM_48000_SAMPLING_RATE;
    if (adev->devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
        port = PORT_SPDIF;
        LOGV("### SPDIF audio out selected! Sampling rate: %d Hz", out->config.rate);
    }
    else if(adev->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        card = CARD_A10_HDMI;
        port = PORT_HDMI;
        out->config.rate = MM_44100_SAMPLING_RATE;
        LOGV("### HDMI audio out selected! Sampling rate: %d Hz", out->config.rate);
    }
    /* HACK: USB DAC output */
    else if(is_device_usb_dac()) {
    	char property[PROPERTY_VALUE_MAX];
        property_get(OUT_CARD_CID_PROPERTY, property, OUT_CARD_CID); 
        /* property value: pcmC[4]D[6]p */
    	card = property[4] - '0';
    	port = property[6] - '0';
    	/* init mixer controls, creative sb live doesn't work without it */
    	adev->mixer = mixer_open(card);
        /* HW Info (failsafe check) */
        struct pcm_config config;
        struct pcm *pcm;
    	pcm = pcm_hwinfo(card, port, PCM_OUT, &config);
    	if (!pcm || !pcm_is_ready(pcm)) {
      		LOGE("### Unable to get Hardware information for device %s (%s)\n",
              property, pcm_get_error(pcm));
      	goto exit;
    	}
        LOGV("# Supported Rates: (%uHz - %uHz)\n", config.rate_min, config.rate_max);
        LOGV("# Supported Channels: (%uCh - %uCh)\n", config.channels_min, config.channels_max);
        /* Define preferred rate */                
    	property_get(OUT_CARD_FREQ_PROPERTY, property, "44100"); 	
    	out->config.rate = atoi(property);
        if (!(out->config.rate >= config.rate_min &&
                  out->config.rate <= config.rate_max)) {
            LOGV("# Requested %dHz using supported value %dHz\n",out->config.rate, config.rate_max);
            out->config.rate = config.rate_max;
    	}
    	pcm_close(pcm);
        /* END of HW Info */
        LOGV("### USB audio out selected! Channels: %dCh Sampling rate: %dHz", out->config.channels, out->config.rate);
    }
exit:
    /* default to low power: will be corrected in out_write if necessary before first write to
     * tinyalsa.
     */
    out->write_threshold = PLAYBACK_LONG_PERIOD_COUNT * LONG_PERIOD_SIZE;
    out->config.start_threshold = SHORT_PERIOD_SIZE * 2;
//    out->config.avail_min = LONG_PERIOD_SIZE;
    out->low_power = 1;

    out->pcm = pcm_open(card, port, PCM_OUT | PCM_MMAP | PCM_NOIRQ, &out->config);

    if (!pcm_is_ready(out->pcm)) {
        LOGE("cannot open pcm_out driver: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        adev->active_output = NULL;
        return -ENOMEM;
    }

    if (adev->echo_reference != NULL)
        out->echo_reference = adev->echo_reference;

    out->resampler->reset(out->resampler);

    return 0;
}

static int check_input_parameters(uint32_t sample_rate, int format, int channel_count)
{
    if (format != AUDIO_FORMAT_PCM_16_BIT)
        return -EINVAL;

    if ((channel_count < 1) || (channel_count > 2))
        return -EINVAL;

    switch(sample_rate) {
    case 8000:
    case 11025:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static size_t get_input_buffer_size(uint32_t sample_rate, int format, int channel_count)
{
    size_t size;
    size_t device_rate;

    if (check_input_parameters(sample_rate, format, channel_count) != 0)
        return 0;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size = (pcm_config_mm_ul.period_size * sample_rate) / pcm_config_mm_ul.rate;
    size = ((size + 15) / 16) * 16;

    return size * channel_count * sizeof(short);
}

static void add_echo_reference(struct sun4i_stream_out *out,
                               struct echo_reference_itfe *reference)
{
    pthread_mutex_lock(&out->lock);
    out->echo_reference = reference;
    pthread_mutex_unlock(&out->lock);
}

static void remove_echo_reference(struct sun4i_stream_out *out,
                                  struct echo_reference_itfe *reference)
{
    pthread_mutex_lock(&out->lock);
    if (out->echo_reference == reference) {
        /* stop writing to echo reference */
        reference->write(reference, NULL);
        out->echo_reference = NULL;
    }
    pthread_mutex_unlock(&out->lock);
}

static void put_echo_reference(struct sun4i_audio_device *adev,
                          struct echo_reference_itfe *reference)
{
    if (adev->echo_reference != NULL &&
            reference == adev->echo_reference) {
        if (adev->active_output != NULL)
            remove_echo_reference(adev->active_output, reference);
        release_echo_reference(reference);
        adev->echo_reference = NULL;
    }
}

static struct echo_reference_itfe *get_echo_reference(struct sun4i_audio_device *adev,
                                               audio_format_t format,
                                               uint32_t channel_count,
                                               uint32_t sampling_rate)
{
    put_echo_reference(adev, adev->echo_reference);
    if (adev->active_output != NULL) {
        struct audio_stream *stream = &adev->active_output->stream.common;
        uint32_t wr_channel_count = popcount(stream->get_channels(stream));
        uint32_t wr_sampling_rate = stream->get_sample_rate(stream);

        int status = create_echo_reference(AUDIO_FORMAT_PCM_16_BIT,
                                           channel_count,
                                           sampling_rate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           wr_channel_count,
                                           wr_sampling_rate,
                                           &adev->echo_reference);
        if (status == 0)
            add_echo_reference(adev->active_output, adev->echo_reference);
    }
    return adev->echo_reference;
}

static int get_playback_delay(struct sun4i_stream_out *out,
                       size_t frames,
                       struct echo_reference_buffer *buffer)
{
    size_t kernel_frames;
    int status;

    status = pcm_get_htimestamp(out->pcm, &kernel_frames, &buffer->time_stamp);
    if (status < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        LOGV("get_playback_delay(): pcm_get_htimestamp error,"
                "setting playbackTimestamp to 0");
        return status;
    }

    kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

    /* adjust render time stamp with delay added by current driver buffer.
     * Add the duration of current frame as we want the render time of the last
     * sample being written. */
    buffer->delay_ns = (long)(((int64_t)(kernel_frames + frames)* 1000000000)/
                            MM_48000_SAMPLING_RATE);

    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return DEFAULT_OUT_SAMPLING_RATE;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct sun4i_stream_out *out = (struct sun4i_stream_out *)stream;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size_t size = (SHORT_PERIOD_SIZE * DEFAULT_OUT_SAMPLING_RATE) / out->config.rate;
    size = ((size + 15) / 16) * 16;
    return size * audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static int out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, int format)
{
    return 0;
}

/* must be called with hw device and output stream mutexes locked */
static int do_output_standby(struct sun4i_stream_out *out)
{
    struct sun4i_audio_device *adev = out->dev;

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;

        adev->active_output = 0;

        /* if in call, don't turn off the output stage. This will
        be done when the call is ended */
        if (adev->mode != AUDIO_MODE_IN_CALL) {
            /* FIXME: only works if only one output can be active at a time */
//            set_route_by_array(adev->mixer, hs_output, 0);
//            set_route_by_array(adev->mixer, hf_output, 0);
        }

        /* stop writing to echo reference */
        if (out->echo_reference != NULL) {
            out->echo_reference->write(out->echo_reference, NULL);
            out->echo_reference = NULL;
        }

        out->standby = 1;
    }
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct sun4i_stream_out *out = (struct sun4i_stream_out *)stream;
    int status;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    status = do_output_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return status;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct sun4i_stream_out *out = (struct sun4i_stream_out *)stream;
    struct sun4i_audio_device *adev = out->dev;
    struct sun4i_stream_in *in;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;
    bool force_input_standby = false;

    parms = str_parms_create_str(kvpairs);

	/* A10 HDMI output handling */
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        if (((adev->devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
            if (out == adev->active_output) {
                /* a change in output device may change the microphone selection */
                if (adev->active_input &&
                        adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                    force_input_standby = true;
                }
                /* force standby if moving to/from HDMI */
                if (((val & AUDIO_DEVICE_OUT_AUX_DIGITAL) ^
                        (adev->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)) ||
                        ((val & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) ^
                        (adev->devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)))
                    do_output_standby(out);
            }
            adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
            adev->devices |= val;
            select_output_device(adev);
        }
        pthread_mutex_unlock(&out->lock);
        if (force_input_standby) {
            in = adev->active_input;
            pthread_mutex_lock(&in->lock);
            do_input_standby(in);
            pthread_mutex_unlock(&in->lock);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct sun4i_stream_out *out = (struct sun4i_stream_out *)stream;

    return (SHORT_PERIOD_SIZE * PLAYBACK_SHORT_PERIOD_COUNT * 1000) / out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret;
    struct sun4i_stream_out *out = (struct sun4i_stream_out *)stream;
    struct sun4i_audio_device *adev = out->dev;
    size_t frame_size = audio_stream_frame_size(&out->stream.common);
    size_t in_frames = bytes / frame_size;
    size_t out_frames = RESAMPLER_BUFFER_SIZE / frame_size;
    bool force_input_standby = false;
    struct sun4i_stream_in *in;
    bool low_power;
    int kernel_frames;
    void *buf;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = 0;
        /* a change in output device may change the microphone selection */
        if (adev->active_input &&
                adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION)
            force_input_standby = true;
    }
    low_power = adev->low_power && !adev->active_input;
    pthread_mutex_unlock(&adev->lock);

    if (low_power != out->low_power) {
        if (low_power) {
            out->write_threshold = LONG_PERIOD_SIZE * PLAYBACK_LONG_PERIOD_COUNT;
//            out->config.avail_min = LONG_PERIOD_SIZE;
        } else {
            out->write_threshold = SHORT_PERIOD_SIZE * PLAYBACK_SHORT_PERIOD_COUNT;
//            out->config.avail_min = SHORT_PERIOD_SIZE;
        }
//        pcm_set_avail_min(out->pcm, out->config.avail_min);
        out->low_power = low_power;
    }

    /* only use resampler if required */
    if (out->config.rate != DEFAULT_OUT_SAMPLING_RATE/* && out->config.rate != 48000*/) {
//    	LOGD("### out->resampler");
        out->resampler->resample_from_input(out->resampler,
                                            (int16_t *)buffer,
                                            &in_frames,
                                            (int16_t *)out->buffer,
                                            &out_frames);
        buf = out->buffer;
    } else {
        out_frames = in_frames;
        buf = (void *)buffer;
    }
    if (out->echo_reference != NULL) {
        struct echo_reference_buffer b;
        b.raw = (void *)buffer;
        b.frame_count = in_frames;

        get_playback_delay(out, out_frames, &b);
        out->echo_reference->write(out->echo_reference, &b);
    }

    /* do not allow more than out->write_threshold frames in kernel pcm driver buffer */
    do {
        struct timespec time_stamp;

        if (pcm_get_htimestamp(out->pcm, (unsigned int *)&kernel_frames, &time_stamp) < 0)
            break;
        kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

        if (kernel_frames > out->write_threshold) {
            unsigned long time = (unsigned long)
                    (((int64_t)(kernel_frames - out->write_threshold) * 1000000) /
                            MM_48000_SAMPLING_RATE);
            if (time < MIN_WRITE_SLEEP_US)
                time = MIN_WRITE_SLEEP_US;
            usleep(time);
        }
    } while (kernel_frames > out->write_threshold);

    ret = pcm_mmap_write(out->pcm, (void *)buf, out_frames * frame_size);

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    if (force_input_standby) {
        pthread_mutex_lock(&adev->lock);
        if (adev->active_input) {
            in = adev->active_input;
            pthread_mutex_lock(&in->lock);
            do_input_standby(in);
            pthread_mutex_unlock(&in->lock);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

/** audio_stream_in implementation **/

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct sun4i_stream_in *in)
{
	F_LOG;	
    int ret = 0;
    struct sun4i_audio_device *adev = in->dev;
    unsigned int card = CARD_DEFAULT;
    unsigned int port = PORT_MM_UL;
    adev->active_input = in;

    if (adev->mode != AUDIO_MODE_IN_CALL) {
        adev->devices &= ~AUDIO_DEVICE_IN_ALL;
        adev->devices |= in->device;
        select_input_device(adev);
    }

    if (in->need_echo_reference && in->echo_reference == NULL)
        in->echo_reference = get_echo_reference(adev,
                                        AUDIO_FORMAT_PCM_16_BIT,
                                        in->config.channels,
                                        in->requested_rate);
    /* HACK: USB DAC input */
    if(is_device_usb_cap()) {
    	char property[PROPERTY_VALUE_MAX];
        property_get(CAP_CARD_CID_PROPERTY, property, CAP_CARD_CID); 
        /* property value: pcmC[4]D[6]c */
    	card = property[4] - '0';
    	port = property[6] - '0';
    	/* init mixer controls, creative sb live doesn't work without it */
    	adev->mixer = mixer_open(card);
        LOGV("# card: %u, port: %u, type: capture", card, port);
        /* HW Info (failsafe check) */
        struct pcm_config config;
        struct pcm *pcm;
    	pcm = pcm_hwinfo(card, port, PCM_IN, &config);
    	if (!pcm || !pcm_is_ready(pcm)) {
      		LOGE("### Unable to get Hardware information for device %s (%s)\n",
              property, pcm_get_error(pcm));
      	goto exit;
    	}
        LOGV("# Supported Rates: (%uHz - %uHz)\n", config.rate_min, config.rate_max);
        LOGV("# Supported Channels: (%uCh - %uCh)\n", config.channels_min, config.channels_max);
        /* Define preferred capture rate */
    	property_get(CAP_CARD_FREQ_PROPERTY, property, "44100"); 	
    	in->config.rate = atoi(property);
        if (!(in->config.rate >= config.rate_min &&
                  in->config.rate<= config.rate_max)) {
            LOGV("# Requested %dHz, using supported value %dHz\n",in->config.rate, config.rate_min);
            in->config.rate = config.rate_min;
    	}
        if (!(in->config.channels >= config.channels_min &&
                  in->config.channels <= config.channels_max)) {
            LOGV("# Requested %dCh, using supported value %dCh\n",in->config.channels, config.channels_min);
            in->config.channels = config.channels_min;
    	}
    	pcm_close(pcm);
        /* END of HW Info */
        LOGV("### USB audio input selected! Channels: %dCh Req Rate: %dHz HW Rate: %dHz", in->config.channels, in->requested_rate, in->config.rate);
    }
exit:	
    /* this assumes routing is done previously */
    in->pcm = pcm_open(card, port, PCM_IN, &in->config);
    if (!pcm_is_ready(in->pcm)) {
        LOGE("cannot open pcm_in driver: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        adev->active_input = NULL;
        return -ENOMEM;
    }

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
		// F_LOG;
		LOGV("### in->resampler");
        in->resampler->reset(in->resampler);
        in->frames_in = 0;
    }
	// F_LOG;
    return 0;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;

    return get_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 in->config.channels);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;

    if (in->config.channels == 1) {
        return AUDIO_CHANNEL_IN_MONO;
    } else {
        return AUDIO_CHANNEL_IN_STEREO;
    }
}

static int in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, int format)
{
    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int do_input_standby(struct sun4i_stream_in *in)
{
    struct sun4i_audio_device *adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;

        adev->active_input = 0;
        if (adev->mode != AUDIO_MODE_IN_CALL) {
            adev->devices &= ~AUDIO_DEVICE_IN_ALL;
            select_input_device(adev);
        }

        if (in->echo_reference != NULL) {
            /* stop reading from echo reference */
            in->echo_reference->read(in->echo_reference, NULL);
            put_echo_reference(adev, in->echo_reference);
            in->echo_reference = NULL;
        }

        in->standby = 1;
    }
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;
    int status;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    status = do_input_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;
    struct sun4i_audio_device *adev = in->dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;
    bool do_standby = false;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value));

    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->source != val) && (val != 0)) {
            in->source = val;
            do_standby = true;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        if ((in->device != val) && (val != 0)) {
            in->device = val;
            do_standby = true;
        }
    }

    if (do_standby)
        do_input_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return ret;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static void get_capture_delay(struct sun4i_stream_in *in,
                       size_t frames,
                       struct echo_reference_buffer *buffer)
{

    /* read frames available in kernel driver buffer */
    size_t kernel_frames;
    struct timespec tstamp;
    long buf_delay;
    long rsmp_delay;
    long kernel_delay;
    long delay_ns;

    if (pcm_get_htimestamp(in->pcm, &kernel_frames, &tstamp) < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        LOGW("read get_capture_delay(): pcm_htimestamp error");
        return;
    }

    /* read frames available in audio HAL input buffer
     * add number of frames being read as we want the capture time of first sample
     * in current buffer */
    buf_delay = (long)(((int64_t)(in->frames_in + in->proc_frames_in) * 1000000000)
                                    / in->config.rate);
    /* add delay introduced by resampler */
    rsmp_delay = 0;
    if (in->resampler) {
        rsmp_delay = in->resampler->delay_ns(in->resampler);
    }

    kernel_delay = (long)(((int64_t)kernel_frames * 1000000000) / in->config.rate);

    delay_ns = kernel_delay + buf_delay + rsmp_delay;

    buffer->time_stamp = tstamp;
    buffer->delay_ns   = delay_ns;
    LOGV("get_capture_delay time_stamp = [%ld].[%ld], delay_ns: [%d],"
         " kernel_delay:[%ld], buf_delay:[%ld], rsmp_delay:[%ld], kernel_frames:[%d], "
         "in->frames_in:[%d], in->proc_frames_in:[%d], frames:[%d]",
         buffer->time_stamp.tv_sec , buffer->time_stamp.tv_nsec, buffer->delay_ns,
         kernel_delay, buf_delay, rsmp_delay, kernel_frames,
         in->frames_in, in->proc_frames_in, frames);

}

static int32_t update_echo_reference(struct sun4i_stream_in *in, size_t frames)
{
    struct echo_reference_buffer b;
    b.delay_ns = 0;

    LOGV("update_echo_reference, frames = [%d], in->ref_frames_in = [%d],  "
          "b.frame_count = [%d]",
         frames, in->ref_frames_in, frames - in->ref_frames_in);
    if (in->ref_frames_in < frames) {
        if (in->ref_buf_size < frames) {
            in->ref_buf_size = frames;
            in->ref_buf = (int16_t *)realloc(in->ref_buf,
                                             in->ref_buf_size *
                                                 in->config.channels * sizeof(int16_t));
        }

        b.frame_count = frames - in->ref_frames_in;
        b.raw = (void *)(in->ref_buf + in->ref_frames_in * in->config.channels);

        get_capture_delay(in, frames, &b);

        if (in->echo_reference->read(in->echo_reference, &b) == 0)
        {
            in->ref_frames_in += b.frame_count;
            LOGV("update_echo_reference: in->ref_frames_in:[%d], "
                    "in->ref_buf_size:[%d], frames:[%d], b.frame_count:[%d]",
                 in->ref_frames_in, in->ref_buf_size, frames, b.frame_count);
        }
    } else
        LOGW("update_echo_reference: NOT enough frames to read ref buffer");
    return b.delay_ns;
}

static int set_preprocessor_param(effect_handle_t handle,
                           effect_param_t *param)
{
    uint32_t size = sizeof(int);
    uint32_t psize = ((param->psize - 1) / sizeof(int) + 1) * sizeof(int) +
                        param->vsize;

    int status = (*handle)->command(handle,
                                   EFFECT_CMD_SET_PARAM,
                                   sizeof (effect_param_t) + psize,
                                   param,
                                   &size,
                                   &param->status);
    if (status == 0)
        status = param->status;

    return status;
}

static int set_preprocessor_echo_delay(effect_handle_t handle,
                                     int32_t delay_us)
{
    uint32_t buf[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
    effect_param_t *param = (effect_param_t *)buf;

    param->psize = sizeof(uint32_t);
    param->vsize = sizeof(uint32_t);
    *(uint32_t *)param->data = AEC_PARAM_ECHO_DELAY;
    *((int32_t *)param->data + 1) = delay_us;

    return set_preprocessor_param(handle, param);
}

static void push_echo_reference(struct sun4i_stream_in *in, size_t frames)
{
    /* read frames from echo reference buffer and update echo delay
     * in->ref_frames_in is updated with frames available in in->ref_buf */
    int32_t delay_us = update_echo_reference(in, frames)/1000;
    int i;
    audio_buffer_t buf;

    if (in->ref_frames_in < frames)
        frames = in->ref_frames_in;

    buf.frameCount = frames;
    buf.raw = in->ref_buf;

    for (i = 0; i < in->num_preprocessors; i++) {
        if ((*in->preprocessors[i])->process_reverse == NULL)
            continue;

        (*in->preprocessors[i])->process_reverse(in->preprocessors[i],
                                               &buf,
                                               NULL);
        set_preprocessor_echo_delay(in->preprocessors[i], delay_us);
    }

    in->ref_frames_in -= buf.frameCount;
    if (in->ref_frames_in) {
        memcpy(in->ref_buf,
               in->ref_buf + buf.frameCount * in->config.channels,
               in->ref_frames_in * in->config.channels * sizeof(int16_t));
    }
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct sun4i_stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct sun4i_stream_in *)((char *)buffer_provider -
                                   offsetof(struct sun4i_stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

//	LOGV("get_next_buffer: in->config.period_size: %d, audio_stream_frame_size: %d", 
//		in->config.period_size, audio_stream_frame_size(&in->stream.common));
    if (in->frames_in == 0) {
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   in->config.period_size *
                                       audio_stream_frame_size(&in->stream.common));
        if (in->read_status != 0) {
            LOGE("get_next_buffer() pcm_read error %d, %s", in->read_status, strerror(errno));
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->frames_in = in->config.period_size;
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer + (in->config.period_size - in->frames_in) *
                                                in->config.channels;

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct sun4i_stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct sun4i_stream_in *)((char *)buffer_provider -
                                   offsetof(struct sun4i_stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct sun4i_stream_in *in, void *buffer, ssize_t frames)
{
	// F_LOG;
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer +
                            frames_wr * audio_stream_frame_size(&in->stream.common)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                    { raw : NULL, },
                    frame_count : frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                           frames_wr * audio_stream_frame_size(&in->stream.common),
                        buf.raw,
                        buf.frame_count * audio_stream_frame_size(&in->stream.common));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}

/* process_frames() reads frames from kernel driver (via read_frames()),
 * calls the active audio pre processings and output the number of frames requested
 * to the buffer specified */
static ssize_t process_frames(struct sun4i_stream_in *in, void* buffer, ssize_t frames)
{
	// F_LOG;
    ssize_t frames_wr = 0;
    audio_buffer_t in_buf;
    audio_buffer_t out_buf;
    int i;

    while (frames_wr < frames) {
        /* first reload enough frames at the end of process input buffer */
        if (in->proc_frames_in < (size_t)frames) {
            ssize_t frames_rd;

            if (in->proc_buf_size < (size_t)frames) {
                in->proc_buf_size = (size_t)frames;
                in->proc_buf = (int16_t *)realloc(in->proc_buf,
                                         in->proc_buf_size *
                                             in->config.channels * sizeof(int16_t));
                LOGV("process_frames(): in->proc_buf %p size extended to %d frames",
                     in->proc_buf, in->proc_buf_size);
            }
            frames_rd = read_frames(in,
                                    in->proc_buf +
                                        in->proc_frames_in * in->config.channels,
                                    frames - in->proc_frames_in);
            if (frames_rd < 0) {
                frames_wr = frames_rd;
                break;
            }
            in->proc_frames_in += frames_rd;
        }

        if (in->echo_reference != NULL)
            push_echo_reference(in, in->proc_frames_in);

         /* in_buf.frameCount and out_buf.frameCount indicate respectively
          * the maximum number of frames to be consumed and produced by process() */
        in_buf.frameCount = in->proc_frames_in;
        in_buf.s16 = in->proc_buf;
        out_buf.frameCount = frames - frames_wr;
        out_buf.s16 = (int16_t *)buffer + frames_wr * in->config.channels;

        for (i = 0; i < in->num_preprocessors; i++)
            (*in->preprocessors[i])->process(in->preprocessors[i],
                                               &in_buf,
                                               &out_buf);

        /* process() has updated the number of frames consumed and produced in
         * in_buf.frameCount and out_buf.frameCount respectively
         * move remaining frames to the beginning of in->proc_buf */
        in->proc_frames_in -= in_buf.frameCount;
        if (in->proc_frames_in) {
            memcpy(in->proc_buf,
                   in->proc_buf + in_buf.frameCount * in->config.channels,
                   in->proc_frames_in * in->config.channels * sizeof(int16_t));
        }

        /* if not enough frames were passed to process(), read more and retry. */
        if (out_buf.frameCount == 0)
            continue;

        frames_wr += out_buf.frameCount;
    }
    return frames_wr;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
	// F_LOG;
    int ret = 0;
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;
    struct sun4i_audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_frame_size(&stream->common);

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the input stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        ret = start_input_stream(in);
        if (ret == 0)
            in->standby = 0;
    }
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0)
        goto exit;

    if (in->num_preprocessors != 0)
        ret = process_frames(in, buffer, frames_rq);
    else if (in->resampler != NULL)
        ret = read_frames(in, buffer, frames_rq);
    else
        ret = pcm_read(in->pcm, buffer, bytes);

    if (ret > 0)
        ret = 0;

    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               in_get_sample_rate(&stream->common));

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;
    int status;
    effect_descriptor_t desc;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors >= MAX_PREPROCESSORS) {
        status = -ENOSYS;
        goto exit;
    }

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        goto exit;

    in->preprocessors[in->num_preprocessors++] = effect;

    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        in->need_echo_reference = true;
        do_input_standby(in);
    }

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;
    int i;
    int status = -EINVAL;
    bool found = false;
    effect_descriptor_t desc;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors <= 0) {
        status = -ENOSYS;
        goto exit;
    }

    for (i = 0; i < in->num_preprocessors; i++) {
        if (found) {
            in->preprocessors[i - 1] = in->preprocessors[i];
            continue;
        }
        if (in->preprocessors[i] == effect) {
            in->preprocessors[i] = NULL;
            status = 0;
            found = true;
        }
    }

    if (status != 0)
        goto exit;

    in->num_preprocessors--;

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        goto exit;
    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        in->need_echo_reference = false;
        do_input_standby(in);
    }

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}


static int adev_open_output_stream(struct audio_hw_device *dev,
                                   uint32_t devices, int *format,
                                   uint32_t *channels, uint32_t *sample_rate,
                                   struct audio_stream_out **stream_out)
{
    struct sun4i_audio_device *ladev = (struct sun4i_audio_device *)dev;
    struct sun4i_stream_out *out;
    int ret;

    out = (struct sun4i_stream_out *)calloc(1, sizeof(struct sun4i_stream_out));
    if (!out)
        return -ENOMEM;

    ret = create_resampler(DEFAULT_OUT_SAMPLING_RATE,
                           MM_48000_SAMPLING_RATE,
                           2,
                           RESAMPLER_QUALITY_DEFAULT,
                           NULL,
                           &out->resampler);
    if (ret != 0)
        goto err_open;
    out->buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;

    out->config = pcm_config_mm;

    out->dev = ladev;
    out->standby = 1;

    /* FIXME: when we support multiple output devices, we will want to
     * do the following:
     * adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
     * adev->devices |= out->device;
     * select_output_device(adev);
     * This is because out_set_parameters() with a route is not
     * guaranteed to be called after an output stream is opened. */

    *format = out_get_format(&out->stream.common);
    *channels = out_get_channels(&out->stream.common);
    *sample_rate = out_get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct sun4i_stream_out *out = (struct sun4i_stream_out *)stream;

    out_standby(&stream->common);
    
    if (out->buffer)
        free(out->buffer);
    if (out->resampler)
        release_resampler(out->resampler);
    
    free(stream);
    return;
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct sun4i_audio_device *adev = (struct sun4i_audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret;

    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_TTY_MODE, value, sizeof(value));
    if (ret >= 0) {
        int tty_mode;

        if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_OFF) == 0)
            tty_mode = TTY_MODE_OFF;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_VCO) == 0)
            tty_mode = TTY_MODE_VCO;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_HCO) == 0)
            tty_mode = TTY_MODE_HCO;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_FULL) == 0)
            tty_mode = TTY_MODE_FULL;
        else
            return -EINVAL;

        pthread_mutex_lock(&adev->lock);
        if (tty_mode != adev->tty_mode) {
            adev->tty_mode = tty_mode;
            if (adev->mode == AUDIO_MODE_IN_CALL)
                select_output_device(adev);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_BT_NREC, value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->bluetooth_nrec = true;
        else
            adev->bluetooth_nrec = false;
    }
#if 0
    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->low_power = false;
        else
            adev->low_power = true;
    }
#endif
    str_parms_destroy(parms);
    return ret;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    struct sun4i_audio_device *adev = (struct sun4i_audio_device *)dev;

    adev->voice_volume = volume;
#ifdef __ENABLE_RIL
    if (adev->mode == AUDIO_MODE_IN_CALL)
        ril_set_call_volume(&adev->ril, SOUND_TYPE_VOICE, volume);
#endif
    return 0;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, int mode)
{
    struct sun4i_audio_device *adev = (struct sun4i_audio_device *)dev;

    pthread_mutex_lock(&adev->lock);
    if (adev->mode != mode) {
        adev->mode = mode;
        select_mode(adev);
    }
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct sun4i_audio_device *adev = (struct sun4i_audio_device *)dev;

    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct sun4i_audio_device *adev = (struct sun4i_audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         uint32_t sample_rate, int format,
                                         int channel_count)
{
    size_t size;

    if (check_input_parameters(sample_rate, format, channel_count) != 0)
        return 0;

    return get_input_buffer_size(sample_rate, format, channel_count);
}

static int adev_open_input_stream(struct audio_hw_device *dev, uint32_t devices,
                                  int *format, uint32_t *channel_mask,
                                  uint32_t *sample_rate,
                                  audio_in_acoustics_t acoustics,
                                  struct audio_stream_in **stream_in)
{
    struct sun4i_audio_device *ladev = (struct sun4i_audio_device *)dev;
    struct sun4i_stream_in *in;
    int ret;
    int channel_count = popcount(*channel_mask);

    if (check_input_parameters(*sample_rate, *format, channel_count) != 0)
        return -EINVAL;

    in = (struct sun4i_stream_in *)calloc(1, sizeof(struct sun4i_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->requested_rate = *sample_rate;

    memcpy(&in->config, &pcm_config_mm_ul, sizeof(pcm_config_mm_ul));
    in->config.channels = channel_count;

	LOGD("to malloc in-buffer: period_size: %d, frame_size: %d", 
		in->config.period_size, audio_stream_frame_size(&in->stream.common));
    in->buffer = malloc(in->config.period_size *
                        audio_stream_frame_size(&in->stream.common) * 8);

    if (!in->buffer) {
        ret = -ENOMEM;
        goto err;
    }

    if (in->requested_rate != in->config.rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ret = create_resampler(in->config.rate,
                               in->requested_rate,
                               in->config.channels,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        if (ret != 0) {
            ret = -EINVAL;
            goto err;
        }
    }

    in->dev = ladev;
    in->standby = 1;
    in->device = devices;

    *stream_in = &in->stream;
    return 0;

err:
    if (in->resampler)
        release_resampler(in->resampler);

    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct sun4i_stream_in *in = (struct sun4i_stream_in *)stream;

    in_standby(&stream->common);

	if (in->buffer) {
        free(in->buffer);
		in->buffer = 0;
	}
    if (in->resampler) {
        release_resampler(in->resampler);
    }
    
    free(stream);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct sun4i_audio_device *adev = (struct sun4i_audio_device *)device;
#ifdef __ENABLE_RIL
    /* RIL */
    ril_close(&adev->ril);
#endif
    mixer_close(adev->mixer);
    free(device);
    return 0;
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *dev)
{
    return (/* OUT */
            AUDIO_DEVICE_OUT_EARPIECE |
            AUDIO_DEVICE_OUT_SPEAKER |
            AUDIO_DEVICE_OUT_WIRED_HEADSET |
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
            AUDIO_DEVICE_OUT_AUX_DIGITAL |
            AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET |
            AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET |
            AUDIO_DEVICE_OUT_ALL_SCO |
            AUDIO_DEVICE_OUT_DEFAULT |
            /* IN */
            AUDIO_DEVICE_IN_COMMUNICATION |
            AUDIO_DEVICE_IN_AMBIENT |
            AUDIO_DEVICE_IN_BUILTIN_MIC |
            AUDIO_DEVICE_IN_WIRED_HEADSET |
            AUDIO_DEVICE_IN_AUX_DIGITAL |
            AUDIO_DEVICE_IN_BACK_MIC |
            AUDIO_DEVICE_IN_ALL_SCO |
            AUDIO_DEVICE_IN_DEFAULT);
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct sun4i_audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct sun4i_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = 0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.get_supported_devices = adev_get_supported_devices;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    /* init mixer controls for A10 */
    adev->mixer = mixer_open(CARD_DEFAULT);
    if (!adev->mixer) {
        free(adev);
        LOGE("Unable to open the mixer, aborting.");
        return -EINVAL;
    }
    adev->mixer_ctls.master_vol = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_MASTER_PLAYBACK_VOLUME);
    adev->mixer_ctls.playback_sw = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_PLAYBACK_SWITCH);
    adev->mixer_ctls.capture_vol = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_CAPTURE_VOLUME);
    adev->mixer_ctls.fm_vol = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_FM_VOLUME);
    adev->mixer_ctls.line_vol = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_LINE_VOLUME);
    adev->mixer_ctls.micl_vol = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_MICL_VOLUME);
    adev->mixer_ctls.micr_vol = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_MICR_VOLUME);
    adev->mixer_ctls.fml_sw = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_FML_SWITCH);
    adev->mixer_ctls.fmr_sw = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_FMR_SWITCH);
    adev->mixer_ctls.linel_sw = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_LINEL_SWITCH);
    adev->mixer_ctls.liner_sw = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_LINER_SWITCH);
    adev->mixer_ctls.ldac_left = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_LDAC_LEFT_MIXER);
    adev->mixer_ctls.rdac_right = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_RDAC_RIGHT_MIXER);
    adev->mixer_ctls.ldac_right = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_LDAC_RIGHT_MIXER);
    adev->mixer_ctls.mic_input_mux = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_MIC_INPUT_MUX);
    adev->mixer_ctls.adc_input_mux = mixer_get_ctl_by_name(adev->mixer,
                                           MIXER_ADC_INPUT_MUX);

    if (!adev->mixer_ctls.master_vol || !adev->mixer_ctls.playback_sw ||
        !adev->mixer_ctls.capture_vol || !adev->mixer_ctls.fm_vol ||
        !adev->mixer_ctls.line_vol || !adev->mixer_ctls.micl_vol ||
        !adev->mixer_ctls.micr_vol || !adev->mixer_ctls.fml_sw ||
        !adev->mixer_ctls.fmr_sw || !adev->mixer_ctls.linel_sw ||
        !adev->mixer_ctls.liner_sw || !adev->mixer_ctls.ldac_left ||
        !adev->mixer_ctls.rdac_right || !adev->mixer_ctls.ldac_right ||
        !adev->mixer_ctls.mic_input_mux || !adev->mixer_ctls.adc_input_mux) {
        mixer_close(adev->mixer);
        free(adev);
        LOGE("Unable to locate all mixer controls, aborting.");
        LOGW("mixer value: %d %d\n %d %d\n %d %d\n %d %d\n %d %d\n %d %d\n %d %d\n %d %d\n",
        !adev->mixer_ctls.master_vol , !adev->mixer_ctls.playback_sw ,
        !adev->mixer_ctls.capture_vol , !adev->mixer_ctls.fm_vol ,
        !adev->mixer_ctls.line_vol , !adev->mixer_ctls.micl_vol ,
        !adev->mixer_ctls.micr_vol , !adev->mixer_ctls.fml_sw ,
        !adev->mixer_ctls.fmr_sw , !adev->mixer_ctls.linel_sw ,
        !adev->mixer_ctls.liner_sw , !adev->mixer_ctls.ldac_left ,
        !adev->mixer_ctls.rdac_right , !adev->mixer_ctls.ldac_right ,
        !adev->mixer_ctls.mic_input_mux , !adev->mixer_ctls.adc_input_mux);
        
        return -EINVAL;
    }

    /* Set the default route before the PCM stream is opened */
    pthread_mutex_lock(&adev->lock);
    /* Set initial mixer controls for A10 mixer */
    set_route_by_array(adev->mixer, defaults, 1);
    adev->mode = AUDIO_MODE_NORMAL;
    adev->devices = AUDIO_DEVICE_OUT_SPEAKER | AUDIO_DEVICE_IN_BUILTIN_MIC;
    select_output_device(adev);

    adev->pcm_modem_dl = NULL;
    adev->pcm_modem_ul = NULL;
    adev->voice_volume = 1.0f;
    adev->tty_mode = TTY_MODE_OFF;
    adev->device_is_a10 = is_device_a10();
    adev->bluetooth_nrec = true;
    adev->wb_amr = 0;

    /* RIL */
#ifdef __ENABLE_RIL
    ril_open(&adev->ril);
#endif
    pthread_mutex_unlock(&adev->lock);

#ifdef __ENABLE_RIL
    /* register callback for wideband AMR setting */
    ril_register_set_wb_amr_callback(audio_set_wb_amr_callback, (void *)adev);
#endif

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Sunxi audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
