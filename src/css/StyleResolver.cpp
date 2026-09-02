#include "css/StyleResolver.h"

#include "core/Ascii.h"
#include "css/Parser.h"
#include "css/Selector.h"
#include "dom/Dom.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::css {

namespace {


// The built-in UA stylesheet: the HTML rendering section's defaults for the
// reader web, kept to the first property set.
constexpr std::string_view ua_stylesheet = R"CSS(
html { display: block }
head, style, script, title, meta, link, base, template, noscript { display: none }
body { display: block; margin: 8px }
address, article, aside, blockquote, center, dd, details, dialog, div, dl, dt,
fieldset, figcaption, figure, footer, form, header, hgroup, hr, legend, listing,
main, menu, nav, ol, p, plaintext, pre, search, section, summary, ul, xmp,
h1, h2, h3, h4, h5, h6, dir, table, caption, tbody, thead, tfoot, tr { display: block }
li { display: list-item }
p, blockquote, figure, dl, ol, ul, pre, listing, plaintext, xmp { margin-top: 1em; margin-bottom: 1em }
blockquote, figure { margin-left: 40px; margin-right: 40px }
ol, ul, dir, menu { padding-left: 40px }
ol { list-style-type: decimal }
ul ul { list-style-type: circle }
ul ul ul { list-style-type: square }
dd { margin-left: 40px }
h1 { font-size: 2em; margin-top: 0.67em; margin-bottom: 0.67em; font-weight: bold }
h2 { font-size: 1.5em; margin-top: 0.83em; margin-bottom: 0.83em; font-weight: bold }
h3 { font-size: 1.17em; margin-top: 1em; margin-bottom: 1em; font-weight: bold }
h4 { margin-top: 1.33em; margin-bottom: 1.33em; font-weight: bold }
h5 { font-size: 0.83em; margin-top: 1.67em; margin-bottom: 1.67em; font-weight: bold }
h6 { font-size: 0.67em; margin-top: 2.33em; margin-bottom: 2.33em; font-weight: bold }
b, strong { font-weight: bolder }
i, em, cite, dfn, var, address { font-style: italic }
small { font-size: 0.83em }
big { font-size: 1.17em }
pre, listing, plaintext, xmp { white-space: pre }
pre, listing, plaintext, xmp, code, kbd, samp, tt { font-family: monospace }
nobr { white-space: nowrap }
a { color: rgb(0, 0, 238); text-decoration: underline }
s, strike, del { text-decoration: line-through }
u, ins { text-decoration: underline }
center, caption, th { text-align: center }
hr { margin-top: 0.5em; margin-bottom: 0.5em; border-top: 1px solid; color: gray }
mark { background-color: yellow }
input, textarea, select, button { font-size: 13.333px; line-height: normal; font-family: sans-serif }
input[type=hidden] { display: none }
input[type=checkbox], input[type=radio] { margin: 3px 3px 3px 4px }
textarea { white-space: pre-wrap }
)CSS";

enum class CascadeRank : int {
    UserAgentNormal = 1,
    AuthorNormal = 2,
    StyleAttributeNormal = 3,
    AuthorImportant = 4,
    StyleAttributeImportant = 5,
    UserAgentImportant = 6,
};

struct CompiledRule {
    SelectorList selectors;
    std::vector<Declaration> declarations;
    bool user_agent = false;
    int order = 0; // rule order across all sheets
};

struct MatchedDeclaration {
    Declaration const* declaration = nullptr;
    int rank = 0;
    Specificity specificity;
    int order = 0;
};

// --- The ancestor filter -------------------------------------------------------

