/* cessb: on a speech-like signal through an SSB modulator like the sBitx's
 * (2048-point overlap-save, a 1025-tap 300-2700 Hz band-pass), the envelope
 * never passes the ceiling; the correction leaves little for the last
 * clip; the average power at the same peak is well above what the audio
 * limiter driven as hard gives; nothing leaves the passband; a signal
 * under the ceiling comes out as it went in, delayed; and a reset forgets
 * the over.
 */
#include <complex.h>
#include <fftw3.h>
#include <math.h>
#include <string.h>

#include "unity.h"
#include "test_util.h"
#include "dsp/cessb.h"

#define N        2048
#define L        (N / 2)
#define TAPS     1025
#define DELAY    (TAPS / 2)
#define FS_HZ    96000
#define FS       ((double) FS_HZ)
#define LO_HZ    300.0
#define HI_HZ    2700.0
#define CEILING  0.95          /* the TX chain's audio limit */
#define SECONDS  4
#define BLOCKS   (SECONDS * FS_HZ / L)
#define GUARD_HZ 150.0         /* the band-pass's skirt, not out of band */

static double complex h[N];
static double audio[BLOCKS * L];
static double complex env[BLOCKS * L];

/* A one-sided band-pass LO_HZ..HI_HZ: a Blackman-windowed sinc shifted to
 * the band's centre, causal (delayed DELAY samples), as n-point response. */
static void design(void)
{
    fftw_complex *b = fftw_alloc_complex(N);
    fftw_plan p = fftw_plan_dft_1d(N, b, b, FFTW_FORWARD, FFTW_ESTIMATE);
    memset(b, 0, N * sizeof(*b));
    double fc = (LO_HZ + HI_HZ) / 2 / FS, bw = (HI_HZ - LO_HZ) / 2 / FS;
    for (int n = 0; n < TAPS; n++) {
        int t = n - DELAY;
        double s = t ? sin(2 * M_PI * bw * t) / (M_PI * t) : 2 * bw;
        double w = 0.42 - 0.5 * cos(2 * M_PI * n / (TAPS - 1)) + 0.08 * cos(4 * M_PI * n / (TAPS - 1));
        b[n] = s * w * cexp(I * 2 * M_PI * fc * t);
    }
    fftw_execute(p);
    memcpy(h, b, sizeof h);
    fftw_destroy_plan(p);
    fftw_free(b);
}

/* Speech-like audio peaking at CEILING: a vowel, which is the glottis's
 * pulses ringing through the vocal tract's resonances (formants at 700,
 * 1220 and 2600 Hz), its pitch gliding 100-160 Hz, in syllables of 0.3 s
 * with gaps between. */
static void speech(void)
{
    static const double formant_hz[] = { 700, 1220, 2600 };
    static const double bandwidth_hz[] = { 80, 90, 120 };
    double y1[3] = {0}, y2[3] = {0};
    double ph = 0, peak = 0;
    for (int i = 0; i < BLOCKS * L; i++) {
        double t = i / FS;
        double f0 = 130 + 30 * sin(2 * M_PI * 0.7 * t);
        ph += f0 / FS;
        double v = 0;
        if (ph >= 1.0) {                /* a glottal pulse */
            ph -= 1.0;
            v = 1.0;
        }
        for (int f = 0; f < 3; f++) {   /* two-pole resonators in cascade */
            double r = exp(-M_PI * bandwidth_hz[f] / FS);
            double y = v + 2 * r * cos(2 * M_PI * formant_hz[f] / FS) * y1[f] - r * r * y2[f];
            y2[f] = y1[f];
            y1[f] = y;
            v = y * (1 - r);
        }
        double syl = fmod(t, 0.4);
        audio[i] = v * (syl < 0.3 ? sin(M_PI * syl / 0.3) : 0.0);
        if (fabs(audio[i]) > peak)
            peak = fabs(audio[i]);
    }
    for (int i = 0; i < BLOCKS * L; i++)
        audio[i] *= CEILING / peak;
}

/* audio (gain g, then the TX chain's limiter at CEILING) through the
 * modulator: the complex envelope, scaled so a tone of amplitude a gives a. */
