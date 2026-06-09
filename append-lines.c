#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_LINE 8192

void print_usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [-H <header_line>]* [-B <body_line>]*\n", progname);
	fprintf(stderr, "Reads mbox from stdin, writes to stdout.\n");
	exit(1);
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

int main(int argc, char *argv[])
{
	char line[MAX_LINE];
	int in_headers = 0;

	if (argc > 1) {
		if (strcmp(argv[1], "-H") != 0 && strcmp(argv[1], "-B") != 0)
			print_usage(argv[0]);
	}

	while (fgets(line, sizeof(line), stdin)) {
		if (strncmp(line, "From ", 5) == 0) {
			in_headers = 1;
			printf("%s", line);
			continue;
		}

		if (in_headers) {
			if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) {
				/* End of headers, append desired headers */
				print_args(argc, argv, "-H");

				printf("%s", line);

				/* and prepend desired body lines */
				print_args(argc, argv, "-B");

				in_headers = 0;
				continue;
			}
		}

		printf("%s", line);
	}

	return 0;
}
