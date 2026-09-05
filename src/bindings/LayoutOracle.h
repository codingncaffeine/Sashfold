#pragma once

// Answers a script's geometry questions for a host that keeps no live page
// of its own — the renderer, the test runners: styles and layout computed
// from the document as it stands the first time a script asks, and again
// whenever the realm has counted a change since. The shell answers from
// its tab instead; both share find_element_box.

#include "bindings/Realm.h"
#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "layout/Layout.h"

#include <cstdint>
#include <optional>
#include <string>

namespace sashfold::bindings {

// The border box of an element in a laid-out tree: its own fragment, else
// the union of the boxes of its text runs (an inline element); nullopt
// when it has neither (display: none, or not laid out).
std::optional<LayoutBox> find_element_box(layout::Fragment const& root, dom::Element const& element);

class LayoutOracle {
public:
    LayoutOracle(dom::Document& document, net::Url const& base, css::SheetFetcher fetch, css::MediaContext media);
    // The realm whose mutation count says when the answers are stale.
    void set_realm(Realm* realm) { m_realm = realm; }
    // Puts layout_box and computed_style on the hooks, answering from here.
    void install(HostHooks& hooks);

    std::optional<LayoutBox> box(dom::Element const& element);
    css::ComputedStyle const* style(dom::Element const& element);
    // Brings styles and layout up to date with the tree.
    void ensure();

private:
    dom::Document& m_document;
    net::Url m_base;
    css::SheetFetcher m_fetch;
    css::MediaContext m_media;
    Realm* m_realm = nullptr;
    bool m_computed = false;
    std::uint64_t m_mutations = 0;
    // The sheets parsed and compiled once per set of stylesheet elements:
    // a script that reads a box after every write to the tree must not
    // pay for a re-parse each time, only for the cascade and the layout.
    std::string m_sheet_signature;
    std::optional<css::StyleSet> m_style_set;
    css::StyleMap m_styles;
    layout::LayoutResult m_layout;
};

}
