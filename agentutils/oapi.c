/* oapi - OpenAPI CLI: register an OpenAPI (swagger) JSON document and
 * call its operations as if they were plain shell commands.
 *
 * Registry: $BA_HOME/oapi/apis/<name>.json holds the fetch source and
 * the complete spec. stdout carries data only, diagnostics go to stderr
 * and a non-2xx response exits 1 (same contract as the python "oapi"
 * CLI this applet mirrors).
 *
 * Modelled on busyagent's infrastructure (StrBuf/JSON from busyagent.h,
 * HTTP from agent_common.c). YAML specs, OAuth flows and redirects are
 * out of scope for the MVP.
 */
//config:config OAPI
//config:	bool "oapi (12 kb)"
//config:	default y
//config:	select AGENTUTILS_COMMON
//config:	help
//config:	  OpenAPI command line client: 'oapi connect NAME SPEC' caches an
//config:	  API document, then 'oapi NAME OPERATION [args] [--flags]' builds
//config:	  and sends the request. 'oapi api NAME METHOD /path' is the raw
//config:	  escape hatch.

//applet:IF_OAPI(APPLET(oapi, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_OAPI) += oapi.o

//usage:#define oapi_trivial_usage
//usage:       "connect NAME SPEC | sync NAME | ls | rm NAME | schema NAME [OP]\n"
//usage:       "	| api NAME METHOD PATH | NAME OP [PARAM]... [--FLAG V]... [--body X] [-F K=V]"
//usage:#define oapi_full_usage "\n\n"
//usage:       "Call OpenAPI operations like shell commands\n"
//usage:       "\n"
//usage:       "Operations are addressed by operationId in kebab-case\n"
//usage:       "	(getPetById -> get-pet-by-id). Required path parameters\n"
//usage:       "	are positional (template order); query/header parameters\n"
//usage:       "	become --kebab-case flags.\n"
//usage:       "\n"
//usage:       "connect NAME SPEC	Cache spec from URL or file as NAME\n"
//usage:       "sync NAME		Re-fetch from the stored source\n"
//usage:       "ls			List registered APIs\n"
//usage:       "rm NAME		Remove NAME from the registry\n"
//usage:       "schema NAME [OP]	List operations or show OP parameters\n"
//usage:       "api NAME METHOD PATH	Raw request escape hatch\n"
//usage:       "\n"
//usage:       "	--body X		Request body: inline JSON, @file or - (stdin)\n"
//usage:       "	-F K=V		Body field, repeatable (value auto-typed)\n"
//usage:       "	--jq PATH	Filter output: a.b[0].c, items[].id\n"
//usage:       "	-o FMT		Output format: json (default) or text\n"
//usage:       "	--header H:V	Custom header, repeatable\n"
//usage:       "	--dry-run	Print the request, send nothing\n"
//usage:       "	--timeout N	Not implemented (compat flag)\n"
//usage:       "	--insecure	Accepted for compatibility (TLS is never verified)"

#include "busyagent.h"
#include "agent_common.h"
#include "libbb.h"
#include "busybox.h"
#include <sys/stat.h>

/* ============================================================
 * registry: $BA_HOME/oapi/apis/<name>.json
 *
 * {"source": "<url-or-path>", "fetched_at": <unix>, "spec": {...}}
 * ============================================================ */

#define OAPI_MAX_HEADERS 16

typedef struct {
	char *source;        /* url or file path the spec came from */
	time_t fetched_at;
	char *data;          /* registry file text (JsonVal views point here) */
	JsonParse jp;         /* parse of the wrapper document */
	JsonVal spec;        /* json_get(jp.val, "spec") */
	char *path;          /* registry file path (owned) */
} OapiRegistry;

/* base dir: $BA_HOME or ~/.busyagent (same rule as busyagent) */
static char *oapi_home(void)
{
	const char *h = getenv("BA_HOME");
	if (h && h[0])
		return xasprintf("%s/oapi/apis", h);
	h = getenv("HOME");
	if (h && h[0])
		return xasprintf("%s/.busyagent/oapi/apis", h);
	return xstrdup("/tmp/busyagent/oapi/apis");
}

static char *oapi_reg_path(const char *name)
{
	char *dir = oapi_home();
	char *p;

	if (!name[0] || strchr(name, '/') || strcmp(name, ".") == 0
	 || strcmp(name, "..") == 0)
		return NULL;
	p = xasprintf("%s/%s.json", dir, name);
	free(dir);
	return p;
}

