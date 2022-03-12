#ifndef MRSS_H
#define MRSS_H

#include "xml_utils.h"

#define ARRAY_IN(arr, x) ((x) < ((&arr)[1]))

static char const MIME_TEXT_HTML[] = "text/html; charset=utf-8";
static char const MIME_TEXT_PLAIN[] = "text/plain; charset=utf-8";

struct media {
	char const *mime_type;
	xmlChar *content;
};

struct entry_author {
	xmlChar *name;
	xmlChar *email;
};

struct entry_category {
	xmlChar *name;
};

struct entry {
	struct entry_author authors[8];
	struct entry_category categories[16];
	xmlChar *date;
	xmlChar *id;
	xmlChar *lang;
	xmlChar *link;
	xmlChar *subject;
	struct media text;
	struct entry const *feed;
};

void entry_process(struct entry const *entry);
void entry_uninit(struct entry *entry);

int atom_parse(xmlNodePtr);
int rdf_parse(xmlNodePtr);
int rss_parse(xmlNodePtr);

#endif
