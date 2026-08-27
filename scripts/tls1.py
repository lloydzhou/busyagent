import re

c = open('agentutils/ba_impl.c').read()

# ---- 1. scheme-aware URL parsing ----
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
}

#if ENABLE_TLS
/* https:// support, wget.c spawn_ssl_client() style: the parent speaks
 * plaintext HTTP over a socketpair; a forked child runs the in-tree TLS
 * state machine (networking/tls.c) against the network socket. */
#include <sys/un.h>

static int ba_tls_connect(const char *host, int port)
{
	int sp[2];
	int pid;
	char servername[256];
	char *p;

	snprintf(servername, sizeof(servername), "%s", host);
	p = strrchr(servername, ':');
	if (p) *p = '\\0';

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
		bb_simple_perror_msg_and_die("socketpair");

	fflush_all();
	pid = xfork();
	if (pid == 0) {
		/* child: in-tree TLS state machine (wget.c:769 pattern) */
		tls_state_t *tls;
		len_and_sockaddr *lsa;
		int nfd;

		close(sp[0]);
		xmove_fd(sp[1], 0);
		xdup2(0, 1);
		lsa = xhost2sockaddr(host, port);
		nfd = xsocket(lsa->u.sa.sa_family, SOCK_STREAM, 0);
		xconnect_stream(lsa);
		tls = new_tls_state();
		tls->ifd = tls->ofd = nfd;
		tls_handshake(tls, servername);
		tls_run_copy_loop(tls, /*flags*/ 0);
		_exit(0);
	}

	/* parent: close child ends, keep the plaintext side */
	close(sp[1]);
	if (getenv("BB_AGENT_TLS_NOTE") == NULL) {
		bb_simple_error_msg("note: TLS certificate validation not implemented");
		setenv("BB_AGENT_TLS_NOTE", "1", 1);
	}
	return sp[0];
}
#endif'''
assert old in c
c = c.replace(old, new)

# ---- 2. connect dispatch in http_post_sse ----
old2 = '''			int fd = ba_connect(u.host, u.port);
			int io_err = 0, http_code = 0;

			if (fd < 0) {
				io_err = 1;
				goto attempt_done;
			}'''
new2 = '''			int fd;
			int io_err = 0, http_code = 0;

#if ENABLE_TLS
			fd = u.is_https ? ba_tls_connect(u.host, u.port)
			                : ba_connect(u.host, u.port);
#else
			if (u.is_https) {
				bb_simple_error_msg("TLS support not compiled in (enable CONFIG_TLS)");
				return -1;
			}
			fd = ba_connect(u.host, u.port);
#endif
			if (fd < 0) {
				io_err = 1;
				goto attempt_done;
			}'''
assert old2 in c
c = c.replace(old2, new2)

# ---- 3. Host header: default-port aware ----
old3 = '''	if (u->port != 80)'''
new3 = '''	if (u->port != (u->is_https ? 443 : 80))'''
assert old3 in c
c = c.replace(old3, new3)

open('agentutils/ba_impl.c','w').write(c)
print('https transport added')

# ---- 4. select TLS ----
b = open('agentutils/busyagent.c').read()
old4 = "//config:\tselect FEATURE_EDITING"
new4 = "//config:\tselect FEATURE_EDITING\n//config:\tselect TLS"
assert old4 in b
b = b.replace(old4, new4)
open('agentutils/busyagent.c','w').write(b)
print('select TLS added')
