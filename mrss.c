#define _XOPEN_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <libxml/tree.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

#include "sha1.h"
#include "version.h"
#include "mrss.h"

static char const MAIL_TMPNAME[] = "tmp/mrss-XXXXXX";

struct mail {
	FILE *stream;
	char path[sizeof MAIL_TMPNAME];
};

enum rfc822_type {
	RFC822_ATOM,
	RFC822_TEXT,
	RFC822_NONASCII,
};

typedef char HASH[16 + 1 /* NUL */];

static char const RFC_822[] = "%a, %d %b %Y %T %z";

static char opt_proxy[1024];
static char opt_from[128];
static int opt_reply_to = 1;
static int opt_verbose;

static CURL *curl;
static char curl_error_buf[CURL_ERROR_SIZE];

static time_t feed_old_latest;
static time_t feed_new_latest;

static void
media_destroy(struct media *media)
{
	xmlFree(media->content);
}

void
post_destroy(struct post *post)
{
	xmlFree(post->author);
	xmlFree(post->category);
	xmlFree(post->date);
	xmlFree(post->id);
	xmlFree(post->lang);
	xmlFree(post->link);
	xmlFree(post->subject);
	media_destroy(&post->text);
}

static void
error(char const *format, ...)
{
	fputs("mrss: ", stderr);
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);

	exit(EXIT_FAILURE);
}

static void
xsnprintf(char *buf, size_t buf_size, char const *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(buf, buf_size, format, ap);
	va_end(ap);
	if ((int)buf_size <= n)
		error("too long string: '%s'...", buf);
}

static FILE *
xfopen(char const *pathname, char const *mode)
{
	FILE *f = fopen(pathname, mode);
	if (!f)
		error("cannot open '%s': %s", pathname, strerror(errno));
	return f;
}

static FILE *
xftmpopen(char *template)
{
	int fd = mkstemp(template);
	if (fd < 0)
		error("cannot create temporary file: %s", strerror(errno));
	return fdopen(fd, "w+");
}

static void
xfclose(FILE *f, char const *pathname)
{
	if (fclose(f))
		error("cannot write '%s': %s", pathname, strerror(errno));
}

static void
xmkdir(char const *path)
{
	if (mkdir(path, S_IRWXU) && EEXIST != errno)
		error("cannot create '%s': %s", path, strerror(errno));
}

static void
xrename(char const *old, char const *new)
{
	if (rename(old, new))
		error("cannot rename '%s' -> '%s': %s",
				old, new, strerror(errno));
}

static void
xlink(char const *from, char const *to)
{
	if (link(from, to) && EEXIST != errno)
		error("cannot link '%s' -> '%s': %s",
				from, to, strerror(errno));
	(void)unlink(from);
}

static int
xfgets(char *s, int size, FILE *stream)
{
	while (fgets(s, size, stream)) {
		if ('#' == *s)
			continue;

		char *end = strchr(s, '\n');
		if (end)
			*end = '\0';
		if (!*s)
			continue;

		return 1;
	}
	return 0;
}

static time_t
parse_date(char const *s)
{
	static struct tm const ZERO_TM;

	struct tm tm;
	if (!strptime(s, RFC_822, (tm = ZERO_TM, &tm)) &&
	    !strptime(s, "%a, %d %b %Y %T %Z", (tm = ZERO_TM, &tm)) &&
	    !strptime(s, "%FT%TZ", (tm = ZERO_TM, &tm)) &&
	    !strptime(s, "%F", (tm = ZERO_TM, &tm)) &&
	    !strptime(s, "%D", (tm = ZERO_TM, &tm)))
		error("invalid date: '%s'", s);

	return mktime(&tm) - timezone;
}

static void
hash_from_sha1(HASH hash, BYTE bytes[static 16])
{
	static char const HEX[16] = "0123456789abcdef";

	for (int i = 0; i < 8; ++i) {
		BYTE b = bytes[i] ^ bytes[i + 8];
		hash[2 * i]     = HEX[b >> 4],
		hash[2 * i + 1] = HEX[b & 0xf];
	}

	hash[16] = '\0';
}

