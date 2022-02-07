#ifndef MRSS_XML_UTILS_H
#define MRSS_XML_UTILS_H

#include <libxml/tree.h>

#define XML_CHAR (xmlChar const *)

#define eachXmlElement(child, node) ( \
	xmlNodePtr child = xmlFirstElementChild(node); \
	child; \
	child = xmlNextElementSibling(child) \
)

int xmlTestNode(xmlNodePtr node, char const *name, xmlChar const *nameSpace);
xmlNodePtr xmlGetNsChild(xmlNodePtr node, char const *name, xmlChar const *nameSpace);
xmlChar *xmlGetNsChildContent(xmlNodePtr node, char const *name, xmlChar const *nameSpace);

#endif
