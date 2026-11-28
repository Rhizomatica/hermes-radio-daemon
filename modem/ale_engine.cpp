/* ale_engine: see ale_engine.h.  The combining receiver is the one measured
 * in ALELite's xval/xval_acq.cpp (docs/ALE-DESIGN.md 14.5), made to run on a
 * stream: the same symbol-phase search, word rotations, decode and
 * acceptance rules, and the same confirmation, evaluated as each symbol
 * window completes instead of over a recording. */

#include "ale_engine.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "App/ale_controller.h"
#include "Codec/ale_decoder.h"
#include "PAL/audio_driver.h"
#include "PAL/radio.h"
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

struct LinkHost;

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

    std::unique_ptr<LinkHost> link;     /* the controller, while it runs */
    ~ale_engine();
};

/* Words for the link layer (defined with LinkHost below). */
static void link_stock_word(ale_engine *e, const ALEWord &w);
static void link_combined_word(ale_engine *e, const ALEWord &w);

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

    if (!confirmed)
        return;
    link_combined_word(e, out);
    if (c.reporter.due(raw, e->n))
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
        link_stock_word(e, w);
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

/* ── The link layer ─────────────────────────────────────────────────────── */

namespace {

/* Transmit audio kept queued ahead of what the radio has played: enough to
 * ride out the feeding thread's period, little enough to stop promptly, and
 * less than a radio's TX queue holds (a Hamlib rig's ring: 16000 samples at
 * 48 kHz, 333 ms), which drops what does not fit. */
constexpr uint64_t TX_LEAD_SAMPLES = SAMPLE_RATE_HZ / 5;          /* 200 ms */
constexpr uint32_t MS_PER_S = 1000;
/* A combining receiver's word goes to the controller once a word period at
 * most, as a standard receiver gives them. */
constexpr uint64_t DELIVER_SPAN_SAMPLES = WORD_SAMP * 3 / 4;
/* ... and only when the stock receiver has heard nothing for this long: the
 * combining window spans up to ALE_ENGINE_MAX_K words, so after a call it
 * goes on decoding the call's words ("ghosts") for that long, which in a
 * handshake reads as a busy channel or a new call.  A call the stock
 * receiver hears needs no combining. */
constexpr uint64_t GHOST_HOLD_SAMPLES = (uint64_t) ALE_ENGINE_MAX_K * WORD_SAMP;
constexpr size_t   DELIVER_SLOTS = 32;

uint8_t mode_of(pal::RadioMode m)
{
    return (m == pal::RadioMode::LSB || m == pal::RadioMode::DATA_LSB) ? ALE_MODE_LSB : ALE_MODE_USB;
}

/* Last delivery of each word, by stream position. */
struct Delivered
{
    struct Slot { uint32_t raw; uint64_t at; bool used; } slot[DELIVER_SLOTS] = {};

    uint64_t *find(uint32_t raw)
    {
        for (Slot &s : slot)
            if (s.used && s.raw == raw)
                return &s.at;
        return nullptr;
    }
    void note(uint32_t raw, uint64_t at)
    {
        if (uint64_t *p = find(raw)) { *p = at; return; }
        Slot *oldest = &slot[0];
        for (Slot &s : slot)
        {
            if (!s.used) { oldest = &s; break; }
            if (s.at < oldest->at) oldest = &s;
        }
        *oldest = { raw, at, true };
    }
    bool within(uint32_t raw, uint64_t now, uint64_t span)
    {
        uint64_t *p = find(raw);
        return p && now - *p < span;
    }
};

} /* namespace */

/* The radio and the sound card, as PC-ALE's controller wants them, on the
 * host's callbacks. */
struct LinkHost : public pal::IAudioDriver, public pal::IRadio
{
    ale_engine *e;
    ale_radio_ops ops;
    ale_link_cb cb;
    void *cb_user;
    ALEController ctrl;
    std::string lqa_file;

