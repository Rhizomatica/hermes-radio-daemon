/* js8_frame - JS8's frame: alphabet, CRC-12, (174,87) LDPC, tones.
 * See js8_frame.h.  Ported from JS8Call-improved's JS8_Mode/JS8.cpp
 * (GPL-3.0): encode(), CRC12(), checkCRC12(), extractmessage174() and
 * bpdecode174() without its rescue and feedback extras.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "js8_frame.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "js8_ldpc_tables.h"

#define JS8_ALPHABET      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+"
#define JS8_WORD_BITS     6
#define JS8_PAYLOAD_BITS  (JS8_CHARS * JS8_WORD_BITS)   /* 72 */
#define JS8_CRC_POLY      0xc06
#define JS8_CRC_BITS      12
#define JS8_CRC_MASK      ((1u << JS8_CRC_BITS) - 1)
#define JS8_CRC_XOR       42        /* JS8Call's final xor */
#define JS8_FRAME_BYTES   11        /* 87 bits and one spare */
#define JS8_TYPE_MASK     ((1 << JS8_TYPE_BITS) - 1)
#define JS8_SYMBOL_BITS   3
#define JS8_LLR_CLIP      8.0f      /* channel confidence bound (bp_math.h) */

static const int js8_costas_tones[2][3][JS8_COSTAS_LEN] = {
    { { 4, 2, 5, 6, 1, 3, 0 }, { 4, 2, 5, 6, 1, 3, 0 }, { 4, 2, 5, 6, 1, 3, 0 } },
    { { 0, 6, 2, 3, 5, 4, 1 }, { 1, 5, 0, 2, 3, 6, 4 }, { 2, 5, 0, 6, 4, 1, 3 } },
};

const int (*js8_costas(js8_costas_type type))[JS8_COSTAS_LEN]
{
    return js8_costas_tones[type == JS8_COSTAS_MODIFIED];
}

int js8_char_word(char c)
{
    const char *p = c ? strchr(JS8_ALPHABET, c) : NULL;
    return p ? (int) (p - JS8_ALPHABET) : -1;
}

/* An augmented CRC (boost::augmented_crc): the data already carries the
 * CRC's room as zero bits, so each bit just goes through the register. */
uint16_t js8_crc12(const uint8_t bytes[JS8_FRAME_BYTES])
{
    unsigned rem = 0;
    for (int i = 0; i < JS8_FRAME_BYTES * 8; i++) {
        unsigned top = (rem >> (JS8_CRC_BITS - 1)) & 1;
        rem = ((rem << 1) | ((bytes[i / 8] >> (7 - i % 8)) & 1)) & JS8_CRC_MASK;
        if (top)
            rem ^= JS8_CRC_POLY;
    }
    return (uint16_t) (rem ^ JS8_CRC_XOR);
}

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
}

/* Generator row `row`, column `col`: its hex string, most significant
 * bit first. */
static int generator(int row, int col)
{
    return (hex_value(js8_generator_hex[row][col / 4]) >> (3 - col % 4)) & 1;
}

static int bit_at(const uint8_t *bytes, int i)
{
    return (bytes[i / 8] >> (7 - i % 8)) & 1;
}

bool js8_encode(const char msg[JS8_CHARS], int type, js8_costas_type costas, int tones[JS8_NN])
{
    uint8_t bytes[JS8_FRAME_BYTES] = { 0 };
    for (int i = 0, j = 0; i < JS8_CHARS; i += 4, j += 3) {
        uint32_t words = 0;
        for (int k = 0; k < 4; k++) {
            int w = js8_char_word(msg[i + k]);
            if (w < 0)
                return false;
            words = (words << JS8_WORD_BITS) | (uint32_t) w;
        }
        bytes[j] = (uint8_t) (words >> 16);
        bytes[j + 1] = (uint8_t) (words >> 8);
        bytes[j + 2] = (uint8_t) words;
    }
    bytes[9] = (uint8_t) ((type & JS8_TYPE_MASK) << 5);
    uint16_t crc = js8_crc12(bytes);
    bytes[9] |= (crc >> 7) & 0x1F;
    bytes[10] = (uint8_t) ((crc & 0x7F) << 1);

    const int (*c)[JS8_COSTAS_LEN] = js8_costas(costas);
    for (int a = 0; a < 3; a++)
        memcpy(tones + a * JS8_COSTAS_STEP, c[a], sizeof c[a]);

    int *parity = tones + JS8_PARITY_AT, *data = tones + JS8_DATA_AT;
    int pw = 0, dw = 0, nbits = 0;
    for (int i = 0; i < JS8_K; i++) {
        int p = 0;
        for (int j = 0; j < JS8_K; j++)
            p ^= generator(i, j) & bit_at(bytes, j);
        pw = (pw << 1) | p;
        dw = (dw << 1) | bit_at(bytes, i);
        if (++nbits == JS8_SYMBOL_BITS) {
            *parity++ = pw;
            *data++ = dw;
            pw = dw = nbits = 0;
        }
    }
    return true;
}

