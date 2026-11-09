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
#include <stdlib.h>
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

/* ── packing: JS8Call's Varicode, sending side ───────────────────── */

#define HB_TYPES        "HEARTBEAT"
#define ALNUM41         "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?"   /* Varicode's alphabet */
#define ALNUM41_N       41
#define JSC_ALLCALL     81        /* "@ALLCALL": JS8Call's table takes only its 7 first */
#define JSC_ALLCALL_LEN 7
#define CMD_FREETEXT    31        /* " ": the rest is text */
#define CHECKSUM16_LEN  3
#define NUM_MIN         (-30)     /* Varicode::packNum's range */
#define NUM_MAX         31
#define LINE_MAX        512
#define CALL_MAX        16
#define GRID_LEN        4

/* The 72 bits, built MSB first, as twelve frame characters. */
typedef struct { uint8_t b[FRAME_BITS]; int n; } bitbuf;

static void put_bits(bitbuf *v, uint64_t x, int n)
{
    for (int i = n - 1; i >= 0 && v->n < FRAME_BITS; i--)
        v->b[v->n++] = (uint8_t) ((x >> i) & 1);
}

static void to_frame(const bitbuf *v, char out[JS8_CHARS + 1])
{
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";
    for (int i = 0; i < JS8_CHARS; i++) {
        int w = 0;
        for (int k = 0; k < 6; k++)
            w = w << 1 | v->b[i * 6 + k];
        out[i] = alphabet[w];
    }
    out[JS8_CHARS] = '\0';
}

/* Pads to 72 bits: a 0, then 1s. */
static void pad_frame(bitbuf *v)
{
    if (v->n < FRAME_BITS)
        put_bits(v, 0, 1);
    while (v->n < FRAME_BITS)
        put_bits(v, 1, 1);
}

static int alnum_index(char c)
{
    const char *p = c ? strchr(ALNUM, c) : NULL;
    return p ? (int) (p - ALNUM) : -1;
}

static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_word(char c) { return is_upper(c) || is_digit(c) || c == '_' || (c >= 'a' && c <= 'z'); }

/* Varicode::packAlphaNumeric50: eleven characters, spaces put in at 3 and
 * 7 unless a '/' is there; 0 if it does not fit. */
static uint64_t pack_alnum50(const char *value)
{
    char w[16];
    int n = 0;
    for (const char *p = value; *p && n < 15; p++)
        if (alnum_index(*p) >= 0 && *p != ' ')
            w[n++] = *p;
    w[n] = '\0';
    char v[12];
    int j = 0;
    for (int i = 0; w[i] && j < 11; i++) {
        if ((j == 3 || j == 7) && w[i] != '/')
            v[j++] = ' ';
        if (j < 11)
            v[j++] = w[i];
    }
    if (n > 0 && j >= 11 && (int) strlen(w) > 9)
        return 0;
    while (j < 11)
        v[j++] = ' ';
    int idx[11];
    for (int i = 0; i < 11; i++)
        if ((idx[i] = alnum_index(v[i])) < 0)
            return 0;
    if (idx[0] >= ALNUM_FIRST_N || (v[0] != '@' && idx[0] >= ALNUM_N))
        return 0;
    uint64_t p = (uint64_t) idx[0];
    p = p * ALNUM_N + (uint64_t) idx[1];
    p = p * ALNUM_N + (uint64_t) idx[2];
    p = p * 2 + (v[3] == '/');
    p = p * ALNUM_N + (uint64_t) idx[4];
    p = p * ALNUM_N + (uint64_t) idx[5];
    p = p * ALNUM_N + (uint64_t) idx[6];
    p = p * 2 + (v[7] == '/');
    p = p * ALNUM_N + (uint64_t) idx[8];
    p = p * ALNUM_N + (uint64_t) idx[9];
    p = p * ALNUM_N + (uint64_t) idx[10];
    return p;
}

static int group_index(const char *call)
{
    for (int i = 0; i < NGROUPS; i++)
        if (!strcmp(GROUPS[i], call))
            return i;
    return -1;
}

/* The six characters of a standard callsign, "([0-9A-Z ])([0-9A-Z])([0-9])
 * ([A-Z ])([A-Z ])([A-Z ])", as Varicode::packCallsign pads it. */