std::uint32_t fnv1a(std::string_view text)
{
    std::uint32_t hash = 2166136261u;
    for (char const c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

// A counting Bloom filter over the identifiers (#id, .class, tag) of the
// elements on the way down the tree. A selector whose ancestor compounds
// require an identifier the filter has never seen cannot match, and is
// dropped before the real matcher walks the ancestors — the win is that
// ".site .nav a" costs three counter reads for the links it does not fit.
class AncestorFilter {
public:
    void push(std::uint32_t hash)
    {
        std::uint8_t& first = m_counts[hash & mask];
        std::uint8_t& second = m_counts[(hash >> 14) & mask];
        if (first != 255)
            ++first;
        if (second != 255)
            ++second;
    }

    void pop(std::uint32_t hash)
    {
        std::uint8_t& first = m_counts[hash & mask];
        std::uint8_t& second = m_counts[(hash >> 14) & mask];
        if (first != 255 && first != 0)
            --first;
        if (second != 255 && second != 0)
            --second;
    }

    bool may_contain(std::uint32_t hash) const
    {
        return m_counts[hash & mask] != 0 && m_counts[(hash >> 14) & mask] != 0;
    }

    bool may_contain_all(std::vector<std::uint32_t> const& hashes) const
    {
        for (std::uint32_t const hash : hashes) {
            if (!may_contain(hash))
                return false;
        }
        return true;
    }

private:
    static constexpr std::uint32_t mask = (1u << 14) - 1;
    std::array<std::uint8_t, 1u << 14> m_counts {};
};

bool cascades_before(MatchedDeclaration const& a, MatchedDeclaration const& b)
{
    if (a.rank != b.rank)
        return a.rank < b.rank;
    if (a.specificity != b.specificity)
        return a.specificity < b.specificity;
    return a.order < b.order;
}

// --- Value parsing ------------------------------------------------------------

// The component values of one declaration with whitespace dropped.
std::vector<ComponentValue const*> significant(std::vector<ComponentValue> const& values)
{
    std::vector<ComponentValue const*> out;
    for (ComponentValue const& value : values) {
        if (!value.is_token(Token::Type::Whitespace))
            out.push_back(&value);
    }
    return out;
}

bool is_ident(ComponentValue const* value, std::string_view name)
{
    return value && value->is_token(Token::Type::Ident)
        && ascii_ci_equals(value->token().value, name);
}

struct NamedColor {
    std::string_view name;
    Color color;
};

// The named colors we are sure of; a missing name means the declaration is
// ignored (safe), a wrong value would render wrong (not safe), so this list
// only grows with verified entries.
constexpr NamedColor named_colors[] = {
    { "black", Color { 0, 0, 0, 255 } },
    { "silver", Color { 192, 192, 192, 255 } },
    { "gray", Color { 128, 128, 128, 255 } },
    { "grey", Color { 128, 128, 128, 255 } },
    { "white", Color { 255, 255, 255, 255 } },
    { "maroon", Color { 128, 0, 0, 255 } },
    { "red", Color { 255, 0, 0, 255 } },
    { "purple", Color { 128, 0, 128, 255 } },
    { "fuchsia", Color { 255, 0, 255, 255 } },
    { "magenta", Color { 255, 0, 255, 255 } },
    { "green", Color { 0, 128, 0, 255 } },
    { "lime", Color { 0, 255, 0, 255 } },
    { "olive", Color { 128, 128, 0, 255 } },
    { "yellow", Color { 255, 255, 0, 255 } },
    { "navy", Color { 0, 0, 128, 255 } },
    { "blue", Color { 0, 0, 255, 255 } },
    { "teal", Color { 0, 128, 128, 255 } },
    { "aqua", Color { 0, 255, 255, 255 } },
    { "cyan", Color { 0, 255, 255, 255 } },
    { "orange", Color { 255, 165, 0, 255 } },
    { "brown", Color { 165, 42, 42, 255 } },
    { "pink", Color { 255, 192, 203, 255 } },
    { "gold", Color { 255, 215, 0, 255 } },
    { "indigo", Color { 75, 0, 130, 255 } },
    { "violet", Color { 238, 130, 238, 255 } },
    { "crimson", Color { 220, 20, 60, 255 } },
    { "coral", Color { 255, 127, 80, 255 } },
    { "salmon", Color { 250, 128, 114, 255 } },
    { "khaki", Color { 240, 230, 140, 255 } },
    { "beige", Color { 245, 245, 220, 255 } },
    { "ivory", Color { 255, 255, 240, 255 } },
    { "snow", Color { 255, 250, 250, 255 } },
    { "tomato", Color { 255, 99, 71, 255 } },
    { "orchid", Color { 218, 112, 214, 255 } },
    { "plum", Color { 221, 160, 221, 255 } },
    { "tan", Color { 210, 180, 140, 255 } },
    { "wheat", Color { 245, 222, 179, 255 } },
    { "lavender", Color { 230, 230, 250, 255 } },
    { "turquoise", Color { 64, 224, 208, 255 } },
    { "chocolate", Color { 210, 105, 30, 255 } },
    { "skyblue", Color { 135, 206, 235, 255 } },
    { "lightblue", Color { 173, 216, 230, 255 } },
    { "lightgray", Color { 211, 211, 211, 255 } },
    { "lightgrey", Color { 211, 211, 211, 255 } },
    { "lightgreen", Color { 144, 238, 144, 255 } },
    { "lightyellow", Color { 255, 255, 224, 255 } },
    { "darkgray", Color { 169, 169, 169, 255 } },
    { "darkgrey", Color { 169, 169, 169, 255 } },
    { "darkred", Color { 139, 0, 0, 255 } },
    { "darkblue", Color { 0, 0, 139, 255 } },
    { "darkgreen", Color { 0, 100, 0, 255 } },
    { "darkorange", Color { 255, 140, 0, 255 } },
    { "darkviolet", Color { 148, 0, 211, 255 } },
    { "dimgray", Color { 105, 105, 105, 255 } },
    { "dimgrey", Color { 105, 105, 105, 255 } },
    { "gainsboro", Color { 220, 220, 220, 255 } },
    { "whitesmoke", Color { 245, 245, 245, 255 } },
    { "aliceblue", Color { 240, 248, 255, 255 } },
    { "ghostwhite", Color { 248, 248, 255, 255 } },
    { "royalblue", Color { 65, 105, 225, 255 } },
    { "steelblue", Color { 70, 130, 180, 255 } },
    { "dodgerblue", Color { 30, 144, 255, 255 } },
    { "cornflowerblue", Color { 100, 149, 237, 255 } },
    { "midnightblue", Color { 25, 25, 112, 255 } },
    { "slategray", Color { 112, 128, 144, 255 } },
    { "slategrey", Color { 112, 128, 144, 255 } },
    { "forestgreen", Color { 34, 139, 34, 255 } },
    { "seagreen", Color { 46, 139, 87, 255 } },
    { "goldenrod", Color { 218, 165, 32, 255 } },
    { "firebrick", Color { 178, 34, 34, 255 } },
    { "rebeccapurple", Color { 102, 51, 153, 255 } },
};

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::optional<Color> parse_hex_color(std::string_view hex)
{
    auto const nibble = [&](std::size_t i) { return hex_nibble(hex[i]); };
    switch (hex.size()) {
    case 3:
    case 4: {
        int const r = nibble(0);
        int const g = nibble(1);
        int const b = nibble(2);
        int const a = hex.size() == 4 ? nibble(3) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0)
            return std::nullopt;
        return Color { static_cast<std::uint8_t>(r * 17), static_cast<std::uint8_t>(g * 17),
            static_cast<std::uint8_t>(b * 17), static_cast<std::uint8_t>(a * 17) };
    }
    case 6:
    case 8: {
        int parts[4] = { 0, 0, 0, 255 };
        for (std::size_t i = 0; i * 2 < hex.size(); ++i) {
            int const high = nibble(i * 2);
            int const low = nibble(i * 2 + 1);
            if (high < 0 || low < 0)
                return std::nullopt;
            parts[i] = high * 16 + low;
        }
        return Color { static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
            static_cast<std::uint8_t>(parts[2]), static_cast<std::uint8_t>(parts[3]) };
    }
    default:
        return std::nullopt;
    }
}

std::uint8_t clamp_channel(double value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return static_cast<std::uint8_t>(value + 0.5);
}

// rgb()/rgba(), modern or legacy syntax. `current` feeds currentcolor.
std::optional<Color> parse_color_component(ComponentValue const& value, Color current)
{
    if (value.is_token(Token::Type::Hash))
        return parse_hex_color(value.token().value);
    if (value.is_token(Token::Type::Ident)) {
        std::string_view const name = value.token().value;
        if (ascii_ci_equals(name, "transparent"))
            return Color { 0, 0, 0, 0 };
        if (ascii_ci_equals(name, "currentcolor"))
            return current;
        for (NamedColor const& entry : named_colors) {
            if (ascii_ci_equals(name, entry.name))
                return entry.color;
        }
        return std::nullopt;
    }
    if (value.is_function()) {
        FunctionValue const& function = value.function();
        if (!ascii_ci_equals(function.name, "rgb") && !ascii_ci_equals(function.name, "rgba"))
            return std::nullopt;
        double channels[4] = { 0, 0, 0, 255 };
        int channel = 0;
        bool alpha_seen = false;
        for (ComponentValue const& argument : function.values) {
            if (argument.is_token(Token::Type::Whitespace) || argument.is_token(Token::Type::Comma))
                continue;
            if (argument.is_token(Token::Type::Delim) && argument.token().delim == U'/')
                continue;
            if (channel >= 4)
                return std::nullopt;
            if (!argument.is_token())
                return std::nullopt;
            Token const& token = argument.token();
            double parsed = 0;
            if (token.type == Token::Type::Number)
                parsed = token.numeric_value;
            else if (token.type == Token::Type::Percentage)
                parsed = token.numeric_value * 255.0 / 100.0;
            else
                return std::nullopt;
            if (channel == 3) {
                // Alpha: number 0..1 or percentage.
                alpha_seen = true;
                parsed = token.type == Token::Type::Percentage ? token.numeric_value * 255.0 / 100.0
                                                               : token.numeric_value * 255.0;
            }
            channels[channel++] = parsed;
        }
        if (channel < 3)
            return std::nullopt;
        return Color { clamp_channel(channels[0]), clamp_channel(channels[1]),
            clamp_channel(channels[2]), alpha_seen ? clamp_channel(channels[3]) : std::uint8_t { 255 } };
    }
    return std::nullopt;
}

struct LengthContext {
    float font_size = 16;
    float root_font_size = 16;
};

std::optional<LengthPercent> parse_length_percent(ComponentValue const& value,
    LengthContext const& context, bool allow_auto, bool allow_percent = true)
{
    if (!value.is_token())
        return std::nullopt;
    Token const& token = value.token();
    if (token.type == Token::Type::Ident && allow_auto && ascii_ci_equals(token.value, "auto"))
        return LengthPercent::auto_value();
    if (token.type == Token::Type::Number) {
        if (token.numeric_value == 0)
            return LengthPercent::px(0);
        return std::nullopt;
    }
    if (token.type == Token::Type::Percentage) {
        if (!allow_percent)
            return std::nullopt;
        return LengthPercent::percent(static_cast<float>(token.numeric_value));
    }
    if (token.type != Token::Type::Dimension)
        return std::nullopt;
    double const number = token.numeric_value;
    std::string_view const unit = token.unit;
    if (ascii_ci_equals(unit, "px"))
        return LengthPercent::px(static_cast<float>(number));
    if (ascii_ci_equals(unit, "em"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.font_size)));
    if (ascii_ci_equals(unit, "rem"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.root_font_size)));
    if (ascii_ci_equals(unit, "pt"))
        return LengthPercent::px(static_cast<float>(number * 4.0 / 3.0));
    if (ascii_ci_equals(unit, "ex") || ascii_ci_equals(unit, "ch"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.font_size) * 0.5));
    return std::nullopt;
}

