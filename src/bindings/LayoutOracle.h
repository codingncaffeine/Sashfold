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
#include "text/FontManager.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace sashfold::bindings {

// The border box of an element in a laid-out tree: its own fragment, else
// the union of the boxes of its text runs (an inline element); nullopt
// when it has neither (display: none, or not laid out) and for a
// display: contents element, which has no box whatever its runs say.
std::optional<LayoutBox> find_element_box(layout::Fragment const& root, dom::Element const& element,
    css::StyleMap const& styles);

// What each object and embed of the realm's document represents, as the realm
// has decided it so far, for layout and for the pictures an object or an embed
// shows. An element the realm has not decided yet is left out.
layout::EmbeddedStates embedded_states(Realm& realm);

class LayoutOracle {
public:
    LayoutOracle(dom::Document& document, net::Url const& base, css::SheetFetcher fetch, css::MediaContext media);
    // The realm whose mutation count says when the answers are stale.
    void set_realm(Realm* realm) { m_realm = realm; }
    // The document's Content Security Policy, whose say on inline styles
    // the answers here honour as the render does.
    void set_policy(net::ContentSecurityPolicy* policy) { m_policy = policy; }
    // Puts layout_box, computed_style and with_fonts on the hooks, answering
    // from here.
    void install(HostHooks& hooks);
    // A frame's answers (Realm.cpp). Its window goes on to another document;
    // its viewport is its container's box, which the page may resize; and the
    // fonts the process lays out with are the page's, put back after each
    // layout here rather than left as this document's.
    void retarget(dom::Document& document, net::Url const& base);
    void set_viewport(float width, float height);
    css::MediaContext const& media() const { return m_media; }
    void keep_page_fonts(bool keep) { m_keep_page_fonts = keep; }

    std::optional<LayoutBox> box(dom::Element const& element);
    css::ComputedStyle const* style(dom::Element const& element);
    // Brings styles and layout up to date with the tree.
    void ensure();
    // Runs `use` with the document's own fonts in the process's font
    // manager, put back afterwards as a layout here does.
    void with_fonts(std::function<void()> const& use);

private:
    // The sheets, and the fonts they bring, parsed again when the elements
    // carrying them have changed.
    void sheets_up_to_date();
    dom::Document* m_document;
    net::Url m_base;
    bool m_keep_page_fonts = false;
    css::SheetFetcher m_fetch;
    css::MediaContext m_media;
    Realm* m_realm = nullptr;
    net::ContentSecurityPolicy* m_policy = nullptr;
    bool m_computed = false;
    std::uint64_t m_mutations = 0;
    // The sheets parsed and compiled once per set of stylesheet elements:
    // a script that reads a box after every write to the tree must not
    // pay for a re-parse each time, only for the cascade and the layout.
    std::string m_sheet_signature;
    std::optional<css::StyleSet> m_style_set;
    std::vector<text::PageFont> m_fonts; // the document's own, from its sheets
    css::StyleMap m_styles;
    layout::LayoutResult m_layout;
};

}