static bool fit_call6(const char *call, char out[7])
{
    int len = (int) strlen(call);
    char tries[3][8];
    int nt = 0;
    snprintf(tries[nt++], sizeof tries[0], "%s", call);
    if (len == 2) snprintf(tries[nt++], sizeof tries[0], " %s   ", call);
    if (len == 3) { snprintf(tries[nt++], sizeof tries[0], " %s  ", call); snprintf(tries[nt++], sizeof tries[0], "%s   ", call); }
    if (len == 4) { snprintf(tries[nt++], sizeof tries[0], " %s ", call); snprintf(tries[nt++], sizeof tries[0], "%s  ", call); }
    if (len == 5) { snprintf(tries[nt++], sizeof tries[0], " %s", call); snprintf(tries[nt++], sizeof tries[0], "%s ", call); }
    for (int t = 0; t < nt; t++) {
        const char *s = tries[t];
        if (strlen(s) < 6)
            continue;
        if ((is_upper(s[0]) || is_digit(s[0]) || s[0] == ' ') && (is_upper(s[1]) || is_digit(s[1])) &&
            is_digit(s[2]) && (is_upper(s[3]) || s[3] == ' ') && (is_upper(s[4]) || s[4] == ' ') &&
            (is_upper(s[5]) || s[5] == ' ')) {
            memcpy(out, s, 6);
            out[6] = '\0';
            return true;
        }
    }
    return false;
}

/* Varicode::packCallsign: a group, or a standard callsign in 28 bits (and
 * /P as a flag); 0 if it is neither. */
static uint32_t pack_call28(const char *value, bool *portable)
{
    *portable = false;
    int g = group_index(value);
    if (g >= 0)
        return NBASECALL + 1 + (uint32_t) g;
    char call[CALL_MAX];
    snprintf(call, sizeof call, "%s", value);
    size_t len = strlen(call);
    if (len > 2 && !strcmp(call + len - 2, "/P")) {
        call[len - 2] = '\0';
        *portable = true;
    }
    char tmp[CALL_MAX];
    if (!strncmp(call, "3DA0", 4)) {          /* Swaziland */
        snprintf(tmp, sizeof tmp, "3D0%s", call + 4);
        snprintf(call, sizeof call, "%s", tmp);
    } else if (!strncmp(call, "3X", 2) && is_upper(call[2])) {   /* Guinea */
        snprintf(tmp, sizeof tmp, "Q%s", call + 2);
        snprintf(call, sizeof call, "%s", tmp);
    }
    len = strlen(call);
    char six[7];
    if (len < 2 || len > 6 || !fit_call6(call, six))
        return 0;
    uint32_t p = (uint32_t) alnum_index(six[0]);
    p = 36 * p + (uint32_t) alnum_index(six[1]);
    p = 10 * p + (uint32_t) alnum_index(six[2]);
    for (int i = 3; i < 6; i++)
        p = 27 * p + (uint32_t) (alnum_index(six[i]) - LETTERS_AT);
    return p;
}

/* "[0-9][A-Z]" or "[A-Z][0-9]" somewhere: a callsign, not a word. */
static bool has_letter_digit(const char *s)
{
    for (; s[0] && s[1]; s++)
        if ((is_digit(s[0]) && is_upper(s[1])) || (is_upper(s[0]) && is_digit(s[1])))
            return true;
    return false;
}

/* Varicode::isValidCallsign's base pattern: optional character, one more,
 * a digit, up to three letters, optional /P. */
static bool is_base_call(const char *c)
{
    char call[CALL_MAX];
    snprintf(call, sizeof call, "%s", c);
    size_t len = strlen(call);
    if (len > 2 && !strcmp(call + len - 2, "/P"))
        call[len - 2] = '\0';
    char six[7];
    len = strlen(call);
    return len >= 2 && len <= 6 && fit_call6(call, six);
}

/* Varicode::isValidCallsign: a group, a base callsign, or a compound one
 * (with '/', or a group, up to 9 characters besides the '/'s). */
