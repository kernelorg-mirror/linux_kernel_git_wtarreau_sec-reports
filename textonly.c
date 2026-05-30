#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define MAX_LINE 4096
#define MAX_STACK 10

int do_clean_hdr = 0;
char *clen_file = NULL;	/* if set, write the produced body length here */
long tot_body_bytes = 0;	/* number of body bytes emitted for the current message */

/* emit a string as body content, counting the bytes so that we can report the
 * resulting Content-Length.
 */
void put_body_str(const char *s)
{
	fputs(s, stdout);
	tot_body_bytes += strlen(s);
}

/* emit a single byte as body content, counting it for the Content-Length */
void put_body_ch(int c)
{
	putchar(c);
	tot_body_bytes++;
}

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

/* Complete word <w> by decoding char <c>. Returns the number of bits emitted
 * (either 0 or 6), or an error (-1) if the char is not a base64 one. If <bits>
 * is not NULL, it's incremented by the number of emitted bits. If <ofs> is
 * not NULL, it's incremented by one for valid chars.
 */
int b64dec(unsigned *word, int *bits, int *ofs, char c)
{
	int nbbits = 6;

	switch (c) {
	case 'A'...'Z':
		c += 0 - 'A';
		break;
	case 'a'...'z':
		c += 26 - 'a';
		break;
	case '0'...'9':
		c += 52 - '0';
		break;
	case '+':
		c = 62;
		break;
	case '/':
		c = 63;
		break;
	case '=':
		/* end of block, no more byte to emit */
		nbbits = 0;
		c = 0;
		break;
	default:
		return -1;
	}

	*word = (*word << 6) + (unsigned char)c;
	if (bits)
		*bits += nbbits;
	if (ofs)
		(*ofs)++;
	return nbbits;
}