/* load the wrapper document; -1 when missing (does not die) */
static int oapi_load(OapiRegistry *r, const char *name)
{
	FILE *f;
	long sz;
	char *data;

	memset(r, 0, sizeof(*r));
	r->path = oapi_reg_path(name);
	if (!r->path)
		return -1;
	f = fopen(r->path, "r");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return -1;
	}
	data = xmalloc(sz + 1);
	{
		size_t got = fread(data, 1, sz, f);
		fclose(f);
		data[got] = '\0';
	}
	r->data = data;
	r->jp = json_parse_root(data);
	/* JsonVal is a zero-copy view into r->data: the buffer must stay
	 * alive until oapi_free() */
	if (r->jp.error) {
		bb_error_msg("%s: invalid registry entry: %s", name, r->jp.error);
		return -1;
	}
	r->source = json_get_string(r->jp.val, "source");
	r->fetched_at = (time_t)json_get_ll(r->jp.val, "fetched_at");
	r->spec = json_get(r->jp.val, "spec");
	if (r->spec.type != JSON_OBJECT) {
		bb_error_msg("%s: registry entry has no spec object", name);
		return -1;
	}
	return 0;
}

static void oapi_free(OapiRegistry *r)
{
	free(r->path);
	free(r->source);
	free(r->data);
}

static int oapi_save(const char *name, const char *source, const char *spec_json)
{
	char *dir = oapi_home();
	char *tmp;
	FILE *f;
	int rc;

	bb_make_directory(dir, 0755, FILEUTILS_RECUR);
	tmp = xasprintf("%s/%s.json.tmp", dir, name);
	f = xfopen(tmp, "w");
	fprintf(f, "{\"source\":");
	{
		StrBuf sb;
		sb_init(&sb);
		sb_append_json_string(&sb, source);
		fputs(sb.data, f);
		sb_free(&sb);
	}
	fprintf(f, ",\"fetched_at\":%lld,\"spec\":", (long long)time(NULL));
	fputs(spec_json, f);
	fputs("}\n", f);
	fclose(f);
	rc = 0;
	{
		char *dst = xasprintf("%s/%s.json", dir, name);
		if (rename(tmp, dst) != 0) {
			bb_perror_msg("rename %s", dst);
			rc = -1;
		}
		free(dst);
	}
	free(tmp);
	free(dir);
	return rc;
}

/* ============================================================
 * case conversion + json pointer
 * ============================================================ */

/* lower camel/kebab/snake to kebab-case: getPetById -> get-pet-by-id */
static char *oapi_kebab(const char *s)
{
	StrBuf sb;
	char *out;
	const char *p;

	sb_init(&sb);
	for (p = s; *p; p++) {
		char c = *p;
		if (c == '_' || c == '-' || c == ' ') {
			if (sb.len)
				sb_append_char(&sb, '-');
		} else if (c >= 'A' && c <= 'Z') {
			/* break before an uppercase run unless the previous
			 * character is uppercase too (acronym: getURL -> get-url) */
			if (sb.len && sb.data[sb.len-1] != '-') {
				char prev = p[-1];
				if (!(prev >= 'A' && prev <= 'Z'))
					sb_append_char(&sb, '-');
			}
			sb_append_char(&sb, (char)(c - 'A' + 'a'));
		} else {
			sb_append_char(&sb, c);
		}
	}
	/* trim a trailing dash (e.g. "id_" -> "id") */
	if (sb.len && sb.data[sb.len-1] == '-')
		sb.data[sb.len-1] = '\0';
	out = sb.data ? sb.data : xstrdup("");
	return out;
}

/* resolve "#/a/b" against root (JSON Pointer subset: no leading key
 * escaping beyond ~0/~1). Returns JSON_NULL when unresolvable. */
static JsonVal oapi_resolve(JsonVal root, const char *ref)
{
	JsonVal v = root;
	const char *p;

	if (ref[0] != '#' || ref[1] != '/') {
		return v;   /* external refs unsupported: return as-is */
	}
	p = ref + 2;
	while (*p) {
		const char *seg = p;
		const char *end = strchr(p, '/');
		size_t seglen = end ? (size_t)(end - p) : strlen(p);
		char key[256];

		if (seglen >= sizeof(key))
			return v;   /* unresolvable, stays where it was */
		memcpy(key, seg, seglen);
		key[seglen] = '\0';
		/* ~1 -> '/', ~0 -> '~' */
		{
			char *w = key;
			const char *rd = key;
			while (*rd) {
				if (rd[0] == '~' && rd[1] == '1') { *w++ = '/'; rd += 2; }
				else if (rd[0] == '~' && rd[1] == '0') { *w++ = '~'; rd += 2; }
				else *w++ = *rd++;
			}
			*w = '\0';
		}
		v = json_get(v, key);
		if (v.type == JSON_NULL)
			return v;
		p = end ? end + 1 : p + seglen;
	}
	return v;
}

/* element of a parameters array with $ref folded in (view into the
 * same document; returns JSON_NULL on garbage) */
static JsonVal oapi_param_ref(JsonVal spec, JsonVal p)
{
	char *ref;

	if (p.type == JSON_OBJECT) {
		ref = json_get_string(p, "$ref");
		if (ref) {
			JsonVal t = oapi_resolve(spec, ref);
			free(ref);
			return t;
		}
		free(ref);
	}
	return p;
}