static bool valid_call(const char *call, bool *compound)
{
    *compound = false;
    if (group_index(call) >= 0)
        return true;
    if (is_base_call(call))
        return strlen(call) > 2 && has_letter_digit(call);
    int slashes = 0, len = 0;
    for (const char *p = call; *p; p++, len++) {
        if (*p == '/')
            slashes++;
        else if (!is_upper(*p) && !is_digit(*p) && !(p == call && *p == '@'))
            return false;
    }
    if (len - slashes > 9 || len == 0)
        return false;
    bool ok;
    const char *slash = strchr(call, '/');
    if (slash) {
        char first[CALL_MAX];
        snprintf(first, sizeof first, "%.*s", (int) (slash - call), call);
        ok = group_index(first) < 0;
    } else {
        ok = call[0] == '@' || (len > 2 && has_letter_digit(call));
    }
    *compound = ok;
    return ok;
}

/* Varicode::packGrid: a 4-character locator, as WSJT-X's grid2deg reads
 * it (its subsquare's middle, longitude west); NMAXGRID for none. */
static unsigned pack_grid(const char *g)
{
    if (!g || strlen(g) < GRID_LEN || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' ||
        !is_digit(g[2]) || !is_digit(g[3]))
        return NMAXGRID;
    int nlong = 180 - 20 * (g[0] - 'A');
    int n20d = 2 * (g[2] - '0');
    float dlong = (float) (nlong - n20d) - 5.0f * 12.5f / 60.0f;
    int nlat = -90 + 10 * (g[1] - 'A') + (g[3] - '0');
    float dlat = (float) nlat + 2.5f * 12.5f / 60.0f;
    int ilong = (int) dlong, ilat = (int) (dlat + 90);
    return (unsigned) (((ilong + 180) / 2) * 180 + ilat);
}

static int cmd_code(const char *cmd)
{
    if (!strcmp(cmd, "?"))
        return 0;
    if (!strcmp(cmd, " QUERY MSGS?"))
        return 12;
    if (!strcmp(cmd, "  "))
        return CMD_FREETEXT;
    for (int i = 0; i < CMD_COUNT; i++)
        if (!strcmp(CMDS[i], cmd))
            return i;
    return -1;
}

/* Varicode's packNum: -30..31 as 1..62; 0 for none. */
static unsigned pack_num(const char *num, bool *ok)
{
    *ok = false;
    while (*num == ' ')
        num++;
    if (!*num)
        return 0;
    char *end;
    long v = strtol(num, &end, 10);
    if (end == num)
        return 0;
    *ok = true;
    if (v < NUM_MIN) v = NUM_MIN;
    if (v > NUM_MAX) v = NUM_MAX;
    return (unsigned) (v - NUM_MIN + 1);
}

/* [3][50][11],[5][3]: Varicode::packCompoundFrame. */
static bool compound_frame(const char *call, int type, unsigned num, unsigned bits3, char out[13])
{
    uint64_t c = pack_alnum50(call);
    if (!c)
        return false;
    bitbuf v = { .n = 0 };
    put_bits(&v, (uint64_t) type, 3);
    put_bits(&v, c, 50);
    put_bits(&v, num >> 5, 11);
    put_bits(&v, num & 31, 5);
    put_bits(&v, bits3, 3);
    to_frame(&v, out);
    return true;
}

/* heartbeat_re: [@ALLCALL |@HB ]type[ GRID] at the start; how many
 * characters it takes (0: not a heartbeat). */
