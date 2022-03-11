#include "mrss.h"

/* @see https://web.resource.org/rss/1.0/spec */
/* @see http://purl.org/dc/elements/1.1/ */

static xmlChar const NS_DC[] = "http://purl.org/dc/elements/1.1/";
static xmlChar const NS_RDF[] = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
static xmlChar const NS_RSS10[] = "http://purl.org/rss/1.0/";

static xmlNodePtr
rdf_find_item(xmlNodePtr li, xmlNodePtr rdf)
{
	xmlChar *resource = xmlGetNsProp(li, XML_CHAR "resource", NS_RDF);
	if (!resource)
		return NULL;

	xmlNodePtr ret = NULL;

	for eachXmlElement(child, rdf) {
		if (!xmlTestNode(child, "item", NS_RSS10))
			continue;
		xmlNodePtr item = child;

		xmlChar *about = xmlGetNsProp(item, XML_CHAR "about", NS_RDF);
		if (!about)
			continue;

		int cmp = xmlStrcmp(about, resource);
		xmlFree(about);
		if (!cmp) {
			ret = item;
			break;
		}
	}

	xmlFree(resource);

	return ret;
}

static void
rdf_parse_item(xmlNodePtr node, struct entry const *feed)
{
	struct entry entry = {
		.date = xmlGetNsChildContent(node, "date", NS_DC),
		.lang = xmlGetNsChildContent(node, "language", NS_DC),
		.link = xmlGetNsChildContent(node, "link", NS_RSS10),
		.subject = xmlGetNsChildContent(node, "title", NS_RSS10),
		.text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(node, "description", NS_RSS10),
		},
		.feed = feed,
	};
	if (!entry.lang)
		entry.lang = xmlStrdup(feed->lang);

	entry_process(&entry);

	entry_destroy(&entry);
}

static void
rdf_parse_channel(xmlNodePtr node, xmlNodePtr rdf)
{
	xmlNodePtr items = xmlGetNsChild(node, "items", NS_RSS10);
	if (!items)
		return;
	xmlNodePtr seq = xmlGetNsChild(items, "Seq", NS_RDF);
	if (!seq)
		return;

	struct entry feed = {
		.lang = xmlGetNsChildContent(node, "language", NS_DC),
		.link = xmlGetNsChildContent(node, "link", NS_RSS10),
		.subject = xmlGetNsChildContent(node, "title", NS_RSS10),
		.text = (struct media){
			.mime_type = MIME_TEXT_HTML,
			.content = xmlGetNsChildContent(node, "description", NS_RSS10),
		},
		.feed = NULL,
	};

	for eachXmlElement(child, seq) {
		if (!xmlTestNode(child, "li", NS_RDF))
			continue;

		xmlNodePtr item = rdf_find_item(child, rdf);
		if (!item)
			continue;

		rdf_parse_item(item, &feed);
	}

	entry_destroy(&feed);
}

int
rdf_parse(xmlNodePtr node)
{
	if (!xmlTestNode(node, "RDF", NS_RDF))
		return 0;

	for eachXmlElement(child, node)
		if (xmlTestNode(child, "channel", NS_RSS10))
			rdf_parse_channel(child, node);

	return 1;
}
