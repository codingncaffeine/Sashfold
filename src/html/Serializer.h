#pragma once

// The HTML fragment serialization algorithm (WHATWG HTML §13.3): the text
// innerHTML and outerHTML give back for a tree. Text is escaped as the
// specification says — & < > and the no-break space in text, & " and the
// no-break space in attribute values — except inside the elements whose
// content the parser took raw (style, script, xmp, iframe, noembed,
// noframes, plaintext, and noscript when scripting is on); void elements
// have no end tag; a template's contents stand in for its children.

#include "dom/Dom.h"

#include <string>
#include <vector>

namespace sashfold::html {

// The children of `node` serialized in order (innerHTML).
std::string serialize_children(dom::Node const& node, bool scripting = true);

// What getHTML() adds (§13.3, the shadow host step): each host's shadow
// root written first among its children as the `<template shadowrootmode>`
// the parser would make it from again — every serializable root when
// those are asked for, and any root named, serializable or not.
struct ShadowRootSerialization {
    bool serializable = false;
    std::vector<dom::ShadowRoot const*> roots;
};
std::string serialize_children(dom::Node const& node, ShadowRootSerialization const& shadow, bool scripting = true);

// The node itself with its children (outerHTML).
std::string serialize_node(dom::Node const& node, bool scripting = true);

// The text content of a node (DOM §4.4 textContent): the concatenation of
// every descendant Text node's data, in tree order.
std::string text_content(dom::Node const& node);

// The XML serialization of a node (DOM Parsing and Serialization §3.2), as
// XMLSerializer gives it: every element closed, an element's namespace
// declared where it is not the one inherited, text and attribute values
// escaped for an XML parser. An HTML element that is not void keeps its
// end tag when it is empty.
std::string serialize_xml(dom::Node const& node);

}
