#ifndef MRSS_H
#define MRSS_H

#include "xml_utils.h"

struct entity {
	xmlChar *author;
	xmlChar *category;
	xmlChar *content;
	xmlChar *date;
	xmlChar *id;
	xmlChar *lang;
	xmlChar *link;
	xmlChar *summary;
	xmlChar *title;
};

void entity_push(struct entity const *item, struct entity const *channel);
void entity_destroy(struct entity *entity);

int atom_parse(xmlNodePtr);
int rdf_parse(xmlNodePtr);
int rss_parse(xmlNodePtr);

#endif