/* ============================================================
 * operation table
 * ============================================================ */

typedef struct {
	char *op_id;         /* raw operationId */
	char *kebab;         /* kebab-case command name */
	char *method;        /* GET/POST/... */
	char *path;          /* URL template */
	char *summary;
	JsonVal params;      /* array view (may be JSON_NULL) */
	int has_body;
} OapiOp;

typedef struct {
	OapiOp *v;
	int count;
	int cap;
	JsonParse jp;        /* owns the spec text (view) */
	JsonVal spec;
} OapiOps;

static const char *const http_methods[] = {
	"get", "put", "post", "delete", "options", "head", "patch", "trace",
};

static void oapi_ops_add(OapiOps *ops, const char *method, const char *path,
			 JsonVal op)
{
	char *id = json_get_string(op, "operationId");
	OapiOp *o;

	if (!id || !id[0]) {
		free(id);
		return;   /* no operationId: not addressable as a command */
	}
	if (ops->count == ops->cap) {
		ops->cap = ops->cap ? ops->cap * 2 : 16;
		ops->v = xrealloc(ops->v, ops->cap * sizeof(ops->v[0]));
	}
	o = &ops->v[ops->count++];
	memset(o, 0, sizeof(*o));
	o->op_id = id;
	o->kebab = oapi_kebab(id);
	o->method = xstrdup(method);
	/* http methods go on the wire uppercase */
	{
		char *m = o->method;
		for (; *m; m++)
			if (*m >= 'a' && *m <= 'z')
				*m = (char)(*m - 'a' + 'A');
	}
	o->path = xstrdup(path);
	o->summary = json_get_string(op, "summary");
	o->params = json_get(op, "parameters");
	o->has_body = (json_get(op, "requestBody").type == JSON_OBJECT);
}

static void oapi_ops_build(OapiOps *ops, JsonVal spec)
{
	JsonObjectIter it;
	JsonVal paths = json_get(spec, "paths");

	memset(ops, 0, sizeof(*ops));
	if (paths.type != JSON_OBJECT)
		return;
	json_obj_iter_init(&it, paths);
	while (json_obj_iter_next(&it)) {
		JsonVal pathobj = it.val;
		int mi;

		if (pathobj.type != JSON_OBJECT)
			continue;
		for (mi = 0; mi < (int)(sizeof(http_methods)/sizeof(http_methods[0])); mi++) {
			JsonVal op = json_get(pathobj, http_methods[mi]);
			if (op.type == JSON_OBJECT)
				oapi_ops_add(ops, http_methods[mi], it.key, op);
		}
	}
	json_obj_iter_cleanup(&it);
}

static void oapi_ops_free(OapiOps *ops)
{
	int i;

	for (i = 0; i < ops->count; i++) {
		free(ops->v[i].op_id);
		free(ops->v[i].kebab);
		free(ops->v[i].method);
		free(ops->v[i].path);
		free(ops->v[i].summary);
	}
	free(ops->v);
}

static OapiOp *oapi_ops_find(OapiOps *ops, const char *cmd)
{
	int i;

	for (i = 0; i < ops->count; i++) {
		if (strcmp(ops->v[i].kebab, cmd) == 0
		 || strcmp(ops->v[i].op_id, cmd) == 0)
			return &ops->v[i];
	}
	return NULL;
}

/* ============================================================
 * request building
 * ============================================================ */

/* percent-encode anything outside unreserved/slash for path segments */
static void oapi_encode(StrBuf *sb, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";

	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		 || (c >= '0' && c <= '9') || c == '-' || c == '_'
		 || c == '.' || c == '~' || c == '/')
			sb_append_char(sb, (char)c);
		else {
			sb_append_char(sb, '%');
			sb_append_char(sb, hex[c >> 4]);
			sb_append_char(sb, hex[c & 15]);
		}
	}
}

/* base url: servers[0] resolved against the fetch source */
static char *oapi_base_url(OapiRegistry *r)
{
	JsonVal servers = json_get(r->spec, "servers");
	char *url;

	if (servers.type == JSON_ARRAY && json_array_len(servers) > 0) {
		JsonVal s0 = json_array_get(servers, 0);
		url = json_get_string(s0, "url");
		if (url && (strncmp(url, "http://", 7) == 0
			 || strncmp(url, "https://", 8) == 0))
			return url;   /* absolute server url */
		if (url && url[0] == '/' && r->source
		 && strncmp(r->source, "http", 4) == 0) {
			/* "/api" + source origin */
			BaUrl su;
			char *abs;
			if (ba_parse_url(r->source, &su) == 0) {
				abs = xasprintf("http%s://%s:%d%s",
						su.is_https ? "s" : "",
						su.host, su.port, url);
				free(url);
				return abs;
			}
		}
		if (url && url[0])
			return url;   /* hope for the best */
		free(url);
	}
	/* fallback: spec source itself */
	return r->source ? xstrdup(r->source) : NULL;
}

/* ============================================================
 * output
 * ============================================================ */

