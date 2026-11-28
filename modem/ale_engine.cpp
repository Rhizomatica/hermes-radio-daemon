/* ale_engine: see ale_engine.h.  The combining receiver is the one measured
 * in ALELite's xval/xval_acq.cpp (docs/ALE-DESIGN.md 14.5), made to run on a
 * stream: the same symbol-phase search, word rotations, decode and
 * acceptance rules, and the same confirmation, evaluated as each symbol
 * window completes instead of over a recording. */

#include "ale_engine.h"

#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "Codec/ale_decoder.h"
#include "FSK/tone_generator.h"
#include "FSK/tx_bandpass.h"
#include "Modem/ale2g_modem.h"
#include "Word/ale_word.h"

using namespace ale;

namespace {

constexpr unsigned WORD_SYMS   = SYMBOLS_PER_WORD;        /* 49 */
constexpr unsigned SYM_SAMP    = SAMPLES_PER_SYMBOL;      /* 64 */
constexpr unsigned WORD_SAMP   = WORD_SYMS * SYM_SAMP;    /* 3136 */
static_assert(WORD_SAMP == ALE_ENGINE_WORD_SAMPLES, "word length");
static_assert(SAMPLE_RATE_HZ == ALE_ENGINE_RATE, "sample rate");

/* Symbol timing: windows start every PHASE_STEP samples, so 8 phases of
 * the 64-sample symbol grid are searched (xval_acq). */
constexpr unsigned PHASES      = 8;
constexpr unsigned PHASE_STEP  = SYM_SAMP / PHASES;

/* Acceptance, as the per-word receiver's (A.5.2.6.3): at least this many
 * unanimous votes of the 2/3 majority voter.  Golay at correction power 2:
 * power 3 let wrong words through the confirmation (26 in the xval sweep),
 * power 2 none. */
constexpr uint8_t   MIN_UNANIMOUS = 33;
constexpr GolayMode COMBINED_GOLAY = GolayMode::Mode2_5;

/* Confirmation: an accepted word counts once the same word was accepted
 * again a whole number of word periods later, within this many samples. */
constexpr long     CONFIRM_SLACK  = 16;
/* Accepted words kept for the confirmation, in word periods. */
constexpr unsigned HIT_KEEP_WORDS = 8;
constexpr size_t   HIT_KEEP_MAX   = 256;

/* A word is reported once per run of repeats: again only after this long
 * without it (the stock receiver, or confirmations at that K).  Near the
 * receiver's limit confirmations come seconds apart within one call (on the
 * bench at -14 dB, 2.4 and 3.5 s), so the hold is longer than that, and
 * shorter than the gap between two calls of a scan (a few seconds). */
constexpr unsigned REPORT_HOLD_MS = 5000;
constexpr uint64_t REPORT_HOLD_SAMPLES = (uint64_t) REPORT_HOLD_MS * SAMPLE_RATE_HZ / 1000;
constexpr size_t   REPORT_SLOTS = 16;

/* The stock demodulator is fed in blocks this long, so a word's reported
 * position is within one block of where it was decoded. */
constexpr size_t   STOCK_BLOCK = SYM_SAMP;

/* A word's 24 bits (A.5.2.1): the preamble in the top 3, then three 7-bit
 * characters, first character highest. */
constexpr unsigned PREAMBLE_SHIFT = 21;
constexpr unsigned CHAR_BITS      = 7;

/* Symbols kept per phase: enough for the largest K. */
constexpr unsigned SYM_RING = ALE_ENGINE_MAX_K * WORD_SYMS;

struct Hit { uint32_t raw; uint64_t start; };
struct Report { uint32_t raw; uint64_t last; bool used; };

struct Reporter
{
    Report slot[REPORT_SLOTS] = {};

    /* True when the word should be reported now; notes it either way. */
    bool due(uint32_t raw, uint64_t now)
    {
        Report *free_slot = nullptr, *oldest = &slot[0];
        for (Report &r : slot)
        {
            if (r.used && r.raw == raw)
            {
                bool again = now - r.last > REPORT_HOLD_SAMPLES;
                r.last = now;
                return again;
            }
            if (!r.used && !free_slot)
                free_slot = &r;
            if (r.used && r.last < oldest->last)
                oldest = &r;
        }
        Report *r = free_slot ? free_slot : oldest;
        *r = { raw, now, true };
        return true;
    }
};

struct Combiner
{
    unsigned k = 0;
    std::vector<Hit> hits;
    Reporter reporter;
};

} /* namespace */

struct ale_engine
{
    ale_word_cb cb = nullptr;
    void *user = nullptr;
    uint64_t n = 0;                             /* samples fed */