static void modulate(double g)
{
    fftw_complex *in = fftw_alloc_complex(N), *out = fftw_alloc_complex(N);
    fftw_plan pf = fftw_plan_dft_1d(N, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_plan pr = fftw_plan_dft_1d(N, out, in, FFTW_BACKWARD, FFTW_ESTIMATE);
    double h_max = 0;
    for (int k = 0; k < N; k++)
        h_max = fmax(h_max, cabs(h[k]));
    double prev[L] = {0};
    for (int b = 0; b < BLOCKS; b++) {
        for (int i = 0; i < L; i++) {
            double v = fmax(-CEILING, fmin(CEILING, audio[b * L + i] * g));
            in[i] = prev[i];
            in[L + i] = v;
            prev[i] = v;
        }
        fftw_execute(pf);
        for (int k = 0; k < N; k++)
            out[k] *= h[k];
        memset(out + N / 2, 0, (N / 2) * sizeof(*out));
        fftw_execute(pr);
        for (int i = 0; i < L; i++)
            env[b * L + i] = in[L + i] / (N * h_max / 2.0);
    }
    fftw_destroy_plan(pf);
    fftw_destroy_plan(pr);
    fftw_free(in);
    fftw_free(out);
}

static double peak_of(void)
{
    double p = 0;
    for (int i = 0; i < BLOCKS * L; i++)
        p = fmax(p, cabs(env[i]));
    return p;
}

static double power_of(void)
{
    double s = 0;
    for (int i = 0; i < BLOCKS * L; i++)
        s += creal(env[i] * conj(env[i]));
    return s / (BLOCKS * L);
}

/* env through cessb at drive_db, in place. */
static cessb *run(double drive_db)
{
    cessb *c = cessb_new(N, DELAY);
    TEST_ASSERT_NOT_NULL(c);
    cessb_set_drive_db(c, drive_db);
    for (int b = 0; b < BLOCKS; b++)
        cessb_process(c, env + b * L, h, CEILING);
    return c;
}

/* The energy outside LO_HZ..HI_HZ (and its skirts) against inside, dB. */
static double out_of_band_db(void)
{
    fftw_complex *s = fftw_alloc_complex(L);
    fftw_plan p = fftw_plan_dft_1d(L, s, s, FFTW_FORWARD, FFTW_ESTIMATE);
    double in = 0, out = 0;
    for (int b = 0; b < BLOCKS; b++) {
        for (int i = 0; i < L; i++)
            s[i] = env[b * L + i] * (0.5 - 0.5 * cos(2 * M_PI * i / (L - 1)));
        fftw_execute(p);
        for (int k = 0; k < L; k++) {
            double f = (k < L / 2 ? k : k - L) * FS / L;
            double e = creal(s[k] * conj(s[k]));
            if (f >= LO_HZ - GUARD_HZ && f <= HI_HZ + GUARD_HZ)
                in += e;
            else
                out += e;
        }
    }
    fftw_destroy_plan(p);
    fftw_free(s);
    return 10 * log10(out / in);
}

void setUp(void) {}
void tearDown(void) {}

static void test_envelope_never_passes_the_ceiling(void)
{
    modulate(1.0);
    cessb_free(run(CESSB_MAX_DB));
    TEST_ASSERT_TRUE_MESSAGE(peak_of() <= CEILING * (1 + 1e-9), test_msg("peak %.4f", peak_of()));
}

static void test_correction_leaves_little_for_the_last_clip(void)
{
    modulate(1.0);
    cessb *c = run(6.0);
    TEST_ASSERT_TRUE_MESSAGE(cessb_residual_peak(c) < 1.02,
                             test_msg("overshoot left %.1f %%", (cessb_residual_peak(c) - 1) * 100));
    cessb_free(c);
}

static void test_more_power_than_the_audio_limiter(void)
{
    const double drive_db = 6.0, g = pow(10, drive_db / 20);
    modulate(g);                        /* today: the audio limiter driven as hard */
    double lim_peak = peak_of(), lim_power = power_of();
    TEST_ASSERT_TRUE_MESSAGE(lim_peak > CEILING * 1.2, test_msg("limiter envelope %.2f", lim_peak / CEILING));
    modulate(1.0);
    cessb_free(run(drive_db));
    /* Both at the same peak: the limiter's backed off to the ceiling. */
    double gain_db = 10 * log10(power_of() / (lim_power * pow(CEILING / lim_peak, 2)));
    /* 2.3 dB on this vowel; 2.9 dB on recorded speech (cessb.c). */
    TEST_ASSERT_TRUE_MESSAGE(gain_db > 1.5, test_msg("%.1f dB over the limiter", gain_db));
}

static void test_stays_in_the_passband(void)
{
    modulate(1.0);
    cessb_free(run(CESSB_MAX_DB));
    double oob = out_of_band_db();
    TEST_ASSERT_TRUE_MESSAGE(oob < -40.0, test_msg("out of band %.0f dB", oob));
}

static void test_under_the_ceiling_passes_unchanged(void)
{
    const double a = 0.5 * CEILING, w = 2 * M_PI * 1000.0 / FS;
    for (int i = 0; i < BLOCKS * L; i++)
        env[i] = a * cexp(I * w * i);
    cessb_free(run(0.0));
    for (int i = 4 * L; i < BLOCKS * L; i++) {
        double complex want = a * cexp(I * w * (i - 2 * DELAY));
        TEST_ASSERT_TRUE_MESSAGE(cabs(env[i] - want) < 1e-3 * a, test_msg("sample %d off by %.2g", i, cabs(env[i] - want)));
    }
}

static void test_reset_forgets_the_over(void)
{
    modulate(1.0);
    cessb *c = run(CESSB_MAX_DB);
    cessb_reset(c);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, cessb_residual_peak(c));
    double complex z[L] = {0};
    cessb_process(c, z, h, CEILING);
    for (int i = 0; i < L; i++)
        TEST_ASSERT_EQUAL_DOUBLE(0.0, cabs(z[i]));
    cessb_free(c);
}

int main(void)
{
    design();
    speech();
    UNITY_BEGIN();
    RUN_TEST(test_envelope_never_passes_the_ceiling);
    RUN_TEST(test_correction_leaves_little_for_the_last_clip);
    RUN_TEST(test_more_power_than_the_audio_limiter);
    RUN_TEST(test_stays_in_the_passband);
    RUN_TEST(test_under_the_ceiling_passes_unchanged);
    RUN_TEST(test_reset_forgets_the_over);
    return UNITY_END();
}
