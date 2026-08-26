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
#include "libbb.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "ba_transport.h"

#define BA_CONNECT_TIMEOUT_MS  5000
#define BA_READ_TIMEOUT_MS    300000   /* idle timeout for SSE streams */
#define BA_MAX_HEADER         (64 * 1024)
#define BA_MAX_RETRIES        2
#define BA_RETRY_MAX_TIME_MS  20000

typedef struct {
	char host[256];
	int port;
	char path[1024];
} BaUrl;

static int ba_parse_url(const char *url, BaUrl *u)
{
	char *colon;
	const char *p, *slash;

	memset(u, 0, sizeof(*u));
	if (strncmp(url, "http://", 7) != 0)
		return -1;
	p = url + 7;
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
		u->port = 80;
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

	lsa = xhost2sockaddr(host, port);
	fd = xsocket(lsa->u.sa.sa_family, SOCK_STREAM, 0);
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

static int send_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = send(fd, buf + off, len - off, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += n;
	}
	return 0;
}

typedef struct {
	int fd;
	int chunked;              /* Transfer-Encoding: chunked */
	long content_length;      /* -1 if unknown */
	long body_left;           /* for content_length mode */
	long chunk_left;          /* for chunked mode */
	int chunk_state;          /* 0=hex line, 1=data, 2=data CRLF, 3=done */
	int eof;
	char hdr[BA_MAX_HEADER];
	size_t hdr_len;
	int status;
	int got_header;
	char pending[4096];
	size_t pending_len;
} BaResp;

