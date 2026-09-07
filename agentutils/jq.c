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
//usage:       "[-re] PATH [FILE]"
//usage:#define jq_full_usage "\n\n"
//usage:       "Extract values from JSON with a path expression\n"
//usage:       "\n"
//usage:       "	-r	Output string values raw (no quotes)\n"
//usage:       "	-e	Exit 1 when nothing was printed\n"
//usage:       "\n"
//usage:       "PATH is a jq path subset: .a.b, .[0], .[], .items[].id.\n"
//usage:       "Missing keys / bad indices give null; [] over a non-array\n"
//usage:       "gives nothing. Reads stdin when FILE is absent or '-'."

#include "busyagent.h"
#include "agent_common.h"
#include "libbb.h"
#include "busybox.h"

int jq_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;

int jq_main(int argc UNUSED_PARAM, char **argv)
{
	unsigned opts;
	const char *path;
	char *data;
	JsonParse jp;
	AgcJqMatches m;
	int i;
	int printed = 0;

	opts = getopt32(argv, "re");
	argv += optind;
	if (!argv[0])
		bb_error_msg_and_die("need a PATH (e.g. '.items[].id')");
	if (argv[1] && argv[2])
		bb_error_msg_and_die("too many arguments");
	path = argv[0];

	if (!argv[1] || strcmp(argv[1], "-") == 0) {
		data = xmalloc_read(STDIN_FILENO, NULL);
	} else {
		FILE *f = fopen(argv[1], "r");
		long sz;

		if (!f)
			bb_perror_msg_and_die("open %s", argv[1]);
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz < 0)
			bb_perror_msg_and_die("read %s", argv[1]);
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
		} else if ((opts & 1) && v.type == JSON_STRING) {
			/* -r: raw string */
			char *s = json_string_val(v);
			puts(s ? s : "");
			free(s);
			printed = 1;
		} else {
			printf("%.*s\n", (int)(v.end - v.start), v.src + v.start);
			printed = 1;
		}
	}
	free(m.v);
	free(data);
	return (opts & 2 && !printed) ? 1 : 0;
}