static void
sha1_update_strnull(SHA1_CTX *ctx, char const *s)
{
	if (s)
		sha1_update(ctx, (BYTE const *)s, strlen(s));
	sha1_update(ctx, (BYTE const *)"|", 1);
}

static void
hash_str(HASH hash, char const *s)
{
	SHA1_CTX ctx;
	sha1_init(&ctx);

	sha1_update_strnull(&ctx, s);

	BYTE bytes[16];
	sha1_final(&ctx, bytes);
	hash_from_sha1(hash, bytes);
}

static void
hash_post(HASH hash, struct post const *post, char const *source)
{
	SHA1_CTX ctx;
	sha1_init(&ctx);

	sha1_update_strnull(&ctx, (char const *)post->id);
	sha1_update_strnull(&ctx, (char const *)post->link);
	sha1_update_strnull(&ctx, (char const *)post->lang);
	sha1_update_strnull(&ctx, (char const *)post->subject);
	sha1_update_strnull(&ctx, (char const *)post->date);
	sha1_update_strnull(&ctx, (char const *)post->text.content);
	sha1_update_strnull(&ctx, source);

	BYTE bytes[16];
	sha1_final(&ctx, bytes);
	hash_from_sha1(hash, bytes);
}

static void
mail_create(struct mail *mail)
{
	strcpy(mail->path, MAIL_TMPNAME);
	mail->stream =  xftmpopen(mail->path);
}

static void
mail_commit(struct mail *mail, char const *name, int new)
{
	xfclose(mail->stream, mail->path);

	char new_path[PATH_MAX];
	xsnprintf(new_path, sizeof new_path,
			new
				? "new/0.%s.localhost"
				: "cur/0.%s.localhost:2,S",
			name);
	xlink(mail->path, new_path);
}

static enum rfc822_type
rfc822_classify(unsigned char const *s)
{
	enum rfc822_type ret = RFC822_ATOM;
	for (; *s; ++s) {
		enum rfc822_type t;
		if (' ' < *s && *s <= '~')
			t = RFC822_ATOM;
		else if (*s <= '~' && !('\r' == s[0] && '\n' == s[1])) {
			t = RFC822_TEXT;
		} else
			t = RFC822_NONASCII;

		if (ret < t)
			ret = t;
	}
	return ret;
}

static int
rfc2047_is_special(unsigned char c)
{
	return ' ' == c || '?' == c || '~' < c;
}

/* Perform "Q" encoding.
 * @see https://datatracker.ietf.org/doc/html/rfc2047 */
static void
rfc2047_write_q(unsigned char const *s, FILE *stream)
{
	static char const HEX[16] = "0123456789ABCDEF";

	fwrite("=?UTF-8?Q?", 1, 10, stream);
	for (; *s; ++s) {
		if (rfc2047_is_special(*s)) {
			if (' ' == *s) {
				fputc('_', stream);
			} else {
				fputc('=', stream);
				fputc(HEX[*s >> 4], stream);
				fputc(HEX[*s & 0xf], stream);
			}
		} else {
			fputc(*s, stream);
		}
	}
	fwrite("?=", 1, 2, stream);
}

static int
rfc822_is_qtext(unsigned char c)
{
	return !('"' == c || '\\' == c || '\r' == c);
}

static void
rfc822_write_qstr(unsigned char const *s, FILE *stream)
{
	fputc('"', stream);
	for (; *s; ++s) {
		if (!rfc822_is_qtext(*s))
			fputc('\\', stream);
		fputc(*s, stream);
	}
	fputc('"', stream);
}

/*
 * Format specifiers:
 * %s: Unrestricted NUL terminated string.
 * %t: Text.
 * %w: Word.
 */
