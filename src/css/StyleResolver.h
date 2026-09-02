#pragma once

// The cascade: UA stylesheet + document <style> sheets + style attributes,
// ordered by importance, origin, specificity, and source order; inheritance
// and value computation produce a ComputedStyle per element.

#include "css/ComputedStyle.h"
#include "css/Stylesheets.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::css {

using StyleMap = std::unordered_map<dom::Element const*, ComputedStyle>;

struct RuleSet;

// The UA stylesheet and the author sheets, in order, parsed, compiled and
// indexed once for a media context: the part of style resolution that does
// not depend on the document's elements. Build it when a page's sheets
// arrive or its viewport changes; resolve as often as needed.
class StyleSet {
public:
    StyleSet(std::vector<SheetSource> const& sheets, MediaContext const& media = {});
    ~StyleSet();
    StyleSet(StyleSet&&) noexcept;
    StyleSet& operator=(StyleSet&&) noexcept;

    std::size_t rule_count() const;
    // Selectors no element can be spared testing: the ones whose rightmost
    // compound names no id, class or type. A perf figure, not a feature.
    std::size_t universal_count() const;
    MediaContext const& media() const;

private:
    friend StyleMap resolve_styles(dom::Document const& document, StyleSet const& set);
    std::unique_ptr<RuleSet> m_rules;
};

// Matches every element against the set, cascades with the style=""
// attributes, and computes styles (subtrees under display:none still get
// entries).
StyleMap resolve_styles(dom::Document const& document, StyleSet const& set);

// The same, building the set for this one resolution.
StyleMap resolve_styles(dom::Document const& document, std::vector<SheetSource> const& sheets,
    MediaContext const& media = {});

// The same over the document's <style> elements alone: nothing is fetched.
StyleMap resolve_styles(dom::Document const& document);

// The style an anonymous box takes under `parent`: the inherited
// properties from it, every other at its initial value. Layout makes the
// boxes the specification generates around misplaced content this way.
ComputedStyle inherited_style(ComputedStyle const& parent);

}
