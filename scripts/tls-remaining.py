#!/usr/bin/env python3
# apply remaining steps of the https transport patch (idempotent-ish)
c = open('agentutils/ba_impl.c').read()
changed = []

def rep(old, new, tag):
    global c
    if new in c:
        changed.append(f"{tag}: already")
        return
    assert old in c, tag
    c = c.replace(old, new)
    changed.append(f"{tag}: applied")

# 1 parse_url
old1 = '''typedef struct {
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
	p = url + 7;'''
new1 = '''typedef struct {
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
	p = url + 7 if False else p;  # placeholder never emitted
'''
# simpler: split into two precise edits
old1a = '''	memset(u, 0, sizeof(*u));
	if (strncmp(url, "http://", 7) != 0)
		return -1;
	p = url + 7;'''
new1a = '''	memset(u, 0, sizeof(*u));
	if (strncmp(url, "https://", 8) == 0) {
		u->is_https = 1;
		p = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		p = url + 7;
	} else {
		return -1;
	}'''
rep(old1a, new1a, "1a")

old1b = '''typedef struct {
	char host[256];
	int port;
	char path[1024];
} BaUrl;'''
new1b = '''typedef struct {
	char host[256];
	int port;
	int is_https;
	char path[1024];
} BaUrl;'''
rep(old1b, new1b, "1b")

old1c = '''	} else {
		u->port = 80;
	}'''
new1c = '''	} else {
		u->port = u->is_https ? 443 : 80;
	}'''
rep(old1c, new1c, "1c")

# 2 BaResp fields
old2 = '''	int eof;
	char hdr[BA_MAX_HEADER];'''
new2 = '''	int eof;
	tls_state_t *tls;
	char tls_plain[8192];
	int tls_plain_len;
	int tls_plain_pos;
	char hdr[BA_MAX_HEADER];'''
rep(old2, new2, "2")

# 3 ba_read TLS branch (insert at top of function body after decls)
old3 = '''	int n = 0;

'''
alt3 = '''	ssize_t n;

'''
# find ba_read signature
i = c.index('static int ba_read(BaResp *r, char *buf, size_t bufsz)')
j = c.index('\n', c.index('{', i)) + 1
# locate the poll loop inside ba_read (first occurrence after i)
k = c.index('for (;;) {\n\t\tif (safe_poll(&pfd, 1, BA_READ_TIMEOUT_MS) < 0) {', i)
tls3 = '''#if ENABLE_TLS
	if (r->tls) {
		struct pollfd pfd;
		pfd.fd = r->tls->ifd;
		pfd.events = POLLIN;
		if (safe_poll(&pfd, 1, BA_READ_TIMEOUT_MS) <= 0)
			return -1;

		while (r->tls_plain_len == 0) {
			int tn = tls_xread_record(r->tls, "application data");
			if (tn < 1)
				return 0;   /* TLS EOF */
			if (r->tls->inbuf[0] != RECORD_TYPE_APPLICATION_DATA)
				return -1;
			if (tn > (int)sizeof(r->tls_plain))
				tn = (int)sizeof(r->tls_plain);
			memcpy(r->tls_plain, r->tls->inbuf + RECHDR_LEN, tn);
			r->tls_plain_len = tn;
			r->tls_plain_pos = 0;
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
'''
c = c[:k] + tls3 + c[k:]
changed.append("3: applied (branch insert)")

# 4 send_all_conn + signature
if 'send_all_conn' not in c:
    old4 = 'static int send_all(int fd, const char *buf, size_t len)'
    c = c.replace(old4, '''static int send_all_conn(tls_state_t *tls, int fd, const char *buf, size_t len)
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
    changed.append("4a: applied")
c = c.replace('static void ba_send_request(int fd, const BaUrl *u, const char **headers,',
              'static void ba_send_request(tls_state_t *tls, int fd, const BaUrl *u, const char **headers,')
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
changed.append("4b: applied")

# 6 helper before Streaming POST comment
if 'ba_tls_connect' not in c:
    anchor = '/* Streaming POST with SSE pump. Mirrors the old curl semantics:'
    helper = '''#if ENABLE_TLS
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

''' + anchor
    c = c.replace(anchor, helper, 1)
    changed.append("6: applied")

open('agentutils/ba_impl.c','w').write(c)
print("; ".join(changed))

# 8 libbb.h decls
h = open('include/libbb.h').read()
if 'tls_xread_record' not in h:
    old = 'void FAST_FUNC tls_handshake(tls_state_t *tls, const char *sni);'
    assert old in h
    h = h.replace(old, '''int FAST_FUNC tls_xread_record(tls_state_t *tls, const char *expected);
void FAST_FUNC tls_xwrite(tls_state_t *tls, int len);
void *FAST_FUNC tls_get_outbuf(tls_state_t *tls, int len);
int FAST_FUNC tls_has_buffered_record(tls_state_t *tls);
void FAST_FUNC tls_handshake(tls_state_t *tls, const char *sni);''')
    open('include/libbb.h','w').write(h)
    print("8: applied")

# 9 tls.c exports
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
done = []
for a, b in subs:
    if a in t:
        t = t.replace(a, b)
        done.append(a.split('(')[0])
    elif b.split('(')[0] in t:
        done.append(a.split('(')[0] + ' (already)')
    else:
        print("MISSING:", a)
open('networking/tls.c','w').write(t)
print("9:", done)
