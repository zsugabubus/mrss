#define _GNU_SOURCE

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <libxml/tree.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
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
/* Shall be always GMT. Use with gmtime(). */
static char const RFC_2616[] = "%a, %d %b %Y %T %Z";

static char opt_from[128];
static char opt_proxy[1024];
static char opt_user_agent[128];
static int opt_expiration = 0;
static int opt_reply_to = 1;
static int opt_verbose = 0;

static long local_timezone;

static CURL *curl;
static char curl_error_buf[CURL_ERROR_SIZE];
struct {
	time_t last_modified;
	time_t expiration;
	char etag[1024];
} old_state, new_state;

static jmp_buf errctx;
static int have_errctx;

void
entry_uninit(struct entry *e)
{
	for (struct entry_author *author = e->authors;
	      ARRAY_IN(e->authors, author);
	      ++author)
	{
		xmlFree(author->name);
		xmlFree(author->email);
	}

	for (struct entry_category *category = e->categories;
	      ARRAY_IN(e->categories, category);
	      ++category)
		xmlFree(category->name);

	xmlFree(e->date);
	xmlFree(e->id);
	xmlFree(e->lang);
	xmlFree(e->link);
	xmlFree(e->subject);
	xmlFree(e->text.content);
}

static void
msg(int priority, char const *format, ...)
{
	switch (priority) {
	case LOG_INFO:
	case LOG_DEBUG:
		if (!opt_verbose)
			return;
	}

	fputs("mrss: ", stderr);

	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);

	if (LOG_ERR == priority) {
		if (have_errctx) {
			longjmp(errctx, 1);
		} else {
			exit(EXIT_FAILURE);
		}
	}
}

static void
xsnprintf(char *buf, size_t buf_size, char const *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(buf, buf_size, format, ap);
	va_end(ap);
	if ((int)buf_size <= n)
		msg(LOG_ERR, "Too long string: '%s'...", buf);
}

static FILE *
xfopen(char const *pathname, char const *mode)
{
	FILE *f = fopen(pathname, mode);
	if (!f)
		msg(LOG_ERR, "Cannot open '%s': %s", pathname, strerror(errno));
	return f;
}

static FILE *
xftmpopen(char *template)
{
	int fd = mkstemp(template);
	if (fd < 0)
		msg(LOG_ERR, "Cannot create temporary file: %s", strerror(errno));
	return fdopen(fd, "w+");
}

static void
xfclose(FILE *f, char const *pathname)
{
	if (fclose(f))
		msg(LOG_ERR, "Cannot write '%s': %s", pathname, strerror(errno));
}

static void
xmkdir(char const *path)
{
	if (mkdir(path, S_IRWXU) && EEXIST != errno)
		msg(LOG_ERR, "Cannot create '%s': %s", path, strerror(errno));
}

static void
xrename(char const *old, char const *new)
{
	if (rename(old, new))
		msg(LOG_ERR, "Cannot rename '%s' -> '%s': %s",
				old, new, strerror(errno));
}

static void
xlink(char const *from, char const *to)
{
	if (link(from, to) && EEXIST != errno)
		msg(LOG_ERR, "Cannot link '%s' -> '%s': %s",
				from, to, strerror(errno));
	(void)unlink(from);
}

static int
xfgets(char *s, int size, FILE *stream)
{
	if (fgets(s, size, stream)) {
		char *end = strchr(s, '\n');
		if (end)
			*end = '\0';
		return 1;
	}
	return 0;
}

static int
xfgets_config(char *s, int size, FILE *stream)
{
	while (xfgets(s, size, stream)) {
		if (!*s || '#' == *s)
			continue;
		return 1;
	}
	return 0;
}