/* Read more bytes into buf, honoring idle timeout. Returns n>0, 0 on EOF, -1 on error/timeout. */
static int ba_read(BaResp *r, char *buf, size_t bufsz)
{
	struct pollfd pfd;
	int n;

	pfd.fd = r->fd;
	pfd.events = POLLIN;
	for (;;) {
		if (safe_poll(&pfd, 1, BA_READ_TIMEOUT_MS) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
			return -1;   /* timeout */
		break;
	}
	n = safe_read(r->fd, buf, bufsz);
	return n;   /* 0 = EOF */
}

/* Read and parse the response header. Returns 0 on success. */
static int ba_read_header(BaResp *r)
{
	char *p, *end, *save;
	size_t idx;

	while (r->hdr_len < BA_MAX_HEADER - 1) {
		char *hit;
		size_t have = r->hdr_len;

		hit = memmem(r->hdr, have, "\r\n\r\n", 4);
		if (have >= 4 && hit)
			break;
		/* also handle leading partial */
		{
			int n = ba_read(r, r->hdr + r->hdr_len, 4096);
			if (n <= 0)
				return -1;
			r->hdr_len += n;
			r->hdr[r->hdr_len] = '\0';
		}
		hit = memmem(r->hdr, r->hdr_len, "\r\n\r\n", 4);
		if (hit)
			break;
		(void)hit; (void)have;
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

	for (idx = 0, save = NULL; ; idx++, save = NULL) {
		char *line = strtok_r(idx == 0 ? r->hdr : NULL, "\r\n", &save);
		if (!line)
			break;
		str_tolower(line);   /* libbb in-place lowercase */
		if (strncmp(line, "transfer-encoding:", 18) == 0
		 && strstr(line, "chunked"))
			r->chunked = 1;
		else if (strncmp(line, "content-length:", 15) == 0)
			r->content_length = atol(line + 15);
	}
	if (r->chunked) {
		r->chunk_state = 0;
		r->chunk_left = 0;
	} else {
		r->body_left = r->content_length;
	}
	return 0;
}

/* Decode next piece of body into out (already de-chunked).
 * Returns n>0 data, 0 end of body, -1 error. */
static int ba_body_read(BaResp *r, char *out, size_t outsz)
{
	if (r->pending_len > 0) {
		size_t n = r->pending_len < outsz ? r->pending_len : outsz;
		memcpy(out, r->pending, n);
		r->pending_len -= n;
		memmove(r->pending, r->pending + n, r->pending_len);
		return n;
	}

	if (r->eof)
		return 0;

	if (!r->chunked) {
		int n;
		size_t want = outsz;
		if (r->content_length >= 0) {
			if (r->body_left <= 0)
				return 0;
			if (want > (size_t)r->body_left)
				want = r->body_left;
		}
		n = ba_read(r, out, want);
		if (n < 0)
			return -1;
		if (n == 0) {
			r->eof = 1;
			return 0;
		}
		if (r->content_length >= 0)
			r->body_left -= n;
		return n;
	}

	/* chunked: state machine over a small scratch window */
	for (;;) {
		if (r->chunk_state == 3)
			return 0;   /* done */
		if (r->chunk_state == 1) {
			int n = ba_read(r, out, r->chunk_left < (long)outsz ? r->chunk_left : outsz);
			if (n < 0)
				return -1;
			if (n == 0)
				return -1;   /* unexpected EOF mid-chunk */
			r->chunk_left -= n;
			if (r->chunk_left == 0)
				r->chunk_state = 2;
			return n;
		}
		if (r->chunk_state == 0 || r->chunk_state == 2) {
			/* read a line: hex size or trailing CRLF / trailer */
			char line[128];
			size_t ln = 0;
			int c;
			for (;;) {
				char ch;
				c = ba_read(r, &ch, 1);
				if (c <= 0)
					return -1;
				if (ch == '\n')
					break;
				if (ln < sizeof(line) - 1 && ch != '\r')
					line[ln++] = ch;
			}
			line[ln] = '\0';
			if (r->chunk_state == 2) {
				if (ln == 0)
					r->chunk_state = 3;   /* final CRLF after last chunk */
				/* else: trailer header line, keep consuming */
				continue;
			}
			/* state 0: hex size (possibly with ;ext) */
			{
				char *semi = strchr(line, ';');
				if (semi)
					*semi = '\0';
			}
			r->chunk_left = strtol(line, NULL, 16);
			if (r->chunk_left == 0) {
				r->chunk_state = 2;   /* last chunk; consume trailers + CRLF */
				continue;
			}
			r->chunk_state = 1;
			continue;
		}
	}
}

static void ba_send_request(int fd, const BaUrl *u, const char **headers,
			    int header_count, const char *body, size_t body_len)
{
	char first[512];
	int i;

	snprintf(first, sizeof(first),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Length: %lu\r\n"
		"Connection: close\r\n",
		u->path, u->host, (unsigned long)body_len);
	send_all(fd, first, strlen(first));
	for (i = 0; i < header_count; i++) {
		send_all(fd, headers[i], strlen(headers[i]));
		send_all(fd, "\r\n", 2);
	}
	send_all(fd, "\r\n", 2);
	send_all(fd, body, body_len);
}

void http_response_free(HttpResponse *r)
{
	if (!r)
		return;
	free(r->body);
	r->body = NULL;
}

/* Synchronous POST: whole body. */
HttpResponse http_post(const char *url, const char **headers, int header_count,
		       const char *body, size_t body_len)
{
	HttpResponse resp;
	BaUrl u;
	BaResp r;
	char buf[4096];
	size_t cap = 0, len = 0;

	memset(&resp, 0, sizeof(resp));
	resp.body = malloc(1);
	if (resp.body)
		resp.body[0] = '\0';

	if (ba_parse_url(url, &u) != 0) {
		resp.status_code = 0;
		free(resp.body);
		resp.body = xstrdup("bad url");
		return resp;
	}

	memset(&r, 0, sizeof(r));
	r.fd = ba_connect(u.host, u.port);
	if (r.fd < 0) {
		resp.status_code = 0;
		free(resp.body);
		resp.body = xstrdup("connect failed");
		return resp;
	}
	ba_send_request(r.fd, &u, headers, header_count, body, body_len);

	if (ba_read_header(&r) != 0) {
		close(r.fd);
		resp.status_code = 0;
		free(resp.body);
		resp.body = xstrdup("bad response header");
		return resp;
	}
	resp.status_code = r.status;

	for (;;) {
		int n = ba_body_read(&r, buf, sizeof(buf));
		if (n < 0)
			break;
		if (n == 0)
			break;
		if (len + (size_t)n + 1 > cap) {
			size_t nc = cap ? cap * 2 : 4096;
			while (nc < len + (size_t)n + 1)
				nc *= 2;
			resp.body = realloc(resp.body, nc);
			cap = nc;
		}
		memcpy(resp.body + len, buf, n);
		len += n;
		resp.body[len] = '\0';
	}
	close(r.fd);
	return resp;
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

	for (attempt = 0; attempt <= BA_MAX_RETRIES; attempt++) {
		StreamCtx sctx;
		BaResp r;
		int fd = ba_connect(u.host, u.port);
		int io_err = 0, http_code = 0;

		if (fd < 0) {
			io_err = 1;
			goto attempt_done;
		}
		memset(&r, 0, sizeof(r));
		r.fd = fd;
		ba_send_request(fd, &u, headers, header_count, body, body_len);
		if (ba_read_header(&r) != 0) {
			io_err = 1;
			close(fd);
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

		if (!io_err) {
			/* non-SSE JSON error bodies etc. */
			sse_stream_finish(&sctx, provider, callback, ctx);
			sse_stream_free(&sctx);
			if (http_code >= 400)
				return http_code;
			return 0;
		}
		sse_stream_free(&sctx);

	attempt_done:
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
		if ((unsigned)(monotonic_ms() - start_ms) >= BA_RETRY_MAX_TIME_MS)
			return io_err ? -1 : (http_code >= 400 ? http_code : 0);
		{
			SseEvent retry_evt;
			memset(&retry_evt, 0, sizeof(retry_evt));
			retry_evt.type = SSE_RETRY;
			callback(ctx, &retry_evt);
		}
		sleep(1);
	}
	return -1;
}
