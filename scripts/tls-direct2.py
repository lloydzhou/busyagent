import re

c = open('agentutils/ba_impl.c').read()

# ---------- 1) scheme-aware URL parsing ----------
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
assert old in c, "1"
c = c.replace(old, new)

# ---------- 2) BaResp: TLS record layer + plaintext carry buffer ----------
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
assert old in c, "2"
c = c.replace(old, new)

# ---------- 3) ba_read(): TLS branch ----------
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
			memcpy(r->tls_plain, r->tls->inbuf + RECHDR_LEN, n);
			r->tls_plain_len = n;
			r->tls_plain_pos = 0;
			if (r->tls_plain_len <= bufsz)
				break;   /* everything fits; hand it over */
			/* oversized plaintext: caller takes part now, the rest
			 * stays here only if another record is already buffered
			 * - loop to top only while empty is impossible here */
			break;
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
assert old in c, "3"
c = c.replace(old, new)

# ---------- 4) writes over TLS ----------
old = '''static int send_all(int fd, const char *buf, size_t len)'''
assert old in c, "4a"
c = c.replace(old, '''static int send_all_conn(tls_state_t *tls, int fd, const char *buf, size_t len)
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

static int send_all(int fd, const char *buf, size_t len)''', 1)

old = '''static void ba_send_request(int fd, const BaUrl *u, const char **headers,'''
assert old in c, "4b"
c = c.replace(old, '''static void ba_send_request(tls_state_t *tls, int fd, const BaUrl *u, const char **headers,''')
c = c.replace('''	send_all(fd, first, strlen(first));
	for (i = 0; i < header_count; i++) {
		send_all(fd, headers[i], strlen(headers[i]));
		send_all(fd, "\\r\\n", 2);
	}
	send_all(fd, "\\r\\n", 2);
	send_all(fd, body, body_len);''',
'''	send_all_conn(tls, fd, first, strlen(first));
	for (i = 0; i < header_count; i++) {
		send_all_conn(tls, fd, headers[i], strlen(headers[i]));
		send_all_conn(tls, fd, "\\r\\n", 2);
	}
	send_all_conn(tls, fd, "\\r\\n", 2);
	send_all_conn(tls, fd, body, body_len);''')

# ---------- 5) connect dispatch + TLS session ----------
old = '''			int fd = ba_connect(u.host, u.port);
			int io_err = 0, http_code = 0;

			if (fd < 0) {
				io_err = 1;
				goto attempt_done;
			}
			memset(&r, 0, sizeof(r));
			r.fd = fd;
			ba_send_request(fd, &u, headers, header_count, body, body_len);'''
new = '''			int fd;
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
#if ENABLE_TLS
			(void)tls;
#endif
			ba_send_request(tls, fd, &u, headers, header_count, body, body_len);'''
assert old in c, "5"
c = c.replace(old, new)

# close points: free the tls state with the socket
c = c.replace('''			if (ba_read_header(&r) != 0) {
				io_err = 1;
				close(fd);
				goto attempt_done;
			}''','''			if (ba_read_header(&r) != 0) {
				io_err = 1;
				close(fd);
				free(r.tls);
				goto attempt_done;
			}''')
c = c.replace('''		if (http_code >= 400)
				return http_code;
			return 0;''','''		if (http_code >= 400) {
				free(r.tls);
				close(fd);
				return http_code;
			}
			free(r.tls);
			close(fd);
			return 0;''')

# ---------- 6) ba_tls_connect helper ----------
anchor = '''/* Streaming POST with SSE pump. Mirrors the old curl semantics:'''
helper = '''#if ENABLE_TLS
/* https:// support: in-tree TLS state machine (networking/tls.c), same
 * as ssl_client/wget use. Returns a handshaked session; the caller owns
 * it and must free() it with the socket. No certificate validation. */
static tls_state_t *ba_tls_connect(const char *host, int port)
{
	static int note_shown;
	len_and_sockaddr *lsa;
	int fd;
	tls_state_t *tls;

	lsa = host2sockaddr(host, port);
	if (!lsa)
		return NULL;
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
	return tls;
}
#endif

/* Streaming POST with SSE pump. Mirrors the old curl semantics:'''
assert anchor in c, "6"
c = c.replace(anchor, helper, 1)

open('agentutils/ba_impl.c','w').write(c)
print('https direct-TLS transport in place')

# ---------- 7) select TLS ----------
b = open('agentutils/busyagent.c').read()
if 'select TLS' not in b:
    b = b.replace('//config:\tselect FEATURE_EDITING',
                  '//config:\tselect FEATURE_EDITING\\n//config:\\tselect TLS')
    open('agentutils/busyagent.c','w').write(b)
    print('select TLS added')

# ---------- 8) libbb.h: export the record API ----------
h = open('include/libbb.h').read()
old = '''void FAST_FUNC tls_handshake(tls_state_t *tls, const char *sni);'''
assert old in h
h = h.replace(old, '''int FAST_FUNC tls_xread_record(tls_state_t *tls, const char *expected);
void FAST_FUNC tls_xwrite(tls_state_t *tls, int len);
void *FAST_FUNC tls_get_outbuf(tls_state_t *tls, int len);
int FAST_FUNC tls_has_buffered_record(tls_state_t *tls);
void FAST_FUNC tls_handshake(tls_state_t *tls, const char *sni);''')
open('include/libbb.h','w').write(h)
print('libbb.h declarations added')

# ---------- 9) tls.c: make the record API non-static ----------
t = open('networking/tls.c').read()
subs = [
 ('static int tls_xread_record(tls_state_t *tls, const char *expected)',
  'int FAST_FUNC tls_xread_record(tls_state_t *tls, const char *expected)'),
 ('static void tls_xwrite(tls_state_t *tls, int len)',
  'void FAST_FUNC tls_xwrite(tls_state_t *tls, int len)'),
 ('static void *tls_get_outbuf(tls_state_t *tls, int len)',
  'void *FAST_FUNC tls_get_outbuf(tls_state_t *tls, int len)'),
 ('static int tls_has_buffered_record(tls_state_t *tls)',
  'int FAST_FUNC tls_has_buffered_record(tls_state_t *tls)'),
]
for a, b2 in subs:
    assert a in t, a
    t = t.replace(a, b2)
open('networking/tls.c','w').write(t)
print('tls.c exports done')
