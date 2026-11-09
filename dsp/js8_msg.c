/* js8_msg - JS8Call's Varicode, receiving side.  See js8_msg.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "js8_msg.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "js8_jsc_words.h"

/* Varicode's frame types: the 72 bits' first three. */
#define FRAME_HEARTBEAT          0
#define FRAME_COMPOUND           1
#define FRAME_COMPOUND_DIRECTED  2
#define FRAME_DIRECTED           3
#define FRAME_FLAG_BITS          3

#define FRAME_BITS     72
#define VALUE_BITS     64              /* the 72 bits as a 64-bit value... */
#define REM_BITS       8               /* ...and 8 more */
#define ALNUM          "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@"
#define ALNUM_N        38              /* the letters, digits, space and '/' */
#define ALNUM_FIRST_N  39              /* the first character may be '@' too */
#define LETTERS_N      27              /* A-Z and space, from ALNUM[10] */
#define LETTERS_AT     10
#define NBASECALL      (37u * 36 * 10 * 27 * 27 * 27)
#define NBASEGRID      (180 * 180)
#define NUSERGRID      (NBASEGRID + 10)
#define NMAXGRID       ((1 << 15) - 1)
#define GRID_ALT_BIT   (1 << 15)       /* a heartbeat's: a CQ, not an HB */
#define CMD_SNR        25
#define CMD_HB_SNR     29
#define CMD_COUNT      32
#define NUM_BIAS       31              /* a number or SNR, sent + 31 */
#define SNR_SHOWN_MAX  60
#define PORTABLE_FROM  0x80            /* a directed frame's last 8 bits */
#define PORTABLE_TO    0x40
#define NUM_MASK       0x3f

/* JS8Call's directed commands by code: the first name of each in its
 * QMap's (sorted) order, as directed_cmds.key() gives it. */
static const char *const CMDS[CMD_COUNT] = {
    " SNR?", " DIT DIT", " NACK", " HEARING?", " GRID?", ">", " STATUS?", " STATUS",
    " HEARING", " MSG", " MSG TO:", " QUERY", " QUERY MSGS", " QUERY CALL", " ACK", " GRID",
    " INFO?", " INFO", " FB", " HW CPY?", " SK", " RR", " QSL?", " QSL",
    " CMD", " SNR", " NO", " YES", " 73", " HEARTBEAT SNR", " AGN?", " ",
};

static const char *const CQS[] = {
    "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD", "CQ CQ", "CQ",
};

/* The group callsigns, from NBASECALL + 1. */
static const char *const GROUPS[] = {
    "<....>", "@ALLCALL", "@JS8NET", "@DX/NA", "@DX/SA", "@DX/EU", "@DX/AS", "@DX/AF",
    "@DX/OC", "@DX/AN", "@REGION/1", "@REGION/2", "@REGION/3", "@GROUP/0", "@GROUP/1",
    "@GROUP/2", "@GROUP/3", "@GROUP/4", "@GROUP/5", "@GROUP/6", "@GROUP/7", "@GROUP/8",
    "@GROUP/9", "@COMMAND", "@CONTROL", "@NET", "@NTS", "@RESERVE/0", "@RESERVE/1",
    "@RESERVE/2", "@RESERVE/3", "@RESERVE/4", "@APRSIS", "@RAGCHEW", "@JS8", "@EMCOMM",
    "@ARES", "@MARS", "@AMRRON", "@RACES", "@RAYNET", "@RADAR", "@SKYWARN", "@CQ", "@HB",
    "@QSO", "@QSOPARTY", "@CONTEST", "@FIELDDAY", "@SOTA", "@IOTA", "@POTA", "@QRP", "@QRO",
};
#define NGROUPS ((int) (sizeof GROUPS / sizeof GROUPS[0]))

/* Varicode's Huffman table for text. */
static const struct { char c; const char *code; } HUFF[] = {
    { ' ', "01" }, { 'E', "100" }, { 'T', "1101" }, { 'A', "0011" }, { 'O', "11111" },
    { 'I', "11100" }, { 'N', "10111" }, { 'S', "10100" }, { 'H', "00011" }, { 'R', "00000" },
    { 'D', "111011" }, { 'L', "110011" }, { 'C', "110001" }, { 'U', "101101" },
    { 'M', "101011" }, { 'W', "001011" }, { 'F', "001001" }, { 'G', "000101" },
    { 'Y', "000011" }, { 'P', "1111011" }, { 'B', "1111001" }, { '.', "1110100" },
    { 'V', "1100101" }, { 'K', "1100100" }, { '-', "1100001" }, { '+', "1100000" },
    { '?', "1011001" }, { '!', "1011000" }, { '"', "1010101" }, { 'X', "1010100" },
    { '0', "0010101" }, { 'J', "0010100" }, { '1', "0010001" }, { 'Q', "0010000" },
    { '2', "0001001" }, { 'Z', "0001000" }, { '3', "0000101" }, { '5', "0000100" },
    { '4', "11110101" }, { '9', "11110100" }, { '8', "11110001" }, { '6', "11110000" },
    { '7', "11101011" }, { '/', "11101010" },
};
#define NHUFF ((int) (sizeof HUFF / sizeof HUFF[0]))

