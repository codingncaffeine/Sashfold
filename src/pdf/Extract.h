#pragma once

// What a viewer needs of a document besides its pictures: each page's text
// in runs with their boxes (the text layer that selection, copying and find
// work on), its links with where they lead, the outline, and destinations
// resolved to a page and a place on it (ISO 32000-2 §12.3).

#include "pdf/Document.h"

#include <optional>
#include <string>
#include <vector>

namespace sashfold::pdf {

// A run of text on one line in one direction, in device pixels at scale 1
// (the page's rotation applied): where its baseline starts, its length
// along it, its height above it and the angle of the baseline (radians,
// clockwise from the x axis as the screen draws, y down).
struct TextRun {
    std::string text; // UTF-8
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double angle = 0;
    bool ends_line = false; // the next run starts a new line
};
std::vector<TextRun> page_text(Document const& doc, std::size_t index);

// Where a destination leads: a page and, when it says, a point on it in
// device pixels at scale 1 from the page's top left.
struct Destination {
    std::size_t page = 0;
    std::optional<double> x;
    std::optional<double> y;
};
std::optional<Destination> resolve_destination(Document const& doc, Object const& destination);

// A link annotation: its rectangle (device pixels at scale 1) and a URL or a destination.
struct Link {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
    std::string url;
    std::optional<Destination> destination;
};
std::vector<Link> page_links(Document const& doc, std::size_t index);

struct OutlineItem {
    std::string title; // UTF-8
    std::optional<Destination> destination;
    std::string url;
    bool open = false;
    std::vector<OutlineItem> children;
};
std::vector<OutlineItem> document_outline(Document const& doc);

// A UTF-32 string as UTF-8.
std::string utf8_of(std::u32string const& text);

}