static void
mail_write_hdr(struct mail *mail, char const *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	for (char const *s = fmt; (s = strchr(s, '%'));) {
		s += 2; /* %X */
		unsigned char const *arg = va_arg(ap, unsigned char *);
		if (!arg) {
			va_end(ap);
			return;
		}
	}
	va_end(ap);

	va_start(ap, fmt);
	for (char const *s = fmt;;) {
		char const *from = s;
		s = strchr(from, '%');
		if (!s)
			s = from + strlen(from);

		size_t len = (size_t)(s - from);
		fwrite(from, 1, len, mail->stream);
		if (!*s)
			break;

		enum rfc822_type allow;
		switch (s[1]) {
		case 's': allow = RFC822_NONASCII; break;
		case 't': allow = RFC822_TEXT; break;
		case 'w': allow = RFC822_ATOM; break;
		default: abort();
		}

		char const *arg = va_arg(ap, char *);
		enum rfc822_type type = rfc822_classify((unsigned char *)arg);

		if (type <= allow) {
			fputs(arg, mail->stream);
		} else switch (type) {
		case RFC822_TEXT:
			rfc822_write_qstr((unsigned char *)arg, mail->stream);
			break;

		case RFC822_NONASCII:
			rfc2047_write_q((unsigned char *)arg, mail->stream);
			break;

		default:
			abort();
		}

		s += 2; /* %s */
	}
	va_end(ap);

	fputc('\n', mail->stream);
}

static char *
get_domain(char **url)
{
	if (!*url) {
		*url = (char *)"localhost";
		return NULL;
	}

	char *slash = NULL;
	char *s = strstr(*url, "://");
	if (s) {
		*url = s + 3;
		slash = strchr(*url, '/');
		if (slash)
			*slash = '\0';
	}
	return slash;
}

static void
mail_write_from_hdr(struct mail *mail, struct post const *group)
{
	char *phrase = NULL;
	char *addr_spec = NULL;

	if (*opt_from)
		phrase = opt_from;
	else
		phrase = (char *)group->subject;
	addr_spec = (char *)group->link;

	char *slash = get_domain(&addr_spec);
	if (phrase)
		mail_write_hdr(mail, "From: %w <feed@%s>", phrase, addr_spec);
	else
		mail_write_hdr(mail, "From: feed@%s", addr_spec);
	if (slash)
		*slash = '/';
}

static void
mail_write_message_id_hdr(struct mail *mail, char const *name, struct post const *group)
{
	HASH id;
	hash_post(id, group, NULL);

	char *s = (char *)group->link;
	char *slash = get_domain(&s);
	mail_write_hdr(mail, "%s: <%s@%s>", name, id, s);
	if (slash)
		*slash = '/';
}

static void
generate_root_mail(struct post const *group)
{
	if (!opt_reply_to)
		return;

	struct mail mail;
	mail_create(&mail);

	mail_write_message_id_hdr(&mail, "Message-ID", group);
	mail_write_from_hdr(&mail, group);
	mail_write_hdr(&mail, "Subject: %t", group->subject);
	mail_write_hdr(&mail, "Link: %t", group->link);
	if (group->text.content) {
		mail_write_hdr(&mail, "Content-Type: %s", group->text.mime_type);
		fprintf(mail.stream, "\n%s", (char const *)group->text.content);
	}

	HASH id;
	hash_post(id, group, NULL);
	mail_commit(&mail, id, 0);
}

static size_t
xml_write_cb(char *buf, size_t size, size_t nmemb, void *userdata)
{
	xmlParserCtxtPtr *xml = userdata;

	size *= nmemb;

	if (!*xml) {
		*xml = xmlCreatePushParserCtxt(NULL, NULL, buf, size, NULL);
		if (!*xml)
			/* XXX: Should ensure that we have enough bytes to kickstart. */
			error("invalid XML");
	} else {
		if (xmlParseChunk(*xml, buf, size, 0 /* Terminate? */))
			error("invalid XML");
	}

	return size;
}

