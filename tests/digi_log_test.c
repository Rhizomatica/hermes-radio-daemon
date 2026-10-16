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

    /* An unknown mode or direction can't be smuggled into the output, and
     * is refused rather than relabelled: falling back to FT8/rx would log a
     * new mode, or a typo, as FT8 traffic with no error. */
    digi_log_entry bad = { .mode = "FT8\",\"x\":\"", .dir = "up", .text = "" };
    n = digi_log_format(&bad, 1, 1.0, line, sizeof line);
    CHECK(n == 0 && line[0] == '\0', "bad mode/dir must be refused: %s", line);
    digi_log_entry newmode = { .mode = "RADE", .dir = "rx", .text = "x" };
    CHECK(digi_log_format(&newmode, 1, 1.0, line, sizeof line) == 0, "a mode outside the schema is refused");
    digi_log_entry baddir = { .mode = "CW", .dir = "up", .text = "x" };
    CHECK(digi_log_format(&baddir, 1, 1.0, line, sizeof line) == 0, "a dir outside the schema is refused");
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

/* digi_log_add refuses an unknown mode: nothing reaches the file or ring. */
static void test_add_refuses_unknown_mode(void)
{
    uint64_t before = digi_log_latest_id();
    size_t l0, l1;
    char *f0 = read_file("digi.jsonl", &l0);
    digi_log_entry e = { .mode = "PSK31", .dir = "rx", .freq_hz = 14070000, .text = "NOPE" };
    digi_log_add(&e);
    char *f1 = read_file("digi.jsonl", &l1);
    CHECK(digi_log_latest_id() == before, "an unknown mode must not get an id");
    CHECK(l1 == l0 && (!f1 || !strstr(f1, "NOPE")), "an unknown mode must not reach digi.jsonl");
    free(f0);
    free(f1);
}

/* Reading the newest entries must not need the whole file: with far more
 * than one 64 KiB window of entries, the tail read still returns exactly
 * the newest N, newest first, and is valid JSON. It must also never pass
 * off the fragment at the top of a window as an entry. */
static void test_tail_read_large_file(void)
{
    char dir[300];
    snprintf(dir, sizeof dir, "%s/tail", tmpdir);
    mkdir(dir, 0755);
    digi_log_set_dir(dir);
    digi_log_set_max_bytes(64L * 1024 * 1024);      /* no rotation here */

    /* A D-STAR entry has a nested '{' and ends in "}}", the shape a
     * fragment could be mistaken for; interleave them. */
    const int total = 1200;
    for (int i = 0; i < total; i++) {
        char t[96];
        snprintf(t, sizeof t, "TAIL READ ENTRY %04d PADDING TO MAKE LINES LONGER XXXXXXXXXXXXXXXX", i);
        if (i % 7 == 0) {
            digi_log_dstar d = { "PU2UIT", "", "CQCQCQ", "", "", false, true };
            digi_log_entry ds = { .mode = "DSTAR", .dir = "rx", .freq_hz = 145670000,
                                  .text = t, .dstar = &d };
            digi_log_add(&ds);
        } else {
            digi_log_entry e = { .mode = "CW", .dir = "rx", .freq_hz = 7030000, .text = t };
            digi_log_add(&e);
        }
    }
    size_t flen;
    char *f = read_file("tail/digi.jsonl", &flen);
    CHECK(flen > 3 * 65536, "the test file must span several windows (%zu bytes)", flen);
    free(f);

    const int want = 500;                    /* needs the window to grow */
    char *arr = digi_log_last_json(want);
    int entries = 0;
    for (const char *p = arr; p && (p = strstr(p, "\"id\":")); p++) entries++;
    CHECK(arr && json_valid(arr), "tail read gives valid JSON");
    CHECK(entries == want, "tail read: %d entries, want %d", entries, want);
    char newest[64], oldest[64], beyond[64];
    snprintf(newest, sizeof newest, "ENTRY %04d", total - 1);
    snprintf(oldest, sizeof oldest, "ENTRY %04d", total - want);
    snprintf(beyond, sizeof beyond, "ENTRY %04d", total - want - 1);
    CHECK(arr && strstr(arr, newest) && strstr(arr, oldest) && !strstr(arr, beyond),
          "exactly the newest %d entries", want);
    CHECK(arr && strstr(arr, newest) < strstr(arr, oldest), "newest first");
    free(arr);
    digi_log_set_dir(tmpdir);
}

/* The precise hazard the tail read guards against: a window that starts
 * mid-line whose first byte happens to be the '{' of a nested dstar object.
 * That fragment starts with '{' and ends with '}', so without dropping it a
 * reader returns it as an entry -- one with no id. The file is built by
 * hand so the 64 KiB window starts exactly on that '{'. */
