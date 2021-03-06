#include "mrss.h"

/* @see https://validator.w3.org/feed/docs/rss2.html */
/* @see https://web.resource.org/rss/1.0/modules/content/ */

static xmlChar const NS_CONTENT[] = "http://purl.org/rss/1.0/modules/content/";

static void
rss_parse_authors(xmlNodePtr node, struct entry *e)
{
	struct entry_author *author = e->authors;
	for eachXmlElement(child, node) {
		if (!xmlTestNode(child, "author", NULL))
			continue;
		if (!ARRAY_IN(e->authors, author))
			break;

		author->name = xmlNodeGetContent(child);
		++author;
	}
}

static void
rss_parse_category(xmlNodePtr node, struct entry *e)
{
	struct entry_category *category = e->categories;
	for eachXmlElement(child, node) {
		if (!xmlTestNode(child, "category", NULL))
			continue;
		if (!ARRAY_IN(e->categories, category))
			break;

		category->name = xmlNodeGetContent(child);
		++category;
	}
}

static void
rss_parse_item(xmlNodePtr node, struct entry const *feed)
{
	xmlNodePtr guid_node = xmlGetNsChild(node, "guid", NULL);
	xmlChar *guid = guid_node ? xmlNodeGetContent(guid_node) : NULL;
	xmlChar *link = NULL;
	if (guid_node) {
		xmlChar *is_permalink = xmlGetNoNsProp(guid_node, XML_CHAR "isPermaLink");
		if (!xmlStrcmp(is_permalink, XML_CHAR "true"))
			link = xmlStrdup(guid);
		xmlFree(is_permalink);
	}
	if (!link)
		link = xmlGetNsChildContent(node, "link", NULL);
	if (!link && guid && !xmlStrncmp(guid, XML_CHAR "http", 4))
		link = xmlStrdup(guid);

	struct media text;
	text = (struct media){
		.mime_type = MIME_TEXT_HTML,
		.content = xmlGetNsChildContent(node, "description", NULL),
	};

	if (!text.content)
		text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(node, "encoded", NS_CONTENT),
		};

	struct entry entry = {
		.date = xmlGetNsChildContent(node, "pubDate", NULL),
		.id = guid,
		.lang = xmlStrdup(feed->lang),
		.link = link,
		.subject = xmlGetNsChildContent(node, "title", NULL),
		.text = text,
		.feed = feed,
	};
	rss_parse_authors(node, &entry);
	rss_parse_category(node, &entry);

	entry_process(&entry);

	entry_uninit(&entry);
}

static void
rss_parse_channel(xmlNodePtr node)
{
	struct entry feed = {
		.lang = xmlGetNsChildContent(node, "language", NULL),
		.link = xmlGetNsChildContent(node, "link", NULL),
		.subject = xmlGetNsChildContent(node, "title", NULL),
		.text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(node, "description", NULL),
		},
		.feed = NULL,
	};
	rss_parse_category(node, &feed);

	for eachXmlElement(child, node)
		if (xmlTestNode(child, "item", NULL))
			rss_parse_item(child, &feed);

	entry_uninit(&feed);
}

int
rss_parse(xmlNodePtr node)
{
	if (!xmlTestNode(node, "rss", NULL))
		return 0;

	for eachXmlElement(child, node)
		if (xmlTestNode(child, "channel", NULL))
			rss_parse_channel(child);

	return 1;
}