void
post_push(struct post const *item, struct post const *group)
{
	time_t date = 0;
	if (item->date) {
		date = parse_date((char *)item->date);

		if (date <= feed_old_latest)
			return;

		if (feed_new_latest < date)
			feed_new_latest = date;
	}

	generate_root_mail(group);

	struct mail mail;
	mail_create(&mail);

	HASH id;
	hash_post(id, item, (char const *)group->link);

	char datetime[50];
	time_t now = time(NULL);
	strftime(datetime, sizeof datetime, RFC_822, localtime(&now));
	mail_write_hdr(&mail, "Received: mrss; %s", datetime);

	mail_write_hdr(&mail, "Message-ID: <%s>", id);
	mail_write_message_id_hdr(&mail, "In-Reply-To", group);
	mail_write_hdr(&mail, "Content-Language: %t", item->lang);
	mail_write_hdr(&mail, "Content-Transfer-Encoding: binary");

	if (date) {
		strftime(datetime, sizeof datetime, RFC_822, localtime(&date));
		mail_write_hdr(&mail, "Date: %s", datetime);
	}

	mail_write_from_hdr(&mail, group);
	mail_write_hdr(&mail, "Subject: %t", item->subject);
	/* TODO: Support multiple categories. */
	mail_write_hdr(&mail, "X-Category: %t", item->category);
	mail_write_hdr(&mail, "Author: %t", item->author);
	mail_write_hdr(&mail, "Link: %t", item->link);
	if (item->text.content) {
		mail_write_hdr(&mail, "Content-Type: %s", item->text.mime_type);
		fprintf(mail.stream, "\n%s", (char const *)item->text.content);
	}

	mail_commit(&mail, id, 1);
}

static void
check_curl(CURLcode rc)
{
	if (rc == CURLE_OK)
		return;

	error("cURL error: %s", *curl_error_buf
			? curl_error_buf
			: curl_easy_strerror(rc));
}

static void
open_url(char const *url)
{
	if (!curl)
		curl = curl_easy_init();
	if (!curl)
		error("cURL error: cannot initialize");

	xmlParserCtxtPtr xml = NULL;

	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_buf);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
	curl_easy_setopt(curl, CURLOPT_VERBOSE, (long)opt_verbose);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	check_curl(curl_easy_setopt(curl, CURLOPT_PROXY, opt_proxy));
	check_curl(curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L));
	check_curl(curl_easy_setopt(curl, CURLOPT_SOCKS5_AUTH, CURLAUTH_BASIC));
	check_curl(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, xml_write_cb));
	check_curl(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &xml));
	check_curl(curl_easy_setopt(curl, CURLOPT_URL, url));
	check_curl(curl_easy_perform(curl));

	if (!xml || xmlParseChunk(xml, NULL, 0, 1 /* Terminate? */))
		error("invalid XML");

	xmlDocPtr doc = xml->myDoc;
	xmlNodePtr root = xmlDocGetRootElement(doc);

	if (!atom_parse(root) &&
	    !rss_parse(root) &&
	    !rdf_parse(root))
		error("unexpected root node");

	xmlFreeParserCtxt(xml);
}

static void
crawl_url(char const *url)
{
	if (opt_verbose)
		fprintf(stderr, "mrss: %s...\n", url);

	char buf[50];
	char statename[PATH_MAX];

	HASH id;
	hash_str(id, url);
	xsnprintf(statename, sizeof statename, ".mrssstate.%s", id);

	FILE *state = xfopen(statename, "a+");

	feed_old_latest = 0;
	if (fgets(buf, sizeof buf, state))
		feed_old_latest = parse_date(buf);
	feed_new_latest = feed_old_latest;

	time_t ignore_until = 0;
	if (fgets(buf, sizeof buf, state))
		ignore_until = parse_date(buf);

	fclose(state);

	time_t now = time(NULL);
	if (now <= ignore_until)
		return;

	xmkdir("tmp");
	xmkdir("new");
	xmkdir("cur");

	open_url(url);

	char tmpname[PATH_MAX];
	strcpy(tmpname, "tmp/mrssstate.XXXXXX");
	FILE *f = xftmpopen(tmpname);

	strftime(buf, sizeof buf, RFC_822, gmtime(&feed_new_latest));
	fputs(buf, f);
	fputc('\n', f);

	strftime(buf, sizeof buf, RFC_822, gmtime(&ignore_until));
	fputs(buf, f);
	fputc('\n', f);

	fputs(url, f);
	fputc('\n', f);

	xfclose(f, tmpname);
	xrename(tmpname, statename);
}