/* apply a --jq dot-path via the shared agc_json_path evaluator.
 * Returns 0 on match (printed), 1 when the path does not resolve. */
static int oapi_jq_print(JsonVal v, const char *path)
{
	AgcJqMatches m;
	int i;

	if (agc_json_path(v, path, &m) != 0)
		return 1;
	for (i = 0; i < m.n; i++) {
		JsonVal el = m.v[i];
		char *s;

		if (el.type == JSON_NULL)
			continue;   /* absent key: nothing to print */
		s = json_as_string(el);
		if (s) {
			puts(s);
			free(s);
		} else {
			/* nested value: raw slice */
			printf("%.*s\n", (int)(el.end - el.start),
			       el.src + el.start);
		}
	}
	return m.n ? 0 : 1;
}

/* text output: scalar as-is, array element per line, object raw json */
static void oapi_text_print(JsonVal v)
{
	int i;

	switch (v.type) {
	case JSON_STRING: {
		char *s = json_string_val(v);
		puts(s ? s : "");
		free(s);
		break;
	}
	case JSON_NUMBER:
		printf("%lld\n", (long long)json_number_val(v));
		break;
	case JSON_BOOL:
		puts(json_bool_val(v) ? "true" : "false");
		break;
	case JSON_NULL:
		break;
	case JSON_ARRAY:
		for (i = 0; i < json_array_len(v); i++)
			oapi_text_print(json_array_get(v, i));
		break;
	case JSON_OBJECT:
	default:
		printf("%.*s\n", (int)(v.end - v.start), v.src + v.start);
		break;
	}
}

/* ============================================================
 * smart -F value typing: true/false/null/number/json, else string
 * ============================================================ */
static void oapi_append_typed(StrBuf *sb, const char *val)
{
	JsonParse jp = json_parse_root(val);

	if (!jp.error) {
		sb_append(sb, val);   /* valid json literal/array/object */
		return;
	}
	/* plain strings need quoting */
	sb_append_char(sb, '"');
	{
		const char *p;
		for (p = val; *p; p++) {
			switch (*p) {
			case '"': sb_append(sb, "\\\""); break;
			case '\\': sb_append(sb, "\\\\"); break;
			case '\n': sb_append(sb, "\\n"); break;
			case '\r': sb_append(sb, "\\r"); break;
			case '\t': sb_append(sb, "\\t"); break;
			default: sb_append_char(sb, *p);
			}
		}
	}
	sb_append_char(sb, '"');
}

/* ============================================================
 * commands
 * ============================================================ */

static char *oapi_fetch_spec(const char *source)
{
	if (strncmp(source, "http://", 7) == 0 || strncmp(source, "https://", 8) == 0) {
		AgcHttpResp resp;
		const char *accept = "Accept: application/json";
		int rc = agc_http_request("GET", source, &accept, 1, "", 0, &resp);
		if (rc != 0) {
			bb_error_msg("fetch %s: transport error", source);
			return NULL;
		}
		if (resp.status < 200 || resp.status >= 300) {
			bb_error_msg("fetch %s: HTTP %d", source, resp.status);
			agc_http_resp_free(&resp);
			return NULL;
		}
		{
			char *body = resp.body ? resp.body : xstrdup("");
			resp.body = NULL;   /* ownership moved */
			agc_http_resp_free(&resp);
			return body;
		}
	}
	return xmalloc_xopen_read_close(source, NULL);
}

static int oapi_cmd_connect(const char *name, const char *source)
{
	char *spec = oapi_fetch_spec(source);
	JsonParse jp;

	if (!spec)
		return 1;
	jp = json_parse_root(spec);
	if (jp.error) {
		bb_error_msg("%s: not a JSON document: %s (YAML is not supported yet)",
			     source, jp.error);
		free(spec);
		return 1;
	}
	if (json_get(jp.val, "paths").type != JSON_OBJECT) {
		bb_error_msg("%s: no paths object (not an OpenAPI document?)", source);
		free(spec);
		return 1;
	}
	if (oapi_save(name, source, spec) != 0) {
		free(spec);
		return 1;
	}
	{
		OapiOps ops;
		oapi_ops_build(&ops, jp.val);
		printf("connected %s: %d operations (source %s)\n",
		       name, ops.count, source);
		oapi_ops_free(&ops);
	}
	free(spec);
	return 0;
}

