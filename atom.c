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
atom_parse_entry(xmlNodePtr entry, struct post const *group)
{
	xmlNodePtr category = xmlGetNsChild(entry, "category", NS_ATOM);
	xmlChar *category_term = NULL;
	if (category)
		category_term  = xmlGetNoNsProp(category, XML_CHAR "term");

	struct media text;
	text = (struct media){
		.mime_type = MIME_TEXT_HTML,
		.content = xmlGetNsChildContent(entry, "content", NS_ATOM),
	};
	if (!text.content) {
		xmlNodePtr media_group = xmlGetNsChild(entry, "group", NS_MEDIA);
		if (media_group)
			text = (struct media){
				.mime_type = MIME_TEXT_PLAIN,
				.content = xmlGetNsChildContent(media_group, "description", NS_MEDIA),
			};
	}
	if (!text.content)
		text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(entry, "summary", NS_ATOM),
		};

	struct post post = {
		.author = atom_get_author(entry),
		.category = category_term,
		.date = xmlGetNsChildContent(entry, "updated", NS_ATOM),
		.id = xmlGetNsChildContent(entry, "id", NS_ATOM),
		.lang = xmlStrdup(group->lang),
		.link = atom_get_link(entry),
		.subject = xmlGetNsChildContent(entry, "title", NS_ATOM),
		.text = text,
	};

	post_push(&post, group);

	post_destroy(&post);
}

int
atom_parse(xmlNodePtr atom)
{
	if (!xmlTestNode(atom, "feed", NS_ATOM))
		return 0;

	struct post group = {
		.author = atom_get_author(atom),
		.id = xmlGetNsChildContent(atom, "id", NS_ATOM),
		.lang = xmlGetNsChildContent(atom, "language", NS_ATOM),
		.link = atom_get_link(atom),
		.subject = xmlGetNsChildContent(atom, "title", NS_ATOM),
		.text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(atom, "description", NS_ATOM),
		},
	};

	for eachXmlElement(child, atom)
		if (xmlTestNode(child, "entry", NS_ATOM))
			atom_parse_entry(child, &group);

	post_destroy(&group);

	return 1;
}