static void
crawl_file(char const *pathname)
{
	FILE *f = xfopen(pathname, "r");
	char line[BUFSIZ];
	while (xfgets(line, sizeof line, f))
		crawl_url(line);
	fclose(f);
}

static void
do_cmd(char const *cmd, char const *arg);

static void
do_cmd_file(char const *pathname)
{
	FILE *f = xfopen(pathname, "r");
	char line[BUFSIZ];
	while (xfgets(line, sizeof line, f)) {
		char *c = line;

		char const *cmd = c;
		while (*c && !isspace(*c))
			++c;

		char *arg = c;
		if (*arg) {
			*arg = '\0';
			while (isspace(*++arg));
		}

		do_cmd(cmd, arg);
	}
	fclose(f);
}

static void
set_str_opt(char *buf, size_t buf_size, char const *arg)
{
	size_t n = strlen(arg);
	if (buf_size <= n)
		error("argument '%s': too long", arg);
	memcpy(buf, arg, n);
	buf[n] = '\0';
}

static void
set_shellstr_opt(char *buf, size_t buf_size, char const *arg)
{
	wordexp_t we;
	if (wordexp(arg, &we, WRDE_NOCMD | WRDE_UNDEF))
		error("invalid path: '%s'", arg);
	if (1 < we.we_wordc)
		error("string '%s' expand into %d words, only a single one is expected",
				arg, we.we_wordc);
	set_str_opt(buf, buf_size, !we.we_wordc ? "" : we.we_wordv[0]);
	wordfree(&we);
}

static void
set_bool_opt(int *b, char const *arg)
{
	if (!strcmp(arg, "y") ||
	    !strcmp(arg, "yes") ||
	    !strcmp(arg, "on") ||
	    !strcmp(arg, "true") ||
	    !strcmp(arg, "1"))
		*b = 1;
	else if (
	    !strcmp(arg, "n") ||
	    !strcmp(arg, "no") ||
	    !strcmp(arg, "off") ||
	    !strcmp(arg, "false") ||
	    !strcmp(arg, "0"))
		*b = 0;
	else
		error("invalid boolean value: '%s'", arg);
}

static void
do_cmd(char const *cmd, char const *arg)
{
	if (!strcmp(cmd, "proxy"))
		set_str_opt(opt_proxy, sizeof opt_proxy, arg);
	else if (!strcmp(cmd, "path")) {
		char path[PATH_MAX];
		set_shellstr_opt(path, sizeof path, arg);
		if (chdir(path) < 0)
			error("failed to change current directory to '%s': %s",
					path, strerror(errno));
	} else if (!strcmp(cmd, "reply_to"))
		set_bool_opt(&opt_reply_to, arg);
	else if (!strcmp(cmd, "from"))
		set_str_opt(opt_from, sizeof opt_from, arg);
	else if (!strcmp(cmd, "verbose")) {
		set_bool_opt(&opt_verbose, arg);
		if (opt_verbose)
			puts(VERSION);
	} else if (!strcmp(cmd, "config") ||
	         !strcmp(cmd, "include"))
		do_cmd_file(arg);
	else if (!strcmp(cmd, "url"))
		crawl_url(arg);
	else if (!strcmp(cmd, "urls"))
		crawl_file(arg);
	else
		error("unknown command: '%s'", cmd);
}

int
main(int argc, char *argv[])
{
	LIBXML_TEST_VERSION

	for (int argi = 1; argi < argc;) {
		if ('-' != argv[argi][0] ||
		    '-' != argv[argi][1])
			error("unknown argument: '%s'", argv[argi]);

		char *cmd = argv[argi] + 2;
		char *arg = strchr(cmd, '=');
		if (arg) {
			*arg++ = '\0';
			argi += 1;
		} else if (argi + 1 < argc) {
			arg = argv[argi + 1];
			argi += 2;
		} else {
			error("missing argument for '%s'", cmd);
		}

		do_cmd(cmd, arg);
	}

	return EXIT_SUCCESS;
}