static int oapi_cmd_ls(void)
{
	char *dir = oapi_home();
	DIR *d = opendir(dir);
	struct dirent *de;
	int n = 0;

	if (!d) {
		printf("no APIs registered (oapi connect NAME SPEC)\n");
		free(dir);
		return 0;
	}
	while ((de = readdir(d)) != NULL) {
		char *dot = strrchr(de->d_name, '.');
		OapiRegistry r;
		char name[256];

		if (!dot || strcmp(dot, ".json") != 0)
			continue;
		{
			size_t n2 = dot - de->d_name;
			if (n2 >= sizeof(name))
				continue;
			memcpy(name, de->d_name, n2);
			name[n2] = '\0';
		}
		if (oapi_load(&r, name) != 0) {
			printf("%-16s (broken entry)\n", name);
			n++;
			continue;
		}
		{
			OapiOps ops;
			oapi_ops_build(&ops, r.spec);
			printf("%-16s %d operations  %s\n", name, ops.count,
			       r.source ? r.source : "?");
			oapi_ops_free(&ops);
		}
		oapi_free(&r);
		n++;
	}
	closedir(d);
	free(dir);
	if (!n)
		printf("no APIs registered (oapi connect NAME SPEC)\n");
	return 0;
}

static int oapi_cmd_schema(OapiRegistry *r, const char *opname)
{
	OapiOps ops;
	int i;
	int found = 0;

	oapi_ops_build(&ops, r->spec);
	if (!opname) {
		for (i = 0; i < ops.count; i++)
			printf("%-28s %-6s %-28s %s\n",
			       ops.v[i].kebab, ops.v[i].method, ops.v[i].path,
			       ops.v[i].summary ? ops.v[i].summary : "");
		oapi_ops_free(&ops);
		return 0;
	}
	for (i = 0; i < ops.count; i++) {
		OapiOp *o = &ops.v[i];
		int pi;

		if (strcmp(o->kebab, opname) != 0 && strcmp(o->op_id, opname) != 0)
			continue;
		found = 1;
		printf("%s %s  (%s)\n", o->method, o->path, o->op_id);
		if (o->summary && o->summary[0])
			printf("  %s\n", o->summary);
		if (o->params.type == JSON_ARRAY) {
			for (pi = 0; pi < json_array_len(o->params); pi++) {
				JsonVal p = oapi_param_ref(r->spec, json_array_get(o->params, pi));
				char *nm = json_get_string(p, "name");
				char *in = json_get_string(p, "in");
				int req = json_get_bool(p, "required", 0);
				char *typ = NULL;
				JsonVal schema = json_get(p, "schema");
				if (schema.type == JSON_OBJECT)
					typ = json_get_string(schema, "type");
				printf("  --%s  in:%s%s%s%s\n",
				       nm ? oapi_kebab(nm) : "?", in ? in : "?",
				       typ ? " type:" : "", typ ? typ : "",
				       req ? "  (required)" : "");
				free(nm); free(in); free(typ);
			}
		}
		if (o->has_body)
			printf("  body: --body JSON | -F key=value\n");
		break;
	}
	oapi_ops_free(&ops);
	if (!found) {
		bb_error_msg("operation '%s' not found (see: oapi schema)", opname);
		return 1;
	}
	return 0;
}

/* -------- operation invocation -------- */

typedef struct {
	const char *headers[OAPI_MAX_HEADERS];
	int n_headers;
	const char *body;        /* --body */
	char *f_keys[32];
	char *f_vals[32];
	int n_f;
	const char *jq;
	int text_out;
	int dry_run;
} OapiOpts;

static void oapi_header_add(OapiOpts *o, const char *h)
{
	if (o->n_headers < OAPI_MAX_HEADERS)
		o->headers[o->n_headers++] = h;
}

