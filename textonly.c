#include <stdio.h>
#include <string.h>

#define MAX_LINE 4096
#define MAX_STACK 10

/* extract the boundary from <in> which must start at "boundary=" into <store>
 * of size <size>. It takes care of quoted strings.
 */
void store_boundary(const char *in, char *store, size_t size)
{
	const char *end = in;

	in += strlen("boundary=");
	if (*in == '"')
		in++;
	for (end = in; *end && *end != '"' && *end > ' '; end++)
		;
	if (end - in >= size)
		end = in + size - 1;
	memcpy(store, in, end - in);
	store[end - in] = '\0';
}

/* returns true if CR or LF */
int is_crlf(int ch)
{
	return ch == '\r' || ch == '\n';
}

/* returns true if SP or HT */
int is_spht(int ch)
{
	return ch == ' ' || ch == '\t';
}

/* reads a possibly multi-line header from <in> and returns it, or NULL if end
 * reached. It requires 2 buffers, one for the currently assembled line, and
 * one for the next one, both of size <size>. The caller is responsible for
 * making sure that <next> is empty on the first call. It will return an empty
 * string once the input is depleted.
 */
const char *read_hdr(FILE *in, char *curr, char *next, int size)
{
	int ofs = 0;
	int ret;

	/* both are the same size, this fits */
	ofs = snprintf(curr, size, "%s", next);
	for (*next = 0;
	     fgets(next, size, in) && (!ofs || is_spht(*next)) && ofs < size;
	     *next = 0, ofs += ret) {
		ret = snprintf(curr + ofs, size - ofs, "%s", next);
		if (ret <= 0 || ret > (size - ofs))
			break;
	}
	//printf("### returning curr=<%s> next=<%s>\n", curr, next);
	return curr;
}