std::optional<float> parse_border_width(ComponentValue const& value, LengthContext const& context)
{
    if (is_ident(&value, "thin"))
        return 1.0f;
    if (is_ident(&value, "medium"))
        return 3.0f;
    if (is_ident(&value, "thick"))
        return 5.0f;
    auto length = parse_length_percent(value, context, false, false);
    if (length && length->kind == LengthPercent::Kind::Px)
        return length->value;
    return std::nullopt;
}

std::optional<BorderStyle> parse_border_style(ComponentValue const& value)
{
    if (!value.is_token(Token::Type::Ident))
        return std::nullopt;
    std::string_view const name = value.token().value;
    if (ascii_ci_equals(name, "none") || ascii_ci_equals(name, "hidden"))
        return BorderStyle::None;
    for (std::string_view solid_ish :
        { "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset" }) {
        if (ascii_ci_equals(name, solid_ish))
            return BorderStyle::Solid;
    }
    return std::nullopt;
}

// --- The resolver -------------------------------------------------------------

} // namespace

// The compiled side of a StyleSet: rules in cascade order and the index
// over them, built once per media context.
struct RuleSet {
    std::vector<CompiledRule> rules;

    // The rule index: every complex selector filed under the id, else the
    // first class, else the type of its rightmost compound (lowercased,
    // since quirks mode and HTML type names compare without case), or
    // under "universal" when it names none. An element then tests only the
    // selectors that could match it; a selector that matches always has
    // its rightmost compound satisfied, so it is always among them.
    struct Candidate {
        std::uint32_t rule;
        std::uint32_t selector;
        // Identifiers the selector's ancestor compounds require: a
        // descendant or child combinator to the right of a compound makes
        // it an ancestor of the subject (a sibling combinator does not).
        std::vector<std::uint32_t> ancestor_hashes;
    };

    static std::vector<std::uint32_t> ancestor_hashes_of(ComplexSelector const& selector)
    {
        std::vector<std::uint32_t> hashes;
        for (std::size_t i = 0; i + 1 < selector.compounds.size(); ++i) {
            Combinator const combinator = selector.combinators[i];
            if (combinator != Combinator::Descendant && combinator != Combinator::Child)
                continue;
            for (SimpleSelector const& simple : selector.compounds[i].simples) {
                if (simple.kind == SimpleSelector::Kind::Id)
                    hashes.push_back(fnv1a("#" + lowercased(simple.name)));
                else if (simple.kind == SimpleSelector::Kind::Class)
                    hashes.push_back(fnv1a("." + lowercased(simple.name)));
                else if (simple.kind == SimpleSelector::Kind::Type)
                    hashes.push_back(fnv1a(lowercased(simple.name)));
            }
        }
        return hashes;
    }
    std::unordered_map<std::string, std::vector<Candidate>> by_id;
    std::unordered_map<std::string, std::vector<Candidate>> by_class;
    std::unordered_map<std::string, std::vector<Candidate>> by_type;
    std::vector<Candidate> universal;

    static std::string lowercased(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (char const c : text)
            out += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
        return out;
    }

    void build_index()
    {
        for (std::uint32_t r = 0; r < rules.size(); ++r) {
            std::vector<ComplexSelector> const& selectors = rules[r].selectors.selectors;
            for (std::uint32_t s = 0; s < selectors.size(); ++s) {
                std::string const* id = nullptr;
                std::string const* class_name = nullptr;
                std::string const* type = nullptr;
                if (!selectors[s].compounds.empty()) {
                    for (SimpleSelector const& simple : selectors[s].compounds.back().simples) {
                        if (simple.kind == SimpleSelector::Kind::Id && !id)
                            id = &simple.name;
                        else if (simple.kind == SimpleSelector::Kind::Class && !class_name)
                            class_name = &simple.name;
                        else if (simple.kind == SimpleSelector::Kind::Type && !type)
                            type = &simple.name;
                    }
                }
                Candidate const candidate { r, s, ancestor_hashes_of(selectors[s]) };
                if (id)
                    by_id[lowercased(*id)].push_back(candidate);
                else if (class_name)
                    by_class[lowercased(*class_name)].push_back(candidate);
                else if (type)
                    by_type[lowercased(*type)].push_back(candidate);
                else
                    universal.push_back(candidate);
            }
        }
    }

    MediaContext media;

    void compile_sheet(std::string_view text, bool user_agent, int& order)
    {
        Stylesheet sheet = parse_stylesheet(text);
        compile_rules(sheet.rules, user_agent, order);
    }

    void compile_rules(std::vector<Rule>& source, bool user_agent, int& order)
    {
        for (Rule& rule : source) {
            if (rule.is_at_rule()) {
                // @media blocks whose query the context satisfies contribute
                // their rules in place; other at-rules (@supports,
                // @font-face, @keyframes, @layer) are not supported yet.
                auto& at = std::get<AtRule>(rule.value);
                if (at.has_block && ascii_ci_equals(at.name, "media")
                    && media_prelude_matches(at.prelude, media))
                    compile_rules(at.child_rules, user_agent, order);
                continue;
            }
            if (!rule.is_qualified())
                continue;
            auto& qualified = std::get<QualifiedRule>(rule.value);
            std::optional<SelectorList> selectors = parse_selector_list(qualified.prelude);
            if (!selectors)
                continue;
            CompiledRule compiled;
            compiled.selectors = std::move(*selectors);
            compiled.declarations = std::move(qualified.declarations);
            compiled.user_agent = user_agent;
            compiled.order = order++;
            rules.push_back(std::move(compiled));
            // Nested child rules wait for the nesting-aware resolver.
        }
    }
};