/* The 72 bits, MSB first. */
typedef struct { uint8_t b[FRAME_BITS]; } bits72;

static bool frame_bits(const char *frame, bits72 *out)
{
    if (!frame || strlen(frame) < JS8_CHARS || strchr(frame, ' '))
        return false;
    for (int i = 0; i < JS8_CHARS; i++) {
        int w = js8_char_word(frame[i]);
        if (w < 0)
            return false;
        for (int k = 0; k < 6; k++)
            out->b[i * 6 + k] = (uint8_t) ((w >> (5 - k)) & 1);
    }
    return true;
}

static uint64_t field(const bits72 *v, int at, int n)
{
    uint64_t x = 0;
    for (int i = 0; i < n; i++)
        x = (x << 1) | v->b[at + i];
    return x;
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ')
        p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = '\0';
}

/* Varicode::unpackAlphaNumeric50: eleven characters, spaces removed. */
static void unpack_alnum50(uint64_t p, char *out, size_t size)
{
    char w[12];
    w[10] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[9] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[8] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[7] = p % 2 ? '/' : ' '; p /= 2;
    w[6] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[5] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[4] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[3] = p % 2 ? '/' : ' '; p /= 2;
    w[2] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[1] = ALNUM[p % ALNUM_N]; p /= ALNUM_N;
    w[0] = ALNUM[p % ALNUM_FIRST_N];
    w[11] = '\0';
    size_t n = 0;
    for (int i = 0; i < 11 && n + 1 < size; i++)
        if (w[i] != ' ')
            out[n++] = w[i];
    out[n] = '\0';
}

/* Varicode::unpackCallsign: a group, or six characters of a standard
 * callsign, with JS8Call's Swaziland and Guinea workarounds undone. */
static void unpack_call28(uint32_t v, bool portable, char *out, size_t size)
{
    if (v > NBASECALL && v - NBASECALL <= (uint32_t) NGROUPS) {
        snprintf(out, size, "%s", GROUPS[v - NBASECALL - 1]);
        return;
    }
    char w[7];
    w[5] = ALNUM[v % LETTERS_N + LETTERS_AT]; v /= LETTERS_N;
    w[4] = ALNUM[v % LETTERS_N + LETTERS_AT]; v /= LETTERS_N;
    w[3] = ALNUM[v % LETTERS_N + LETTERS_AT]; v /= LETTERS_N;
    w[2] = ALNUM[v % 10]; v /= 10;
    w[1] = ALNUM[v % 36]; v /= 36;
    w[0] = v < sizeof ALNUM - 1 ? ALNUM[v] : ' ';
    w[6] = '\0';
    char call[16];
    if (!strncmp(w, "3D0", 3))
        snprintf(call, sizeof call, "3DA0%s", w + 3);
    else if (w[0] == 'Q' && w[1] >= 'A' && w[1] <= 'Z')
        snprintf(call, sizeof call, "3X%s", w + 1);
    else
        snprintf(call, sizeof call, "%s", w);
    trim(call);
    snprintf(out, size, portable ? "%s/P" : "%s", call);
}

/* Varicode::unpackGrid: a 4-character locator, "" past NBASEGRID.  Its
 * longitude counts west, as WSJT-X's deg2grid. */
static void unpack_grid(unsigned v, char *out, size_t size)
{
    out[0] = '\0';
    if (v > NBASEGRID || size < 5)
        return;
    float dlat = (float) (v % 180) - 90;
    float dlong = (float) (v / 180 * 2) - 180 + 2;
    int nlong = (int) (60.0 * (180.0 - dlong) / 5);
    int nlat = (int) (60.0 * (dlat + 90) / 2.5);
    out[0] = (char) ('A' + nlong / 240);
    out[1] = (char) ('A' + nlat / 240);
    out[2] = (char) ('0' + (nlong - 240 * (nlong / 240)) / 24);
    out[3] = (char) ('0' + (nlat - 240 * (nlat / 240)) / 24);
    out[4] = '\0';
}

static const char *cmd_name(unsigned code)
{
    return code < CMD_COUNT ? CMDS[code] : "";
}

static bool is_snr_cmd(unsigned code) { return code == CMD_SNR || code == CMD_HB_SNR; }