    /* the last symbol window, twice over so it is always contiguous */
    int16_t x[2 * SYM_SAMP] = {};
    unsigned xpos = 0;

    /* tone energies per phase, per symbol: e[phase][slot][tone] */
    float e[PHASES][SYM_RING][NUM_TONES] = {};
    double cos_t[NUM_TONES][SYM_SAMP], sin_t[NUM_TONES][SYM_SAMP];

    Combiner comb[ALE_ENGINE_MAX_KS];
    size_t ncomb = 0;

    ALE2GModem::Demodulator stock;
    bool stock_on = true;
    Reporter stock_reporter;
    int16_t stock_buf[STOCK_BLOCK] = {};
    size_t stock_fill = 0;
};

static void emit(ale_engine *e, const ALEWord &w, uint8_t combined, float snr_db, uint64_t end)
{
    ale_word_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.end_sample  = end;
    ev.combined    = combined;
    ev.preamble    = (uint8_t) w.type;
    memcpy(ev.text, w.address, 3);
    ev.text[3]     = '\0';
    ev.unanimous   = w.unanimous_votes;
    ev.fec_errors  = w.fec_errors;
    ev.tone_snr_db = snr_db;
    if (e->cb)
        e->cb(&ev, e->user);
}

/* A word of K combined words ends at symbol t of this phase. */
static void try_combined(ale_engine *e, Combiner &c, unsigned phase, uint64_t t)
{
    if (t + 1 < (uint64_t) c.k * WORD_SYMS)
        return;
    const uint64_t first = t + 1 - (uint64_t) c.k * WORD_SYMS;   /* first symbol combined */

    uint8_t sym[WORD_SYMS];
    double ratio_sum = 0;
    for (unsigned s = 0; s < WORD_SYMS; s++)
    {
        float sum[NUM_TONES] = {};
        for (unsigned w = 0; w < c.k; w++)
        {
            const float *en = e->e[phase][(first + (uint64_t) w * WORD_SYMS + s) % SYM_RING];
            for (unsigned v = 0; v < NUM_TONES; v++)
                sum[v] += en[v];
        }
        unsigned best = 0;
        float total = 0;
        for (unsigned v = 0; v < NUM_TONES; v++)
        {
            total += sum[v];
            if (sum[v] > sum[best])
                best = v;
        }
        sym[s] = (uint8_t) best;
        float others = (total - sum[best]) / (NUM_TONES - 1);
        ratio_sum += others > 0 ? sum[best] / others : 0;
    }

    ALEWord out;
    Golay::DecodeResult fec;
    uint8_t unanimous = 0;
    if (!ALEDecoder::decode(sym, out, fec, &unanimous, COMBINED_GOLAY) || !out.valid
        || unanimous < MIN_UNANIMOUS)
        return;
    if (out.type != PreambleType::TO && out.type != PreambleType::TIS
        && out.type != PreambleType::TWAS)
        return;

    const uint32_t raw = ((uint32_t) out.type << PREAMBLE_SHIFT) | out.raw_payload;
    const uint64_t start = (uint64_t) phase * PHASE_STEP + (uint64_t) SYM_SAMP * first;

    /* forget hits too old to confirm anything */
    const uint64_t keep = (uint64_t) HIT_KEEP_WORDS * WORD_SAMP;
    size_t kept = 0;
    for (const Hit &h : c.hits)
        if (start - h.start <= keep || h.start > start)
            c.hits[kept++] = h;
    c.hits.resize(kept);

    bool confirmed = false;
    for (const Hit &h : c.hits)
    {
        if (h.raw != raw || h.start > start)
            continue;
        long d = (long) (start - h.start);
        if (d < (long) WORD_SAMP - CONFIRM_SLACK)
            continue;
        long r = d % (long) WORD_SAMP;
        if (r > CONFIRM_SLACK && r < (long) WORD_SAMP - CONFIRM_SLACK)
            continue;
        confirmed = true;
        break;
    }
    if (c.hits.size() < HIT_KEEP_MAX)
        c.hits.push_back({ raw, start });

    if (confirmed && c.reporter.due(raw, e->n))
        emit(e, out, (uint8_t) c.k, (float) (10.0 * log10(ratio_sum / WORD_SYMS)), e->n);
}

/* The symbol window that ends at sample e->n is complete. */
static void symbol_window(ale_engine *e)
{
    const uint64_t s0 = e->n - SYM_SAMP;
    const unsigned phase = (unsigned) ((s0 / PHASE_STEP) % PHASES);
    const uint64_t t = s0 / SYM_SAMP;                   /* symbol index within the phase */
    const int16_t *w = &e->x[e->xpos];

    float *en = e->e[phase][t % SYM_RING];
    for (unsigned v = 0; v < NUM_TONES; v++)
    {
        double re = 0, im = 0;
        for (unsigned i = 0; i < SYM_SAMP; i++)
        {
            re += w[i] * e->cos_t[v][i];
            im += w[i] * e->sin_t[v][i];
        }
        en[v] = (float) (re * re + im * im);
    }
    for (size_t i = 0; i < e->ncomb; i++)
        try_combined(e, e->comb[i], phase, t);
}