// One resolution of one document against a RuleSet.
struct Resolver {
    RuleSet const& set;
    StyleMap map;
    float root_font_size = 16;
    std::vector<int> rule_stamp; // per rule: the element (stamp) it last matched
    std::vector<Specificity> rule_best; // its best matching selector for that element
    int stamp = 0;
    std::vector<std::uint32_t> matched_rules; // scratch, reused per element
    AncestorFilter ancestors; // the identifiers of the elements above the one being styled

    explicit Resolver(RuleSet const& the_set)
        : set(the_set)
        , rule_stamp(the_set.rules.size(), 0)
        , rule_best(the_set.rules.size(), Specificity {})
    {
    }

    // The rules matching the element, in rule order, each with the
    // specificity of its best matching selector.
    void matching_rules(dom::Element const& element, std::vector<std::uint32_t>& out)
    {
        ++stamp;
        out.clear();
        auto const consider = [&](std::vector<RuleSet::Candidate> const& candidates) {
            for (RuleSet::Candidate const& candidate : candidates) {
                if (!ancestors.may_contain_all(candidate.ancestor_hashes))
                    continue;
                ComplexSelector const& selector
                    = set.rules[candidate.rule].selectors.selectors[candidate.selector];
                if (!matches(selector, element))
                    continue;
                if (rule_stamp[candidate.rule] != stamp) {
                    rule_stamp[candidate.rule] = stamp;
                    rule_best[candidate.rule] = selector.specificity;
                    out.push_back(candidate.rule);
                } else if (selector.specificity > rule_best[candidate.rule]) {
                    rule_best[candidate.rule] = selector.specificity;
                }
            }
        };
        auto const consider_bucket
            = [&](std::unordered_map<std::string, std::vector<RuleSet::Candidate>> const& bucket,
                  std::string const& key) {
                  if (auto const it = bucket.find(key); it != bucket.end())
                      consider(it->second);
              };
        if (dom::Attr const* id = element.find_attribute("id"))
            consider_bucket(set.by_id, RuleSet::lowercased(id->value));
        if (dom::Attr const* classes = element.find_attribute("class")) {
            std::string_view const value = classes->value;
            std::size_t start = 0;
            while (start < value.size()) {
                while (start < value.size()
                    && is_tokenizer_whitespace(static_cast<unsigned char>(value[start])))
                    ++start;
                std::size_t end = start;
                while (end < value.size()
                    && !is_tokenizer_whitespace(static_cast<unsigned char>(value[end])))
                    ++end;
                if (end > start)
                    consider_bucket(set.by_class, RuleSet::lowercased(value.substr(start, end - start)));
                start = end;
            }
        }
        consider_bucket(set.by_type, RuleSet::lowercased(element.local_name()));
        consider(set.universal);
        std::sort(out.begin(), out.end());
    }

    // The identifiers an element offers to the selectors of its descendants.
    static std::vector<std::uint32_t> identifier_hashes(dom::Element const& element)
    {
        std::vector<std::uint32_t> hashes;
        hashes.push_back(fnv1a(RuleSet::lowercased(element.local_name())));
        if (dom::Attr const* id = element.find_attribute("id"))
            hashes.push_back(fnv1a("#" + RuleSet::lowercased(id->value)));
        if (dom::Attr const* classes = element.find_attribute("class")) {
            std::string_view const value = classes->value;
            std::size_t start = 0;
            while (start < value.size()) {
                while (start < value.size()
                    && is_tokenizer_whitespace(static_cast<unsigned char>(value[start])))
                    ++start;
                std::size_t end = start;
                while (end < value.size()
                    && !is_tokenizer_whitespace(static_cast<unsigned char>(value[end])))
                    ++end;
                if (end > start)
                    hashes.push_back(fnv1a("." + RuleSet::lowercased(value.substr(start, end - start))));
                start = end;
            }
        }
        return hashes;
    }

