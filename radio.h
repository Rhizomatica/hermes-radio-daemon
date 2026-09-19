/* hermes-radio-daemon - generic radio control daemon
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef RADIO_H_
#define RADIO_H_

#define CFG_RADIO_PATH "/etc/hermes/core.ini"
#define CFG_USER_PATH "/etc/hermes/user.ini"
#define CFG_WEBSOCKET_PATH "/etc/hermes/web"
#define CFG_SSL_CERT "/etc/ssl/certs/hermes.radio.crt"
#define CFG_SSL_KEY "/etc/ssl/private/hermes.radio.key"

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include <iniparser.h>

/* Radio mode definitions. SHM and websocket carry these as integer indices. */
#define MODE_LSB  0
#define MODE_USB  1
#define MODE_CW   2
#define MODE_FM   3
#define MODE_AM   4
#define MODE_DRM  5
#define MODE_FT8  6
#define MODE_RTTY 7
#define MODE_DSTAR 8

/* Per-profile "operating mode". For the hfsignals backend it selects the
 * ALSA/DSP signal path. For the hamlib backend it picks voice vs data SSB on
 * the rig: FULL_VOICE → USB/LSB; anything else → PKTUSB/PKTLSB (DATA-U/L). */
#define OPERATING_MODE_FULL_VOICE     0 /* IO+ALSA+DSP, TX from MIC */
#define OPERATING_MODE_FULL_LOOPBACK  1 /* IO+ALSA+DSP, TX from ALSA loopback */
#define OPERATING_MODE_CONTROLS_ONLY  2 /* just IO, no ALSA */
#define OPERATING_MODE_EXTERNAL_DSP   3 /* IO+ALSA, signal to external DSP */

/* AGC settings */
#define AGC_OFF    0
#define AGC_SLOW   1
#define AGC_MEDIUM 2
#define AGC_FAST   3

/* Compressor settings */
#define COMPRESSOR_OFF 0
#define COMPRESSOR_ON  1

/* Pre-emphasis (TX) and noise reduction (RX) toggles */
#define TX_PREEMPHASIS_OFF  0
#define TX_PREEMPHASIS_ON   1
#define NOISE_REDUCTION_OFF 0
#define NOISE_REDUCTION_ON  1

/* TX/RX states */
#define IN_RX 0
#define IN_TX 1

/* Encoder rotation speed (sbitx hardware only) */
#define ENC_FAST 1
#define ENC_SLOW 5

/* SWR protection: ~400 ms before pulling TX on sustained high SWR */
#define REF_PEAK_REMOVAL 10

/* sbitx GPIO pin numbers — hardware-internal but referenced by both
 * sbitx_gpio.c and the hfsignals backend init code. */
#define ENC1_A    9
#define ENC1_B   10
#define ENC1_SW  11
#define ENC2_A   17
#define ENC2_B   27
#define ENC2_SW  22

#define PTT       4
#define DASH      5
#define TX_LINE  23
#define TX_POWER 16
#define LPF_A    24
#define LPF_B    25
#define LPF_C     8
#define LPF_D     7

#define ZBITX_RX_LINE 15
#define ZBITX_LPF_E   12

#define SBITX_BFO_FREQUENCY 40035000U
#define ZBITX_BFO_FREQUENCY 40048000U

/* Maximum number of radio profiles. SHM wire format packs profile in the
 * upper 2 bits of cmd[4] (radio_cmds.h), so SHM clients can only address
 * profiles 0..3 directly in per-profile commands. Profiles 4..8 are
 * reachable via CMD_SET_PROFILE and via the websocket API. */
#define MAX_RADIO_PROFILES 9
#define MAX_CAL_BANDS      16

#define AUDIO_DEVICE_NAME_MAX 256
#define WEBSOCKET_URL_MAX     256
#define RECORDING_PATH_MAX    512
#define WATERFALL_BINS        128
#define RADIO_MESSAGE_MAX     128
#define CONFIG_PATH_MAX       512
#define BACKEND_PATH_MAX      512

#define DIGI_TX_QUEUE_DEPTH 16
#define DIGI_TX_MSG_MAX     256

typedef struct {
    int16_t *samples;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} audio_ring_buffer;