/* Varicode::formatSNR: "+05", "-12"; "" out of range. */
static void format_snr(int snr, char *out, size_t size)
{
    if (snr < -SNR_SHOWN_MAX || snr > SNR_SHOWN_MAX)
        out[0] = '\0';
    else if (snr >= 0)
        snprintf(out, size, "+%02d", snr);
    else
        snprintf(out, size, "-%02d", -snr);
}

/* JSC: (s,c)-dense coding of word indices into its word list, in 4-bit
 * groups (JSC::decompress).  A group below JSC_S ends a word and is
 * followed by a bit: 1 for a space after the word. */
#define JSC_GROUP_BITS 4
#define JSC_S          7
#define JSC_C          ((1 << JSC_GROUP_BITS) - JSC_S)
#define JSC_MAX_GROUPS 8          /* JSC's base[] table: up to 7 continuations */
#define JSC_MAX_WORDS  32         /* 72 bits hold far fewer */

static uint32_t       g_jsc_offset[JS8_JSC_WORDS];
static pthread_once_t g_jsc_once = PTHREAD_ONCE_INIT;

static void jsc_index(void)
{
    const char *p = js8_jsc_words;
    for (int i = 0; i < JS8_JSC_WORDS; i++) {
        g_jsc_offset[i] = (uint32_t) (p - js8_jsc_words);
        p += strlen(p) + 1;
    }
}

/* Appends a Latin-1 string as UTF-8. */
static size_t put_latin1(char *out, size_t n, size_t size, const char *w)
{
    for (const unsigned char *c = (const unsigned char *) w; *c; c++) {
        if (*c < 0x80) {
            if (n + 1 < size)
                out[n++] = (char) *c;
        } else if (n + 2 < size) {
            out[n++] = (char) (0xC0 | *c >> 6);
            out[n++] = (char) (0x80 | (*c & 0x3F));
        }
    }
    out[n] = '\0';
    return n;
}

static bool jsc_decompress(const uint8_t *bits, int count, char *out, size_t size)
{
    pthread_once(&g_jsc_once, jsc_index);
    uint32_t base[JSC_MAX_GROUPS], pw = JSC_S;
    base[0] = 0;
    for (int k = 1; k < JSC_MAX_GROUPS; k++) {
        base[k] = base[k - 1] + pw;
        pw *= JSC_C;
    }
    unsigned groups[FRAME_BITS / JSC_GROUP_BITS + 1];
    bool space[FRAME_BITS / JSC_GROUP_BITS + 1] = { false };
    int ng = 0;
    for (int i = 0; i + JSC_GROUP_BITS <= count;) {
        unsigned g = 0;
        for (int k = 0; k < JSC_GROUP_BITS; k++)
            g = g << 1 | bits[i + k];
        i += JSC_GROUP_BITS;
        groups[ng] = g;
        if (g < JSC_S) {
            space[ng] = i < count && bits[i];
            i++;
        }
        ng++;
    }
    size_t n = 0;
    out[0] = '\0';
    for (int start = 0, words = 0; start < ng && words < JSC_MAX_WORDS; words++) {
        uint32_t j = 0;
        int k = 0;
        while (start + k < ng && groups[start + k] >= JSC_S) {
            j = j * JSC_C + (groups[start + k] - JSC_S);
            k++;
        }
        if (j >= JS8_JSC_WORDS || start + k >= ng || k >= JSC_MAX_GROUPS)
            break;
        j = j * JSC_S + groups[start + k] + base[k];
        if (j >= JS8_JSC_WORDS)
            break;
        n = put_latin1(out, n, size, js8_jsc_words + g_jsc_offset[j]);
        if (space[start + k] && n + 1 < size) {
            out[n++] = ' ';
            out[n] = '\0';
        }
        start += k + 1;
    }
    return n > 0;
}

/* The last 0 among bits [from, FRAME_BITS): where the padding starts. */
static int last_zero(const bits72 *v, int from)
{
    int z = -1;
    for (int i = from; i < FRAME_BITS; i++)
        if (!v->b[i])
            z = i;
    return z;
}

/* Data: [1][compressed][bits..., 0, 1...]: Huffman-coded text, or JSC. */
static bool unpack_data(const bits72 *v, char *text, size_t size)
{
    if (!v->b[0])
        return false;
    if (v->b[1]) {
        int end = last_zero(v, 1);
        return end > 2 && jsc_decompress(v->b + 2, end - 2, text, size);
    }
    int at = 2, end = last_zero(v, 1);   /* after the two flags, up to the pad's 0 */
    size_t n = 0;
    while (at < end) {
        int k = 0;
        for (; k < NHUFF; k++) {
            size_t len = strlen(HUFF[k].code);
            if (at + (int) len > end)
                continue;
            bool match = true;
            for (size_t j = 0; j < len && match; j++)
                match = v->b[at + j] == (uint8_t) (HUFF[k].code[j] - '0');
            if (match) {
                if (n + 1 < size)
                    text[n++] = HUFF[k].c;
                at += (int) len;
                break;
            }
        }
        if (k == NHUFF)
            break;
    }
    text[n] = '\0';
    return n > 0;
}