static int oapi_cmd_call(OapiRegistry *r, const char *opname,
			 int argc, char **argv, OapiOpts *opts)
{
	OapiOps ops;
	OapiOp *op;
	JsonVal params[64];
	int n_params = 0;
	char *flags[64];
	char *flag_vals[64];
	int n_flags = 0;
	const char *positional[16];
	int n_pos = 0;
	int i;
	StrBuf url;
	StrBuf query;
	StrBuf body;
	char *base;
	int rc = 1;
	int need_pos = 0;

	oapi_ops_build(&ops, r->spec);
	op = oapi_ops_find(&ops, opname);
	if (!op) {
		bb_error_msg("operation '%s' not found (see: oapi schema)",
			     opname);
		goto out;
	}

	/* split argv: known global flags, --op-flags (with values), positionals */
	for (i = 0; i < argc; i++) {
		char *a = argv[i];
		if (a[0] == '-' && a[1] == '-' && a[2]) {
			if (strcmp(a, "--dry-run") == 0) { opts->dry_run = 1; continue; }
			if (strcmp(a, "--body") == 0 && argv[i+1]) { opts->body = argv[++i]; continue; }
			if (strcmp(a, "--jq") == 0 && argv[i+1]) { opts->jq = argv[++i]; continue; }
			if (strcmp(a, "--header") == 0 && argv[i+1]) { oapi_header_add(opts, argv[++i]); continue; }
			if (strcmp(a, "--timeout") == 0 && argv[i+1]) { i++; continue; }
			if (strcmp(a, "--insecure") == 0) { continue; }
			if (strcmp(a, "--text") == 0) { opts->text_out = 1; continue; }
			if (strncmp(a, "--header=", 9) == 0) { oapi_header_add(opts, a + 9); continue; }
			if (strncmp(a, "--jq=", 5) == 0) { opts->jq = a + 5; continue; }
			/* operation flag: --name or --name=value */
			{
				char *eq = strchr(a + 2, '=');
				if (n_flags < 64) {
					if (eq) {
						flags[n_flags] = xstrndup(a + 2, eq - a - 2);
						flag_vals[n_flags] = xstrdup(eq + 1);
					} else {
						flags[n_flags] = xstrdup(a + 2);
						flag_vals[n_flags] = NULL;
					}
					n_flags++;
					/* consume the next argv as value unless it looks
					 * like another flag; boolean flags (no value)
					 * match spec type=boolean */
					if (!eq && argv[i+1] && !(argv[i+1][0] == '-' && argv[i+1][1]))
						flag_vals[n_flags-1] = xstrdup(argv[++i]);
				}
				continue;
			}
		}
		if (a[0] == '-' && a[1] == 'F' && a[2]) {
			char *eq = strchr(a + 2, '=');
			if (eq && opts->n_f < 32) {
				opts->f_keys[opts->n_f] = xstrndup(a + 2, eq - a - 2);
				opts->f_vals[opts->n_f] = xstrdup(eq + 1);
				opts->n_f++;
			}
			continue;
		}
		if (strcmp(a, "-F") == 0 && argv[i+1]) {
			char *nxt = argv[++i];
			char *eq = strchr(nxt, '=');
			if (eq && opts->n_f < 32) {
				opts->f_keys[opts->n_f] = xstrndup(nxt, eq - nxt);
				opts->f_vals[opts->n_f] = xstrdup(eq + 1);
				opts->n_f++;
			}
			continue;
		}
		if (strcmp(a, "-o") == 0 && argv[i+1]) {
			opts->text_out = (strcmp(argv[++i], "text") == 0);
			continue;
		}
		if (n_pos < 16)
			positional[n_pos++] = a;
	}

	/* flatten parameters (path-level + operation-level, refs folded) */
	if (op->params.type == JSON_ARRAY) {
		for (i = 0; i < json_array_len(op->params) && n_params < 64; i++) {
			JsonVal p = oapi_param_ref(r->spec, json_array_get(op->params, i));
			if (p.type == JSON_OBJECT)
				params[n_params++] = p;
		}
	}
	need_pos = 0;
	for (i = 0; i < n_params; i++) {
		char *in = json_get_string(params[i], "in");
		int is_path = (in && strcmp(in, "path") == 0);
		free(in);
		if (is_path)
			need_pos++;
	}
	if (n_pos < need_pos) {
		bb_error_msg("%s needs %d positional path parameter(s), got %d",
			     opname, need_pos, n_pos);
		goto out_free;
	}

	/* url: base + filled path */
	base = oapi_base_url(r);
	if (!base) {
		bb_error_msg("spec has no usable server url");
		goto out_free;
	}
	sb_init(&url);
	{
		/* strip trailing slash duplication */
		size_t bl = strlen(base);
		sb_append(&url, base);
		if (bl && base[bl-1] == '/')
			sb_truncate(&url, url.len - 1);
	}
	{
		int pi = 0;
		const char *p = op->path;
		while (*p) {
			if (*p == '{') {
				const char *e = strchr(p, '}');
				char name[128];
				size_t nl;
				if (!e)
					break;
				nl = e - p - 1;
				if (nl >= sizeof(name))
					nl = sizeof(name) - 1;
				memcpy(name, p + 1, nl);
				name[nl] = '\0';
				/* path params are positional, in template order */
				if (pi < n_pos) {
					oapi_encode(&url, positional[pi]);
					pi++;
				} else {
					bb_error_msg("missing value for path parameter {%s}", name);
					sb_free(&url);
					goto out_free;
				}
				p = e + 1;
			} else {
				sb_append_char(&url, *p);
				p++;
			}
		}
	}

	/* query/header flags per spec */
	sb_init(&query);
	for (i = 0; i < n_flags; i++) {
		int j;
		int matched = 0;
		for (j = 0; j < n_params; j++) {
			char *nm = json_get_string(params[j], "name");
			char *in = json_get_string(params[j], "in");
			char *kb;
			int hit;
			if (!nm) { free(in); continue; }
			kb = oapi_kebab(nm);
			hit = (strcmp(kb, flags[i]) == 0 || strcmp(nm, flags[i]) == 0);
			free(kb);
			if (!hit) { free(nm); free(in); continue; }
			if (in && strcmp(in, "header") == 0) {
				char *h = xasprintf("%s: %s", nm,
						    flag_vals[i] ? flag_vals[i] : "");
				oapi_header_add(opts, h);   /* leaks until exit: fine for a CLI */
				matched = 1;
			} else {
				/* query (default) */
				if (query.len)
					sb_append_char(&query, '&');
				oapi_encode(&query, nm);
				sb_append_char(&query, '=');
				oapi_encode(&query, flag_vals[i] ? flag_vals[i] : "");
				matched = 1;
			}
			free(nm);
			free(in);
			if (matched)
				break;
		}
		if (!matched) {
			bb_error_msg("unknown flag --%s (see: oapi schema for parameters)",
				     flags[i]);
			sb_free(&url);
			sb_free(&query);
			goto out_free;
		}
	}

	/* body: --body or -F fields */
	sb_init(&body);
	if (opts->body) {
		if (opts->body[0] == '@') {
			char *data = xmalloc_xopen_read_close(opts->body + 1, NULL);
			if (!data) {
				bb_perror_msg("%s", opts->body + 1);
				sb_free(&url); sb_free(&query); sb_free(&body);
				goto out_free;
			}
			sb_append(&body, data);
			free(data);
		} else if (strcmp(opts->body, "-") == 0) {
			char *data = xmalloc_read(STDIN_FILENO, NULL);
			sb_append(&body, data ? data : "");
			free(data);
		} else {
			sb_append(&body, opts->body);
		}
	} else if (opts->n_f) {
		sb_append_char(&body, '{');
		for (i = 0; i < opts->n_f; i++) {
			if (i)
				sb_append_char(&body, ',');
			sb_append_char(&body, '"');
			sb_append(&body, opts->f_keys[i]);
			sb_append(&body, "\":");
			oapi_append_typed(&body, opts->f_vals[i]);
		}
		sb_append_char(&body, '}');
	}

	if (query.len) {
		sb_append_char(&url, '?');
		sb_append(&url, query.data);
	}

	if (body.len)
		oapi_header_add(opts, "Content-Type: application/json");

	/* send */
	if (opts->dry_run) {
		printf("%s %s\n", op->method, url.data);
		for (i = 0; i < opts->n_headers; i++)
			printf("  %s\n", opts->headers[i]);
		if (body.len)
			printf("  body: %s\n", body.data);
		rc = 0;
	} else {
		AgcHttpResp resp;
		int irc = agc_http_request(op->method, url.data,
					   opts->headers, opts->n_headers,
					   body.data ? body.data : "",
					   body.len, &resp);
		if (irc != 0) {
			bb_error_msg("request failed (transport error)");
			goto out_send;
		}
		if (resp.status < 200 || resp.status >= 300) {
			bb_error_msg("HTTP %d: %.*s", resp.status,
				     resp.body ? (int)strnlen(resp.body, 512) : 0,
				     resp.body ? resp.body : "");
			agc_http_resp_free(&resp);
			goto out_send;
		}
		if (opts->jq) {
			JsonParse jp = json_parse_root(resp.body ? resp.body : "");
			if (jp.error) {
				bb_error_msg("response is not JSON (--jq needs JSON)");
				agc_http_resp_free(&resp);
				goto out_send;
			}
			if (oapi_jq_print(jp.val, opts->jq) != 0) {
				bb_error_msg("--jq path '%s' not found", opts->jq);
				agc_http_resp_free(&resp);
				goto out_send;
			}
		} else if (opts->text_out) {
			JsonParse jp = json_parse_root(resp.body ? resp.body : "");
			if (!jp.error)
				oapi_text_print(jp.val);
			else
				fputs(resp.body ? resp.body : "", stdout);
		} else {
			fputs(resp.body ? resp.body : "", stdout);
			if (!resp.body_len || resp.body[resp.body_len-1] != '\n')
				fputc('\n', stdout);
		}
		agc_http_resp_free(&resp);
		rc = 0;
	}
 out_send:
	sb_free(&url);
	sb_free(&query);
	sb_free(&body);
 out_free:
	for (i = 0; i < n_flags; i++) {
		free(flags[i]);
		free(flag_vals[i]);
	}
	for (i = 0; i < opts->n_f; i++) {
		free(opts->f_keys[i]);
		free(opts->f_vals[i]);
	}
	opts->n_f = 0;
 out:
	oapi_ops_free(&ops);
	return rc;
}

