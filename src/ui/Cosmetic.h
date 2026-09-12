#pragma once

// Cosmetic filtering: the element-hiding rules of the session's lists,
// applied to a page as one more stylesheet — `selector { display: none
// !important }` for every selector the lists name for the page's site —
// added after the page's own sheets, so its `!important` beats theirs.
// A list carries tens of thousands of generic rules; only those that name
// an id or a class the page has, and those that name neither, go into
// the sheet, which is how every content blocker keeps the cost down.

#include "css/Stylesheets.h"
#include "net/Filters.h"
#include "net/Url.h"

#include <optional>
#include <string>
#include <vector>

namespace sashfold::dom {
class Document;
}

namespace sashfold::ui {

// The selectors of `lists` for the page at `page`, held to the ones that
// can match something in `document` — the ids and classes the page has —
// in the lists' order.
std::vector<std::string> cosmetic_selectors(net::Blocklists const& lists, net::Url const& page,
    dom::Document const& document);

// The sheet that hides them, or nothing when there is nothing to hide.
std::optional<css::SheetSource> cosmetic_sheet(net::Blocklists const& lists, net::Url const& page,
    dom::Document const& document);

}
