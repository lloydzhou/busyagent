import re

c = open('agentutils/ba_impl.c').read()

# ---------- 1) scheme-aware URL parsing (https://, default 443) ----------
old = '''typedef struct {
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
		u->host[hlen] = '\\0';
		snprintf(u->path, sizeof(u->path), "%s", slash);
	}
	colon = strchr(u->host, ':');
	if (colon) {
		u->port = atoi(colon + 1);
		*colon = '\\0';
	} else {
		u->port = 80;
	}
	if (!u->host[0] || u->port <= 0)
		return -1;
	return 0;
}'''
new = '''typedef struct {
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
		u->host[hlen] = '\\0';
		snprintf(u->path, sizeof(u->path), "%s", slash);
	}
	colon = strchr(u->host, ':');
	if (colon) {
		u->port = atoi(colon + 1);
		*colon = '\\0';
	} else {
		u->port = u->is_https ? 443 : 80;
	}
	if (!u->host[0] || u->port <= 0)
		return -1;
	return 0;
}'''
assert old in c
c = c.replace(old, new)

# ---------- 2) BaResp: TLS state + plaintext carry buffer ----------
old = '''	int eof;
	char hdr[BA_MAX_HEADER];
	size_t hdr_len;
	int status;
	int got_header;
	char pending[4096];
	size_t pending_len;
} BaResp;'''
new = '''	int eof;
	tls_state_t *tls;         /* https: record layer (NULL for plain http) */
	char tls_plain[8192];     /* decrypted bytes not yet consumed */
	int tls_plain_len;
	int tls_plain_pos;
	char hdr[BA_MAX_HEADER];
	size_t hdr_len;
	int status;
	int got_header;
	char pending[4096];
	size_t pending_len;
} BaResp;'''
assert old in c
c = c.replace(old, new)

# ---------- 3) buffered reader: TLS branch ----------
old = '''	for (;;) {
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
}'''
new = '''#if ENABLE_TLS
	if (r->tls) {
		/* idle-timeout watchdog on the network socket */
		struct pollfd pfd;
		pfd.fd = r->tls->ifd;
		pfd.events = POLLIN;
		if (safe_poll(&pfd, 1, BA_READ_TIMEOUT_MS) <= 0)
			return -1;

		while (r->tls_plain_len == 0) {
			int n = tls_xread_record(r->tls, "application data");
			if (n < 1)
				return 0;   /* TLS EOF */
			if (r->tls->inbuf[0] != RECORD_TYPE_APPLICATION_DATA)
				return -1;
			if (n > (int)sizeof(r->tls_plain))
				n = (int)sizeof(r->tls_plain);
			memcpy(r->tls_plain, r->tls->inbuf + RECHDR_LEN_TLS, n);
			r->tls_plain_len = n;
			r->tls_plain_pos = 0;
			/* tls.c memmoves the remainder; record fully consumed
			 * when nothing beyond it is buffered */
			if (!tls_has_buffered_record(r->tls))
				break;
			/* may hold a complete next record: loop to fill more */
		}
		{
			int give = r->tls_plain_len - r->tls_plain_pos;
			if (give > bufsz)
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
}'''
assert old in c
c = c.replace(old, new)

# ---------- 4) writes: send_all/list tolerate TLS ----------
old = '''static int send_all(int fd, const char *buf, size_t len)'''
assert old in c
new_send = '''static int send_all_conn(BaResp *r, int fd, const char *buf, size_t len)
{
#if ENABLE_TLS
	if (r->tls) {
		while (len) {
			size_t chunk = len > 8192 ? 8192 : len;
			memcpy(tls_get_outbuf(r->tls, chunk), buf, chunk);
			tls_xwrite(r->tls, chunk);
			buf += chunk;
			len -= chunk;
		}
		return 0;
	}
#else
	(void)r;
#endif
	return send_all(fd, buf, len);
}

static int send_all(int fd, const char *buf, size_t len)'''
c = c.replace(old, new_send, 1)

