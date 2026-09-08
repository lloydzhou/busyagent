/*
 * agent_common.c - shared infrastructure for the agentutils applets.
 *
 * The block below (URL parse -> connect -> send -> header/body decode)
 * was extracted verbatim from ba_impl.c; ba_send_request() gained a
 * "method" argument. New code: ba_resp_close(), the one-shot full-body
 * client agc_http_request() and the provider-agnostic SSE splitter.
 */
//kbuild:lib-$(CONFIG_AGENTUTILS_COMMON) += agent_common.o

#include "agent_common.h"
#include "busybox.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

/* ============================================================
 * internal tuning constants (private to the HTTP core below)
 * ============================================================ */
#define BA_CONNECT_TIMEOUT_MS  5000
#define BA_MAX_HEADER          (64 * 1024)
/* one SSE field line; SSE data payloads can be much larger */
#define BA_MAX_SSE_LINE        (1024 * 1024)
#define BA_MAX_SSE_DATA        (4 * 1024 * 1024)
#define BA_MAX_HEADERS         64
#define BA_TLS_RECHDR_LEN      5     /* TLS record header (networking/tls.c) */
#define BA_TLS_APPDATA         23    /* RECORD_TYPE_APPLICATION_DATA */
/* RFC 5246: a TLSPlaintext fragment carries at most 2^14 bytes, and the
 * record layer may add up to 2^10 of compression overhead + cipher block
 * padding. 18 KiB covers every legal decrypted application-data record:
 * records larger than this are refused, never silently truncated. */
#define BA_TLS_PLAIN_MAX       (18 * 1024)
/* chunked transfer: sane upper bound for a single chunk size line value */
#define BA_MAX_CHUNK_SIZE      (16 * 1024 * 1024)
/* ba_read() polls in short slices so the cancelled flag is checked
 * promptly instead of blocking for the full idle timeout */
#define BA_POLL_SLICE_MS       250

/* ============================================================
 * URL parsing
 * ============================================================ */
int ba_parse_url(const char *url, BaUrl *u)
{
	char *colon;
	const char *p, *slash;

	if (!url || !u)
		return -1;
	{
		/* control characters anywhere in the url (path included)
		 * would smuggle line breaks into the request; reject them
		 * up front, before any part is copied into place */
		const char *c;

		for (c = url; *c; c++)
			if ((unsigned char)*c < 0x20 || *c == 0x7f)
				return -1;
	}
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
		if (strlen(slash) >= sizeof(u->path))
			return -1;
		snprintf(u->path, sizeof(u->path), "%s", slash);
	}
	colon = strchr(u->host, ':');
	if (colon) {
		const char *q = colon + 1;
		unsigned long port = 0;
		if (!*q)
			return -1;
		for (; *q; q++) {
			if (*q < '0' || *q > '9' || port > 65535 / 10)
				return -1;
			port = port * 10 + (*q - '0');
			if (port > 65535)
				return -1;
		}
		u->port = (int)port;
		*colon = '\0';
	} else {
		u->port = u->is_https ? 443 : 80;
	}
	if (!u->host[0] || u->port <= 0)
		return -1;
	if (strchr(u->host, '\r') || strchr(u->host, '\n'))
		return -1;
	return 0;
}

/* ============================================================
 * TCP connect (private)
 * ============================================================ */
/* connection state shared by the header/body decoders below; owned by
 * agc_http_request_stream() and released with ba_resp_close(). */
typedef struct {
	int fd;
	int chunked;              /* Transfer-Encoding: chunked */
	long content_length;      /* -1 if unknown */
	long body_left;           /* for content_length mode */
	long chunk_left;          /* for chunked mode */
	int chunk_state;          /* 0=size line, 1=data, 2=data CRLF, 3=trailers, 4=done */
	int eof;
	int fd_owned;
	tls_state_t *tls;
	char tls_plain[BA_TLS_PLAIN_MAX];
	int tls_plain_len;
	int tls_plain_pos;
	char hdr[BA_MAX_HEADER];
	size_t hdr_len;
	int status;
	int got_header;
	char *pending;
	size_t pending_len;
	size_t pending_cap;
volatile int *cancelled;  /* checked between poll slices */
	int read_timeout_ms;       /* 0 => BA_READ_TIMEOUT_MS */
} BaResp;

