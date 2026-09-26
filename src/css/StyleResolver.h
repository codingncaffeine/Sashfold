#pragma once

// The cascade: UA stylesheet + document <style> sheets + style attributes,
// ordered by importance, origin, specificity, and source order; inheritance
// and value computation produce a ComputedStyle per element.

#include "css/ComputedStyle.h"
#include "css/Stylesheets.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::css {

using StyleMap = std::unordered_map<dom::Element const*, ComputedStyle>;

// Whether a style="" attribute may apply to its element (the page's
// Content Security Policy says); an attribute refused is not there.
using StyleAttributeCheck = std::function<bool(dom::Element const& element, std::string_view text)>;

struct RuleSet;

// What a set's selectors can read besides the element they end on and its
// ancestors, which decides how far a change to one element can reach:
// sibling combinators and the `of S` counts read the siblings before it
// (their attributes as much as their number), the positional pseudo-classes
// its place among them, :empty its children, :has() anything below or after
// it, and ::first-letter styles are handed down past the element that asks.
struct StyleUses {
    bool sibling_combinators = false; // + and ~
    bool positional = false; // :nth-*, :first-/:last-/:only-child and -of-type
    bool nth_of = false; // :nth-child(An+B of S), :nth-last-child(... of S)
    bool empty = false; // :empty
    bool has = false; // :has()
    bool first_letter = false; // ::first-letter
};

// The UA stylesheet and the author sheets, in order, parsed, compiled and
// indexed once for a media context: the part of style resolution that does
// not depend on the document's elements. Build it when a page's sheets
// arrive or its viewport changes; resolve as often as needed.
class StyleSet {
public:
    // `document_url` is the base for the URLs in style attributes and
    // presentational hints; a sheet's own URLs resolve against the sheet.
    StyleSet(std::vector<SheetSource> const& sheets, MediaContext const& media = {},
        net::Url const* document_url = nullptr);
    ~StyleSet();
    StyleSet(StyleSet&&) noexcept;
    StyleSet& operator=(StyleSet&&) noexcept;

    std::size_t rule_count() const;
    // Selectors no element can be spared testing: the ones whose rightmost
    // compound names no id, class or type. A perf figure, not a feature.
    std::size_t universal_count() const;
    MediaContext const& media() const;
    // Whether the set would hold the same rules compiled for `other`: the
    // same scale, and every @media condition it met coming to what it came
    // to. A window changing size crosses a breakpoint now and then; between
    // two of them the sheets need not be read again, only told the size
    // (set_viewport), which the viewport units are taken against.
    bool same_rules_for(MediaContext const& other) const;
    void set_viewport(float width, float height);
    // Whether the last resolve_styles against this set took any length
    // against the viewport (vw, vh, vmin, vmax). When it did not, and the
    // rules are the same, the styles are the same at any size.
    bool viewport_lengths() const;
    // The same by side: against the viewport's width (vw, vmin, vmax), and
    // against its height (vh, vmin, vmax). A window made wider changes no
    // style that measured only its height.
    bool viewport_width_lengths() const;
    bool viewport_height_lengths() const;
    // The page's say on style attributes, asked for each one at every
    // resolution; none set means every attribute applies.
    void set_style_attribute_check(StyleAttributeCheck check);
    StyleUses const& uses() const;
    // A number no other set, and no earlier state of this one, has had: it
    // moves whenever what the set computes could change (a new viewport, a
    // new say on style attributes), so styles kept from before are known
    // to be from another set.
    std::uint64_t generation() const;

private:
    friend StyleMap resolve_styles(dom::Document const& document, StyleSet const& set);
    friend struct Restyler;
    std::unique_ptr<RuleSet> m_rules;
};

// What update_styles keeps between runs beside the map it brings up to
// date: which document and which state of which set the map is for, how far
// along the document's style clock it has read, and the styles of the root
// and body as the cascade gave them, before the viewport took their
// overflow and the root took body's writing mode. Its holder keeps it with
// the map and hands both over together; a fresh record makes the next
// update compute everything.
struct StyleRecord {
    dom::Document const* document = nullptr;
    std::uint64_t set_generation = 0;
    std::uint32_t read_at = 0;
    int quirks = -1;
    dom::Element const* root = nullptr;
    dom::Element const* body = nullptr;
    std::optional<ComputedStyle> root_cascaded;
    std::optional<ComputedStyle> body_cascaded;
};