typedef struct {
    FILE *fp;
    char path[RECORDING_PATH_MAX];
    uint32_t sample_rate;
    uint32_t data_bytes;
    pthread_mutex_t mutex;
    bool active;
} wav_recording;

/* Encoder state (sbitx hardware front panel only) */
typedef struct {
    int pin_a, pin_b;
    int speed;
    int prev_state;
    int history;
} encoder;

/* Per-band TX power calibration (sbitx hardware only) */
typedef struct {
    int    f_start;
    int    f_stop;
    double scale;
} power_settings;

/* Hardware variant (sbitx vs zbitx — different GPIO + BFO) */
typedef enum {
    HW_PROFILE_UNKNOWN = 0,
    HW_PROFILE_SBITX,
    HW_PROFILE_ZBITX,
} hw_profile_t;

/* PTT type values (mirrors Hamlib ptt_type_t values) */
#define PTT_NONE        0
#define PTT_RIG         1
#define PTT_SERIAL_RTS  2
#define PTT_SERIAL_DTR  3
#define PTT_PARALLEL    4
#define PTT_CM108       5
#define PTT_GPIO        6
#define PTT_RIG_MICDATA 7

typedef enum {
    RADIO_BACKEND_HAMLIB    = 0,
    RADIO_BACKEND_HFSIGNALS = 1,
} radio_backend_kind;

struct radio_backend_ops;
struct radio_pipeline_descriptor;

/* Outbound digi-mode text queue. Filled by the websocket digi_send handler;
 * drained by the FT8/CW/RTTY TX paths. When empty, the digi TX path stays
 * idle (PTT off). */
typedef struct {
    char msgs[DIGI_TX_QUEUE_DEPTH][DIGI_TX_MSG_MAX];
    int  head;
    int  tail;
    int  count;
    pthread_mutex_t mutex;
} digi_tx_queue;

/* Per-profile radio configuration */
typedef struct {
    _Atomic uint32_t freq;

    /* sbitx ALSA path: full / loopback / controls-only / external. On the
     * hamlib backend it picks voice vs data SSB (USB↔PKTUSB, LSB↔PKTLSB). */
    uint16_t operating_mode;

    _Atomic uint16_t mode;                   /* MODE_* */
    _Atomic uint16_t agc;                    /* AGC_* */
    _Atomic uint16_t compressor;             /* COMPRESSOR_* */

    /* ALSA mixer levels (0..100). Used by sbitx ALSA path. */
    _Atomic uint32_t mic_level;
    _Atomic uint32_t rx_level;
    _Atomic uint32_t speaker_level;
    _Atomic uint32_t tx_level;

    /* DSP band-pass filter (Hz). Read once at profile load by sbitx DSP. */
    uint32_t bpf_low;
    uint32_t bpf_high;

    /* Receiver filter passband in Hz, 0 = rig default ("normal"). On the
     * hamlib backend this is the width argument of rig_set_mode; on the
     * hfsignals backend it is bpf_high - bpf_low. Kept per profile so a
     * profile switch restores the operator's filter along with the mode. */
    _Atomic uint32_t filter_width;

    _Atomic uint16_t power_level_percentage; /* 0..100 */

    /* Front-panel knob/PTT enables (sbitx hardware) */
    bool enable_knob_volume;
    bool enable_knob_frequency;
    bool enable_ptt;

    _Atomic uint16_t tx_preemphasis;         /* TX_PREEMPHASIS_* */
    _Atomic uint16_t noise_reduction;        /* NOISE_REDUCTION_* */

    _Atomic bool digital_voice;
} radio_profile;

/* Main radio handle. One instance per process; passed to every backend
 * vtable function. Backend-specific fields (rig handle, encoders, hw GPIO
 * mutex) coexist; each backend uses what it needs. */