void process_mbox(FILE *in)
{
	char line[MAX_LINE], next[MAX_LINE];
	char boundaries[MAX_STACK][MAX_LINE];
	int stack_ptr = -1;
	int is_multipart = 0;
	int found_and_dumped = 0;
	const char *boundary;
	char part_hdrs[4*MAX_LINE]; // should be sufficient for a few headers
	int part_hdr_len = 0;

	while (*read_hdr(in, line, next, sizeof(line))) {
		if (strncmp(line, "From ", 5) == 0) {
			/* new mail */
			printf("%s", line);
			stack_ptr = -1;
			is_multipart = 0;
			found_and_dumped = 0;

			/* 1. HEADER: drop content-length, lines, and look for
			 * content-type. If multipart, we'll inspect attachments.
			 * We stop before the empty line. Note that CR/LF are
			 * part of the line here.
			 */
			while (*read_hdr(in, line, next, sizeof(line)) && !is_crlf(line[0])) {
				if (strncasecmp(line, "Content-Length:", 15) == 0)
					continue;

				if (strncasecmp(line, "Lines:", 6) == 0)
					continue;

				if (strncasecmp(line, "Content-Type: multipart", 23) == 0) {
					/* the boundary is on this line */
					is_multipart = 1;
					boundary = strstr(line, "boundary=");
					if (boundary && stack_ptr < MAX_STACK - 1) {
						stack_ptr++;
						store_boundary(boundary, boundaries[stack_ptr], sizeof(boundaries[stack_ptr]));
						//printf("### boundaries[%d]=%s\n", stack_ptr, boundaries[stack_ptr]);
					}
					continue;
				}
				printf("%s", line);
			}

			/* We've reached the empty line, we're now inspecting
			 * the body. If not multipart, we dump everything and
			 * we're done till the next message. We have the next
			 * line in <next>.
			 */
			if (!is_multipart) {
				printf("%s", line);
				while (*next && strncmp(next, "From ", 5) != 0) {
					read_hdr(in, line, next, sizeof(line));
					printf("%s", line);
				}
			} else {
				/* Multipart: let's not emit the empty line yet
				 * because we want to append the headers of the
				 * first text/plain part. The boundary appears as
				 * the first line of header, starting with "--"
				 * suffixed by the programmed boundary. Be
				 * careful, some mailers set boundaries starting
				 * with "--".
				 */
				while (!found_and_dumped &&
				       strncmp(next, "From ", 5) != 0 &&
				       *read_hdr(in, line, next, sizeof(line))) {
					// Check for any known boundary
					if (line[0] == '-' && line[1] == '-' && stack_ptr >= 0) {
						int i, lvl = -1;
						int is_text_plain = 0;

						/* check if this looks like a known boundary */
						for (lvl = stack_ptr; lvl >= 0; lvl--) {
							if (strncmp(line + 2, boundaries[lvl], strlen(boundaries[lvl])) == 0)
								break;
						}

						if (lvl < 0)
							continue;

						/* this matches a known boundary, adjust the current stack
						 * level and skip that line.
						 */
						if (strncmp(line, boundaries[lvl], strlen(boundaries[lvl])) == 0) {
							stack_ptr = lvl - 1; // Close this level and all nested levels
							continue;
						}

						part_hdr_len = 0;
						part_hdrs[0] = 0;

						/* now time to inspect part-headers */
						while (*read_hdr(in, line, next, sizeof(line))) {
							int ret;

							if (strncasecmp(line, "Content-Type: multipart", 23) == 0 && stack_ptr < MAX_STACK - 1) {
								/* this is a nested multipart, the parent is likely multipart/alternative */
								boundary = strstr(line, "boundary=");
								if (boundary) {
									stack_ptr++;
									store_boundary(boundary, boundaries[stack_ptr], sizeof(boundaries[stack_ptr]));
								}
								/* this format is recursive, we're supposed to have
								 * other optional headers, a blank line, a boundary,
								 * and headers. It's easier to explicitly match them
								 * here.
								 */
								while (*read_hdr(in, line, next, sizeof(line))) {
									if (line[0] == '-' && line[1] == '-') {
										for (i = 0; i <= stack_ptr; i++)
											if (strncmp(line + 2, boundaries[i], strlen(boundaries[i])) == 0)
												break;
										if (i <= stack_ptr)
											break;
									}
								}
								/* we've skipped all multipart headers, the blank
								 * line, and the boundary, so we should now expect
								 * the part's headers.
								 */
								part_hdr_len = 0;
								part_hdrs[0] = 0;
								continue;
							}

							ret = snprintf(part_hdrs + part_hdr_len, sizeof(part_hdrs) - part_hdr_len, "%s", line);
							if (ret >= 0 && ret < sizeof(part_hdrs) - part_hdr_len)
								part_hdr_len += ret;

							if (is_crlf(line[0]))
								break;

							if (strncasecmp(line, "Content-Type: text/plain", 24) == 0)
								is_text_plain = 1;
						}

						if (is_text_plain) {
							/* OK that's finally text/plain, we're going to dump its
							 * headers so that we have the content-type and even the
							 * content-transfer-encoding.
							 */
							printf("%s", part_hdrs);
							while (*read_hdr(in, line, next, sizeof(line))) {
								if (line[0] == '-' && line[1] == '-') {
									for (i = 0; i <= stack_ptr; i++)
										if (strncmp(line + 2, boundaries[i], strlen(boundaries[i])) == 0)
											break;
									if (i <= stack_ptr)
										break;
								}
								printf("%s", line);
							}
							found_and_dumped = 1;
						}
					}
				}
                
				/* If we found nothing, ensure a blank line exists */
				if (!found_and_dumped)
					printf("\n");

				/* skip to end or next mail */
				while (*next && strncmp(next, "From ", 5) != 0)
					read_hdr(in, line, next, sizeof(line));
			}
		}
	}
}

int main(int argc, char *argv[])
{
	if (argc < 2)
		process_mbox(stdin);
	else {
		FILE *f = fopen(argv[1], "r");

		if (!f)
			return 1;
		process_mbox(f);
		fclose(f);
	}
	return 0;
}