void js8_msg_unpack(const char *frame, int type, js8_msg *m)
{
    memset(m, 0, sizeof *m);
    m->kind = JS8_MSG_RAW;
    m->first = type & JS8_TX_FIRST;
    m->last = type & JS8_TX_LAST;
    snprintf(m->text, sizeof m->text, "%s", frame ? frame : "");
    bits72 v;
    if (!frame_bits(frame, &v))
        return;
    if (type & JS8_TX_DATA) {   /* data only: all 72 bits are JSC */
        int end = last_zero(&v, 0);
        if (end > 0 && jsc_decompress(v.b, end, m->text, sizeof m->text))
            m->kind = JS8_MSG_DATA;
        else
            snprintf(m->text, sizeof m->text, "%s", frame);
        return;
    }

    if (unpack_data(&v, m->text, sizeof m->text)) {
        m->kind = JS8_MSG_DATA;
        return;
    }
    if (v.b[0]) {   /* data that does not decode */
        snprintf(m->text, sizeof m->text, "%s", frame);
        return;
    }

    unsigned flag = (unsigned) field(&v, 0, FRAME_FLAG_BITS);
    uint64_t rem = field(&v, VALUE_BITS, REM_BITS);
    if (flag == FRAME_HEARTBEAT || flag == FRAME_COMPOUND || flag == FRAME_COMPOUND_DIRECTED) {
        /* [3][50][11],[5][3] */
        unpack_alnum50(field(&v, 3, 50), m->from, sizeof m->from);
        unsigned num = (unsigned) (field(&v, 53, 11) << 5 | rem >> 3);
        unsigned bits3 = (unsigned) (rem & 7);
        if (flag == FRAME_HEARTBEAT) {
            bool cq = num & GRID_ALT_BIT;
            unpack_grid(num & (GRID_ALT_BIT - 1), m->grid, sizeof m->grid);
            m->kind = cq ? JS8_MSG_CQ : JS8_MSG_HEARTBEAT;
            snprintf(m->text, sizeof m->text, "%s: %s%s %s", m->from,
                     cq ? "@ALLCALL " : "@HB ", cq ? CQS[bits3] : "HEARTBEAT", m->grid);
        } else if (flag == FRAME_COMPOUND) {
            if (num <= NBASEGRID)
                unpack_grid(num, m->grid, sizeof m->grid);
            m->kind = JS8_MSG_COMPOUND;
            snprintf(m->text, sizeof m->text, "%s:", m->from);
        } else {
            char extra[32] = "";
            if (num >= NUSERGRID && num < NMAXGRID) {
                unsigned c = (num - NUSERGRID) & 0xff, n = 0;
                if (c & 0x80) {
                    n = c & NUM_MASK;
                    c = c & 0x40 ? CMD_HB_SNR : CMD_SNR;
                } else {
                    c &= 0x7f;
                }
                char snr[8] = "";
                if (is_snr_cmd(c))
                    format_snr((int) n - NUM_BIAS, snr, sizeof snr);
                snprintf(extra, sizeof extra, "%s%s%s", cmd_name(c), snr[0] ? " " : "", snr);
            } else if (num <= NBASEGRID) {
                char g[JS8_MSG_GRID_MAX];
                unpack_grid(num, g, sizeof g);
                snprintf(extra, sizeof extra, " %s", g);
            }
            m->kind = JS8_MSG_COMPOUND_DIRECTED;
            snprintf(m->text, sizeof m->text, "%s%s", m->from, extra);
        }
        trim(m->text);
        return;
    }
    if (flag == FRAME_DIRECTED) {
        /* [3][28][28][5],[2][6] */
        unpack_call28((uint32_t) field(&v, 3, 28), rem & PORTABLE_FROM, m->from, sizeof m->from);
        unpack_call28((uint32_t) field(&v, 31, 28), rem & PORTABLE_TO, m->to, sizeof m->to);
        unsigned c = (unsigned) field(&v, 59, 5) % CMD_COUNT;
        int extra = (int) (rem % (NUM_MASK + 1));
        char num[8] = "";
        if (extra) {
            if (is_snr_cmd(c))
                format_snr(extra - NUM_BIAS, num, sizeof num);
            else
                snprintf(num, sizeof num, "%d", extra - NUM_BIAS);
        }
        m->kind = JS8_MSG_DIRECTED;
        snprintf(m->text, sizeof m->text, "%s: %s%s%s%s", m->from, m->to, cmd_name(c),
                 num[0] ? " " : "", num);
        trim(m->text);
    }
}