    void resolve_tree(dom::Node const& node, ComputedStyle const& parent_style)
    {
        ComputedStyle const* style_for_children = &parent_style;
        std::vector<std::uint32_t> offered;
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            ComputedStyle style = compute_for(element, parent_style);
            bool const is_root = node.parent() && !node.parent()->is_element();
            if (is_root)
                root_font_size = style.font_size; // rem resolves against this
            auto const [it, inserted] = map.emplace(&element, std::move(style));
            (void)inserted;
            style_for_children = &it->second;
            offered = identifier_hashes(element);
            for (std::uint32_t const hash : offered)
                ancestors.push(hash);
        }
        for (dom::Node const* child : node.children())
            resolve_tree(*child, *style_for_children);
        for (std::uint32_t const hash : offered)
            ancestors.pop(hash);
    }

    ComputedStyle compute_for(dom::Element const& element, ComputedStyle const& parent)
    {
        // Inherited properties flow in; the rest start at their initial values.
        ComputedStyle style;
        style.color = parent.color;
        style.font_size = parent.font_size;
        style.font_weight = parent.font_weight;
        style.font_style = parent.font_style;
        style.font_family = parent.font_family;
        style.line_height = parent.line_height;
        style.text_align = parent.text_align;
        style.white_space = parent.white_space;
        style.list_style_type = parent.list_style_type;

        std::vector<MatchedDeclaration> matched;
        matching_rules(element, matched_rules);
        for (std::uint32_t const index : matched_rules) {
            CompiledRule const& rule = set.rules[index];
            Specificity const specificity = rule_best[index];
            for (Declaration const& declaration : rule.declarations) {
                MatchedDeclaration entry;
                entry.declaration = &declaration;
                entry.specificity = specificity;
                entry.order = rule.order;
                if (rule.user_agent) {
                    entry.rank = static_cast<int>(declaration.important
                            ? CascadeRank::UserAgentImportant
                            : CascadeRank::UserAgentNormal);
                } else {
                    entry.rank = static_cast<int>(declaration.important
                            ? CascadeRank::AuthorImportant
                            : CascadeRank::AuthorNormal);
                }
                matched.push_back(entry);
            }
        }

        std::vector<Declaration> attribute_declarations;
        if (dom::Attr const* style_attribute = element.find_attribute("style")) {
            attribute_declarations = parse_declaration_list(style_attribute->value);
            int order = 1 << 20;
            for (Declaration const& declaration : attribute_declarations) {
                MatchedDeclaration entry;
                entry.declaration = &declaration;
                entry.order = order++;
                entry.rank = static_cast<int>(declaration.important
                        ? CascadeRank::StyleAttributeImportant
                        : CascadeRank::StyleAttributeNormal);
                matched.push_back(entry);
            }
        }

        std::stable_sort(matched.begin(), matched.end(), cascades_before);

        // font-size first: em and font-relative units in the same element's
        // other declarations resolve against it.
        for (MatchedDeclaration const& entry : matched) {
            if (ascii_ci_equals(entry.declaration->name, "font-size"))
                apply_font_size(style, parent, *entry.declaration);
        }
        bool border_top_color_set = false;
        bool border_right_color_set = false;
        bool border_bottom_color_set = false;
        bool border_left_color_set = false;
        for (MatchedDeclaration const& entry : matched) {
            apply(style, *entry.declaration, border_top_color_set, border_right_color_set,
                border_bottom_color_set, border_left_color_set);
        }

        // currentColor is the border default.
        if (!border_top_color_set)
            style.border_top.color = style.color;
        if (!border_right_color_set)
            style.border_right.color = style.color;
        if (!border_bottom_color_set)
            style.border_bottom.color = style.color;
        if (!border_left_color_set)
            style.border_left.color = style.color;
        // A border with style none has zero used width.
        for (BorderSide* side :
            { &style.border_top, &style.border_right, &style.border_bottom, &style.border_left }) {
            if (side->style == BorderStyle::None)
                side->width = 0;
        }
        return style;
    }

    void apply_font_size(ComputedStyle& style, ComputedStyle const& parent,
        Declaration const& declaration)
    {
        auto const values = significant(declaration.value);
        if (values.size() != 1)
            return;
        ComponentValue const& value = *values[0];
        if (value.is_token(Token::Type::Ident)) {
            std::string_view const name = value.token().value;
            struct Keyword {
                std::string_view name;
                float factor;
            };
            constexpr Keyword keywords[] = {
                { "xx-small", 3.0f / 5.0f },
                { "x-small", 3.0f / 4.0f },
                { "small", 8.0f / 9.0f },
                { "medium", 1.0f },
                { "large", 6.0f / 5.0f },
                { "x-large", 3.0f / 2.0f },
                { "xx-large", 2.0f },
            };
            for (Keyword const& keyword : keywords) {
                if (ascii_ci_equals(name, keyword.name)) {
                    style.font_size = 16.0f * keyword.factor;
                    return;
                }
            }
            if (ascii_ci_equals(name, "larger"))
                style.font_size = parent.font_size * 1.2f;
            else if (ascii_ci_equals(name, "smaller"))
                style.font_size = parent.font_size / 1.2f;
            return;
        }
        // em/% on font-size resolve against the parent's font-size.
        LengthContext const context { parent.font_size, root_font_size };
        auto length = parse_length_percent(value, context, false);
        if (!length)
            return;
        if (length->kind == LengthPercent::Kind::Px)
            style.font_size = length->value;
        else if (length->kind == LengthPercent::Kind::Percent)
            style.font_size = parent.font_size * length->value / 100.0f;
    }

    void apply(ComputedStyle& style, Declaration const& declaration, bool& border_top_color_set,
        bool& border_right_color_set, bool& border_bottom_color_set, bool& border_left_color_set)
    {
        std::string const name = [&] {
            std::string lowered;
            for (char const c : declaration.name)
                lowered += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
            return lowered;
        }();
        auto const values = significant(declaration.value);
        if (values.empty())
            return;
        LengthContext const context { style.font_size, root_font_size };

        auto const one_length = [&](LengthPercent& out, bool allow_auto) {
            if (values.size() != 1)
                return;
            if (auto length = parse_length_percent(*values[0], context, allow_auto))
                out = *length;
        };
        auto const one_color = [&](Color& out) {
            if (values.size() != 1)
                return false;
            if (auto color = parse_color_component(*values[0], style.color)) {
                out = *color;
                return true;
            }
            return false;
        };
        auto const four_lengths = [&](LengthPercent& top, LengthPercent& right,
                                      LengthPercent& bottom, LengthPercent& left, bool allow_auto) {
            std::vector<LengthPercent> sides;
            for (ComponentValue const* value : values) {
                auto length = parse_length_percent(*value, context, allow_auto);
                if (!length)
                    return;
                sides.push_back(*length);
            }
            if (sides.empty() || sides.size() > 4)
                return;
            top = sides[0];
            right = sides.size() > 1 ? sides[1] : sides[0];
            bottom = sides.size() > 2 ? sides[2] : sides[0];
            left = sides.size() > 3 ? sides[3] : right;
        };

        if (name == "display") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "none"))
                style.display = Display::None;
            else if (ascii_ci_equals(keyword, "inline"))
                style.display = Display::Inline;
            else if (ascii_ci_equals(keyword, "list-item"))
                style.display = Display::ListItem;
            else if (ascii_ci_equals(keyword, "flow-root"))
                style.display = Display::FlowRoot;
            else if (ascii_ci_equals(keyword, "flex") || ascii_ci_equals(keyword, "inline-flex"))
                style.display = Display::Flex; // inline-flex is block-level until inline-block lands
            else if (ascii_ci_equals(keyword, "block") || ascii_ci_equals(keyword, "grid")
                || ascii_ci_equals(keyword, "table"))
                style.display = Display::Block; // grid lays out as a block until it lands
            else if (ascii_ci_equals(keyword, "inline-block"))
                style.display = Display::Inline; // inline-block is not supported yet
            return;
        }
        if (name == "float") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "left"))
                style.floating = Float::Left;
            else if (ascii_ci_equals(keyword, "right"))
                style.floating = Float::Right;
            else if (ascii_ci_equals(keyword, "none"))
                style.floating = Float::None;
            return;
        }
        if (name == "clear") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "left"))
                style.clear = Clear::Left;
            else if (ascii_ci_equals(keyword, "right"))
                style.clear = Clear::Right;
            else if (ascii_ci_equals(keyword, "both"))
                style.clear = Clear::Both;
            else if (ascii_ci_equals(keyword, "none"))
                style.clear = Clear::None;
            return;
        }
        if (name == "overflow" || name == "overflow-x" || name == "overflow-y") {
            // One keyword, or one per axis; any axis that is not visible
            // makes the box contain its floats (the other axis computes to
            // auto then, as the specification says).
            if (values.empty() || values.size() > 2)
                return;
            bool visible = true;
            for (ComponentValue const* value : values) {
                if (!value->is_token(Token::Type::Ident))
                    return;
                std::string_view const keyword = value->token().value;
                if (ascii_ci_equals(keyword, "hidden") || ascii_ci_equals(keyword, "clip")
                    || ascii_ci_equals(keyword, "auto") || ascii_ci_equals(keyword, "scroll"))
                    visible = false;
                else if (!ascii_ci_equals(keyword, "visible"))
                    return;
            }
            style.overflow = visible ? Overflow::Visible : Overflow::Hidden;
            return;
        }
        if (name == "flex-direction") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "row"))
                style.flex_direction = FlexDirection::Row;
            else if (ascii_ci_equals(keyword, "row-reverse"))
                style.flex_direction = FlexDirection::RowReverse;
            else if (ascii_ci_equals(keyword, "column"))
                style.flex_direction = FlexDirection::Column;
            else if (ascii_ci_equals(keyword, "column-reverse"))
                style.flex_direction = FlexDirection::ColumnReverse;
            return;
        }
        if (name == "flex-wrap") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "nowrap"))
                style.flex_wrap = FlexWrap::NoWrap;
            else if (ascii_ci_equals(keyword, "wrap"))
                style.flex_wrap = FlexWrap::Wrap;
            else if (ascii_ci_equals(keyword, "wrap-reverse"))
                style.flex_wrap = FlexWrap::WrapReverse;
            return;
        }
        if (name == "flex-flow") {
            // A direction, a wrap, or both in either order.
            if (values.size() > 2)
                return;
            std::optional<FlexDirection> direction;
            std::optional<FlexWrap> wrap;
            for (ComponentValue const* value : values) {
                if (!value->is_token(Token::Type::Ident))
                    return;
                std::string_view const keyword = value->token().value;
                if (ascii_ci_equals(keyword, "row"))
                    direction = FlexDirection::Row;
                else if (ascii_ci_equals(keyword, "row-reverse"))
                    direction = FlexDirection::RowReverse;
                else if (ascii_ci_equals(keyword, "column"))
                    direction = FlexDirection::Column;
                else if (ascii_ci_equals(keyword, "column-reverse"))
                    direction = FlexDirection::ColumnReverse;
                else if (ascii_ci_equals(keyword, "nowrap"))
                    wrap = FlexWrap::NoWrap;
                else if (ascii_ci_equals(keyword, "wrap"))
                    wrap = FlexWrap::Wrap;
                else if (ascii_ci_equals(keyword, "wrap-reverse"))
                    wrap = FlexWrap::WrapReverse;
                else
                    return;
            }
            style.flex_direction = direction.value_or(FlexDirection::Row);
            style.flex_wrap = wrap.value_or(FlexWrap::NoWrap);
            return;
        }
        if (name == "justify-content") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "flex-start") || ascii_ci_equals(keyword, "start")
                || ascii_ci_equals(keyword, "left") || ascii_ci_equals(keyword, "normal"))
                style.justify_content = JustifyContent::FlexStart;
            else if (ascii_ci_equals(keyword, "flex-end") || ascii_ci_equals(keyword, "end")
                || ascii_ci_equals(keyword, "right"))
                style.justify_content = JustifyContent::FlexEnd;
            else if (ascii_ci_equals(keyword, "center"))
                style.justify_content = JustifyContent::Center;
            else if (ascii_ci_equals(keyword, "space-between"))
                style.justify_content = JustifyContent::SpaceBetween;
            else if (ascii_ci_equals(keyword, "space-around"))
                style.justify_content = JustifyContent::SpaceAround;
            else if (ascii_ci_equals(keyword, "space-evenly"))
                style.justify_content = JustifyContent::SpaceEvenly;
            return;
        }
        auto const alignment_of = [](std::string_view keyword) -> std::optional<AlignItems> {
            if (ascii_ci_equals(keyword, "stretch") || ascii_ci_equals(keyword, "normal"))
                return AlignItems::Stretch;
            if (ascii_ci_equals(keyword, "flex-start") || ascii_ci_equals(keyword, "start")
                || ascii_ci_equals(keyword, "self-start"))
                return AlignItems::FlexStart;
            if (ascii_ci_equals(keyword, "flex-end") || ascii_ci_equals(keyword, "end")
                || ascii_ci_equals(keyword, "self-end"))
                return AlignItems::FlexEnd;
            if (ascii_ci_equals(keyword, "center"))
                return AlignItems::Center;
            if (ascii_ci_equals(keyword, "baseline"))
                return AlignItems::Baseline;
            return std::nullopt;
        };
        if (name == "align-items") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            if (std::optional<AlignItems> const alignment = alignment_of(values[0]->token().value))
                style.align_items = *alignment;
            return;
        }
        if (name == "align-self") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "auto"))
                style.align_self = AlignItems::Auto;
            else if (std::optional<AlignItems> const alignment = alignment_of(keyword))
                style.align_self = *alignment;
            return;
        }
        if (name == "align-content") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "stretch") || ascii_ci_equals(keyword, "normal"))
                style.align_content = AlignContent::Stretch;
            else if (ascii_ci_equals(keyword, "flex-start") || ascii_ci_equals(keyword, "start"))
                style.align_content = AlignContent::FlexStart;
            else if (ascii_ci_equals(keyword, "flex-end") || ascii_ci_equals(keyword, "end"))
                style.align_content = AlignContent::FlexEnd;
            else if (ascii_ci_equals(keyword, "center"))
                style.align_content = AlignContent::Center;
            else if (ascii_ci_equals(keyword, "space-between"))
                style.align_content = AlignContent::SpaceBetween;
            else if (ascii_ci_equals(keyword, "space-around"))
                style.align_content = AlignContent::SpaceAround;
            else if (ascii_ci_equals(keyword, "space-evenly"))
                style.align_content = AlignContent::SpaceEvenly;
            return;
        }
        auto const non_negative_number = [](ComponentValue const& value) -> std::optional<float> {
            if (!value.is_token(Token::Type::Number))
                return std::nullopt;
            double const number = value.token().numeric_value;
            if (number < 0)
                return std::nullopt;
            return static_cast<float>(number);
        };
        auto const flex_basis_of = [&](ComponentValue const& value) -> std::optional<LengthPercent> {
            if (is_ident(&value, "content"))
                return LengthPercent::auto_value();
            auto length = parse_length_percent(value, context, true);
            if (!length || (!length->is_auto() && length->value < 0))
                return std::nullopt;
            return length;
        };
        if (name == "flex-grow" || name == "flex-shrink") {
            if (values.size() != 1)
                return;
            if (std::optional<float> const number = non_negative_number(*values[0]))
                (name == "flex-grow" ? style.flex_grow : style.flex_shrink) = *number;
            return;
        }
        if (name == "flex-basis") {
            if (values.size() != 1)
                return;
            if (std::optional<LengthPercent> const basis = flex_basis_of(*values[0]))
                style.flex_basis = *basis;
            return;
        }
        if (name == "flex") {
            // none | auto | initial | <grow> <shrink>? <basis>?, in either
            // order — the shorthand settles all three: a number alone means
            // <number> 1 0, a basis alone means 1 1 <basis>.
            if (values.size() == 1 && values[0]->is_token(Token::Type::Ident)) {
                std::string_view const keyword = values[0]->token().value;
                if (ascii_ci_equals(keyword, "none")) {
                    style.flex_grow = 0;
                    style.flex_shrink = 0;
                    style.flex_basis = LengthPercent::auto_value();
                } else if (ascii_ci_equals(keyword, "auto") || ascii_ci_equals(keyword, "content")) {
                    style.flex_grow = 1;
                    style.flex_shrink = 1;
                    style.flex_basis = LengthPercent::auto_value();
                } else if (ascii_ci_equals(keyword, "initial")) {
                    style.flex_grow = 0;
                    style.flex_shrink = 1;
                    style.flex_basis = LengthPercent::auto_value();
                }
                return;
            }
            if (values.size() > 3)
                return;
            std::optional<float> grow;
            std::optional<float> shrink;
            std::optional<LengthPercent> basis;
            for (ComponentValue const* value : values) {
                if (value->is_token(Token::Type::Number)) {
                    std::optional<float> const number = non_negative_number(*value);
                    if (!number)
                        return;
                    if (!grow)
                        grow = number;
                    else if (!shrink)
                        shrink = number;
                    else if (!basis && *number == 0)
                        basis = LengthPercent::px(0); // a unitless zero after two factors
                    else
                        return;
                    continue;
                }
                if (basis)
                    return;
                basis = flex_basis_of(*value);
                if (!basis)
                    return;
            }
            if (!grow && !basis)
                return;
            style.flex_grow = grow.value_or(1.0f);
            style.flex_shrink = shrink.value_or(1.0f);
            style.flex_basis = basis.value_or(grow ? LengthPercent::px(0) : LengthPercent::auto_value());
            return;
        }
        if (name == "gap" || name == "row-gap" || name == "column-gap") {
            auto const gap_px = [&](ComponentValue const& value) -> std::optional<float> {
                if (is_ident(&value, "normal"))
                    return 0.0f;
                auto length = parse_length_percent(value, context, false);
                if (!length)
                    return std::nullopt;
                if (length->kind == LengthPercent::Kind::Percent)
                    return 0.0f; // percentages wait for their base
                if (length->value < 0)
                    return std::nullopt;
                return length->value;
            };
            if (name == "gap") {
                if (values.size() > 2)
                    return;
                std::optional<float> const row = gap_px(*values[0]);
                std::optional<float> const column = values.size() == 2 ? gap_px(*values[1]) : row;
                if (!row || !column)
                    return;
                style.row_gap = *row;
                style.column_gap = *column;
                return;
            }
            if (values.size() != 1)
                return;
            if (std::optional<float> const gap = gap_px(*values[0]))
                (name == "row-gap" ? style.row_gap : style.column_gap) = *gap;
            return;
        }
        if (name == "order") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Number))
                return;
            double const number = values[0]->token().numeric_value;
            if (number != static_cast<double>(static_cast<int>(number)))
                return;
            style.order = static_cast<int>(number);
            return;
        }
        if (name == "color") {
            (void)one_color(style.color);
            return;
        }
        if (name == "background-color") {
            (void)one_color(style.background_color);
            return;
        }
        if (name == "background") {
            // The color-only form, or a color among other layers' parts.
            for (ComponentValue const* value : values) {
                if (auto color = parse_color_component(*value, style.color)) {
                    style.background_color = *color;
                    return;
                }
            }
            return;
        }
        if (name == "width") {
            one_length(style.width, true);
            return;
        }
        if (name == "height") {
            one_length(style.height, true);
            return;
        }
        if (name == "margin") {
            four_lengths(style.margin_top, style.margin_right, style.margin_bottom,
                style.margin_left, true);
            return;
        }
        if (name == "margin-top") {
            one_length(style.margin_top, true);
            return;
        }
        if (name == "margin-right") {
            one_length(style.margin_right, true);
            return;
        }
        if (name == "margin-bottom") {
            one_length(style.margin_bottom, true);
            return;
        }
        if (name == "margin-left") {
            one_length(style.margin_left, true);
            return;
        }
        if (name == "padding") {
            four_lengths(style.padding_top, style.padding_right, style.padding_bottom,
                style.padding_left, false);
            return;
        }
        if (name == "padding-top") {
            one_length(style.padding_top, false);
            return;
        }
        if (name == "padding-right") {
            one_length(style.padding_right, false);
            return;
        }
        if (name == "padding-bottom") {
            one_length(style.padding_bottom, false);
            return;
        }
        if (name == "padding-left") {
            one_length(style.padding_left, false);
            return;
        }
        if (name == "font-size")
            return; // handled in the first pass
        if (name == "font-weight") {
            if (values.size() != 1)
                return;
            ComponentValue const& value = *values[0];
            if (value.is_token(Token::Type::Number)) {
                double const number = value.token().numeric_value;
                if (number >= 1 && number <= 1000)
                    style.font_weight = static_cast<int>(number);
                return;
            }
            if (is_ident(&value, "normal"))
                style.font_weight = 400;
            else if (is_ident(&value, "bold"))
                style.font_weight = 700;
            else if (is_ident(&value, "bolder"))
                style.font_weight = style.font_weight < 350 ? 400
                    : style.font_weight < 550               ? 700
                                                            : 900;
            else if (is_ident(&value, "lighter"))
                style.font_weight = style.font_weight < 550 ? 100
                    : style.font_weight < 750               ? 400
                                                            : 700;
            return;
        }
        if (name == "font-family") {
            // Comma-separated names: a string, or identifiers joined by
            // single spaces. One bad name drops the whole declaration.
            auto families = std::make_shared<std::vector<std::string>>();
            std::string current;
            bool after_string = false;
            for (ComponentValue const* value : values) {
                if (value->is_token(Token::Type::Comma)) {
                    if (current.empty())
                        return;
                    families->push_back(std::move(current));
                    current.clear();
                    after_string = false;
                    continue;
                }
                if (value->is_token(Token::Type::Ident) && !after_string) {
                    if (!current.empty())
                        current += ' ';
                    current += value->token().value;
                    continue;
                }
                if (value->is_token(Token::Type::String) && current.empty()) {
                    current = value->token().value;
                    after_string = true;
                    continue;
                }
                return;
            }
            if (current.empty())
                return;
            families->push_back(std::move(current));
            style.font_family = std::move(families);
            return;
        }
        if (name == "font-style") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "normal"))
                style.font_style = FontStyle::Normal;
            else if (is_ident(values[0], "italic") || is_ident(values[0], "oblique"))
                style.font_style = FontStyle::Italic;
            return;
        }
        if (name == "line-height") {
            if (values.size() != 1)
                return;
            ComponentValue const& value = *values[0];
            if (is_ident(&value, "normal")) {
                style.line_height = { LineHeight::Kind::Normal, 0 };
                return;
            }
            if (value.is_token(Token::Type::Number)) {
                style.line_height = { LineHeight::Kind::Number,
                    static_cast<float>(value.token().numeric_value) };
                return;
            }
            if (value.is_token(Token::Type::Percentage)) {
                style.line_height = { LineHeight::Kind::Px,
                    style.font_size * static_cast<float>(value.token().numeric_value) / 100.0f };
                return;
            }
            auto length = parse_length_percent(value, context, false, false);
            if (length && length->kind == LengthPercent::Kind::Px)
                style.line_height = { LineHeight::Kind::Px, length->value };
            return;
        }
        if (name == "text-align") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "left") || is_ident(values[0], "start"))
                style.text_align = TextAlign::Left;
            else if (is_ident(values[0], "right") || is_ident(values[0], "end"))
                style.text_align = TextAlign::Right;
            else if (is_ident(values[0], "center"))
                style.text_align = TextAlign::Center;
            else if (is_ident(values[0], "justify"))
                style.text_align = TextAlign::Justify;
            return;
        }
        if (name == "white-space") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "normal"))
                style.white_space = WhiteSpace::Normal;
            else if (is_ident(values[0], "pre"))
                style.white_space = WhiteSpace::Pre;
            else if (is_ident(values[0], "nowrap"))
                style.white_space = WhiteSpace::NoWrap;
            else if (is_ident(values[0], "pre-wrap"))
                style.white_space = WhiteSpace::PreWrap;
            else if (is_ident(values[0], "pre-line"))
                style.white_space = WhiteSpace::PreLine;
            return;
        }
        if (name == "list-style-type" || name == "list-style") {
            for (ComponentValue const* value : values) {
                if (is_ident(value, "disc"))
                    style.list_style_type = ListStyleType::Disc;
                else if (is_ident(value, "circle"))
                    style.list_style_type = ListStyleType::Circle;
                else if (is_ident(value, "square"))
                    style.list_style_type = ListStyleType::Square;
                else if (is_ident(value, "decimal"))
                    style.list_style_type = ListStyleType::Decimal;
                else if (is_ident(value, "none"))
                    style.list_style_type = ListStyleType::None;
                else
                    continue;
                return;
            }
            return;
        }
        if (name == "text-decoration" || name == "text-decoration-line") {
            for (ComponentValue const* value : values) {
                if (is_ident(value, "none")) {
                    style.text_decoration = TextDecorationLine::None;
                    return;
                }
                if (is_ident(value, "underline")) {
                    style.text_decoration = TextDecorationLine::Underline;
                    return;
                }
                if (is_ident(value, "line-through")) {
                    style.text_decoration = TextDecorationLine::LineThrough;
                    return;
                }
            }
            return;
        }

        // Borders.
        auto const apply_border_shorthand = [&](BorderSide& side, bool& color_set) {
            BorderSide result;
            result.width = 3; // medium, when a style appears without a width
            bool style_seen = false;
            bool color_seen = false;
            for (ComponentValue const* value : values) {
                if (auto border_style = parse_border_style(*value)) {
                    result.style = *border_style;
                    style_seen = true;
                    continue;
                }
                if (auto width = parse_border_width(*value, context)) {
                    result.width = *width;
                    continue;
                }
                if (auto color = parse_color_component(*value, style.color)) {
                    result.color = *color;
                    color_seen = true;
                    continue;
                }
                return; // junk: whole declaration ignored
            }
            if (!style_seen)
                result.style = BorderStyle::None;
            side = result;
            color_set = color_seen;
        };

        if (name == "border") {
            apply_border_shorthand(style.border_top, border_top_color_set);
            style.border_right = style.border_top;
            style.border_bottom = style.border_top;
            style.border_left = style.border_top;
            border_right_color_set = border_top_color_set;
            border_bottom_color_set = border_top_color_set;
            border_left_color_set = border_top_color_set;
            return;
        }
        if (name == "border-top") {
            apply_border_shorthand(style.border_top, border_top_color_set);
            return;
        }
        if (name == "border-right") {
            apply_border_shorthand(style.border_right, border_right_color_set);
            return;
        }
        if (name == "border-bottom") {
            apply_border_shorthand(style.border_bottom, border_bottom_color_set);
            return;
        }
        if (name == "border-left") {
            apply_border_shorthand(style.border_left, border_left_color_set);
            return;
        }
        if (name == "border-width") {
            std::vector<float> widths;
            for (ComponentValue const* value : values) {
                auto width = parse_border_width(*value, context);
                if (!width)
                    return;
                widths.push_back(*width);
            }
            if (widths.empty() || widths.size() > 4)
                return;
            style.border_top.width = widths[0];
            style.border_right.width = widths.size() > 1 ? widths[1] : widths[0];
            style.border_bottom.width = widths.size() > 2 ? widths[2] : widths[0];
            style.border_left.width = widths.size() > 3 ? widths[3] : style.border_right.width;
            return;
        }
        if (name == "border-style") {
            std::vector<BorderStyle> styles;
            for (ComponentValue const* value : values) {
                auto border_style = parse_border_style(*value);
                if (!border_style)
                    return;
                styles.push_back(*border_style);
            }
            if (styles.empty() || styles.size() > 4)
                return;
            style.border_top.style = styles[0];
            style.border_right.style = styles.size() > 1 ? styles[1] : styles[0];
            style.border_bottom.style = styles.size() > 2 ? styles[2] : styles[0];
            style.border_left.style = styles.size() > 3 ? styles[3] : style.border_right.style;
            // A bare border-style implies medium width where none was given.
            for (BorderSide* side : { &style.border_top, &style.border_right, &style.border_bottom,
                     &style.border_left }) {
                if (side->style == BorderStyle::Solid && side->width == 0)
                    side->width = 3;
            }
            return;
        }
        if (name == "border-color") {
            std::vector<Color> colors;
            for (ComponentValue const* value : values) {
                auto color = parse_color_component(*value, style.color);
                if (!color)
                    return;
                colors.push_back(*color);
            }
            if (colors.empty() || colors.size() > 4)
                return;
            style.border_top.color = colors[0];
            style.border_right.color = colors.size() > 1 ? colors[1] : colors[0];
            style.border_bottom.color = colors.size() > 2 ? colors[2] : colors[0];
            style.border_left.color = colors.size() > 3 ? colors[3] : style.border_right.color;
            border_top_color_set = border_right_color_set = true;
            border_bottom_color_set = border_left_color_set = true;
            return;
        }
        // Unknown properties fall on the floor, by design.
    }
};