static time_t
parse_date(char const *s)
{
	static char const *const FORMATS[] = {
		RFC_822,
		RFC_2616,
		"%FT%T%z",
		"%FT%TZ",
		"%F",
		"%D",
		NULL,
	};

	while (isspace(*s))
		++s;

	struct tm tm;
	for (char const *const *fmt = FORMATS; *fmt; ++fmt) {
		memset(&tm, 0, sizeof tm);
		char *end = strptime(s, *fmt, &tm);
		if (!end)
			continue;

		while (isspace(*end))
			++end;

		if (*end)
			msg(LOG_WARNING, "Unprocessed characters in date '%s': '%s'",
					s, end);

		time_t offset = tm.tm_gmtoff + local_timezone;
		return mktime(&tm) - offset;
	}

	msg(LOG_ERR, "Invalid date: '%s'", s);
	return 0;
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
hash_entry(HASH hash, struct entry const *e, int with_content)
{
	SHA1_CTX ctx;
	sha1_init(&ctx);

	sha1_update_strnull(&ctx, (char const *)e->id);
	sha1_update_strnull(&ctx, (char const *)e->link);
	if (with_content) {
		sha1_update_strnull(&ctx, (char const *)e->lang);
		sha1_update_strnull(&ctx, (char const *)e->subject);
		sha1_update_strnull(&ctx, (char const *)e->date);
		sha1_update_strnull(&ctx, (char const *)e->text.content);
	}
	if (e->feed)
		sha1_update_strnull(&ctx, (char const *)e->feed->link);

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

/* "Q"-encoding. */
static void
rfc2047_write_qenc(unsigned char const *s, FILE *stream)
{
	static char const HEX[16] = "0123456789ABCDEF";

	fwrite("=?UTF-8?Q?", 1, 10, stream);
	for (; *s; ++s) {
		if (' ' == *s) {
			fputc('_', stream);
		} else if (('0' <= *s && *s <= '9') ||
		           ('a' <= *s && *s <= 'z') ||
		           ('A' <= *s && *s <= 'Z') ||
		           '+' == *s || '-' == *s)
		{
			fputc(*s, stream);
		} else {
			fputc('=', stream);
			fputc(HEX[*s >> 4], stream);
			fputc(HEX[*s & 0xf], stream);
		}
	}
	fwrite("?=", 1, 2, stream);
}

static void
rfc822_write_quoted(unsigned char const *s, FILE *stream)
{
	fputc('"', stream);
	for (; *s; ++s) {
		if ('"' == *s || '\\' == *s || '\r' == *s)
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
			rfc822_write_quoted((unsigned char *)arg, mail->stream);
			break;

		case RFC822_NONASCII:
			rfc2047_write_qenc((unsigned char *)arg, mail->stream);
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
mail_write_from_hdr(struct mail *mail, struct entry const *feed)
{
	char *phrase = NULL;
	char *addr_spec = NULL;

	if (*opt_from)
		phrase = opt_from;
	else
		phrase = (char *)feed->subject;
	addr_spec = (char *)feed->link;

	char *slash = get_domain(&addr_spec);
	if (phrase)
		mail_write_hdr(mail, "From: %w <feed@%s>", phrase, addr_spec);
	else
		mail_write_hdr(mail, "From: feed@%s", addr_spec);
	if (slash)
		*slash = '/';
}

static void
mail_write_feed_msgid_hdr(struct mail *mail, char const *name, struct entry const *feed)
{
	HASH id;
	hash_entry(id, feed, 1);

	char *s = (char *)feed->link;
	char *slash = get_domain(&s);
	mail_write_hdr(mail, "%s: <%s@%s>", name, id, s);
	if (slash)
		*slash = '/';
}

static int
entry_has_author(struct entry const *e, struct entry_author const *author)
{
	for (struct entry_author const *x = e->authors;
	      ARRAY_IN(e->authors, x);
	      ++x)
		if (x->name &&
		    author->name &&
		    !xmlStrcmp(x->name, author->name))
			return 1;
	return 0;
}

static void
mail_write_author_hdr(struct mail *mail, struct entry const *e)
{
	for (struct entry_author const *author = e->authors;
	      ARRAY_IN(e->authors, author);
	      ++author)
	{
		if (e->feed && entry_has_author(e->feed, author))
			continue;

		if (author->name && author->email)
			mail_write_hdr(mail, "Author: %t <%t>",
					author->name, author->email);
		else if (author->name)
			mail_write_hdr(mail, "Author: %t",
					author->name);
	}
}

static int
entry_has_category(struct entry const *e, struct entry_category const *category)
{
	for (struct entry_category const *x = e->categories;
	      ARRAY_IN(e->categories, x);
	      ++x)
		if (x->name &&
		    category->name &&
		    !xmlStrcmp(x->name, category->name))
			return 1;
	return 0;
}

static void
mail_write_category_hdr(struct mail *mail, struct entry const *e)
{
	for (struct entry_category const *category = e->categories;
	      ARRAY_IN(e->categories, category);
	      ++category)
	{
		if (e->feed && entry_has_category(e->feed, category))
			continue;

		if (category->name)
			mail_write_hdr(mail, "X-Category: %t",
					category->name);
	}
}

static void
generate_root_mail(struct entry const *feed)
{
	if (!opt_reply_to)
		return;

	struct mail mail;
	mail_create(&mail);

	mail_write_feed_msgid_hdr(&mail, "Message-ID", feed);
	mail_write_from_hdr(&mail, feed);
	mail_write_hdr(&mail, "Subject: %t", feed->subject);
	mail_write_hdr(&mail, "Link: %t", feed->link);
	if (feed->text.content) {
		mail_write_hdr(&mail, "Content-Type: %s", feed->text.mime_type);
		fprintf(mail.stream, "\n%s", (char const *)feed->text.content);
	}

	HASH id;
	hash_entry(id, feed, 1);
	mail_commit(&mail, id, 0);
}

static size_t
header_cb(char *buf, size_t size, size_t nmemb, void *userdata)
{
	(void)userdata;

	size *= nmemb;

	if (curl_strnequal(buf, "etag:", 5)) {
		size_t n = size - 5 - 2 /* CRLF */;
		if (n <= sizeof new_state.etag - 1 /* NUL */) {
			memcpy(new_state.etag, buf + 5, n);
			new_state.etag[n] = '\0';
		} else {
			msg(LOG_WARNING, "Response ETag is ignored because too long");
		}
	}

	if (curl_strnequal(buf, "expires:", 8))
		new_state.expiration = parse_date(buf + 8);

	if (curl_strnequal(buf, "last-modified:", 14))
		new_state.last_modified = parse_date(buf + 14);

	return size;
}

static size_t
write_xml(char *buf, size_t size, size_t nmemb, void *userdata)
{
	xmlParserCtxtPtr *xml = userdata;

	size *= nmemb;

	if (!*xml) {
		*xml = xmlCreatePushParserCtxt(NULL, NULL, buf, size, NULL);
		if (!*xml)
			/* XXX: Should ensure that we have enough bytes to kickstart. */
			msg(LOG_ERR, "Invalid XML");
	} else {
		if (xmlParseChunk(*xml, buf, size, 0 /* Terminate? */))
			msg(LOG_ERR, "Invalid XML");
	}

	return size;
}

void
entry_process(struct entry const *entry)
{
	struct entry const *feed = entry->feed;
	msg(LOG_INFO, "Received entry [%s] '%s'", entry->date, entry->subject);

	time_t date = 0;
	if (entry->date) {
		date = parse_date((char *)entry->date);

		if (date <= old_state.last_modified)
			return;

		if (new_state.last_modified < date)
			new_state.last_modified = date;
	}

	msg(LOG_INFO, "New");

	generate_root_mail(feed);

	struct mail mail;
	mail_create(&mail);


	char datetime[50];
	time_t now = time(NULL);
	strftime(datetime, sizeof datetime, RFC_822, localtime(&now));
	mail_write_hdr(&mail, "Received: mrss; %s", datetime);

	HASH id;
	hash_entry(id, entry, 0);
	mail_write_hdr(&mail, "Message-ID: <%s@localhost>", id);
	mail_write_feed_msgid_hdr(&mail, "In-Reply-To", feed);
	mail_write_hdr(&mail, "Content-Language: %t", entry->lang);
	mail_write_hdr(&mail, "Content-Transfer-Encoding: binary");

	if (date) {
		strftime(datetime, sizeof datetime, RFC_822, localtime(&date));
		mail_write_hdr(&mail, "Date: %s", datetime);
	}

	mail_write_from_hdr(&mail, feed);
	mail_write_hdr(&mail, "Subject: %t", entry->subject);
	mail_write_category_hdr(&mail, feed);
	mail_write_category_hdr(&mail, entry);
	mail_write_author_hdr(&mail, feed);
	mail_write_author_hdr(&mail, entry);
	mail_write_hdr(&mail, "Link: %t", entry->link);
	if (entry->text.content) {
		mail_write_hdr(&mail, "Content-Type: %s", entry->text.mime_type);
		fprintf(mail.stream, "\n%s", (char const *)entry->text.content);
	}

	hash_entry(id, entry, 1);
	mail_commit(&mail, id, 1);
}

static void
check_curl(CURLcode rc)
{
	if (rc == CURLE_OK)
		return;

	msg(LOG_ERR, "cURL error: %s", *curl_error_buf
			? curl_error_buf
			: curl_easy_strerror(rc));
}

static int
open_feed_curl(xmlParserCtxtPtr *xml, char const *url)
{
	if (!curl)
		curl = curl_easy_init();
	if (!curl)
		msg(LOG_ERR, "cURL error: cannot initialize");

	struct curl_slist *headers = NULL;
	char buf[50 + 1024];

	if (*old_state.etag) {
		sprintf(buf, "If-None-Match:%s", old_state.etag);
		headers = curl_slist_append(headers, buf);
	} else if (old_state.last_modified) {
		char datetime[50];
		strftime(datetime, sizeof datetime, RFC_2616,
				gmtime(&old_state.last_modified));
		sprintf(buf, "If-Modified-Since: %s", datetime);
		headers = curl_slist_append(headers, buf);
	}

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
	check_curl(curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb));
	check_curl(curl_easy_setopt(curl, CURLOPT_HEADERDATA, xml));
	check_curl(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_xml));
	check_curl(curl_easy_setopt(curl, CURLOPT_WRITEDATA, xml));
	check_curl(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers));
	check_curl(curl_easy_setopt(curl, CURLOPT_USERAGENT,
			*opt_user_agent ? opt_user_agent : NULL));
	check_curl(curl_easy_setopt(curl, CURLOPT_URL, url));

	check_curl(curl_easy_perform(curl));

	curl_slist_free_all(headers);

	long status_code;
	check_curl(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code));

	/* Non-HTTP requests return 0. */
	return !status_code || 200 == status_code;
}

