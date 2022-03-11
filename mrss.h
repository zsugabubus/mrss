#ifndef MRSS_H
#define MRSS_H

#include "xml_utils.h"

static char const MIME_TEXT_HTML[] = "text/html; charset=utf-8";
static char const MIME_TEXT_PLAIN[] = "text/plain; charset=utf-8";

struct media {
	char const *mime_type;
	xmlChar *content;
};

struct entry {
	xmlChar *author;
	xmlChar *category;
	xmlChar *date;
	xmlChar *id;
	xmlChar *lang;
	xmlChar *link;
	xmlChar *subject;
	struct media text;
	struct entry const *feed;
};

void entry_process(struct entry const *entry);
void entry_destroy(struct entry *entry);

int atom_parse(xmlNodePtr);
int rdf_parse(xmlNodePtr);
int rss_parse(xmlNodePtr);

#endif
