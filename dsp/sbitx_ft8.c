/* sBitx FT8 modem - encode/decode using vendored ft8_lib
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "ft8/text.h"
#include "common/wave.h"

#include "sbitx_ft8.h"

const char *digi_spool_dir = "/var/spool/hermes-digi";

#define FT8_SYMBOL_PERIOD 0.160f
#define FT8_SYMBOL_BT 2.0f
#define GFSK_CONST_K 5.336446f

static int spool_index = 0;

static void synth_gfsk(const uint8_t *symbols, int n_sym, float f0,
                       int signal_rate, float *signal, int *num_samples)
{
    float tone_spacing = 1.0f / FT8_SYMBOL_PERIOD;
    float pi2 = (float)(2.0 * M_PI);
    int sym_samples = (int)(FT8_SYMBOL_PERIOD * signal_rate);
    float ts = 1.0f / signal_rate;
    float phase = 0.0f;
    int idx = 0;

    float gauss_a = GFSK_CONST_K * FT8_SYMBOL_BT / FT8_SYMBOL_PERIOD;

    for (int i = 0; i < n_sym; i++)
    {
        float freq = f0 + symbols[i] * tone_spacing;

        if (i == 0 || symbols[i] != symbols[i - 1])
        {
            float sum = 0.0f;
            for (int j = 0; j < sym_samples; j++)
            {
                float t = (j - sym_samples / 2.0f) * ts;
                sum += expf(-(t * t) * gauss_a * gauss_a / 2.0f);
            }
            float norm = 1.0f / sum;

            float accum = 0.0f;
            for (int j = 0; j < sym_samples && idx < *num_samples; j++, idx++)
            {
                float t = (j - sym_samples / 2.0f) * ts;
                accum += expf(-(t * t) * gauss_a * gauss_a / 2.0f) * norm;
                float inst_freq = f0 + symbols[i] * tone_spacing;
                if (i > 0 && j < sym_samples / 2 && accum < 0.5f)
                    inst_freq = f0 + symbols[i - 1] * tone_spacing;
                phase += pi2 * inst_freq * ts;
                if (phase > M_PI)  phase -= pi2;
                if (phase < -M_PI) phase += pi2;
                signal[idx] = sinf(phase);
            }
        }
        else
        {
            for (int j = 0; j < sym_samples && idx < *num_samples; j++, idx++)
            {
                phase += pi2 * freq * ts;
                if (phase > M_PI)  phase -= pi2;
                if (phase < -M_PI) phase += pi2;
                signal[idx] = sinf(phase);
            }
        }
    }

    *num_samples = idx;
}

bool sbitx_ft8_init(void)
{
    mkdir(digi_spool_dir, 0755);
    return true;
}

void sbitx_ft8_shutdown(void) {}

int sbitx_ft8_encode(const char *message, float *signal, int max_samples,
                     float tone_freq)
{
    ftx_message_t msg;

    /* Pass NULL for the callsign-hash interface. ft8_lib guards every
     * callback with `if (hash_if != NULL)`, so NULL safely disables the
     * hash table (we don't need it for plain CQ/standard messages). The
     * old code passed a zeroed struct, which is non-NULL but has NULL
     * function pointers — pack28() then called save_hash() == NULL and
     * segfaulted. */
    ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, message);
    if (rc != FTX_MESSAGE_RC_OK)
        return -1;

    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    int n = max_samples;
    synth_gfsk(tones, FT8_NN, tone_freq, 12000, signal, &n);

    sbitx_ft8_spool_add("FT8", "tx", (int)(tone_freq / 1000.0f), message);
    return n;
}

int sbitx_ft8_decode(float *audio_12k, int nsamples, char *decoded,
                     int max_decoded_len)
{
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_%d.wav", digi_spool_dir, getpid());

    save_wav(audio_12k, nsamples, 12000, tmp_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "decode_ft8 %s 2>/dev/null", tmp_path);

    FILE *p = popen(cmd, "r");
    if (!p)
    {
        unlink(tmp_path);
        return 0;
    }

    int len = 0;
    while (len < max_decoded_len - 1 && fgets(decoded + len, max_decoded_len - len - 1, p))
        len = strlen(decoded);

    pclose(p);
    unlink(tmp_path);

    if (len > 0 && decoded[len - 1] == '\n')
        decoded[len - 1] = '\0';

    if (len > 0)
    {
        char *msg_start = strrchr(decoded, '~');
        if (msg_start)
        {
            msg_start += 2;
            char clean[256];
            snprintf(clean, sizeof(clean), "%s", msg_start);
            snprintf(decoded, max_decoded_len, "%s", clean);
            len = strlen(decoded);
        }
    }

    if (len > 0)
        sbitx_ft8_spool_add("FT8", "rx", 0, decoded);

    return len;
}

void spool_log_line(const char *mode, const char *dir, int freq_khz, const char *text)
{
    mkdir(digi_spool_dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/spool.log", digi_spool_dir);

    FILE *f = fopen(path, "a");
    if (f)
    {
        fprintf(f, "%s %s %d.%03d: %s\n", mode, dir,
                freq_khz / 1000, freq_khz % 1000, text);
        fclose(f);
    }
}

int sbitx_ft8_spool_count(void)
{
    DIR *d = opendir(digi_spool_dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        count++;
    }
    closedir(d);
    return count;
}

int sbitx_ft8_spool_read(int index, char *text, int max_len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/spool.log", digi_spool_dir);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int current = 0;
    while (fgets(text, max_len, f))
    {
        if (current == index)
        {
            int len = strlen(text);
            if (len > 0 && text[len - 1] == '\n')
                text[len - 1] = '\0';
            fclose(f);
            return len;
        }
        current++;
    }
    fclose(f);
    return -1;
}

void sbitx_ft8_spool_add(const char *mode, const char *dir, int freq_khz, const char *text)
{
    spool_log_line(mode, dir, freq_khz, text);
}