// How an update went: how many elements it computed, whether it computed
// the whole document, and why when it did.
struct RestyleOutcome {
    std::size_t computed = 0;
    bool whole = false;
    std::string_view reason;
};

// Brings `styles` up to date with the document: the elements the style
// marks say changed since `record` was last read, and what those changes
// can reach, are computed again against their parents' current styles;
// every other element keeps its style as it was. What cannot be bounded,
// or was never computed, is computed whole as resolve_styles does, and
// the outcome says so.
RestyleOutcome update_styles(dom::Document const& document, StyleSet const& set, StyleMap& styles, StyleRecord& record);

// Resolves the document from scratch and compares every element's style
// with the one `styles` holds, field by field: the first element that is
// missing, kept though it is not in the tree, or different, with the
// field; nullopt when the two agree. What makes an update's bounds
// checkable.
std::optional<std::string> check_incremental(dom::Document const& document, StyleSet const& set, StyleMap const& styles);

// The first field, by name, in which two computed styles differ: lists by
// what they hold, custom properties by the values an element sees.
std::optional<std::string_view> first_style_difference(ComputedStyle const& a, ComputedStyle const& b);

// Whether two sets of custom properties give an element the same values.
bool same_custom_properties(CustomProperties const& a, CustomProperties const& b);

// Matches every element against the set, cascades with the style=""
// attributes, and computes styles (subtrees under display:none still get
// entries).
StyleMap resolve_styles(dom::Document const& document, StyleSet const& set);

// The same, building the set for this one resolution.
StyleMap resolve_styles(dom::Document const& document, std::vector<SheetSource> const& sheets,
    MediaContext const& media = {}, net::Url const* document_url = nullptr);

// The same over the document's <style> elements alone: nothing is fetched.
StyleMap resolve_styles(dom::Document const& document);

// The style an anonymous box takes under `parent`: the inherited
// properties from it, every other at its initial value. Layout makes the
// boxes the specification generates around misplaced content this way.
ComputedStyle inherited_style(ComputedStyle const& parent);

// A color as CSS writes one — a hex color, a name, rgb(), hsl() and the
// rest of what a declaration's value may hold — from text that is nothing
// else; nullopt for anything more or less. For what reads colors outside a
// stylesheet: a browser theme's manifest writes its colors this way.
std::optional<Color> parse_color_text(std::string_view text);

// The font shorthand as text alone — what a canvas context's font attribute
// takes: the size (relative sizes against `parent_font_size`), the weight,
// the slant, small-caps and the family list, by the cascade's own rules;
// nullopt for a value the cascade would drop, and for the CSS-wide keywords.
struct FontShorthandValue {
    float size = 16;
    int weight = 400;
    bool italic = false;
    bool oblique = false;
    bool small_caps = false;
    int stretch = 100;
    std::vector<std::string> families;
};
std::optional<FontShorthandValue> parse_font_shorthand_text(std::string_view text, float parent_font_size);

// css-conditional-3 §6: whether a @supports prelude's <supports-condition>
// holds, over its already-parsed component values — `not`/`and`/`or` of
// `( <supports-condition> )`, `( <declaration> )`, `selector( <complex-
// selector> )` and `at-rule( <at-keyword> )`, with anything else
// (<general-enclosed>) false. A declaration is supported when this engine's
// own cascade actually accepts it: not a separate list, but the resolver's
// real dispatch, tried on a scratch style (a value with var() in it, once
// the property is known).
// Used by compile_rules for the @supports at-rule, and — doubled with the
// implicit-parentheses retry §8 asks for — by CSS.supports() in
// bindings/Style.cpp.
bool supports_condition_matches(std::vector<ComponentValue> const& prelude);

// CSS.supports(conditionText): `text` as a <supports-condition>, or else
// (§8) the same wrapped in parentheses — which is what makes a bare
// declaration like "display: grid" work, and what the two-argument form of
// CSS.supports reduces to once its caller has joined "property: value".
bool supports_condition_text_matches(std::string_view text);

// What a page wrote that the style system drops, told to the sink a host
// sets: a property it does not know ("css property", the name), an at-rule
// whose rules it skips ("css at-rule", "@name"), a rule whose selector does
// not parse ("css selector", the pseudo-class or pseudo-element that stops
// it). No sink is set unless a host asks for the census (--render --gaps).
using GapSink = std::function<void(std::string_view kind, std::string_view item)>;
void set_gap_sink(GapSink sink);

}