static int match_heartbeat(const char *line, char *type, size_t tsize, char *grid)
{
    static const char *const TYPES[] = { "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD",
                                         "CQ FD", "CQ CQ", "CQ", "HB", "HEARTBEAT" };
    const char *p = line;
    while (*p == ' ')
        p++;
    for (int with_call = 1; with_call >= 0; with_call--) {
        const char *q = p;
        if (with_call) {
            if (!strncmp(q, "@ALLCALL", 8)) q += 8;
            else if (!strncmp(q, "@HB", 3)) q += 3;
            else continue;
            if (*q != ' ')
                continue;
            while (*q == ' ')
                q++;
        }
        for (size_t t = 0; t < sizeof TYPES / sizeof TYPES[0]; t++) {
            size_t tl = strlen(TYPES[t]);
            if (strncmp(q, TYPES[t], tl))
                continue;
            const char *r = q + tl;
            if (!strcmp(TYPES[t], "HEARTBEAT")) {   /* not HEARTBEAT SNR */
                const char *s = r;
                while (*s == ' ')
                    s++;
                if (s != r && !strncmp(s, "SNR", 3))
                    continue;
            }
            grid[0] = '\0';
            if (r[0] == ' ' && r[1] >= 'A' && r[1] <= 'R' && r[2] >= 'A' && r[2] <= 'R' &&
                is_digit(r[3]) && is_digit(r[4]) && !is_word(r[5])) {
                memcpy(grid, r + 1, GRID_LEN);
                grid[GRID_LEN] = '\0';
                r += 1 + GRID_LEN;
            } else if (is_word(*r)) {
                continue;   /* \b after the type */
            }
            snprintf(type, tsize, "%s", TYPES[t]);
            return (int) (r - line);
        }
    }
    return 0;
}

/* optional_cmd_pattern, at s (the regex's alternatives, in its order). */
static int match_cmd(const char *s, char *cmd, size_t size)
{
    static const char *const PLAIN[] = { "AGN?", "QSL?", "HW CPY?", "MSG TO:", "SNR?", "INFO?",
                                         "GRID?", "STATUS?", "QUERY MSGS?", "HEARING?" };
    static const char *const WORDS[] = { "STATUS", "HEARING", "QUERY CALL", "QUERY MSGS", "QUERY",
                                         "CMD", "MSG", "NACK", "ACK", "73", "YES", "NO",
                                         "HEARTBEAT SNR", "SNR", "QSL", "RR", "SK", "FB", "INFO",
                                         "GRID", "DIT DIT" };
    for (int sp = s[0] == ' ' ? 1 : 0; sp >= 0; sp--) {
        const char *q = s + sp;
        for (size_t i = 0; i < sizeof PLAIN / sizeof PLAIN[0]; i++)
            if (!strncmp(q, PLAIN[i], strlen(PLAIN[i]))) {
                int n = sp + (int) strlen(PLAIN[i]);
                snprintf(cmd, size, "%.*s", n, s);
                return n;
            }
        for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
            size_t wl = strlen(WORDS[i]);
            if (!strncmp(q, WORDS[i], wl) && (q[wl] == ' ' || q[wl] == '\0')) {
                int n = sp + (int) wl;
                snprintf(cmd, size, "%.*s", n, s);
                return n;
            }
        }
        if (*q == '?' || *q == '>' || *q == ' ') {
            int n = sp + 1;
            snprintf(cmd, size, "%.*s", n, s);
            return n;
        }
    }
    return 0;
}

/* directed_re: CALL, a command, and after SNR a number; how many it takes. */
static int match_directed(const char *line, char *to, size_t tsize, char *cmd, size_t csize,
                          char *num, size_t nsize)
{
    const char *p = line;
    if (*p == '@')
        p++;
    while (is_upper(*p) || is_digit(*p) || *p == '/')
        p++;
    if (p == line || (line[0] == '@' && p == line + 1))
        return 0;
    snprintf(to, tsize, "%.*s", (int) (p - line), line);
    cmd[0] = num[0] = '\0';
    int c = match_cmd(p, cmd, csize);
    p += c;
    size_t cl = strlen(cmd);
    if (cl >= 3 && !strcmp(cmd + cl - 3, "SNR")) {   /* (?<=SNR)\s?[-+]?(?:3[01]|[0-2]?[0-9]) */
        const char *q = p;
        if (*q == ' ')
            q++;
        if (*q == '-' || *q == '+')
            q++;
        const char *d = q;
        if (d[0] == '3' && (d[1] == '0' || d[1] == '1'))
            q = d + 2;
        else if (d[0] >= '0' && d[0] <= '2' && is_digit(d[1]))
            q = d + 2;
        else if (is_digit(d[0]))
            q = d + 1;
        if (q > d) {
            snprintf(num, nsize, "%.*s", (int) (q - p), p);
            p = q;
        }
    }
    return (int) (p - line);
}