static int
open_feed_program(xmlParserCtxtPtr *xml, char const *command)
{
	char buf[BUFSIZ];
	FILE *stream = popen(command, "r");
	if (!stream)
		msg(LOG_ERR, "Failed to execute command");
	for (size_t n; (n = fread(buf, 1, sizeof buf, stream));)
		write_xml(buf, 1, n, xml);

	if (EXIT_SUCCESS == pclose(stream))
		return 1;
	/* No XML == not changed. */
	else if (!xml)
		return 0;

	msg(LOG_ERR, "Process terminated with failure");
	abort();
}

static void
open_feed(char const *url)
{
	xmlParserCtxtPtr xml = NULL;

	if (!strncmp(url, "system:", 7)) {
		if (!open_feed_program(&xml, url + 7))
			return;
	} else {
		if (!open_feed_curl(&xml, url))
			return;
	}

	if (!xml || xmlParseChunk(xml, NULL, 0, 1 /* Terminate? */))
		msg(LOG_ERR, "Invalid XML");

	xmlDocPtr doc = xml->myDoc;
	xmlNodePtr root = xmlDocGetRootElement(doc);

	if (!atom_parse(root) &&
	    !rss_parse(root) &&
	    !rdf_parse(root))
		msg(LOG_ERR, "Unexpected root node %s", root->name);

	xmlFreeParserCtxt(xml);
}

