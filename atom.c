#include "mrss.h"

/* @see https://validator.w3.org/feed/docs/atom.html */

static xmlChar const NS_ATOM[] = "http://www.w3.org/2005/Atom";
static xmlChar const NS_MEDIA[] = "http://search.yahoo.com/mrss/";

static void
atom_parse_authors(xmlNodePtr node, struct entry *e)
{
	struct entry_author *author = e->authors;
	for eachXmlElement(child, node) {
		if (!xmlTestNode(child, "author", NS_ATOM) &&
		    !xmlTestNode(child, "contributor", NS_ATOM))
			continue;
		if (!ARRAY_IN(e->authors, author))
			break;

		author->name = xmlGetNsChildContent(child, "name", NS_ATOM);
		author->email = xmlGetNsChildContent(child, "email", NS_ATOM);
		++author;
	}
}

static void
atom_parse_categories(xmlNodePtr node, struct entry *e)
{
	struct entry_category *category = e->categories;
	for eachXmlElement(child, node) {
		if (!xmlTestNode(child, "category", NS_ATOM))
			continue;
		if (ARRAY_IN(e->categories, category))
			break;

		category->name = xmlGetNoNsProp(child, XML_CHAR "label");
		if (!category->name)
			category->name = xmlGetNoNsProp(child, XML_CHAR "term");
		++category;
	}
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

static struct media
atom_get_text(xmlNodePtr node)
{
	if (!node)
		return (struct media){ 0 };

	xmlChar *type = xmlGetNoNsProp(node, XML_CHAR "type");
	int is_text = !type || !xmlStrcmp(type, XML_CHAR "text");
	if (type)
		xmlFree(type);

	return (struct media){
		.mime_type = is_text
			? MIME_TEXT_PLAIN
			: MIME_TEXT_HTML,
		.content = xmlNodeGetContent(node),
	};
}

static void
atom_parse_entry(xmlNodePtr node, struct entry const *feed)
{
	struct media text;
	text = atom_get_text(xmlGetNsChild(node, "content", NS_ATOM));
	if (!text.content) {
		xmlNodePtr media_group = xmlGetNsChild(node, "group", NS_MEDIA);
		if (media_group)
			text = atom_get_text(xmlGetNsChild(media_group, "description", NS_MEDIA));
	}
	if (!text.content)
		text = atom_get_text(xmlGetNsChild(node, "summary", NS_ATOM));

	struct entry entry = {
		.date = xmlGetNsChildContent(node, "updated", NS_ATOM),
		.id = xmlGetNsChildContent(node, "id", NS_ATOM),
		.lang = xmlStrdup(feed->lang),
		.link = atom_get_link(node),
		.subject = xmlGetNsChildContent(node, "title", NS_ATOM),
		.text = text,
		.feed = feed,
	};
	atom_parse_authors(node, &entry);
	atom_parse_categories(node, &entry);

	entry_process(&entry);

	entry_uninit(&entry);
}

int
atom_parse(xmlNodePtr node)
{
	if (!xmlTestNode(node, "feed", NS_ATOM))
		return 0;

	struct entry feed = {
		.id = xmlGetNsChildContent(node, "id", NS_ATOM),
		.lang = xmlGetNsChildContent(node, "language", NS_ATOM),
		.link = atom_get_link(node),
		.subject = xmlGetNsChildContent(node, "title", NS_ATOM),
		.text = atom_get_text(xmlGetNsChild(node, "description", NS_ATOM)),
		.feed = NULL,
	};
	atom_parse_authors(node, &feed);
	atom_parse_categories(node, &feed);

	for eachXmlElement(child, node)
		if (xmlTestNode(child, "entry", NS_ATOM))
			atom_parse_entry(child, &feed);

	entry_uninit(&feed);

	return 1;
}
