#include "mrss.h"

/* @see https://validator.w3.org/feed/docs/atom.html */

static xmlChar const NS_ATOM[] = "http://www.w3.org/2005/Atom";

static xmlChar *
atom_get_author(xmlNodePtr node)
{
	return xmlGetNsChildContent(
			xmlGetNsChild(node, "author", NS_ATOM),
			"name", NS_ATOM);
}

static xmlChar *
atom_get_link(xmlNodePtr node)
{
	for eachXmlElement(child, node) {
		if (!xmlTestNode(child, "link", NS_ATOM))
			continue;
		xmlNodePtr link = child;

		xmlChar *rel = xmlGetNoNsProp(link, XML_CHAR "rel");
		if (rel) {
			int cmp = xmlStrcmp(rel, XML_CHAR "alternate");
			xmlFree(rel);
			if (cmp)
				continue;
		}

		return xmlGetNoNsProp(link, XML_CHAR "href");
	}
	return NULL;
}

static void
atom_parse_entry(xmlNodePtr entry, struct entity const *channel)
{
	xmlNodePtr category = xmlGetNsChild(entry, "category", NS_ATOM);
	struct entity entity = {
		.author = atom_get_author(entry),
		.category = category ? xmlGetNoNsProp(category, XML_CHAR "term") : NULL,
		.content = xmlGetNsChildContent(entry, "content", NS_ATOM),
		.date = xmlGetNsChildContent(entry, "updated", NS_ATOM),
		.id = xmlGetNsChildContent(entry, "id", NS_ATOM),
		.lang = xmlStrdup(channel->lang),
		.link = atom_get_link(entry),
		.summary = xmlGetNsChildContent(entry, "summary", NS_ATOM),
		.title = xmlGetNsChildContent(entry, "title", NS_ATOM),
	};

	entity_push(&entity, channel);

	entity_destroy(&entity);
}

int
atom_parse(xmlNodePtr atom)
{
	if (!xmlTestNode(atom, "feed", NS_ATOM))
		return 0;

	struct entity entity = {
		.author = atom_get_author(atom),
		.id = xmlGetNsChildContent(atom, "id", NS_ATOM),
		.lang = xmlGetNsChildContent(atom, "language", NS_ATOM),
		.link = atom_get_link(atom),
		.summary = xmlGetNsChildContent(atom, "description", NS_ATOM),
		.title = xmlGetNsChildContent(atom, "title", NS_ATOM),
	};

	for eachXmlElement(child, atom)
		if (xmlTestNode(child, "entry", NS_ATOM))
			atom_parse_entry(child, &entity);

	entity_destroy(&entity);

	return 1;
}