extern "C" ale_engine *ale_engine_create(const uint8_t *ks, size_t nk, ale_word_cb cb, void *user)
{
    if (nk > ALE_ENGINE_MAX_KS || (nk && !ks))
        return nullptr;
    for (size_t i = 0; i < nk; i++)
        if (ks[i] < 2 || ks[i] > ALE_ENGINE_MAX_K)
            return nullptr;

    ale_engine *e = new (std::nothrow) ale_engine;
    if (!e)
        return nullptr;
    e->cb = cb;
    e->user = user;
    for (unsigned v = 0; v < NUM_TONES; v++)
        for (unsigned i = 0; i < SYM_SAMP; i++)
        {
            double ph = 2 * M_PI * SYMBOL_TO_FREQ[v] * i / SAMPLE_RATE_HZ;
            e->cos_t[v][i] = cos(ph);
            e->sin_t[v][i] = sin(ph);
        }
    e->ncomb = nk;
    for (size_t i = 0; i < nk; i++)
    {
        e->comb[i].k = ks[i];
        e->comb[i].hits.reserve(HIT_KEEP_MAX);
    }
    e->stock.set_word_callback([e](const ALEWord &w) {
        if (!w.valid)
            return;
        const uint32_t raw = ((uint32_t) w.type << PREAMBLE_SHIFT) | w.raw_payload;
        if (e->stock_reporter.due(raw, e->n))
            emit(e, w, 0, 0.0f, e->n);
    });
    return e;
}

extern "C" void ale_engine_destroy(ale_engine *e)
{
    delete e;
}

extern "C" void ale_engine_set_stock(ale_engine *e, int on)
{
    if (e->stock_on && !on)
    {
        e->stock.reset();
        e->stock_fill = 0;
    }
    e->stock_on = on != 0;
}

extern "C" void ale_engine_feed(ale_engine *e, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        e->x[e->xpos] = e->x[e->xpos + SYM_SAMP] = pcm[i];
        e->xpos = (e->xpos + 1) % SYM_SAMP;
        e->n++;

        if (e->stock_on)
            e->stock_buf[e->stock_fill++] = pcm[i];
        if (e->stock_fill == STOCK_BLOCK)
        {
            e->stock.push_samples(e->stock_buf, (uint32_t) STOCK_BLOCK);
            e->stock_fill = 0;
        }

        if (e->n >= SYM_SAMP && e->n % PHASE_STEP == 0)
            symbol_window(e);
    }
}

extern "C" uint64_t ale_engine_position(const ale_engine *e)
{
    return e->n;
}

extern "C" const char *ale_preamble_name(uint8_t preamble)
{
    static const char *const names[] = { "DATA", "THRU", "TO", "TWAS", "FROM", "TIS", "CMD", "REP" };
    return preamble < sizeof(names) / sizeof(names[0]) ? names[preamble] : "?";
}

extern "C" size_t ale_engine_make_call(uint8_t preamble, const char *text3, unsigned words,
                                       int16_t *out, size_t max)
{
    if (!text3 || strlen(text3) != 3 || preamble > ALE_PREAMBLE_REP)
        return 0;
    const uint32_t raw = ((uint32_t) preamble << PREAMBLE_SHIFT)
                       | ((uint32_t) (uint8_t) text3[0] << (2 * CHAR_BITS))
                       | ((uint32_t) (uint8_t) text3[1] << CHAR_BITS) | (uint8_t) text3[2];
    ALEWord w;
    WordParser parser;
    if (!parser.parse_from_bits(raw, w) || !w.valid)
        return 0;

    ALE2GModem::Modulator mod;
    for (unsigned i = 0; i < words; i++)
        mod.enqueue_word(w);
    ToneGenerator tones;
    TxBandpass bandpass;
    std::vector<int16_t> pcm(WORD_SAMP), filtered;
    uint8_t syms[WORD_SYMS];
    size_t written = 0;
    while (mod.pull_symbol_frame(syms))
    {
        tones.generate_symbols(syms, WORD_SYMS, pcm.data(), TX_AMPLITUDE);
        filtered.clear();
        bandpass.process(pcm.data(), pcm.size(), filtered);
        for (int16_t s : filtered)
        {
            if (written == max)
                return written;
            out[written++] = s;
        }
    }
    return written;
}