    /* TX: frames pulled from the controller's modulator, played in order */
    std::function<bool(uint8_t *)> source;
    std::deque<std::function<void()>> armed;     /* one per frame, in order */
    std::deque<uint64_t> frame_ends;             /* where each frame's audio ends (rendered samples) */
    bool want_key = false;          /* the controller's PTT: keyed once there is audio */
    bool keyed = false;
    bool flushed = true;
    std::deque<int16_t> pending;    /* rendered, not yet handed to the radio */
    uint64_t rendered = 0;          /* samples rendered this over */
    uint64_t fed = 0;               /* samples handed to the radio this over */
    ToneGenerator tones;
    std::unique_ptr<TxBandpass> bandpass;

    pal::Channel channel;
    std::string peer;
    size_t tail_samples = 0;            /* ale_link_config.tx_tail_ms */
    bool call_waiting = false;          /* a call alerted, waiting for accept or reject */
    Delivered combined_sent;
    uint64_t last_stock_word = 0;       /* stream position of the stock receiver's last word */
    bool heard_stock = false;

    LinkHost(ale_engine *eng, const ale_radio_ops &o, ale_link_cb c, void *u)
        : e(eng), ops(o), cb(c), cb_user(u) {}

    void event(uint8_t kind, const std::string &peer_addr, const std::string &text, uint32_t hz = 0)
    {
        if (!cb)
            return;
        ale_link_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = kind;
        snprintf(ev.peer, sizeof(ev.peer), "%s", peer_addr.c_str());
        snprintf(ev.text, sizeof(ev.text), "%s", text.c_str());
        ev.hz = hz;
        cb(&ev, cb_user);
    }

    /* pal::IAudioDriver: the controller's view of the sound card */
    bool open(const std::string &, const std::string &) override { return true; }
    void close() override {}
    void set_symbol_source(std::function<bool(uint8_t *)> fn) override { source = std::move(fn); }
    void arm_frame_complete(std::function<void()> fn) override { armed.push_back(std::move(fn)); }
    void tick(std::vector<int16_t> &rx_out) override { rx_out.clear(); }
    bool is_open() const override { return true; }
    std::vector<std::string> list_devices() const override { return {}; }

    /* pal::IRadio: the controller's view of the radio */
    bool initialize() override { return true; }
    void shutdown() override {}
    bool start() override { return true; }
    void stop() override {}
    bool set_channel(const pal::Channel &ch) override
    {
        if (!ops.retune || !ops.retune(ch.rx_frequency, mode_of(ch.rx_mode), ops.user))
            return false;
        channel = ch;
        event(ALE_EV_CHANNEL, "", "", ch.rx_frequency);
        return true;
    }
    pal::Channel get_channel() const override { return channel; }
    /* The controller's PTT goes with its RX switch, also around phases that
     * send nothing: the radio is keyed only when the first frame is ready
     * (pump), so those never key it. */
    void set_ptt(bool on) override
    {
        want_key = on;
        if (on)
            return;
        if (keyed)
        {
            if (ops.key)
                ops.key(false, ops.user);
            keyed = false;
        }
        complete_all();
        frame_ends.clear();
        pending.clear();
    }

    /* Frames the radio never played still complete, so the controller's
     * counters end. */
    void complete_all()
    {
        while (!armed.empty())
        {
            auto fn = std::move(armed.front());
            armed.pop_front();
            fn();
        }
    }

    bool is_transmitting() const override { return keyed; }
    bool is_ready() const override { return true; }
    std::string get_port_config() const override { return ""; }
    void register_send_callback(SendCommandCallback) override {}
    void register_ack_callback(AckCallback) override {}
    void process_response(const uint8_t *, size_t) override {}

    void render(const std::vector<int16_t> &pcm)
    {
        pending.insert(pending.end(), pcm.begin(), pcm.end());
        rendered += pcm.size();
    }