/* returns the hex char value or -1 if not a hex char */
int h2i(char c)
{
	return (c >= '0' && c <= '9') ? c - '0' :
	       (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
	       (c >= 'A' && c <= 'F') ? c - 'A' + 10 :
	       -1;
}

/* Decode a single line of quoted-printable text and emits it on stdout. Note
 * that the spec says the input cannot be longer than 76 chars. Accepted line
 * endings are CR, LF or NUL.
 */
void decode_qp_line(const char *line)
{
	int h, l, i;

	for (i = 0; line[i] != '\r' && line[i] != '\n' && line[i] != '\0'; i++) {
		if (line[i] != '=') {
			put_body_ch(line[i]);
			continue;
		}
		if ((h = h2i(line[i + 1])) >= 0 && (l = h2i(line[i + 2])) >= 0) {
			put_body_ch((h << 4) + l);
			i += 2;
		}
		else if (line[i + 1] == '\r' || line[i + 1] == '\n' || line[i + 1] == '\0') {
			/* a lone '=' at the end of a line is a soft break: the
			 * line continues on the next one.
			 */
			return;
		}
		else {
			/* probably an encoding issue, let's dump the '='. */
			put_body_ch('=');
		}
	}
	/* input is one line at a time, and soft break has already been handled */
	put_body_ch('\n');
}

/* returns true if <hdr> starts with <start>, ignoring case */
int hdr_starts_with(const char *hdr, const char *start)
{
	do {
		if (!*start)
			return 1;
	} while (tolower(*hdr++) == tolower(*start++));
	return 0;
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

/* Tells whether the message body is over. <clen> is the number of body bytes
 * still to consume according to the Content-Length header, or negative if the
 * header was absent. With a Content-Length, the body ends exactly when the
 * count is exhausted, which is robust against "From " lines inlined in the body
 * (e.g. quoted mails or pasted patches). Without one, we fall back to the
 * historical heuristic: the body ends at the next mbox "From " line.
 */
int body_done(long clen, const char *line)
{
	if (clen >= 0)
		return clen <= 0;
	return strncmp(line, "From ", 5) == 0;
}

/* Account <line> as consumed body input against the Content-Length budget
 * <*clen>. No-op when no Content-Length was present (<*clen> negative).
 */
void consume(long *clen, const char *line)
{
	if (*clen >= 0)
		*clen -= strlen(line);
}

/* If <line> is a MIME boundary delimiter, i.e. "--" followed by one of the
 * boundaries currently on the stack (levels 0..<stack_ptr>), return its level,
 * else -1. The deepest matching level is returned so that an outer boundary
 * can close the inner levels it encloses.
 */
int boundary_level(const char *line, char boundaries[][MAX_LINE], int stack_ptr)
{
	int lvl;

	if (line[0] != '-' || line[1] != '-')
		return -1;
	for (lvl = stack_ptr; lvl >= 0; lvl--)
		if (strncmp(line + 2, boundaries[lvl], strlen(boundaries[lvl])) == 0)
			return lvl;
	return -1;
}

/* True if <line> is the closing delimiter "--<boundary>--" rather than a plain
 * part separator "--<boundary>". This does NOT verify that <line> matches
 * <boundary> at all: it only inspects the two bytes right after the boundary.
 * The caller must have already established, via boundary_level(), that <line>
 * begins with "--" followed by <boundary>; otherwise the result is meaningless.
 */
int is_close_delim(const char *line, const char *boundary)
{
	int n = strlen(boundary);

	return line[2 + n] == '-' && line[2 + n + 1] == '-';
}

/* Emit the body of a leaf (non-multipart) MIME part, decoding base64 or
 * quoted-printable as requested, and counting the produced bytes. Stops at a
 * known boundary delimiter (levels 0..<level>), when the Content-Length budget
 * <*clen> is exhausted, or at end of input.
 */
void dump_leaf(FILE *in, char *next, long *clen, char boundaries[][MAX_LINE],
               int level, int is_base64, int is_qp)
{
	unsigned base64_word = 0;
	int base64_ofs = 0;
	int bits = 0;
	char *c;

	while (*next) {
		if (boundary_level(next, boundaries, level) >= 0)
			break;
		if (*clen >= 0 && *clen <= 0)
			break;
		consume(clen, next);
		if (is_base64) {
			/* decode and dump accumulated base64 bytes */
			for (c = next; *c; c++) {
				if (b64dec(&base64_word, &bits, &base64_ofs, *c) < 0)
					continue;
				if (base64_ofs == 4) {
					if (bits >= 8)
						put_body_ch((unsigned char)(base64_word >> 16));
					if (bits >= 16)
						put_body_ch((unsigned char)(base64_word >> 8));
					if (bits >= 24)
						put_body_ch((unsigned char)base64_word);
					base64_ofs = 0;
					bits = 0;
				}
			}
		}
		else if (is_qp) {
			/* decode quoted printable */
			decode_qp_line(next);
		}
		else
			put_body_str(next);

		if (!fgets(next, MAX_LINE, in)) {
			*next = 0;
			break;
		}
	}
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
	int is_qp = 0;
	long clen = -1;	/* body bytes left to consume per Content-Length, or -1 */

	while (*read_hdr(in, line, next, sizeof(line))) {
		if (strncmp(line, "From ", 5) == 0) {
			/* new mail */
			printf("%s", line);
			stack_ptr = -1;
			is_multipart = 0;
			is_qp = 0;
			found_and_dumped = 0;
			clen = -1;
			tot_body_bytes = 0;

			/* 1. HEADER: capture and drop content-length, drop lines,
			 * and look for content-type. If multipart, we'll inspect
			 * attachments. We stop before the empty line. Note that
			 * CR/LF are part of the line here.
			 */
			while (*read_hdr(in, line, next, sizeof(line)) && !is_crlf(line[0])) {
				if (hdr_starts_with(line, "Content-Length:")) {
					/* keep the value to delimit the body, but
					 * don't emit it: the body we produce will
					 * have a different size.
					 */
					clen = atol(line + strlen("Content-Length:"));
					continue;
				}

				if (hdr_starts_with(line, "Lines:"))
					continue;

				if (hdr_starts_with(line, "Content-Type: multipart")) {
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

				if (hdr_starts_with(line, "Content-Transfer-Encoding: quoted-printable")) {
					is_qp = 1;
					continue;
				}

				if (do_clean_hdr) {
					if (hdr_starts_with(line, "Received"))
						continue;
					if (hdr_starts_with(line, "X-"))
						continue;
					if (hdr_starts_with(line, "Authentication-"))
						continue;
					if (hdr_starts_with(line, "ARC-"))
						continue;
					if (hdr_starts_with(line, "DKIM-"))
						continue;
					if (hdr_starts_with(line, "DMARC-"))
						continue;
				}
				printf("%s", line);
			}

			/* We've reached the empty line, we're now inspecting
			 * the body. If not multipart, we dump everything and
			 * we're done till the next message. We have the next
			 * line in <next>. The empty line is not part of the body
			 * counted by Content-Length.
			 */
			if (!is_multipart) {
				printf("%s", line);
				while (*next && !body_done(clen, next)) {
					consume(&clen, next);
					if (is_qp) {
						/* decode quoted printable */
						decode_qp_line(next);
					}
					else
						put_body_str(next);

					if (!fgets(next, sizeof(next), in)) {
						*next = 0;
						break;
					}
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
				while (!found_and_dumped && !body_done(clen, next) &&
				       *read_hdr(in, line, next, sizeof(line))) {
					int lvl;

					consume(&clen, line);

					// Check for any known boundary
					lvl = boundary_level(line, boundaries, stack_ptr);
					if (lvl >= 0) {
						int is_text_plain = 0;
						int is_base64 = 0;

						/* boundary_level() has already proven <line> is
						 * "--<boundaries[depth]>...". This is what tells us
						 * we hit a boundary at all. is_close_delim() does
						 * not re-check the boundary; it only looks at the
						 * bytes after it to tell a closing "--<boundary>--"
						 * from a separator "--<boundary>".
						 */
						if (is_close_delim(line, boundaries[lvl])) {
							stack_ptr = lvl - 1;
							continue;
						}
						stack_ptr = lvl;

						part_hdr_len = 0;
						part_hdrs[0] = 0;
						is_base64 = 0;
						is_qp = 0;

						/* now time to inspect part-headers */
						while (*read_hdr(in, line, next, sizeof(line))) {
							int ret;

							consume(&clen, line);

							if (hdr_starts_with(line, "Content-Type: multipart") && stack_ptr < MAX_STACK - 1) {
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
									consume(&clen, line);
									if (boundary_level(line, boundaries, stack_ptr) >= 0)
										break;
								}
								/* we've skipped all multipart headers, the blank
								 * line, and the boundary, so we should now expect
								 * the part's headers.
								 */
								part_hdr_len = 0;
								part_hdrs[0] = 0;
								continue;
							}

							if (hdr_starts_with(line, "Content-Transfer-Encoding: base64")) {
								is_base64 = 1;
								continue;
							}
							else if (hdr_starts_with(line, "Content-Transfer-Encoding: quoted-printable")) {
								is_qp = 1;
								continue;
							}

							/* the rest is always appended */
							ret = snprintf(part_hdrs + part_hdr_len, sizeof(part_hdrs) - part_hdr_len, "%s", line);
							if (ret >= 0 && ret < sizeof(part_hdrs) - part_hdr_len)
								part_hdr_len += ret;

							if (is_crlf(line[0]))
								break;

							if (hdr_starts_with(line, "Content-Type: text/plain"))
								is_text_plain = 1;
						}

						if (is_text_plain) {
							/* OK that's finally text/plain, we're going to dump its
							 * headers so that we have the content-type and even the
							 * content-transfer-encoding.
							 */
							printf("%s", part_hdrs);
							dump_leaf(in, next, &clen, boundaries, stack_ptr, is_base64, is_qp);
							found_and_dumped = 1;
						}
					}
				}

				/* If we found nothing, ensure a blank line exists */
				if (!found_and_dumped)
					printf("\n");

				/* skip to end or next mail */
				while (*next && !body_done(clen, next)) {
					read_hdr(in, line, next, sizeof(line));
					consume(&clen, line);
				}
			}

			/* report the number of body bytes we produced, so the
			 * next stage of the pipeline can compute an accurate
			 * Content-Length for the rewritten message.
			 */
			if (clen_file) {
				FILE *cf = fopen(clen_file, "w");

				if (cf) {
					fprintf(cf, "%ld\n", tot_body_bytes);
					fclose(cf);
				}
			}
		}
	}
}

int main(int argc, char *argv[])
{
	while (argc > 1 && argv[1][0] == '-') {
		if (strcmp(argv[1], "-c") == 0) {
			/* clean useless headers */
			do_clean_hdr = 1;
			argv++;
			argc--;
		} else if (strcmp(argv[1], "-w") == 0 && argc > 2) {
			/* write the produced body length to this file */
			clen_file = argv[2];
			argv += 2;
			argc -= 2;
		} else {
			break;
		}
	}

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
