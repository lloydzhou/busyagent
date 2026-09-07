/*
 * agent_common.h - infrastructure shared by the agentutils applets
 * (busyagent, mcpc, oapi): URL parsing, TCP/TLS connect, a generic
 * HTTP/1.1 client (any method, full-body reads, chunked decoding) and
 * a provider-agnostic SSE line splitter.
 *
 * The network layer was extracted verbatim from ba_impl.c (busyagent);
 * ba_send_request() gained a "method" parameter, everything else kept
 * its name and behavior so busyagent's SSE pump is unchanged.
 */
#ifndef AGENT_COMMON_H
#define AGENT_COMMON_H

#include "libbb.h"

/* ---- tuning constants (shared with the original ba_impl.c code) ---- */
#define BA_CONNECT_TIMEOUT_MS  5000
#define BA_READ_TIMEOUT_MS    300000   /* idle timeout for SSE streams */
#define BA_MAX_HEADER         (64 * 1024)
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

/* strdup, NULL-safe */
char *util_strdup(const char *s);

/* UTF-8 sanitize: invalid bytes become the \ufffd literal, malloc'd */
char *util_sanitize_utf8(const char *src);
/* getenv with a default */
const char *util_env(const char *name, const char *defval);
/* current timestamp string (ISO 8601) */
char *util_timestamp_now(void);
/* parse a number with k/m/g suffixes (util_parse_size parity) */
long util_parse_size(const char *s);
/* current epoch seconds */
long util_epoch_seconds(void);
/* count UTF-8 characters (approximate token counting) */
int util_utf8_char_count(const char *s);
/* largest offset <= max_bytes that never splits a UTF-8 char */
size_t util_utf8_truncate_len(const char *s, size_t max_bytes);
/* truncate in place to max_total bytes (UTF-8 safe), append "..." */
void util_truncate_str(char *s, size_t max_total);


