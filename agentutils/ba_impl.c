/*
 * ba_impl.c - support implementation for the busyagent applet.
 *
 * Everything the applet needs besides its main loop lives here, in
 * dependency order: utils, JSON parser/serializer, HTTP client (SSE),
 * session store, display rendering, protocol adaptation, system prompt
 * assembly and the busybox tool executor.
 */
#include "busyagent.h"
#include "ba_builtin_schemas.h"
/* everything below leans on libbb (xmalloc_read, full_write,
 * bb_make_directory, lineedit, ...) - include it once up front */
#include "libbb.h"
#include "busybox.h"

/* ==== ba_util.c ==== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* ============================================================
 * StrBuf - growable string buffer
 * ============================================================ */

void sb_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_ensure(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t newcap = sb->cap ? sb->cap : 256;
    while (newcap < sb->len + extra + 1) newcap *= 2;
    char *p = realloc(sb->data, newcap);
    if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
    sb->data = p;
    sb->cap = newcap;
}

void sb_append(StrBuf *sb, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    sb_appendn(sb, s, n);
}

void sb_appendn(StrBuf *sb, const char *s, size_t n) {
    if (n == 0) return;
    sb_ensure(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_ensure(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
    sb->data[sb->len] = '\0';
}

void sb_append_char(StrBuf *sb, char c) {
    sb_ensure(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_truncate(StrBuf *sb, size_t len) {
    if (len < sb->len) {
        sb->len = len;
        sb->data[len] = '\0';
    }
}

void sb_append_json_string(StrBuf *sb, const char *src) {
    if (!src) { sb_append(sb, "null"); return; }
    sb_append_char(sb, '"');
    while (*src) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
            case '"':  sb_append(sb, "\\\""); src++; break;
            case '\\': sb_append(sb, "\\\\"); src++; break;
            case '\b': sb_append(sb, "\\b"); src++; break;
            case '\f': sb_append(sb, "\\f"); src++; break;
            case '\n': sb_append(sb, "\\n"); src++; break;
            case '\r': sb_append(sb, "\\r"); src++; break;
            case '\t': sb_append(sb, "\\t"); src++; break;
            default:
                if (c < 0x20) {
                    /* control chars -> \uXXXX (JSON spec) */
                    sb_appendf(sb, "\\u%04x", c);
                    src++;
                } else {
                    /* ASCII printable + all non-control bytes (incl. UTF-8 multibyte) pass through;
                     * invalid UTF-8 is fixed at the source by util_sanitize_utf8 */
                    sb_append_char(sb, c);
                    src++;
                }
                break;
        }
    }
    sb_append_char(sb, '"');
}

void sb_append_shell_arg(StrBuf *sb, const char *src) {
    if (!src) {
        sb_append(sb, "''");
        return;
    }
    sb_append_char(sb, '\'');
    for (; *src; src++) {
        if (*src == '\'') sb_append(sb, "'\\''");
        else sb_append_char(sb, *src);
    }
    sb_append_char(sb, '\'');
}

/* UTF-8 sanitize: logic identical to awk/sanitize_utf8.awk:
 * byte-by-byte scan, invalid UTF-8 bytes become the literal text \ufffd (6 ASCII chars).
 * Returns a new malloc'd string; caller frees.
 */
char *util_sanitize_utf8(const char *src) {
    if (!src) return util_strdup("");
    size_t len = strlen(src);
    /* worst case: every byte invalid, replaced by 6-char \ufffd */
    StrBuf sb;
    sb_init(&sb);
    sb_ensure(&sb, len * 6 + 1);

    const unsigned char *p = (const unsigned char *)src;
    const unsigned char *end = p + len;

    while (p < end) {
        unsigned char b = *p;
        if (b < 0x80) {
            /* ASCII (0x00-0x7F): pass through */
            sb_append_char(&sb, b);
            p++;
        } else if (b >= 0xC2 && b <= 0xDF) {
            /* 2-byte sequence: C2-DF + 80-BF */
            if (p + 1 < end && p[1] >= 0x80 && p[1] <= 0xBF) {
                sb_appendn(&sb, (const char *)p, 2);
                p += 2;
            } else {
                sb_append(&sb, "\\ufffd");
                p++;
            }
        } else if (b >= 0xE0 && b <= 0xEF) {
            /* 3-byte sequence: E0-EF + 80-BF + 80-BF */
            if (p + 2 < end && p[1] >= 0x80 && p[1] <= 0xBF && p[2] >= 0x80 && p[2] <= 0xBF) {
                sb_appendn(&sb, (const char *)p, 3);
                p += 3;
            } else {
                sb_append(&sb, "\\ufffd");
                p++;
            }
        } else if (b >= 0xF0 && b <= 0xF4) {
            /* 4-byte sequence: F0-F4 + 80-BF + 80-BF + 80-BF */
            if (p + 3 < end && p[1] >= 0x80 && p[1] <= 0xBF && p[2] >= 0x80 && p[2] <= 0xBF && p[3] >= 0x80 && p[3] <= 0xBF) {
                sb_appendn(&sb, (const char *)p, 4);
                p += 4;
            } else {
                sb_append(&sb, "\\ufffd");
                p++;
            }
        } else {
            /* invalid: C0-C1 (overlong), 80-BF (stray continuation), F5-FF (out of range) */
            sb_append(&sb, "\\ufffd");
            p++;
        }
    }
    return sb.data;
}

/* ============================================================
 * utility functions
 * ============================================================ */

char *util_new_session_id(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char *buf = malloc(64);  /* larger than needed; silences -Wformat-truncation */
    unsigned short r = (unsigned short)(rand() & 0xFFFF);
    snprintf(buf, 64, "%04d%02d%02d-%02d%02d%02d-%04x",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, r);
    return buf;
}

char *util_path_join(const char *a, const char *b) {
    size_t alen = strlen(a);
    /* skip leading slashes of b */
    while (*b == '/') b++;
    size_t blen = strlen(b);
    char *r = malloc(alen + 1 + blen + 1);
    memcpy(r, a, alen);
    /* make sure a ends with a slash */
    if (alen > 0 && a[alen - 1] != '/') {
        r[alen++] = '/';
    }
    memcpy(r + alen, b, blen + 1);
    return r;
}

int util_mkdirs(const char *path, int mode) {
	/* bb_make_directory: recursive mkdir, 0 on success (EEXIST ok) */
	return bb_make_directory((char *)path, mode, FILEUTILS_RECUR);
}

const char *util_home_dir(void) {
    const char *home = getenv("HOME");
    if (home) return home;
    return "/tmp";
}

char *util_strdup(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

const char *util_env(const char *name, const char *defval) {
    const char *v = getenv(name);
    return v ? v : defval;
}

char *util_timestamp_now(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char *buf = malloc(32);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

long util_parse_size(const char *s) {
    if (!s || !*s) return -1;
    char *endp = NULL;
    long val = strtol(s, &endp, 10);
    if (endp == s || val <= 0) return -1;
    if (*endp == 'k' || *endp == 'K') { val *= 1000; endp++; }
    else if (*endp == 'm' || *endp == 'M') { val *= 1000000; endp++; }
    else if (*endp == 'g' || *endp == 'G') { val *= 1000000000; endp++; }
    return (*endp == '\0') ? val : -1;
}

long util_epoch_seconds(void) {
    return (long)time(NULL);
}

int util_utf8_char_count(const char *s) {
    int count = 0;
    for (; *s; s++) {
        /* UTF-8 continuation bytes are 10xxxxxx (0x80-0xBF); not counted */
        if ((*(unsigned char*)s & 0xC0) != 0x80) count++;
    }
    return count;
}

size_t util_utf8_truncate_len(const char *s, size_t max_bytes) {
    size_t len = strlen(s);
    if (len <= max_bytes) return len;
    /* walk back over UTF-8 continuation bytes so we never cut mid-character */
    while (max_bytes > 0 && ((unsigned char)s[max_bytes] & 0xC0) == 0x80) {
        max_bytes--;
    }
    return max_bytes;
}

void util_truncate_str(char *s, size_t max_total) {
    size_t len = strlen(s);
    if (len <= max_total) return;
    /* leave 3 bytes for "..."; UTF-8-safe truncation */
    size_t cut = (max_total >= 3) ? max_total - 3 : 0;
    cut = util_utf8_truncate_len(s, cut);
    s[cut] = '.';
    s[cut + 1] = '.';
    s[cut + 2] = '.';
    s[cut + 3] = '\0';
}

void util_truncate_chars(char *s, int max_chars) {
    if (util_utf8_char_count(s) <= max_chars) return;
    /* leave 3 chars for "..."; find the byte offset of the (max_chars - 3)-th char */
    int target = max_chars >= 3 ? max_chars - 3 : 0;
    int char_count = 0;
    char *p = s;
    while (*p && char_count < target) {
        if ((*(unsigned char *)p & 0xC0) != 0x80) char_count++;
        p++;
    }
    p[0] = '.'; p[1] = '.'; p[2] = '.'; p[3] = '\0';
}

char *util_rtrim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' '  || s[len-1] == '\t')) {
        s[--len] = '\0';
    }
    return s;
}

char *util_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

int util_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(content);
    size_t nw = fwrite(content, 1, len, f);
    fclose(f);
    return (nw == len) ? 0 : -1;
}

/* ==== ba_json.c ==== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* ============================================================
 * internal helpers
 * ============================================================ */

static void skip_ws(const char *src, size_t *pos) {
    while (src[*pos] == ' ' || src[*pos] == '\t' ||
           src[*pos] == '\n' || src[*pos] == '\r') {
        (*pos)++;
    }
}

static JsonParse make_err(const char *msg) {
    JsonParse p;
    memset(&p, 0, sizeof(p));
    p.error = msg;
    return p;
}

static JsonParse make_val(JsonType type, const char *src, size_t start, size_t end) {
    JsonParse p;
    p.val.type = type;
    p.val.src = src;
    p.val.start = start;
    p.val.end = end;
    p.error = NULL;
    return p;
}

/* parse a JSON string (starting at the opening quote) */
static JsonParse parse_string(const char *src, size_t *pos) {
    size_t start = *pos;
    (*pos)++; /* skip opening quote */
    while (src[*pos] && src[*pos] != '"') {
        if (src[*pos] == '\\') {
            (*pos)++; /* skip escaped char */
            if (!src[*pos])
                return make_err("unterminated escape sequence");
            /* NB: without this check the second ++ below walks past the
             * NUL terminator and the loop keeps reading out of bounds */
        }
        (*pos)++;
    }
    if (src[*pos] != '"') return make_err("unclosed string");
    (*pos)++; /* skip closing quote */
    return make_val(JSON_STRING, src, start, *pos);
}

/* parse a JSON number */
static JsonParse parse_number(const char *src, size_t *pos) {
    size_t start = *pos;
    if (src[*pos] == '-') (*pos)++;
    while (isdigit((unsigned char)src[*pos])) (*pos)++;
    if (src[*pos] == '.') {
        (*pos)++;
        while (isdigit((unsigned char)src[*pos])) (*pos)++;
    }
    if (src[*pos] == 'e' || src[*pos] == 'E') {
        (*pos)++;
        if (src[*pos] == '+' || src[*pos] == '-') (*pos)++;
        while (isdigit((unsigned char)src[*pos])) (*pos)++;
    }
    return make_val(JSON_NUMBER, src, start, *pos);
}

/* forward declarations */
static JsonParse json_parse_internal(const char *src, size_t *pos);

/* parse a JSON array */
static JsonParse parse_array(const char *src, size_t *pos) {
    size_t start = *pos;
    (*pos)++; /* skip [ */
    skip_ws(src, pos);
    if (src[*pos] == ']') { (*pos)++; return make_val(JSON_ARRAY, src, start, *pos); }
    for (;;) {
        skip_ws(src, pos);
        JsonParse vp = json_parse(src, pos);
        if (vp.error) return vp;
        skip_ws(src, pos);
        if (src[*pos] == ',') { (*pos)++; continue; }
        if (src[*pos] == ']') { (*pos)++; break; }
        return make_err("expected ',' or ']'");
    }
    return make_val(JSON_ARRAY, src, start, *pos);
}

/* parse a JSON object */
static JsonParse parse_object(const char *src, size_t *pos) {
    size_t start = *pos;
    (*pos)++; /* skip { */
    skip_ws(src, pos);
    if (src[*pos] == '}') { (*pos)++; return make_val(JSON_OBJECT, src, start, *pos); }
    for (;;) {
        skip_ws(src, pos);
        if (src[*pos] != '"') return make_err("expected string key");
        JsonParse kp = parse_string(src, pos);
        if (kp.error) return kp;
        skip_ws(src, pos);
        if (src[*pos] != ':') return make_err("expected ':'");
        (*pos)++;
        skip_ws(src, pos);
        JsonParse vp = json_parse(src, pos);
        if (vp.error) return vp;
        skip_ws(src, pos);
        if (src[*pos] == ',') { (*pos)++; continue; }
        if (src[*pos] == '}') { (*pos)++; break; }
        return make_err("expected ',' or '}'");
    }
    return make_val(JSON_OBJECT, src, start, *pos);
}

/* parse a JSON value */
static JsonParse json_parse_internal(const char *src, size_t *pos) {    skip_ws(src, pos);
    char c = src[*pos];
    if (c == '"') return parse_string(src, pos);
    if (c == '{') return parse_object(src, pos);
    if (c == '[') return parse_array(src, pos);
    if (c == 't') {
        if (strncmp(src + *pos, "true", 4) == 0) { *pos += 4; return make_val(JSON_BOOL, src, *pos - 4, *pos); }
        return make_err("expected 'true'");
    }
    if (c == 'f') {
        if (strncmp(src + *pos, "false", 5) == 0) { *pos += 5; return make_val(JSON_BOOL, src, *pos - 5, *pos); }
        return make_err("expected 'false'");
    }
    if (c == 'n') {
        if (strncmp(src + *pos, "null", 4) == 0) { *pos += 4; return make_val(JSON_NULL, src, *pos - 4, *pos); }
        return make_err("expected 'null'");
    }
    if (c == '-' || isdigit((unsigned char)c)) return parse_number(src, pos);
    return make_err("unexpected character");
}

/* public parse entry points */
JsonParse json_parse(const char *src, size_t *pos) {
    return json_parse_internal(src, pos);
}

JsonParse json_parse_root(const char *src) {
    if (!src) return make_err("null input");
    size_t pos = 0;
    JsonParse p = json_parse_internal(src, &pos);
    if (p.error) return p;
    skip_ws(src, &pos);
    if (src[pos] != '\0') return make_err("trailing content");
    return p;
}

/* ============================================================
 * queries
 * ============================================================ */

JsonVal json_get(JsonVal obj, const char *key) {
    if (obj.type != JSON_OBJECT) {
        JsonVal null_val;
        memset(&null_val, 0, sizeof(null_val));
        return null_val;
    }
    size_t pos = obj.start + 1; /* skip { */
    const char *src = obj.src;
    skip_ws(src, &pos);
    if (src[pos] == '}') {
        JsonVal null_val;
        memset(&null_val, 0, sizeof(null_val));
        return null_val;
    }
    for (;;) {
        skip_ws(src, &pos);
        /* parse key */
        JsonParse kp = parse_string(src, &pos);
        if (kp.error) break;
        /* compare key (without quotes) */
        size_t klen = (kp.val.end - 1) - (kp.val.start + 1);
        const char *kstr = src + kp.val.start + 1;
        bool match = (strlen(key) == klen && strncmp(key, kstr, klen) == 0);
        skip_ws(src, &pos);
        if (src[pos] != ':') break;
        pos++;
        skip_ws(src, &pos);
        if (match) {
            return json_parse(src, &pos).val;
        }
        /* skip the value */
        JsonParse vp = json_parse(src, &pos);
        if (vp.error) break;
        skip_ws(src, &pos);
        if (src[pos] == ',') { pos++; continue; }
        break;
    }
    JsonVal null_val;
    memset(&null_val, 0, sizeof(null_val));
    return null_val;
}

char *json_get_string(JsonVal obj, const char *key) {
    JsonVal v = json_get(obj, key);
    return json_string_val(v);
}

int json_get_int(JsonVal obj, const char *key) {
    JsonVal v = json_get(obj, key);
    if (v.type != JSON_NUMBER) return 0;
    /* extract the span and convert to int */
    char buf[64];
    size_t len = v.end - v.start;
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, v.src + v.start, len);
    buf[len] = '\0';
    return (int)strtod(buf, NULL);
}

long long json_get_ll(JsonVal obj, const char *key) {
    JsonVal v = json_get(obj, key);
    if (v.type != JSON_NUMBER) return 0;
    char buf[64];
    size_t len = v.end - v.start;
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, v.src + v.start, len);
    buf[len] = '\0';
    return strtoll(buf, NULL, 10);
}

double json_get_double(JsonVal obj, const char *key) {
    JsonVal v = json_get(obj, key);
    return json_number_val(v);
}

bool json_get_bool(JsonVal obj, const char *key, bool def) {
    JsonVal v = json_get(obj, key);
    if (v.type == JSON_NULL) return def;
    return json_bool_val(v);
}

/* ============================================================
 * array operations
 * ============================================================ */

int json_array_len(JsonVal arr) {
    if (arr.type != JSON_ARRAY) return 0;
    int count = 0;
    size_t pos = arr.start + 1; /* skip [ */
    const char *src = arr.src;
    skip_ws(src, &pos);
    if (src[pos] == ']') return 0;
    for (;;) {
        skip_ws(src, &pos);
        JsonParse vp = json_parse(src, &pos);
        if (vp.error) break;
        count++;
        skip_ws(src, &pos);
        if (src[pos] == ',') { pos++; continue; }
        break;
    }
    return count;
}

JsonVal json_array_get(JsonVal arr, int index) {
    if (arr.type != JSON_ARRAY) {
        JsonVal null_val;
        memset(&null_val, 0, sizeof(null_val));
        return null_val;
    }
    size_t pos = arr.start + 1;
    const char *src = arr.src;
    skip_ws(src, &pos);
    if (src[pos] == ']') {
        JsonVal null_val;
        memset(&null_val, 0, sizeof(null_val));
        return null_val;
    }
    int cur = 0;
    for (;;) {
        skip_ws(src, &pos);
        JsonParse vp = json_parse(src, &pos);
        if (vp.error) break;
        if (cur == index) return vp.val;
        cur++;
        skip_ws(src, &pos);
        if (src[pos] == ',') { pos++; continue; }
        break;
    }
    JsonVal null_val;
    memset(&null_val, 0, sizeof(null_val));
    return null_val;
}

/* ============================================================
 * value extraction
 * ============================================================ */