/* CRC-16/KERMIT of the text, in three characters of Varicode's alphabet. */
static void checksum16(const char *text, char out[CHECKSUM16_LEN + 1])
{
    uint16_t crc = 0;
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        crc ^= *p;
        for (int k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    out[0] = ALNUM41[crc / (ALNUM41_N * ALNUM41_N)];
    out[1] = ALNUM41[crc / ALNUM41_N % ALNUM41_N];
    out[2] = ALNUM41[crc % ALNUM41_N];
    out[3] = '\0';
}

/* JSC::compress's lookup: the longest word of the list that starts the
 * text (what its list order gives), "@ALLCALL" taking 7. */
static uint32_t g_jsc_sorted[JS8_JSC_WORDS];
static pthread_once_t g_jsc_sorted_once = PTHREAD_ONCE_INIT;

static int jsc_cmp(const void *a, const void *b)
{
    return strcmp(js8_jsc_words + g_jsc_offset[*(const uint32_t *) a],
                  js8_jsc_words + g_jsc_offset[*(const uint32_t *) b]);
}

static void jsc_sort(void)
{
    pthread_once(&g_jsc_once, jsc_index);
    for (uint32_t i = 0; i < JS8_JSC_WORDS; i++)
        g_jsc_sorted[i] = i;
    qsort(g_jsc_sorted, JS8_JSC_WORDS, sizeof g_jsc_sorted[0], jsc_cmp);
}

static bool jsc_find(const char *w, size_t n, uint32_t *index)
{
    size_t lo = 0, hi = JS8_JSC_WORDS;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *m = js8_jsc_words + g_jsc_offset[g_jsc_sorted[mid]];
        int c = strncmp(w, m, n);
        if (c == 0 && m[n] != '\0')
            c = -1;
        if (c == 0) {
            *index = g_jsc_sorted[mid];
            return true;
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return false;
}

#define JSC_WORD_MAX 26   /* the list's longest */

static bool jsc_lookup(const char *w, uint32_t *index, int *size)
{
    pthread_once(&g_jsc_sorted_once, jsc_sort);
    size_t len = strlen(w);
    for (size_t n = len < JSC_WORD_MAX ? len : JSC_WORD_MAX; n > 0; n--)
        if (jsc_find(w, n, index)) {
            *size = *index == JSC_ALLCALL ? JSC_ALLCALL_LEN : (int) n;
            return true;
        }
    return false;
}

/* JSC::codeword: the index's base-9 continuations (+7), then its last
 * base-7 digit and the space bit. */
static int jsc_codeword(uint32_t index, bool space, uint8_t *bits)
{
    unsigned digits[JSC_MAX_GROUPS];
    int nd = 0, n = 0;
    for (uint32_t x = index / JSC_S; x > 0 && nd < JSC_MAX_GROUPS; x /= JSC_C) {
        x -= 1;
        digits[nd++] = x % JSC_C + JSC_S;
    }
    while (nd > 0) {
        unsigned d = digits[--nd];
        for (int k = JSC_GROUP_BITS - 1; k >= 0; k--)
            bits[n++] = (uint8_t) ((d >> k) & 1);
    }
    unsigned last = (index % JSC_S) << 1 | (space ? 1u : 0u);
    for (int k = JSC_GROUP_BITS; k >= 0; k--)
        bits[n++] = (uint8_t) ((last >> k) & 1);
    return n;
}

/* Varicode's packCompressedMessage: as many words of the text as fit in
 * the frame after its prefix bits; the characters taken (0: none). */
static int pack_jsc(const char *text, bitbuf *v)
{
    int taken = 0;
    const char *p = text;
    bool stop = false;
    while (*p && !stop) {
        const char *sp = strchr(p, ' ');
        size_t wl = sp ? (size_t) (sp - p) : strlen(p);
        bool last_word = sp == NULL;
        char w[LINE_MAX];
        bool is_space = false;
        if (wl == 0 && !last_word) {
            snprintf(w, sizeof w, " ");
            is_space = true;
        } else {
            snprintf(w, sizeof w, "%.*s", (int) wl, p);
        }
        char *q = w;
        while (*q) {
            uint32_t idx;
            int sz;
            if (!jsc_lookup(q, &idx, &sz)) {
                stop = true;   /* JS8Call drops the rest of the word: it is sent next frame */
                break;
            }
            q += sz;
            bool space_after = !*q && !is_space && !last_word;
            uint8_t cw[2 * FRAME_BITS];
            int n = jsc_codeword(idx, space_after, cw);
            if (v->n + n >= FRAME_BITS) {
                stop = true;
                break;
            }
            for (int k = 0; k < n; k++)
                v->b[v->n++] = cw[k];
            taken += sz + (space_after ? 1 : 0);
        }
        if (stop)
            break;
        p += wl + (sp ? 1 : 0);
    }
    return taken;
}

static const char *huff_code(char c)
{
    for (int i = 0; i < NHUFF; i++)
        if (HUFF[i].c == c)
            return HUFF[i].code;
    return NULL;
}

/* Varicode's packHuffMessage: every character must have a code; as many
 * as fit after the prefix bits; the characters taken (0: none). */
static int pack_huff(const char *text, bitbuf *v)
{
    for (const char *p = text; *p; p++)
        if (!huff_code(*p))
            return 0;
    int taken = 0;
    for (const char *p = text; *p; p++) {
        const char *code = huff_code(*p);
        int n = (int) strlen(code);
        if (v->n + n >= FRAME_BITS)
            break;
        for (int k = 0; k < n; k++)
            v->b[v->n++] = (uint8_t) (code[k] - '0');
        taken++;
    }
    return taken;
}

/* A data frame: Normal's, [1][0]Huffman or [1][1]JSC, whichever takes
 * more; the other submodes', all 72 bits JSC (flagged JS8_TX_DATA). */
static int pack_data(const char *text, bool fast, char out[13], int *type)
{
    bitbuf jsc = { .n = 0 };
    if (fast) {
        int n = pack_jsc(text, &jsc);
        pad_frame(&jsc);
        to_frame(&jsc, out);
        *type = JS8_TX_DATA;
        return n;
    }
    bitbuf huff = { .n = 0 };
    put_bits(&huff, 2, 2);   /* [1][0] */
    put_bits(&jsc, 3, 2);    /* [1][1] */
    int nh = pack_huff(text, &huff);
    int nj = pack_jsc(text, &jsc);
    bitbuf *best = nh > nj ? &huff : &jsc;
    pad_frame(best);
    to_frame(best, out);
    *type = 0;
    return nh > nj ? nh : nj;
}

static void lstrip(char *s)
{
    char *p = s;
    while (*p == ' ')
        p++;
    memmove(s, p, strlen(p) + 1);
}

static void add_frame(js8_tx_frame *out, int *n, int max, const char *frame, int type)
{
    if (*n < max) {
        snprintf(out[*n].frame, sizeof out[*n].frame, "%s", frame);
        out[*n].type = type;
        (*n)++;
    }
}

int js8_msg_pack(const char *mycall, const char *mygrid, const char *text, bool fast,
                 js8_tx_frame *out, int max)
{
    if (!mycall || !*mycall || !text || !out || max <= 0)
        return 0;
    char line[LINE_MAX];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof line; p++)
        line[n++] = (*p >= 'a' && *p <= 'z') ? (char) (*p - 'a' + 'A') : *p;
    line[n] = '\0';
    size_t ml = strlen(mycall);
    if (!strncmp(line, mycall, ml) && (line[ml] == ':' || line[ml] == ' ')) {
        memmove(line, line + ml + 1, strlen(line + ml + 1) + 1);
        lstrip(line);
    }
    while (n > 0 && line[strlen(line) - 1] == ' ' && strlen(line) > 1)
        line[strlen(line) - 1] = '\0';

    bool my_compound = false;
    if (!valid_call(mycall, &my_compound))
        return 0;
    int count = 0;
    bool has_directed = false, has_data = false, identified = false;
    while (line[0] && count < max) {
        char frame[JS8_CHARS + 1], type_s[16], grid[GRID_LEN + 1];
        char to[CALL_MAX * 2], cmd[24], num[8];
        int l = 0, d = 0;
        if (!has_directed && !has_data)
            l = match_heartbeat(line, type_s, sizeof type_s, grid);
        if (!l && !has_directed && !has_data) {
            d = match_directed(line, to, sizeof to, cmd, sizeof cmd, num, sizeof num);
            bool to_compound = false;
            if (d && (!cmd[0] || !strcmp(to, mycall) || !valid_call(to, &to_compound) ||
                      cmd_code(cmd) < 0))
                d = 0;
            if (d) {
                bool num_ok;
                unsigned inum = pack_num(num, &num_ok);
                int code = cmd_code(cmd);
                if (my_compound || to_compound) {
                    /* JS8Call's cases 1-3: who we are in a compound frame,
                     * then the command to them in a compound-directed one */
                    if (!compound_frame(mycall, FRAME_COMPOUND, pack_grid(mygrid), 0, frame))
                        return count;
                    add_frame(out, &count, max, frame, 0);
                    unsigned extra = (unsigned) NUSERGRID +
                        (is_snr_cmd((unsigned) code)
                             ? (((1u << 1) | (code == CMD_HB_SNR)) << 6) + (inum & NUM_MASK)
                             : (unsigned) code & 0x7f);
                    if (!compound_frame(to, FRAME_COMPOUND_DIRECTED, extra, 0, frame))
                        return count;
                    add_frame(out, &count, max, frame, 0);
                } else {
                    bool pf, pt;
                    uint32_t from = pack_call28(mycall, &pf), dest = pack_call28(to, &pt);
                    if (!from || !dest) {
                        d = 0;
                    } else {
                        bitbuf v = { .n = 0 };
                        put_bits(&v, FRAME_DIRECTED, 3);
                        put_bits(&v, from, 28);
                        put_bits(&v, dest, 28);
                        put_bits(&v, (unsigned) code % CMD_COUNT, 5);
                        put_bits(&v, (pf ? PORTABLE_FROM : 0) | (pt ? PORTABLE_TO : 0) | inum, 8);
                        to_frame(&v, frame);
                        add_frame(out, &count, max, frame, 0);
                    }
                }
                if (d) {
                    has_directed = identified = true;
                    memmove(line, line + d, strlen(line + d) + 1);
                    /* a buffered command's text goes with a checksum */
                    if (line[0] && cmd_code(cmd) != CMD_FREETEXT) {
                        lstrip(line);
                        static const int CHECKSUMMED[] = { 5, 9, 10, 11, 12, 13, 24 };
                        for (size_t i = 0; i < sizeof CHECKSUMMED / sizeof CHECKSUMMED[0]; i++)
                            if (code == CHECKSUMMED[i] && strcmp(to, "@APRSIS")) {
                                char chk[CHECKSUM16_LEN + 1];
                                checksum16(line, chk);
                                size_t ll = strlen(line);
                                snprintf(line + ll, sizeof line - ll, " %s", chk);
                                break;
                            }
                    } else {
                        lstrip(line);
                    }
                    continue;
                }
            }
        }
        if (l) {
            unsigned extra = pack_grid(grid);
            unsigned bits3 = 0;
            if (!strncmp(type_s, "CQ", 2)) {
                extra |= GRID_ALT_BIT;
                for (unsigned i = 0; i < sizeof CQS / sizeof CQS[0]; i++)
                    if (!strcmp(CQS[i], type_s))
                        bits3 = i;
            }
            if (!compound_frame(mycall, FRAME_HEARTBEAT, extra, bits3, frame))
                return count;
            add_frame(out, &count, max, frame, 0);
            identified = true;
            memmove(line, line + l, strlen(line + l) + 1);
            lstrip(line);
            continue;
        }
        /* text: say who it is from, if nothing has */
        if (!identified && !has_data && !strstr(line, mycall)) {
            char with[LINE_MAX];
            int room = (int) sizeof with - (int) strlen(mycall) - 3;   /* ": " and the NUL */
            snprintf(with, sizeof with, "%s: %.*s", mycall, room > 0 ? room : 0, line);
            memcpy(line, with, sizeof line);
        }
        identified = true;
        int type;
        int m = pack_data(line, fast, frame, &type);
        if (m <= 0)
            break;
        add_frame(out, &count, max, frame, type);
        has_data = true;
        memmove(line, line + m, strlen(line + m) + 1);
    }
    if (count > 0) {
        out[0].type |= JS8_TX_FIRST;
        out[count - 1].type |= JS8_TX_LAST;
    }
    return count;
}
