/* ale_tool - ALE 2G calls and recordings, offline, through radiod's ALE engine.
 *
 *   ale_tool gen [-p TO|TIS|TWAS] [-w words] [-g gain] ADDR OUT.raw
 *       a scanning call: `words` repeats (default 32, 12.5 s) of one word
 *       addressed to ADDR (3 characters of the basic set), as PC-ALE's
 *       transmit chain makes it, gain times PC-ALE's TX amplitude (default 1).
 *   ale_tool rx [-k K,K,..] [-s 0|1] IN.raw
 *       the words both receivers decode in a recording.
 *
 * Audio: mono signed 16-bit, native byte order, 8 kHz (as modem_audio and
 * the RTP stream carry it).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ale_engine.h"

#define CALL_WORDS_DEFAULT 32
#define CALL_WORDS_MAX     1024
#define READ_BLOCK         4096
/* the combining receiver's K values unless -k gives others: radiod's default */
#define RX_DEFAULT_KS      { 4, 8, 16 }
#define RX_DEFAULT_NK      3

static void usage(void)
{
    fprintf(stderr,
            "usage: ale_tool gen [-p TO|TIS|TWAS] [-w words] [-g gain] ADDR OUT.raw\n"
            "       ale_tool rx [-k K,K,..] [-s 0|1] IN.raw\n");
    exit(2);
}

static int preamble_from(const char *s)
{
    if (!strcmp(s, "TO"))   return ALE_PREAMBLE_TO;
    if (!strcmp(s, "TIS"))  return ALE_PREAMBLE_TIS;
    if (!strcmp(s, "TWAS")) return ALE_PREAMBLE_TWAS;
    return -1;
}

static int gen(int argc, char **argv)
{
    int preamble = ALE_PREAMBLE_TO, words = CALL_WORDS_DEFAULT, c;
    double gain = 1.0;
    while ((c = getopt(argc, argv, "p:w:g:")) != -1)
    {
        if (c == 'p' && (preamble = preamble_from(optarg)) >= 0) continue;
        if (c == 'w' && (words = atoi(optarg)) > 0 && words <= CALL_WORDS_MAX) continue;
        if (c == 'g' && (gain = atof(optarg)) > 0) continue;
        usage();
    }
    if (argc - optind != 2)
        usage();
    char addr[4] = { 0 };
    for (int i = 0; i < 3 && argv[optind][i]; i++)
        addr[i] = (char) toupper((unsigned char) argv[optind][i]);

    size_t max = (size_t) words * ALE_ENGINE_WORD_SAMPLES;
    int16_t *pcm = malloc(max * sizeof(*pcm));
    if (!pcm)
        return 1;
    size_t n = ale_engine_make_call((uint8_t) preamble, addr, (unsigned) words, pcm, max);
    if (!n)
    {
        fprintf(stderr, "ale_tool: \"%s\" is not a 3-character address of the basic set\n", argv[optind]);
        return 1;
    }
    for (size_t i = 0; i < n; i++)
    {
        double v = pcm[i] * gain;
        pcm[i] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    FILE *f = fopen(argv[optind + 1], "wb");
    if (!f || fwrite(pcm, sizeof(*pcm), n, f) != n || fclose(f))
    {
        perror(argv[optind + 1]);
        return 1;
    }
    printf("%s %s x %d: %.2f s, %zu samples\n", ale_preamble_name((uint8_t) preamble), addr, words,
           n / (double) ALE_ENGINE_RATE, n);
    free(pcm);
    return 0;
}

static void on_word(const ale_word_event *ev, void *user)
{
    (void) user;
    printf("%9.2f s  %-4s %s  %-13s  unanimous %2u/48",
           ev->end_sample / (double) ALE_ENGINE_RATE, ale_preamble_name(ev->preamble), ev->text,
           ev->combined ? "" : "stock", ev->unanimous);
    if (ev->combined)
        printf("  combining K=%-2u tone %.1f dB", ev->combined, ev->tone_snr_db);
    else
        printf("  corrected %u", ev->fec_errors);
    printf("\n");
}

static int rx(int argc, char **argv)
{
    uint8_t ks[ALE_ENGINE_MAX_KS] = RX_DEFAULT_KS;
    size_t nk = RX_DEFAULT_NK;
    int stock = 1, c;
    while ((c = getopt(argc, argv, "k:s:")) != -1)
    {
        if (c == 'k')
        {
            nk = 0;
            for (char *tok = strtok(optarg, ","); tok; tok = strtok(NULL, ","))
            {
                if (nk == ALE_ENGINE_MAX_KS)
                    usage();
                ks[nk++] = (uint8_t) atoi(tok);
            }
            continue;
        }
        if (c == 's') { stock = atoi(optarg) != 0; continue; }
        usage();
    }
    if (argc - optind != 1)
        usage();
    ale_engine *e = ale_engine_create(ks, nk, on_word, NULL);
    if (!e)
    {
        fprintf(stderr, "ale_tool: each K is 2 to %d\n", ALE_ENGINE_MAX_K);
        return 2;
    }
    ale_engine_set_stock(e, stock);
    FILE *f = fopen(argv[optind], "rb");
    if (!f)
    {
        perror(argv[optind]);
        return 1;
    }
    int16_t buf[READ_BLOCK];
    size_t n;
    while ((n = fread(buf, sizeof(buf[0]), READ_BLOCK, f)) > 0)
        ale_engine_feed(e, buf, n);
    fclose(f);
    printf("%.1f s of audio\n", ale_engine_position(e) / (double) ALE_ENGINE_RATE);
    ale_engine_destroy(e);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        usage();
    if (!strcmp(argv[1], "gen"))
        return gen(argc - 1, argv + 1);
    if (!strcmp(argv[1], "rx"))
        return rx(argc - 1, argv + 1);
    usage();
    return 2;
}