static void test_tail_fragment_on_brace(void)
{
    char dir[300], path[400];
    snprintf(dir, sizeof dir, "%s/frag", tmpdir);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/digi.jsonl", dir);
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL, "create the crafted file");
    if (!fp) return;

    const char *filler = "{\"id\":1,\"mode\":\"CW\",\"text\":\"FILLER\"}\n";
    const char *line_l = "{\"id\":2,\"mode\":\"DSTAR\",\"text\":\"LINE L\",\"dstar\":{\"mycall\":\"PU2UIT\"}}\n";
    const char *suffix = strstr(line_l, "{\"mycall\"");   /* where the window must start */
    const long window = 65536;
    long rest = window - (long) strlen(suffix);          /* bytes of lines after L */
    fputs(filler, fp);
    fputs(line_l, fp);
    int after = 0;
    long written = 0;
    while (written < rest) {
        char ln[160];
        long left = rest - written;
        int len = left >= 200 ? 100 : (int) left;        /* last line takes the remainder */
        if (left >= 200 || left < 40)
            len = left < 40 ? (int) left : 100;
        /* {"id":N,"text":"xxx...x"}\n, padded to exactly len bytes */
        int head = snprintf(ln, sizeof ln, "{\"id\":%d,\"text\":\"", 100 + after);
        int pad = len - head - 3;                         /* closing "}\n */
        if (pad < 0) pad = 0;
        memset(ln + head, 'x', (size_t) pad);
        memcpy(ln + head + pad, "\"}\n", 3);
        fwrite(ln, 1, (size_t) (head + pad + 3), fp);
        written += head + pad + 3;
        after++;
    }
    fclose(fp);

    struct stat st;
    stat(path, &st);
    long cut = (long) st.st_size - window;
    long brace_at = (long) strlen(filler) + (long) (suffix - line_l);
    CHECK(cut == brace_at, "the window must start on the nested '{' (cut %ld, brace %ld)", cut, brace_at);

    digi_log_set_dir(dir);
    int want = after + 1;                                /* reaches line L */
    char *arr = digi_log_last_json(want);
    int ids = 0;
    for (const char *p = arr; p && (p = strstr(p, "\"id\":")); p++) ids++;
    CHECK(arr && json_valid(arr), "valid JSON");
    CHECK(arr && !strstr(arr, "[{\"mycall\"") && !strstr(arr, ",{\"mycall\""),
          "a fragment must never be returned as an entry");
    CHECK(ids == want, "every element must be a real entry: %d ids for %d wanted", ids, want);
    CHECK(arr && strstr(arr, "LINE L"), "line L itself is reached by widening the window");
    free(arr);
    digi_log_set_dir(tmpdir);
}

/* Ids continue from the file, so they stay ordered across a restart even
 * when the clock comes up behind the previous run. Simulated by planting an
 * entry whose id is far in the future, then logging into that directory. */
static void test_ids_continue_from_file(void)
{
    char dir[300], path[400];
    snprintf(dir, sizeof dir, "%s/ids", tmpdir);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/digi.jsonl", dir);
    const unsigned long long future = 4102444800000000ULL;   /* year 2100, in us */
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL, "plant a previous run's log");
    if (!fp) return;
    fprintf(fp, "{\"id\":%llu,\"at\":\"2100-01-01T00:00:00.000Z\",\"mode\":\"CW\",\"dir\":\"rx\","
                "\"frequency_hz\":7030000,\"offset_hz\":null,\"snr_db\":null,\"dt_s\":null,"
                "\"text\":\"PREVIOUS RUN\",\"from\":null,\"to\":null,\"grid\":null,\"dstar\":null}\n",
            future);
    fclose(fp);

    digi_log_set_dir(dir);
    digi_log_entry e = { .mode = "CW", .dir = "rx", .freq_hz = 7030000, .text = "AFTER RESTART" };
    digi_log_add(&e);
    CHECK(digi_log_latest_id() > future,
          "a new id must follow the file's last one (%llu), got %llu",
          future, (unsigned long long) digi_log_latest_id());
    digi_log_set_dir(tmpdir);
}

/* One helper builds every D-STAR header entry, both backends and both
 * directions; a failed CRC must not put untrusted callsigns in from/to. */
static void test_dstar_header_helper(void)
{
    uint8_t h[41];
    memset(h, ' ', sizeof h);
    h[0] = h[1] = h[2] = 0;
    memcpy(h + 3,  "DIRECT  ", 8);
    memcpy(h + 11, "DIRECT  ", 8);
    memcpy(h + 19, "CQCQCQ  ", 8);
    memcpy(h + 27, "PU2UIT  ", 8);
    memcpy(h + 35, "MOBI", 4);

    digi_log_dstar_header(h, sizeof h, "tx", 7100000, true, true);
    char *arr = digi_log_last_json(1);
    CHECK(arr && json_valid(arr), "header entry is valid JSON");
    CHECK(arr && strstr(arr, "\"mode\":\"DSTAR\",\"dir\":\"tx\""), "mode and dir: %s", arr);
    CHECK(arr && strstr(arr, "\"from\":\"PU2UIT\",\"to\":\"CQCQCQ\""), "from/to when CRC ok: %s", arr);
    CHECK(arr && strstr(arr, "\"encrypted\":true,\"crc_ok\":true"), "flags: %s", arr);
    CHECK(arr && strstr(arr, "\"suffix\":\"MOBI\""), "suffix: %s", arr);
    free(arr);

    h[27] = (char) 0xff;                            /* a garbled callsign */
    digi_log_dstar_header(h, sizeof h, "rx", 7100000, false, false);
    arr = digi_log_last_json(1);
    CHECK(arr && json_valid(arr), "garbled header is still valid JSON");
    CHECK(arr && strstr(arr, "\"from\":null,\"to\":null"), "no from/to on a failed CRC: %s", arr);
    CHECK(arr && strstr(arr, "\"crc_ok\":false"), "crc flag: %s", arr);
    free(arr);

    uint64_t before = digi_log_latest_id();
    digi_log_dstar_header(h, 10, "rx", 7100000, false, true);   /* too short */
    CHECK(digi_log_latest_id() == before, "a short header is ignored");
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
    test_add_refuses_unknown_mode();
    test_rotation();
    test_tail_read_large_file();
    test_tail_fragment_on_brace();
    test_ids_continue_from_file();
    test_dstar_header_helper();

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