typedef struct {
    /* Selected radio backend */
    radio_backend_kind backend_kind;
    const struct radio_backend_ops *backend_ops;
    _Atomic(const struct radio_pipeline_descriptor *) pipeline;

    /* ── hamlib backend ──────────────────────────────────────────── */
    int  hamlib_model;
    char rig_pathname[256];
    int  serial_rate;
    int  ptt_type;
    char ptt_pathname[256];
    void *rig;                        /* hamlib RIG* (opaque) */

    /* ── sbitx hardware backend ──────────────────────────────────── */
    char i2c_device[64];
    char dream_path[256];
    int  i2c_bus;
    pthread_mutex_t i2c_mutex;
    pthread_mutex_t gpio_mutex;
    hw_profile_t hw_profile;
    encoder enc_a;
    encoder enc_b;
    _Atomic int32_t  volume_ticks;
    _Atomic int32_t  tuning_ticks;
    _Atomic uint32_t knob_a_pressed;
    _Atomic uint32_t knob_b_pressed;
    _Atomic bool key_down;            /* PTT button */
    _Atomic bool dash_down;
    _Atomic uint32_t bridge_compensation;
    power_settings band_power[MAX_CAL_BANDS];
    uint32_t band_power_count;
    _Atomic bool send_ws_update;      /* embedded DSP signals new state */

    /* ── shared status ───────────────────────────────────────────── */
    _Atomic bool     txrx_state;              /* IN_RX or IN_TX */
    _Atomic uint32_t reflected_threshold;     /* vswr * 10 */
    _Atomic bool     swr_protection_enabled;
    _Atomic bool     tone_generation;

    pthread_mutex_t  message_mutex;
    char             message[RADIO_MESSAGE_MAX];
    _Atomic bool     message_available;

    /* Power measurements (raw 10-bit ADC for sbitx, watts*10 for hamlib) */
    _Atomic uint32_t fwd_power;
    _Atomic uint32_t ref_power;

    /* RX signal strength (S-meter), dB relative to S9 (S9 = 0): S0 ~ -54,
     * each S-unit 6 dB, positive = over S9. -200 = no reading yet. */
    _Atomic int32_t  s_meter_db;

    /* Informational */
    _Atomic uint32_t serial_number;
    _Atomic bool     system_is_connected;     /* VARA / modem connection status */
    _Atomic bool     system_is_ok;            /* uucp / system health */

    /* Modem statistics (written by external modem app) */
    _Atomic uint32_t bitrate;
    _Atomic int32_t  snr;
    _Atomic uint32_t bytes_transmitted;
    _Atomic uint32_t bytes_received;

    /* Tuning step (Hz) */
    _Atomic uint32_t step_size;

    /* sbitx local oscillator (BFO) frequency. Hamlib backend stores for
     * config compatibility but doesn't apply. */
    _Atomic uint32_t bfo_frequency;

    /* ── interface enables ───────────────────────────────────────── */
    _Atomic bool enable_shm_control;
    _Atomic bool enable_websocket;
    bool rig_server_enable;
    int rig_server_port;
    /* Native-CAT gateway (cat_server.c): a TCP port speaking the radio's own
     * CAT dialect, for Windows loggers that can only open a COM port. */
    bool cat_server_enable;
    int  cat_server_port;
    int  cat_server_mode;                    /* cat_server_mode enum */
    char cat_server_bind[64];
    /* How long a passthrough transaction waits for the rig to start
     * answering. A command that draws no reply (most "set" commands) costs
     * exactly this much, so it trades logger responsiveness against the risk
     * of missing a slow rig's answer. */
    int  cat_reply_timeout_ms;
    _Atomic bool enable_audio_bridge;
    _Atomic bool enable_shm_audio;   /* bridge codec audio to mercury via POSIX SHM */

    /* Half-duplex codec arbitration. Some rigs (e.g. FT-710) expose their CAT
     * serial and audio codec behind one shared full-speed USB hub; running
     * capture + playback isoc streams at once there overloads the hub and the
     * codec drops out (-ENODEV / USB reset). When set, only one direction holds
     * the codec at a time, switched by txrx_state (HF is half-duplex anyway).
     * The two _holds_codec atomics serialise the RX/TX handoff. */
    _Atomic bool audio_half_duplex;
    _Atomic bool media_capture_holds_codec;
    _Atomic bool media_playback_holds_codec;

    /* Pointer to the backend's CAT-serialisation mutex (the hamlib serial lock),
     * or NULL. The media threads take it around codec open/close/recover so a USB
     * altsetting reconfiguration never races a CAT transaction. On the FT-710 the
     * C-Media codec and the CP2105 CAT serial sit behind ONE full-speed USB hub;
     * a codec reconfig concurrent with serial I/O resets the device. Steady isoc
     * streaming + CAT coexists fine (mercury proves it) — only reconfiguration
     * must be serialised, so this lock is held only briefly, never while streaming. */
    pthread_mutex_t *cat_bus_lock;

    char websocket_url[WEBSOCKET_URL_MAX];   /* full ws:// or wss:// URL */

    char capture_device[AUDIO_DEVICE_NAME_MAX];
    char playback_device[AUDIO_DEVICE_NAME_MAX];
    /* snd-aloop modem bridge (loop_audio): the daemon mirrors codec audio to/from
     * these loopback devices at native rate so mercury (-x alsa) reads/writes the
     * other ends. loop_playback = RX to modem (e.g. hw:1,0); loop_capture = TX
     * from modem (e.g. hw:2,1). Empty -> disabled. */
    _Atomic bool enable_loop_audio;
    char loop_capture_device[AUDIO_DEVICE_NAME_MAX];
    char loop_playback_device[AUDIO_DEVICE_NAME_MAX];
    /* Optional operator-side headset (hardware) audio path. When both
     * device names are non-empty, the daemon spawns capture+playback threads
     * that mirror the websocket browser audio path onto a local ALSA device. */
    char headset_capture_device[AUDIO_DEVICE_NAME_MAX];
    char headset_playback_device[AUDIO_DEVICE_NAME_MAX];
    _Atomic uint32_t headset_sample_rate;
    /* audio_sample_rate: rate the daemon audio rings carry (consumed by
     * recording, spectrum FFT and the websocket binary frames). */
    _Atomic uint32_t audio_sample_rate;
    /* rig_audio_rate: native rate of the rig-facing ALSA device. Set to 0
     * to mean "same as audio_sample_rate" (back-compat). When different, the
     * audio_bridge resamples between rig native and ring rate. */
    _Atomic uint32_t rig_audio_rate;
    _Atomic uint32_t audio_period_size;
    _Atomic uint32_t audio_queue_samples;

    /* ── digital-mode config ─────────────────────────────────────── */
    _Atomic uint16_t cw_wpm;
    _Atomic uint16_t cw_pitch;
    _Atomic uint16_t rtty_baud;
    _Atomic uint16_t rtty_mark;
    _Atomic uint16_t rtty_shift;

    /* D-STAR DV: FM deviation of the GMSK modulator in Hz (default 1200,
     * the MSK h=0.5 peak deviation for 4800 baud) and TX/RX drive gains.
     * The TX gain maps the modem baseband (±0.0257) onto the FM
     * modulator's deviation. dstar_rx_polarity flips the discriminator
     * sign for rigs that present it inverted (1 normal, -1 inverted). */
    _Atomic uint16_t dstar_deviation;
    float dstar_tx_gain;
    float dstar_rx_gain;
    float dstar_rx_polarity;
    char  dstar_mycall[16];
    char  dstar_urcall[16];
    /* Log D-STAR RX frames/sync to stderr (0 off, 1 on) for bench debugging. */
    _Atomic uint16_t dstar_verbose;
    /* Sample-clock correction for the D-STAR RX chain, in ppm.
     *
     * The rig's 4800 baud symbol clock and the sbitx codec's sampling clock
     * are independent crystals. On this hardware the codec runs ~+300 ppm
     * fast, which walks the symbol phase ~3 samples per 420 ms superframe —
     * and a symbol is only 5 samples at 24 kHz, so the sampling point drifts
     * through the eye in about a second. The modem slices at a fixed phase,
     * so that shows up as a few bit errors per frame: sync holds, the header
     * FEC survives, and the AMBE payload comes out as garble.
     *
     * Positive values speed the decimated stream up. Measure it by finding
     * the baud line in the discriminator stream (it should sit at exactly
     * 4800 Hz) and negating the offset. */
    _Atomic int32_t dstar_clock_ppm;
    /* Audio gain applied to the DECODED D-STAR voice on its way to the
     * speaker and the websocket. Kept separate from dstar_rx_gain, which
     * scales the discriminator signal feeding the modem: one is a listening
     * level, the other is a demodulator input level, and they had been
     * sharing a single knob. */
    float dstar_af_gain;
    /* Run-time tunable: enable the specbleach denoise front-end on the
     * D-STAR TX mic path (1 on, 0 off). */
    _Atomic uint16_t dstar_denoise;

    /* Outbound text queue for FT8/CW/RTTY (filled by digi_send) */
    digi_tx_queue digi_tx;

    /* TX drive gain applied to the digi-mode audio (CW/FT8/RTTY/RADAE)
     * before it reaches the rig. 1.0 = encoder native level; raise to push
     * more audio into the rig USB codec. Clipped at full scale on push. */
    float digi_tx_gain;

    char recording_dir[RECORDING_PATH_MAX];

    /* ── profile management ──────────────────────────────────────── */
    _Atomic uint32_t profile_active_idx;
    _Atomic int32_t  profile_timeout;        /* -1 = disabled */
    _Atomic uint32_t profile_default_idx;
    _Atomic uint32_t profiles_count;
    radio_profile    profiles[MAX_RADIO_PROFILES];

    /* ── configuration ───────────────────────────────────────────── */
    pthread_mutex_t cfg_mutex;
    dictionary     *cfg_radio;
    dictionary     *cfg_user;
    char            cfg_radio_path[CONFIG_PATH_MAX];
    char            cfg_user_path[CONFIG_PATH_MAX];
    _Atomic bool    cfg_radio_dirty;
    _Atomic bool    cfg_user_dirty;

    /* ── media: rings, recording, spectrum ───────────────────────── */
    audio_ring_buffer rx_audio_ring;
    audio_ring_buffer tx_audio_ring;
    /* RADAE bypass rings (digital voice). When `digital_voice` is active
     * on a backend that doesn't do its own SDR IF processing (hamlib),
     * the RADAE pump consumes tx_audio_ring (browser speech) and produces
     * the modem-real audio into tx_radae_ring, which the rig playback
     * pump then drains in place of tx_audio_ring. RX side is symmetric:
     * capture writes raw rig audio to rx_audio_ring; the RADAE pump
     * consumes that, decodes speech, and writes to rx_radae_ring for the
     * websocket to broadcast. */
    audio_ring_buffer rx_radae_ring;
    audio_ring_buffer tx_radae_ring;
    /* D-STAR bypass rings (same pattern as RADAE): the hamlib D-STAR
     * pump encodes tx_audio_ring speech into GMSK modem audio in
     * tx_dstar_ring (drained by playback in place of tx_audio_ring),
     * and decodes rig audio from rx_audio_ring into rx_dstar_ring for
     * the websocket to broadcast. */
    audio_ring_buffer rx_dstar_ring;
    audio_ring_buffer tx_dstar_ring;
    wav_recording     rx_recording;
    wav_recording     tx_recording;

    pthread_mutex_t spectrum_mutex;
    float           rx_spectrum[WATERFALL_BINS];
    float           tx_spectrum[WATERFALL_BINS];
    _Atomic uint32_t rx_spectrum_seq;
    _Atomic uint32_t tx_spectrum_seq;
    _Atomic bool     rx_spectrum_valid;
    _Atomic bool     tx_spectrum_valid;
    _Atomic uint32_t spectrum_sample_rate;

    /* Last D-STAR header decoded on receive, for display by the web panel and
     * any other client. Written by the DSP's header callback (once per over,
     * or once per superframe when it is reassembled from slow data), read by
     * build_status_json().
     *
     * Deliberately appended at the END of this struct: it lives in shared
     * memory, so a client built against an older layout keeps working as long
     * as nothing before it moves.
     *
     * No lock. Each field is written as a whole fixed-size callsign and the
     * reader only displays it, so the worst a concurrent read can show is one
     * field from the previous over next to one from the current. dstar_rx_heard
     * counts headers so a client can tell a fresh decode from a stale one. */
    char             dstar_rx_mycall[9];
    char             dstar_rx_urcall[9];
    char             dstar_rx_rpt1[9];
    char             dstar_rx_rpt2[9];
    char             dstar_rx_suffix[5];
    _Atomic uint32_t dstar_rx_heard;
} radio;

/* digi_tx_queue helpers (defined in cfg_utils.c or radio_websocket.c). */
void digi_tx_queue_init(digi_tx_queue *q);
void digi_tx_queue_destroy(digi_tx_queue *q);
bool digi_tx_queue_push(digi_tx_queue *q, const char *text);
bool digi_tx_queue_pop(digi_tx_queue *q, char *out, size_t out_len);
bool digi_tx_queue_pending(digi_tx_queue *q);   /* true if any text is queued */

#endif /* RADIO_H_ */