    void render_frame(const uint8_t *syms)
    {
        std::vector<int16_t> pcm(WORD_SAMP), out;
        tones.generate_symbols(syms, WORD_SYMS, pcm.data(), TX_AMPLITUDE);
        bandpass->process(pcm.data(), pcm.size(), out);
        render(out);
        flushed = false;
        frame_ends.push_back(rendered + bandpass->group_delay());
    }

    /* The band-pass holds the last group delay of audio: push it out. */
    void flush()
    {
        if (flushed || !bandpass)
            return;
        std::vector<int16_t> zeros(bandpass->group_delay(), 0), out;
        bandpass->process(zeros.data(), zeros.size(), out);
        render(out);
        render(std::vector<int16_t>(tail_samples, 0));
        if (!frame_ends.empty())
            frame_ends.back() += tail_samples;
        flushed = true;
    }

    /* Hand the radio what keeps it TX_LEAD_SAMPLES ahead of its playout. */
    void feed(uint64_t played)
    {
        uint64_t ahead = fed - std::min(played, fed);
        if (ahead >= TX_LEAD_SAMPLES || pending.empty())
            return;
        size_t n = (size_t) std::min<uint64_t>(TX_LEAD_SAMPLES - ahead, pending.size());
        std::vector<int16_t> chunk(pending.begin(), pending.begin() + (long) n);
        pending.erase(pending.begin(), pending.begin() + (long) n);
        if (ops.tx_audio)
            ops.tx_audio(chunk.data(), n, ops.user);
        fed += n;
    }

    /* Keep the radio fed and tell the controller of each frame played. */
    void pump()
    {
        uint8_t syms[WORD_SYMS];
        if (want_key && !keyed)
        {
            if (!source || !source(syms))
                return;                 /* nothing to send yet */
            keyed = ops.key && ops.key(true, ops.user);
            if (!keyed)
            {
                want_key = false;       /* refused: the words never go out */
                while (source(syms)) {}
                complete_all();
                return;
            }
            rendered = fed = 0;
            pending.clear();
            frame_ends.clear();
            bandpass.reset(new TxBandpass());
            render_frame(syms);
        }
        if (!keyed)
            return;
        /* render a frame ahead of what the radio is fed */
        while (pending.size() < TX_LEAD_SAMPLES && source && source(syms))
            render_frame(syms);
        if (pending.size() < TX_LEAD_SAMPLES)
            flush();                    /* nothing more to send now */
        uint64_t played = ops.tx_played ? ops.tx_played(ops.user) : fed;
        feed(played);
        while (!frame_ends.empty() && played >= frame_ends.front())
        {
            frame_ends.pop_front();
            if (!armed.empty())
            {
                auto fn = std::move(armed.front());
                armed.pop_front();
                fn();
            }
        }
    }
};

ale_engine::~ale_engine() = default;

static uint32_t raw_of(const ALEWord &w)
{
    return ((uint32_t) w.type << PREAMBLE_SHIFT) | w.raw_payload;
}

static void link_stock_word(ale_engine *e, const ALEWord &w)
{
    if (!e->link)
        return;
    e->link->last_stock_word = e->n;
    e->link->heard_stock = true;
    e->link->ctrl.receive_word(w);
}

static void link_combined_word(ale_engine *e, const ALEWord &w)
{
    if (!e->link)
        return;
    LinkHost &l = *e->link;
    const uint32_t raw = raw_of(w);
    /* call detection only: idle or scanning */
    ALEState st = l.ctrl.state();
    if (st != ALEState::IDLE && st != ALEState::SCANNING)
        return;
    if (l.heard_stock && e->n - l.last_stock_word < GHOST_HOLD_SAMPLES)
        return;
    if (l.combined_sent.within(raw, e->n, DELIVER_SPAN_SAMPLES))
        return;
    l.combined_sent.note(raw, e->n);
    l.ctrl.receive_word(w);
}

