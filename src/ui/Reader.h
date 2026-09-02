#pragma once

// Reader mode: the article inside a page, found by scoring the containers
// of its paragraphs — text length, commas, link density, class and id
// hints, the way readability tools have done it — and shown as a clean page
// of its own with the site's title, the source, and the content through a
// whitelist of elements with every link and picture made absolute.

#include "net/Url.h"

#include <string>

namespace sashfold::dom {
class Document;
}

namespace sashfold::ui {

struct Article {
    std::string title; // the page's first heading, else its <title>
    std::string html; // the content, serialized
};

Article extract_article(dom::Document const& document, net::Url const& base);

// The whole reader page for a document fetched from `url`.
std::string reader_page(dom::Document const& document, net::Url const& url);

}