static void ba_tls_dispose(tls_state_t *tls);

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
	if (flags >= 0)
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

/* ============================================================
 * Send helpers
 * ============================================================ */
static int send_all(int fd, const char *buf, size_t len)
{
	ssize_t n;

	if (len && !buf)
		return -1;
	while (len) {
		n = safe_write(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		buf += n;
		len -= n;
	}
	return 0;
}

/* 0 on success, -1 if any send failed. The plaintext path surfaces
 * write errors (EPIPE when the peer closed, ECONNRESET...); the TLS
 * path uses xwrite(), which exits the process on failure (libbb
 * semantics), so a short TLS write never returns here. */
static int send_all_conn(tls_state_t *tls, int fd, const char *buf, size_t len);

static int ba_send_request(tls_state_t *tls, int fd, const char *method,
		    const BaUrl *u, const char **headers,
		    int header_count, const char *body, size_t body_len)
{
	char *first;
	int i;
	int rc = 0;

	if (!method || !u || header_count < 0
	 || (header_count && !headers) || (body_len && !body))
		return -1;
	/* host and path are each up to ~1 KiB: too long for a stack buffer */
	if (!method[0])
		return -1;
	for (i = 0; method[i]; i++)
		if ((unsigned char)method[i] <= 0x20 || method[i] == 0x7f
		 || strchr("()<>@,;:\\\"/[]?={}", method[i]))
			return -1;
	if (u->is_https ? u->port != 443 : u->port != 80)
		first = xasprintf("%s %s HTTP/1.1\r\n"
				  "Host: %s:%d\r\n"
				  "Content-Length: %lu\r\n"
				  "Connection: close\r\n",
				  method, u->path, u->host, u->port,
				  (unsigned long)body_len);
	else
		first = xasprintf("%s %s HTTP/1.1\r\n"
				  "Host: %s\r\n"
				  "Content-Length: %lu\r\n"
				  "Connection: close\r\n",
				  method, u->path, u->host,
				  (unsigned long)body_len);
	rc |= send_all_conn(tls, fd, first, strlen(first));
	free(first);
	for (i = 0; i < header_count; i++) {
		if (!headers[i] || strchr(headers[i], '\r') || strchr(headers[i], '\n'))
			return -1;
		rc |= send_all_conn(tls, fd, headers[i], strlen(headers[i]));
		rc |= send_all_conn(tls, fd, "\r\n", 2);
	}
	rc |= send_all_conn(tls, fd, "\r\n", 2);
	rc |= send_all_conn(tls, fd, body, body_len);
	return rc;
}

static int send_all_conn(tls_state_t *tls, int fd, const char *buf, size_t len)
{
	if (len && !buf)
		return -1;
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

/* ============================================================
 * Response reading (private)
 * ============================================================ */
/* Read more bytes into buf, honoring idle timeout and the cancelled flag.
 * Returns n>0, 0 on EOF, -1 on error/timeout/cancel. */
static int ba_read(BaResp *r, char *buf, size_t bufsz)
{
	int timeout_ms = r->read_timeout_ms > 0
		? r->read_timeout_ms : BA_READ_TIMEOUT_MS;
	int64_t deadline = (int64_t)monotonic_ms() + timeout_ms;
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
#if ENABLE_TLS
		/* buffered TLS plaintext or a whole buffered record must not
		 * wait for socket readability again (they are already here) */
		if (r->tls && (r->tls_plain_len > r->tls_plain_pos
			       || tls_has_buffered_record(r->tls)))
			break;
#endif
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
			if (give <= 0) {
				r->tls_plain_len = 0;
				r->tls_plain_pos = 0;
				return 0;
			}
			memcpy(buf, r->tls_plain + r->tls_plain_pos, give);
			r->tls_plain_pos += give;
			if (r->tls_plain_pos == r->tls_plain_len) {
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

/* locate the end of the response header block: RFC-correct CRLF CRLF,
 * with an LF LF fallback - CGI gateways (incl. busybox httpd) forward
 * script output verbatim, and shell scripts echoing plain newlines
 * produce LF-only separators. Returns NULL while incomplete. */
static char *ba_hdr_end(BaResp *r, size_t *body_off)
{
	char *end = memmem(r->hdr, r->hdr_len, "\r\n\r\n", 4);

	if (end) {
		*body_off = (end - r->hdr) + 4;
		return end;
	}
	end = memmem(r->hdr, r->hdr_len, "\n\n", 2);
	if (end) {
		*body_off = (end - r->hdr) + 2;
		return end;
	}
	return NULL;
}

/* Read and parse the response header. Returns 0 on success. */
static int ba_read_header(BaResp *r)
{
	char *p, *end;
	size_t body_start;

	while (r->hdr_len < BA_MAX_HEADER - 1) {
		char *hit;
		size_t off;

		hit = (r->hdr_len >= 2) ? ba_hdr_end(r, &off) : NULL;
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
		hit = ba_hdr_end(r, &off);
		if (hit)
			break;
	}
	end = ba_hdr_end(r, &body_start);
	if (!end)
		return -1;

	/* The read that completed the header may have consumed body bytes.
	 * Keep the complete remainder: truncating it would silently drop the
	 * beginning of an SSE event or a chunk-size line. */
	{
		size_t avail = r->hdr_len - body_start;
		if (avail) {
			r->pending = xmalloc(avail);
			memcpy(r->pending, r->hdr + body_start, avail);
			r->pending_len = avail;
			r->pending_cap = avail;
		}
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
	 * Transfer-Encoding/Content-Length were never seen. Values keep their
	 * original case (Mcp-Session-Id is case-sensitive): compare the known
	 * field names case-insensitively instead of lowercasing in place. */
	{
		char *save = NULL;
		char *line = strtok_r(r->hdr, "\r\n", &save);
		while (line) {
			if (strncasecmp(line, "Transfer-Encoding:", 18) == 0
			 && strcasestr(line, "chunked"))
				r->chunked = 1;
			else if (strncasecmp(line, "Content-Length:", 15) == 0) {
				const char *v = line + 15;
				char *endp;
				unsigned long long n;
				while (*v == ' ' || *v == '\t')
					v++;
				if (*v < '0' || *v > '9')
					return -1;
				errno = 0;
				n = bb_strtoull(v, &endp, 10);
				while (*endp == ' ' || *endp == '\t')
					endp++;
				if (errno || *endp || n > LONG_MAX)
					return -1;
				r->content_length = (long)n;
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
		if (!r->pending)
			return -1;
		memcpy(out, r->pending, n);
		r->pending_len -= n;
		if (r->pending_len)
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
	if (outsz == 0)
		return 0;
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

/* close(fd) + release TLS state; safe on a zeroed/partial BaResp. */
static void ba_resp_close(BaResp *r)
{
	if (!r)
		return;
	if (r->fd >= 0 && r->fd_owned)
		close(r->fd);
	r->fd = -1;
	if (r->tls) {
		ba_tls_dispose(r->tls);
		r->tls = NULL;
	}
	free(r->pending);
	r->pending = NULL;
	r->pending_len = 0;
	r->pending_cap = 0;
}

/* ============================================================
 * TLS helpers
 * ============================================================ */
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
 * ba_tls_notice() prints a one-time warning so the trade-off is explicit. */
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
 * still works out of the box; the connection is allowed silently (per
 * maintainer preference - point -u at a TLS-terminating gateway or a trusted
 * network for API credentials). */
static void ba_tls_notice(void)
{
}

/* ============================================================
 * One-shot full-body request (mcpc / oapi)
 * ============================================================ */
void agc_http_resp_free(AgcHttpResp *resp)
{
	if (!resp)
		return;
	free(resp->content_type);
	free(resp->session_id);
	free(resp->body);
	memset(resp, 0, sizeof(*resp));
}

/* Pick selected header values out of the already-parsed header block.
 * ba_read_header() ran strtok_r over r->hdr, which NUL-terminates each
 * token - but for CRLF headers only the '\r' is consumed (strtok_r
 * skips the '\n' without rewriting it), and LF-only headers (raw CGI
 * output) leave lone NULs. Walk defensively: skip runs of NUL/CR/LF
 * between lines instead of assuming one NUL per separator. Field names
 * match case-insensitively; values keep their original case. */
/* Extract interesting header values into resp. Must run after
 * ba_read_header(): it relies on strtok_r() having turned the header
 * separators into NUL bytes, so each line is a NUL-terminated string.
 * LF-only separators work the same way. */
static void agc_hdr_pick(BaResp *r, AgcHttpResp *resp)
{
	const char *p = r->hdr;
	const char *end = r->hdr + r->hdr_len;

	while (p < end) {
		size_t ll = strnlen(p, end - p);

		if (ll == 0) {
			p++;   /* inside a separator run */
			continue;
		}
		if (strncasecmp(p, "Content-Type:", 13) == 0 && !resp->content_type) {
			const char *v = p + 13;
			while (*v == ' ' || *v == '\t')
				v++;
			resp->content_type = xstrndup(v, strcspn(v, "\r\n"));
		} else if (strncasecmp(p, "Mcp-Session-Id:", 15) == 0
			&& !resp->session_id) {
			const char *v = p + 15;
			while (*v == ' ' || *v == '\t')
				v++;
			resp->session_id = xstrndup(v, strcspn(v, "\r\n"));
		}
		p += ll;
		while (p < end && (*p == '\0' || *p == '\r' || *p == '\n'))
			p++;
	}
}

int agc_http_request_stream(const AgcHttpReq *req,
			    agc_http_chunk_fn callback, void *ctx,
			    AgcHttpResp *resp)
{
	BaUrl u;
	BaResp r;
	tls_state_t *tls = NULL;
	int fd = -1;
	char buf[4096];
	int stopped = 0;
	int rc;

	if (!req || !resp || !req->method || !req->url)
		return AGC_HTTP_ERROR;
	memset(resp, 0, sizeof(*resp));
	if (req->header_count < 0 || req->header_count > BA_MAX_HEADERS
	 || (req->header_count && !req->headers)
	 || (req->body_len && !req->body))
		return AGC_HTTP_ERROR;
	if (req->headers) {
		int i;
		for (i = 0; i < req->header_count; i++)
			if (!req->headers[i])
				return AGC_HTTP_ERROR;
	}
	if (ba_parse_url(req->url, &u) != 0)
		return AGC_HTTP_ERROR;
#if !ENABLE_TLS
	/* belt and braces: without TLS support an https url must fail
	 * instead of silently degrading to a plaintext connection */
	if (u.is_https)
		return AGC_HTTP_ERROR;
#endif
	if (u.is_https)
		ba_tls_notice();

#if ENABLE_TLS
	if (u.is_https) {
		tls = ba_tls_connect(u.host, u.port);
		if (!tls)
			return AGC_HTTP_ERROR;
		fd = tls->ifd;
	} else
#endif
	{
		fd = ba_connect(u.host, u.port);
		if (fd < 0)
			return AGC_HTTP_ERROR;
	}

	memset(&r, 0, sizeof(r));
	r.fd = fd;
	r.fd_owned = 1;
	r.tls = tls;
	r.cancelled = req->cancelled;
	r.read_timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : BA_READ_TIMEOUT_MS;
	if (ba_send_request(tls, fd, req->method, &u, req->headers,
			    req->header_count, req->body ? req->body : "",
			    req->body_len) != 0)
		goto fail;
	if (ba_read_header(&r) != 0)
		goto fail;
	resp->status = r.status;
	agc_hdr_pick(&r, resp);

	for (;;) {
		int n = ba_body_read(&r, buf, sizeof(buf));
		int cb;
		if (n < 0)
			goto fail;
		if (n == 0)
			break;
		if (!callback)
			continue;
		cb = callback(ctx, buf, (size_t)n);
		if (cb < 0)
			goto fail;
		if (cb > 0) {
			stopped = 1;
			break;
		}
	}
	ba_resp_close(&r);
	rc = stopped ? AGC_HTTP_STOPPED : AGC_HTTP_COMPLETE;
	return rc;

fail:
	ba_resp_close(&r);
	agc_http_resp_free(resp);
	return AGC_HTTP_ERROR;
}

static int agc_collect_body(void *ctx, const char *data, size_t len)
{
	StrBuf *sb = ctx;

	if (sb->len > BA_MAX_BODY || len > BA_MAX_BODY - sb->len)
		return AGC_HTTP_ERROR;
	sb_appendn(sb, data, len);
	return AGC_HTTP_COMPLETE;
}

int agc_http_request(const AgcHttpReq *req, AgcHttpResp *resp)
{
	StrBuf bodybuf;
	int rc;

	sb_init(&bodybuf);
	rc = agc_http_request_stream(req, agc_collect_body, &bodybuf, resp);
	if (rc < 0) {
		sb_free(&bodybuf);
		return AGC_HTTP_ERROR;
	}
	resp->body = bodybuf.data ? bodybuf.data : xstrdup("");
	resp->body_len = bodybuf.len;
	return rc;
}

/* ============================================================
 * Generic SSE line splitter
 * ============================================================ */
void agc_sse_init(AgcSse *s)
{
	memset(s, 0, sizeof(*s));
}

void agc_sse_free(AgcSse *s)
{
	free(s->line);
	free(s->event);
	free(s->data);
	memset(s, 0, sizeof(*s));
}

static void agc_sse_line_putc(AgcSse *s, char c)
{
	if (s->error)
		return;
	if (s->line_len >= BA_MAX_SSE_LINE) {
		s->error = 1;
		return;
	}
	if (s->line_len + 2 > s->line_cap) {
		size_t cap = s->line_cap ? s->line_cap * 2 : 256;
		if (cap > BA_MAX_SSE_LINE + 1)
			cap = BA_MAX_SSE_LINE + 1;
		s->line_cap = cap;
		s->line = xrealloc(s->line, s->line_cap);
	}
	s->line[s->line_len++] = c;
	s->line[s->line_len] = '\0';
}

/* append one data-line payload; join multiple data: lines with '\n' */
static void agc_sse_data_append(AgcSse *s, const char *str, size_t n)
{
	int need_nl = (s->data_len > 0);

	if (s->error || n > BA_MAX_SSE_DATA
	 || (need_nl && s->data_len > BA_MAX_SSE_DATA - 1)
	 || s->data_len + (need_nl ? 1 : 0) > BA_MAX_SSE_DATA - n) {
		s->error = 1;
		return;
	}
	if (s->data_len + n + (need_nl ? 1 : 0) + 1 > s->data_cap) {
		s->data_cap = s->data_cap ? s->data_cap : 256;
		while (s->data_len + n + (need_nl ? 1 : 0) + 1 > s->data_cap) {
			if (s->data_cap > (BA_MAX_SSE_DATA + 1) / 2) {
				s->data_cap = BA_MAX_SSE_DATA + 1;
				break;
			}
			s->data_cap *= 2;
		}
		s->data = xrealloc(s->data, s->data_cap);
	}
	if (need_nl)
		s->data[s->data_len++] = '\n';
	memcpy(s->data + s->data_len, str, n);
	s->data_len += n;
	s->data[s->data_len] = '\0';
}

/* one complete field line was accumulated: fold it into the event */
static void agc_sse_field(AgcSse *s)
{
	const char *line = s->line;

	if (s->error)
		return;
	s->line[s->line_len] = '\0';

	if (line[0] == ':') {
		/* comment line: part of the event, carries no data */
		s->has_field = 1;
		s->saw_sse = 1;
	} else if (strncmp(line, "event:", 6) == 0) {
		const char *v = line + 6;
		while (*v == ' ' || *v == '\t')
			v++;
		free(s->event);
		s->event = xstrdup(v);
		s->has_field = 1;
		s->saw_sse = 1;
	} else if (strncmp(line, "data:", 5) == 0) {
		const char *v = line + 5;
		while (*v == ' ' || *v == '\t')
			v++;
		agc_sse_data_append(s, v, strlen(v));
		s->has_field = 1;
		s->saw_sse = 1;
	} else if (strncmp(line, "id:", 3) == 0
		|| strncmp(line, "retry:", 6) == 0) {
		s->has_field = 1;   /* recognized, ignored */
		s->saw_sse = 1;
	}
	/* unknown field names: ignored, do not mark the event */
	s->line_len = 0;
}

/* empty line: the event (if any fields were seen) is complete */
static void agc_sse_dispatch(AgcSse *s, agc_sse_event_fn fn, void *ctx)
{
	if (!s->has_field)
		return;
	if (fn)
		fn(ctx, s->event ? s->event : "message",
		   s->data ? s->data : "", s->data_len);
	free(s->event);
	s->event = NULL;
	free(s->data);
	s->data = NULL;
	s->data_len = 0;
	s->data_cap = 0;
	s->has_field = 0;
	/* saw_sse stays set: it marks "this stream speaks SSE" so callers
	 * can tell an event-stream apart from a plain JSON body even after
	 * the last event was dispatched */
}

void agc_sse_feed(AgcSse *s, const char *ptr, size_t len,
		  agc_sse_event_fn fn, void *ctx)
{
	size_t i;

	if (s->error)
		return;
	for (i = 0; i < len; i++) {
		char c = ptr[i];

		if (s->skip_lf) {
			s->skip_lf = 0;
			if (c == '\n')
				continue;
		}
		if (c == '\r' || c == '\n') {
			if (s->line_len > 0)
				agc_sse_field(s);
			else
				agc_sse_dispatch(s, fn, ctx);
			if (c == '\r')
				s->skip_lf = 1;
		} else {
			agc_sse_line_putc(s, c);
		}
		if (s->error)
			return;
	}
}

void agc_sse_finish(AgcSse *s, agc_sse_event_fn fn, void *ctx)
{
	if (s->error)
		return;
	if (s->line_len > 0)
		agc_sse_field(s);
	if (s->error)
		return;   /* the final field may still overflow */
	agc_sse_dispatch(s, fn, ctx);
}

/* ============================================================
 * StrBuf / utils / JSON parser (moved from ba_impl.c)
 * ============================================================ */
/* ============================================================
 * StrBuf - growable string buffer
 * ============================================================ */

static void skip_ws(const char *src, size_t *pos);

/* nesting limit: keeps the recursive-descent parser and the printer
 * away from stack overflow on adversarial input like 50000 nested
 * arrays */
#define JSON_MAX_DEPTH 1024

/* Print one JSON value (jq-style) into sb.
 * indent >= 0: pretty form with 2-space indentation.
 * indent < 0: compact single-line form (whitespace dropped, strings
 * kept intact - not a source-slice echo, so multi-line input still
 * comes out on one line). */
static void agc_json_print_depth(StrBuf *sb, JsonVal v, int indent, int depth);

void agc_json_print(StrBuf *sb, JsonVal v, int indent)
{
	agc_json_print_depth(sb, v, indent, 0);
}

static void agc_json_print_depth(StrBuf *sb, JsonVal v, int indent, int depth)
{
	int child = indent < 0 ? indent : indent + 1;

	if (depth > JSON_MAX_DEPTH) {
		/* parsed values are already bounded by the parser's
		 * limit, so this only guards hand-built trees */
		sb_append(sb, "null");
		return;
	}
	switch (v.type) {
	case JSON_OBJECT: {
		JsonObjectIter it;
		int n = 0;

		sb_append(sb, "{");
		json_obj_iter_init(&it, v);
		while (json_obj_iter_next(&it)) {
			sb_append(sb, n++ ? (indent < 0 ? "," : ",\n")
					  : (indent < 0 ? "" : "\n"));
			if (indent >= 0) {
				int k;

				for (k = 0; k <= indent; k++)
					sb_append(sb, "  ");
			}
			sb_append_json_string(sb, it.key);
			sb_append(sb, indent < 0 ? ":" : ": ");
			agc_json_print_depth(sb, it.val, child, depth + 1);
		}
		json_obj_iter_cleanup(&it);
		if (n && indent >= 0) {
			int k;

			sb_append(sb, "\n");
			for (k = 0; k < indent; k++)
				sb_append(sb, "  ");
		}
		sb_append(sb, "}");
		break;
	}
	case JSON_ARRAY: {
		const char *src = v.src;
		size_t pos = v.start + 1;	/* skip [ */
		int n = 0;

		sb_append(sb, "[");
		skip_ws(src, &pos);
		if (src[pos] != ']') {
			for (;;) {
				JsonParse vp;

				skip_ws(src, &pos);
				vp = json_parse(src, &pos);
				if (vp.error)
					break;
				sb_append(sb, n++ ? (indent < 0 ? "," : ",\n")
						  : (indent < 0 ? "" : "\n"));
				if (indent >= 0) {
					int k;

					for (k = 0; k <= indent; k++)
						sb_append(sb, "  ");
				}
				agc_json_print_depth(sb, vp.val, child, depth + 1);
				skip_ws(src, &pos);
				if (src[pos] == ',') {
					pos++;
					continue;
				}
				break;
			}
		}
		if (n && indent >= 0) {
			int k;

			sb_append(sb, "\n");
			for (k = 0; k < indent; k++)
				sb_append(sb, "  ");
		}
		sb_append(sb, "]");
		break;
	}
	case JSON_STRING:
		sb_appendn(sb, v.src + v.start, v.end - v.start);
		break;
	case JSON_NUMBER:
		sb_appendn(sb, v.src + v.start, v.end - v.start);
		break;
	case JSON_BOOL:
		sb_append(sb, json_bool_val(v) ? "true" : "false");
		break;
	default:
		sb_append(sb, "null");
		break;
	}
}

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
    size_t need;
    size_t newcap;

    if (extra > SIZE_MAX - sb->len - 1)
        bb_error_msg_and_die("string buffer too large");
    need = sb->len + extra + 1;
    if (need <= sb->cap) return;
    newcap = sb->cap ? sb->cap : 256;
    while (newcap < need) {
        if (newcap > SIZE_MAX / 2) {
            newcap = need;
            break;
        }
        newcap *= 2;
    }
    {
        char *p = realloc(sb->data, newcap);
        if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
        sb->data = p;
        sb->cap = newcap;
    }
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
static JsonParse json_parse_internal(const char *src, size_t *pos, int depth);

/* parse a JSON array */
static JsonParse parse_array(const char *src, size_t *pos, int depth) {
    size_t start = *pos;
    (*pos)++; /* skip [ */
    skip_ws(src, pos);
    if (src[*pos] == ']') { (*pos)++; return make_val(JSON_ARRAY, src, start, *pos); }
    for (;;) {
        skip_ws(src, pos);
        JsonParse vp = json_parse_internal(src, pos, depth + 1);
        if (vp.error) return vp;
        skip_ws(src, pos);
        if (src[*pos] == ',') { (*pos)++; continue; }
        if (src[*pos] == ']') { (*pos)++; break; }
        return make_err("expected ',' or ']'");
    }
    return make_val(JSON_ARRAY, src, start, *pos);
}

/* parse a JSON object */
static JsonParse parse_object(const char *src, size_t *pos, int depth) {
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
        JsonParse vp = json_parse_internal(src, pos, depth + 1);
        if (vp.error) return vp;
        skip_ws(src, pos);
        if (src[*pos] == ',') { (*pos)++; continue; }
        if (src[*pos] == '}') { (*pos)++; break; }
        return make_err("expected ',' or '}'");
    }
    return make_val(JSON_OBJECT, src, start, *pos);
}

/* parse a JSON value */
static JsonParse json_parse_internal(const char *src, size_t *pos, int depth) {
    skip_ws(src, pos);
    if (depth > JSON_MAX_DEPTH)
        return make_err("nesting too deep");
    char c = src[*pos];
    if (c == '"') return parse_string(src, pos);
    if (c == '{') return parse_object(src, pos, depth);
    if (c == '[') return parse_array(src, pos, depth);
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
    return json_parse_internal(src, pos, 0);
}

JsonParse json_parse_root(const char *src) {
    if (!src) return make_err("null input");
    size_t pos = 0;
    JsonParse p = json_parse_internal(src, &pos, 0);
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
        /* compare the decoded key: a literal "a\u0062" in the source
         * names the same member as "ab" */
        size_t klen;
        bool match;
        {
            char *kdec = json_string_val(kp.val);

            if (!kdec)
                break;
            klen = strlen(kdec);
            match = (strlen(key) == klen
                     && memcmp(key, kdec, klen) == 0);
            free(kdec);
        }
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

/* decode the escape sequences of the raw JSON string span
 * src[start,end) into buf; returns the decoded length. buf needs room
 * for (end - start) + 1 bytes: every escape decodes to no more bytes
 * than it occupies in the source. */
static size_t json_decode_raw(const char *src, size_t start, size_t end,
			      char *buf)
{
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
    return out;
}

/* decode a JSON_STRING into a caller-provided buffer; returns the
 * decoded length (embedded NULs are preserved) and NUL-terminates */
size_t json_string_decode(JsonVal v, char *buf)
{
    if (v.type != JSON_STRING) {
        buf[0] = '\0';
        return 0;
    }
    size_t out = json_decode_raw(v.src, v.start + 1, v.end - 1, buf);
    buf[out] = '\0';
    return out;
}

char *json_string_val(JsonVal v) {
    if (v.type != JSON_STRING) return NULL;
    size_t len = (v.end - 1) - (v.start + 1);
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    json_string_decode(v, buf);
    return buf;
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
    /* decoded key: escapes are resolved once here, so printers and
     * lookups see the actual member name */
    char *key = json_string_val(kp.val);
    if (!key)
        return false;
    it->key = key;
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

/* ============================================================
 * jq-style path evaluation
 * ============================================================ */

/* one parsed path segment */
typedef struct {
	char name[128];   /* ".name" segment key */
	int has_name;
	long index;       /* [N] */
	int is_index;
	int spread;       /* [] */
} AgcJqSeg;

/* parse ".a.b[0].c[]" / "a.b[].c" into segments; -1 on syntax error */
static int agc_jq_parse(const char *path, AgcJqSeg *segs, int *n_segs)
{
	const char *p = path;
	int n = 0;

	if (*p == '.')
		p++;
	if (!*p) {
		/* "." alone selects the root */
		*n_segs = 0;
		return 0;
	}
	while (*p) {
		AgcJqSeg *s;

		if (n >= 16)
			return -1;
		if (*p == '[') {
			p++;
			s = &segs[n++];
			memset(s, 0, sizeof(*s));
			if (*p == ']') {
				s->spread = 1;
				p++;
			} else {
				char *end;

				s->is_index = 1;
				s->index = strtol(p, &end, 10);
				if (end == p || *end != ']')
					return -1;
				p = end + 1;
			}
			if (*p == '.')
				p++;
			else if (*p && *p != '[')
				return -1;
			continue;
		}
		{
			size_t len = 0;

			s = &segs[n++];
			memset(s, 0, sizeof(*s));
			while (p[len] && p[len] != '.' && p[len] != '[')
				len++;
			if (!len || len >= sizeof(s->name))
				return -1;
			memcpy(s->name, p, len);
			s->name[len] = '\0';
			s->has_name = 1;
			p += len;
			if (*p == '.')
				p++;
			else if (*p && *p != '[')
				return -1;
		}
	}
	*n_segs = n;
	return 0;
}

static int agc_jq_push(JsonVal **v, int *n, int *cap, JsonVal value)
{
	if (*n == *cap) {
		int next = *cap ? *cap * 2 : AGC_JQ_INITIAL_MATCHES;
		if (next < *cap || next > 1048576)
			return -1;
		*v = xrealloc(*v, (size_t)next * sizeof((*v)[0]));
		*cap = next;
	}
	(*v)[(*n)++] = value;
	return 0;
}

int agc_json_path(JsonVal root, const char *path, AgcJqMatches *m)
{
	AgcJqSeg segs[16];
	int n_segs;
	JsonVal *work = NULL;
	int n_work = 0;
	int cap_work = 0;
	int i;

	memset(m, 0, sizeof(*m));
	if (agc_jq_parse(path, segs, &n_segs) != 0
		|| agc_jq_push(&work, &n_work, &cap_work, root) != 0)
		goto fail;
	for (i = 0; i < n_segs; i++) {
		AgcJqSeg *s = &segs[i];
		JsonVal *next = NULL;
		int n_next = 0;
		int cap_next = 0;
		int k;

		for (k = 0; k < n_work; k++) {
			JsonVal v = work[k];

			if (s->has_name) {
				if (agc_jq_push(&next, &n_next, &cap_next,
						json_get(v, s->name)) != 0)
					goto step_fail;
			} else if (s->is_index) {
				if (agc_jq_push(&next, &n_next, &cap_next,
						json_array_get(v, (int)s->index)) != 0)
					goto step_fail;
			} else {
				int j;
				int len = v.type == JSON_ARRAY ? json_array_len(v) : 0;
				for (j = 0; j < len; j++)
					if (agc_jq_push(&next, &n_next, &cap_next,
							json_array_get(v, j)) != 0)
						goto step_fail;
			}
		}
		free(work);
		work = next;
		n_work = n_next;
		cap_work = cap_next;
		if (!n_work)
			break;
		continue;
step_fail:
		free(next);
		goto fail;
	}
	m->v = work;
	m->n = n_work;
	m->cap = cap_work;
	return 0;
fail:
	free(work);
	memset(m, 0, sizeof(*m));
	return -1;
}