/* raw escape hatch: oapi api NAME METHOD PATH [--params J] [--data J] */
static int oapi_cmd_api(OapiRegistry *r, const char *method, const char *path,
			int argc, char **argv, OapiOpts *opts)
{
	char *base = oapi_base_url(r);
	StrBuf url;
	const char *params = NULL;
	const char *data = NULL;
	AgcHttpResp resp;
	int i;
	int irc;

	if (!base) {
		bb_error_msg("spec has no usable server url");
		return 1;
	}
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--params") == 0 && argv[i+1])
			params = argv[++i];
		else if (strcmp(argv[i], "--data") == 0 && argv[i+1])
			data = argv[++i];
	}
	sb_init(&url);
	sb_append(&url, base);
	{
		/* avoid "//" when both base ends and path starts with "/" */
		size_t bl = strlen(base);
		const char *p2 = path;
		if (bl && base[bl-1] == '/' && path[0] == '/')
			p2++;
		sb_append(&url, p2);
	}
	if (params && params[0]) {
		sb_append_char(&url, '?');
		sb_append(&url, params);
	}
	if (data && data[0])
		oapi_header_add(opts, "Content-Type: application/json");
	if (opts->dry_run) {
		printf("%s %s\n", method, url.data);
		for (i = 0; i < opts->n_headers; i++)
			printf("  %s\n", opts->headers[i]);
		if (data)
			printf("  body: %s\n", data);
		sb_free(&url);
		free(base);
		return 0;
	}
	irc = agc_http_request(method, url.data, opts->headers, opts->n_headers,
			       data ? data : "", data ? strlen(data) : 0, &resp);
	sb_free(&url);
	free(base);
	if (irc != 0) {
		bb_error_msg("request failed (transport error)");
		return 1;
	}
	if (resp.status < 200 || resp.status >= 300) {
		bb_error_msg("HTTP %d: %.*s", resp.status,
			     resp.body ? (int)strnlen(resp.body, 512) : 0,
			     resp.body ? resp.body : "");
		agc_http_resp_free(&resp);
		return 1;
	}
	fputs(resp.body ? resp.body : "", stdout);
	if (!resp.body_len || resp.body[resp.body_len-1] != '\n')
		fputc('\n', stdout);
	agc_http_resp_free(&resp);
	return 0;
}

