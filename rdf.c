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
rdf_parse_item(xmlNodePtr item, struct entity const *channel)
{
	struct entity entity = {
		.date = xmlGetNsChildContent(item, "date", NS_DC),
		.lang = xmlGetNsChildContent(item, "language", NS_DC),
		.link = xmlGetNsChildContent(item, "link", NS_RSS10),
		.summary = xmlGetNsChildContent(item, "description", NS_RSS10),
		.title = xmlGetNsChildContent(item, "title", NS_RSS10),
	};
	if (!entity.lang)
		entity.lang = xmlStrdup(channel->lang);

	entity_push(&entity, channel);

	entity_destroy(&entity);
}

static void
rdf_parse_channel(xmlNodePtr channel, xmlNodePtr rdf)
{
	xmlNodePtr items = xmlGetNsChild(channel, "items", NS_RSS10);
	if (!items)
		return;
	xmlNodePtr seq = xmlGetNsChild(items, "Seq", NS_RDF);
	if (!seq)
		return;

	struct entity entity = {
		.lang = xmlGetNsChildContent(channel, "language", NS_DC),
		.link = xmlGetNsChildContent(channel, "link", NS_RSS10),
		.summary = xmlGetNsChildContent(channel, "description", NS_RSS10),
		.title = xmlGetNsChildContent(channel, "title", NS_RSS10),
	};

	for eachXmlElement(child, seq) {
		if (!xmlTestNode(child, "li", NS_RDF))
			continue;

		xmlNodePtr item = rdf_find_item(child, rdf);
		if (!item)
			continue;

		rdf_parse_item(item, &entity);
	}

	entity_destroy(&entity);
}

int
rdf_parse(xmlNodePtr rdf)
{
	if (!xmlTestNode(rdf, "RDF", NS_RDF))
		return 0;

	for eachXmlElement(child, rdf)
		if (xmlTestNode(child, "channel", NS_RSS10))
			rdf_parse_channel(child, rdf);

	return 1;
}
