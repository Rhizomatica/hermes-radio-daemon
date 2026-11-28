/* ale_engine: the ALE 2G (MIL-STD-188-141B App. A) receiver, as a C ABI over
 * PC-ALE (vendor/pc-ale).  The only C++ in radiod (modem/ale_engine.cpp).
 *
 * Two receivers run on the same 8 kHz audio:
 *  - the stock one: PC-ALE's per-word Demodulator, as a standard ALE radio;
 *  - a combining one: the tone energies of K repeated words are summed
 *    before the ordinary decode (docs/ALE-DESIGN.md 14.5), for each K given
 *    at creation.  It finds the symbol phase and the word start itself, and
 *    accepts a word only when the same word is decoded again one word period
 *    later on the same grid.
 * A word is reported once per run of repeats (5 s without it starts a new run).
 *
 * Not thread-safe: one thread feeds an engine; the callback runs on it. */

#ifndef ALE_ENGINE_H
#define ALE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALE_ENGINE_RATE       8000  /* Hz, mono int16 in and out */
#define ALE_ENGINE_MAX_K      16    /* most words combined */
#define ALE_ENGINE_MAX_KS     4     /* most K values at once */
#define ALE_ENGINE_WORD_SAMPLES 3136 /* one word: 49 symbols of 64 samples (392 ms) */

/* Preambles (the word's 3 top bits) */
#define ALE_PREAMBLE_DATA  0
#define ALE_PREAMBLE_THRU  1
#define ALE_PREAMBLE_TO    2
#define ALE_PREAMBLE_TWAS  3
#define ALE_PREAMBLE_FROM  4
#define ALE_PREAMBLE_TIS   5
#define ALE_PREAMBLE_CMD   6
#define ALE_PREAMBLE_REP   7

typedef struct
{
    uint64_t end_sample;   /* stream position just after the word (or the last word combined) */
    uint8_t  combined;     /* 0: the stock receiver; else the number of words combined */
    uint8_t  preamble;     /* ALE_PREAMBLE_* */
    char     text[4];      /* the 3 characters, NUL-terminated */
    uint8_t  unanimous;    /* unanimous votes of the 2/3 majority voter, 0..48 */
    uint8_t  fec_errors;   /* Golay errors corrected */
    float    tone_snr_db;  /* combining: mean ratio of the winning tone to the other 7,
                            * over the 49 symbols; stock: 0 */
} ale_word_event;

typedef void (*ale_word_cb)(const ale_word_event *ev, void *user);

typedef struct ale_engine ale_engine;

/* ks: the K values of the combining receiver (2..ALE_ENGINE_MAX_K, at most
 * ALE_ENGINE_MAX_KS of them; nk 0 runs only the stock receiver).  NULL on a
 * bad argument or no memory. */
ale_engine *ale_engine_create(const uint8_t *ks, size_t nk, ale_word_cb cb, void *user);
void        ale_engine_destroy(ale_engine *e);

/* Run the stock receiver too (on at creation).  It costs most of the
 * engine's CPU: PC-ALE's demodulator attempts a decode every 16 samples. */
void        ale_engine_set_stock(ale_engine *e, int on);

/* Feed n samples; words decoded meanwhile go to the callback. */
void        ale_engine_feed(ale_engine *e, const int16_t *pcm, size_t n);

/* Samples fed so far. */
uint64_t    ale_engine_position(const ale_engine *e);

const char *ale_preamble_name(uint8_t preamble);

/* ── The link layer: PC-ALE's ALEController ──────────────────────────────
 *
 * Scanning, calls and the three-way handshake, AMD, sounding and LQA.  The
 * engine's receivers feed it: every word of the stock receiver, and a
 * combining receiver's word when the stock one missed it (at most one a word
 * period).  The host gives the radio as callbacks and drives the controller
 * with ale_engine_tick from the thread that feeds the engine. */

#define ALE_ADDRESS_MAX   15      /* an ALE address: 1-15 characters of the basic set */
#define ALE_AMD_MAX       90      /* AMD text: at most 90 characters of the expanded set */
#define ALE_MODE_USB      1       /* a channel's sideband, as radio.h's MODE_USB/LSB */
#define ALE_MODE_LSB      0

typedef struct
{
    /* Tune to a channel (not saved); false when the radio cannot. */
    bool     (*retune)(uint32_t hz, uint8_t mode, void *user);
    /* Key (true) or unkey; false when keying is refused. */
    bool     (*key)(bool on, void *user);
    /* Audio to transmit, 8 kHz, after key(true). */
    void     (*tx_audio)(const int16_t *pcm, size_t n, void *user);
    /* Samples of this over that have left for the radio (modem_audio_tx_played). */
    uint64_t (*tx_played)(void *user);
    void     *user;
} ale_radio_ops;