/* growable string buffer */
typedef struct {
    char *data;
    size_t len;      /* current length (excluding '\0') */
    size_t cap;      /* buffer capacity */
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_ensure(StrBuf *sb, size_t extra);
void sb_append(StrBuf *sb, const char *s);
void sb_appendn(StrBuf *sb, const char *s, size_t n);
void sb_appendf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void sb_append_char(StrBuf *sb, char c);
/* truncate to a given length */
void sb_truncate(StrBuf *sb, size_t len);

/* JSON-escape src and append to sb */
void sb_append_json_string(StrBuf *sb, const char *src);
/* shell-quote src as one argument and append to sb */
void sb_append_shell_arg(StrBuf *sb, const char *src);

/* session id: YYYYMMDD-HHMMSS-XXXX */
char *util_new_session_id(void);

/* join paths a/b (handles trailing/leading slashes) */
char *util_path_join(const char *a, const char *b);

/* ensure a directory exists (recursive, mkdir -p) */
int util_mkdirs(const char *path, int mode);

/* home directory path */
const char *util_home_dir(void);

/* strdup, NULL-safe */
char *util_strdup(const char *s);

/* free + NULL */
#define FREE_PTR(p) do { free(p); (p) = NULL; } while(0)

/* getenv with a default */
const char *util_env(const char *name, const char *defval);

/* current timestamp string (ISO 8601) */
char *util_timestamp_now(void);

/* parse a number with k/m/g suffixes (util_parse_size parity) */
long util_parse_size(const char *s);

/* current epoch seconds */
long util_epoch_seconds(void);

/* count UTF-8 characters (approximate token counting) */
int util_utf8_char_count(const char *s);

/* largest offset <= max_bytes that never splits a UTF-8 char */
size_t util_utf8_truncate_len(const char *s, size_t max_bytes);

/* truncate in place to max_total bytes (UTF-8 safe), append "..." */
void util_truncate_str(char *s, size_t max_total);

/* truncate in place to max_chars UTF-8 chars, append "..." */
void util_truncate_chars(char *s, int max_chars);

/* UTF-8 sanitize: invalid bytes become the \ufffd literal, malloc'd */
char *util_sanitize_utf8(const char *src);

/* trim trailing whitespace */
char *util_rtrim(char *s);

/* read a whole file into a string */
char *util_read_file(const char *path);

/* write a whole file */
int util_write_file(const char *path, const char *content);


/* ==== ba_json.h ==== */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdbool.h>

/*
 * lightweight JSON parser - what Claude/OpenAI responses and
 *
 * design:
 *   - single pass, zero copy (values point into the original text)
 *   - json_get_string etc. return malloc'd copies
 *   - objects, arrays, strings, numbers, booleans, null
 *   - no serialization (assemble via StrBuf instead)
 */

/* JSON value types */
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

/* JSON value - a view into the original JSON text */
typedef struct {
    JsonType type;
    const char *src;        /* the original JSON string */
    size_t start;           /* value start offset in src */
    size_t end;             /* value end offset in src (exclusive) */
    /* STRING: src+start..src+end is the raw value (with quotes) */
    /* OBJECT/ARRAY: src+start..src+end is the whole structure */
} JsonVal;

/* parse result */
typedef struct {
    JsonVal val;            /* parsed value */
    const char *error;      /* error message, NULL on success */
} JsonParse;

/* ============================================================
 * parsing
 * ============================================================ */

/* parse a JSON value from src+pos; updates pos */
JsonParse json_parse(const char *src, size_t *pos);

/* parse a complete JSON string (from the root) */
JsonParse json_parse_root(const char *src);

/* ============================================================
 * queries - extract fields from an OBJECT
 * ============================================================ */

/* value for key, or type=JSON_NULL when absent */
JsonVal json_get(JsonVal obj, const char *key);

/* string value (malloc'd copy, NULL when absent) */
char *json_get_string(JsonVal obj, const char *key);

/* integer value */
int json_get_int(JsonVal obj, const char *key);

/* long long value (large token counts) */
long long json_get_ll(JsonVal obj, const char *key);

/* double value */
double json_get_double(JsonVal obj, const char *key);

/* boolean value (def when absent) */
bool json_get_bool(JsonVal obj, const char *key, bool def);

/* ============================================================
 * array operations
 * ============================================================ */

/* array length */
int json_array_len(JsonVal arr);

/* i-th element (JSON_NULL when out of range) */
JsonVal json_array_get(JsonVal arr, int index);

/* ============================================================
 * value extraction
 * ============================================================ */

/* decoded string from a JSON_STRING (malloc'd copy) */
char *json_string_val(JsonVal v);

/* double from a JSON_NUMBER */
double json_number_val(JsonVal v);

/* bool from a JSON_BOOL */
bool json_bool_val(JsonVal v);

/* generic: decode when v is a string, else NULL */
char *json_as_string(JsonVal v);

/* ============================================================
 * iterate OBJECT key/value pairs
 * ============================================================ */

typedef struct {
    const char *key;        /* key (malloc'd; _next frees the previous) */
    JsonVal val;            /* value */
    /* internal state */
    const char *src;
    size_t pos;
    bool first;
} JsonObjectIter;

void json_obj_iter_init(JsonObjectIter *it, JsonVal obj);
bool json_obj_iter_next(JsonObjectIter *it);
void json_obj_iter_cleanup(JsonObjectIter *it);  /* call to break out early */

/* ============================================================
 * JSON Lines appending
 * ============================================================ */

/* append one line to a JSONL file (adds \n) */
int jsonl_append(const char *path, const char *json_line);

#endif /* JSON_H */

/* ============================================================
 * URL parsing
 * ============================================================ */
typedef struct {
	char host[256];
	int port;
	int is_https;
	char path[1024];
} BaUrl;

/* "http(s)://host[:port]/path" -> BaUrl. 0 on success. */
int ba_parse_url(const char *url, BaUrl *u);

/* ============================================================
 * Connect / TLS
 * ============================================================ */
/* TCP connect with a bounded non-blocking handshake. fd or -1. */
int ba_connect(const char *host, int port);

/* In-tree TLS session (networking/tls.c). The daemon/client owns it and
 * releases it with ba_tls_dispose(). See ba_tls_notice(): no cert check. */
tls_state_t *ba_tls_connect(const char *host, int port);
void ba_tls_dispose(tls_state_t *tls);
/* one-time stderr warning about the unverified TLS client */
void ba_tls_notice(void);

/* ============================================================
 * Request send (any method)
 * ============================================================ */
/* Send "<method> <path> HTTP/1.1" + Host + Content-Length + headers
 * ("Name: value" lines, no trailing CRLF) + body. 0 on success. */
int ba_send_request(tls_state_t *tls, int fd, const char *method,
		    const BaUrl *u, const char **headers, int header_count,
		    const char *body, size_t body_len);

/* write buf to the TLS session (chunked through the record layer) or
 * the plain socket; 0 on success (-1 only on plain-socket errors:
 * the TLS path uses libbb xwrite semantics). */
int send_all_conn(tls_state_t *tls, int fd, const char *buf, size_t len);

/* ============================================================
 * Response reading (header + decoded body, chunked-aware)
 * ============================================================ */
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

/* Read + parse the response header into r (status, chunked, lengths).
 * 0 on success. */
int ba_read_header(BaResp *r);

/* Read the next piece of the decoded body. n>0 data, 0 end of body,
 * -1 error/timeout/cancel. Handles content-length and chunked. */
int ba_body_read(BaResp *r, char *out, size_t outsz);

/* close(fd) + ba_tls_dispose(tls); safe on partial/zeroed BaResp. */
void ba_resp_close(BaResp *r);

/* ============================================================
 * One-shot request with full-body response (mcpc / oapi)
 * ============================================================ */
typedef struct {
	int status;           /* HTTP status code (set even for 4xx/5xx) */
	char *content_type;   /* lowercased value or NULL */
	char *location;       /* value of location: or NULL */
	char *session_id;     /* value of mcp-session-id: or NULL (MCP) */
	char *body;           /* NUL-terminated, malloc'd (may be NULL) */
	size_t body_len;
} AgcHttpResp;

/* Connect, send, read the whole response body into memory.
 * No retries, no redirects. Returns 0 when a complete response was
 * received (resp->status carries 4xx/5xx too), -1 on transport error
 * (connect/send/header parse/body error). Caller frees with
 * agc_http_resp_free(). */
int agc_http_request(const char *method, const char *url,
		     const char **headers, int header_count,
		     const char *body, size_t body_len,
		     AgcHttpResp *resp);
void agc_http_resp_free(AgcHttpResp *resp);

/* ============================================================
 * Generic SSE line splitter (mcpc Streamable-HTTP responses)
 * ============================================================ */
typedef struct {
	char *line;        /* accumulating field line (malloc'd) */
	size_t line_len;
	size_t line_cap;
	char *event;       /* event: value of the current event (malloc'd) */
	char *data;        /* data: lines joined with '\n' (malloc'd) */
	size_t data_len;
	size_t data_cap;
	int has_field;     /* any field line seen in the current event */
} AgcSse;

/* called once per complete event; event defaults to "message"; data is
 * NUL-terminated but data_len is authoritative */
typedef void (*agc_sse_event_fn)(void *ctx, const char *event,
				 const char *data, size_t data_len);

void agc_sse_init(AgcSse *s);
void agc_sse_free(AgcSse *s);
/* feed body bytes; dispatches complete events as they close.
 * Returns 0 when cancelled (s->cancelled semantics belong to the caller:
 * this splitter never blocks, so no cancel flag is needed here). */
void agc_sse_feed(AgcSse *s, const char *ptr, size_t len,
		  agc_sse_event_fn fn, void *ctx);
/* dispatch a trailing unterminated event at EOF (usually nothing). */
void agc_sse_finish(AgcSse *s, agc_sse_event_fn fn, void *ctx);

/* ============================================================
 * jq-style path evaluation (jq applet, oapi --jq)
 * ============================================================ */
#define AGC_JQ_MAX_MATCHES 64

typedef struct {
	JsonVal v[AGC_JQ_MAX_MATCHES];
	int n;
} AgcJqMatches;

/* Evaluate a jq-style path against root: ".a.b[0].c", ".items[].id",
 * ".[]", "items[].name" (leading dot optional). Missing keys and
 * out-of-range indices yield a JSON_NULL entry (printed as "null");
 * "[]" over a non-array yields nothing. Returns 0 on success, -1 on a
 * syntax error. */
int agc_json_path(JsonVal root, const char *path, AgcJqMatches *m);

#endif /* AGENT_COMMON_H */
