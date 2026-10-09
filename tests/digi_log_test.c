/* digi_log: sanitising, JSON escaping and validity, newest-first reads,
 * the live ring, rotation and the legacy spool.log line.
 *
 * Text logged here comes over the air from anyone with a transmitter, so
 * the property that matters most is checked with random bytes: whatever
 * goes in, every line written is valid JSON and one line per entry.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dsp/digi_log.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── a minimal JSON validator (RFC 8259 syntax, ASCII) ─────────────── */
static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
static const char *js_value(const char *p);
static const char *js_string(const char *p)
{
    if (*p++ != '"') return NULL;
    while (*p && *p != '"') {
        if ((unsigned char) *p < 0x20) return NULL;
        if (*p == '\\') {
            p++;
            if (*p == 'u') { for (int i = 1; i <= 4; i++) if (!strchr("0123456789abcdefABCDEF", p[i]) || !p[i]) return NULL; p += 5; continue; }
            if (!strchr("\"\\/bfnrt", *p) || !*p) return NULL;
        }
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}
static const char *js_number(const char *p)
{
    const char *s = p;
    if (*p == '-') p++;
    if (*p == '0') p++; else if (*p >= '1' && *p <= '9') while (*p >= '0' && *p <= '9') p++; else return NULL;
    if (*p == '.') { p++; if (!(*p >= '0' && *p <= '9')) return NULL; while (*p >= '0' && *p <= '9') p++; }
    if (*p == 'e' || *p == 'E') { p++; if (*p == '+' || *p == '-') p++; if (!(*p >= '0' && *p <= '9')) return NULL; while (*p >= '0' && *p <= '9') p++; }
    return p > s ? p : NULL;
}
static const char *js_value(const char *p)
{
    p = js_ws(p);
    if (*p == '{') {
        p = js_ws(p + 1);
        if (*p == '}') return p + 1;
        for (;;) {
            p = js_string(js_ws(p)); if (!p) return NULL;
            p = js_ws(p); if (*p++ != ':') return NULL;
            p = js_value(p); if (!p) return NULL;
            p = js_ws(p);
            if (*p == ',') { p++; continue; }
            return *p == '}' ? p + 1 : NULL;
        }
    }
    if (*p == '[') {
        p = js_ws(p + 1);
        if (*p == ']') return p + 1;
        for (;;) {
            p = js_value(p); if (!p) return NULL;
            p = js_ws(p);
            if (*p == ',') { p++; continue; }
            return *p == ']' ? p + 1 : NULL;
        }
    }
    if (*p == '"') return js_string(p);
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    return js_number(p);
}
static int json_valid(const char *s)
{
    const char *e = js_value(s);
    return e && *js_ws(e) == '\0';
}

static char tmpdir[256];

static char *read_file(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", tmpdir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { *len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t) n + 1);
    *len = fread(b, 1, (size_t) n, f);
    b[*len] = '\0';
    fclose(f);
    return b;
}

static int count_lines(const char *s)
{
    int n = 0;
    for (; s && *s; s++) n += *s == '\n';
    return n;
}

static void test_sanitize(void)
{
    char out[64];
    digi_log_sanitize("  CQ\tPU2UIT\x01\x1b[31m GG66 \xc3\xa9  ", out, sizeof out, 60);
    CHECK(strcmp(out, "CQ PU2UIT[31m GG66") == 0, "sanitised [%s]", out);
    digi_log_sanitize("ABCDEFGHIJ", out, sizeof out, 4);
    CHECK(strcmp(out, "ABCD") == 0, "capped [%s]", out);
    digi_log_sanitize(NULL, out, sizeof out, 10);
    CHECK(out[0] == '\0', "NULL gives empty");
}

static void test_format_escapes(void)
{
    char line[DIGI_LOG_LINE_MAX];
    digi_log_entry e = { .mode = "RTTY", .dir = "rx", .freq_hz = 7040000,
                         .text = "say \"hi\" \\ </script><b>", .from = "PU2UIT", .grid = NULL };
    size_t n = digi_log_format(&e, 42, 1791570615.25, line, sizeof line);
    printf("format: %s\n", line);
    CHECK(n > 0 && json_valid(line), "formatted line is not valid JSON: %s", line);
    CHECK(strstr(line, "\"text\":\"say \\\"hi\\\" \\\\ </script><b>\"") != NULL, "escaping: %s", line);
    CHECK(strstr(line, "\"at\":\"2026-10-09T18:30:15.250Z\"") != NULL, "timestamp: %s", line);
    CHECK(strstr(line, "\"from\":\"PU2UIT\",\"to\":null,\"grid\":null") != NULL, "optional fields: %s", line);
    CHECK(strstr(line, "\"snr_db\":null,\"dt_s\":null") != NULL, "absent numbers are null: %s", line);
    CHECK(strstr(line, "\"dstar\":null") != NULL, "no dstar object: %s", line);

    /* An unknown mode or direction can't be smuggled into the output. */
    digi_log_entry bad = { .mode = "FT8\",\"x\":\"", .dir = "up", .text = "" };
    n = digi_log_format(&bad, 1, 1.0, line, sizeof line);
    CHECK(n > 0 && json_valid(line) && strstr(line, "\"mode\":\"FT8\",\"dir\":\"rx\""), "bad mode/dir: %s", line);
}

static void test_random_bytes_stay_valid(void)
{
    char line[DIGI_LOG_LINE_MAX], text[700], call[40];
    srand(1234);
    for (int iter = 0; iter < 20000; iter++) {
        int tl = rand() % (int) sizeof text, cl = rand() % (int) sizeof call;
        for (int i = 0; i < tl - 1; i++) text[i] = (char) (1 + rand() % 255);
        text[tl > 0 ? tl - 1 : 0] = '\0';
        for (int i = 0; i < cl - 1; i++) call[i] = (char) (1 + rand() % 255);
        call[cl > 0 ? cl - 1 : 0] = '\0';
        digi_log_dstar d = { call, call, call, call, call, iter & 1, iter & 2 };
        digi_log_entry e = { .mode = (iter % 3) ? "CW" : "DSTAR", .dir = "rx", .freq_hz = 14074000,
                             .has_snr = true, .snr_db = (float) (rand() % 61 - 30),
                             .text = text, .from = call, .to = call, .grid = call, .dstar = &d };
        size_t n = digi_log_format(&e, (uint64_t) iter, 1e9 + iter, line, sizeof line);
        if (!(n > 0 && json_valid(line) && !strchr(line, '\n'))) {
            CHECK(0, "random input %d gave invalid output: %s", iter, line);
            break;
        }
    }
}

static void test_add_read_ring_and_legacy(void)
{
    digi_log_set_dir(tmpdir);
    uint64_t start_id = digi_log_latest_id();

    for (int i = 0; i < 5; i++) {
        char t[32];
        snprintf(t, sizeof t, "MSG %d", i);
        digi_log_entry e = { .mode = "FT8", .dir = i == 4 ? "tx" : "rx", .freq_hz = 14074000,
                             .has_offset = true, .offset_hz = 1500 + i, .text = t };
        digi_log_add(&e);
    }
    /* A newline in received text must not forge a second legacy line. */
    digi_log_entry inj = { .mode = "RTTY", .dir = "rx", .freq_hz = 7040000,
                           .text = "HELLO\nFT8 rx 14.074: FAKE LINE" };
    digi_log_add(&inj);
    digi_log_dstar d = { "PU2UIT", "", "CQCQCQ", "", "", false, true };
    digi_log_entry ds = { .mode = "DSTAR", .dir = "rx", .freq_hz = 145670000, .text = "",
                          .from = "PU2UIT", .to = "CQCQCQ", .dstar = &d };
    digi_log_add(&ds);

    size_t len;
    char *jsonl = read_file("digi.jsonl", &len);
    CHECK(jsonl && count_lines(jsonl) == 7, "digi.jsonl has %d lines, want 7", count_lines(jsonl));
    for (char *line = jsonl, *nl; line && (nl = strchr(line, '\n')); line = nl + 1) {
        *nl = '\0';
        CHECK(json_valid(line), "jsonl line not valid JSON: %s", line);
    }
    free(jsonl);

    char *legacy = read_file("spool.log", &len);
    printf("spool.log:\n%s", legacy ? legacy : "(none)");
    CHECK(legacy && count_lines(legacy) == 7, "spool.log has %d lines, want 7", count_lines(legacy));
    CHECK(legacy && strstr(legacy, "FT8 rx 14.074: MSG 0\n"), "legacy FT8 line");
    CHECK(legacy && strstr(legacy, "FT8 tx 14.074: MSG 4\n"), "legacy tx line");
    CHECK(legacy && strstr(legacy, "RTTY rx 7.040: HELLOFT8 rx 14.074: FAKE LINE\n"), "injected newline removed");
    CHECK(legacy && strstr(legacy, "DSTAR rx header: ur=CQCQCQ my=PU2UIT\n"), "legacy D-STAR line");
    free(legacy);

    char *arr = digi_log_last_json(3);
    printf("last 3: %s\n", arr);
    CHECK(arr && json_valid(arr), "last_json not valid JSON");
    CHECK(arr && strstr(arr, "\"mode\":\"DSTAR\"") && strstr(arr, "HELLO") && strstr(arr, "MSG 4")
          && !strstr(arr, "MSG 3"), "last 3 should be D-STAR, RTTY, MSG 4: %s", arr);
    CHECK(arr && strstr(arr, "DSTAR") < strstr(arr, "MSG 4"), "newest first");
    free(arr);

    char lines[16][DIGI_LOG_LINE_MAX];
    uint64_t ids[16];
    int n = digi_log_since(start_id, lines, ids, 16);
    CHECK(n == 7, "since(start) gave %d entries, want 7", n);
    for (int i = 1; i < n; i++)
        CHECK(ids[i] > ids[i - 1], "ids not increasing");
    CHECK(n == 7 && strstr(lines[0], "MSG 0") && strstr(lines[6], "DSTAR"), "ring order oldest first");
    CHECK(digi_log_since(ids[n - 1], lines, ids, 16) == 0, "nothing after the newest id");
    CHECK(digi_log_latest_id() == ids[n - 1], "latest id");
}

static void test_rotation(void)
{
    digi_log_set_max_bytes(4096);
    for (int i = 0; i < 60; i++) {
        char t[64];
        snprintf(t, sizeof t, "ROTATION TEST ENTRY NUMBER %02d", i);
        digi_log_entry e = { .mode = "CW", .dir = "rx", .freq_hz = 7030000, .text = t };
        digi_log_add(&e);
    }
    size_t len1, len0;
    char *old = read_file("digi.jsonl.1", &len1);
    char *cur = read_file("digi.jsonl", &len0);
    CHECK(old != NULL, "rotated file digi.jsonl.1 exists");
    CHECK(cur && (long) len0 < 4096 + DIGI_LOG_LINE_MAX, "current file stays near the cap (%zu bytes)", len0);
    free(old);
    free(cur);

    /* Two files are kept, so a read reaches back into digi.jsonl.1 but no
     * further. Ask for a few more than the current file holds. */
    size_t clen;
    char *c = read_file("digi.jsonl", &clen);
    int in_current = count_lines(c);
    free(c);
    int want = in_current + 5;
    char *arr = digi_log_last_json(want);
    int entries = 0;
    for (const char *p = arr; p && (p = strstr(p, "\"id\":")); p++) entries++;
    CHECK(arr && json_valid(arr) && entries == want, "last %d across both files: %d entries", want, entries);
    CHECK(arr && strstr(arr, "NUMBER 59") && strstr(arr, "NUMBER 59") < strstr(arr, "NUMBER 58"),
          "newest first across the rotation");
    free(arr);
}

int main(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/digi_log_test.XXXXXX");
    const char *t = getenv("TMPDIR");
    if (t && t[0])
        snprintf(tmpdir, sizeof tmpdir, "%s/digi_log_test.XXXXXX", t);
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }

    test_sanitize();
    test_format_escapes();
    test_random_bytes_stay_valid();
    test_add_read_ring_and_legacy();
    test_rotation();

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) { /* best effort */ }

    if (failures) {
        printf("digi_log_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("digi_log_test: ok\n");
    return 0;
}