typedef struct
{
    char     self[ALE_ADDRESS_MAX + 1];
    const char *channel_file;     /* PC-ALE's: "rx_hz[:tx_hz] [mode] [label]" a line; NULL: none */
    const char *lqa_file;         /* NULL: LQA kept in memory only */
    uint32_t scan_dwell_ms;
    bool     manual_accept;       /* a call waits for ale_engine_accept (else answered at once) */
    uint32_t accept_timeout_ms;   /* unanswered for this long: dropped, no reply sent */
    bool     auto_sounding;
    /* A transmission ends with this much silence, and its last word counts as
     * sent only once that has left for the radio: the audio still in the
     * rig's sound card, USB codec and TX chain, which no count sees (a
     * Hamlib rig's last word was cut without it). */
    uint32_t tx_tail_ms;
    uint32_t start_hz;            /* the channel the radio is on at the start */
    uint8_t  start_mode;          /* ALE_MODE_* */
} ale_link_config;

enum
{
    ALE_EV_STATE = 1,     /* state changed: text = the new state */
    ALE_EV_CALL,          /* a call for us: peer; text = its AMD, if any */
    ALE_EV_LINKED,        /* link established with peer */
    ALE_EV_TERMINATED,    /* link ended: text = why */
    ALE_EV_AMD,           /* AMD received from peer: text */
    ALE_EV_CHANNEL,       /* tuned to hz */
    ALE_EV_STATUS,        /* the controller's trace: text */
};

typedef struct
{
    uint8_t  kind;                        /* ALE_EV_* */
    char     peer[ALE_ADDRESS_MAX + 1];
    char     text[ALE_AMD_MAX + 1];
    uint32_t hz;
} ale_link_event;

typedef void (*ale_link_cb)(const ale_link_event *ev, void *user);

typedef struct
{
    char     state[16];                   /* IDLE, SCANNING, CALLING, HANDSHAKE, LINKED, SOUNDING, ERROR */
    char     peer[ALE_ADDRESS_MAX + 1];   /* while calling, in a handshake or linked */
    uint32_t hz;                          /* the channel tuned */
    uint8_t  mode;                        /* ALE_MODE_* */
    bool     transmitting;
    uint32_t link_seconds;
    uint8_t  votes;                       /* the last word heard: unanimous votes, 0..48 */
    uint8_t  fec_errors;
    uint16_t channels;                    /* in the channel list */
} ale_link_state;

/* Start the link layer, idle (listening on the radio's channel).  0, or -1
 * on a bad configuration (self address). */
int  ale_engine_link_start(ale_engine *e, const ale_link_config *cfg,
                           const ale_radio_ops *ops, ale_link_cb cb, void *user);
/* Stop it at once (an emergency stop: unkeys); the receivers go on. */
void ale_engine_link_stop(ale_engine *e);
bool ale_engine_link_running(const ale_engine *e);

/* Operator actions; each false when the state does not allow it. */
bool ale_engine_listen(ale_engine *e);            /* idle on the current channel */
bool ale_engine_scan(ale_engine *e);              /* scan the channel list */
bool ale_engine_call(ale_engine *e, const char *to, const char *amd);   /* amd may be NULL */
bool ale_engine_accept(ale_engine *e);
bool ale_engine_reject(ale_engine *e);
bool ale_engine_terminate(ale_engine *e);
bool ale_engine_sound(ale_engine *e);

/* Tune the link layer's radio to a channel (through ale_radio_ops.retune):
 * false while it is not idle or scanning, or the radio cannot. */
bool ale_engine_tune(ale_engine *e, uint32_t hz, uint8_t mode);

/* Drive the controller (timers, handshake) and the transmitter: often,
 * every few ms, from the feeding thread.  now_ms: a monotonic clock. */
void ale_engine_tick(ale_engine *e, uint32_t now_ms);

void ale_engine_link_get_state(ale_engine *e, ale_link_state *out);

/* Whether text is an ALE address (1-15 of A-Z 0-9 @ ?, uppercase), or AMD
 * text (at most 90 of the expanded set). */
bool ale_address_valid(const char *text);
bool ale_amd_valid(const char *text);

/* A scanning call, as a standard ALE radio sends it: `words` repeats of one
 * word (preamble, 3 characters of the basic 38 set) through PC-ALE's
 * modulator, tone generator and TX band-pass, at PC-ALE's TX amplitude.
 * Writes at most max samples; returns the number written, 0 if the word is
 * not valid. */
size_t      ale_engine_make_call(uint8_t preamble, const char *text3, unsigned words,
                                 int16_t *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif
