#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define MAX_LINE 8192

void print_usage(const char *prog, int rc)
{
	fprintf(rc ? stderr : stdout,
		"Usage: %s [-h] [-L <bytes>] [-H <header_line>]* [-B <body_line>]*\n"
		"  -L <bytes>   re-emit Content-Length = <bytes> + the body bytes added below\n"
		"  -H <header>  add a header line at the end of the header block\n"
		"  -B <line>    prepend a line to the body\n"
		"  -h           show this help\n"
		"Reads an mbox from stdin, writes to stdout.\n",
		prog);
	exit(rc);
}

void print_args(int argc, char *argv[], const char *flag)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], flag) == 0) {
			if (i + 1 < argc)
				puts(argv[++i]);
		}
	}
}

/* Number of body bytes the -B options will add. puts() appends a newline to
 * each emitted line, hence the +1.
 */
long added_body_bytes(int argc, char *argv[])
{
	long n = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-B") == 0) {
			if (i + 1 < argc)
				n += strlen(argv[++i]) + 1;
		}
	}
	return n;
}

int main(int argc, char *argv[])
{
	char line[MAX_LINE];
	int in_headers = 0;
	int done = 0;		/* set once we've annotated the first message */
	long base_clen = -1;	/* base Content-Length given with -L, or -1 */
	long final_clen = 0;
	int arg;

	/* Parse the options one at a time. -H and -B may repeat and are emitted
	 * later, in order, by print_args(), so here we only validate them and
	 * skip over their value. An unknown option, a missing value, or a
	 * positional argument shows the usage.
	 */
	for (arg = 1; arg < argc; arg++) {
		if (argv[arg][0] != '-')
			print_usage(argv[0], 1);
		switch (argv[arg][1]) {
		case 'h':
			print_usage(argv[0], 0);
			break;
		case 'L':
			if (arg + 1 >= argc)
				print_usage(argv[0], 1);
			base_clen = atol(argv[arg + 1]);
			arg++;
			break;
		case 'H':
		case 'B':
			if (arg + 1 >= argc)
				print_usage(argv[0], 1);
			arg++;
			break;
		default:
			print_usage(argv[0], 1);
		}
	}

	/* -L gives the body length produced upstream (by textonly). The final
	 * Content-Length is that base plus whatever the -B options add.
	 */
	if (base_clen >= 0)
		final_clen = base_clen + added_body_bytes(argc, argv);

	while (fgets(line, sizeof(line), stdin)) {
		/* We only annotate the first message. Once that's done, the
		 * rest (including any "From " lines inlined in the body) is
		 * passed through verbatim so that inlined patches don't trigger
		 * a spurious second insertion.
		 */
		if (done) {
			printf("%s", line);
			continue;
		}

		if (strncmp(line, "From ", 5) == 0) {
			in_headers = 1;
			printf("%s", line);
			continue;
		}

		if (in_headers) {
			/* drop any stale Content-Length; we re-introduce our own */
			if (strncasecmp(line, "Content-Length:", 15) == 0)
				continue;

			if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) {
				/* End of headers, append desired headers */
				print_args(argc, argv, "-H");

				/* re-introduce an accurate Content-Length if asked */
				if (base_clen >= 0) {
					if (strcmp(line, "\r\n") == 0)
						printf("Content-Length: %ld\r\n", final_clen);
					else
						printf("Content-Length: %ld\n", final_clen);
				}

				printf("%s", line);

				/* and prepend desired body lines */
				print_args(argc, argv, "-B");

				in_headers = 0;
				done = 1;
				continue;
			}
		}

		printf("%s", line);
	}

	return 0;
}
