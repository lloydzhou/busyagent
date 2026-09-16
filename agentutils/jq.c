/* jq - a tiny jq: extract values from JSON with a path expression.
 *
 * This is the path-subset of jq: ".a.b[0].c", ".items[].id", ".[]".
 * Missing keys and out-of-range indices yield null (like jq); "[]" over
 * a non-array yields nothing. Pipes, construction, arithmetic and
 * filters are out of scope - the evaluator lives in agent_common.c
 * (agc_json_path) and is shared with "oapi --jq".
 */
//config:config JQ
//config:	bool "jq (6 kb)"
//config:	default y
//config:	select AGENTUTILS_COMMON
//config:	help
//config:	  A tiny jq: extract values from JSON with a path expression.
//config:	  jq '.items[].id' file.json, echo '{...}' | jq -r '.name'.
//config:	  Path subset only (no pipes/filters).

//applet:IF_JQ(APPLET(jq, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_JQ) += jq.o

//usage:#define jq_trivial_usage
//usage:       "[-rec] [PATH [FILE]]"
//usage:#define jq_full_usage "\n\n"
//usage:       "Extract values from JSON with a path expression\n"
//usage:       "\n"
//usage:       "	-r	Output string values raw (no quotes)\n"
//usage:       "	-e	Exit 1 if last output false/null, 4 if none\n"
//usage:       "	-c	Compact output (single line, no indentation)\n"
//usage:       "\n"
//usage:       "PATH is a jq path subset: .a.b, .[0], .[], .items[].id.\n"
//usage:       "PATH defaults to '.' (pretty-print, like jq 1.7+).\n"
//usage:       "Missing keys / bad indices give null; [] over a non-array\n"
//usage:       "gives nothing. Objects/arrays pretty-print (2-space indent)\n"
//usage:       "unless -c. Reads stdin when FILE is absent or '-'."

#include "busyagent.h"
#include "agent_common.h"
#include "libbb.h"
#include "busybox.h"

int jq_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;

int jq_main(int argc, char **argv)
{
	unsigned opts;
	const char *path;
	const char *file;
	char *data;
	JsonParse jp;
	AgcJqMatches m;
	int i;
	int printed = 0;
	int last_falsy = 0;   /* -e: last output was false or null */
	int nargs;

	opts = getopt32(argv, "rec");
	nargs = argc - optind;
	argv += optind;
	if (nargs > 2)
		bb_error_msg_and_die("too many arguments");
	/* jq 1.7+ runs the identity filter when no PATH is given */
	path = (nargs >= 1) ? argv[0] : ".";
	file = (nargs >= 2) ? argv[1] : NULL;

	if (!file || strcmp(file, "-") == 0) {
		data = xmalloc_read(STDIN_FILENO, NULL);
	} else {
		FILE *f = fopen(file, "r");
		long sz;

		if (!f)
			bb_perror_msg_and_die("open %s", file);
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz < 0)
			bb_perror_msg_and_die("read %s", file);
		data = xmalloc(sz + 1);
		{
			size_t got = fread(data, 1, sz, f);
			fclose(f);
			data[got] = '\0';
		}
	}
	if (!data)
		bb_error_msg_and_die("no input");

	jp = json_parse_root(data);
	if (jp.error)
		bb_error_msg_and_die("invalid JSON: %s", jp.error);

	if (agc_json_path(jp.val, path, &m) != 0)
		bb_error_msg_and_die("bad path '%s'", path);

	for (i = 0; i < m.n; i++) {
		JsonVal v = m.v[i];

		if (v.type == JSON_NULL) {
			puts("null");
			printed = 1;
			last_falsy = 1;
		} else if ((opts & 1) && v.type == JSON_STRING) {
			/* -r: top-level selected string, decoded, no quotes;
			 * the explicit length keeps embedded NULs intact */
			size_t raw = (v.end - 1) - (v.start + 1);
			char *s = xmalloc(raw + 1);
			size_t n = json_string_decode(v, s);

			fwrite(s, 1, n, stdout);
			putchar('\n');
			free(s);
			printed = 1;
			last_falsy = 0;
		} else {
			/* pretty by default, -c asks for the compact form;
			 * container contents always stay valid JSON */
			StrBuf out;

			sb_init(&out);
			agc_json_print(&out, v, (opts & 4) ? -1 : 0);
			sb_appendn(&out, "\n", 1);
			if (out.len)
				fwrite(out.data, 1, out.len, stdout);
			sb_free(&out);
			printed = 1;
			last_falsy = (v.type == JSON_BOOL
				      && !json_bool_val(v));
		}
	}
	free(m.v);
	free(data);
	if (opts & 2) {
		/* jq 1.7: 1 when the last output was false/null,
		 * 4 when nothing was ever printed */
		if (!printed)
			return 4;
		if (last_falsy)
			return 1;
	}
	return 0;
}