static void
process_feed(char const *url)
{
	char statename[PATH_MAX];

	HASH id;
	hash_str(id, url);
	xsnprintf(statename, sizeof statename, ".mrssstate.%s", id);

	msg(LOG_DEBUG, "Processing %s %s", id, url);

	char buf[BUFSIZ];
	FILE *fstate = xfopen(statename, "a+");

	old_state.last_modified = 0;
	if (xfgets(buf, sizeof buf, fstate))
		old_state.last_modified = parse_date(buf);
	new_state.last_modified = old_state.last_modified;

	old_state.expiration = 0;
	if (xfgets(buf, sizeof buf, fstate))
		old_state.expiration = parse_date(buf);
	new_state.expiration = old_state.expiration;

	*old_state.etag = '\0';
	xfgets(old_state.etag, sizeof old_state.etag, fstate);
	strcpy(new_state.etag, old_state.etag);

	fclose(fstate);

	time_t now = time(NULL);
	if (now <= old_state.expiration) {
		msg(LOG_INFO, "Cached for %lu minutes",
				(unsigned long)(old_state.expiration - now) / 60);
		return;
	}

	xmkdir("tmp");
	xmkdir("new");
	xmkdir("cur");

	open_feed(url);

	if (new_state.expiration < now + opt_expiration)
		new_state.expiration = now + opt_expiration;

	if (new_state.last_modified == old_state.last_modified &&
	    new_state.expiration == old_state.expiration &&
	    !strcmp(new_state.etag, old_state.etag))
	{
		msg(LOG_INFO, "State not changed");
		return;
	}

	char tmpname[PATH_MAX];
	strcpy(tmpname, "tmp/mrssstate.XXXXXX");
	FILE *f = xftmpopen(tmpname);

	strftime(buf, sizeof buf, RFC_2616, gmtime(&new_state.last_modified));
	fputs(buf, f);
	fputc('\n', f);

	strftime(buf, sizeof buf, RFC_2616, gmtime(&new_state.expiration));
	fputs(buf, f);
	fputc('\n', f);

	fputs(new_state.etag, f);
	fputc('\n', f);

	fputs(url, f);
	fputc('\n', f);

	xfclose(f, tmpname);
	xrename(tmpname, statename);

	msg(LOG_INFO, "State updated");
}

