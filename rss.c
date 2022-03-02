#include "mrss.h"

/* @see https://validator.w3.org/feed/docs/rss2.html */
/* @see https://web.resource.org/rss/1.0/modules/content/ */

static xmlChar const NS_CONTENT[] = "http://purl.org/rss/1.0/modules/content/";

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
		.author = xmlGetNsChildContent(node, "author", NULL),
		.date = xmlGetNsChildContent(node, "pubDate", NULL),
		.id = guid,
		.lang = xmlStrdup(feed->lang),
		.link = link,
		.subject = xmlGetNsChildContent(node, "title", NULL),
		.text = text,
	};

	entry_process(&entry, feed);

	entry_destroy(&entry);
}

static void
rss_parse_channel(xmlNodePtr node)
{
	struct entry feed = {
		.category = xmlGetNsChildContent(node, "category", NULL),
		.lang = xmlGetNsChildContent(node, "language", NULL),
		.link = xmlGetNsChildContent(node, "link", NULL),
		.subject = xmlGetNsChildContent(node, "title", NULL),
		.text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(node, "description", NULL),
		},
	};

	for eachXmlElement(child, node)
		if (xmlTestNode(child, "item", NULL))
			rss_parse_item(child, &feed);

	entry_destroy(&feed);
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