static float clip_llr(float v)
{
    return isnan(v) ? 0.0f : v < -JS8_LLR_CLIP ? -JS8_LLR_CLIP : v > JS8_LLR_CLIP ? JS8_LLR_CLIP : v;
}

static float check_message(float product)
{
    const float limit = 1.0f - FLT_EPSILON;
    product = product < -limit ? -limit : product > limit ? limit : product;
    return 2.0f * atanhf(-product);
}

/* Which of bit b's checks check c is. */
static int slot_of(int b, int c)
{
    for (int k = 0; k < JS8_MN_MAX; k++)
        if (js8_Mn[b][k] == c)
            return k;
    return -1;
}

int js8_ldpc_decode(const float llr[JS8_N], int max_iterations, uint8_t bits[JS8_K])
{
    float in[JS8_N], zn[JS8_N];
    float tov[JS8_N][JS8_MN_MAX] = { { 0 } };
    float toc[JS8_M][JS8_NM_MAX], tanhtoc[JS8_M][JS8_NM_MAX];
    uint8_t cw[JS8_N];
    int ncnt = 0, nclast = 0;

    for (int i = 0; i < JS8_N; i++)
        in[i] = clip_llr(llr[i]);
    for (int i = 0; i < JS8_M; i++)
        for (int j = 0; j < js8_Nm[i].n; j++)
            toc[i][j] = in[js8_Nm[i].bit[j]];

    for (int iter = 0; iter <= max_iterations; iter++) {
        for (int i = 0; i < JS8_N; i++) {
            zn[i] = in[i];
            for (int k = 0; k < JS8_MN_MAX; k++)
                zn[i] += tov[i][k];
            cw[i] = zn[i] > 0;
        }
        int ncheck = 0;
        for (int i = 0; i < JS8_M; i++) {
            int s = 0;
            for (int j = 0; j < js8_Nm[i].n; j++)
                s += cw[js8_Nm[i].bit[j]];
            ncheck += s & 1;
        }
        if (ncheck == 0) {
            memcpy(bits, cw + JS8_M, JS8_K);
            int nerr = 0;
            for (int i = 0; i < JS8_N; i++)
                if ((2 * cw[i] - 1) * llr[i] < 0.0f)
                    nerr++;
            return nerr;
        }
        /* JS8Call's early stop: no progress for 5 iterations past the
         * 10th, with many checks still failing. */
        if (iter > 0) {
            ncnt = ncheck - nclast < 0 ? 0 : ncnt + 1;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15)
                return -1;
        }
        nclast = ncheck;

        for (int i = 0; i < JS8_M; i++)
            for (int j = 0; j < js8_Nm[i].n; j++) {
                int b = js8_Nm[i].bit[j];
                toc[i][j] = zn[b] - tov[b][slot_of(b, i)];
                tanhtoc[i][j] = tanhf(-toc[i][j] / 2.0f);
            }
        for (int i = 0; i < JS8_M; i++)
            for (int e = 0; e < js8_Nm[i].n; e++) {
                float t = 1.0f;
                for (int k = 0; k < js8_Nm[i].n; k++)
                    if (k != e)
                        t *= tanhtoc[i][k];
                int b = js8_Nm[i].bit[e];
                tov[b][slot_of(b, i)] = check_message(t);
            }
    }
    return -1;
}

bool js8_unpack(const uint8_t bits[JS8_K], char *msg, int *type)
{
    uint8_t bytes[JS8_FRAME_BYTES] = { 0 };
    for (int i = 0; i < JS8_K; i++)
        if (bits[i])
            bytes[i / 8] |= (uint8_t) (1 << (7 - i % 8));
    uint16_t crc = (uint16_t) (((bytes[9] & 0x1F) << 7) | (bytes[10] >> 1));
    bytes[9] &= 0xE0;
    bytes[10] = 0;
    if (crc != js8_crc12(bytes))
        return false;
    for (int i = 0; i < JS8_CHARS; i++) {
        int w = 0;
        for (int k = 0; k < JS8_WORD_BITS; k++)
            w = (w << 1) | bits[i * JS8_WORD_BITS + k];
        msg[i] = JS8_ALPHABET[w];
    }
    msg[JS8_CHARS] = '\0';
    if (type) {
        int t = 0;
        for (int k = 0; k < JS8_TYPE_BITS; k++)
            t = (t << 1) | bits[JS8_PAYLOAD_BITS + k];
        *type = t;
    }
    return true;
}