c = c.replace('''	send_all(fd, first, strlen(first));
	for (i = 0; i < header_count; i++) {
		send_all(fd, headers[i], strlen(headers[i]));
		send_all(fd, "\\r\\n", 2);
	}
	send_all(fd, "\\r\\n", 2);
	send_all(fd, body, body_len);''',
'''	send_all_conn(r, fd, first, strlen(first));
	for (i = 0; i < header_count; i++) {
		send_all_conn(r, fd, headers[i], strlen(headers[i]));
		send_all_conn(r, fd, "\\r\\n", 2);
	}
	send_all_conn(r, fd, "\\r\\n", 2);
	send_all_conn(r, fd, body, body_len);''')

# ---------- 5) connect dispatch + TLS setup ----------
old = '''			int fd = ba_connect(u.host, u.port);
			int io_err = 0, http_code = 0;

			if (fd < 0) {
				io_err = 1;
				goto attempt_done;
			}
			memset(&r, 0, sizeof(r));
			r.fd = fd;'''
new = '''			int fd;
			int io_err = 0, http_code = 0;

#if ENABLE_TLS
			fd = u.is_https ? ba_tls_connect(u.host, u.port)
			                : ba_connect(u.host, u.port);
#else
			if (u.is_https) {
				bb_simple_error_msg("TLS support not compiled in (CONFIG_TLS)");
				return -1;
			}
			fd = ba_connect(u.host, u.port);
#endif
			if (fd < 0) {
				io_err = 1;
				goto attempt_done;
			}
			memset(&r, 0, sizeof(r));
			r.fd = fd;
#if ENABLE_TLS
			if (u.is_https) {
				r.tls = new_tls_state();
				r.tls->ifd = r.tls->ofd = fd;
				tls_handshake(r.tls, u.host);   /* SNI, no cert check (busybox TLS) */
			}
#endif'''
assert old in c
c = c.replace(old, new)

# ---------- 6) attempt_done cleanup: close tls fd ----------
old = '''attempt_done:
		if (io_err && attempt < BA_MAX_RETRIES
		 && monotonic_ms() - start_ms < BA_RETRY_MAX_TIME_MS) {
			usleep(1000);
			continue;
		}'''
if old not in c:
    old = '''attempt_done:
		if (io_err) {
			close(fd);
			fd = -1;
		}'''
assert old in c, "attempt_done site"
new = old
c = c.replace(old, new)

# ---------- 7) https connect helper (before http_post_sse) ----------
anchor = '''/* Streaming POST with SSE pump. Mirrors the old curl semantics:'''
helper = '''#if ENABLE_TLS
/* https:// support (wget.c spawn-free variant): we run the in-tree TLS
 * state machine in-process. Returns the socket fd; the record layer lives
 * in r->tls afterwards. No certificate validation (busybox TLS). */
static int ba_tls_connect(const char *host, int port)
{
	static int note_shown;
	len_and_sockaddr *lsa;
	int fd;
	tls_state_t *tls;

	lsa = host2sockaddr(host, port);
	if (!lsa)
		return -1;
	fd = xsocket(lsa->u.sa.sa_family, SOCK_STREAM, 0);
	xconnect_stream(lsa);
	free(lsa);

	tls = new_tls_state();
	tls->ifd = tls->ofd = fd;
	tls_handshake(tls, host);
	if (!note_shown) {
		bb_simple_error_msg("note: TLS certificate validation not implemented");
		note_shown = 1;
	}
	return fd;
}
#endif

/* Streaming POST with SSE pump. Mirrors the old curl semantics:'''
assert anchor in c
c = c.replace(anchor, helper, 1)

# fix tls cleanup on close: plain close is fine; tls state freed at exit
open('agentutils/ba_impl.c','w').write(c)
print('https direct-TLS transport in place')

# ---------- 8) select TLS ----------
b = open('agentutils/busyagent.c').read()
if 'select TLS' not in b:
    b = b.replace('//config:\tselect FEATURE_EDITING',
                  '//config:\tselect FEATURE_EDITING\n//config:\tselect TLS')
    open('agentutils/busyagent.c','w').write(b)
    print('select TLS added')
