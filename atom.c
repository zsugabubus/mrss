#include "mrss.h"

/* @see https://validator.w3.org/feed/docs/atom.html */

static xmlChar const NS_ATOM[] = "http://www.w3.org/2005/Atom";
static xmlChar const NS_MEDIA[] = "http://search.yahoo.com/mrss/";

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
atom_parse_entry(xmlNodePtr node, struct entry const *feed)
{
	xmlNodePtr category = xmlGetNsChild(node, "category", NS_ATOM);
	xmlChar *category_term = NULL;
	if (category)
		category_term  = xmlGetNoNsProp(category, XML_CHAR "term");

	struct media text;
	text = (struct media){
		.mime_type = MIME_TEXT_HTML,
		.content = xmlGetNsChildContent(node, "content", NS_ATOM),
	};
	if (!text.content) {
		xmlNodePtr media_group = xmlGetNsChild(node, "group", NS_MEDIA);
		if (media_group)
			text = (struct media){
				.mime_type = MIME_TEXT_PLAIN,
				.content = xmlGetNsChildContent(media_group, "description", NS_MEDIA),
			};
	}
	if (!text.content)
		text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(node, "summary", NS_ATOM),
		};

	struct entry entry = {
		.author = atom_get_author(node),
		.category = category_term,
		.date = xmlGetNsChildContent(node, "updated", NS_ATOM),
		.id = xmlGetNsChildContent(node, "id", NS_ATOM),
		.lang = xmlStrdup(feed->lang),
		.link = atom_get_link(node),
		.subject = xmlGetNsChildContent(node, "title", NS_ATOM),
		.text = text,
	};

	entry_process(&entry, feed);

	entry_destroy(&entry);
}

int
atom_parse(xmlNodePtr node)
{
	if (!xmlTestNode(node, "feed", NS_ATOM))
		return 0;

	struct entry feed = {
		.author = atom_get_author(node),
		.id = xmlGetNsChildContent(node, "id", NS_ATOM),
		.lang = xmlGetNsChildContent(node, "language", NS_ATOM),
		.link = atom_get_link(node),
		.subject = xmlGetNsChildContent(node, "title", NS_ATOM),
		.text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(node, "description", NS_ATOM),
		},
	};

	for eachXmlElement(child, node)
		if (xmlTestNode(child, "entry", NS_ATOM))
			atom_parse_entry(child, &feed);

	entry_destroy(&feed);

	return 1;
}
