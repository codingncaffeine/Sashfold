#pragma once

// The document base URL (HTML §2.4.3): what a document's relative URLs are
// resolved against — a link's href, a picture's src, a form's action, the
// URL a script fetches. It is the document's own URL unless the document
// holds a base element with an href, and then it is that href, resolved
// against the document's own URL.

#include "net/Url.h"

namespace sashfold::dom {
class Document;
}

namespace sashfold::html {

// `fallback` is the fallback base URL: the document's URL, or for an
// about:blank or srcdoc document the base URL of the document that made it.
// The first base element with an href in tree order decides, as the
// specification has it: when that href does not parse, or names a data: or
// javascript: URL, the fallback stands — a later base element is not asked.
net::Url document_base_url(dom::Document const& document, net::Url const& fallback);

}
