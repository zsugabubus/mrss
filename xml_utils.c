#include "xml_utils.h"

int
xmlTestNode(xmlNodePtr node, char const *name, xmlChar const *nameSpace)
{
	return
		(nameSpace
		 ? node->ns && !xmlStrcmp(node->ns->href, nameSpace)
		 : !node->ns) &&
		!xmlStrcmp(node->name, XML_CHAR name);
}

xmlNodePtr
xmlGetNsChild(xmlNodePtr node, char const *name, xmlChar const *nameSpace)
{
	for eachXmlElement(child, node)
		if (xmlTestNode(child, name, nameSpace))
			return child;
	return NULL;
}

xmlChar *
xmlGetNsChildContent(xmlNodePtr node, char const *name, xmlChar const *nameSpace)
{
	xmlNodePtr child = xmlGetNsChild(node, name, nameSpace);
	if (!child)
		return NULL;
	return xmlNodeGetContent(child);
}