int oapi_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;

int oapi_main(int argc UNUSED_PARAM, char **argv)
{
	OapiOpts opts;
	const char *name;
	OapiRegistry r;

	memset(&opts, 0, sizeof(opts));

	if (!argv[1])
		bb_show_usage();

	/* leading global flags (dynamic operation flags rule out getopt32) */
	{
		int i = 1;
		while (argv[i] && argv[i][0] == '-' && argv[i][1] == '-') {
			if (strcmp(argv[i], "--dry-run") == 0) {
				opts.dry_run = 1;
				i++;
			} else if (strcmp(argv[i], "--insecure") == 0) {
				i++;
			} else if (strncmp(argv[i], "--header=", 9) == 0) {
				oapi_header_add(&opts, argv[i] + 9);
				i++;
			} else if (strcmp(argv[i], "--header") == 0 && argv[i+1]) {
				oapi_header_add(&opts, argv[i+1]);
				i += 2;
			} else {
				break;
			}
		}
		argv += i - 1;   /* argv[1] is now the subcommand or API name */
		argc -= i - 1;
	}

	if (strcmp(argv[1], "connect") == 0) {
		if (!argv[2] || !argv[3] || argv[4])
			bb_error_msg_and_die("usage: oapi connect NAME SPEC-URL-OR-FILE");
		return oapi_cmd_connect(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "sync") == 0) {
		int rc;
		if (!argv[2] || argv[3])
			bb_error_msg_and_die("usage: oapi sync NAME");
		if (oapi_load(&r, argv[2]) != 0)
			return 1;
		if (!r.source || !r.source[0])
			bb_error_msg_and_die("%s: registry entry has no source", argv[2]);
		rc = oapi_cmd_connect(argv[2], r.source);
		oapi_free(&r);
		return rc;
	}
	if (strcmp(argv[1], "ls") == 0)
		return oapi_cmd_ls();
	if (strcmp(argv[1], "rm") == 0) {
		char *p;
		if (!argv[2] || argv[3])
			bb_error_msg_and_die("usage: oapi rm NAME");
		p = oapi_reg_path(argv[2]);
		if (!p)
			bb_error_msg_and_die("bad name '%s'", argv[2]);
		if (unlink(p) != 0) {
			bb_perror_msg("rm %s", p);
			free(p);
			return 1;
		}
		free(p);
		return 0;
	}

	name = argv[1];
	if (strcmp(name, "schema") == 0) {
		int rc;
		if (argc < 3 || argc > 4)
			bb_error_msg_and_die("usage: oapi schema NAME [OPERATION]");
		if (oapi_load(&r, argv[2]) != 0)
			return 1;
		rc = oapi_cmd_schema(&r, argv[3]);
		oapi_free(&r);
		return rc;
	}
	if (strcmp(name, "api") == 0) {
		int rc;
		if (argc < 5)
			bb_error_msg_and_die("usage: oapi api NAME METHOD PATH");
		if (oapi_load(&r, argv[2]) != 0)
			return 1;
		rc = oapi_cmd_api(&r, argv[3], argv[4], argc - 5, argv + 5, &opts);
		oapi_free(&r);
		return rc;
	}
	if (oapi_load(&r, name) != 0) {
		bb_error_msg("unknown API '%s' (connect first: oapi connect NAME SPEC)", name);
		return 1;
	}

	if (!argv[2])
		bb_error_msg_and_die("no operation (see: oapi schema %s)", name);
	{
		int rc = oapi_cmd_call(&r, argv[2], argc - 3, argv + 3, &opts);
		oapi_free(&r);
		return rc;
	}
}
