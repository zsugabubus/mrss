#include "mrss.h"

/* @see https://validator.w3.org/feed/docs/rss2.html */

static void
rss_parse_item(xmlNodePtr item, struct entity const *channel)
{
	xmlNodePtr guid_node = xmlGetNsChild(item, "guid", NULL);
	xmlChar *guid = guid_node ? xmlNodeGetContent(guid_node) : NULL;
	xmlChar *link = NULL;
	if (guid_node) {
		xmlChar *is_permalink = xmlGetNoNsProp(guid_node, XML_CHAR "isPermaLink");
		if (!xmlStrcmp(is_permalink, XML_CHAR "true"))
			link = xmlStrdup(guid);
		xmlFree(is_permalink);
	}
	if (!link)
		link = xmlGetNsChildContent(item, "link", NULL);
	if (!link && guid && !xmlStrncmp(guid, XML_CHAR "http", 4))
		link = xmlStrdup(guid);

	struct entity entity = {
		.author = xmlGetNsChildContent(item, "author", NULL),
		.date = xmlGetNsChildContent(item, "pubDate", NULL),
		.id = guid,
		.lang = xmlStrdup(channel->lang),
		.link = link,
		.summary = xmlGetNsChildContent(item, "description", NULL),
		.title = xmlGetNsChildContent(item, "title", NULL),
	};

	entity_push(&entity, channel);

	entity_destroy(&entity);
}

static void
rss_parse_channel(xmlNodePtr channel)
{
	struct entity entity = {
		.category = xmlGetNsChildContent(channel, "category", NULL),
		.lang = xmlGetNsChildContent(channel, "language", NULL),
		.link = xmlGetNsChildContent(channel, "link", NULL),
		.summary = xmlGetNsChildContent(channel, "description", NULL),
		.title = xmlGetNsChildContent(channel, "title", NULL),
	};

	for eachXmlElement(child, channel)
		if (xmlTestNode(child, "item", NULL))
			rss_parse_item(child, &entity);

	entity_destroy(&entity);
}

int
rss_parse(xmlNodePtr rss)
{
	if (!xmlTestNode(rss, "rss", NULL))
		return 0;

	for eachXmlElement(child, rss)
		if (xmlTestNode(child, "channel", NULL))
			rss_parse_channel(child);

	return 1;
}