char *json_string_val(JsonVal v) {
    if (v.type != JSON_STRING) return NULL;
    /* decode a JSON string (strip quotes, handle escapes) */
    const char *src = v.src;
    size_t start = v.start + 1; /* skip opening quote */
    size_t end = v.end - 1;     /* skip closing quote */
    size_t len = end - start;
    char *buf = malloc(len * 2 + 1); /* worst case */
    size_t out = 0;
    for (size_t i = start; i < end; i++) {
        if (src[i] == '\\') {
            i++;
            switch (src[i]) {
                case '"':  buf[out++] = '"'; break;
                case '\\': buf[out++] = '\\'; break;
                case '/':  buf[out++] = '/'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'n':  buf[out++] = '\n'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'u': {
                    /* \uXXXX - surrogate pairs supported */
                    unsigned int cp = 0;
                    int hex_digits = 0;
                    for (int j = 0; j < 4 && i + 1 < end; j++) {
                        i++;
                        char h = src[i];
                        cp = cp * 16;
                        if (h >= '0' && h <= '9') { cp += h - '0'; hex_digits++; }
                        else if (h >= 'a' && h <= 'f') { cp += h - 'a' + 10; hex_digits++; }
                        else if (h >= 'A' && h <= 'F') { cp += h - 'A' + 10; hex_digits++; }
                        else break;
                    }
                    /* malformed \u (short hex) -> U+FFFD; avoid embedded NUL */
                    if (hex_digits < 4)
                        cp = 0xFFFD;
                    /* high surrogate? check the following \uDC00-\uDFFF */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        i + 1 < end && src[i + 1] == '\\' && src[i + 2] == 'u') {
                        size_t saved = i;
                        i += 2; /* skip \u */
                        unsigned int lo = 0;
                        int valid = 1;
                        for (int j = 0; j < 4 && i + 1 < end; j++) {
                            i++;
                            char h = src[i];
                            if (h >= '0' && h <= '9') lo = lo * 16 + (h - '0');
                            else if (h >= 'a' && h <= 'f') lo = lo * 16 + (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo = lo * 16 + (h - 'A' + 10);
                            else { valid = 0; break; }
                        }
                        if (valid && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            /* not a valid low surrogate; fall back */
                            i = saved;
                        }
                    }
                    /* UTF-8 encode */
                    if (cp < 0x80) {
                        buf[out++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[out++] = (char)(0xC0 | (cp >> 6));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        buf[out++] = (char)(0xE0 | (cp >> 12));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[out++] = (char)(0xF0 | (cp >> 18));
                        buf[out++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: buf[out++] = src[i]; break;
            }
        } else {
            buf[out++] = src[i];
        }
    }
    buf[out] = '\0';
    /* shrink to the actual size */
    char *result = realloc(buf, out + 1);
    return result ? result : buf;
}

double json_number_val(JsonVal v) {
    if (v.type != JSON_NUMBER) return 0.0;
    char buf[64];
    size_t len = v.end - v.start;
    if (len >= sizeof(buf)) return 0.0;
    memcpy(buf, v.src + v.start, len);
    buf[len] = '\0';
    return strtod(buf, NULL);
}

bool json_bool_val(JsonVal v) {
    if (v.type != JSON_BOOL) return false;
    /* true spells "true", false "false" */
    return v.src[v.start] == 't';
}

char *json_as_string(JsonVal v) {
    if (v.type == JSON_STRING) return json_string_val(v);
    if (v.type == JSON_NULL) return NULL;
    /* other types: take the raw text */
    size_t len = v.end - v.start;
    char *s = malloc(len + 1);
    memcpy(s, v.src + v.start, len);
    s[len] = '\0';
    return s;
}

/* ============================================================
 * object iterator
 * ============================================================ */

void json_obj_iter_init(JsonObjectIter *it, JsonVal obj) {
    memset(it, 0, sizeof(*it));
    if (obj.type != JSON_OBJECT) return;
    it->src = obj.src;
    it->pos = obj.start + 1; /* skip { */
    it->first = true;
}

bool json_obj_iter_next(JsonObjectIter *it) {
    /* free the previous iteration's key */
    free((char *)it->key);
    it->key = NULL;

    const char *src = it->src;
    if (!src) return false;
    skip_ws(src, &it->pos);
    if (src[it->pos] == '}' || src[it->pos] == '\0') return false;
    if (!it->first) {
        /* skip the comma */
        if (src[it->pos] == ',') it->pos++;
        skip_ws(src, &it->pos);
        if (src[it->pos] == '}' || src[it->pos] == '\0') return false;
    }
    it->first = false;
    /* parse the key */
    JsonParse kp = parse_string(src, &it->pos);
    if (kp.error) return false;
    /* key text (without quotes) */
    size_t klen = (kp.val.end - 1) - (kp.val.start + 1);
    char *key = malloc(klen + 1);
    memcpy(key, src + kp.val.start + 1, klen);
    key[klen] = '\0';
    it->key = key; /* points at a temp buffer */
    /* skip : */
    skip_ws(src, &it->pos);
    if (src[it->pos] == ':') it->pos++;
    skip_ws(src, &it->pos);
    /* parse the value */
    JsonParse vp = json_parse(src, &it->pos);
    if (vp.error) { free(key); return false; }
    it->val = vp.val;
    return true;
}

/* ============================================================
 * JSONL append
 * ============================================================ */

int jsonl_append(const char *path, const char *json_line) {
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "%s\n", json_line);
    fclose(f);
    return 0;
}

void json_obj_iter_cleanup(JsonObjectIter *it) {
    free((char *)it->key);
    it->key = NULL;
}

/* ==== bb_http.c ==== */
/*
 * bb_http - plain-HTTP transport for busyagent
 *
 * Replaces the libcurl backend of bash-agent's transport.c using only
 * busybox/libbb primitives: xhost2sockaddr for DNS, non-blocking connect
 * with poll() timeout, safe_read/safe_poll for the body pump, and a wget
 * style chunked decoder feeding the provider-agnostic SSE pump
 * (sse_stream_feed) in ba_transport.c.
 *
 * Only http:// is supported in phase 1. TLS is a later phase.
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define BA_CONNECT_TIMEOUT_MS  5000
#define BA_READ_TIMEOUT_MS    300000   /* idle timeout for SSE streams */
#define BA_MAX_HEADER         (64 * 1024)
#define BA_MAX_RETRIES        2
#define BA_RETRY_MAX_TIME_MS  20000
#define BA_TLS_RECHDR_LEN     5     /* TLS record header (networking/tls.c) */
#define BA_TLS_APPDATA        23    /* RECORD_TYPE_APPLICATION_DATA */
/* RFC 5246: a TLSPlaintext fragment carries at most 2^14 bytes, and the
 * record layer may add up to 2^10 of compression overhead + cipher block
 * padding. 18 KiB covers every legal decrypted application-data record:
 * records larger than this are refused, never silently truncated. */
#define BA_TLS_PLAIN_MAX      (18 * 1024)
/* chunked transfer: sane upper bound for a single chunk size line value */
#define BA_MAX_CHUNK_SIZE     (16 * 1024 * 1024)
/* ba_read() polls in short slices so the cancelled flag is checked
 * promptly instead of blocking for the full idle timeout */
#define BA_POLL_SLICE_MS      250

typedef struct {
	char host[256];
	int port;
	int is_https;
	char path[1024];
} BaUrl;

static int ba_parse_url(const char *url, BaUrl *u)
{
	char *colon;
	const char *p, *slash;

	memset(u, 0, sizeof(*u));
	if (strncmp(url, "https://", 8) == 0) {
		u->is_https = 1;
		p = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		p = url + 7;
	} else {
		return -1;
	}
	slash = strchr(p, '/');
	if (!slash) {
		snprintf(u->host, sizeof(u->host), "%s", p);
		snprintf(u->path, sizeof(u->path), "/");
	} else {
		size_t hlen = slash - p;
		if (hlen >= sizeof(u->host))
			return -1;
		memcpy(u->host, p, hlen);
		u->host[hlen] = '\0';
		snprintf(u->path, sizeof(u->path), "%s", slash);
	}
	colon = strchr(u->host, ':');
	if (colon) {
		u->port = atoi(colon + 1);
		*colon = '\0';
	} else {
		u->port = u->is_https ? 443 : 80;
	}
	if (!u->host[0] || u->port <= 0)
		return -1;
	return 0;
}

/* Connect with timeout (non-blocking connect + POLLOUT). -1 on error. */
static int ba_connect(const char *host, int port)
{
	len_and_sockaddr *lsa;
	int fd, flags, rc;
	struct pollfd pfd;
	socklen_t slen;
	int err;

	lsa = host2sockaddr(host, port);
	if (!lsa)
		return -1;
	fd = socket(lsa->u.sa.sa_family, SOCK_STREAM, 0);
	if (fd < 0) {
		free(lsa);
		return -1;
	}
	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	rc = connect(fd, &lsa->u.sa, lsa->len);
	if (rc < 0 && errno != EINPROGRESS) {
		close(fd);
		free(lsa);
		return -1;
	}
	if (rc != 0) {
		pfd.fd = fd;
		pfd.events = POLLOUT;
		if (safe_poll(&pfd, 1, BA_CONNECT_TIMEOUT_MS) <= 0) {
			close(fd);
			free(lsa);
			return -1;
		}
		err = 0;
		slen = sizeof(err);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &slen);
		if (err != 0) {
			close(fd);
			free(lsa);
			return -1;
		}
	}
	fcntl(fd, F_SETFL, flags);   /* back to blocking */
	free(lsa);
	return fd;
}

static int send_all(int fd, const char *buf, size_t len);
static int send_all_conn(tls_state_t *tls, int fd, const char *buf, size_t len)
{
#if ENABLE_TLS
	if (tls) {
		while (len) {
			size_t chunk = len > 8192 ? 8192 : len;
			memcpy(tls_get_outbuf(tls, chunk), buf, chunk);
			tls_xwrite(tls, chunk);
			buf += chunk;
			len -= chunk;
		}
		return 0;
	}
#else
	(void)tls;
#endif
	return send_all(fd, buf, len);
}

static int send_all(int fd, const char *buf, size_t len)
{
	return full_write(fd, buf, len) == (ssize_t)len ? 0 : -1;
}

typedef struct {
	int fd;
	int chunked;              /* Transfer-Encoding: chunked */
	long content_length;      /* -1 if unknown */
	long body_left;           /* for content_length mode */
	long chunk_left;          /* for chunked mode */
	int chunk_state;          /* 0=size line, 1=data, 2=data CRLF, 3=trailers, 4=done */
	int eof;
	tls_state_t *tls;
	char tls_plain[BA_TLS_PLAIN_MAX];
	int tls_plain_len;
	int tls_plain_pos;
	char hdr[BA_MAX_HEADER];
	size_t hdr_len;
	int status;
	int got_header;
	char pending[4096];
	size_t pending_len;
	volatile int *cancelled;  /* checked between poll slices */
} BaResp;

/* Read more bytes into buf, honoring idle timeout and the cancelled flag.
 * Returns n>0, 0 on EOF, -1 on error/timeout/cancel. */
static int ba_read(BaResp *r, char *buf, size_t bufsz)
{
	int64_t deadline = (int64_t)monotonic_ms() + BA_READ_TIMEOUT_MS;
	int fd;

#if ENABLE_TLS
	if (r->tls)
		fd = r->tls->ifd;
	else
#endif
		fd = r->fd;

	for (;;) {
		struct pollfd pfd;
		int pr;

		if (r->cancelled && *(r->cancelled))
			return -1;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pr = safe_poll(&pfd, 1, BA_POLL_SLICE_MS);
		if (pr > 0)
			break;
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		/* poll slice timed out: re-check cancel/total-idle deadlines */
		if ((int64_t)monotonic_ms() > deadline)
			return -1;
	}

#if ENABLE_TLS
	if (r->tls) {
		while (r->tls_plain_len == 0) {
			int tn = tls_xread_record(r->tls, "application data");
			if (tn < 1)
				return 0;   /* TLS EOF */
			if (r->tls->inbuf[0] != BA_TLS_APPDATA)
				return -1;
			/* never truncate: a record beyond the protocol maximum
			 * means the peer (or a MITM) misbehaves - fail loudly */
			if (tn > (int)sizeof(r->tls_plain))
				return -1;
			memcpy(r->tls_plain, r->tls->inbuf + BA_TLS_RECHDR_LEN, tn);
			r->tls_plain_len = tn;
			r->tls_plain_pos = 0;
		}
		{
			int give = r->tls_plain_len - r->tls_plain_pos;
			if (give > (int)bufsz)
				give = bufsz;
			memcpy(buf, r->tls_plain + r->tls_plain_pos, give);
			r->tls_plain_pos += give;
			if (r->tls_plain_pos >= r->tls_plain_len) {
				r->tls_plain_len = 0;
				r->tls_plain_pos = 0;
			}
			return give;
		}
	}
#endif
	{
		int n = safe_read(fd, buf, bufsz);
		return n;   /* 0 = EOF */
	}
}

/* Read and parse the response header. Returns 0 on success. */
static int ba_read_header(BaResp *r)
{
	char *p, *end;

	while (r->hdr_len < BA_MAX_HEADER - 1) {
		char *hit;
		size_t have = r->hdr_len;

		hit = (have >= 4) ? memmem(r->hdr, have, "\r\n\r\n", 4) : NULL;
		if (hit)
			break;
		{
			size_t want = BA_MAX_HEADER - 1 - r->hdr_len;
			int n;
			if (want > 4096)
				want = 4096;
			if (want == 0)
				return -1;   /* header too large */
			n = ba_read(r, r->hdr + r->hdr_len, want);
			if (n <= 0)
				return -1;
			r->hdr_len += n;
			r->hdr[r->hdr_len] = '\0';
		}
		hit = memmem(r->hdr, r->hdr_len, "\r\n\r\n", 4);
		if (hit)
			break;
		(void)hit;
	}
	end = memmem(r->hdr, r->hdr_len, "\r\n\r\n", 4);
	if (!end)
		return -1;

	/* the read that completed the header may have consumed body bytes:
	 * keep them for ba_body_read (first SSE chunk lives here) */
	{
		size_t body_start = (end - r->hdr) + 4;
		size_t avail = r->hdr_len - body_start;
		if (avail > sizeof(r->pending))
			avail = sizeof(r->pending);
		memcpy(r->pending, r->hdr + body_start, avail);
		r->pending_len = avail;
	}

	/* status code from first line */
	r->status = 0;
	p = strchr(r->hdr, ' ');
	if (p)
		r->status = atoi(p + 1);

	r->chunked = 0;
	r->content_length = -1;
	r->got_header = 1;

	/* NB: keep one strtok_r state across the whole header - re-initialising
	 * it per iteration made every call restart from the status line, so
	 * Transfer-Encoding/Content-Length were never seen */
	{
		char *save = NULL;
		char *line = strtok_r(r->hdr, "\r\n", &save);
		while (line) {
			str_tolower(line);   /* libbb in-place lowercase */
			if (strncmp(line, "transfer-encoding:", 18) == 0
			 && strstr(line, "chunked"))
				r->chunked = 1;
			else if (strncmp(line, "content-length:", 15) == 0) {
				const char *v = line + 15;
				while (*v == ' ' || *v == '\t')
					v++;
				/* digits only: junk or a negative value must not
				 * drive body_left below zero */
				if (*v >= '0' && *v <= '9')
					r->content_length = atol(v);
			}
			line = strtok_r(NULL, "\r\n", &save);
		}
	}
	if (r->chunked) {
		r->chunk_state = 0;
		r->chunk_left = 0;
	} else {
		r->body_left = r->content_length;
	}
	return 0;
}

/* Raw byte source for body decoding: bytes the header read already
 * consumed come first, then the socket/TLS stream. Returns n>0, 0 EOF,
 * -1 error/timeout/cancel. */
static int resp_raw_read(BaResp *r, char *out, size_t outsz)
{
	if (r->pending_len > 0) {
		size_t n = r->pending_len < outsz ? r->pending_len : outsz;
		memcpy(out, r->pending, n);
		r->pending_len -= n;
		memmove(r->pending, r->pending + n, r->pending_len);
		return (int)n;
	}
	if (r->eof)
		return 0;
	return ba_read(r, out, outsz);
}

/* strict hex chunk-size: digits only (no sign), bounded, no overflow.
 * Accepts 0: the zero chunk is legal - it starts the trailer section. */
static int parse_chunk_size_hex(const char *s, long *out)
{
	unsigned long v = 0;

	if (!*s)
		return -1;
	for (; *s; s++) {
		int d;
		if (*s >= '0' && *s <= '9')      d = *s - '0';
		else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else
			return -1;   /* sign, space or extension junk */
		if (v > (ULONG_MAX >> 4))
			return -1;
		v = (v << 4) | (unsigned long)d;
	}
	if (v > (unsigned long)BA_MAX_CHUNK_SIZE)
		return -1;
	*out = (long)v;
	return 0;
}

/* Decode next piece of body into out (already de-chunked).
 * Returns n>0 data, 0 end of body, -1 error. */
static int ba_body_read(BaResp *r, char *out, size_t outsz)
{
	if (!r->chunked) {
		int n;
		size_t want = outsz;
		if (r->content_length >= 0) {
			if (r->body_left <= 0)
				return 0;
			if (want > (size_t)r->body_left)
				want = (size_t)r->body_left;
		}
		n = resp_raw_read(r, out, want);
		if (n < 0)
			return -1;
		if (n == 0) {
			r->eof = 1;
			/* a fixed-length body cut short is an error, not EOF */
			if (r->content_length >= 0 && r->body_left > 0)
				return -1;
			return 0;
		}
		if (r->content_length >= 0)
			r->body_left -= n;
		return n;
	}

	/* chunked: 0=size line, 1=chunk data, 2=CRLF after chunk data,
	 * 3=trailer section after the zero chunk, 4=done.  The data-CRLF
	 * and the trailer section are distinct states: collapsing them
	 * (the old code) ended the body after the FIRST chunk. */
	for (;;) {
		char line[130];
		size_t ln = 0;

		if (r->chunk_state == 4)
			return 0;   /* done */
		if (r->chunk_state == 1) {
			int n;
			size_t want = r->chunk_left < (long)outsz
			            ? (size_t)r->chunk_left : outsz;
			n = resp_raw_read(r, out, want);
			if (n < 0)
				return -1;
			if (n == 0)
				return -1;   /* unexpected EOF mid-chunk */
			r->chunk_left -= n;
			if (r->chunk_left == 0)
				r->chunk_state = 2;
			return n;
		}
		/* read one header line (size line, data CRLF or trailer) */
		for (;;) {
			char ch;
			int c = resp_raw_read(r, &ch, 1);
			if (c <= 0)
				return -1;
			if (ch == '\n')
				break;
			if (ch == '\r')
				continue;   /* part of CRLF */
			if (ln >= sizeof(line) - 1)
				return -1;   /* size/trailer line too long */
			line[ln++] = ch;
		}
		line[ln] = '\0';
		if (r->chunk_state == 2) {
			if (ln != 0)
				return -1;   /* missing CRLF after chunk data */
			r->chunk_state = 0;
			continue;   /* next chunk size */
		}
		if (r->chunk_state == 3) {
			if (ln == 0) {
				r->chunk_state = 4;   /* end of trailer section */
				return 0;
			}
			continue;   /* trailer field: keep consuming */
		}
		/* state 0: "HEX[;ext]" size line */
		{
			char *semi = strchr(line, ';');
			long v;
			if (semi)
				*semi = '\0';
			if (parse_chunk_size_hex(line, &v) != 0)
				return -1;
			if (v == 0) {
				r->chunk_state = 3;   /* zero chunk: trailers follow */
				continue;
			}
			r->chunk_left = v;
			r->chunk_state = 1;
		}
	}
}

static void ba_send_request(tls_state_t *tls, int fd, const BaUrl *u, const char **headers,
			    int header_count, const char *body, size_t body_len)
{
	char first[512];
	int i;

	if (u->port != 80)
		snprintf(first, sizeof(first),
			"POST %s HTTP/1.1\r\n"
			"Host: %s:%d\r\n"
			"Content-Length: %lu\r\n"
			"Connection: close\r\n",
			u->path, u->host, u->port, (unsigned long)body_len);
	else
		snprintf(first, sizeof(first),
			"POST %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Content-Length: %lu\r\n"
			"Connection: close\r\n",
			u->path, u->host, (unsigned long)body_len);
	send_all_conn(tls, fd, first, strlen(first));
	for (i = 0; i < header_count; i++) {
		send_all_conn(tls, fd, headers[i], strlen(headers[i]));
		send_all_conn(tls, fd, "\r\n", 2);
	}
	send_all_conn(tls, fd, "\r\n", 2);
	send_all_conn(tls, fd, body, body_len);
}


#if ENABLE_TLS
/* https:// support: in-tree TLS state machine (networking/tls.c), the same
 * code ssl_client and wget use. Returns a handshaked session; the caller
 * owns it and free()s it with the socket.
 *
 * SECURITY: the in-tree TLS client performs NO certificate-chain, validity,
 * hostname, handshake-signature or Finished verification, and record MAC/tag
 * checking is not implemented either (networking/tls.c upstream comments).
 * A machine-in-the-middle can therefore finish a handshake with any
 * self-signed certificate and read/inject traffic, including API keys.
 * http_post_sse() prints a one-time warning so the trade-off is explicit. */
static tls_state_t *ba_tls_connect(const char *host, int port)
{
	len_and_sockaddr *lsa;
	int fd;
	tls_state_t *tls;

	lsa = host2sockaddr(host, port);
	if (!lsa)
		return NULL;
	/* xconnect_stream() creates the socket, connects and RETURNS the fd
	 * (libbb semantics) - keep its return value */
	fd = xconnect_stream(lsa);
	free(lsa);

	tls = new_tls_state();
	tls->ifd = tls->ofd = fd;
	tls_handshake(tls, host);
	return tls;
}
#endif

/* release the per-request TLS state (inbuf/outbuf/hsd allocations);
 * the socket itself stays owned by the caller.  Compiled regardless of
 * ENABLE_TLS so the pump can call it unconditionally (no-op there). */
static void ba_tls_dispose(tls_state_t *tls)
{
	if (!tls)
		return;
#if ENABLE_TLS
	free(tls->inbuf);
	free(tls->outbuf);
	free(tls->hsd);
#endif
	free(tls);
}

/* The in-tree TLS client (networking/tls.c) authenticates neither the peer
 * nor the records: no certificate-chain, validity or hostname validation, no
 * handshake-signature/Finished verification and no MAC/tag checking. HTTPS
 * therefore works out of the box, but a machine in the middle controlling
 * the network can impersonate the endpoint. We make that trade-off explicit:
 * the connection is allowed, with a one-time warning on stderr. Point -u at
 * a TLS-terminating gateway or a trusted network for API credentials. */
static void ba_tls_notice(void)
{
	static int warned;

	if (!warned) {
		warned = 1;
		bb_error_msg("WARNING: https: the built-in TLS client does not verify"
			     " server certificates or hostnames - use a trusted network"
			     " or a TLS-terminating gateway for API credentials.");
	}
}

/* Streaming POST with SSE pump. Mirrors the old curl semantics:
 * up to 2 retries, 1s delay, 20s total retry window, retry on 5xx. */
int http_post_sse(const char *url, const char **headers, int header_count,
		  const char *body, size_t body_len,
		  const char *provider,
		  sse_callback_fn callback, void *ctx,
		  volatile int *cancelled)
{
	BaUrl u;
	char buf[4096];
	unsigned start_ms = monotonic_ms();
	int attempt;

	if (ba_parse_url(url, &u) != 0)
		return -1;

	/* https:// works out of the box; the authentication trade-off of the
	 * in-tree TLS client (see ba_tls_notice) is announced on stderr once.
	 * This also covers the ENABLE_TLS=0 build, where an https URL would
	 * otherwise fall through to a plaintext connect. */
	if (u.is_https)
		ba_tls_notice();

	for (attempt = 0; attempt <= BA_MAX_RETRIES; attempt++) {
		StreamCtx sctx;
		BaResp r;
		int fd = -1;
		tls_state_t *tls = NULL;
		int io_err = 0, http_code = 0;

#if ENABLE_TLS
		if (u.is_https) {
			tls = ba_tls_connect(u.host, u.port);
			if (!tls) {
				io_err = 1;
				goto attempt_done;
			}
			fd = tls->ifd;
		} else
#endif
		{
			fd = ba_connect(u.host, u.port);
			if (fd < 0) {
				io_err = 1;
				goto attempt_done;
			}
		}
		memset(&r, 0, sizeof(r));
		r.fd = fd;
		r.tls = tls;
		r.cancelled = cancelled;
		(void)tls;
		ba_send_request(tls, fd, &u, headers, header_count, body, body_len);
		if (ba_read_header(&r) != 0) {
			io_err = 1;
			close(fd);
			ba_tls_dispose(tls);
			tls = NULL;
			goto attempt_done;
		}
		http_code = r.status;

		sse_stream_init(&sctx, provider, callback, ctx, cancelled);
		for (;;) {
			int n = ba_body_read(&r, buf, sizeof(buf));
			if (n < 0) {
				io_err = 1;
				break;
			}
			if (n == 0)
				break;
			if (sse_stream_feed(&sctx, buf, n) == 0) {
				/* cancelled */
				sse_stream_free(&sctx);
				close(fd);
				ba_tls_dispose(tls);
				{
					SseEvent st;
					memset(&st, 0, sizeof(st));
					st.type = SSE_STOP;
					st.content = (char *)"interrupted";
					callback(ctx, &st);
				}
				return 0;
			}
		}
		close(fd);
		ba_tls_dispose(tls);
		tls = NULL;

		if (!io_err && http_code < 500) {
			/* non-SSE JSON error bodies etc. */
			sse_stream_finish(&sctx, provider, callback, ctx);
			sse_stream_free(&sctx);
			if (http_code >= 400)
				return http_code;
			return 0;
		}
		sse_stream_free(&sctx);
		/* 5xx or io error: fall through to retry logic */

	attempt_done:
		ba_tls_dispose(tls);
		tls = NULL;
		if (cancelled && *cancelled) {
			SseEvent st;
			memset(&st, 0, sizeof(st));
			st.type = SSE_STOP;
			st.content = (char *)"interrupted";
			callback(ctx, &st);
			return 0;
		}
		if (attempt >= BA_MAX_RETRIES)
			return io_err ? -1 : (http_code >= 400 ? http_code : 0);
		(void)0;
			if ((unsigned)(monotonic_ms() - start_ms) >= BA_RETRY_MAX_TIME_MS)
			return io_err ? -1 : (http_code >= 400 ? http_code : 0);
		{
			SseEvent retry_evt;
			memset(&retry_evt, 0, sizeof(retry_evt));
			retry_evt.type = SSE_RETRY;
			callback(ctx, &retry_evt);
		}
		/* interruptible backoff: keep checking the cancelled flag so a
		 * Ctrl-C during the wait does not wait out the full second */
		{
			int slept = 0;
			while (slept < 1000 && !(cancelled && *cancelled)) {
				usleep(50 * 1000);
				slept += 50;
			}
		}
	}
	return -1;
}

/* ==== ba_store.c ==== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* ============================================================
 * SessionPaths
 * ============================================================ */

void store_session_paths_free(SessionPaths *p) {
    if (!p) return;
    FREE_PTR(p->base_dir);
    FREE_PTR(p->session_dir);
    FREE_PTR(p->conversation);
    FREE_PTR(p->events);
    FREE_PTR(p->stats);
    FREE_PTR(p->summary);
    FREE_PTR(p->plan);
    FREE_PTR(p->plan_draft);
}

char *store_session_project_key(const char *cwd) {
    /* mirrors the bash AWK algorithm:
     *   sub(/^\/+/, "", $0)              - strip leading /
     *   gsub(/\//, "-", $0)              — / → -
     *   gsub(/[^A-Za-z0-9._-]/, "-", $0) - map others to -
     *   gsub(/-+/, "-", $0)              - squeeze runs of -
     *   sub(/^-+/, "", $0)               - strip leading -
     *   sub(/-+$/, "", $0)               - strip trailing -
     *   print "-" $0                      - prefix with -
     */
    if (!cwd || !cwd[0]) return util_strdup("-");

    size_t len = strlen(cwd);
    char *key = malloc(len + 3); /* room for the - prefix */
    if (!key) return NULL;

    /* skip leading / */
    const char *src = cwd;
    while (*src == '/') src++;

    /* convert in one pass */
    size_t ki = 0;
    char prev = '\0';
    for (; *src; src++) {
        char c = *src;
        if (c == '/') c = '-';
        else if (!(  (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            c = '-';
        /* squeeze runs of - */
        if (c == '-' && prev == '-') continue;
        key[ki++] = c;
        prev = c;
    }
    key[ki] = '\0';

    /* strip trailing - */
    while (ki > 0 && key[ki - 1] == '-') key[--ki] = '\0';

    /* prefix with - */
    char *result = malloc(ki + 2);
    if (!result) { free(key); return NULL; }
    result[0] = '-';
    memcpy(result + 1, key, ki + 1);
    free(key);
    return result;
}

SessionPaths store_session_paths_for(const char *home, const char *cwd, const char *session_id) {
    SessionPaths p;
    memset(&p, 0, sizeof(p));

    char *key = store_session_project_key(cwd);
    StrBuf buf;
    sb_init(&buf);

    /* base_dir = $BA_HOME/projects/<key> */
    sb_appendf(&buf, "%s/projects/%s", home, key);
    p.base_dir = util_strdup(buf.data);

    /* session_dir = base_dir/<session-id> */
    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/%s", p.base_dir, session_id);
    p.session_dir = util_strdup(buf.data);

    /* the file paths */
    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/conversation.jsonl", p.session_dir);
    p.conversation = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/events.jsonl", p.session_dir);
    p.events = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/stats.json", p.session_dir);
    p.stats = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/summary.txt", p.session_dir);
    p.summary = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/plan.md", p.session_dir);
    p.plan = util_strdup(buf.data);

    sb_truncate(&buf, 0);
    sb_appendf(&buf, "%s/plan.draft", p.session_dir);
    p.plan_draft = util_strdup(buf.data);

    sb_free(&buf);
    free(key);
    return p;
}

/* touch a file (create when missing) */
static int touch_file(const char *path) {
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    fclose(f);
    return 0;
}

const char *store_session_image_dir(const SessionPaths *paths) {
    static __thread char buf[1024];
    snprintf(buf, sizeof(buf), "%s/images", paths->session_dir);
    return buf;
}

int store_session_init(const SessionPaths *p, int is_new) {
    if (util_mkdirs(p->base_dir, 0755) != 0) return -1;
    if (util_mkdirs(p->session_dir, 0755) != 0) return -1;
    mkdir(store_session_image_dir(p), 0755);
    touch_file(p->conversation);
    touch_file(p->events);
    touch_file(p->summary);
    touch_file(p->plan);
    touch_file(p->plan_draft);

    if (is_new) {
        /* write the initial stats.json */
        FILE *f = fopen(p->stats, "w");
        if (!f) return -1;
        fprintf(f, "{\"current_turn_count\":0,\"agent_request_count\":0,"
                   "\"compact_request_count\":0,\"sub_agent_request_count\":0,"
                   "\"total_input_tokens\":0,"
                   "\"total_output_tokens\":0,\"total_cache_read_tokens\":0,"
                   "\"total_cache_creation_tokens\":0,\"current_context_tokens\":0,"
                   "\"last_updated\":\"\"}\n");
        fclose(f);

        /* write the session_start event (bash parity) */
        {
            StrBuf evt;
            sb_init(&evt);
            sb_append(&evt, "{\"type\":\"session_start\",\"session_id\":");
            /* extract session_id from the session_dir path */
            const char *sid = strrchr(p->session_dir, '/');
            sb_append_json_string(&evt, sid ? sid + 1 : "");
            sb_append_char(&evt, '}');
            store_event_append(p, evt.data);
            sb_free(&evt);
        }
    } else {
        touch_file(p->stats);
    }

    /* create the images directory */
    {
        char imgdir[1024];
        snprintf(imgdir, sizeof(imgdir), "%s/images", p->session_dir);
        util_mkdirs(imgdir, 0755);
    }

    return 0;
}

int store_session_fork(const SessionPaths *parent, const SessionPaths *child) {
    util_mkdirs(child->session_dir, 0755);
    char *parent_conv = util_read_file(parent->conversation);
    if (parent_conv && strlen(parent_conv) > 0) util_write_file(child->conversation, parent_conv);
    free(parent_conv);
    char *parent_summary = util_read_file(parent->summary);
    if (parent_summary && strlen(parent_summary) > 0) util_write_file(child->summary, parent_summary);
    free(parent_summary);
    char *parent_plan = util_read_file(parent->plan);
    if (parent_plan && strlen(parent_plan) > 0) util_write_file(child->plan, parent_plan);
    free(parent_plan);
    return 0;
}

int store_session_init_sub(const SessionPaths *parent_paths, const SessionPaths *sub_paths, int fork) {
    if (fork) {
        store_session_fork(parent_paths, sub_paths);
    }
    if (store_session_init(sub_paths, 1) != 0) return -1;
    return 0;
}

char *session_new_id(void) {
    time_t now = time(NULL);
    static int seeded = 0;
    if (!seeded) { srand((unsigned)(now ^ getpid())); seeded = 1; }
    struct tm *t = localtime(&now);
    unsigned short r = (unsigned short)(rand() % 0xFFFF);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d-%04x",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, r);
    return util_strdup(buf);
}

char *store_session_resolve_continue(const char *home, const char *cwd) {
    char *key = store_session_project_key(cwd);
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "%s/projects/%s", home, key);

    DIR *dir = opendir(buf.data);
    if (!dir) { sb_free(&buf); free(key); return NULL; }

    char *latest_id = NULL;
    long latest_time = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || strncmp(entry->d_name, "sub_", 4) == 0) continue;
        /* try to parse the dir name as a timestamp */
        struct stat st;
        sb_truncate(&buf, 0);
        sb_appendf(&buf, "%s/projects/%s/%s", home, key, entry->d_name);
        if (stat(buf.data, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        /* prefer events.jsonl mtime, fall back to dir mtime */
        time_t mtime = st.st_mtime;
        size_t base_len = buf.len;
        sb_append(&buf, "/events.jsonl");
        struct stat events_st;
        if (stat(buf.data, &events_st) == 0) {
            mtime = events_st.st_mtime;
        }
        sb_truncate(&buf, base_len);
        if (mtime > latest_time) {
            latest_time = mtime;
            free(latest_id);
            latest_id = util_strdup(entry->d_name);
        }
    }
    closedir(dir);
    sb_free(&buf);
    free(key);
    return latest_id;
}

int store_session_list_rows(const char *home, const char *cwd, StrBuf *out) {
    char *key = store_session_project_key(cwd);
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "%s/projects/%s", home, key);

    struct dirent **namelist;
    int n = scandir(buf.data, &namelist, NULL, alphasort);
    if (n < 0) { sb_free(&buf); free(key); return 0; }

    /* collect valid session names and mtimes */
    char **names = calloc(n, sizeof(char *));
    time_t *mtimes = calloc(n, sizeof(time_t));
    int valid = 0;

    for (int i = 0; i < n; i++) {
        struct dirent *entry = namelist[i];
        if (entry->d_name[0] == '.') { free(entry); continue; }
        struct stat st;
        sb_truncate(&buf, 0);
        sb_appendf(&buf, "%s/projects/%s/%s", home, key, entry->d_name);
        if (stat(buf.data, &st) != 0 || !S_ISDIR(st.st_mode)) { free(entry); continue; }
        names[valid] = util_strdup(entry->d_name);
        mtimes[valid] = st.st_mtime;
        valid++;
        free(entry);
    }
    free(namelist);

    /* sort by mtime, newest first */
    /* indirect sort via an index array */
    int *order = calloc(valid, sizeof(int));
    for (int i = 0; i < valid; i++) order[i] = i;
    /* selection sort (few sessions); compare mtimes[order[i]] */
    for (int i = 0; i < valid - 1; i++) {
        for (int j = i + 1; j < valid; j++) {
            if (mtimes[order[j]] > mtimes[order[i]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    int count = 0;
    for (int idx = 0; idx < valid; idx++) {
        int i = order[idx];
        struct stat st;
        sb_truncate(&buf, 0);
        sb_appendf(&buf, "%s/projects/%s/%s", home, key, names[i]);
        stat(buf.data, &st);

        /* modified: dir mtime, YYYY-MM-DD HH:MM (bash parity) */
        char time_buf[32];
        struct tm *tm = localtime(&st.st_mtime);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", tm);

        /* preview: first non-empty summary line; >60 chars -> 57 + ... */
        char preview[1024];
        preview[0] = '\0';
        size_t base_len = buf.len;
        sb_append(&buf, "/summary.txt");
        FILE *fp = fopen(buf.data, "r");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                /* trim leading/trailing whitespace */
                char *start = line;
                while (*start && isspace((unsigned char)*start)) start++;
                if (*start) {
                    size_t len = strlen(start);
                    while (len > 0 && isspace((unsigned char)start[len - 1])) start[--len] = '\0';
                    strncpy(preview, start, sizeof(preview) - 1);
                    preview[sizeof(preview) - 1] = '\0';
                    break;
                }
            }
            fclose(fp);
        }
        sb_truncate(&buf, base_len);

        /* truncate by UTF-8 char count (bash ${#preview} parity) */
        util_truncate_chars(preview, 60);

        sb_appendf(out, "%-40s %-16s %s\n", names[i], time_buf, preview);
        count++;
    }

    for (int i = 0; i < valid; i++) FREE_PTR(names[i]);
    free(names);
    free(mtimes);
    free(order);
    sb_free(&buf);
    free(key);
    return count;
}

/* ============================================================
 * conversation.jsonl operations
 * ============================================================ */

int store_conv_add_user(const char *path, const char *content) {
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "{\"role\":\"user\",\"content\":");
    sb_append_json_string(&buf, content);
    sb_append(&buf, "}");
    int rc = jsonl_append(path, buf.data);
    sb_free(&buf);
    return rc;
}

int store_conv_add_assistant(const char *path, const char *thinking, const char *text,
                       int tool_count, const char **tool_ids,
                       const char **tool_names, const char **tool_inputs) {
    StrBuf buf;
    int first = 1;
    sb_init(&buf);
    sb_append(&buf, "{\"role\":\"assistant\",\"content\":[");

    /* thinking block - only when non-empty (Claude API rejects empty) */
    if (thinking && thinking[0]) {
        sb_append(&buf, "{\"type\":\"thinking\",\"thinking\":");
        sb_append_json_string(&buf, thinking);
        sb_append(&buf, "}");
        first = 0;
    }

    /* text block - same */
    if (text && text[0]) {
        if (!first) sb_append(&buf, ",");
        sb_append(&buf, "{\"type\":\"text\",\"text\":");
        sb_append_json_string(&buf, text);
        sb_append(&buf, "}");
        first = 0;
    }

    /* tool_use blocks */
    for (int i = 0; i < tool_count; i++) {
        if (!first) sb_append(&buf, ",");
        sb_appendf(&buf, "{\"type\":\"tool_use\",\"id\":");
        sb_append_json_string(&buf, tool_ids[i]);
        sb_append(&buf, ",\"name\":");
        sb_append_json_string(&buf, tool_names[i]);
        sb_append(&buf, ",\"input\":");
        sb_append(&buf, tool_inputs[i]); /* already JSON */
        sb_append(&buf, "}");
        first = 0;
    }

    /* all-empty message gets a placeholder text to stay legal */
    if (first)
        sb_append(&buf, "{\"type\":\"text\",\"text\":\"(empty)\"}");

    sb_append(&buf, "]}");
    int rc = jsonl_append(path, buf.data);
    sb_free(&buf);
    return rc;
}

int store_conv_add_tool_results(const char *path, int count, const char **tool_use_ids,
                          const char **contents) {
    StrBuf buf;
    sb_init(&buf);
    sb_append(&buf, "{\"role\":\"user\",\"content\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0) sb_append(&buf, ",");
        sb_append(&buf, "{\"type\":\"tool_result\",\"tool_use_id\":");
        sb_append_json_string(&buf, tool_use_ids[i]);
        sb_append(&buf, ",\"content\":");
        sb_append_json_string(&buf, contents[i]);
        sb_append(&buf, "}");
    }
    sb_append(&buf, "]}");
    int rc = jsonl_append(path, buf.data);
    sb_free(&buf);
    return rc;
}

int store_conv_line_count(const char *path, char ***out, int *out_count) {
    *out = NULL;
    *out_count = 0;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int cap = 64;
    int count = 0;
    char **lines = malloc(cap * sizeof(char *));
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t read_len;
    if (!lines) goto fail;

    /* getline grows on real newlines; a long JSONL record is one line */
    while ((read_len = getline(&line, &line_cap, f)) != -1) {
        /* strip trailing newline */
        size_t len = (size_t)read_len;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        if (count >= cap) {
            int new_cap = cap * 2;
            char **new_lines = realloc(lines, new_cap * sizeof(char *));
            if (!new_lines) goto fail;
            lines = new_lines;
            cap = new_cap;
        }
        lines[count] = util_strdup(line);
        if (!lines[count]) goto fail;
        count++;
    }

    free(line);
    fclose(f);
    *out = lines;
    *out_count = count;
    return 0;

fail:
    free(line);
    fclose(f);
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return -1;
}

int store_conv_trim_tail(const char *path, int keep_lines) {
    char **lines = NULL;
    int count = 0;
    if (store_conv_line_count(path, &lines, &count) != 0) return -1;
    if (keep_lines >= count) {
        for (int i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return 0;
    }

    /* rewrite keeping only the last keep_lines lines */
    FILE *f = fopen(path, "w");
    if (!f) {
        for (int i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return -1;
    }
    int start = count - keep_lines;
    for (int i = start; i < count; i++) {
        fprintf(f, "%s\n", lines[i]);
    }
    fclose(f);
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return 0;
}

int store_conv_user_turn_count(const char *path) {
    char **lines = NULL;
    int count = 0;
    if (store_conv_line_count(path, &lines, &count) != 0) return 0;
    int user_count = 0;
    for (int i = 0; i < count; i++) {
        JsonParse jp = json_parse_root(lines[i]);
        if (jp.error) continue;
        char *role = json_get_string(jp.val, "role");
        if (role && strcmp(role, "user") == 0) {
            JsonVal content = json_get(jp.val, "content");
            if (content.type == JSON_STRING) {
                user_count++;
            }
        }
        free(role);
    }
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return user_count;
}

long store_conv_total_bytes(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

/* ============================================================
 * stats.json operations
 * ============================================================ */

char *store_stats_read(const char *path) {
    return util_read_file(path);
}

static void stats_write_canonical(const char *path,
                                  int current_turn_count,
                                  int agent_request_count,
                                  int compact_request_count,
                                  int sub_agent_request_count,
                                  int total_input_tokens,
                                  int total_output_tokens,
                                  int total_cache_read_tokens,
                                  int total_cache_creation_tokens,
                                  int current_context_tokens,
                                  const char *last_updated) {
    StrBuf buf;
    sb_init(&buf);
    sb_appendf(&buf, "{\"current_turn_count\":%d,\"agent_request_count\":%d,"
               "\"compact_request_count\":%d,\"sub_agent_request_count\":%d,"
               "\"total_input_tokens\":%d,\"total_output_tokens\":%d,"
               "\"total_cache_read_tokens\":%d,\"total_cache_creation_tokens\":%d,"
               "\"current_context_tokens\":%d,\"last_updated\":",
               current_turn_count, agent_request_count, compact_request_count,
               sub_agent_request_count, total_input_tokens, total_output_tokens,
               total_cache_read_tokens, total_cache_creation_tokens,
               current_context_tokens);
    sb_append_json_string(&buf, last_updated ? last_updated : "");
    sb_append(&buf, "}\n");
    util_write_file(path, buf.data);
    sb_free(&buf);
}

void store_stats_add_int(JsonVal obj, const char *key, int delta) {
    int cur = store_stats_get_int(obj, key);
    store_stats_set_int(obj, key, cur + delta);
}

void store_stats_set_int(JsonVal obj, const char *key, int value) {
    /* modify a numeric value inside the JSON source in place.
     * Used only as the store_stats_update callback.
     * If the new number fits, overwrite; pad leftover digits with spaces.
     * If it does not fit, skip (cannot grow in place). */
    if (!obj.src) return;
    /* find "key":<number> in the source text */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(obj.src, search);
    if (!p) return;
    p += strlen(search);
    /* skip whitespace and the colon */
    while (*p == ' ' || *p == ':') p++;
    /* p now points at the start of the value */
    const char *val_start = p;
    /* find the end of the value (comma, } or whitespace) */
    while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\r') p++;
    int old_len = (int)(p - val_start);
    char new_val[32];
    snprintf(new_val, sizeof(new_val), "%d", value);
    int new_len = (int)strlen(new_val);
    if (new_len > old_len) return; /* cannot grow in place */
    memcpy((char*)val_start, new_val, new_len);
    /* pad leftover positions with spaces */
    for (int i = new_len; i < old_len; i++) ((char*)val_start)[i] = ' ';
}

int store_stats_get_int(JsonVal obj, const char *key) {
    return json_get_int(obj, key);
}

/* simple file-level helper: read an integer field from the stats file */
int store_stats_get_file_int(const char *path, const char *key) {
    char *content = store_stats_read(path);
    if (!content) return 0;
    JsonParse jp = json_parse_root(content);
    int val = jp.error ? 0 : json_get_int(jp.val, key);
    free(content);
    return val;
}

/* Set an integer field in the stats file.
 * Reads existing fields; missing/invalid read as 0; writes canonical stats JSON.
 * Old versions missing fields get them on the next write; no backfill. */
int store_stats_get_int_file(const char *path, const char *key)
{
	char *txt = store_stats_read(path);
	JsonParse jp;
	int v = 0;
	if (!txt) return 0;
	jp = json_parse_root(txt);
	if (!jp.error)
		v = store_stats_get_int(jp.val, key);
	free(txt);
	return v;
}

void store_stats_set_int_file(const char *path, const char *key, int value) {
    int current_turn_count = 0, agent_request_count = 0;
    int compact_request_count = 0, sub_agent_request_count = 0;
    int total_input_tokens = 0, total_output_tokens = 0;
    int total_cache_read_tokens = 0, total_cache_creation_tokens = 0;
    int current_context_tokens = 0;

    char *content = store_stats_read(path);
    if (content && content[0]) {
        JsonParse jp = json_parse_root(content);
        if (!jp.error) {
            current_turn_count = json_get_int(jp.val, "current_turn_count");
            agent_request_count = json_get_int(jp.val, "agent_request_count");
            compact_request_count = json_get_int(jp.val, "compact_request_count");
            sub_agent_request_count = json_get_int(jp.val, "sub_agent_request_count");
            total_input_tokens = json_get_int(jp.val, "total_input_tokens");
            total_output_tokens = json_get_int(jp.val, "total_output_tokens");
            total_cache_read_tokens = json_get_int(jp.val, "total_cache_read_tokens");
            total_cache_creation_tokens = json_get_int(jp.val, "total_cache_creation_tokens");
            current_context_tokens = json_get_int(jp.val, "current_context_tokens");
        }
    }

    if (strcmp(key, "current_turn_count") == 0) current_turn_count = value;
    else if (strcmp(key, "agent_request_count") == 0) agent_request_count = value;
    else if (strcmp(key, "compact_request_count") == 0) compact_request_count = value;
    else if (strcmp(key, "sub_agent_request_count") == 0) sub_agent_request_count = value;
    else if (strcmp(key, "total_input_tokens") == 0) total_input_tokens = value;
    else if (strcmp(key, "total_output_tokens") == 0) total_output_tokens = value;
    else if (strcmp(key, "total_cache_read_tokens") == 0) total_cache_read_tokens = value;
    else if (strcmp(key, "total_cache_creation_tokens") == 0) total_cache_creation_tokens = value;
    else if (strcmp(key, "current_context_tokens") == 0) current_context_tokens = value;

    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    stats_write_canonical(path, current_turn_count, agent_request_count,
                          compact_request_count, sub_agent_request_count,
                          total_input_tokens, total_output_tokens,
                          total_cache_read_tokens, total_cache_creation_tokens,
                          current_context_tokens, ts);
    if (content) {
        free(content);
    }
}

/* generic stats update: read -> mutate -> write back */
int store_stats_update(const char *path, stats_update_fn fn, void *ctx) {
    char *content = util_read_file(path);
    if (!content) return -1;

    JsonParse jp = json_parse_root(content);
    if (jp.error) { free(content); return -1; }

    /* let the callback mutate (re-serialize via StrBuf) */
    fn(ctx, jp.val);

    /* re-serialize */
    StrBuf buf;
    sb_init(&buf);
    sb_append_char(&buf, '{');
    JsonObjectIter it;
    json_obj_iter_init(&it, jp.val);
    int first = 1;
    while (json_obj_iter_next(&it)) {
        if (!first) sb_append(&buf, ",");
        first = 0;
        sb_append_json_string(&buf, it.key);
        sb_append_char(&buf, ':');
        /* value taken verbatim from the source text */
        size_t vlen = it.val.end - it.val.start;
        sb_appendn(&buf, jp.val.src + it.val.start, vlen);
    }
    sb_append_char(&buf, '}');
    sb_append_char(&buf, '\n');

    int rc = util_write_file(path, buf.data);
    sb_free(&buf);
    free(content);
    return rc;
}

/* ============================================================
 * events.jsonl operations
 * ============================================================ */

static _Thread_local int g_store_event_stream_json = 0;

void store_event_set_stream_json(int enabled) {
    g_store_event_stream_json = enabled ? 1 : 0;
}

int store_event_stream_json_enabled(void) {
    return g_store_event_stream_json;
}

int store_event_append(const SessionPaths *p, const char *json_str) {
    if (g_store_event_stream_json) {
        printf("%s\n", json_str);
        fflush(stdout);
    }
    return jsonl_append(p->events, json_str);
}

int store_event_lines(const SessionPaths *p, char ***out, int *out_count) {
    return store_conv_line_count(p->events, out, out_count);
}

/* ============================================================
 * summary / plan file operations
 * ============================================================ */

char *store_summary_get(const SessionPaths *p) {
    char *s = util_read_file(p->summary);
    if (s) {
        size_t len = strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'))
            s[--len] = '\0';
        if (len == 0) { free(s); return NULL; }
    }
    return s;
}

int store_summary_set(const SessionPaths *p, const char *content) {
    return util_write_file(p->summary, content);
}

char *store_plan_draft_read(const SessionPaths *p) {
    return util_read_file(p->plan_draft);
}

int store_plan_draft_set(const SessionPaths *p, const char *content) {
    return util_write_file(p->plan_draft, content);
}

int store_plan_draft_clear(const SessionPaths *p) {
    return util_write_file(p->plan_draft, "");
}

int store_plan_set(const SessionPaths *p, const char *content) {
    return util_write_file(p->plan, content);
}

int store_plan_clear(const SessionPaths *p) {
    return util_write_file(p->plan, "");
}

/* ==== ba_display.c ==== */
/*
 * ba_display.c - synchronous display layer, ported from bash-agent
 * display.c / agent_tool_display_summary. Rendering rules, ANSI colors,
 * truncation and stream-json shapes are verbatim; linenoise output calls
 * degrade to plain stdio because busyagent is always non-interactive.
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include <string.h>
#include <stdio.h>

/* ---- DisplayState (ported verbatim) ---- */
static void ds_init(BaDisplay *ds) {
    memset(ds, 0, sizeof(*ds));
    ds->last_char[0] = '\n';
    ds->last_char[1] = '\0';
    ds->prev_was_thinking = 0;
}

static void ds_update_last_char(BaDisplay *ds, const char *text) {
    if (!text || !*text) return;
    {
        const char *p = text;
        const char *last = p;
        while (*p) {
            last = p;
            unsigned char c = (unsigned char)*p;
            if (c < 0x80) p++;
            else if (c < 0xE0) p += 2;
            else if (c < 0xF0) p += 3;
            else p += 4;
        }
        size_t len = p - last;
        if (len > 0 && len < 8) {
            memcpy(ds->last_char, last, len);
            ds->last_char[len] = '\0';
        }
    }
}

/* non-interactive equivalent of linenoiseWrite (no raw-mode terminal) */
static void lw_write(const char *s, size_t n) {
    /* bash-agent's linenoiseWrite flushes immediately; streamed deltas
     * must not sit in stdout buffering (would delay until exit) */
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

#include <stdarg.h>
static void lw_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void ensure_newline(BaDisplay *ds) {
    if (ds->last_char[0] != '\n') {
        lw_write("\n", 1);
        ds->last_char[0] = '\n';
        ds->last_char[1] = '\0';
    }
}

void ba_disp_init(BaDisplay *d, BaDisplayFormat fmt)
{
    ds_init(d);
    d->format = fmt;
    d->out = stdout;
}

/* ---- tool-call summary (verbatim port of agent_tool_display_summary) ---- */
char *ba_tool_call_summary(const char *name, const char *input_json)
{
    char *field = NULL;
    JsonParse jp = json_parse_root(input_json && input_json[0] ? input_json : "{}");

    if (!jp.error) {
        if (!strcmp(name, "Read") || !strcmp(name, "Write") || !strcmp(name, "Edit")) {
            field = json_get_string(jp.val, "path");
        } else if (!strcmp(name, "Glob") || !strcmp(name, "Grep")) {
            field = json_get_string(jp.val, "pattern");
        } else if (!strcmp(name, "Bash")) {
            field = json_get_string(jp.val, "command");
            /* newlines to spaces, truncate long commands (bash parity) */
            if (field) {
                char *p;
                while ((p = strchr(field, '\n')) != NULL) *p = ' ';
                size_t flen = strlen(field);
                if (flen > 80) {
                    size_t slen = util_utf8_truncate_len(field, 77);
                    char *trunc = malloc(slen + 4);
                    memcpy(trunc, field, slen);
                    strcpy(trunc + slen, "...");
                    free(field);
                    field = trunc;
                }
            }
        } else if (!strcmp(name, "TodoWrite")) {
            JsonVal todos_arr = json_get(jp.val, "todos");
            if (todos_arr.type == JSON_ARRAY) {
                int total = json_array_len(todos_arr);
                int comp = 0;
                for (int ti = 0; ti < total; ti++) {
                    JsonVal it = json_array_get(todos_arr, ti);
                    char *st = json_get_string(it, "status");
                    if (st && strcmp(st, "completed") == 0) comp++;
                    free(st);
                }
                char buf2[32];
                snprintf(buf2, sizeof(buf2), "%d/%d", comp, total);
                field = xstrdup(buf2);
            }
        } else if (!strcmp(name, "Skill")) {
            field = json_get_string(jp.val, "name");
        } else if (!strcmp(name, "SubAgent")) {
            field = json_get_string(jp.val, "description");
        }
    }

    if (field)
        return field;
    if (input_json && input_json[0]) {
        size_t len = strlen(input_json);
        size_t cut = len > 80 ? util_utf8_truncate_len(input_json, 77) : len;
        char *s = xmalloc(cut + 4);
        memcpy(s, input_json, cut);
        strcpy(s + cut, len > 80 ? "..." : "");
        return s;
    }
    return xstrdup("");
}

/* ---- main renderer: verbatim port of the render_message branches ---- */
char *ba_display_push(BaDisplay *ds, const BaDisplayMsg *msg)
{
    StrBuf buf;

    if (ds->format == BA_FMT_NONE)
        return NULL;

    if (ds->format == BA_FMT_STREAM_JSON) {
        sb_init(&buf);
        switch (msg->type) {
        case BA_DM_TEXT:
            sb_append(&buf, "{\"type\":\"text\",\"content\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_THINKING:
            sb_append(&buf, "{\"type\":\"thinking\",\"content\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_TOOL_CALL:
            sb_append(&buf, "{\"type\":\"tool_call\",\"name\":");
            sb_append_json_string(&buf, msg->tool_name ? msg->tool_name : "");
            sb_append(&buf, ",\"id\":");
            sb_append_json_string(&buf, msg->tool_id ? msg->tool_id : "");
            sb_append(&buf, ",\"input\":");
            sb_append(&buf, (msg->tool_input && msg->tool_input[0]) ? msg->tool_input : "{}");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_TOOL_RESULT:
            sb_append(&buf, "{\"type\":\"tool_result\",\"tool_use_id\":");
            sb_append_json_string(&buf, msg->tool_id ? msg->tool_id : "");
            sb_append(&buf, ",\"name\":");
            sb_append_json_string(&buf, msg->tool_name ? msg->tool_name : "");
            sb_append(&buf, ",\"content\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_USAGE:
            sb_appendf(&buf, "{\"type\":\"usage\",\"input_tokens\":%d,\"output_tokens\":%d,"
                       "\"cache_read_input_tokens\":%d,\"cache_creation_input_tokens\":%d,"
                       "\"kind\":\"agent\"}",
                       msg->in_tokens, msg->out_tokens,
                       msg->cache_read_tokens, msg->cache_creation_tokens);
            break;
        case BA_DM_STOP:
            sb_append(&buf, "{\"type\":\"stop\",\"reason\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_ERROR:
            sb_append(&buf, "{\"type\":\"error\",\"message\":");
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_SUB_AGENT_START:
            sb_append(&buf, "{\"type\":\"sub_agent_start\",\"session_id\":");
            sb_append_json_string(&buf, msg->session_id ? msg->session_id : "");
            sb_append_char(&buf, '}');
            break;
        case BA_DM_SUB_AGENT_RESULT:
            sb_append(&buf, "{\"type\":\"sub_agent_result\",\"session_id\":");
            sb_append_json_string(&buf, msg->session_id ? msg->session_id : "");
            sb_appendf(&buf, ",\"status\":\"%s\"",
                       msg->tool_exit_code == 0 ? "ok" : "failed");
            sb_appendf(&buf, ",\"input_tokens\":%d,\"output_tokens\":%d}",
                       msg->in_tokens, msg->out_tokens);
            break;
        case BA_DM_ASYNC_TASK_RESULT:
            sb_append(&buf, "{\"type\":\"async_task_result\",\"task_id\":");
            sb_append_json_string(&buf, msg->session_id ? msg->session_id : "");
            sb_appendf(&buf, ",\"exit_code\":%d,\"output\":",
                       msg->tool_exit_code);
            sb_append_json_string(&buf, msg->content ? msg->content : "");
            sb_append_char(&buf, '}');
            break;
        default:
            sb_free(&buf);
            return NULL;
        }
        fprintf(ds->out, "%s\n", buf.data);
        fflush(ds->out);
        return buf.data;
    }

    /* human mode (ANSI/truncation rules ported verbatim) */
    switch (msg->type) {
    case BA_DM_THINKING:
        if (msg->content)
            lw_printf("\x1b[90m%s\x1b[0m", msg->content);
        ds_update_last_char(ds, msg->content);
        ds->prev_was_thinking = 1;
        break;

    case BA_DM_TEXT:
        if (msg->content) {
            if (ds->prev_was_thinking && ds->last_char[0] != '\n') {
                lw_write("\n", 1);
                ds->last_char[0] = '\n';
            }
            lw_write(msg->content, strlen(msg->content));
            ds_update_last_char(ds, msg->content);
        }
        ds->prev_was_thinking = 0;
        break;

    case BA_DM_TOOL_CALL: {
        ensure_newline(ds);
        const char *name = msg->tool_name ? msg->tool_name : "unknown";
        const char *summary = msg->content ? msg->content : "";
        lw_printf("\x1b[33m[tool] %s(%s)\x1b[0m\n", name, summary);
        ds->last_char[0] = '\n';
        ds->prev_was_thinking = 0;
        break;
    }

    case BA_DM_TOOL_RESULT:
        if (msg->content && msg->content[0]) {
            if (ds->prev_was_thinking && ds->last_char[0] != '\n')
                lw_write("\n", 1);
            ds->prev_was_thinking = 0;
            /* bash-agent display.c:222-236 - Edit prints in full; Read and
             * Write show only the first line (whole files stay out of the
             * terminal) */
            if (msg->tool_name && strcmp(msg->tool_name, "Edit") == 0) {
                lw_printf("%s\n", msg->content);
            } else if (msg->tool_name
                    && (strcmp(msg->tool_name, "Read") == 0
                     || strcmp(msg->tool_name, "Write") == 0)) {
                const char *nl = strchr(msg->content, '\n');
                if (nl) {
                    lw_write(msg->content, (size_t)(nl - msg->content));
                    lw_write("\n", 1);
                } else {
                    lw_printf("%s\n", msg->content);
                }
            } else {
                lw_printf("%s\n", msg->content);
            }
            ds->last_char[0] = '\n';
        }
        break;

    case BA_DM_USAGE:
        break;

    case BA_DM_STOP:
        ensure_newline(ds);
        if (msg->content && strcmp(msg->content, "interrupted") == 0)
            lw_printf("\x1b[36mInterrupted.\x1b[0m\n");
        ds->last_char[0] = '\n';
        break;

    case BA_DM_ERROR:
        ensure_newline(ds);
        lw_printf("\x1b[31mError: %s\x1b[0m\n",
                msg->content ? msg->content : "unknown");
        ds->last_char[0] = '\n';
        break;

    case BA_DM_CONTEXT_UPDATE:
        ensure_newline(ds);
        lw_printf("\x1b[36mContext compacted (%s).\x1b[0m\n",
                msg->tool_name ? msg->tool_name : "auto");
        ds->last_char[0] = '\n';
        break;

    case BA_DM_SUB_AGENT_START:
        break;   /* no human output (same as display.c:250) */

    case BA_DM_SUB_AGENT_RESULT: {
        ensure_newline(ds);
        if (msg->tool_exit_code == 0)
            lw_printf("\x1b[35m[sub-agent %s] completed (in=%d, out=%d)\x1b[0m\n",
                    msg->session_id ? msg->session_id : "?",
                    msg->in_tokens, msg->out_tokens);
        else
            lw_printf("\x1b[31m[sub-agent %s] failed\x1b[0m\n",
                    msg->session_id ? msg->session_id : "?");
        if (msg->tool_name && msg->tool_name[0]) {
            int tlen = (int)util_utf8_truncate_len(msg->tool_name, 120);
            lw_printf("\x1b[90m%.*s%s\x1b[0m\n",
                    tlen, msg->tool_name,
                    strlen(msg->tool_name) > 120 ? "\xe2\x80\xa6" : "");
        }
        if (msg->content && msg->content[0]) {
            int clen = (int)util_utf8_truncate_len(msg->content, 120);
            lw_printf("%.*s%s\n",
                    clen, msg->content,
                    strlen(msg->content) > 120 ? "\xe2\x80\xa6" : "");
        }
        ds->last_char[0] = '\n';
        ds->prev_was_thinking = 0;
        break;
    }

    case BA_DM_ASYNC_TASK_RESULT: {
        ensure_newline(ds);
        lw_printf("\x1b[%sm[bg-bash %s] exit_code=%d\x1b[0m\n",
                msg->tool_exit_code == 0 ? "36" : "31",
                msg->session_id ? msg->session_id : "?", msg->tool_exit_code);
        if (msg->content && msg->content[0]) {
            int clen = (int)util_utf8_truncate_len(msg->content, 120);
            lw_printf("%.*s%s\n",
                    clen, msg->content,
                    strlen(msg->content) > 120 ? "\xe2\x80\xa6" : "");
        }
        ds->last_char[0] = '\n';
        ds->prev_was_thinking = 0;
        break;
    }

    default:
        break;
    }
    return NULL;
}

/* ==== ba_transport.c ==== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>


static void emit_simple_event(sse_callback_fn callback, void *ctx,
                              SseEventType type, const char *content);
static void fill_openai_usage_event(SseEvent *evt, JsonVal usage);
static void process_residual_json(StreamCtx *sctx, const char *provider,
                              sse_callback_fn callback, void *ctx);

static void streamctx_free_openai_tools(StreamCtx *sctx) {
    for (int i = 0; i < sctx->responses_item_count; i++) FREE_PTR(sctx->responses_item_ids[i]);
    FREE_PTR(sctx->responses_item_ids);
    FREE_PTR(sctx->responses_item_indexes);
    sctx->responses_item_count = 0;
    sctx->responses_item_cap = 0;
    for (int i = 0; i < sctx->openai_tool_count; i++) {
        FREE_PTR(sctx->openai_tools[i].id);
        FREE_PTR(sctx->openai_tools[i].name);
        sb_free(&sctx->openai_tools[i].arguments);
    }
    FREE_PTR(sctx->openai_tools);
    sctx->openai_tool_count = 0;
    sctx->openai_tool_cap = 0;
}

static void streamctx_reset_openai_tool(OpenAIToolAccum *tool) {
    FREE_PTR(tool->id);
    FREE_PTR(tool->name);
    sb_free(&tool->arguments);
    memset(tool, 0, sizeof(*tool));
}

static OpenAIToolAccum *streamctx_ensure_openai_tool(StreamCtx *sctx, int idx) {
    for (int i = 0; i < sctx->openai_tool_count; i++) {
        if (sctx->openai_tools[i].index == idx) return &sctx->openai_tools[i];
    }
    if (sctx->openai_tool_count >= sctx->openai_tool_cap) {
        int old_cap = sctx->openai_tool_cap;
        sctx->openai_tool_cap = sctx->openai_tool_cap ? sctx->openai_tool_cap * 2 : 4;
        sctx->openai_tools = realloc(sctx->openai_tools,
            (size_t)sctx->openai_tool_cap * sizeof(*sctx->openai_tools));
        memset(sctx->openai_tools + old_cap, 0,
            (size_t)(sctx->openai_tool_cap - old_cap) * sizeof(*sctx->openai_tools));
    }
    OpenAIToolAccum *tool = &sctx->openai_tools[sctx->openai_tool_count++];
    tool->index = idx;
    sb_init(&tool->arguments);
    return tool;
}

static void streamctx_emit_openai_tool_calls(StreamCtx *sctx) {
    for (int i = 0; i < sctx->openai_tool_count; i++) {
        OpenAIToolAccum *tool = &sctx->openai_tools[i];
        if (tool->arguments.len == 0) continue;
        SseEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SSE_TOOL_CALL;
        evt.tool_id = tool->id ? tool->id : "";
        evt.tool_name = tool->name ? tool->name : "";
        evt.tool_input = tool->arguments.data ? tool->arguments.data : "{}";
        sctx->callback(sctx->ctx, &evt);
        streamctx_reset_openai_tool(tool);
    }
    sctx->openai_tool_count = 0;
}

static void parse_openai_sse_event(StreamCtx *sctx, const char *data, size_t data_len) {
    if (data_len == 0) return;
    if (strcmp(data, "[DONE]") == 0) return;

    size_t pos = 0;
    JsonParse jp = json_parse(data, &pos);
    if (jp.error) return;

    char *obj_type = json_get_string(jp.val, "object");
    /* strict only when the field is present: several OpenAI-compatible
     * gateways omit "object" on stream chunks */
    if (obj_type && strcmp(obj_type, "chat.completion.chunk") != 0) {
        FREE_PTR(obj_type);
        return;
    }
    FREE_PTR(obj_type);

    JsonVal choices = json_get(jp.val, "choices");
    if (choices.type == JSON_ARRAY) {
        JsonVal choice = json_array_get(choices, 0);
        JsonVal delta = json_get(choice, "delta");
        char *content = json_get_string(delta, "content");
        if (content) {
            emit_simple_event(sctx->callback, sctx->ctx, SSE_TEXT, content);
            FREE_PTR(content);
        }
        char *reasoning = json_get_string(delta, "reasoning_content");
        if (!reasoning) reasoning = json_get_string(delta, "reasoning");
        if (reasoning) {
            emit_simple_event(sctx->callback, sctx->ctx, SSE_THINKING, reasoning);
            FREE_PTR(reasoning);
        }
        JsonVal tool_calls = json_get(delta, "tool_calls");
        if (tool_calls.type == JSON_ARRAY) {
            int tc_len = json_array_len(tool_calls);
            for (int i = 0; i < tc_len; i++) {
                JsonVal tc = json_array_get(tool_calls, i);
                int idx = json_get_int(tc, "index");
                JsonVal fn = json_get(tc, "function");
                OpenAIToolAccum *tool = streamctx_ensure_openai_tool(sctx, idx);
                char *id = json_get_string(tc, "id");
                char *name = json_get_string(fn, "name");
                char *arguments = json_get_string(fn, "arguments");
                /* non-standard OpenAI-compatible APIs (e.g. sensenova) send empty
                 * strings "" instead of omitting fields; [0] guard */
                if (id && id[0]) {
                    FREE_PTR(tool->id);
                    tool->id = id;
                } else {
                    FREE_PTR(id);
                }
                if (name && name[0]) {
                    FREE_PTR(tool->name);
                    tool->name = name;
                } else {
                    FREE_PTR(name);
                }
                if (arguments) {
                    sb_append(&tool->arguments, arguments);
                    FREE_PTR(arguments);
                }
            }
        }
        char *finish = json_get_string(choice, "finish_reason");
        /* non-standard APIs may use "" instead of null (e.g. sensenova);
         * an empty string must not trigger STOP */
        if (finish && finish[0]) {
            if (strcmp(finish, "tool_calls") == 0) {
                streamctx_emit_openai_tool_calls(sctx);
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "tool_use");
            } else if (strcmp(finish, "stop") == 0) {
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "end_turn");
            } else if (strcmp(finish, "length") == 0) {
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "max_tokens");
            } else {
                emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, finish);
            }
            FREE_PTR(finish);
        }
    }

    JsonVal usage = json_get(jp.val, "usage");
    if (usage.type != JSON_NULL) {
        SseEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SSE_USAGE;
        fill_openai_usage_event(&evt, usage);
        sctx->callback(sctx->ctx, &evt);
    }
}



static int responses_tool_index(StreamCtx *sctx, JsonVal root, JsonVal item) {
    char *item_id = json_get_string(root, "item_id");
    if (!item_id && item.type != JSON_NULL) item_id = json_get_string(item, "id");
    JsonVal output_index = json_get(root, "output_index");
    int idx = output_index.type == JSON_NUMBER ? json_get_int(root, "output_index") : -1;
    if (idx < 0 && item_id) {
        for (int i = 0; i < sctx->responses_item_count; i++) {
            if (strcmp(sctx->responses_item_ids[i], item_id) == 0) { idx = sctx->responses_item_indexes[i]; break; }
        }
    }
    if (idx >= 0 && item_id) {
        int found = 0;
        for (int i = 0; i < sctx->responses_item_count; i++) if (strcmp(sctx->responses_item_ids[i], item_id) == 0) { found = 1; break; }
        if (!found) {
            if (sctx->responses_item_count >= sctx->responses_item_cap) {
                sctx->responses_item_cap = sctx->responses_item_cap ? sctx->responses_item_cap * 2 : 4;
                sctx->responses_item_ids = realloc(sctx->responses_item_ids, (size_t)sctx->responses_item_cap * sizeof(char *));
                sctx->responses_item_indexes = realloc(sctx->responses_item_indexes, (size_t)sctx->responses_item_cap * sizeof(int));
            }
            int pos = sctx->responses_item_count++;
            sctx->responses_item_ids[pos] = util_strdup(item_id);
            sctx->responses_item_indexes[pos] = idx;
        }
    }
    FREE_PTR(item_id);
    return idx;
}

static void responses_record_usage(StreamCtx *sctx, JsonVal response) {
    JsonVal usage = json_get(response, "usage");
    if (usage.type == JSON_NULL) usage = response;
    sctx->responses_output_tokens = json_get_int(usage, "output_tokens");
    JsonVal input_details = json_get(usage, "input_tokens_details");
    int nested_cached = input_details.type == JSON_NULL ? 0 : json_get_int(input_details, "cached_tokens");
    sctx->responses_cache_read_tokens = nested_cached > 0
        ? nested_cached
        : json_get_int(usage, "cached_tokens");
    sctx->responses_input_tokens = json_get_int(usage, "input_tokens") - sctx->responses_cache_read_tokens;
    if (sctx->responses_input_tokens < 0) sctx->responses_input_tokens = 0;
}

static void responses_emit_usage(StreamCtx *sctx) {
    SseEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = SSE_USAGE;
    evt.in_tokens = sctx->responses_input_tokens;
    evt.out_tokens = sctx->responses_output_tokens;
    evt.cache_read_tokens = sctx->responses_cache_read_tokens;
    sctx->callback(sctx->ctx, &evt);
}

static void parse_responses_sse_event(StreamCtx *sctx, const char *event, const char *data, size_t data_len) {
    if (data_len == 0) return;
    size_t pos = 0;
    JsonParse jp = json_parse(data, &pos);
    if (jp.error) return;
    JsonVal root = jp.val;
    if (strcmp(event, "response.reasoning_text.delta") == 0) {
        char *delta = json_get_string(root, "delta");
        if (delta && delta[0]) emit_simple_event(sctx->callback, sctx->ctx, SSE_THINKING, delta);
        FREE_PTR(delta);
    } else if (strcmp(event, "response.output_text.delta") == 0) {
        char *delta = json_get_string(root, "delta");
        if (delta) {
            char *text = delta;
            if (!sctx->responses_saw_text) while (*text == '\n' || *text == '\r') text++;
            if (*text) { sctx->responses_saw_text = 1; emit_simple_event(sctx->callback, sctx->ctx, SSE_TEXT, text); }
        }
        FREE_PTR(delta);
    } else if (strcmp(event, "response.output_item.added") == 0 || strcmp(event, "response.output_item.done") == 0) {
        JsonVal item = json_get(root, "item");
        char *type = json_get_string(item, "type");
        if (type && strcmp(type, "function_call") == 0) {
            int idx = responses_tool_index(sctx, root, item);
            if (idx < 0) { FREE_PTR(type); return; }
            OpenAIToolAccum *tool = streamctx_ensure_openai_tool(sctx, idx);
            char *id = json_get_string(item, "call_id");
            char *name = json_get_string(item, "name");
            char *args = json_get_string(item, "arguments");
            if (id && id[0]) { FREE_PTR(tool->id); tool->id = id; } else FREE_PTR(id);
            if (name && name[0]) { FREE_PTR(tool->name); tool->name = name; } else FREE_PTR(name);
            if (args && args[0]) { sb_truncate(&tool->arguments, 0); sb_append(&tool->arguments, args); }
            FREE_PTR(args);
        }
        FREE_PTR(type);
    } else if (strcmp(event, "response.function_call_arguments.delta") == 0) {
        int idx = responses_tool_index(sctx, root, json_get(root, "item"));
        if (idx < 0) return;
        OpenAIToolAccum *tool = streamctx_ensure_openai_tool(sctx, idx);
        char *delta = json_get_string(root, "delta");
        if (delta) { sb_append(&tool->arguments, delta); FREE_PTR(delta); }
    } else if (strcmp(event, "response.completed") == 0) {
        JsonVal response = json_get(root, "response");
        if (response.type == JSON_NULL) response = root;
        responses_record_usage(sctx, response);
        int has_tools = sctx->openai_tool_count > 0;
        streamctx_emit_openai_tool_calls(sctx);
        responses_emit_usage(sctx);
        emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, has_tools ? "tool_use" : "end_turn");
        sctx->responses_terminal = 1;
    } else if (strcmp(event, "response.failed") == 0 || strcmp(event, "response.incomplete") == 0 || strcmp(event, "error") == 0) {
        JsonVal response = json_get(root, "response");
        if (response.type == JSON_NULL) response = root;
        responses_record_usage(sctx, response);
        JsonVal error = json_get(response, "error");
        char *message = json_get_string(error, "message");
        if (!message) message = json_get_string(response, "message");
        if (!message) message = json_get_string(response, "reason");
        if (!message) message = util_strdup(strcmp(event, "response.incomplete") == 0 ? "Response incomplete" : (strcmp(event, "error") == 0 ? "Stream error" : "Response failed"));
        emit_simple_event(sctx->callback, sctx->ctx, SSE_ERROR, message);
        FREE_PTR(message);
        responses_emit_usage(sctx);
        emit_simple_event(sctx->callback, sctx->ctx, SSE_STOP, "error");
        sctx->responses_terminal = 1;
    }
}

/* Feed one decoded chunk of body; splits SSE events internally.
 * Called by the http pump; returns 0 when cancelled. */
int sse_stream_feed(StreamCtx *sctx, const char *ptr, size_t total) {
    if (sctx->cancelled && *(sctx->cancelled)) return 0;

    for (size_t i = 0; i < total; i++) {
        if (sctx->cancelled && *(sctx->cancelled)) return 0;
        if (ptr[i] == '\n') {
            char *line = sctx->line_buf.data;
            size_t llen;
            if (!line) {   /* empty line before any data (SSE allows it) */
                continue;
            }
            llen = strlen(line);
            if (llen > 0 && line[llen-1] == '\r') line[--llen] = '\0';

            if (strncmp(line, "event: ", 7) == 0 && strcmp(sctx->provider, "responses") == 0) {
                FREE_PTR(sctx->event);
                sctx->event = util_strdup(line + 7);
            } else if (strncmp(line, "data: ", 6) == 0) {
                const char *data = line + 6;
                if (strcmp(sctx->provider, "openai") == 0) parse_openai_sse_event(sctx, data, strlen(data));
                else if (strcmp(sctx->provider, "responses") == 0) parse_responses_sse_event(sctx, sctx->event ? sctx->event : "", data, strlen(data));
                else sse_parse_event(sctx->provider, data, strlen(data), sctx->callback, sctx->ctx);
            } else if (strncmp(line, "data:", 5) == 0) {
                const char *data = line + 5;
                while (*data == ' ') data++;
                if (strcmp(sctx->provider, "openai") == 0) parse_openai_sse_event(sctx, data, strlen(data));
                else if (strcmp(sctx->provider, "responses") == 0) parse_responses_sse_event(sctx, sctx->event ? sctx->event : "", data, strlen(data));
                else sse_parse_event(sctx->provider, data, strlen(data), sctx->callback, sctx->ctx);
            }
            sb_truncate(&sctx->line_buf, 0);
        } else {
            sb_append_char(&sctx->line_buf, ptr[i]);
        }
    }
    return 1;
}

/* init/free StreamCtx (extracted from the old inline init) */
void sse_stream_init(StreamCtx *sctx, const char *provider,
                     sse_callback_fn callback, void *ctx,
                     volatile int *cancelled) {
    memset(sctx, 0, sizeof(*sctx));
    sctx->callback = callback;
    sctx->ctx = ctx;
    sb_init(&sctx->line_buf);
    sctx->cancelled = cancelled;
    sctx->provider = (char *)provider;
}


/* After the stream: handle leftover non-SSE JSON, check responses termination.
 * Mirrors the success tail of the old curl-based http_post_sse. */
void sse_stream_finish(StreamCtx *sctx, const char *provider,
                       sse_callback_fn callback, void *ctx) {
    process_residual_json(sctx, provider, callback, ctx);
    if (strcmp(provider, "responses") == 0 && !sctx->responses_terminal) {
        emit_simple_event(callback, ctx, SSE_ERROR, "Stream interrupted (no response.completed received)");
        emit_simple_event(callback, ctx, SSE_STOP, "error");
    }
}

void sse_stream_free(StreamCtx *sctx) {
    sb_free(&sctx->line_buf);
    FREE_PTR(sctx->event);
    streamctx_free_openai_tools(sctx);
}

/* ============================================================
 * HTTP request
 * ============================================================ */

/* forward declaration - defined before sse_parse_event */
static void emit_simple_event(sse_callback_fn callback, void *ctx,
                              SseEventType type, const char *content);

static int openai_cached_tokens(JsonVal usage) {
    int cached = json_get_int(usage, "cached_tokens");
    if (cached > 0) return cached;
    JsonVal details = json_get(usage, "prompt_tokens_details");
    if (details.type != JSON_NULL) cached = json_get_int(details, "cached_tokens");
    return cached;
}

static void fill_openai_usage_event(SseEvent *evt, JsonVal usage) {
    int prompt = json_get_int(usage, "prompt_tokens");
    int cached = openai_cached_tokens(usage);
    evt->out_tokens = json_get_int(usage, "completion_tokens");
    evt->cache_read_tokens = cached;
    if (prompt > 0) {
        evt->in_tokens = prompt - cached;
        if (evt->in_tokens < 0) evt->in_tokens = 0;
    }
}

/* handle non-SSE responses: parse leftover JSON in line_buf as a full reply */
static void process_residual_json(StreamCtx *sctx, const char *provider,
                                  sse_callback_fn callback, void *ctx) {
    if (!sctx->line_buf.data || sctx->line_buf.len == 0) return;
    char *residual = sctx->line_buf.data;
    while (*residual == ' ' || *residual == '\t' || *residual == '\r' || *residual == '\n') residual++;
    if (*residual != '{') return;

    size_t pos = 0;
    JsonParse jp = json_parse(residual, &pos);
    if (jp.error) return;

    char *err_msg = json_get_string(jp.val, "error");
    if (err_msg) {
        emit_simple_event(callback, ctx, SSE_ERROR, err_msg);
        free(err_msg);
    } else if (strcmp(provider, "claude") == 0) {
        JsonVal content = json_get(jp.val, "content");
        if (content.type == JSON_ARRAY) {
            int clen = json_array_len(content);
            for (int i = 0; i < clen; i++) {
                JsonVal block = json_array_get(content, i);
                char *btype = json_get_string(block, "type");
                if (btype && strcmp(btype, "text") == 0) {
                    char *txt = json_get_string(block, "text");
                    if (txt) { emit_simple_event(callback, ctx, SSE_TEXT, txt); free(txt); }
                } else if (btype && strcmp(btype, "thinking") == 0) {
                    char *txt = json_get_string(block, "thinking");
                    if (txt) { emit_simple_event(callback, ctx, SSE_THINKING, txt); free(txt); }
                } else if (btype && strcmp(btype, "tool_use") == 0) {
                    char *id = json_get_string(block, "id");
                    char *name = json_get_string(block, "name");
                    SseEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = SSE_TOOL_CALL;
                    evt.tool_id = id;
                    evt.tool_name = name;
                    evt.tool_input = "{}";
                    callback(ctx, &evt);
                    free(id); free(name);
                }
                free(btype);
            }
        }
        char *stop_reason = json_get_string(jp.val, "stop_reason");
        if (stop_reason) {
            emit_simple_event(callback, ctx, SSE_STOP, stop_reason);
            free(stop_reason);
        }
        JsonVal usage = json_get(jp.val, "usage");
        if (usage.type != JSON_NULL) {
            SseEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = SSE_USAGE;
            evt.in_tokens = json_get_int(usage, "input_tokens");
            evt.out_tokens = json_get_int(usage, "output_tokens");
            evt.cache_read_tokens = json_get_int(usage, "cache_read_input_tokens");
            evt.cache_creation_tokens = json_get_int(usage, "cache_creation_input_tokens");
            callback(ctx, &evt);
        }
    } else {
        JsonVal choices = json_get(jp.val, "choices");
        if (choices.type == JSON_ARRAY) {
            JsonVal choice = json_array_get(choices, 0);
            JsonVal msg = json_get(choice, "message");
            char *content = json_get_string(msg, "content");
            if (content) {
                emit_simple_event(callback, ctx, SSE_TEXT, content);
                free(content);
            }
            char *reasoning = json_get_string(msg, "reasoning_content");
            if (!reasoning) reasoning = json_get_string(msg, "reasoning");
            if (reasoning) {
                emit_simple_event(callback, ctx, SSE_THINKING, reasoning);
                free(reasoning);
            }
            JsonVal tool_calls = json_get(msg, "tool_calls");
            if (tool_calls.type == JSON_ARRAY) {
                int tc_len = json_array_len(tool_calls);
                for (int i = 0; i < tc_len; i++) {
                    JsonVal tc = json_array_get(tool_calls, i);
                    JsonVal fn = json_get(tc, "function");
                    char *id = json_get_string(tc, "id");
                    char *name = json_get_string(fn, "name");
                    char *arguments = json_get_string(fn, "arguments");
                    SseEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = SSE_TOOL_CALL;
                    evt.tool_id = id;
                    evt.tool_name = name;
                    evt.tool_input = arguments ? arguments : (char *)"{}";
                    callback(ctx, &evt);
                    free(id);
                    free(name);
                    free(arguments);
                }
            }
            char *finish = json_get_string(choice, "finish_reason");
            if (finish) {
                if (strcmp(finish, "tool_calls") == 0) emit_simple_event(callback, ctx, SSE_STOP, "tool_use");
                else if (strcmp(finish, "stop") == 0) emit_simple_event(callback, ctx, SSE_STOP, "end_turn");
                else emit_simple_event(callback, ctx, SSE_STOP, finish);
                free(finish);
            }
        }
        JsonVal usage = json_get(jp.val, "usage");
        if (usage.type != JSON_NULL) {
            SseEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = SSE_USAGE;
            fill_openai_usage_event(&evt, usage);
            callback(ctx, &evt);
        }
    }
}


/* ============================================================
 * SSE event parsing
 * ============================================================ */

static void emit_simple_event(sse_callback_fn callback, void *ctx,
                              SseEventType type, const char *content) {
    SseEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.content = (char *)content;  /* borrowed, not freed */
    callback(ctx, &evt);
}

/* helpers that copy strings inside the callback */
/* kept for future use */
#if 0
static char *dup_and_free(char *s) {
    return util_strdup(s);
}
#endif

int sse_parse_event(const char *provider, const char *data, size_t data_len,
                    sse_callback_fn callback, void *ctx) {
    if (data_len == 0) return 0;
    if (strcmp(data, "[DONE]") == 0) {
        if (strcmp(provider, "claude") == 0) emit_simple_event(callback, ctx, SSE_STOP, "end_turn");
        return 0;
    }

    /* parse JSON */
    size_t pos = 0;
    JsonParse jp = json_parse(data, &pos);
    if (jp.error) return 0;

    if (strcmp(provider, "claude") == 0) {
        /* Claude SSE format */
        char *type = json_get_string(jp.val, "type");
        if (!type) return 0;

        if (strcmp(type, "content_block_delta") == 0) {
            JsonVal delta = json_get(jp.val, "delta");
            char *dtype = json_get_string(delta, "type");
            if (dtype && strcmp(dtype, "text_delta") == 0) {
                char *text = json_get_string(delta, "text");
                if (text) { emit_simple_event(callback, ctx, SSE_TEXT, text); free(text); }
            } else if (dtype && strcmp(dtype, "thinking_delta") == 0) {
                char *text = json_get_string(delta, "thinking");
                if (text) { emit_simple_event(callback, ctx, SSE_THINKING, text); free(text); }
            } else if (dtype && strcmp(dtype, "input_json_delta") == 0) {
                /* tool-call input delta */
                char *partial = json_get_string(delta, "partial_json");
                if (partial) {
                    SseEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = SSE_TOOL_INPUT_DELTA;
                    evt.content = partial;
                    evt.tool_id = NULL; /* index is used for matching */
                    callback(ctx, &evt);
                    free(partial);
                }
            }
            free(dtype);
        } else if (strcmp(type, "content_block_start") == 0) {
            JsonVal cb = json_get(jp.val, "content_block");
            char *cb_type = json_get_string(cb, "type");
            if (cb_type && strcmp(cb_type, "tool_use") == 0) {
                char *id = json_get_string(cb, "id");
                char *name = json_get_string(cb, "name");
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_TOOL_CALL_START;
                evt.tool_id = id;
                evt.tool_name = name;
                callback(ctx, &evt);
                /* copied in the callback; free here */
                free(id);
                free(name);
            }
            free(cb_type);
        } else if (strcmp(type, "content_block_stop") == 0) {
            /* tool call complete - the accumulator handles it after stop */
        } else if (strcmp(type, "message_delta") == 0) {
            JsonVal delta = json_get(jp.val, "delta");
            char *stop_reason = json_get_string(delta, "stop_reason");
            if (stop_reason) {
                emit_simple_event(callback, ctx, SSE_STOP, stop_reason);
                free(stop_reason);
            }
            JsonVal usage = json_get(jp.val, "usage");
            if (usage.type != JSON_NULL) {
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_USAGE;
                evt.out_tokens = json_get_int(usage, "output_tokens");
                /* input/cache_* only when message_start did not provide them (Rust parity);
                 * the OpenAI path has no message_start; transport synthesizes it */
                int it = json_get_int(usage, "input_tokens");
                int cr = json_get_int(usage, "cache_read_input_tokens");
                int cc = json_get_int(usage, "cache_creation_input_tokens");
                if (it > 0) evt.in_tokens = it;
                if (cr > 0) evt.cache_read_tokens = cr;
                if (cc > 0) evt.cache_creation_tokens = cc;
                callback(ctx, &evt);
            }
        } else if (strcmp(type, "message_start") == 0) {
            JsonVal msg = json_get(jp.val, "message");
            JsonVal usage = json_get(msg, "usage");
            if (usage.type != JSON_NULL) {
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_USAGE;
                evt.in_tokens = json_get_int(usage, "input_tokens");
                evt.cache_read_tokens = json_get_int(usage, "cache_read_input_tokens");
                evt.cache_creation_tokens = json_get_int(usage, "cache_creation_input_tokens");
                callback(ctx, &evt);
            }
        } else if (strcmp(type, "error") == 0) {
            /* Claude errors look like {"type":"error","error":{...}} */
            char *msg = NULL;
            JsonVal err_obj = json_get(jp.val, "error");
            if (err_obj.type == JSON_OBJECT)
                msg = json_get_string(err_obj, "message");
            if (!msg) msg = json_get_string(jp.val, "error");
            if (!msg) msg = json_get_string(jp.val, "message");
            emit_simple_event(callback, ctx, SSE_ERROR, msg ? msg : "unknown error");
            free(msg);
        }
        free(type);
    } else {
        /* OpenAI SSE format */
        char *obj_type = json_get_string(jp.val, "object");
        if (!obj_type) return 0;

        if (strcmp(obj_type, "chat.completion.chunk") == 0) {
            JsonVal choices = json_get(jp.val, "choices");
            if (choices.type == JSON_ARRAY) {
                JsonVal choice = json_array_get(choices, 0);
                JsonVal delta = json_get(choice, "delta");
                char *content = json_get_string(delta, "content");
                if (content) {
                    emit_simple_event(callback, ctx, SSE_TEXT, content);
                    free(content);
                }
                char *reasoning = json_get_string(delta, "reasoning_content");
                if (!reasoning) reasoning = json_get_string(delta, "reasoning");
                if (reasoning) {
                    emit_simple_event(callback, ctx, SSE_THINKING, reasoning);
                    free(reasoning);
                }
                JsonVal tool_calls = json_get(delta, "tool_calls");
                if (tool_calls.type == JSON_ARRAY) {
                            int tc_len = json_array_len(tool_calls);
                            for (int i = 0; i < tc_len; i++) {
                                JsonVal tc = json_array_get(tool_calls, i);
                                JsonVal fn = json_get(tc, "function");
                                char *id = json_get_string(tc, "id");
                                char *name = json_get_string(fn, "name");
                        char *arguments = json_get_string(fn, "arguments");
                        if (id || name) {
                            SseEvent evt;
                            memset(&evt, 0, sizeof(evt));
                            evt.type = SSE_TOOL_CALL_START;
                            evt.tool_id = id;
                            evt.tool_name = name;
                            callback(ctx, &evt);
                        }
                        if (arguments) {
                            SseEvent evt;
                            memset(&evt, 0, sizeof(evt));
                            evt.type = SSE_TOOL_INPUT_DELTA;
                            evt.content = arguments;
                            callback(ctx, &evt);
                        }
                        free(id);
                        free(name);
                        free(arguments);
                    }
                }
                char *finish = json_get_string(choice, "finish_reason");
                if (finish) {
                    if (strcmp(finish, "tool_calls") == 0) {
                        emit_simple_event(callback, ctx, SSE_STOP, "tool_use");
                    } else if (strcmp(finish, "stop") == 0) {
                        emit_simple_event(callback, ctx, SSE_STOP, "end_turn");
                    } else if (strcmp(finish, "length") == 0) {
                        emit_simple_event(callback, ctx, SSE_STOP, "max_tokens");
                    } else {
                        emit_simple_event(callback, ctx, SSE_STOP, finish);
                    }
                    free(finish);
                }
            }
            JsonVal usage = json_get(jp.val, "usage");
            if (usage.type != JSON_NULL) {
                SseEvent evt;
                memset(&evt, 0, sizeof(evt));
                evt.type = SSE_USAGE;
                fill_openai_usage_event(&evt, usage);
                callback(ctx, &evt);
            }
        }
        free(obj_type);
    }
    return 0;
}

/* ============================================================
 * SSE accumulator
 * ============================================================ */

void sse_accum_init(SseAccumulator *acc) {
    memset(acc, 0, sizeof(*acc));
    sb_init(&acc->text);
    sb_init(&acc->thinking);
    acc->tool_cap = 8;
    acc->tools = calloc(acc->tool_cap, sizeof(ToolCallAccum));
    acc->tool_count = 0;
    acc->current_block_index = -1;
}

void sse_accum_free(SseAccumulator *acc) {
    sb_free(&acc->text);
    sb_free(&acc->thinking);
    for (int i = 0; i < acc->tool_count; i++) {
        FREE_PTR(acc->tools[i].id);
        FREE_PTR(acc->tools[i].name);
        sb_free(&acc->tools[i].input_json);
    }
    free(acc->tools);
    FREE_PTR(acc->current_block_type);
    FREE_PTR(acc->current_tool_id);
    FREE_PTR(acc->current_tool_name);
    FREE_PTR(acc->stop_reason);
    FREE_PTR(acc->error);
}

void sse_accum_callback(void *ctx, const SseEvent *evt) {
    SseAccumulator *acc = (SseAccumulator *)ctx;

    switch (evt->type) {
    case SSE_TEXT:
        sb_append(&acc->text, evt->content);
        break;

    case SSE_THINKING:
        sb_append(&acc->thinking, evt->content);
        break;

    case SSE_TOOL_CALL_START: {
        if (acc->tool_count >= acc->tool_cap) {
            acc->tool_cap *= 2;
            acc->tools = realloc(acc->tools, acc->tool_cap * sizeof(ToolCallAccum));
        }
        ToolCallAccum *tc = &acc->tools[acc->tool_count];
        memset(tc, 0, sizeof(*tc));
        sb_init(&tc->input_json);
        tc->id = util_strdup(evt->tool_id);
        tc->name = util_strdup(evt->tool_name);
        acc->tool_count++;
        break;
    }

    case SSE_TOOL_INPUT_DELTA: {
        if (acc->tool_count > 0 && evt->content) {
            sb_append(&acc->tools[acc->tool_count - 1].input_json, evt->content);
        }
        break;
    }

    case SSE_TOOL_CALL: {
        /* complete tool call (non-streaming, e.g. OpenAI) */
        if (acc->tool_count >= acc->tool_cap) {
            acc->tool_cap *= 2;
            acc->tools = realloc(acc->tools, acc->tool_cap * sizeof(ToolCallAccum));
        }
        ToolCallAccum *tc = &acc->tools[acc->tool_count];
        memset(tc, 0, sizeof(*tc));
        tc->id = util_strdup(evt->tool_id ? evt->tool_id : "");
        tc->name = util_strdup(evt->tool_name ? evt->tool_name : "");
        sb_init(&tc->input_json);
        sb_append(&tc->input_json, evt->tool_input ? evt->tool_input : "{}");
        acc->tool_count++;
        break;
    }

    case SSE_USAGE:
        if (evt->in_tokens > 0) acc->in_tokens = evt->in_tokens;
        if (evt->out_tokens > 0) acc->out_tokens = evt->out_tokens;
        if (evt->cache_read_tokens > 0) acc->cache_read_tokens = evt->cache_read_tokens;
        if (evt->cache_creation_tokens > 0) acc->cache_creation_tokens = evt->cache_creation_tokens;
        break;

    case SSE_STOP:
        acc->stopped = 1;
        if (evt->content) {
            FREE_PTR(acc->stop_reason);
            acc->stop_reason = util_strdup(evt->content);
        }
        break;

    case SSE_ERROR:
        FREE_PTR(acc->error);
        acc->error = util_strdup(evt->content ? evt->content : "unknown error");
        break;

    case SSE_RETRY:
        /* reset current accumulation (stream_display_callback parity) */
        sb_truncate(&acc->text, 0);
        sb_truncate(&acc->thinking, 0);
        for (int i = 0; i < acc->tool_count; i++) {
            FREE_PTR(acc->tools[i].id);
            FREE_PTR(acc->tools[i].name);
            sb_free(&acc->tools[i].input_json);
        }
        acc->tool_count = 0;
        acc->stopped = 0;
        FREE_PTR(acc->stop_reason);
        acc->in_tokens = 0;
        acc->out_tokens = 0;
        acc->cache_read_tokens = 0;
        acc->cache_creation_tokens = 0;
        break;
    }
}

/* ============================================================
 * request body building
 * ============================================================ */

char *build_claude_request(const char *model, const char *system_prompt,
                           const char *tools_json,
                           char **conv_lines, int conv_line_count,
                           int max_tokens, const char *thinking, const char *effort) {
    StrBuf buf;
    sb_init(&buf);

    /* field order matches the Go/Rust map alphabetical order:
     * max_tokens → messages → model → output_config → stream → system → thinking → tools */
    sb_append(&buf, "{\"max_tokens\":");
    sb_appendf(&buf, "%d", max_tokens);

    /* messages */
    sb_append(&buf, ",\"messages\":[");
    for (int i = 0; i < conv_line_count; i++) {
        if (i > 0) sb_append(&buf, ",");
        sb_append(&buf, conv_lines[i]);
    }
    sb_append(&buf, "]");

    /* model */
    sb_append(&buf, ",\"model\":");
    sb_append_json_string(&buf, model);

    /* output_config (only when thinking != disabled) */
    if (thinking && strcmp(thinking, "disabled") != 0) {
        sb_append(&buf, ",\"output_config\":{\"effort\":");
        sb_append_json_string(&buf, effort ? effort : "high");
        sb_append(&buf, "}");
    }

    /* stream */
    sb_append(&buf, ",\"stream\":true");

    /* system prompt */
    if (system_prompt && system_prompt[0]) {
        sb_append(&buf, ",\"system\":");
        sb_append_json_string(&buf, system_prompt);
    }

    /* thinking (only when thinking != disabled) */
    if (thinking && strcmp(thinking, "disabled") != 0) {
        sb_append(&buf, ",\"thinking\":{\"type\":");
        sb_append_json_string(&buf, thinking);
        sb_append(&buf, "}");
    }

    /* tools */
    if (tools_json) {
        sb_append(&buf, ",\"tools\":");
        sb_append(&buf, tools_json);
    }

    sb_append(&buf, "}");

    char *result = buf.data;
    /* no sb_free: we return buf.data */
    return result;
}

static void sb_append_json_val(StrBuf *sb, JsonVal v) {
    if (v.type == JSON_NULL || !v.src) {
        sb_append(sb, "null");
        return;
    }
    sb_appendn(sb, v.src + v.start, v.end - v.start);
}

static void openai_convert_tools(StrBuf *out, JsonVal tools_val) {
    if (tools_val.type != JSON_ARRAY) {
        sb_append(out, "[]");
        return;
    }
    sb_append_char(out, '[');
    int n = json_array_len(tools_val);
    for (int i = 0; i < n; i++) {
        JsonVal td = json_array_get(tools_val, i);
        if (i > 0) sb_append_char(out, ',');
        char *type = json_get_string(td, "type");
        if (type && strcmp(type, "function") == 0) {
            sb_append_json_val(out, td);
            FREE_PTR(type);
            continue;
        }
        FREE_PTR(type);
        char *name = json_get_string(td, "name");
        char *desc = json_get_string(td, "description");
        JsonVal params = json_get(td, "input_schema");
        if (params.type == JSON_NULL) params = json_get(td, "parameters");
        sb_append(out, "{\"type\":\"function\",\"function\":{\"name\":");
        sb_append_json_string(out, name ? name : "");
        sb_append(out, ",\"description\":");
        sb_append_json_string(out, desc ? desc : "");
        sb_append(out, ",\"parameters\":");
        if (params.type == JSON_NULL) sb_append(out, "{}");
        else sb_append_json_val(out, params);
        sb_append(out, "}}");
        FREE_PTR(name);
        FREE_PTR(desc);
    }
    sb_append_char(out, ']');
}

static void openai_convert_assistant_message(StrBuf *out, JsonVal content_val) {
    StrBuf text, reasoning, tool_calls;
    sb_init(&text);
    sb_init(&reasoning);
    sb_init(&tool_calls);

    int n = json_array_len(content_val);
    for (int i = 0; i < n; i++) {
        JsonVal block = json_array_get(content_val, i);
        char *btype = json_get_string(block, "type");
        if (!btype) continue;
        if (strcmp(btype, "thinking") == 0) {
            char *t = json_get_string(block, "thinking");
            if (t) { sb_append(&reasoning, t); FREE_PTR(t); }
        } else if (strcmp(btype, "text") == 0) {
            char *t = json_get_string(block, "text");
            if (t) { sb_append(&text, t); FREE_PTR(t); }
        } else if (strcmp(btype, "tool_use") == 0) {
            char *id = json_get_string(block, "id");
            char *name = json_get_string(block, "name");
            JsonVal input = json_get(block, "input");
            if (tool_calls.len > 0) sb_append_char(&tool_calls, ',');
            sb_append(&tool_calls, "{\"id\":");
            sb_append_json_string(&tool_calls, id ? id : "");
            sb_append(&tool_calls, ",\"type\":\"function\",\"function\":{\"name\":");
            sb_append_json_string(&tool_calls, name ? name : "");
            sb_append(&tool_calls, ",\"arguments\":");
            if (input.type == JSON_NULL) sb_append_json_string(&tool_calls, "{}");
            else {
                StrBuf arg;
                sb_init(&arg);
                sb_append_json_val(&arg, input);
                sb_append_json_string(&tool_calls, arg.data ? arg.data : "{}");
                sb_free(&arg);
            }
            sb_append(&tool_calls, "}}");
            FREE_PTR(id);
            FREE_PTR(name);
        }
        FREE_PTR(btype);
    }

    sb_append(out, "{\"role\":\"assistant\",\"reasoning_content\":");
    sb_append_json_string(out, reasoning.data ? reasoning.data : "");
    sb_append(out, ",\"content\":");
    sb_append_json_string(out, text.data ? text.data : "");
    if (tool_calls.len > 0) {
        sb_append(out, ",\"tool_calls\":[");
        sb_append(out, tool_calls.data);
        sb_append_char(out, ']');
    }
    sb_append_char(out, '}');

    sb_free(&text);
    sb_free(&reasoning);
    sb_free(&tool_calls);
}

static int openai_convert_tool_results(StrBuf *out, JsonVal content_val) {
    int written = 0;
    int n = json_array_len(content_val);
    for (int i = 0; i < n; i++) {
        JsonVal block = json_array_get(content_val, i);
        char *btype = json_get_string(block, "type");
        if (!btype || strcmp(btype, "tool_result") != 0) {
            FREE_PTR(btype);
            continue;
        }
        char *tool_use_id = json_get_string(block, "tool_use_id");
        char *content = json_get_string(block, "content");
        if (written > 0) sb_append_char(out, ',');
        sb_append(out, "{\"role\":\"tool\",\"tool_call_id\":");
        sb_append_json_string(out, tool_use_id ? tool_use_id : "");
        sb_append(out, ",\"content\":");
        sb_append_json_string(out, content ? content : "");
        sb_append_char(out, '}');
        written++;
        FREE_PTR(tool_use_id);
        FREE_PTR(content);
        FREE_PTR(btype);
    }
    return written;
}

static void openai_convert_messages(StrBuf *out, JsonVal messages_val) {
    sb_append_char(out, '[');
    int wrote = 0;
    int n = json_array_len(messages_val);
    for (int i = 0; i < n; i++) {
        JsonVal msg = json_array_get(messages_val, i);
        char *role = json_get_string(msg, "role");
        JsonVal content = json_get(msg, "content");
        if (role && strcmp(role, "assistant") == 0 && content.type == JSON_ARRAY) {
            if (wrote > 0) sb_append_char(out, ',');
            openai_convert_assistant_message(out, content);
            wrote++;
        } else if (role && strcmp(role, "user") == 0 && content.type == JSON_ARRAY) {
            int before = wrote;
            if (wrote > 0 && json_array_len(content) > 0) {
                /* openai_convert_tool_results handles commas after the first item */
            }
            if (wrote > 0) {
                StrBuf tmp;
                sb_init(&tmp);
                int tool_written = openai_convert_tool_results(&tmp, content);
                if (tool_written > 0) {
                    sb_append_char(out, ',');
                    sb_append(out, tmp.data);
                    wrote += tool_written;
                } else {
                    if (wrote > 0) sb_append_char(out, ',');
                    sb_append_json_val(out, msg);
                    wrote++;
                }
                sb_free(&tmp);
            } else {
                int tool_written = openai_convert_tool_results(out, content);
                if (tool_written > 0) wrote += tool_written;
                else {
                    sb_append_json_val(out, msg);
                    wrote++;
                }
            }
            (void)before;
        } else {
            if (wrote > 0) sb_append_char(out, ',');
            sb_append_json_val(out, msg);
            wrote++;
        }
        FREE_PTR(role);
    }
    sb_append_char(out, ']');
}

char *convert_to_openai(const char *claude_body) {
    JsonParse jp = json_parse_root(claude_body);
    if (jp.error) return util_strdup(claude_body);

    char *model = json_get_string(jp.val, "model");
    int max_tokens = json_get_int(jp.val, "max_tokens");
    JsonVal system_val = json_get(jp.val, "system");
    JsonVal thinking_val = json_get(jp.val, "thinking");
    JsonVal output_config_val = json_get(jp.val, "output_config");
    JsonVal messages_val = json_get(jp.val, "messages");
    JsonVal tools_val = json_get(jp.val, "tools");

    StrBuf messages, tools, result;
    sb_init(&messages);
    sb_init(&tools);
    sb_init(&result);

    openai_convert_messages(&messages, messages_val);
    if (tools_val.type == JSON_ARRAY && json_array_len(tools_val) > 0) {
        openai_convert_tools(&tools, tools_val);
    }

    sb_append(&result, "{\"model\":");
    sb_append_json_string(&result, model ? model : "");
    sb_append(&result, ",\"max_tokens\":");
    sb_appendf(&result, "%d", max_tokens);
    sb_append(&result, ",\"stream\":true,\"stream_options\":{\"include_usage\":true}");

    if (system_val.type != JSON_NULL) {
        char *sys = json_as_string(system_val);
        if (sys && sys[0]) {
            StrBuf with_system;
            sb_init(&with_system);
            sb_append(&with_system, "[{\"role\":\"system\",\"content\":");
            sb_append_json_string(&with_system, sys);
            sb_append_char(&with_system, '}');
            if (messages.len > 2) {
                sb_append_char(&with_system, ',');
                sb_appendn(&with_system, messages.data + 1, messages.len - 2);
            }
            sb_append_char(&with_system, ']');
            sb_free(&messages);
            messages = with_system;
        }
        FREE_PTR(sys);
    }

    char *thinking_type = json_get_string(thinking_val, "type");
    if (thinking_type &&
        (strcmp(thinking_type, "adaptive") == 0 || strcmp(thinking_type, "enabled") == 0)) {
        sb_append(&result, ",\"thinking\":{\"type\":\"enabled\"}");
        char *effort = json_get_string(output_config_val, "effort");
        sb_append(&result, ",\"reasoning_effort\":");
        sb_append_json_string(&result, (effort && effort[0]) ? effort : "high");
        FREE_PTR(effort);
    }
    FREE_PTR(thinking_type);

    if (tools.len > 0 && strcmp(tools.data, "[]") != 0) {
        sb_append(&result, ",\"tools\":");
        sb_append(&result, tools.data);
    }

    sb_append(&result, ",\"messages\":");
    sb_append(&result, messages.data ? messages.data : "[]");
    sb_append_char(&result, '}');

    FREE_PTR(model);
    sb_free(&messages);
    sb_free(&tools);
    return result.data;
}


static void responses_convert_tools(StrBuf *out, JsonVal tools_val) {
    sb_append_char(out, '[');
    int wrote = 0;
    int n = json_array_len(tools_val);
    for (int i = 0; i < n; i++) {
        JsonVal tool = json_array_get(tools_val, i);
        char *name = json_get_string(tool, "name");
        if (!name || !name[0]) { FREE_PTR(name); continue; }
        char *desc = json_get_string(tool, "description");
        JsonVal parameters = json_get(tool, "input_schema");
        if (parameters.type == JSON_NULL) parameters = json_get(tool, "parameters");
        if (wrote++) sb_append_char(out, ',');
        sb_append(out, "{\"type\":\"function\",\"name\":");
        sb_append_json_string(out, name);
        sb_append(out, ",\"description\":");
        sb_append_json_string(out, desc ? desc : "");
        sb_append(out, ",\"parameters\":");
        if (parameters.type == JSON_NULL) sb_append(out, "{}"); else sb_append_json_val(out, parameters);
        sb_append_char(out, '}');
        FREE_PTR(name); FREE_PTR(desc);
    }
    sb_append_char(out, ']');
}

static void responses_convert_messages(StrBuf *out, JsonVal messages_val) {
    sb_append_char(out, '[');
    int wrote = 0;
    int n = json_array_len(messages_val);
    for (int i = 0; i < n; i++) {
        JsonVal msg = json_array_get(messages_val, i);
        char *role = json_get_string(msg, "role");
        JsonVal content = json_get(msg, "content");
        if (role && strcmp(role, "assistant") == 0 && content.type == JSON_ARRAY) {
            StrBuf text; sb_init(&text);
            int blocks = json_array_len(content);
            for (int j = 0; j < blocks; j++) {
                JsonVal block = json_array_get(content, j);
                char *type = json_get_string(block, "type");
                if (type && strcmp(type, "text") == 0) {
                    char *value = json_get_string(block, "text");
                    if (value) { sb_append(&text, value); FREE_PTR(value); }
                } else if (type && strcmp(type, "tool_use") == 0) {
                    char *id = json_get_string(block, "id");
                    char *name = json_get_string(block, "name");
                    JsonVal input = json_get(block, "input");
                    if (wrote++) sb_append_char(out, ',');
                    sb_append(out, "{\"type\":\"function_call\",\"call_id\":"); sb_append_json_string(out, id ? id : "");
                    sb_append(out, ",\"name\":"); sb_append_json_string(out, name ? name : "");
                    sb_append(out, ",\"arguments\":");
                    StrBuf args; sb_init(&args); if (input.type == JSON_NULL) sb_append(&args, "{}"); else sb_append_json_val(&args, input);
                    sb_append_json_string(out, args.data ? args.data : "{}"); sb_free(&args); sb_append_char(out, '}');
                    FREE_PTR(id); FREE_PTR(name);
                }
                FREE_PTR(type);
            }
            if (text.len > 0) { if (wrote++) sb_append_char(out, ','); sb_append(out, "{\"role\":\"assistant\",\"content\":"); sb_append_json_string(out, text.data); sb_append_char(out, '}'); }
            sb_free(&text);
        } else if (role && strcmp(role, "user") == 0 && content.type == JSON_ARRAY) {
            int blocks = json_array_len(content);
            for (int j = 0; j < blocks; j++) {
                JsonVal block = json_array_get(content, j);
                char *type = json_get_string(block, "type");
                if (type && strcmp(type, "tool_result") == 0) {
                    char *id = json_get_string(block, "tool_use_id"); char *value = json_get_string(block, "content");
                    if (wrote++) sb_append_char(out, ','); sb_append(out, "{\"type\":\"function_call_output\",\"call_id\":"); sb_append_json_string(out, id ? id : ""); sb_append(out, ",\"output\":"); sb_append_json_string(out, value ? value : ""); sb_append_char(out, '}');
                    FREE_PTR(id); FREE_PTR(value);
                } else if (type && strcmp(type, "text") == 0) {
                    char *value = json_get_string(block, "text"); if (wrote++) sb_append_char(out, ','); sb_append(out, "{\"role\":\"user\",\"content\":"); sb_append_json_string(out, value ? value : ""); sb_append_char(out, '}'); FREE_PTR(value);
                }
                FREE_PTR(type);
            }
        } else { if (wrote++) sb_append_char(out, ','); sb_append_json_val(out, msg); }
        FREE_PTR(role);
    }
    sb_append_char(out, ']');
}

char *convert_to_responses(const char *claude_body) {
    JsonParse jp = json_parse_root(claude_body);
    if (jp.error) return util_strdup(claude_body);
    char *model = json_get_string(jp.val, "model");
    int max_tokens = json_get_int(jp.val, "max_tokens");
    JsonVal system = json_get(jp.val, "system");
    JsonVal thinking = json_get(jp.val, "thinking");
    JsonVal output = json_get(jp.val, "output_config");
    JsonVal messages = json_get(jp.val, "messages");
    JsonVal tools_val = json_get(jp.val, "tools");
    StrBuf input, tools, result; sb_init(&input); sb_init(&tools); sb_init(&result);
    responses_convert_messages(&input, messages);
    if (tools_val.type == JSON_ARRAY && json_array_len(tools_val)) responses_convert_tools(&tools, tools_val);
    sb_append(&result, "{\"model\":"); sb_append_json_string(&result, model ? model : "");
    sb_append(&result, ",\"input\":"); sb_append(&result, input.data ? input.data : "[]");
    sb_appendf(&result, ",\"max_output_tokens\":%d,\"stream\":true", max_tokens);
    if (system.type != JSON_NULL) { char *value = json_as_string(system); if (value && value[0]) { sb_append(&result, ",\"instructions\":"); sb_append_json_string(&result, value); } FREE_PTR(value); }
    char *thinking_type = json_get_string(thinking, "type");
    if (thinking_type && (!strcmp(thinking_type, "adaptive") || !strcmp(thinking_type, "enabled"))) { char *effort = json_get_string(output, "effort"); sb_append(&result, ",\"reasoning\":{\"effort\":"); sb_append_json_string(&result, effort && effort[0] ? effort : "high"); sb_append_char(&result, '}'); FREE_PTR(effort); }
    FREE_PTR(thinking_type);
    if (tools.len && strcmp(tools.data, "[]")) { sb_append(&result, ",\"tools\":"); sb_append(&result, tools.data); }
    sb_append_char(&result, '}');
    FREE_PTR(model); sb_free(&input); sb_free(&tools);
    return result.data;
}

/* ==== ba_prompt.c ==== */
/*
 * ba_prompt.c - system prompt construction, ported verbatim from
 * bash-agent's agent_build_prompt suite (agent.c). Section order, XML
 * tags and section text are kept identical; adaptations are limited to:
 *   - agent identity string: bash-agent -> busyagent
 *   - skill dirs: .claude/skills dropped, replaced by generic agent dirs
 *     (cwd/skills, ~/.agents/skills, $BA_HOME/skills)
 *   - Bash background / SubAgent sync guidance lines match busyagent's
 *     actual single-turn semantics
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include <dirent.h>
#include <sys/utsname.h>

/* ============================================================
 * system prompt building - helpers (ported verbatim)
 * ============================================================ */

/* append an XML section: <tag>... or <tag name=...> */
static void prompt_append_attr_escaped(StrBuf *buf, const char *src) {
    if (!src) return;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
            case '"':  sb_append(buf, "\\\""); break;
            case '\\': sb_append(buf, "\\\\"); break;
            case '\b': sb_append(buf, "\\b"); break;
            case '\f': sb_append(buf, "\\f"); break;
            case '\n': sb_append(buf, "\\n"); break;
            case '\r': sb_append(buf, "\\r"); break;
            case '\t': sb_append(buf, "\\t"); break;
            default:
                if (c < 0x20) sb_appendf(buf, "\\u%04x", c);
                else sb_append_char(buf, c);
                break;
        }
    }
}

static void prompt_append_section(StrBuf *buf, const char *tag,
                                   const char *content, const char *name) {
    if (!content || !content[0]) return;
    size_t content_len = strlen(content);
    while (content_len > 0 &&
           (content[content_len - 1] == '\n' || content[content_len - 1] == '\r')) {
        content_len--;
    }
    if (content_len == 0) return;
    if (name && name[0]) {
        sb_appendf(buf, "<%s name=\"", tag);
        prompt_append_attr_escaped(buf, name);
        sb_append(buf, "\">\n");
        sb_appendn(buf, content, content_len);
        sb_appendf(buf, "\n</%s>\n", tag);
    } else {
        sb_appendf(buf, "<%s>\n", tag);
        sb_appendn(buf, content, content_len);
        sb_appendf(buf, "\n</%s>\n", tag);
    }
}

/* util_path_join equivalent (busybox has no such helper) */
static char *pj(const char *a, const char *b) {
    return xasprintf("%s/%s", a, b);
}

/* util_read_file equivalent */
static char *read_all(const char *path) {
    int fd = open(path, O_RDONLY);
    long sz;
    char *buf;
    if (fd < 0)
        return NULL;
    sz = xlseek(fd, 0, SEEK_END);
    xlseek(fd, 0, SEEK_SET);
    buf = xzalloc(sz + 1);
    sz = full_read(fd, buf, sz);
    if (sz < 0) sz = 0;
    buf[sz] = '\0';
    close(fd);
    return buf;
}

/* locale detection: LC_ALL -> LC_MESSAGES -> LANG -> "en_US", strip .xxx */
static const char *detect_locale(void) {
    static char buf[128];
    const char *loc = getenv("LC_ALL");
    if (!loc || !loc[0]) loc = getenv("LC_MESSAGES");
    if (!loc || !loc[0]) loc = getenv("LANG");
    if (!loc || !loc[0]) loc = "en_US";
    strncpy(buf, loc, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    {
        char *dot = strchr(buf, '.');
        if (dot) *dot = '\0';
    }
    return buf;
}

/* find an instructions file in dir (AGENTS.md / AGENT.md; generic agent dirs),
 * Returns malloc'd content or NULL. bash-agent also looks for CLAUDE.md
 * variants; this project does not bind Claude dirs. */
static char *find_instruction_file(const char *dir) {
    const char *candidates[] = { "AGENTS.md", "AGENT.md", NULL };
    int i;

    for (i = 0; candidates[i]; i++) {
        char *path = pj(dir, candidates[i]);
        char *content = read_all(path);
        free(path);
        if (content && content[0])
            return content;
        free(content);
    }
    return NULL;
}

/* extract a summary from SKILL.md: prefer description:, else first non-empty non-heading non--- line */
static void extract_skill_summary(const char *md, StrBuf *out) {
    const char *p = md;
    char line[1024];
    int found = 0;
    char fallback[1024] = "";

    while (*p) {
        /* read one line */
        int li = 0;
        while (*p && *p != '\n' && li < (int)sizeof(line) - 1) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        if (*p == '\n') p++;

        /* trim */
        {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            {
                char *e = s + strlen(s);
                while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
                *e = '\0';
                if (*s == '\0') continue;

                /* description: line */
                if (strncmp(s, "description:", 12) == 0) {
                    char *val = s + 12;
                    while (*val == ' ' || *val == '\t') val++;
                    /* strip quotes */
                    size_t vl = strlen(val);
                    if (vl >= 2 && ((val[0] == '"' && val[vl-1] == '"') ||
                                    (val[0] == '\'' && val[vl-1] == '\''))) {
                        val++;
                        vl -= 2;
                    }
                    sb_appendn(out, val, vl);
                    found = 1;
                    return;
                }
                /* fallback: not a heading, not ---, not ``` */
                if (!found && fallback[0] == '\0' && s[0] != '#' &&
                    !(s[0] == '-' && s[1] == '-' && s[2] == '-' && s[3] == '\0') &&
                    !(s[0] == '`' && s[1] == '`' && s[2] == '`')) {
                    strncpy(fallback, s, sizeof(fallback) - 1);
                    fallback[sizeof(fallback) - 1] = '\0';
                }
            }
        }
    }
    if (!found && fallback[0]) {
        sb_append(out, fallback);
    }
}

/* scan the skill dir list (dedup) and build the skill-index.
 * order: cwd/skills > $HOME/.agents/skills > bag_home/skills */
static void build_skill_index(StrBuf *index, const char *cwd,
                              const char *agents_home, const char *bag_home) {
    char *dirs[4];
    int dcount = 0;
    {
        dirs[dcount++] = xasprintf("%s/skills", cwd);
        if (agents_home && agents_home[0])
            dirs[dcount++] = xasprintf("%s/.agents/skills", agents_home);
        if (bag_home && bag_home[0])
            dirs[dcount++] = xasprintf("%s/skills", bag_home);
    }

    /* dedup via the seen list */
    char *seen[256];
    int seen_count = 0;

    for (int d = 0; d < dcount; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            /* already seen? */
            int dup = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen[s], ent->d_name) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            /* SKILL.md present? */
            char *skill_md = xasprintf("%s/%s/SKILL.md", dirs[d], ent->d_name);
            char *md_content = read_all(skill_md);
            free(skill_md);
            if (!md_content || !md_content[0]) { free(md_content); continue; }

            /* mark seen */
            if (seen_count < 256) seen[seen_count++] = util_strdup(ent->d_name);

            /* extract the summary */
            StrBuf summary;
            sb_init(&summary);
            extract_skill_summary(md_content, &summary);
            free(md_content);

            sb_appendf(index, "- %s", ent->d_name);
            if (summary.len > 0) sb_appendf(index, ": %s", summary.data);
            sb_append_char(index, '\n');
            sb_free(&summary);
        }
        closedir(dir);
    }
    for (int d = 0; d < dcount; d++) free(dirs[d]);
    for (int s = 0; s < seen_count; s++) free(seen[s]);
}

char *ba_load_skill(const char *skill_name, const char *cwd,
                    const char *agents_home, const char *bag_home,
                    char **out_skill_dir) {
    char *dirs[4];
    int dcount = 0;
    char *content = NULL;
    int d;

    dirs[dcount++] = xasprintf("%s/skills", cwd);
    if (agents_home && agents_home[0])
        dirs[dcount++] = xasprintf("%s/.agents/skills", agents_home);
    if (bag_home && bag_home[0])
        dirs[dcount++] = xasprintf("%s/skills", bag_home);

    for (d = 0; d < dcount && !content; d++) {
        char *skill_dir_path = xasprintf("%s/%s", dirs[d], skill_name);
        char *md_path = xasprintf("%s/SKILL.md", skill_dir_path);
        content = read_all(md_path);
        free(md_path);
        if (content) {
            /* replace ${BA_AGENT_SKILL_DIR} placeholders */
            const char *placeholder = strstr(content, "${BA_AGENT_SKILL_DIR}");
            if (placeholder) {
                StrBuf replaced;
                sb_init(&replaced);
                size_t prefix_len = placeholder - content;
                sb_appendn(&replaced, content, prefix_len);
                sb_append(&replaced, skill_dir_path);
                sb_append(&replaced, placeholder + strlen("${BA_AGENT_SKILL_DIR}"));
                free(content);
                content = replaced.data;
            }
            /* format: Base directory: <dir>\n\n<content> (bash-agent parity) */
            StrBuf full;
            sb_init(&full);
            sb_appendf(&full, "Base directory: %s\n\n%s", skill_dir_path, content);
            free(content);
            content = full.data;
            if (out_skill_dir) *out_skill_dir = skill_dir_path;
            else free(skill_dir_path);
        } else {
            free(skill_dir_path);
        }
    }
    for (d = 0; d < dcount; d++) free(dirs[d]);
    return content;
}

/* ============================================================
 * system prompt building - main function (block order follows bash-agent)
 * ============================================================ */

char *ba_build_prompt(const BaPromptCtx *ctx) {
    StrBuf buf;
    sb_init(&buf);

    const char *locale = detect_locale();
    int is_zh = (locale[0] == 'z' && locale[1] == 'h');

    /* 1. agent-identity */
    {
        const char *identity = "You are busyagent, a lightweight coding agent that runs as a busybox applet.";
        if (is_zh) identity = "你是 busyagent，一个以 busybox applet 形式运行的轻量级编码智能体。";
        prompt_append_section(&buf, "agent-identity", identity, NULL);
    }

    /* 2. environment */
    {
        struct utsname uts;
        StrBuf env;
        char *sh_env = getenv("SHELL");
        sb_init(&env);
        sb_appendf(&env, "lang: %s\n", detect_locale());
        sb_appendf(&env, "pwd: %s\n", ctx->cwd ? ctx->cwd : "?");
        sb_appendf(&env, "home: %s\n", ctx->home ? ctx->home : "?");
        if (uname(&uts) == 0)
            sb_appendf(&env, "platform: %s\n", uts.sysname);
        else
            sb_append(&env, "platform: unknown\n");
        sb_appendf(&env, "shell: %s", sh_env && sh_env[0] ? sh_env : "/bin/sh");
        prompt_append_section(&buf, "environment", env.data, NULL);
        sb_free(&env);
    }

    /* 3. rules */
    {
        const char *rules = "- Be concise and concrete. Lead with the answer. Use short sections or bullets when they improve readability. No pleasantries, no explanations unless asked. Raw results only.\n"
                            "- Prefer safe, exact edits.\n"
                            "- Report failures clearly.";
        prompt_append_section(&buf, "rules", rules, NULL);
    }

    /* 4. using-your-tools */
    {
        const char *tool_guidance =
            "- Use Read for a single file. If you need multiple files, call Read multiple times.\n"
            "- Read supports optional offset and limit parameters to read specific line ranges (saves tokens for large files). Output includes line numbers.\n"
            "- Use Glob and Grep for one pattern at a time.\n"
            "- Grep supports a context parameter to show surrounding lines — use it to get enough text for Edit directly from Grep output, avoiding a separate Read.\n"
            "- Use multiple tool calls in one response when they are independent.\n"
            "- Prefer dedicated tools over Bash when a dedicated tool fits the task.\n"
            "- For Edit: copy old_string exactly (including whitespace/indent/newlines). If you already know the location from prior context, use Read with offset/limit. If you need to locate the text first, use Grep with context — its output is often sufficient for Edit without an extra Read.\n"
            "- For skills, first check the skill-index section, then use Skill(name) for the matching skill.\n"
            "- Bash supports background=true for long-running commands. Returns task_id immediately; this build delivers the output by writing it to a temp file - read that file with Read to fetch results (bash-agent injects it via async events instead).";
        prompt_append_section(&buf, "using-your-tools", tool_guidance, NULL);
    }

    /* 5. sub-agent-guidance (sync wording adjusted, rest verbatim) */
    {
        const char *sag =
            "- **When to use**: delegating independent sub-tasks that do NOT need your current conversation context — e.g. investigating a separate file, running a focused search, testing a hypothesis in isolation.\n"
            "- **Recursion limit**: only the main agent may launch SubAgent. A child agent must not call SubAgent again; the runtime rejects nested launches.\n"
            "- **When NOT to use**: tasks that depend on your working context, conversation history, or intermediate state. The child agent starts with a blank slate.\n"
            "- **Prompt design**: write a complete, self-contained prompt. Include all file paths, function names, error messages, and constraints the child needs. Assume zero shared context.\n"
            "- **Result handling**: this build runs the sub-agent synchronously; its final answer is returned directly as this call's result. Interpret it in your next turn before acting.";
        prompt_append_section(&buf, "sub-agent-guidance", sag, NULL);
    }

    /* 6. todo-guidance (verbatim) */
    {
        const char *todo =
            "- Use TodoWrite proactively for complex multi-step implementation, debugging, refactoring, review, or multi-file tasks.\n"
            "- Do not use TodoWrite for trivial single-step, single-command, or purely informational requests.\n"
            "- After receiving a non-trivial task, create an initial checklist before or as you begin work.\n"
            "- When you use TodoWrite, write the full updated checklist for the current session, not a partial diff.\n"
            "- Keep the checklist short, concrete, and actionable.\n"
            "- Prefer exactly one in_progress item when work is actively underway.\n"
            "- Mark items completed immediately after finishing them, and remove stale items that no longer matter.";
        prompt_append_section(&buf, "todo-guidance", todo, NULL);
    }

    /* 7. plan-lifecycle-guidance */
    {
        StrBuf plg;
        sb_init(&plg);
        sb_append(&plg, "- **PLANNING WORKFLOW** — For complex multi-step tasks (3+ steps OR multi-file OR user requests planning)\n");
        sb_appendf(&plg, "- **Files**: PLAN_DRAFT_FILE: %s | PLAN_FILE: %s\n",
                   ctx->plan_draft ? ctx->plan_draft : "<not set>",
                   ctx->plan ? ctx->plan : "<not set>");
        sb_append(&plg, "- **Why draft first?** Writing to PLAN_FILE immediately invalidates the system prompt cache. Use PLAN_DRAFT_FILE for all drafting iterations to avoid this cost.\n");
        sb_append(&plg, "- **Drafting phase** (PLAN_DRAFT_FILE non-empty → you are drafting):\n"
                        "  Every user reply MUST be classified as exactly ONE of:\n"
                        "  ① REVISE (any feedback/question/change) → Write/Edit PLAN_DRAFT_FILE → ask confirmation → stay in drafting\n"
                        "  ② CONFIRM (explicit ok/go/confirmed) → call PlanConfirm IMMEDIATELY (before any other action) → TodoWrite checklist → execute\n"
                        "  ③ CANCEL (explicit cancel/forget it) → empty out PLAN_DRAFT_FILE → exit to idle\n"
                        "  ⚠ On CONFIRM you MUST call PlanConfirm first — no edits, no tool calls before it.\n");
        sb_append(&plg, "- **Execution phase**: after PlanConfirm → TodoWrite checklist → execute tasks → PlanClear when all done\n"
                        "- **Plan vs Todo**: PLAN_FILE=locked plan (only via PlanConfirm), PLAN_DRAFT_FILE=draft (edit freely), TodoWrite=progress tracker. Do NOT mix.");
        prompt_append_section(&buf, "plan-lifecycle-guidance", plg.data, NULL);
        sb_free(&plg);
    }

    /* 8. instruction-files */
    {
        StrBuf ifiles;
        char *gc = find_instruction_file(ctx->home);
        char *pc = ctx->cwd ? find_instruction_file(ctx->cwd) : NULL;

        sb_init(&ifiles);
        if (gc && gc[0]) {
            prompt_append_section(&ifiles, "instruction-file", gc, "global");
        }
        if (pc && pc[0]) {
            prompt_append_section(&ifiles, "instruction-file", pc, "project");
        }
        if (ifiles.len > 0 && ifiles.data[ifiles.len - 1] == '\n')
            sb_truncate(&ifiles, ifiles.len - 1);
        prompt_append_section(&buf, "instruction-files", ifiles.data, NULL);
        sb_free(&ifiles);
        free(gc);
        free(pc);
    }

    /* 9. skill-index */
    {
        StrBuf si;
        sb_init(&si);
        build_skill_index(&si, ctx->cwd, getenv("HOME"), ctx->home);
        if (si.len > 0 && si.data[si.len - 1] == '\n')
            sb_truncate(&si, si.len - 1);
        prompt_append_section(&buf, "skill-index", si.data, NULL);
        sb_free(&si);
    }

    /* 11. current-plan */
    {
        char *plan = ctx->plan ? read_all(ctx->plan) : NULL;
        if (plan && plan[0]) {
            /* bash version: name attribute = plan file path */
            prompt_append_section(&buf, "current-plan", plan, ctx->plan);
        }
        free(plan);
    }

    /* 13. output-language */
    {
        StrBuf ol;
        sb_init(&ol);
        if (is_zh) {
            sb_append(&ol, "再次强调：必须使用中文进行所有输出，包括你的思考过程（Chain of Thought/推理/thinking）！严禁在思考或回答中出现任何英文内容！");
        } else {
            sb_appendf(&ol, "MUST use \"%s\" for all output, including your Chain of Thought/reasoning/thinking! Never mix languages! Code, commands, and file content remain as-is.", locale);
        }
        prompt_append_section(&buf, "output-language", ol.data, NULL);
        sb_free(&ol);
    }

    /* drop the trailing \n (bash printf '%s' semantics) */
    if (buf.len > 0 && buf.data[buf.len - 1] == '\n') {
        buf.data[buf.len - 1] = '\0';
        buf.len--;
    }

    return buf.data;
}

/* ==== ba_tools.c ==== */
/*
 * ba_tools - table-driven tool execution for busyagent
 *
 * Tool definitions live in $BA_HOME/tools.json — the only source,
 * never embedded in the binary. A sample lives at scripts/tools.example.json.
 * Each
 * tool carries an "exec" mapping: {"applet": "grep", "argv": ["-nH", "-e",
 * "$pattern", "$path"]}. At execution time argv templates are expanded with
 * values from the model's input JSON (missing optional values drop their
 * argument), then dispatched:
 *
 *   - applet is NOFORK  -> run_nofork_applet() in a forked child (direct
 *     in-process call, no exec: the busybox advantage)
 *   - anything else     -> fork + execl(bb_busybox_exec_path, applet, ...)
 *
 * Children run with piped stdout/stderr and are SIGKILLed on timeout.
 *
 * Copyright (C) 2026 by Lloyd Zhou <lloydzhou@qq.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include "busybox.h"   /* for APPLET_IS_NOFORK */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define BA_TOOL_TIMEOUT_MS   120000
#define BA_OUTPUT_MAX        (128 * 1024)

/* ---- tool table ---- */

typedef struct {
	char *name;            /* LLM-visible tool name */
	char *applet;          /* busybox applet to dispatch */
	char **argv_tpl;       /* argv template strings, $var placeholders */
	int argv_count;
} BaTool;

static BaTool *g_tools;
static int g_tool_count;
static char *g_tools_json_text;   /* raw text of the active tools array */
static const SessionPaths *g_paths;   /* session sink for builtin state tools */

static char *read_file_all(const char *path)
{
	int fd = open(path, O_RDONLY);
	char *buf;
	if (fd < 0)
		return NULL;
	buf = xmalloc_read(fd, NULL);   /* libbb: allocates and NUL-terminates */
	close(fd);
	return buf;
}

/* Parse one tools JSON text into the global table. Returns 0 on success.
 * Every entry must carry a non-empty name and a well-formed exec mapping
 * (exec.applet: non-empty string, exec.argv: array of strings) - malformed
 * entries are rejected here instead of crashing later at execution time
 * (a non-string argv template would dereference NULL in expand_token). */
static int ba_tools_parse(const char *json)
{
	JsonParse jp = json_parse_root(json);
	JsonVal arr;
	int n, i;

	if (jp.error)
		return -1;
	arr = jp.val;
	n = json_array_len(arr);
	if (n <= 0)
		return -1;

	g_tools = xzalloc(n * sizeof(BaTool));
	g_tool_count = 0;

	for (i = 0; i < n; i++) {
		JsonVal item, exec, argv_val, fnv;
		BaTool *t = &g_tools[g_tool_count];
		int an, j, bad = 0;

		item = json_array_get(arr, i);
		fnv = json_get(item, "function");
		t->name = (fnv.type != JSON_NULL)
			? json_get_string(fnv, "name")
			: json_get_string(item, "name");
		exec = json_get(item, "exec");
		argv_val = json_get(exec, "argv");
		if (exec.type == JSON_OBJECT)
			t->applet = json_get_string(exec, "applet");
		an = (argv_val.type == JSON_ARRAY) ? json_array_len(argv_val) : 0;

		if (!t->name || !t->name[0]) {
			bb_error_msg("tools.json: entry %d: missing tool name, skipped", i);
			bad = 1;
		} else if (exec.type != JSON_OBJECT
		 || !t->applet || !t->applet[0]) {
			bb_error_msg("tools.json: entry '%s': exec.applet missing, skipped", t->name);
			bad = 1;
		}
		if (!bad && an > 0) {
			t->argv_tpl = xzalloc(an * sizeof(char *));
			t->argv_count = an;
			for (j = 0; j < an; j++) {
				JsonVal av = json_array_get(argv_val, j);
				if (av.type != JSON_STRING) {
					bb_error_msg("tools.json: entry '%s': exec.argv[%d]"
						     " is not a string, skipped", t->name, j);
					bad = 1;
					break;
				}
				t->argv_tpl[j] = json_string_val(av);
			}
		}
		if (bad) {
			free(t->name);
			free(t->applet);
			if (t->argv_tpl) {
				for (j = 0; j < t->argv_count; j++)
					free(t->argv_tpl[j]);
				free(t->argv_tpl);
			}
			memset(t, 0, sizeof(*t));
			continue;
		}
		g_tool_count++;
	}
	if (g_tool_count == 0) {
		free(g_tools);
		g_tools = NULL;
		return -1;
	}
	return 0;
}

/* Load tools from $BA_HOME/tools.json. The file is the only
 * source: nothing is embedded in the binary. Missing or broken file
 * means "no tools" — plain Q&A still works, requests just omit tools. */
void ba_tools_set_paths(const SessionPaths *paths)
{
	g_paths = paths;
}

int ba_tools_init(const char *home, const SessionPaths *paths)
{
	char *path = NULL;
	char *data = NULL;

	if (g_tools)
		return g_tool_count;
	g_paths = paths;

	if (home && home[0])
		path = xasprintf("%s/tools.json", home);
	if (path)
		data = read_file_all(path);
	if (data && ba_tools_parse(data) == 0) {
		g_tools_json_text = data;
	} else {
		if (data) {
			bb_error_msg("tools.json parse failed, dynamic zone ignored (builtins remain)");
			free(data);
		}
		/* missing file is the normal path: builtins only, stay quiet */
		g_tools_json_text = NULL;
	}
	free(path);
	return g_tool_count;
}

void ba_tools_free(void)
{
	int i, j;
	for (i = 0; i < g_tool_count; i++) {
		free(g_tools[i].name);
		free(g_tools[i].applet);
		for (j = 0; j < g_tools[i].argv_count; j++)
			free(g_tools[i].argv_tpl[j]);
		free(g_tools[i].argv_tpl);
	}
	free(g_tools);
	g_tools = NULL;
	g_tool_count = 0;
	free(g_tools_json_text);
	g_tools_json_text = NULL;
}

/* span copy: JsonVal is a (src,start,end) view */
static char *val_span_dup(const char *src, JsonVal v)
{
    size_t n = v.end - v.start;
    char *s = xmalloc(n + 1);
    memcpy(s, src + v.start, n);
    s[n] = '\0';
    return s;
}

/* Append one dynamic tool entry to the LLM-visible tools array.
 * The internal "exec" mapping (applet + argv template) is host-side only:
 * the request is rebuilt from the public fields so the model can neither
 * see nor influence how the tool dispatches. */
static void tools_json_append_entry(StrBuf *sb, const char *src, JsonVal item)
{
    JsonVal fnv = json_get(item, "function");
    int is_fn = (fnv.type != JSON_NULL);
    JsonVal body = is_fn ? fnv : item;
    JsonVal namev = json_get(body, "name");
    JsonVal descv = json_get(body, "description");
    JsonVal schemav = json_get(body, is_fn ? "parameters" : "input_schema");
    char *span;

    if (namev.type != JSON_STRING)
        return;   /* caller already reported unnamed entries */

    sb_append(sb, ",\n  ");
    if (is_fn)
        sb_append(sb, "{\"type\":\"function\",\"function\":{\"name\":");
    else
        sb_append(sb, "{\"name\":");
    span = val_span_dup(src, namev);   /* includes the quotes */
    sb_append(sb, span);
    free(span);
    if (descv.type == JSON_STRING) {
        sb_append(sb, ",\"description\":");
        span = val_span_dup(src, descv);
        sb_append(sb, span);
        free(span);
    }
    if (schemav.type == JSON_OBJECT) {
        sb_append(sb, is_fn ? ",\"parameters\":" : ",\"input_schema\":");
        span = val_span_dup(src, schemav);
        sb_append(sb, span);
        free(span);
    }
    sb_append(sb, is_fn ? "}}" : "}");
}

/* The tools array as the LLM sees it (exec stripped).
 * NULL when no table: the caller omits tools from the request. */
/* LLM-visible tools = 11 builtins + dynamic zone (exec stripped).
 * Without a dynamic zone only the builtins ship (sh anchor). */
char *ba_tools_json(void)
{
    const char *src = g_tools_json_text;
    StrBuf sb;

    if (!src)
        return util_strdup(ba_builtin_schemas);

    {
        JsonParse jp = json_parse_root(src);
        int n, i;
        if (jp.error)
            return util_strdup(ba_builtin_schemas);
        n = json_array_len(jp.val);
        sb_init(&sb);
        sb_append(&sb, ba_builtin_schemas);
        sb_truncate(&sb, sb.len - 2);   /* drop trailing "]\n", append dynamic zone */
        for (i = 0; i < n; i++) {
            JsonVal item = json_array_get(jp.val, i);
            JsonVal fnv = json_get(item, "function");
            char *nm = (fnv.type != JSON_NULL)
                     ? json_get_string(fnv, "name")
                     : json_get_string(item, "name");
            /* builtin names may not be shadowed */
            if (nm && (!strcmp(nm, "Read") || !strcmp(nm, "Write") || !strcmp(nm, "Edit")
                    || !strcmp(nm, "Bash") || !strcmp(nm, "Glob") || !strcmp(nm, "Grep")
                    || !strcmp(nm, "TodoWrite") || !strcmp(nm, "PlanConfirm")
                    || !strcmp(nm, "PlanClear") || !strcmp(nm, "Skill")
                    || !strcmp(nm, "SubAgent"))) {
                bb_error_msg("tools.json: '%s' shadows a builtin, skipped", nm);
                free(nm);
                continue;
            }
            tools_json_append_entry(&sb, src, item);
            free(nm);
        }
        sb_append(&sb, "]\n");
    }
    return sb.data;
}

/* dynamic-zone starter template (-i export) */
/* template is derived from ba_builtin_schemas at write time */

/* Export the starter tools table to path. Entry names must not overlap
 * the builtins (enforced at runtime too). 0 ok; -1 exists; -2 write error. */
int ba_tools_write_template(const char *path)
{
	int fd;
	char *dir;

	if (access(path, F_OK) == 0)
		return -1;
	dir = xstrdup(path);
	{
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			bb_make_directory(dir, 0755, FILEUTILS_RECUR);
		}
	}
	free(dir);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return -2;
	{
		const char *b = ba_builtin_schemas;
		size_t bl = strlen(b);
		/* strip the trailing "]" of the builtin array, then close with a
		 * worked custom entry so the file is one valid JSON array. The
		 * entry MUST carry an exec mapping (applet + argv template) -
		 * without it the tool would be offered to the model but fail
		 * at execution time. $name placeholders expand from the model's
		 * input JSON at call time. */
		static const char tail[] =
			",\n"
			"  {\n"
			"    \"name\": \"MyApplet\",\n"
			"    \"description\": \"Example custom entry - edit or remove.\",\n"
			"    \"input_schema\": {\n"
			"      \"type\": \"object\",\n"
			"      \"properties\": { \"pattern\": { \"type\": \"string\" } },\n"
			"      \"required\": [\"pattern\"]\n"
			"    },\n"
			"    \"exec\": {\n"
			"      \"applet\": \"grep\",\n"
			"      \"argv\": [\"-rl\", \"-e\", \"$pattern\", \".\"]\n"
			"    }\n"
			"  }]\n";
		if (bl < 2 || full_write(fd, b, bl - 2) < 0
		 || full_write(fd, tail, sizeof(tail) - 1) < 0) {
			close(fd);
			return -2;
		}
	}
	close(fd);
	return 0;
}

/* ---- execution ---- */

struct out_buf {
	char *data;
	size_t len;
	size_t cap;
};

static void out_append(struct out_buf *b, const char *ptr, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 4096;
		while (nc < b->len + n + 1)
			nc *= 2;
		if (nc > BA_OUTPUT_MAX + 1)
			nc = BA_OUTPUT_MAX + 1;
		if (nc <= b->len)
			return;   /* full */
		b->data = xrealloc(b->data, nc);
		b->cap = nc;
	}
	if (b->len + n > BA_OUTPUT_MAX)
		n = BA_OUTPUT_MAX - b->len;
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';
}

/* Run one argv in a forked child with captured output.
 * NOFORK applets -> run_nofork_applet() direct call; others -> exec
 * busybox itself with the applet name. */
static int run_captured(const char *applet, char **argv,
			struct out_buf *out, struct out_buf *err,
			int timeout_ms)
{
	int pipe_out[2], pipe_err[2];
	pid_t pid;
	int applet_no = find_applet_by_name(applet);
	int nofork = (applet_no >= 0 && APPLET_IS_NOFORK(applet_no));
	int status = 0;
	int64_t deadline;
	char buf[4096];

	if (applet_no < 0 && strcmp(applet, "sh") != 0)
		return -1;   /* unknown and not sh: refuse */

	xpipe(pipe_out);
	xpipe(pipe_err);

	pid = fork();
	if (pid < 0) {
		close(pipe_out[0]); close(pipe_out[1]);
		close(pipe_err[0]); close(pipe_err[1]);
		return -1;
	}
	if (pid == 0) {
		/* child */
		signal(SIGPIPE, SIG_DFL);
		close(pipe_out[0]); close(pipe_err[0]);
		xmove_fd(xopen("/dev/null", O_RDONLY), STDIN_FILENO);
		xmove_fd(pipe_out[1], STDOUT_FILENO);
		xmove_fd(pipe_err[1], STDERR_FILENO);
		if (nofork) {
			/* NOFORK applets must never run in the parent process:
			 * they mutate global busybox state. We are forked. */
			_exit(run_nofork_applet(applet_no, argv));
		}
		BB_EXECVP(bb_busybox_exec_path, argv);
		/* argv[0] must be the applet name for busybox dispatch */
		_exit(127);
	}
	close(pipe_out[1]); close(pipe_err[1]);

	deadline = (int64_t)monotonic_ms() + timeout_ms;
	for (;;) {
		struct pollfd pfd[2];
		int nready;

		pfd[0].fd = pipe_out[0];
		pfd[0].events = POLLIN;
		pfd[1].fd = pipe_err[0];
		pfd[1].events = POLLIN;
		nready = poll(pfd, 2, 100);
		if (nready > 0) {
			if (pfd[0].revents & (POLLIN | POLLHUP)) {
				ssize_t n = read(pipe_out[0], buf, sizeof(buf));
				if (n > 0) out_append(out, buf, n);
			}
			if (pfd[1].revents & (POLLIN | POLLHUP)) {
				ssize_t n = read(pipe_err[0], buf, sizeof(buf));
				if (n > 0) out_append(err, buf, n);
			}
		}
		if (waitpid(pid, &status, WNOHANG) == pid) {
			ssize_t n;
			while ((n = read(pipe_out[0], buf, sizeof(buf))) > 0)
				out_append(out, buf, n);
			while ((n = read(pipe_err[0], buf, sizeof(buf))) > 0)
				out_append(err, buf, n);
			break;
		}
		if (monotonic_ms() > deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			status = -2;   /* timeout marker */
			break;
		}
	}
	close(pipe_out[0]); close(pipe_err[0]);
	return status;
}

static void result_wrap(StrBuf *sb, int status, struct out_buf *out,
			struct out_buf *err)
{
	if (status == -2) {
		sb_append(sb, "Error: command timed out\n");
		return;
	}
	if (status == -1) {
		sb_append(sb, "Error: failed to start command\n");
		return;
	}
	if (out->data && out->len)
		sb_append(sb, out->data);
	if (err->data && err->len) {
		sb_append(sb, "\n[stderr]\n");
		sb_append(sb, err->data);
	}
	if ((!out->data || !out->len) && (!err->data || !err->len)) {
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "(no output, exit %d)",
			 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		sb_append(sb, tmp);
	}
}

static const BaTool *find_tool(const char *name)
{
	int i;
	for (i = 0; i < g_tool_count; i++)
		if (g_tools[i].name && strcmp(g_tools[i].name, name) == 0)
			return &g_tools[i];
	return NULL;
}

/* Expand one argv template token: "$var" -> value from input JSON;
 * a token that references a missing key expands to NULL (dropped). */
static char *expand_token(const char *tpl, JsonVal input)
{
	char key[128];
	const char *p;

	if (tpl[0] != '$')
		return util_strdup(tpl);
	p = tpl + 1;
	if (p[0] == '{' && p[strlen(p) - 1] == '}') {
		/* ${name} */
		size_t n = strlen(p) - 2;
		if (n >= sizeof(key))
			return NULL;
		memcpy(key, p + 1, n);
		key[n] = '\0';
		p = key;
	}
	return json_get_string(input, p);
}

char *ba_tool_execute(const char *name, const char *input_json, int timeout_ms)
{
	StrBuf sb;
	JsonParse jp;
	const BaTool *tool;
	char **argv;
	int argc, i, ai = 0;
	struct out_buf out = { NULL, 0, 0 }, err = { NULL, 0, 0 };
	int status;

	sb_init(&sb);

	jp = json_parse_root(input_json && input_json[0] ? input_json : "{}");
	if (jp.error) {
		sb_append(&sb, "Error: invalid tool input JSON");
		return sb.data;
	}

	/* ---- builtin reserved names (L2/L3): loop-level semantics ---- */

	if (strcmp(name, "Bash") == 0) {
		char *cmd = json_get_string(jp.val, "command");
		int background = json_get_bool(jp.val, "background", false);
		int tmo_sec = json_get_int(jp.val, "timeout");
		int tmo_ms = json_get_int(jp.val, "timeout_ms");

		if (!cmd || !cmd[0]) {
			sb_append(&sb, "Error: Bash requires 'command'");
			free(cmd);
			return sb.data;
		}

		if (background) {
			/* One registered implementation only (ba_background_spawn in
			 * busyagent.c): task gets a task_id + hard deadline, its log
			 * is RLIMIT_FSIZE-capped, and ba_drain_background reaps it
			 * and injects the result. The previous second, unregistered
			 * double-fork variant here leaked processes, temp files and
			 * could never report completion. */
			if (!cmd[0]) {
				sb_append(&sb, "Error: no command provided");
				free(cmd);
				return sb.data;
			}
			{
				char *resp = ba_background_spawn(cmd);
				sb_append(&sb, resp);
				free(resp);
			}
			free(cmd);
			return sb.data;
		}

		{
			char *sh_argv[4];
			int tmo = tmo_ms;
			if (!tmo && tmo_sec > 0)
				tmo = tmo_sec * 1000;   /* bash-agent takes seconds */
			if (!tmo)
				tmo = timeout_ms;

			sh_argv[0] = (char *)"sh";
			sh_argv[1] = (char *)"-c";
			sh_argv[2] = cmd;
			sh_argv[3] = NULL;
			status = run_captured("sh", sh_argv, &out, &err, tmo);
			result_wrap(&sb, status, &out, &err);
		}
		free(cmd);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	if (strcmp(name, "Read") == 0) {
		/* cat -n semantics: whole file or offset/limit paging (no fork) */
		char *path = json_get_string(jp.val, "path");
		int offset = json_get_int(jp.val, "offset");
		int limit = json_get_int(jp.val, "limit");
		char *data, *p;
		size_t lineno = 0, count = 0;

		if (!path || !path[0]) {
			sb_append(&sb, "Error: Read requires 'path'");
			free(path);
			return sb.data;
		}
		data = read_file_all(path);
		if (!data) {
			sb_appendf(&sb, "Error: cannot read %s", path);
			free(path);
			return sb.data;
		}
		p = data;
		while (*p) {
			char *next = strchr(p, '\n');
			int ll = next ? (int)(next - p) : (int)strlen(p);
			lineno++;
			if ((!offset || lineno >= (size_t)offset)
			 && (!limit || count < (size_t)limit)) {
				sb_appendf(&sb, "%6zu\t%.*s\n", lineno, ll, p);
				count++;
			}
			if (!next)
				break;
			p = next + 1;
		}
		if (offset && lineno < (size_t)offset)
			sb_appendf(&sb, "(file has only %zu lines)", lineno);
		free(data);
		free(path);
		return sb.data;
	}

	if (strcmp(name, "Write") == 0) {
		char *path = json_get_string(jp.val, "path");
		char *content = json_get_string(jp.val, "content");
		int fd;

		if (!path || !path[0] || !content) {
			sb_append(&sb, "Error: Write requires 'path' and 'content'");
			free(path); free(content);
			return sb.data;
		}
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			sb_appendf(&sb, "Error: cannot write %s", path);
		} else if (full_write(fd, content, strlen(content)) < 0) {
			close(fd);
			sb_appendf(&sb, "Error: short write to %s", path);
		} else {
			close(fd);
			sb_append(&sb, "wrote ");
			sb_append(&sb, path);
		}
		free(path); free(content);
		return sb.data;
	}

	if (strcmp(name, "Edit") == 0) {
		char *path = json_get_string(jp.val, "path");
		char *old_s = json_get_string(jp.val, "old_string");
		char *new_s = json_get_string(jp.val, "new_string");
		char *data, *hit, *second;

		if (!path || !old_s || !old_s[0] || !new_s) {
			sb_append(&sb, "Error: Edit requires 'path', 'old_string', 'new_string'");
			free(path); free(old_s); free(new_s);
			return sb.data;
		}
		data = read_file_all(path);
		if (!data) {
			sb_appendf(&sb, "Error: cannot read %s", path);
			free(path); free(old_s); free(new_s);
			return sb.data;
		}
		hit = strstr(data, old_s);
		if (!hit) {
			sb_append(&sb, "Error: old_string not found in ");
			sb_append(&sb, path);
		} else {
			second = strstr(hit + 1, old_s);
			if (second) {
				sb_append(&sb, "Error: old_string occurs more than once in ");
				sb_append(&sb, path);
			} else {
				StrBuf nb;
				sb_init(&nb);
				sb_append(&nb, data);
				sb_truncate(&nb, hit - data);   /* head */
				sb_append(&nb, new_s);          /* replacement */
				sb_append(&nb, hit + strlen(old_s)); /* tail */
				{
					int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
					if (fd < 0) {
						sb_appendf(&sb, "Error: cannot write %s", path);
					} else if (full_write(fd, nb.data, nb.len) < 0) {
						close(fd);
						sb_appendf(&sb, "Error: short write to %s", path);
					} else {
						close(fd);
						sb_append(&sb, "edited ");
						sb_append(&sb, path);
					}
				}
				sb_free(&nb);
			}
		}
		free(data);
		free(path); free(old_s); free(new_s);
		return sb.data;
	}

	if (strcmp(name, "Glob") == 0) {
		char *pattern = json_get_string(jp.val, "pattern");
		char *gpath = json_get_string(jp.val, "path");
		char *gv[6];
		int gi = 0;

		if (!pattern || !pattern[0]) {
			sb_append(&sb, "Error: Glob requires 'pattern'");
			free(pattern); free(gpath);
			return sb.data;
		}
		/* path becomes find(1)'s positional root argument: a leading '-'
		 * would be parsed as an option/action (e.g. "-delete", "-exec") */
		if (gpath && gpath[0] == '-') {
			sb_append(&sb, "Error: Glob 'path' must not start with '-'");
			free(pattern); free(gpath);
			return sb.data;
		}
		gv[gi++] = (char *)"find";
		gv[gi++] = (gpath && gpath[0]) ? gpath : (char *)".";
		gv[gi++] = (char *)"-name";
		gv[gi++] = pattern;
		gv[gi] = NULL;
		status = run_captured("find", gv, &out, &err, timeout_ms);
		result_wrap(&sb, status, &out, &err);
		free(pattern); free(gpath);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	if (strcmp(name, "Grep") == 0) {
		char *pattern = json_get_string(jp.val, "pattern");
		char *gpath = json_get_string(jp.val, "path");
		char *glob_f = json_get_string(jp.val, "glob");
		int ctx = json_get_int(jp.val, "context");
		char ctxs[16];
		char *ctx_dup = NULL, *inc_dup = NULL;
		char *gv[12];
		int gi = 0;

		if (!pattern || !pattern[0]) {
			sb_append(&sb, "Error: Grep requires 'pattern'");
			free(pattern); free(gpath); free(glob_f);
			return sb.data;
		}
		/* same option-injection guard as Glob: path is grep's positional
		 * search root and would otherwise be read as an option */
		if (gpath && gpath[0] == '-') {
			sb_append(&sb, "Error: Grep 'path' must not start with '-'");
			free(pattern); free(gpath); free(glob_f);
			return sb.data;
		}
		gv[gi++] = (char *)"grep";
		if (glob_f && glob_f[0]) {
			inc_dup = xasprintf("--include=%s", glob_f);
			gv[gi++] = inc_dup;
		}
		if (ctx > 0) {
			snprintf(ctxs, sizeof(ctxs), "-%d", ctx);
			ctx_dup = xstrdup(ctxs);
			gv[gi++] = ctx_dup;
		}
		gv[gi++] = (char *)"-nH";
		gv[gi++] = (char *)"-r";
		gv[gi++] = (char *)"-e";
		gv[gi++] = pattern;
		if (gpath && gpath[0])
			gv[gi++] = gpath;
		gv[gi] = NULL;
		status = run_captured("grep", gv, &out, &err, timeout_ms);
		result_wrap(&sb, status, &out, &err);
		free(ctx_dup);
		free(pattern); free(gpath);
		free(out.data);
		free(err.data);
		return sb.data;
	}

	if (strcmp(name, "TodoWrite") == 0) {
		/* todos array -> markdown checklist, fed back as the tool result.
		 * Persistence lives in the conversation history (the full call record
		 * IS the state); nothing hits disk or the system prompt. */
		JsonVal todos = json_get(jp.val, "todos");
		int total, i;
		if (todos.type != JSON_ARRAY) {
			sb_append(&sb, "OK");   /* bash-agent parity: no error on bad shapes */
			return sb.data;
		}
		total = json_array_len(todos);
		for (i = 0; i < total; i++) {
			JsonVal item = json_array_get(todos, i);
			char *content = json_get_string(item, "content");
			char *st = json_get_string(item, "status");
			sb_append(&sb, "- [");
			sb_append(&sb, (st && strcmp(st, "completed") == 0) ? "x" : " ");
			sb_append(&sb, "] ");
			sb_append(&sb, content ? content : "");
			sb_append(&sb, "\n");
			free(content); free(st);
		}
		/* drop the trailing newline (bash-agent parity) */
		if (sb.len > 0 && sb.data[sb.len - 1] == '\n') {
			sb.data[sb.len - 1] = '\0';
			sb.len--;
		}
		return sb.data;
	}

	if (strcmp(name, "PlanConfirm") == 0) {
		char *draft = g_paths ? store_plan_draft_read(g_paths) : NULL;
		if (!draft || !draft[0]) {
			sb_append(&sb, "Error: no plan draft to confirm (write it first)");
			free(draft);
			return sb.data;
		}
		store_plan_set(g_paths, draft);
		store_plan_draft_clear(g_paths);
		/* note: bash-agent moves the file after compaction; this port
 * is synchronous without compaction, so write it here */
		sb_append(&sb, "Plan confirmed and locked in.");
		free(draft);
		return sb.data;
	}

	if (strcmp(name, "PlanClear") == 0) {
		if (g_paths)
			store_plan_clear(g_paths);
		sb_append(&sb, "Plan cleared.");
		return sb.data;
	}

	if (strcmp(name, "Skill") == 0) {
		/* L2 knowledge level: unified search via ba_load_skill
		 * cwd/skills > ~/.agents/skills > $BA_HOME/skills，
		 * supports ${BA_AGENT_SKILL_DIR} substitution */
		char *skill = json_get_string(jp.val, "name");
		const char *agents_home = getenv("HOME");
		const char *bag_home = getenv("BA_HOME");
		char *cwd = xrealloc_getcwd_or_warn(NULL);
		char *content;

		if (!skill || !skill[0]) {
			sb_append(&sb, "Error: Skill requires 'name'");
			free(skill); free(cwd);
			return sb.data;
		}
		content = ba_load_skill(skill, cwd,
					(agents_home && agents_home[0]) ? agents_home : NULL,
					bag_home ? bag_home : "/tmp/busyagent",
					NULL);
		free(cwd);
		if (!content) {
			sb_appendf(&sb, "Error: skill '%s' not found "
				     "(searched $CWD/skills, ~/.agents/skills, $BA_HOME/skills)", skill);
			free(skill);
			return sb.data;
		}
		sb_append(&sb, content);
		free(content);
		free(skill);
		return sb.data;
	}

	if (strcmp(name, "SubAgent") == 0) {
		/* placeholder: intercepted by the turn loop (tools.c parity) */
		sb_append(&sb, "SubAgent handled by agent layer");
		return sb.data;
	}

	/* ---- L0/L1: exec mapping ---- */

	tool = find_tool(name);
	if (!tool || !tool->applet) {
		sb_appendf(&sb, "Error: tool '%s' is not in the tool table", name);
		return sb.data;
	}

	/* argv[0] = applet name (busybox dispatch convention) */
	argc = tool->argv_count + 1;
	argv = xzalloc(argc * sizeof(char *));
	argv[ai++] = tool->applet;
	for (i = 0; i < tool->argv_count; i++) {
		char *v = expand_token(tool->argv_tpl[i], jp.val);
		if (!v) {
			/* missing optional key: drop this argument */
			argc--;
			continue;
		}
		argv[ai++] = v;
	}
	argv[ai] = NULL;

	if (timeout_ms <= 0)
		timeout_ms = BA_TOOL_TIMEOUT_MS;
	status = run_captured(tool->applet, argv, &out, &err, timeout_ms);
	result_wrap(&sb, status, &out, &err);

	for (i = 1; i < ai; i++)
		free(argv[i]);
	free(argv);
	free(out.data);
	free(err.data);
	return sb.data;
}