extern "C" bool ale_address_valid(const char *text)
{
    size_t n = text ? strlen(text) : 0;
    if (n < 1 || n > ALE_ADDRESS_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!WordParser::is_valid_basic38_char(text[i]))
            return false;
    return true;
}

extern "C" bool ale_amd_valid(const char *text)
{
    size_t n = text ? strlen(text) : 0;
    if (n > ALE_AMD_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!WordParser::is_valid_expanded64_char(text[i]))
            return false;
    return true;
}

extern "C" int ale_engine_link_start(ale_engine *e, const ale_link_config *cfg,
                                     const ale_radio_ops *ops, ale_link_cb cb, void *user)
{
    if (!cfg || !ops || !ale_address_valid(cfg->self))
        return -1;
    ale_engine_link_stop(e);
    std::unique_ptr<LinkHost> l(new (std::nothrow) LinkHost(e, *ops, cb, user));
    if (!l)
        return -1;
    LinkHost *h = l.get();
    h->tail_samples = (size_t) cfg->tx_tail_ms * SAMPLE_RATE_HZ / MS_PER_S;
    ALEController &c = h->ctrl;
    c.set_self_address(cfg->self);
    if (cfg->channel_file && *cfg->channel_file)
    {
        c.load_channels(cfg->channel_file);
        c.set_channel_file(cfg->channel_file);
    }
    if (cfg->lqa_file && *cfg->lqa_file)
    {
        h->lqa_file = cfg->lqa_file;
        c.load_lqa(h->lqa_file);
    }
    if (cfg->scan_dwell_ms)
        c.set_scan_dwell_ms(cfg->scan_dwell_ms);
    c.set_manual_accept_mode(cfg->manual_accept, cfg->accept_timeout_ms);
    c.enable_automatic_sounding(cfg->auto_sounding);
    c.set_radio(h);
    c.set_audio_device(h);

    c.on_call_received = [h](const std::string &from) {
        h->peer = from;
        h->call_waiting = true;
        h->event(ALE_EV_CALL, from, "");
    };
    c.on_link_established = [h](const std::string &peer) {
        /* the controller gives the peer's first word (3 characters); the
         * call (ours or theirs) gave the whole address */
        if (h->peer.empty() || h->peer.compare(0, peer.size(), peer) != 0)
            h->peer = peer;
        h->event(ALE_EV_LINKED, h->peer, "");
    };
    c.on_link_terminated = [h](const std::string &why) {
        h->event(ALE_EV_TERMINATED, h->peer, why);
        h->peer.clear();
    };
    c.on_amd_received = [h](const std::string &from, const std::string &text) {
        h->event(ALE_EV_AMD, from, text);
    };
    c.on_status_changed = [h](const std::string &msg) {
        h->event(ALE_EV_STATUS, "", msg);
    };

    /* what the radio is tuned to now is the channel ALE listens on */
    h->channel.rx_frequency = h->channel.tx_frequency = cfg->start_hz;
    h->channel.rx_mode = h->channel.tx_mode =
        cfg->start_mode == ALE_MODE_LSB ? pal::RadioMode::LSB : pal::RadioMode::USB;
    e->link = std::move(l);
    c.start_available();
    return 0;
}

extern "C" void ale_engine_link_stop(ale_engine *e)
{
    if (!e->link)
        return;
    e->link->ctrl.emergency_stop();
    e->link->set_ptt(false);
    if (!e->link->lqa_file.empty())
        e->link->ctrl.save_lqa(e->link->lqa_file);
    e->link.reset();
}

extern "C" bool ale_engine_link_running(const ale_engine *e)
{
    return e->link != nullptr;
}

static bool idle_or_scanning(ale_engine *e)
{
    ALEState st = e->link->ctrl.state();
    return st == ALEState::IDLE || st == ALEState::SCANNING;
}