static void
exec_cmd_url(char const *url)
{
	have_errctx = 1;
	if (!setjmp(errctx))
		process_feed(url);
	else
		msg(LOG_NOTICE, "Errored URL: %s", url);
	have_errctx = 0;

	*opt_from = '\0';
}

static void
exec_cmd_urls(char const *pathname)
{
	FILE *f = xfopen(pathname, "r");
	char line[BUFSIZ];
	while (xfgets_config(line, sizeof line, f))
		exec_cmd_url(line);
	fclose(f);
}

static void
exec_cmd(char const *cmd, char const *arg);

static void
exec_cmd_file(char const *pathname)
{
	FILE *f = xfopen(pathname, "r");
	char line[BUFSIZ];
	while (xfgets_config(line, sizeof line, f)) {
		char *c = line;

		char const *cmd = c;
		while (*c && !isspace(*c))
			++c;

		char *arg = c;
		if (*arg) {
			*arg = '\0';
			while (isspace(*++arg));
		}

		exec_cmd(cmd, arg);
	}
	fclose(f);
}

static void
set_str_opt(char *buf, size_t buf_size, char const *arg)
{
	size_t n = strlen(arg);
	if (buf_size <= n)
		msg(LOG_ERR, "Argument '%s': too long", arg);
	memcpy(buf, arg, n);
	buf[n] = '\0';
}

