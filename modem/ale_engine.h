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