extern "C" bool ale_engine_listen(ale_engine *e)
{
    if (!e->link || !idle_or_scanning(e))
        return false;
    e->link->ctrl.start_available();
    return true;
}

extern "C" bool ale_engine_scan(ale_engine *e)
{
    if (!e->link || !idle_or_scanning(e) || e->link->ctrl.channels().empty())
        return false;
    e->link->ctrl.start_scanning();
    return true;
}

extern "C" bool ale_engine_call(ale_engine *e, const char *to, const char *amd)
{
    if (!e->link || !ale_address_valid(to) || (amd && !ale_amd_valid(amd)))
        return false;
    if (amd && *amd)
        e->link->ctrl.process_command(std::string("CMD:AMD ") + amd);
    if (!e->link->ctrl.initiate_call(to))
        return false;
    e->link->peer = to;
    return true;
}

/* Accept and reject act on a call that has alerted (its conclusion heard)
 * and still waits: earlier in the handshake the controller ignores them. */
static bool call_waiting(ale_engine *e)
{
    return e->link && e->link->call_waiting && e->link->ctrl.state() == ALEState::HANDSHAKE;
}

extern "C" bool ale_engine_accept(ale_engine *e)
{
    if (!call_waiting(e))
        return false;
    e->link->call_waiting = false;
    e->link->ctrl.accept_call();
    return true;
}

extern "C" bool ale_engine_reject(ale_engine *e)
{
    if (!call_waiting(e))
        return false;
    e->link->call_waiting = false;
    e->link->ctrl.reject_call();
    return true;
}

extern "C" bool ale_engine_terminate(ale_engine *e)
{
    if (!e->link || e->link->ctrl.state() != ALEState::LINKED)
        return false;
    e->link->ctrl.terminate_link();
    return true;
}

extern "C" bool ale_engine_tune(ale_engine *e, uint32_t hz, uint8_t mode)
{
    if (!e->link || !idle_or_scanning(e))
        return false;
    pal::Channel ch = e->link->channel;
    ch.rx_frequency = ch.tx_frequency = hz;
    ch.rx_mode = ch.tx_mode = mode == ALE_MODE_LSB ? pal::RadioMode::LSB : pal::RadioMode::USB;
    return e->link->set_channel(ch);
}

extern "C" bool ale_engine_sound(ale_engine *e)
{
    return e->link && e->link->ctrl.send_sounding();
}

extern "C" void ale_engine_tick(ale_engine *e, uint32_t now_ms)
{
    if (!e->link)
        return;
    ALEState before = e->link->ctrl.state();
    e->link->ctrl.update(now_ms);
    e->link->pump();
    ALEState after = e->link->ctrl.state();
    if (after != ALEState::HANDSHAKE)
        e->link->call_waiting = false;
    if (after != before)
        e->link->event(ALE_EV_STATE, e->link->peer, ALEStateMachine::state_name(after));
}

extern "C" void ale_engine_link_get_state(ale_engine *e, ale_link_state *out)
{
    memset(out, 0, sizeof(*out));
    if (!e->link)
    {
        snprintf(out->state, sizeof(out->state), "OFF");
        return;
    }
    LinkHost &l = *e->link;
    snprintf(out->state, sizeof(out->state), "%s", ALEStateMachine::state_name(l.ctrl.state()));
    snprintf(out->peer, sizeof(out->peer), "%s", l.peer.c_str());
    out->hz = l.channel.rx_frequency;
    out->mode = mode_of(l.channel.rx_mode);
    out->transmitting = l.keyed;
    out->link_seconds = l.ctrl.is_link_active() ? l.ctrl.get_call_duration_seconds() : 0;
    ALEController::SignalQuality q = l.ctrl.get_current_signal_quality();
    out->votes = (uint8_t) q.votes;
    out->fec_errors = (uint8_t) q.fec_errors;
    out->channels = (uint16_t) l.ctrl.channels().size();
}