static void
set_shellstr_opt(char *buf, size_t buf_size, char const *arg)
{
	wordexp_t we;
	if (wordexp(arg, &we, WRDE_NOCMD | WRDE_UNDEF))
		msg(LOG_ERR, "Invalid path: '%s'", arg);
	if (1 < we.we_wordc)
		msg(LOG_ERR, "String '%s' expand into %d words, only a single one is expected",
				arg, we.we_wordc);
	set_str_opt(buf, buf_size, !we.we_wordc ? "" : we.we_wordv[0]);
	wordfree(&we);
}

static void
set_choice_opt(int *b, char const *arg)
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
		msg(LOG_ERR, "Invalid boolean value: '%s'", arg);
}

static void
set_int_opt(int *value, char const *arg)
{
	char *end;
	errno = 0;
	*value = strtol(arg, &end, 10);
	if (errno)
		msg(LOG_ERR, "Argument '%s': invalid number", arg);

	int unit;
	switch (*end) {
	case 'd':
		unit = 24 * 60 * 60;
		break;

	case 'h':
		unit = 60 * 60;
		break;

	case 'm':
		unit = 60;
		break;

	case 's':
	case '\0':
		unit = 1;
		break;

	default:
		msg(LOG_ERR, "Argument '%s': unknown unit '%c'", arg, *end);
	}

	*value *= unit;
}

static void
exec_cmd(char const *cmd, char const *arg)
{
	if (!strcmp(cmd, "cd")) {
		char path[PATH_MAX];
		set_shellstr_opt(path, sizeof path, arg);
		if (chdir(path) < 0)
			msg(LOG_ERR, "Failed to change current directory to '%s': %s",
					path, strerror(errno));
	} else if (!strcmp(cmd, "config"))
		exec_cmd_file(arg);
	else if (!strcmp(cmd, "expire"))
		set_int_opt(&opt_expiration, arg);
	else if (!strcmp(cmd, "from"))
		set_str_opt(opt_from, sizeof opt_from, arg);
	else if (!strcmp(cmd, "include")) {
		char path[PATH_MAX];
		set_shellstr_opt(path, sizeof path, arg);
		exec_cmd_file(path);
	} else if (!strcmp(cmd, "proxy"))
		set_str_opt(opt_proxy, sizeof opt_proxy, arg);
	else if (!strcmp(cmd, "reply_to"))
		set_choice_opt(&opt_reply_to, arg);
	else if (!strcmp(cmd, "url"))
		exec_cmd_url(arg);
	else if (!strcmp(cmd, "urls"))
		exec_cmd_urls(arg);
	else if (!strcmp(cmd, "user_agent"))
		set_str_opt(opt_user_agent, sizeof opt_user_agent, arg);
	else if (!strcmp(cmd, "verbose")) {
		set_choice_opt(&opt_verbose, arg);
		msg(LOG_NOTICE, "Version: " VERSION);
	} else
		msg(LOG_ERR, "Unknown command: '%s'", cmd);
}

int
main(int argc, char *argv[])
{
	LIBXML_TEST_VERSION;

	/* Parsing %Z modifies timezone so it has to be saved. */
	tzset();
	local_timezone = timezone;

	for (int argi = 1; argi < argc;) {
		if ('-' != argv[argi][0] ||
		    '-' != argv[argi][1])
			msg(LOG_ERR, "Unknown argument: '%s'", argv[argi]);

		char *cmd = argv[argi] + 2;
		char *arg = strchr(cmd, '=');
		if (arg) {
			*arg++ = '\0';
			argi += 1;
		} else if (argi + 1 < argc) {
			arg = argv[argi + 1];
			argi += 2;
		} else {
			msg(LOG_ERR, "Missing argument for '%s'", cmd);
		}

		exec_cmd(cmd, arg);
	}

	return EXIT_SUCCESS;
}