StyleSet::StyleSet(std::vector<SheetSource> const& sheets, MediaContext const& media)
    : m_rules(std::make_unique<RuleSet>())
{
    m_rules->media = media;
    int order = 0;
    m_rules->compile_sheet(ua_stylesheet, true, order);
    for (SheetSource const& sheet : sheets)
        m_rules->compile_sheet(sheet.text, false, order);
    m_rules->build_index();
}

StyleSet::~StyleSet() = default;
StyleSet::StyleSet(StyleSet&&) noexcept = default;
StyleSet& StyleSet::operator=(StyleSet&&) noexcept = default;

std::size_t StyleSet::rule_count() const { return m_rules->rules.size(); }

std::size_t StyleSet::universal_count() const { return m_rules->universal.size(); }

MediaContext const& StyleSet::media() const { return m_rules->media; }

StyleMap resolve_styles(dom::Document const& document, StyleSet const& set)
{
    Resolver resolver(*set.m_rules);
    ComputedStyle initial;
    resolver.resolve_tree(document, initial);
    return std::move(resolver.map);
}

StyleMap resolve_styles(dom::Document const& document, std::vector<SheetSource> const& sheets,
    MediaContext const& media)
{
    return resolve_styles(document, StyleSet(sheets, media));
}

StyleMap resolve_styles(dom::Document const& document)
{
    return resolve_styles(document, collect_stylesheets(document, nullptr, {}));
}

}
