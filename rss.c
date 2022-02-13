#include "mrss.h"

/* @see https://validator.w3.org/feed/docs/rss2.html */
/* @see https://web.resource.org/rss/1.0/modules/content/ */

static xmlChar const NS_CONTENT[] = "http://purl.org/rss/1.0/modules/content/";

static void
rss_parse_item(xmlNodePtr item, struct post const *group)
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

	struct media text;
	text = (struct media){
		.mime_type = MIME_TEXT_HTML,
		.content = xmlGetNsChildContent(item, "description", NULL),
	};

	if (!text.content)
		text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(item, "encoded", NS_CONTENT),
		};

	struct post post = {
		.author = xmlGetNsChildContent(item, "author", NULL),
		.date = xmlGetNsChildContent(item, "pubDate", NULL),
		.id = guid,
		.lang = xmlStrdup(group->lang),
		.link = link,
		.subject = xmlGetNsChildContent(item, "title", NULL),
		.text = text,
	};

	post_push(&post, group);

	post_destroy(&post);
}

static void
rss_parse_channel(xmlNodePtr channel)
{
	struct post group = {
		.category = xmlGetNsChildContent(channel, "category", NULL),
		.lang = xmlGetNsChildContent(channel, "language", NULL),
		.link = xmlGetNsChildContent(channel, "link", NULL),
		.subject = xmlGetNsChildContent(channel, "title", NULL),
		.text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(channel, "description", NULL),
		},
	};

	for eachXmlElement(child, channel)
		if (xmlTestNode(child, "item", NULL))
			rss_parse_item(child, &group);

	post_destroy(&group);
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
