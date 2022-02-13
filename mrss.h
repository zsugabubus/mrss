#ifndef MRSS_H
#define MRSS_H

#include "xml_utils.h"

static char const MIME_TEXT_HTML[] = "text/html; charset=utf-8";
static char const MIME_TEXT_PLAIN[] = "text/plain; charset=utf-8";

struct media {
	char const *mime_type;
	xmlChar *content;
};

struct post {
	xmlChar *author;
	xmlChar *category;
	xmlChar *date;
	xmlChar *id;
	xmlChar *lang;
	xmlChar *link;
	xmlChar *subject;
	struct media text;
};

void post_push(struct post const *post, struct post const *group);
void post_destroy(struct post *post);

int atom_parse(xmlNodePtr);
int rdf_parse(xmlNodePtr);
int rss_parse(xmlNodePtr);

#endif
