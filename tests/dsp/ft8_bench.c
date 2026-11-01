/* ft8_bench - count what the FT8 decoder finds in a recording
 *
 *   ft8_bench [-s] [-o SECONDS] [-p PASSES] [-r ROUNDS] [-x DECODES] FILE.wav
 *
 * Reads a WAV (16-bit PCM, any rate, the first channel), resamples it to
 * 12 kHz as the digi pump does (stream_resampler, int16 / 32768), and runs
 * sbitx_ft8_decode_ex() on it, printing one line per message:
 *
 *   MSG <window> <snr> <offset_hz> <pass> <subtracted> <text>
 *
 * and a total per pass.  -p sets which of ft8_js8's passes run (a bit mask
 * of FT8_JS8_*: 0 is ft8_lib alone, 15 all of them; default 15), -r how
 * many rounds LDPC feedback gets (default 2), -x how many decoding passes
 * subtraction gets (sbitx_ft8_subtract_passes: 1 none, default 2); <subtracted>
 * is 1 for a message found after it.  The windows are the UTC slots: 15 s each, the first starting
 * -o SECONDS into the file (radiod's recordings start on a slot boundary
 * less 1 s, so 1).  With -s they are the digi pump's instead: 15 s, every
 * 2 s, a message kept once per 1.5 s.  Two builds of the decoder compare by
 * diffing their MSG lines.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sbitx_ft8.h"
#include "ft8_js8.h"
#include "stream_resampler.h"

#define RATE 12000

static float *read_wav(const char *path, size_t *n_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12], ch[8];
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0, len;
    float *pcm = NULL;
    size_t n = 0;

    if (!f || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        exit(1);
    }

    while (fread(ch, 1, 8, f) == 8) {
        len = (uint32_t) ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t) ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            if (len < 16 || fread(fmt, 1, 16, f) != 16) break;
            channels = (uint16_t) (fmt[2] | fmt[3] << 8);
            rate = (uint32_t) fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t) fmt[7] << 24;
            bits = (uint16_t) (fmt[14] | fmt[15] << 8);
            fseek(f, (long) len - 16, SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (bits != 16 || !channels || !rate) break;
            /* a recording still being written says 0 */
            size_t frames = len ? len / (2u * channels) : 0;
            int16_t *raw = NULL;
            if (!frames) {
                long here = ftell(f);
                fseek(f, 0, SEEK_END);
                frames = (size_t) (ftell(f) - here) / (2u * channels);
                fseek(f, here, SEEK_SET);
            }
            raw = malloc(frames * channels * 2);
            frames = fread(raw, 2u * channels, frames, f);
            float *in = malloc(frames * sizeof(float));
            for (size_t i = 0; i < frames; i++)
                in[i] = (float) raw[i * channels] / 32768.0f;
            free(raw);

            stream_resampler rs;
            int g = 1, a = RATE, b = (int) rate;
            for (int x = a, y = b; y; ) { int t = x % y; x = y; y = t; g = x; }
            if (!stream_resampler_init(&rs, RATE / g, (int) rate / g, 0.02f)) {
                fprintf(stderr, "%s: cannot resample %u Hz\n", path, rate);
                exit(1);
            }
            pcm = malloc((frames * RATE / rate + RATE) * sizeof(float));
            for (size_t i = 0; i < frames; i += 4800) {
                size_t m = frames - i < 4800 ? frames - i : 4800;
                size_t k = stream_resampler_run(&rs, in + i, m);
                memcpy(pcm + n, rs.out, k * sizeof(float));
                n += k;
            }
            stream_resampler_free(&rs);
            free(in);
            break;
        } else {
            fseek(f, (long) len + (len & 1), SEEK_CUR);
        }
    }

    fclose(f);
    if (!pcm) {
        fprintf(stderr, "%s: no 16-bit PCM data\n", path);
        exit(1);
    }
    *n_out = n;
    return pcm;
}

typedef struct { char text[48]; double at; } seen_t;

int main(int argc, char **argv)
{
    int sliding = 0, opt, total = 0, nseen = 0;
    double offset = 1.0;
    static seen_t seen[20000];

    int by_pass[16] = { 0 }, by_sub[2] = { 0 };

    while ((opt = getopt(argc, argv, "so:p:r:x:")) != -1) {
        if (opt == 's') sliding = 1;
        else if (opt == 'o') offset = atof(optarg);
        else if (opt == 'p') ft8_js8_passes = (unsigned) strtoul(optarg, NULL, 0);
        else if (opt == 'r') ft8_js8_feedback_rounds = atoi(optarg);
        else if (opt == 'x') sbitx_ft8_subtract_passes = atoi(optarg);
        else { fprintf(stderr, "usage: ft8_bench [-s] [-o SECONDS] [-p PASSES] [-r ROUNDS] [-x DECODES] FILE.wav\n"); return 2; }
    }
    if (optind >= argc) { fprintf(stderr, "usage: ft8_bench [-s] [-o SECONDS] [-p PASSES] [-r ROUNDS] [-x DECODES] FILE.wav\n"); return 2; }

    size_t n;
    float *pcm = read_wav(argv[optind], &n);
    const int win = 15 * RATE;
    const double step = sliding ? 2.0 : 15.0;
    const double first = sliding ? 0.0 : offset;

    for (int w = 0; ; w++) {
        size_t start = (size_t) ((first + w * step) * RATE);
        if (start + win > n) break;
        sbitx_ft8_decode_result r[50];
        /* the window's start on a clock whose 15 s slots are the file's */
        double start_s = 1500.0 + (double) start / RATE - offset;
        int k = sbitx_ft8_decode_at(pcm + start, win, start_s, r, 50);
        for (int i = 0; i < k; i++) {
            double at = (double) start / RATE + r[i].time_s;
            int dup = 0;
            for (int j = nseen - 1; j >= 0 && at - seen[j].at < 20.0; j--)
                if (!strcmp(seen[j].text, r[i].text) && fabs(at - seen[j].at) < 1.5) { dup = 1; break; }
            if (dup) continue;
            if (nseen < (int) (sizeof seen / sizeof seen[0])) {
                snprintf(seen[nseen].text, sizeof seen[nseen].text, "%s", r[i].text);
                seen[nseen++].at = at;
            }
            printf("MSG %d %+.0f %4.0f %d %d %s\n", sliding ? (int) floor((at - offset) / 15.0) : w,
                   r[i].has_snr ? r[i].snr_db : -99.0, r[i].freq_hz, r[i].pass, r[i].subtracted, r[i].text);
            total++;
            by_pass[r[i].pass & 15]++;
            by_sub[r[i].subtracted > 0]++;
        }
    }

    printf("TOTAL %d messages in %.0f s (%s windows, passes 0x%x): ft8_lib %d, feedback %d, "
           "whitened %d, linear %d, combined %d; after subtraction %d\n", total, (double) n / RATE,
           sliding ? "sliding 2 s" : "UTC 15 s", ft8_js8_passes, by_pass[0], by_pass[FT8_JS8_FEEDBACK],
           by_pass[FT8_JS8_WHITEN], by_pass[FT8_JS8_LINEAR], by_pass[FT8_JS8_COMBINE],
           by_sub[1]);
    free(pcm);
    return 0;
}
