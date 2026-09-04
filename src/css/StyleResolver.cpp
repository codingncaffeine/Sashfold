#include "css/StyleResolver.h"

#include "core/Ascii.h"
#include "core/Bidi.h"
#include "core/Unicode.h"
#include "css/Grid.h"
#include "css/Parser.h"
#include "css/Selector.h"
#include "dom/Dom.h"
#include "net/Url.h"
#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
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
h1, h2, h3, h4, h5, h6, dir { display: block }
[hidden] { display: none }
table { display: table; border-collapse: separate; border-spacing: 2px; border-color: gray }
caption { display: table-caption }
tbody { display: table-row-group; vertical-align: middle }
thead { display: table-header-group; vertical-align: middle }
tfoot { display: table-footer-group; vertical-align: middle }
tr { display: table-row; vertical-align: inherit }
col { display: table-column }
colgroup { display: table-column-group }
td, th { display: table-cell; vertical-align: inherit; padding: 1px }
th { font-weight: bold }
li { display: list-item }
p, blockquote, figure, dl, ol, ul, pre, listing, plaintext, xmp { margin-top: 1em; margin-bottom: 1em }
blockquote, figure { margin-left: 40px; margin-right: 40px }
ol, ul, dir, menu { padding-inline-start: 40px }
ol, ul, dir, menu { counter-reset: list-item }
ol { list-style-type: decimal }
ul ul { list-style-type: circle }
ul ul ul { list-style-type: square }
dd { margin-inline-start: 40px }
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
sub { vertical-align: sub; font-size: smaller }
sup { vertical-align: super; font-size: smaller }
pre, listing, plaintext, xmp { white-space: pre }
pre, listing, plaintext, xmp, code, kbd, samp, tt { font-family: monospace }
nobr { white-space: nowrap }
a:link { color: rgb(0, 0, 238); text-decoration: underline }
s, strike, del { text-decoration: line-through }
u, ins { text-decoration: underline }
center, caption, th { text-align: center }
hr { margin-top: 0.5em; margin-bottom: 0.5em; border-top: 1px solid; color: gray }
mark { background-color: yellow }
input, textarea, select, button { font-size: 13.333px; line-height: normal; font-family: sans-serif }
input[type=hidden] { display: none }
input[type=checkbox], input[type=radio] { margin: 3px 3px 3px 4px }
textarea { white-space: pre-wrap }
[dir=ltr i] { direction: ltr }
[dir=rtl i] { direction: rtl }
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
    std::shared_ptr<net::Url const> base; // the sheet's URL: what its url() values resolve against
};

struct MatchedDeclaration {
    Declaration const* declaration = nullptr;
    net::Url const* base = nullptr; // for the URLs in the declaration
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

// --- Presentational hints -----------------------------------------------------

// HTML's rules for parsing dimension values: digits, an optional fraction,
// an optional percent sign; anything else is not a dimension.
std::string legacy_dimension(std::string_view value)
{
    std::size_t i = 0;
    while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n' || value[i] == '\r'))
        ++i;
    std::size_t const start = i;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9')
        ++i;
    if (i == start)
        return {};
    std::string number(value.substr(start, i - start));
    if (i < value.size() && value[i] == '.') {
        std::size_t const dot = i++;
        while (i < value.size() && value[i] >= '0' && value[i] <= '9')
            ++i;
        if (i > dot + 1)
            number += value.substr(dot, i - dot);
    }
    bool const percent = i < value.size() && value[i] == '%';
    return number + (percent ? "%" : "px");
}

// A legacy color: a name or a #-color as CSS has it, or six (or three)
// hex digits without the sign, which HTML allows.
std::string legacy_color(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n'))
        value.remove_suffix(1);
    if (value.empty())
        return {};
    if (value.front() == '#')
        return std::string(value);
    bool hex = true;
    bool alpha = true;
    for (char const c : value) {
        bool const digit = c >= '0' && c <= '9';
        bool const hex_letter = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        bool const letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        hex = hex && (digit || hex_letter);
        alpha = alpha && letter;
    }
    if (hex && (value.size() == 3 || value.size() == 6))
        return "#" + std::string(value);
    if (alpha)
        return std::string(value);
    return {};
}

// The text `dir=auto` reads to find its direction: the element's own text
// in tree order, stopping at the first strongly directional character.
// Skipped, as HTML says: a descendant that states a direction of its own —
// its text answers for itself, not for this element — and `bdi`, `script`,
// `style` and `textarea`, whose content is not the element's text.
std::u32string auto_direction_text(dom::Element const& element)
{
    std::u32string text;
    auto const opaque = [](dom::Element const& candidate) {
        std::string const& name = candidate.local_name();
        return name == "bdi" || name == "script" || name == "style" || name == "textarea";
    };
    auto const walk = [&](auto&& self, dom::Node const& node) -> void {
        for (dom::Node const* const child : node.children()) {
            if (child->is_text()) {
                text += decode_utf8(static_cast<dom::Text const&>(*child).data, true);
                continue;
            }
            if (!child->is_element())
                continue;
            dom::Element const& candidate = static_cast<dom::Element const&>(*child);
            if (opaque(candidate) || candidate.has_attribute("dir"))
                continue;
            self(self, candidate);
        }
    };
    walk(walk, element);
    return text;
}

// The HTML rendering section's mappings of attributes to properties, as
// declarations: sizes and colors of tables, cells, images and rules; the
// alignment attributes; a table's cellspacing, cellpadding and border,
// the last two reaching its cells.
std::string presentational_hints(dom::Element const& element)
{
    if (!element.is_html())
        return {};
    std::string css;
    auto const attribute = [&](char const* name) -> std::optional<std::string_view> {
        dom::Attr const* const found = element.find_attribute(name);
        if (!found)
            return std::nullopt;
        return std::string_view(found->value);
    };
    auto const add = [&](std::string_view property, std::string const& value) {
        if (value.empty())
            return;
        css += property;
        css += ':';
        css += value;
        css += ';';
    };
    std::string const& tag = element.local_name();
    bool const cell = tag == "td" || tag == "th";
    bool const row = tag == "tr";
    bool const row_group = tag == "tbody" || tag == "thead" || tag == "tfoot";
    bool const column = tag == "col" || tag == "colgroup";
    bool const table = tag == "table";
    bool const heading = tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6';
    bool const embedded = tag == "img" || tag == "iframe" || tag == "video" || tag == "canvas"
        || tag == "embed" || tag == "object";

    // dir=auto: the direction is the one its own content reads as. The
    // ltr and rtl values are user-agent rules ([dir=ltr i], [dir=rtl i]);
    // this one cannot be written as a selector, because the answer is in
    // the text. HTML's traversal skips a descendant that states a direction
    // of its own, and the elements whose content is not the element's text.
    if (std::optional<std::string_view> const dir = attribute("dir")) {
        if (ascii_ci_equals(*dir, "auto"))
            add("direction", first_strong_is_rtl(auto_direction_text(element)) ? "rtl" : "ltr");
    }
    if (table || cell || column || tag == "hr" || embedded) {
        if (std::optional<std::string_view> const width = attribute("width"))
            add("width", legacy_dimension(*width));
    }
    if (table || cell || row || embedded) {
        if (std::optional<std::string_view> const height = attribute("height"))
            add("height", legacy_dimension(*height));
    }
    if (tag == "hr") {
        if (std::optional<std::string_view> const size = attribute("size"))
            add("height", legacy_dimension(*size));
    }
    // The list attributes, through the counter they really mean: `start` puts
    // the counter one below the first item's number, since every item adds
    // one of its own; `value` writes the number this item wears, and the ones
    // after it carry on from there.
    if (tag == "ol" || tag == "li") {
        auto const integer = [](std::string_view text) -> std::optional<long long> {
            std::size_t i = 0;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
                ++i;
            bool const negative = i < text.size() && text[i] == '-';
            if (i < text.size() && (text[i] == '-' || text[i] == '+'))
                ++i;
            if (i >= text.size() || text[i] < '0' || text[i] > '9')
                return std::nullopt;
            long long value = 0;
            for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                value = value * 10 + (text[i] - '0');
                if (value > 2147483647LL)
                    value = 2147483647LL;
            }
            return negative ? -value : value;
        };
        if (tag == "ol") {
            if (std::optional<std::string_view> const start = attribute("start")) {
                if (std::optional<long long> const n = integer(*start))
                    add("counter-reset", "list-item " + std::to_string(*n - 1));
            }
        } else if (std::optional<std::string_view> const value = attribute("value")) {
            if (std::optional<long long> const n = integer(*value))
                add("counter-set", "list-item " + std::to_string(*n));
        }
    }
    // `type` on a list names the marker's counter style.
    if (tag == "ol" || tag == "ul" || tag == "li") {
        if (std::optional<std::string_view> const type = attribute("type")) {
            std::string_view style;
            if (type->size() == 1) {
                switch (type->front()) {
                case '1': style = "decimal"; break;
                case 'a': style = "lower-alpha"; break;
                case 'A': style = "upper-alpha"; break;
                case 'i': style = "lower-roman"; break;
                case 'I': style = "upper-roman"; break;
                default: break;
                }
            }
            if (style.empty() && (tag == "ul" || tag == "li")) {
                if (ascii_ci_equals(*type, "disc"))
                    style = "disc";
                else if (ascii_ci_equals(*type, "circle"))
                    style = "circle";
                else if (ascii_ci_equals(*type, "square"))
                    style = "square";
            }
            if (!style.empty())
                add("list-style-type", std::string(style));
        }
    }
    if (tag == "body" || table || cell || row || row_group || column || tag == "caption") {
        if (std::optional<std::string_view> const color = attribute("bgcolor"))
            add("background-color", legacy_color(*color));
    }
    if (tag == "body") {
        if (std::optional<std::string_view> const color = attribute("text"))
            add("color", legacy_color(*color));
    }
    if (tag == "font") {
        if (std::optional<std::string_view> const color = attribute("color"))
            add("color", legacy_color(*color));
        if (std::optional<std::string_view> const face = attribute("face"))
            add("font-family", std::string(*face));
        if (std::optional<std::string_view> const size = attribute("size")) {
            // 1..7 on the HTML scale; a sign makes it relative to 3.
            std::string_view text = *size;
            int n = 0;
            bool relative = false;
            int sign = 1;
            if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
                relative = true;
                sign = text.front() == '-' ? -1 : 1;
                text.remove_prefix(1);
            }
            for (char const c : text) {
                if (c < '0' || c > '9')
                    break;
                n = n * 10 + (c - '0');
            }
            if (!text.empty() && text.front() >= '0' && text.front() <= '9') {
                int const level = std::clamp(relative ? 3 + sign * n : n, 1, 7);
                static constexpr char const* sizes[]
                    = { "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large" };
                add("font-size", sizes[level - 1]);
            }
        }
    }
    if (std::optional<std::string_view> const align = attribute("align")) {
        std::string_view const value = *align;
        bool const left = ascii_ci_equals(value, "left");
        bool const right = ascii_ci_equals(value, "right");
        bool const center = ascii_ci_equals(value, "center") || ascii_ci_equals(value, "middle");
        if (table) {
            if (center)
                add("margin-left", "auto"), add("margin-right", "auto");
            else if (left)
                add("float", "left");
            else if (right)
                add("float", "right");
        } else if (embedded) {
            if (left)
                add("float", "left");
            else if (right)
                add("float", "right");
        } else if (tag == "hr") {
            if (left)
                add("margin-right", "auto");
            else if (right)
                add("margin-left", "auto");
        } else if (tag == "div" || tag == "p" || heading || cell || row || row_group || column
            || tag == "caption" || tag == "legend" || tag == "figcaption") {
            if (left)
                add("text-align", "left");
            else if (right)
                add("text-align", "right");
            else if (center)
                add("text-align", "center");
            else if (ascii_ci_equals(value, "justify"))
                add("text-align", "justify");
        }
    }
    if (cell || row || row_group || column) {
        if (std::optional<std::string_view> const valign = attribute("valign")) {
            std::string_view const value = *valign;
            if (ascii_ci_equals(value, "top"))
                add("vertical-align", "top");
            else if (ascii_ci_equals(value, "middle") || ascii_ci_equals(value, "center"))
                add("vertical-align", "middle");
            else if (ascii_ci_equals(value, "bottom"))
                add("vertical-align", "bottom");
            else if (ascii_ci_equals(value, "baseline"))
                add("vertical-align", "baseline");
        }
    }
    if (cell && attribute("nowrap"))
        add("white-space", "nowrap");
    if (table) {
        if (std::optional<std::string_view> const spacing = attribute("cellspacing"))
            add("border-spacing", legacy_dimension(*spacing));
        if (std::optional<std::string_view> const border = attribute("border")) {
            std::string const width = legacy_dimension(*border);
            if (width.empty()) {
                add("border-width", "1px"), add("border-style", "outset");
            } else if (width != "0px") {
                add("border-width", width), add("border-style", "outset");
            }
        }
    }
    if (cell) {
        // The enclosing table's cellpadding and border reach the cells.
        for (dom::Node const* parent = element.parent(); parent; parent = parent->parent()) {
            if (!parent->is_element())
                continue;
            auto const& ancestor = static_cast<dom::Element const&>(*parent);
            if (!ancestor.is_html("table"))
                continue;
            if (dom::Attr const* const padding = ancestor.find_attribute("cellpadding"))
                add("padding", legacy_dimension(padding->value));
            if (dom::Attr const* const border = ancestor.find_attribute("border")) {
                std::string const width = legacy_dimension(border->value);
                if (width != "0px")
                    add("border-width", "1px"), add("border-style", "inset");
            }
            break;
        }
    }
    if (tag == "img") {
        if (std::optional<std::string_view> const space = attribute("hspace")) {
            std::string const length = legacy_dimension(*space);
            add("margin-left", length), add("margin-right", length);
        }
        if (std::optional<std::string_view> const space = attribute("vspace")) {
            std::string const length = legacy_dimension(*space);
            add("margin-top", length), add("margin-bottom", length);
        }
        if (std::optional<std::string_view> const border = attribute("border")) {
            std::string const width = legacy_dimension(*border);
            if (!width.empty())
                add("border-width", width), add("border-style", "solid");
        }
    }
    return css;
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

// The counter styles this engine spells, by the name list-style-type and
// counter()'s second argument share. `lower-latin` and `upper-latin` are
// the same lists as the `-alpha` pair; a name it does not know comes back
// empty, and the declaration holding it is dropped.
std::optional<ListStyleType> parse_list_style_type(ComponentValue const* value)
{
    if (!value || !value->is_token(Token::Type::Ident))
        return std::nullopt;
    static constexpr std::pair<std::string_view, ListStyleType> names[] = {
        { "disc", ListStyleType::Disc },
        { "circle", ListStyleType::Circle },
        { "square", ListStyleType::Square },
        { "decimal", ListStyleType::Decimal },
        { "decimal-leading-zero", ListStyleType::DecimalLeadingZero },
        { "lower-roman", ListStyleType::LowerRoman },
        { "upper-roman", ListStyleType::UpperRoman },
        { "lower-alpha", ListStyleType::LowerAlpha },
        { "lower-latin", ListStyleType::LowerAlpha },
        { "upper-alpha", ListStyleType::UpperAlpha },
        { "upper-latin", ListStyleType::UpperAlpha },
        { "lower-greek", ListStyleType::LowerGreek },
        { "armenian", ListStyleType::Armenian },
        { "georgian", ListStyleType::Georgian },
        { "none", ListStyleType::None },
    };
    for (auto const& [name, type] : names) {
        if (ascii_ci_equals(value->token().value, name))
            return type;
    }
    return std::nullopt;
}

// An integer as the counter properties want it: no unit, no fraction, and
// clamped to what an int holds — the suite writes both -2147483648 and
// 2147483647 and expects them back whole.
std::optional<int> parse_counter_integer(ComponentValue const* value)
{
    if (!value || !value->is_token(Token::Type::Number))
        return std::nullopt;
    if (value->token().numeric_type != Token::NumericType::Integer)
        return std::nullopt; // a fraction is not an integer
    double const number = value->token().numeric_value;
    if (number <= static_cast<double>(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::min();
    if (number >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(number);
}

// CSS Overflow §3: `visible` and `clip` cannot stand beside an axis that
// scrolls — `visible` becomes `auto` there and `clip` becomes `hidden`,
// both of which clip. The box clips at all when either axis does.
void settle_overflow(ComputedStyle& style)
{
    Overflow const across = style.overflow_x;
    Overflow const down = style.overflow_y;
    if (down == Overflow::Hidden)
        style.overflow_x = Overflow::Hidden;
    if (across == Overflow::Hidden)
        style.overflow_y = Overflow::Hidden;
    style.overflow = style.overflow_x == Overflow::Visible && style.overflow_y == Overflow::Visible
        ? Overflow::Visible
        : Overflow::Hidden;
}

// The names of a font-family value (or a font shorthand's tail):
// comma-separated, each a string or identifiers joined by single spaces.
// Null when any name is bad.
std::shared_ptr<std::vector<std::string>> parse_family_list(
    std::vector<ComponentValue const*> const& values)
{
    auto families = std::make_shared<std::vector<std::string>>();
    std::string current;
    bool after_string = false;
    for (ComponentValue const* value : values) {
        if (value->is_token(Token::Type::Comma)) {
            if (current.empty())
                return nullptr;
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
        return nullptr;
    }
    if (current.empty())
        return nullptr;
    families->push_back(std::move(current));
    return families;
}

bool is_font_size_value(ComponentValue const& value)
{
    if (!value.is_token())
        return false;
    Token const& token = value.token();
    if (token.type == Token::Type::Dimension || token.type == Token::Type::Percentage)
        return true;
    if (token.type == Token::Type::Number)
        return token.numeric_value == 0;
    if (token.type != Token::Type::Ident)
        return false;
    for (std::string_view const keyword : { "xx-small", "x-small", "small", "medium", "large",
             "x-large", "xx-large", "xxx-large", "larger", "smaller" }) {
        if (ascii_ci_equals(token.value, keyword))
            return true;
    }
    return false;
}

// A font shorthand split into its parts: what precedes the size (style,
// variant, weight, normal), the size, a line-height after a slash, and the
// family. nullopt when the value is not one — the system font keywords
// included.
struct FontShorthand {
    std::vector<ComponentValue const*> before;
    ComponentValue const* size = nullptr;
    ComponentValue const* line_height = nullptr;
    std::vector<ComponentValue const*> family;
};

std::optional<FontShorthand> split_font_shorthand(std::vector<ComponentValue const*> const& values)
{
    FontShorthand parts;
    std::size_t i = 0;
    for (; i < values.size() && !is_font_size_value(*values[i]); ++i) {
        if (parts.before.size() >= 4)
            return std::nullopt;
        parts.before.push_back(values[i]);
    }
    if (i >= values.size())
        return std::nullopt;
    parts.size = values[i++];
    if (i < values.size() && values[i]->is_token(Token::Type::Delim)
        && values[i]->token().delim == U'/') {
        ++i;
        if (i >= values.size())
            return std::nullopt;
        parts.line_height = values[i++];
    }
    for (; i < values.size(); ++i)
        parts.family.push_back(values[i]);
    if (parts.family.empty())
        return std::nullopt;
    return parts;
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
    { "antiquewhite", Color { 250, 235, 215, 255 } },
    { "aquamarine", Color { 127, 255, 212, 255 } },
    { "azure", Color { 240, 255, 255, 255 } },
    { "bisque", Color { 255, 228, 196, 255 } },
    { "blanchedalmond", Color { 255, 235, 205, 255 } },
    { "blueviolet", Color { 138, 43, 226, 255 } },
    { "burlywood", Color { 222, 184, 135, 255 } },
    { "cadetblue", Color { 95, 158, 160, 255 } },
    { "chartreuse", Color { 127, 255, 0, 255 } },
    { "cornsilk", Color { 255, 248, 220, 255 } },
    { "darkcyan", Color { 0, 139, 139, 255 } },
    { "darkgoldenrod", Color { 184, 134, 11, 255 } },
    { "darkkhaki", Color { 189, 183, 107, 255 } },
    { "darkmagenta", Color { 139, 0, 139, 255 } },
    { "darkolivegreen", Color { 85, 107, 47, 255 } },
    { "darkorchid", Color { 153, 50, 204, 255 } },
    { "darksalmon", Color { 233, 150, 122, 255 } },
    { "darkseagreen", Color { 143, 188, 143, 255 } },
    { "darkslateblue", Color { 72, 61, 139, 255 } },
    { "darkslategray", Color { 47, 79, 79, 255 } },
    { "darkslategrey", Color { 47, 79, 79, 255 } },
    { "darkturquoise", Color { 0, 206, 209, 255 } },
    { "deeppink", Color { 255, 20, 147, 255 } },
    { "deepskyblue", Color { 0, 191, 255, 255 } },
    { "floralwhite", Color { 255, 250, 240, 255 } },
    { "greenyellow", Color { 173, 255, 47, 255 } },
    { "honeydew", Color { 240, 255, 240, 255 } },
    { "hotpink", Color { 255, 105, 180, 255 } },
    { "indianred", Color { 205, 92, 92, 255 } },
    { "lavenderblush", Color { 255, 240, 245, 255 } },
    { "lawngreen", Color { 124, 252, 0, 255 } },
    { "lemonchiffon", Color { 255, 250, 205, 255 } },
    { "lightcoral", Color { 240, 128, 128, 255 } },
    { "lightcyan", Color { 224, 255, 255, 255 } },
    { "lightgoldenrodyellow", Color { 250, 250, 210, 255 } },
    { "lightpink", Color { 255, 182, 193, 255 } },
    { "lightsalmon", Color { 255, 160, 122, 255 } },
    { "lightseagreen", Color { 32, 178, 170, 255 } },
    { "lightskyblue", Color { 135, 206, 250, 255 } },
    { "lightslategray", Color { 119, 136, 153, 255 } },
    { "lightslategrey", Color { 119, 136, 153, 255 } },
    { "lightsteelblue", Color { 176, 196, 222, 255 } },
    { "limegreen", Color { 50, 205, 50, 255 } },
    { "linen", Color { 250, 240, 230, 255 } },
    { "mediumaquamarine", Color { 102, 205, 170, 255 } },
    { "mediumblue", Color { 0, 0, 205, 255 } },
    { "mediumorchid", Color { 186, 85, 211, 255 } },
    { "mediumpurple", Color { 147, 112, 219, 255 } },
    { "mediumseagreen", Color { 60, 179, 113, 255 } },
    { "mediumslateblue", Color { 123, 104, 238, 255 } },
    { "mediumspringgreen", Color { 0, 250, 154, 255 } },
    { "mediumturquoise", Color { 72, 209, 204, 255 } },
    { "mediumvioletred", Color { 199, 21, 133, 255 } },
    { "mintcream", Color { 245, 255, 250, 255 } },
    { "mistyrose", Color { 255, 228, 225, 255 } },
    { "moccasin", Color { 255, 228, 181, 255 } },
    { "navajowhite", Color { 255, 222, 173, 255 } },
    { "oldlace", Color { 253, 245, 230, 255 } },
    { "olivedrab", Color { 107, 142, 35, 255 } },
    { "orangered", Color { 255, 69, 0, 255 } },
    { "palegoldenrod", Color { 238, 232, 170, 255 } },
    { "palegreen", Color { 152, 251, 152, 255 } },
    { "paleturquoise", Color { 175, 238, 238, 255 } },
    { "palevioletred", Color { 219, 112, 147, 255 } },
    { "papayawhip", Color { 255, 239, 213, 255 } },
    { "peachpuff", Color { 255, 218, 185, 255 } },
    { "peru", Color { 205, 133, 63, 255 } },
    { "powderblue", Color { 176, 224, 230, 255 } },
    { "rosybrown", Color { 188, 143, 143, 255 } },
    { "saddlebrown", Color { 139, 69, 19, 255 } },
    { "sandybrown", Color { 244, 164, 96, 255 } },
    { "seashell", Color { 255, 245, 238, 255 } },
    { "sienna", Color { 160, 82, 45, 255 } },
    { "slateblue", Color { 106, 90, 205, 255 } },
    { "springgreen", Color { 0, 255, 127, 255 } },
    { "thistle", Color { 216, 191, 216, 255 } },
    { "yellowgreen", Color { 154, 205, 50, 255 } },
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
        if (ascii_ci_equals(function.name, "hsl") || ascii_ci_equals(function.name, "hsla")) {
            // hsl(H S L / A) or the legacy hsl(H, S%, L%, A): the hue an
            // angle (a bare number is degrees), saturation and lightness
            // percentages (bare numbers read as such), then CSS Color 4's
            // conversion.
            double parts[4] = { 0, 0, 0, 1 };
            int part = 0;
            bool alpha_seen = false;
            for (ComponentValue const& argument : function.values) {
                if (argument.is_token(Token::Type::Whitespace) || argument.is_token(Token::Type::Comma))
                    continue;
                if (argument.is_token(Token::Type::Delim) && argument.token().delim == U'/')
                    continue;
                if (part >= 4 || !argument.is_token())
                    return std::nullopt;
                Token const& token = argument.token();
                double parsed = 0;
                if (part == 0) {
                    if (token.type == Token::Type::Number) {
                        parsed = token.numeric_value;
                    } else if (token.type == Token::Type::Dimension) {
                        if (ascii_ci_equals(token.unit, "deg"))
                            parsed = token.numeric_value;
                        else if (ascii_ci_equals(token.unit, "grad"))
                            parsed = token.numeric_value * 360.0 / 400.0;
                        else if (ascii_ci_equals(token.unit, "rad"))
                            parsed = token.numeric_value * 180.0 / 3.14159265358979323846;
                        else if (ascii_ci_equals(token.unit, "turn"))
                            parsed = token.numeric_value * 360.0;
                        else
                            return std::nullopt;
                    } else {
                        return std::nullopt;
                    }
                } else if (part == 3) {
                    alpha_seen = true;
                    if (token.type == Token::Type::Number)
                        parsed = token.numeric_value;
                    else if (token.type == Token::Type::Percentage)
                        parsed = token.numeric_value / 100.0;
                    else
                        return std::nullopt;
                } else {
                    if (token.type == Token::Type::Percentage || token.type == Token::Type::Number)
                        parsed = token.numeric_value;
                    else
                        return std::nullopt;
                }
                parts[part++] = parsed;
            }
            if (part < 3)
                return std::nullopt;
            double hue = parts[0];
            hue = hue - 360.0 * static_cast<double>(static_cast<long long>(hue / 360.0));
            if (hue < 0)
                hue += 360.0;
            double const saturation = std::clamp(parts[1], 0.0, 100.0) / 100.0;
            double const lightness = std::clamp(parts[2], 0.0, 100.0) / 100.0;
            auto const channel = [&](double n) {
                double k = n + hue / 30.0;
                k = k - 12.0 * static_cast<double>(static_cast<long long>(k / 12.0));
                double const a = saturation * std::min(lightness, 1.0 - lightness);
                double const m = std::max(-1.0, std::min({ k - 3.0, 9.0 - k, 1.0 }));
                return (lightness - a * m) * 255.0;
            };
            return Color { clamp_channel(channel(0)), clamp_channel(channel(8)), clamp_channel(channel(4)),
                alpha_seen ? clamp_channel(parts[3] * 255.0) : std::uint8_t { 255 } };
        }
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
    float viewport_width = 1024;
    float viewport_height = 768;
    // One `ex` and one `ch` in px: the x-height of the first available face,
    // and the advance of its "0", at this font size. Half the font size when
    // the face does not say — the fallback the specification names.
    float ex_size = 8;
    float ch_size = 8;
};

// How much of a font size one `ex` and one `ch` are, for a face: ratios, not
// pixels, because they depend on the face and not on the size it is used at.
struct FontRatios {
    float ex = 0.5f;
    float ch = 0.5f;
};

std::optional<LengthPercent> parse_length_percent(ComponentValue const& value,
    LengthContext const& context, bool allow_auto, bool allow_percent = true);

std::string lowercase_name(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        out += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

// calc(), min(), max() and clamp() over lengths and percentages: a sum of
// products of lengths, percentages and numbers, with parentheses and the
// four functions nested. A value is pixels plus a percentage, or a bare
// number (a factor). Comparisons need all arms in one currency — pixels
// alone or percentages alone.
struct CalcValue {
    float px = 0;
    float percent = 0;
    bool number = false; // a unitless number: only a factor
    float scalar = 0;
};

class CalcEvaluator {
public:
    CalcEvaluator(std::vector<ComponentValue const*> items, LengthContext const& context, bool allow_percent)
        : m_items(std::move(items))
        , m_context(context)
        , m_allow_percent(allow_percent)
    {
    }

    std::optional<CalcValue> whole()
    {
        std::optional<CalcValue> const result = sum();
        if (!result || m_at != m_items.size())
            return std::nullopt;
        return result;
    }

private:
    bool is_delim(char32_t which) const
    {
        return m_at < m_items.size() && m_items[m_at]->is_token(Token::Type::Delim)
            && m_items[m_at]->token().delim == which;
    }

    std::optional<CalcValue> sum()
    {
        std::optional<CalcValue> left = product();
        if (!left)
            return std::nullopt;
        while (is_delim(U'+') || is_delim(U'-')) {
            bool const minus = is_delim(U'-');
            ++m_at;
            std::optional<CalcValue> const right = product();
            if (!right || left->number != right->number)
                return std::nullopt;
            float const sign = minus ? -1.0f : 1.0f;
            left->px += sign * right->px;
            left->percent += sign * right->percent;
            left->scalar += sign * right->scalar;
        }
        return left;
    }

    std::optional<CalcValue> product()
    {
        std::optional<CalcValue> left = unit();
        if (!left)
            return std::nullopt;
        while (is_delim(U'*') || is_delim(U'/')) {
            bool const divide = is_delim(U'/');
            ++m_at;
            std::optional<CalcValue> const right = unit();
            if (!right)
                return std::nullopt;
            if (divide) {
                if (!right->number || right->scalar == 0)
                    return std::nullopt;
                left->px /= right->scalar;
                left->percent /= right->scalar;
                left->scalar /= right->scalar;
            } else if (right->number) {
                left->px *= right->scalar;
                left->percent *= right->scalar;
                left->scalar *= right->scalar;
            } else if (left->number) {
                float const factor = left->scalar;
                left = right;
                left->px *= factor;
                left->percent *= factor;
            } else {
                return std::nullopt; // a length times a length
            }
        }
        return left;
    }

    std::optional<CalcValue> unit()
    {
        if (m_at >= m_items.size())
            return std::nullopt;
        ComponentValue const& value = *m_items[m_at];
        if (value.is_block() && value.block().open == Token::Type::OpenParen) {
            ++m_at;
            CalcEvaluator inner(significant(value.block().values), m_context, m_allow_percent);
            return inner.whole();
        }
        if (value.is_function()) {
            ++m_at;
            return function(value.function());
        }
        if (value.is_token(Token::Type::Number)) {
            ++m_at;
            CalcValue number;
            number.number = true;
            number.scalar = static_cast<float>(value.token().numeric_value);
            return number;
        }
        std::optional<LengthPercent> const length = parse_length_percent(value, m_context, false, m_allow_percent);
        if (!length)
            return std::nullopt;
        ++m_at;
        CalcValue result;
        if (length->kind == LengthPercent::Kind::Percent)
            result.percent = length->value;
        else if (length->kind == LengthPercent::Kind::Calc) {
            result.px = length->value;
            result.percent = length->percent;
        } else
            result.px = length->value;
        return result;
    }

    std::optional<CalcValue> function(FunctionValue const& call)
    {
        std::string const name = lowercase_name(call.name);
        std::vector<std::vector<ComponentValue const*>> arguments(1);
        for (ComponentValue const* item : significant(call.values)) {
            if (item->is_token(Token::Type::Comma))
                arguments.emplace_back();
            else
                arguments.back().push_back(item);
        }
        std::vector<CalcValue> values;
        for (std::vector<ComponentValue const*> const& argument : arguments) {
            CalcEvaluator inner(argument, m_context, m_allow_percent);
            std::optional<CalcValue> const value = inner.whole();
            if (!value)
                return std::nullopt;
            values.push_back(*value);
        }
        if (name == "calc")
            return values.size() == 1 ? std::optional<CalcValue>(values[0]) : std::nullopt;
        if (name != "min" && name != "max" && name != "clamp")
            return std::nullopt;
        if (name == "clamp" ? values.size() != 3 : values.empty())
            return std::nullopt;
        // Comparable only in one currency.
        bool const pixels = std::all_of(values.begin(), values.end(),
            [](CalcValue const& v) { return !v.number && v.percent == 0; });
        bool const percents = std::all_of(values.begin(), values.end(),
            [](CalcValue const& v) { return !v.number && v.px == 0; });
        if (!pixels && !percents)
            return std::nullopt;
        auto const measure = [&](CalcValue const& v) { return pixels ? v.px : v.percent; };
        CalcValue result = values[0];
        if (name == "min") {
            for (CalcValue const& v : values)
                if (measure(v) < measure(result))
                    result = v;
        } else if (name == "max") {
            for (CalcValue const& v : values)
                if (measure(v) > measure(result))
                    result = v;
        } else {
            result = values[1];
            if (measure(result) < measure(values[0]))
                result = values[0];
            if (measure(result) > measure(values[2]))
                result = values[2];
        }
        return result;
    }

    std::vector<ComponentValue const*> m_items;
    std::size_t m_at = 0;
    LengthContext const& m_context;
    bool m_allow_percent;
};

std::optional<LengthPercent> parse_length_percent(ComponentValue const& value,
    LengthContext const& context, bool allow_auto, bool allow_percent)
{
    if (value.is_function()) {
        std::string const name = lowercase_name(value.function().name);
        if (name != "calc" && name != "min" && name != "max" && name != "clamp")
            return std::nullopt;
        CalcEvaluator evaluator({ &value }, context, allow_percent);
        std::optional<CalcValue> const result = evaluator.whole();
        if (!result || result->number)
            return std::nullopt;
        return LengthPercent::calc(result->px, result->percent);
    }
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
        return LengthPercent::percent_of(static_cast<float>(token.numeric_value));
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
    // The absolute units, anchored to 96 px per inch.
    if (ascii_ci_equals(unit, "in"))
        return LengthPercent::px(static_cast<float>(number * 96.0));
    if (ascii_ci_equals(unit, "cm"))
        return LengthPercent::px(static_cast<float>(number * 96.0 / 2.54));
    if (ascii_ci_equals(unit, "mm"))
        return LengthPercent::px(static_cast<float>(number * 96.0 / 25.4));
    if (ascii_ci_equals(unit, "q"))
        return LengthPercent::px(static_cast<float>(number * 96.0 / 101.6));
    if (ascii_ci_equals(unit, "pc"))
        return LengthPercent::px(static_cast<float>(number * 16.0));
    // The font-relative pair: `ex` is the face's x-height, `ch` the advance
    // of its "0".
    if (ascii_ci_equals(unit, "ex"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.ex_size)));
    if (ascii_ci_equals(unit, "ch"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.ch_size)));
    // The viewport units, against the viewport this resolution is for.
    if (ascii_ci_equals(unit, "vw"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.viewport_width) / 100.0));
    if (ascii_ci_equals(unit, "vh"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.viewport_height) / 100.0));
    if (ascii_ci_equals(unit, "vmin"))
        return LengthPercent::px(static_cast<float>(
            number * static_cast<double>(std::min(context.viewport_width, context.viewport_height)) / 100.0));
    if (ascii_ci_equals(unit, "vmax"))
        return LengthPercent::px(static_cast<float>(
            number * static_cast<double>(std::max(context.viewport_width, context.viewport_height)) / 100.0));
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
    if (length && length->kind == LengthPercent::Kind::Px && length->value >= 0)
        return length->value; // a negative width is no width at all: the declaration is ignored
    return std::nullopt;
}

std::optional<BorderStyle> parse_border_style(ComponentValue const& value)
{
    if (!value.is_token(Token::Type::Ident))
        return std::nullopt;
    std::string_view const name = value.token().value;
    if (ascii_ci_equals(name, "none"))
        return BorderStyle::None;
    if (ascii_ci_equals(name, "hidden"))
        return BorderStyle::Hidden;
    for (std::string_view solid_ish :
        { "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset" }) {
        if (ascii_ci_equals(name, solid_ish))
            return BorderStyle::Solid;
    }
    return std::nullopt;
}

// --- Flow-relative properties -------------------------------------------------

// The physical properties a flow-relative one stands for (css-logical-1).
// `split` shares a shorthand's two values out, one to each edge; a
// shorthand that does not split gives all of them to both, which is what
// `border-inline: 1px solid` means.
struct LogicalMap {
    std::vector<std::string> names;
    bool split = true;
};

// Which physical edge a flow-relative name stands for: a question about
// the writing mode and the direction together. A horizontal mode runs its
// inline axis across the page and stacks lines down it; a vertical one
// runs the inline axis down and stacks lines sideways, from the right in
// `vertical-rl` and from the left in `vertical-lr`. `sideways-lr` is the
// one mode whose text reads upwards, so its start edge is the bottom.
std::optional<LogicalMap> logical_mapping(std::string_view name, bool rtl, WritingMode mode)
{
    bool const vertical = is_vertical(mode);
    bool const up = inline_runs_up(mode) != rtl;
    std::string const inline_start
        = vertical ? (up ? "bottom" : "top") : (rtl ? "right" : "left");
    std::string const inline_end
        = vertical ? (up ? "top" : "bottom") : (rtl ? "left" : "right");
    std::string const block_start
        = vertical ? (blocks_run_left(mode) ? "right" : "left") : "top";
    std::string const block_end
        = vertical ? (blocks_run_left(mode) ? "left" : "right") : "bottom";
    // margin-inline-start is margin-left where the content starts left.
    for (std::string_view const group : { "margin-", "padding-" }) {
        if (!name.starts_with(group))
            continue;
        std::string const prefix(group);
        std::string_view const rest = name.substr(group.size());
        if (rest == "inline-start")
            return LogicalMap { { prefix + inline_start } };
        if (rest == "inline-end")
            return LogicalMap { { prefix + inline_end } };
        if (rest == "block-start")
            return LogicalMap { { prefix + block_start } };
        if (rest == "block-end")
            return LogicalMap { { prefix + block_end } };
        if (rest == "inline")
            return LogicalMap { { prefix + inline_start, prefix + inline_end } };
        if (rest == "block")
            return LogicalMap { { prefix + block_start, prefix + block_end } };
        return std::nullopt;
    }
    // The offsets a positioned box is placed by.
    if (name == "inset-inline-start")
        return LogicalMap { { inline_start } };
    if (name == "inset-inline-end")
        return LogicalMap { { inline_end } };
    if (name == "inset-block-start")
        return LogicalMap { { block_start } };
    if (name == "inset-block-end")
        return LogicalMap { { block_end } };
    if (name == "inset-inline")
        return LogicalMap { { inline_start, inline_end } };
    if (name == "inset-block")
        return LogicalMap { { block_start, block_end } };
    // border-inline-start[-width|-style|-color] and the axis shorthands:
    // border-inline-width takes one value per edge, border-inline one
    // whole border for both.
    if (name.starts_with("border-inline") || name.starts_with("border-block")) {
        bool const inline_axis = name.starts_with("border-inline");
        std::string_view rest = name.substr(inline_axis ? 13 : 12);
        std::string const start = "border-" + (inline_axis ? inline_start : block_start);
        std::string const end = "border-" + (inline_axis ? inline_end : block_end);
        if (rest.empty())
            return LogicalMap { { start, end }, false };
        if (rest == "-width" || rest == "-style" || rest == "-color")
            return LogicalMap { { start + std::string(rest), end + std::string(rest) } };
        bool const at_start = rest.starts_with("-start");
        if (!at_start && !rest.starts_with("-end"))
            return std::nullopt;
        std::string const edge = at_start ? start : end;
        rest = rest.substr(at_start ? 6 : 4);
        if (rest.empty())
            return LogicalMap { { edge } };
        if (rest == "-width" || rest == "-style" || rest == "-color")
            return LogicalMap { { edge + std::string(rest) } };
        return std::nullopt;
    }
    // The sizes: the inline size of a horizontal writing mode is its
    // width, of a vertical one its height.
    std::string const across = is_vertical(mode) ? "height" : "width";
    std::string const along = is_vertical(mode) ? "width" : "height";
    if (name == "inline-size")
        return LogicalMap { { across } };
    if (name == "block-size")
        return LogicalMap { { along } };
    if (name == "min-inline-size")
        return LogicalMap { { "min-" + across } };
    if (name == "min-block-size")
        return LogicalMap { { "min-" + along } };
    if (name == "max-inline-size")
        return LogicalMap { { "max-" + across } };
    if (name == "max-block-size")
        return LogicalMap { { "max-" + along } };
    return std::nullopt;
}

// --- The resolver -------------------------------------------------------------

} // namespace

// The compiled side of a StyleSet: rules in cascade order and the index
// over them, built once per media context.
// --- Backgrounds ------------------------------------------------------------

using Values = std::vector<ComponentValue const*>;

// A declaration's values split at its top-level commas: one group per layer.
std::vector<Values> split_commas(Values const& values)
{
    std::vector<Values> groups(1);
    for (ComponentValue const* value : values) {
        if (value->is_token(Token::Type::Comma)) {
            groups.emplace_back();
            continue;
        }
        groups.back().push_back(value);
    }
    return groups;
}

bool all_none(std::vector<BackgroundImage> const& images)
{
    for (BackgroundImage const& image : images) {
        if (!image.none())
            return false;
    }
    return true;
}

// url(x) as a token, or url("x") as a function: the text as written.
std::optional<std::string> url_of(ComponentValue const& value)
{
    if (value.is_token(Token::Type::Url))
        return value.token().value;
    if (value.is_function() && ascii_ci_equals(value.function().name, "url")) {
        for (ComponentValue const& inner : value.function().values) {
            if (inner.is_token(Token::Type::Whitespace))
                continue;
            if (inner.is_token(Token::Type::String))
                return inner.token().value;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// An <angle> in degrees: deg, grad, rad or turn (a bare 0 counts).
std::optional<float> parse_angle(ComponentValue const& value)
{
    if (value.is_token(Token::Type::Number))
        return value.token().numeric_value == 0 ? std::optional<float>(0.0f) : std::nullopt;
    if (!value.is_token(Token::Type::Dimension))
        return std::nullopt;
    auto const number = static_cast<float>(value.token().numeric_value);
    std::string_view const unit = value.token().unit;
    if (ascii_ci_equals(unit, "deg"))
        return number;
    if (ascii_ci_equals(unit, "grad"))
        return number * 0.9f;
    if (ascii_ci_equals(unit, "rad"))
        return number * 180.0f / 3.14159265f;
    if (ascii_ci_equals(unit, "turn"))
        return number * 360.0f;
    return std::nullopt;
}

// A position keyword or length along one axis of a background position.
std::optional<LengthPercent> position_component(ComponentValue const& value, LengthContext const& context,
    bool horizontal, bool& keyword)
{
    keyword = false;
    if (value.is_token(Token::Type::Ident)) {
        std::string_view const word = value.token().value;
        keyword = true;
        if (ascii_ci_equals(word, "center"))
            return LengthPercent::percent_of(50);
        if (horizontal && ascii_ci_equals(word, "left"))
            return LengthPercent::percent_of(0);
        if (horizontal && ascii_ci_equals(word, "right"))
            return LengthPercent::percent_of(100);
        if (!horizontal && ascii_ci_equals(word, "top"))
            return LengthPercent::percent_of(0);
        if (!horizontal && ascii_ci_equals(word, "bottom"))
            return LengthPercent::percent_of(100);
        keyword = false;
        return std::nullopt;
    }
    return parse_length_percent(value, context, false);
}

// <bg-position> from `from`: one or two values (keywords in either order
// when both are keywords). `taken` is how many values it used.
std::optional<BackgroundPosition> parse_background_position(Values const& values, std::size_t from,
    LengthContext const& context, std::size_t& taken)
{
    taken = 0;
    if (from >= values.size())
        return std::nullopt;
    bool key_a = false;
    bool key_b = false;
    std::optional<LengthPercent> const a_x = position_component(*values[from], context, true, key_a);
    std::optional<LengthPercent> const a_y = position_component(*values[from], context, false, key_a);
    if (!a_x && !a_y)
        return std::nullopt;
    BackgroundPosition position;
    if (from + 1 < values.size()) {
        ComponentValue const& second = *values[from + 1];
        std::optional<LengthPercent> const b_y = position_component(second, context, false, key_b);
        std::optional<LengthPercent> const b_x = position_component(second, context, true, key_b);
        if (a_x && b_y) {
            position.x = *a_x;
            position.y = *b_y;
            taken = 2;
            return position;
        }
        if (key_a && key_b && a_y && b_x) {
            position.x = *b_x;
            position.y = *a_y;
            taken = 2;
            return position;
        }
    }
    // One value: the other axis is centered.
    taken = 1;
    if (a_x) {
        position.x = *a_x;
        position.y = LengthPercent::percent_of(50);
    } else {
        position.x = LengthPercent::percent_of(50);
        position.y = *a_y;
    }
    return position;
}

// <bg-size> from `from`: auto | cover | contain | [<length-percentage> | auto]{1,2}.
std::optional<BackgroundSize> parse_background_size(Values const& values, std::size_t from,
    LengthContext const& context, std::size_t& taken)
{
    taken = 0;
    if (from >= values.size())
        return std::nullopt;
    BackgroundSize size;
    ComponentValue const& first = *values[from];
    if (is_ident(&first, "cover") || is_ident(&first, "contain")) {
        size.kind = is_ident(&first, "cover") ? BackgroundSize::Kind::Cover : BackgroundSize::Kind::Contain;
        taken = 1;
        return size;
    }
    std::optional<LengthPercent> const width = parse_length_percent(first, context, true);
    if (!width || (width->kind == LengthPercent::Kind::Px && width->value < 0))
        return std::nullopt;
    size.width = *width;
    taken = 1;
    if (from + 1 < values.size()) {
        if (std::optional<LengthPercent> const height = parse_length_percent(*values[from + 1], context, true);
            height && !(height->kind == LengthPercent::Kind::Px && height->value < 0)) {
            size.height = *height;
            taken = 2;
        }
    }
    size.kind = size.width.is_auto() && size.height.is_auto() ? BackgroundSize::Kind::Auto
                                                                 : BackgroundSize::Kind::Lengths;
    return size;
}

// <repeat-style> from `from`: repeat-x | repeat-y | [repeat | space | round | no-repeat]{1,2}.
std::optional<BackgroundRepeatPair> parse_background_repeat(Values const& values, std::size_t from,
    std::size_t& taken)
{
    taken = 0;
    if (from >= values.size())
        return std::nullopt;
    auto const one = [](ComponentValue const& value) -> std::optional<BackgroundRepeat> {
        if (is_ident(&value, "repeat"))
            return BackgroundRepeat::Repeat;
        if (is_ident(&value, "space"))
            return BackgroundRepeat::Space;
        if (is_ident(&value, "round"))
            return BackgroundRepeat::Round;
        if (is_ident(&value, "no-repeat"))
            return BackgroundRepeat::NoRepeat;
        return std::nullopt;
    };
    BackgroundRepeatPair pair;
    if (is_ident(values[from], "repeat-x")) {
        pair.y = BackgroundRepeat::NoRepeat;
        taken = 1;
        return pair;
    }
    if (is_ident(values[from], "repeat-y")) {
        pair.x = BackgroundRepeat::NoRepeat;
        taken = 1;
        return pair;
    }
    std::optional<BackgroundRepeat> const x = one(*values[from]);
    if (!x)
        return std::nullopt;
    pair.x = *x;
    pair.y = *x;
    taken = 1;
    if (from + 1 < values.size()) {
        if (std::optional<BackgroundRepeat> const y = one(*values[from + 1])) {
            pair.y = *y;
            taken = 2;
        }
    }
    return pair;
}

std::optional<BackgroundBox> parse_background_box(ComponentValue const& value)
{
    if (is_ident(&value, "border-box"))
        return BackgroundBox::BorderBox;
    if (is_ident(&value, "padding-box"))
        return BackgroundBox::PaddingBox;
    if (is_ident(&value, "content-box"))
        return BackgroundBox::ContentBox;
    return std::nullopt;
}

// linear-gradient() and radial-gradient(), repeating or not: the direction
// or shape and center, then the color stops (a color with up to two
// positions; a bare length between stops, a hint, is passed over).
std::optional<Gradient> parse_gradient(ComponentValue const& value, LengthContext const& context, Color current)
{
    if (!value.is_function())
        return std::nullopt;
    std::string const name = lowercase_name(value.function().name);
    Gradient gradient;
    if (name == "linear-gradient" || name == "repeating-linear-gradient")
        gradient.kind = Gradient::Kind::Linear;
    else if (name == "radial-gradient" || name == "repeating-radial-gradient")
        gradient.kind = Gradient::Kind::Radial;
    else
        return std::nullopt;
    gradient.repeating = name.starts_with("repeating");
    Values const all = significant(value.function().values);
    std::vector<Values> const args = split_commas(all);
    if (args.empty())
        return std::nullopt;
    std::size_t first_stop = 0;
    Values const& head = args[0];
    if (gradient.kind == Gradient::Kind::Linear && !head.empty()) {
        if (std::optional<float> const angle = parse_angle(*head[0]); angle && head.size() == 1) {
            gradient.angle = *angle;
            first_stop = 1;
        } else if (is_ident(head[0], "to")) {
            bool top = false;
            bool bottom = false;
            bool left = false;
            bool right = false;
            for (std::size_t i = 1; i < head.size(); ++i) {
                if (is_ident(head[i], "top"))
                    top = true;
                else if (is_ident(head[i], "bottom"))
                    bottom = true;
                else if (is_ident(head[i], "left"))
                    left = true;
                else if (is_ident(head[i], "right"))
                    right = true;
                else
                    return std::nullopt;
            }
            if (head.size() < 2 || head.size() > 3 || (left && right) || (top && bottom))
                return std::nullopt;
            if ((top || bottom) && (left || right)) {
                gradient.corner = top ? (left ? Gradient::Corner::TopLeft : Gradient::Corner::TopRight)
                                      : (left ? Gradient::Corner::BottomLeft : Gradient::Corner::BottomRight);
            } else {
                gradient.angle = top ? 0.0f : bottom ? 180.0f : left ? 270.0f : 90.0f;
            }
            first_stop = 1;
        }
    } else if (gradient.kind == Gradient::Kind::Radial && !head.empty()) {
        bool consumed = false;
        std::size_t i = 0;
        while (i < head.size()) {
            ComponentValue const& item = *head[i];
            if (is_ident(&item, "circle")) {
                gradient.shape = Gradient::Shape::Circle;
            } else if (is_ident(&item, "ellipse")) {
                gradient.shape = Gradient::Shape::Ellipse;
            } else if (is_ident(&item, "closest-side")) {
                gradient.extent = Gradient::Extent::ClosestSide;
            } else if (is_ident(&item, "closest-corner")) {
                gradient.extent = Gradient::Extent::ClosestCorner;
            } else if (is_ident(&item, "farthest-side")) {
                gradient.extent = Gradient::Extent::FarthestSide;
            } else if (is_ident(&item, "farthest-corner")) {
                gradient.extent = Gradient::Extent::FarthestCorner;
            } else if (is_ident(&item, "at")) {
                std::size_t taken = 0;
                std::optional<BackgroundPosition> const center
                    = parse_background_position(head, i + 1, context, taken);
                if (!center)
                    return std::nullopt;
                gradient.center_x = center->x;
                gradient.center_y = center->y;
                i += 1 + taken;
                consumed = true;
                continue;
            } else if (parse_length_percent(item, context, false)) {
                // An explicit size: drawn at the extent for now.
            } else {
                break;
            }
            consumed = true;
            ++i;
        }
        if (consumed) {
            if (i != head.size())
                return std::nullopt;
            first_stop = 1;
        }
    }
    for (std::size_t a = first_stop; a < args.size(); ++a) {
        Values const& arg = args[a];
        if (arg.empty() || arg.size() > 3)
            return std::nullopt;
        std::optional<Color> const color = parse_color_component(*arg[0], current);
        if (!color) {
            if (arg.size() == 1 && parse_length_percent(*arg[0], context, false))
                continue; // a hint
            return std::nullopt;
        }
        GradientStop stop;
        stop.color = *color;
        if (arg.size() >= 2) {
            std::optional<LengthPercent> const position = parse_length_percent(*arg[1], context, false);
            if (!position)
                return std::nullopt;
            stop.position = position;
        }
        gradient.stops.push_back(stop);
        if (arg.size() == 3) {
            std::optional<LengthPercent> const position = parse_length_percent(*arg[2], context, false);
            if (!position)
                return std::nullopt;
            GradientStop second = stop;
            second.position = position;
            gradient.stops.push_back(second);
        }
    }
    if (gradient.stops.size() < 2)
        return std::nullopt;
    return gradient;
}

// <bg-image>: none, a url() resolved against `base`, or a gradient.
std::optional<BackgroundImage> parse_background_image(ComponentValue const& value, LengthContext const& context,
    Color current, net::Url const* base)
{
    if (is_ident(&value, "none"))
        return BackgroundImage {};
    if (std::optional<std::string> const url = url_of(value)) {
        BackgroundImage image;
        std::optional<net::Url> const resolved = net::parse_url(*url, base);
        image.url = resolved ? resolved->serialize(true) : *url;
        if (image.url.empty())
            return std::nullopt;
        return image;
    }
    if (std::optional<Gradient> gradient = parse_gradient(value, context, current)) {
        BackgroundImage image;
        image.gradient = std::make_shared<Gradient const>(std::move(*gradient));
        return image;
    }
    return std::nullopt;
}

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
    std::optional<net::Url> document_url; // the base for style attributes' URLs

    void compile_sheet(std::string_view text, bool user_agent, int& order,
        std::shared_ptr<net::Url const> const& base)
    {
        Stylesheet sheet = parse_stylesheet(text);
        compile_rules(sheet.rules, user_agent, order, base);
    }

    void compile_rules(std::vector<Rule>& source, bool user_agent, int& order,
        std::shared_ptr<net::Url const> const& base)
    {
        for (Rule& rule : source) {
            if (rule.is_at_rule()) {
                // @media blocks whose query the context satisfies contribute
                // their rules in place; other at-rules (@supports,
                // @font-face, @keyframes, @layer) are not supported yet.
                auto& at = std::get<AtRule>(rule.value);
                if (at.has_block && ascii_ci_equals(at.name, "media")
                    && media_prelude_matches(at.prelude, media))
                    compile_rules(at.child_rules, user_agent, order, base);
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
            compiled.base = base;
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
    // Rules are matched for four targets at once — the element itself, its
    // ::before, its ::after and its ::first-letter — since one selector walk
    // serves all four. Per target and rule: the element (stamp) it last
    // matched and its best matching selector for that element.
    static constexpr int target_count = 4;
    std::array<std::vector<int>, target_count> rule_stamp;
    std::array<std::vector<Specificity>, target_count> rule_best;
    int stamp = 0;
    std::array<std::vector<std::uint32_t>, target_count> matched_rules; // scratch, reused per element
    AncestorFilter ancestors; // the identifiers of the elements above the one being styled
    int quote_depth = 0; // the nesting of quotation marks so far, in tree order

    // One counter instance: a value, and the depth of the box that made it.
    struct CounterInstance {
        std::string name;
        int value = 0;
        int depth = 0;
    };
    // The instances in scope, innermost last. CSS 2.1 §12.4.3: the instance
    // a counter-reset makes covers that box, its descendants, and its
    // following siblings with their descendants — so it lives until the walk
    // leaves the box's parent, which is what the mark taken around each
    // children loop does.
    std::vector<CounterInstance> counter_stack;
    int counter_depth = 0; // the depth of the box whose turn it is
    int display_none_depth = 0; // >0 inside a subtree that generates no boxes

    // A reset by a later sibling shadows an earlier one's instance of the
    // same name — §12.4.3 says the earlier counter's scope stops there — and
    // so does a second reset of the same name on one box. Both are an
    // instance at this very depth, and there can be at most one, since a
    // deeper one was dropped when its own parent's children were done.
    void reset_counter(std::string const& name, int value)
    {
        for (std::size_t i = counter_stack.size(); i-- > 0;) {
            if (counter_stack[i].depth < counter_depth)
                break;
            if (counter_stack[i].name == name) {
                counter_stack.erase(counter_stack.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        counter_stack.push_back(CounterInstance { name, value, counter_depth });
    }

    CounterInstance const* innermost_counter(std::string const& name) const
    {
        for (std::size_t i = counter_stack.size(); i-- > 0;) {
            if (counter_stack[i].name == name)
                return &counter_stack[i];
        }
        return nullptr;
    }

    // §12.4.3 again: a counter that no counter-reset put in scope behaves as
    // though one had reset it to zero on the box that asks for it.
    CounterInstance& counter_in_scope(std::string const& name)
    {
        for (std::size_t i = counter_stack.size(); i-- > 0;) {
            if (counter_stack[i].name == name)
                return counter_stack[i];
        }
        counter_stack.push_back(CounterInstance { name, 0, counter_depth });
        return counter_stack.back();
    }

    // The suite writes the largest and smallest integers there are, so the
    // step saturates rather than wrapping (which would be undefined).
    static int add_saturating(int value, int step)
    {
        long long const sum = static_cast<long long>(value) + step;
        if (sum > std::numeric_limits<int>::max())
            return std::numeric_limits<int>::max();
        if (sum < std::numeric_limits<int>::min())
            return std::numeric_limits<int>::min();
        return static_cast<int>(sum);
    }

    // A box's counter work, in the order the properties are applied: reset,
    // then increment, then set. A box that is not generated — display: none,
    // and everything inside it — does none of it.
    void apply_counter_ops(ComputedStyle& style)
    {
        if (display_none_depth > 0)
            return;
        if (style.counter_reset) {
            for (CounterOp const& op : *style.counter_reset)
                reset_counter(op.name, op.value);
        }
        if (style.counter_increment) {
            for (CounterOp const& op : *style.counter_increment) {
                CounterInstance& instance = counter_in_scope(op.name);
                instance.value = add_saturating(instance.value, op.value);
            }
        }
        // css-lists-3 §4.2: a list item adds one to `list-item` of its own
        // accord, unless its counter-increment named that counter itself.
        if (style.display == Display::ListItem && !names_list_item(style.counter_increment.get())) {
            CounterInstance& instance = counter_in_scope("list-item");
            instance.value = add_saturating(instance.value, 1);
        }
        if (style.counter_set) {
            for (CounterOp const& op : *style.counter_set)
                counter_in_scope(op.name).value = op.value;
        }
        // The marker's number, read once the box's own work is done, so an
        // author's reset, increment or set on the item itself all count.
        if (style.display == Display::ListItem) {
            CounterInstance const* const instance = innermost_counter("list-item");
            style.list_item_value = instance ? instance->value : 0;
        }
    }

    static bool names_list_item(CounterOps const* ops)
    {
        if (!ops)
            return false;
        for (CounterOp const& op : *ops) {
            if (op.name == "list-item")
                return true;
        }
        return false;
    }
    // What one `ex` and one `ch` come to for the element being cascaded and
    // for its parent: settled as soon as the font is, and read by every
    // length in those units afterwards.
    FontRatios own_ratios;
    FontRatios parent_ratios;

    // The ratios for the face a style names — its x-height and the advance
    // of its "0", as fractions of the font size, since neither depends on
    // the size. A face that does not say its x-height, and one with no "0",
    // keep the halves the specification falls back to.
    static FontRatios ratios_for(ComputedStyle const& style)
    {
        text::FontRequest request;
        if (style.font_family)
            request.families = *style.font_family;
        request.weight = style.font_weight;
        request.italic = style.font_style == FontStyle::Italic;
        text::Face const& face = text::FontManager::instance().resolve(request).primary();
        // Measured at a size large enough that the division keeps its digits.
        constexpr float probe = 1024;
        FontRatios ratios;
        if (float const x = face.metrics(probe).x_height; x > 0)
            ratios.ex = x / probe;
        if (float const zero = face.advance(face.glyph_index(U'0'), probe); zero > 0)
            ratios.ch = zero / probe;
        return ratios;
    }

    // A length context: this style's font size, with one `ex` and one `ch`
    // of the face it names.
    LengthContext length_context(ComputedStyle const& style, FontRatios const& ratios) const
    {
        return LengthContext { style.font_size, root_font_size, set.media.width, set.media.height,
            style.font_size * ratios.ex, style.font_size * ratios.ch };
    }

    explicit Resolver(RuleSet const& the_set)
        : set(the_set)
    {
        for (int target = 0; target < target_count; ++target) {
            rule_stamp[static_cast<std::size_t>(target)].assign(the_set.rules.size(), 0);
            rule_best[static_cast<std::size_t>(target)].assign(the_set.rules.size(), Specificity {});
        }
    }

    static std::size_t target_of(ComplexSelector const& selector)
    {
        switch (selector.pseudo_element) {
        case ComplexSelector::PseudoElement::None:
            return 0;
        case ComplexSelector::PseudoElement::Before:
            return 1;
        case ComplexSelector::PseudoElement::After:
            return 2;
        case ComplexSelector::PseudoElement::FirstLetter:
            return 3;
        }
        return 0;
    }

    // The rules matching the element, per target, in rule order, each with
    // the specificity of its best matching selector.
    void matching_rules(dom::Element const& element)
    {
        ++stamp;
        for (std::vector<std::uint32_t>& out : matched_rules)
            out.clear();
        auto const consider = [&](std::vector<RuleSet::Candidate> const& candidates) {
            for (RuleSet::Candidate const& candidate : candidates) {
                if (!ancestors.may_contain_all(candidate.ancestor_hashes))
                    continue;
                ComplexSelector const& selector
                    = set.rules[candidate.rule].selectors.selectors[candidate.selector];
                if (!matches(selector, element))
                    continue;
                std::size_t const target = target_of(selector);
                if (rule_stamp[target][candidate.rule] != stamp) {
                    rule_stamp[target][candidate.rule] = stamp;
                    rule_best[target][candidate.rule] = selector.specificity;
                    matched_rules[target].push_back(candidate.rule);
                } else if (selector.specificity > rule_best[target][candidate.rule]) {
                    rule_best[target][candidate.rule] = selector.specificity;
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
        for (std::vector<std::uint32_t>& out : matched_rules)
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

    // Elements whose content is not a document subtree get no generated
    // boxes: replaced elements, form controls and line breaks.
    static bool can_generate(dom::Element const& element)
    {
        if (!element.is_html())
            return true;
        for (std::string_view const name :
            { "img", "br", "input", "textarea", "select", "iframe", "video", "audio", "canvas", "hr" }) {
            if (element.is_html(name))
                return false;
        }
        return true;
    }

    static bool generates_box(ComputedStyle const& pseudo)
    {
        return pseudo.content.kind == Content::Kind::Items && pseudo.display != Display::None
            && pseudo.display != Display::TableColumn && pseudo.display != Display::TableColumnGroup;
    }

    // The text a generated box shows: its content items resolved in tree
    // order — attributes read off the element, quotation marks chosen by
    // the nesting depth (the pair at the depth, the last pair for anything
    // deeper; open-quote then goes one deeper, close-quote first comes one
    // back; quotes: none inserts nothing).
    std::string content_text(dom::Element const& element, ComputedStyle const& pseudo)
    {
        QuotePairs const* const pairs = pseudo.quotes.get();
        auto const mark = [&](int depth, bool open) -> std::string {
            if (pairs) {
                if (pairs->empty())
                    return {};
                std::size_t const index = std::min(static_cast<std::size_t>(depth), pairs->size() - 1);
                return open ? (*pairs)[index].first : (*pairs)[index].second;
            }
            if (depth % 2 == 0)
                return open ? "“" : "”";
            return open ? "‘" : "’";
        };
        std::string text;
        for (ContentItem const& item : pseudo.content.items) {
            switch (item.kind) {
            case ContentItem::Kind::String:
                text += item.text;
                break;
            case ContentItem::Kind::Attr:
                if (dom::Attr const* attribute = element.find_attribute(item.text))
                    text += attribute->value;
                else
                    text += item.fallback;
                break;
            case ContentItem::Kind::OpenQuote:
                text += mark(quote_depth, true);
                ++quote_depth;
                break;
            case ContentItem::Kind::CloseQuote:
                if (quote_depth > 0)
                    --quote_depth;
                text += mark(quote_depth, false);
                break;
            case ContentItem::Kind::NoOpenQuote:
                ++quote_depth;
                break;
            case ContentItem::Kind::NoCloseQuote:
                if (quote_depth > 0)
                    --quote_depth;
                break;
            case ContentItem::Kind::Counter: {
                // The innermost instance in scope; a counter nothing has
                // reset reads as the zero §12.4.3 would have put there.
                CounterInstance const* const instance = innermost_counter(item.text);
                text += format_counter(instance ? instance->value : 0, item.style);
                break;
            }
            case ContentItem::Kind::Counters: {
                // Every instance in scope, outermost first, with the
                // separator between them. Instances that went out of scope
                // are off the stack already, so the stack IS the nesting.
                bool written = false;
                for (CounterInstance const& instance : counter_stack) {
                    if (instance.name != item.text)
                        continue;
                    if (written)
                        text += item.fallback;
                    text += format_counter(instance.value, item.style);
                    written = true;
                }
                if (!written)
                    text += format_counter(0, item.style);
                break;
            }
            }
        }
        return text;
    }

    void resolve_tree(dom::Node const& node, ComputedStyle const& parent_style)
    {
        ComputedStyle const* style_for_children = &parent_style;
        std::vector<std::uint32_t> offered;
        GeneratedContent* generated = nullptr;
        dom::Element const* owner = nullptr;
        bool hidden_subtree = false;
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            ComputedStyle style = compute_for(element, parent_style);
            bool const is_root = node.parent() && !node.parent()->is_element();
            if (is_root)
                root_font_size = style.font_size; // rem resolves against this
            // A box that is never generated does no counter work, and neither
            // does anything inside it (§12.4.3). visibility: hidden is not
            // that — a hidden box is generated and counts.
            if (style.display == Display::None) {
                hidden_subtree = true;
                ++display_none_depth;
            }
            apply_counter_ops(style);
            // The generated boxes: each cascades from the rules matched for
            // its target (still in the scratch lists), inheriting from the
            // element. Their own counter work and text wait until the walk
            // has stepped into the element, since a pseudo-element is a
            // child of it — the ::before before the children, the ::after
            // after them, which is the order the quotation marks want too.
            if (style.display != Display::None && can_generate(element)) {
                std::optional<ComputedStyle> before;
                std::optional<ComputedStyle> after;
                if (!matched_rules[1].empty())
                    before = cascade(1, element, style, false);
                if (!matched_rules[2].empty())
                    after = cascade(2, element, style, false);
                bool const has_before = before && generates_box(*before);
                bool const has_after = after && generates_box(*after);
                if (has_before || has_after) {
                    auto boxes = std::make_shared<GeneratedContent>();
                    if (has_before)
                        boxes->before = GeneratedBox { std::move(*before), {} };
                    if (has_after)
                        boxes->after = GeneratedBox { std::move(*after), {} };
                    generated = boxes.get();
                    owner = &element;
                    style.generated = std::move(boxes);
                }
            }
            // ::first-letter cascades the same way, from the rules matched
            // for its own target. It addresses a block container's first
            // line, so a flex or grid container, a table box or a row is
            // passed by. Layout decides whether there is a first letter to
            // dress; the style is ready either way.
            if (is_block_container_display(style.display) && !matched_rules[3].empty())
                style.first_letter
                    = std::make_shared<ComputedStyle const>(cascade(3, element, style, false));
            auto const [it, inserted] = map.emplace(&element, std::move(style));
            (void)inserted;
            style_for_children = &it->second;
            offered = identifier_hashes(element);
            for (std::uint32_t const hash : offered)
                ancestors.push(hash);
        }
        // Everything from here down is inside this node: the instances the
        // pseudo-elements and the children make go out of scope together
        // when the walk comes back out, which is exactly §12.4.3's rule that
        // a counter covers its box's following siblings and no further.
        std::size_t const scope = counter_stack.size();
        ++counter_depth;
        if (generated && generated->before) {
            apply_counter_ops(generated->before->style);
            generated->before->text = content_text(*owner, generated->before->style);
        }
        for (dom::Node const* child : node.children())
            resolve_tree(*child, *style_for_children);
        if (generated && generated->after) {
            apply_counter_ops(generated->after->style);
            generated->after->text = content_text(*owner, generated->after->style);
        }
        --counter_depth;
        counter_stack.resize(scope);
        for (std::uint32_t const hash : offered)
            ancestors.pop(hash);
        if (hidden_subtree)
            --display_none_depth;
    }

    // The block-level box that holds this element's first formatted line,
    // when its own content is boxes rather than text: the first in-flow
    // block-level child, and only while nothing inline comes before it.
    dom::Element const* first_block_child(dom::Element const& element) const
    {
        for (dom::Node const* child : element.children()) {
            if (child->is_text()) {
                for (char const c : static_cast<dom::Text const*>(child)->data) {
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
                        return nullptr; // the first line is this element's own
                }
                continue;
            }
            if (!child->is_element())
                continue;
            auto const* candidate = static_cast<dom::Element const*>(child);
            auto const it = map.find(candidate);
            if (it == map.end() || it->second.display == Display::None)
                continue;
            if (it->second.out_of_flow() || it->second.floating != Float::None)
                continue; // out of the flow: the first line is not its business
            if (!is_block_level_display(it->second.display))
                return nullptr;
            // Only a block container has a first line to hand the style on to.
            return is_block_container_display(it->second.display) ? candidate : nullptr;
        }
        return nullptr;
    }

    // CSS 2.1 §5.12.2: ::first-letter dresses the first letter of the
    // block's first formatted line, and when the block holds boxes rather
    // than text that line lives inside the first of them. The style is
    // handed down that chain until it reaches the box whose own content
    // starts the line; layout then takes it from there.
    void hand_down_first_letters(dom::Node const& node)
    {
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            auto const it = map.find(&element);
            if (it != map.end() && it->second.first_letter) {
                std::shared_ptr<ComputedStyle const> const letter = it->second.first_letter;
                for (dom::Element const* target = first_block_child(element); target;) {
                    auto const child = map.find(target);
                    if (child == map.end() || child->second.first_letter)
                        break;
                    child->second.first_letter = letter;
                    target = first_block_child(*target);
                }
            }
        }
        for (dom::Node const* child : node.children())
            hand_down_first_letters(*child);
    }

    // Inherited properties flow in from the parent; the rest start at
    // their initial values.
    static ComputedStyle inherited_from(ComputedStyle const& parent)
    {
        ComputedStyle style;
        style.color = parent.color;
        style.font_size = parent.font_size;
        style.font_weight = parent.font_weight;
        style.font_style = parent.font_style;
        style.font_family = parent.font_family;
        style.line_height = parent.line_height;
        style.direction = parent.direction;
        style.writing_mode = parent.writing_mode;
        style.text_orientation = parent.text_orientation;
        style.text_align = parent.text_align;
        style.text_align_last = parent.text_align_last;
        style.text_justify = parent.text_justify;
        style.letter_spacing = parent.letter_spacing;
        style.word_spacing = parent.word_spacing;
        style.text_indent = parent.text_indent;
        style.white_space = parent.white_space;
        style.text_transform = parent.text_transform;
        style.list_style_type = parent.list_style_type;
        style.list_style_position = parent.list_style_position;
        style.quotes = parent.quotes;
        style.custom = parent.custom;
        style.visibility = parent.visibility;
        style.border_collapse = parent.border_collapse;
        style.border_spacing_horizontal = parent.border_spacing_horizontal;
        style.border_spacing_vertical = parent.border_spacing_vertical;
        style.caption_side = parent.caption_side;
        style.empty_cells = parent.empty_cells;
        return style;
    }

    // --- The CSS-wide keywords --------------------------------------------------

    // inherit takes the parent's computed value, initial the property's
    // initial value, unset one or the other by whether the property
    // inherits; revert, with no user sheet to revert to, reads as unset.
    enum class Wide {
        Inherit,
        Initial,
        Unset,
    };

    static std::optional<Wide> wide_keyword(std::vector<ComponentValue const*> const& values)
    {
        if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
            return std::nullopt;
        std::string_view const keyword = values[0]->token().value;
        if (ascii_ci_equals(keyword, "inherit"))
            return Wide::Inherit;
        if (ascii_ci_equals(keyword, "initial"))
            return Wide::Initial;
        if (ascii_ci_equals(keyword, "unset") || ascii_ci_equals(keyword, "revert")
            || ascii_ci_equals(keyword, "revert-layer"))
            return Wide::Unset;
        return std::nullopt;
    }

    // A property the keywords can copy: its members, and whether it
    // inherits. A shorthand copies every longhand it holds.
    struct PropertyCopy {
        std::string_view name;
        bool inherited;
        void (*copy)(ComputedStyle& to, ComputedStyle const& from);
        // Which border sides' colors the property carries: their "set"
        // flags follow the copy (a parent's color is a color of its own;
        // an initial one is currentColor).
        unsigned border_colors; // bits: 1 top, 2 right, 4 bottom, 8 left
    };

    static std::vector<PropertyCopy> const& property_copies()
    {
        using S = ComputedStyle;
        static std::vector<PropertyCopy> const table = {
            { "display", false, [](S& to, S const& from) { to.display = from.display; }, 0 },
            { "width", false, [](S& to, S const& from) { to.width = from.width; }, 0 },
            { "height", false, [](S& to, S const& from) { to.height = from.height; }, 0 },
            { "min-width", false, [](S& to, S const& from) { to.min_width = from.min_width; }, 0 },
            { "max-width", false, [](S& to, S const& from) { to.max_width = from.max_width; }, 0 },
            { "min-height", false, [](S& to, S const& from) { to.min_height = from.min_height; }, 0 },
            { "max-height", false, [](S& to, S const& from) { to.max_height = from.max_height; }, 0 },
            { "margin-top", false, [](S& to, S const& from) { to.margin_top = from.margin_top; }, 0 },
            { "margin-right", false, [](S& to, S const& from) { to.margin_right = from.margin_right; }, 0 },
            { "margin-bottom", false, [](S& to, S const& from) { to.margin_bottom = from.margin_bottom; }, 0 },
            { "margin-left", false, [](S& to, S const& from) { to.margin_left = from.margin_left; }, 0 },
            { "margin", false,
                [](S& to, S const& from) {
                    to.margin_top = from.margin_top;
                    to.margin_right = from.margin_right;
                    to.margin_bottom = from.margin_bottom;
                    to.margin_left = from.margin_left;
                },
                0 },
            { "padding-top", false, [](S& to, S const& from) { to.padding_top = from.padding_top; }, 0 },
            { "padding-right", false, [](S& to, S const& from) { to.padding_right = from.padding_right; }, 0 },
            { "padding-bottom", false, [](S& to, S const& from) { to.padding_bottom = from.padding_bottom; }, 0 },
            { "padding-left", false, [](S& to, S const& from) { to.padding_left = from.padding_left; }, 0 },
            { "padding", false,
                [](S& to, S const& from) {
                    to.padding_top = from.padding_top;
                    to.padding_right = from.padding_right;
                    to.padding_bottom = from.padding_bottom;
                    to.padding_left = from.padding_left;
                },
                0 },
            { "border-top", false, [](S& to, S const& from) { to.border_top = from.border_top; }, 1 },
            { "border-right", false, [](S& to, S const& from) { to.border_right = from.border_right; }, 2 },
            { "border-bottom", false, [](S& to, S const& from) { to.border_bottom = from.border_bottom; }, 4 },
            { "border-left", false, [](S& to, S const& from) { to.border_left = from.border_left; }, 8 },
            { "border", false,
                [](S& to, S const& from) {
                    to.border_top = from.border_top;
                    to.border_right = from.border_right;
                    to.border_bottom = from.border_bottom;
                    to.border_left = from.border_left;
                },
                15 },
            { "border-top-width", false, [](S& to, S const& from) { to.border_top.width = from.border_top.width; }, 0 },
            { "border-right-width", false, [](S& to, S const& from) { to.border_right.width = from.border_right.width; }, 0 },
            { "border-bottom-width", false, [](S& to, S const& from) { to.border_bottom.width = from.border_bottom.width; }, 0 },
            { "border-left-width", false, [](S& to, S const& from) { to.border_left.width = from.border_left.width; }, 0 },
            { "border-width", false,
                [](S& to, S const& from) {
                    to.border_top.width = from.border_top.width;
                    to.border_right.width = from.border_right.width;
                    to.border_bottom.width = from.border_bottom.width;
                    to.border_left.width = from.border_left.width;
                },
                0 },
            { "border-top-left-radius", false,
                [](S& to, S const& from) { to.border_top_left_radius = from.border_top_left_radius; }, 0 },
            { "border-top-right-radius", false,
                [](S& to, S const& from) { to.border_top_right_radius = from.border_top_right_radius; }, 0 },
            { "border-bottom-right-radius", false,
                [](S& to, S const& from) { to.border_bottom_right_radius = from.border_bottom_right_radius; }, 0 },
            { "border-bottom-left-radius", false,
                [](S& to, S const& from) { to.border_bottom_left_radius = from.border_bottom_left_radius; }, 0 },
            { "border-radius", false,
                [](S& to, S const& from) {
                    to.border_top_left_radius = from.border_top_left_radius;
                    to.border_top_right_radius = from.border_top_right_radius;
                    to.border_bottom_right_radius = from.border_bottom_right_radius;
                    to.border_bottom_left_radius = from.border_bottom_left_radius;
                },
                0 },
            { "border-top-style", false, [](S& to, S const& from) { to.border_top.style = from.border_top.style; }, 0 },
            { "border-right-style", false, [](S& to, S const& from) { to.border_right.style = from.border_right.style; }, 0 },
            { "border-bottom-style", false, [](S& to, S const& from) { to.border_bottom.style = from.border_bottom.style; }, 0 },
            { "border-left-style", false, [](S& to, S const& from) { to.border_left.style = from.border_left.style; }, 0 },
            { "border-style", false,
                [](S& to, S const& from) {
                    to.border_top.style = from.border_top.style;
                    to.border_right.style = from.border_right.style;
                    to.border_bottom.style = from.border_bottom.style;
                    to.border_left.style = from.border_left.style;
                },
                0 },
            { "border-top-color", false, [](S& to, S const& from) { to.border_top.color = from.border_top.color; }, 1 },
            { "border-right-color", false, [](S& to, S const& from) { to.border_right.color = from.border_right.color; }, 2 },
            { "border-bottom-color", false, [](S& to, S const& from) { to.border_bottom.color = from.border_bottom.color; }, 4 },
            { "border-left-color", false, [](S& to, S const& from) { to.border_left.color = from.border_left.color; }, 8 },
            { "border-color", false,
                [](S& to, S const& from) {
                    to.border_top.color = from.border_top.color;
                    to.border_right.color = from.border_right.color;
                    to.border_bottom.color = from.border_bottom.color;
                    to.border_left.color = from.border_left.color;
                },
                15 },
            { "float", false, [](S& to, S const& from) { to.floating = from.floating; }, 0 },
            { "clear", false, [](S& to, S const& from) { to.clear = from.clear; }, 0 },
            { "box-sizing", false, [](S& to, S const& from) { to.box_sizing = from.box_sizing; }, 0 },
            { "overflow", false,
                [](S& to, S const& from) {
                    to.overflow = from.overflow;
                    to.overflow_x = from.overflow_x;
                    to.overflow_y = from.overflow_y;
                },
                0 },
            { "overflow-x", false,
                [](S& to, S const& from) { to.overflow = from.overflow; to.overflow_x = from.overflow_x; }, 0 },
            { "overflow-y", false,
                [](S& to, S const& from) { to.overflow = from.overflow; to.overflow_y = from.overflow_y; }, 0 },
            { "position", false, [](S& to, S const& from) { to.position = from.position; }, 0 },
            { "top", false, [](S& to, S const& from) { to.top = from.top; }, 0 },
            { "right", false, [](S& to, S const& from) { to.right = from.right; }, 0 },
            { "bottom", false, [](S& to, S const& from) { to.bottom = from.bottom; }, 0 },
            { "left", false, [](S& to, S const& from) { to.left = from.left; }, 0 },
            { "inset", false,
                [](S& to, S const& from) {
                    to.top = from.top;
                    to.right = from.right;
                    to.bottom = from.bottom;
                    to.left = from.left;
                },
                0 },
            { "z-index", false, [](S& to, S const& from) { to.z_index = from.z_index; }, 0 },
            { "visibility", true, [](S& to, S const& from) { to.visibility = from.visibility; }, 0 },
            { "opacity", false, [](S& to, S const& from) { to.opacity = from.opacity; }, 0 },
            { "transform", false,
                [](S& to, S const& from) {
                    to.translate_x = from.translate_x;
                    to.translate_y = from.translate_y;
                    to.transformed = from.transformed;
                },
                0 },
            { "translate", false,
                [](S& to, S const& from) {
                    to.translate_x = from.translate_x;
                    to.translate_y = from.translate_y;
                    to.transformed = from.transformed;
                },
                0 },
            { "flex-direction", false, [](S& to, S const& from) { to.flex_direction = from.flex_direction; }, 0 },
            { "flex-wrap", false, [](S& to, S const& from) { to.flex_wrap = from.flex_wrap; }, 0 },
            { "flex-flow", false,
                [](S& to, S const& from) {
                    to.flex_direction = from.flex_direction;
                    to.flex_wrap = from.flex_wrap;
                },
                0 },
            { "justify-content", false, [](S& to, S const& from) { to.justify_content = from.justify_content; }, 0 },
            { "align-items", false, [](S& to, S const& from) { to.align_items = from.align_items; }, 0 },
            { "align-self", false, [](S& to, S const& from) { to.align_self = from.align_self; }, 0 },
            { "align-content", false, [](S& to, S const& from) { to.align_content = from.align_content; }, 0 },
            { "flex-grow", false, [](S& to, S const& from) { to.flex_grow = from.flex_grow; }, 0 },
            { "flex-shrink", false, [](S& to, S const& from) { to.flex_shrink = from.flex_shrink; }, 0 },
            { "flex-basis", false, [](S& to, S const& from) { to.flex_basis = from.flex_basis; }, 0 },
            { "flex", false,
                [](S& to, S const& from) {
                    to.flex_grow = from.flex_grow;
                    to.flex_shrink = from.flex_shrink;
                    to.flex_basis = from.flex_basis;
                },
                0 },
            { "row-gap", false, [](S& to, S const& from) { to.row_gap = from.row_gap; }, 0 },
            { "column-gap", false, [](S& to, S const& from) { to.column_gap = from.column_gap; }, 0 },
            { "gap", false,
                [](S& to, S const& from) {
                    to.row_gap = from.row_gap;
                    to.column_gap = from.column_gap;
                },
                0 },
            { "grid-row-gap", false, [](S& to, S const& from) { to.row_gap = from.row_gap; }, 0 },
            { "grid-column-gap", false, [](S& to, S const& from) { to.column_gap = from.column_gap; }, 0 },
            { "grid-gap", false,
                [](S& to, S const& from) {
                    to.row_gap = from.row_gap;
                    to.column_gap = from.column_gap;
                },
                0 },
            { "order", false, [](S& to, S const& from) { to.order = from.order; }, 0 },
            { "border-collapse", true, [](S& to, S const& from) { to.border_collapse = from.border_collapse; }, 0 },
            { "border-spacing", true,
                [](S& to, S const& from) {
                    to.border_spacing_horizontal = from.border_spacing_horizontal;
                    to.border_spacing_vertical = from.border_spacing_vertical;
                },
                0 },
            { "caption-side", true, [](S& to, S const& from) { to.caption_side = from.caption_side; }, 0 },
            { "empty-cells", true, [](S& to, S const& from) { to.empty_cells = from.empty_cells; }, 0 },
            { "table-layout", false, [](S& to, S const& from) { to.table_layout = from.table_layout; }, 0 },
            { "justify-items", false, [](S& to, S const& from) { to.justify_items = from.justify_items; }, 0 },
            { "justify-self", false, [](S& to, S const& from) { to.justify_self = from.justify_self; }, 0 },
            { "place-items", false,
                [](S& to, S const& from) {
                    to.align_items = from.align_items;
                    to.justify_items = from.justify_items;
                },
                0 },
            { "place-self", false,
                [](S& to, S const& from) {
                    to.align_self = from.align_self;
                    to.justify_self = from.justify_self;
                },
                0 },
            { "place-content", false,
                [](S& to, S const& from) {
                    to.align_content = from.align_content;
                    to.justify_content = from.justify_content;
                },
                0 },
            { "grid-template-columns", false,
                [](S& to, S const& from) { to.grid_template_columns = from.grid_template_columns; }, 0 },
            { "grid-template-rows", false,
                [](S& to, S const& from) { to.grid_template_rows = from.grid_template_rows; }, 0 },
            { "grid-template-areas", false,
                [](S& to, S const& from) { to.grid_template_areas = from.grid_template_areas; }, 0 },
            { "grid-template", false,
                [](S& to, S const& from) {
                    to.grid_template_columns = from.grid_template_columns;
                    to.grid_template_rows = from.grid_template_rows;
                    to.grid_template_areas = from.grid_template_areas;
                },
                0 },
            { "grid-auto-columns", false,
                [](S& to, S const& from) { to.grid_auto_columns = from.grid_auto_columns; }, 0 },
            { "grid-auto-rows", false, [](S& to, S const& from) { to.grid_auto_rows = from.grid_auto_rows; }, 0 },
            { "grid-auto-flow", false, [](S& to, S const& from) { to.grid_auto_flow = from.grid_auto_flow; }, 0 },
            { "grid", false,
                [](S& to, S const& from) {
                    to.grid_template_columns = from.grid_template_columns;
                    to.grid_template_rows = from.grid_template_rows;
                    to.grid_template_areas = from.grid_template_areas;
                    to.grid_auto_columns = from.grid_auto_columns;
                    to.grid_auto_rows = from.grid_auto_rows;
                    to.grid_auto_flow = from.grid_auto_flow;
                },
                0 },
            { "grid-row-start", false, [](S& to, S const& from) { to.grid_row_start = from.grid_row_start; }, 0 },
            { "grid-row-end", false, [](S& to, S const& from) { to.grid_row_end = from.grid_row_end; }, 0 },
            { "grid-column-start", false,
                [](S& to, S const& from) { to.grid_column_start = from.grid_column_start; }, 0 },
            { "grid-column-end", false, [](S& to, S const& from) { to.grid_column_end = from.grid_column_end; }, 0 },
            { "grid-row", false,
                [](S& to, S const& from) {
                    to.grid_row_start = from.grid_row_start;
                    to.grid_row_end = from.grid_row_end;
                },
                0 },
            { "grid-column", false,
                [](S& to, S const& from) {
                    to.grid_column_start = from.grid_column_start;
                    to.grid_column_end = from.grid_column_end;
                },
                0 },
            { "grid-area", false,
                [](S& to, S const& from) {
                    to.grid_row_start = from.grid_row_start;
                    to.grid_row_end = from.grid_row_end;
                    to.grid_column_start = from.grid_column_start;
                    to.grid_column_end = from.grid_column_end;
                },
                0 },
            { "color", true, [](S& to, S const& from) { to.color = from.color; }, 0 },
            { "background-color", false, [](S& to, S const& from) { to.background_color = from.background_color; }, 0 },
            { "background-image", false, [](S& to, S const& from) { to.background_images = from.background_images; }, 0 },
            { "background-repeat", false, [](S& to, S const& from) { to.background_repeats = from.background_repeats; }, 0 },
            { "background-position", false,
                [](S& to, S const& from) { to.background_positions = from.background_positions; }, 0 },
            { "background-size", false, [](S& to, S const& from) { to.background_sizes = from.background_sizes; }, 0 },
            { "background-origin", false, [](S& to, S const& from) { to.background_origins = from.background_origins; }, 0 },
            { "background-clip", false, [](S& to, S const& from) { to.background_clips = from.background_clips; }, 0 },
            { "background", false,
                [](S& to, S const& from) {
                    to.background_color = from.background_color;
                    to.background_images = from.background_images;
                    to.background_repeats = from.background_repeats;
                    to.background_positions = from.background_positions;
                    to.background_sizes = from.background_sizes;
                    to.background_origins = from.background_origins;
                    to.background_clips = from.background_clips;
                },
                0 },
            { "font-size", true, [](S& to, S const& from) { to.font_size = from.font_size; }, 0 },
            { "font-weight", true, [](S& to, S const& from) { to.font_weight = from.font_weight; }, 0 },
            { "font-style", true, [](S& to, S const& from) { to.font_style = from.font_style; }, 0 },
            { "font-family", true, [](S& to, S const& from) { to.font_family = from.font_family; }, 0 },
            { "line-height", true, [](S& to, S const& from) { to.line_height = from.line_height; }, 0 },
            { "font", true,
                [](S& to, S const& from) {
                    to.font_size = from.font_size;
                    to.font_weight = from.font_weight;
                    to.font_style = from.font_style;
                    to.font_family = from.font_family;
                    to.line_height = from.line_height;
                },
                0 },
            { "vertical-align", false, [](S& to, S const& from) { to.vertical_align = from.vertical_align; }, 0 },
            // text-align is a shorthand of text-align-all and
            // text-align-last (css-text-3 §7.1), so it carries both.
            { "text-align", true,
                [](S& to, S const& from) {
                    to.text_align = from.text_align;
                    to.text_align_last = from.text_align_last;
                },
                0 },
            { "text-align-last", true,
                [](S& to, S const& from) { to.text_align_last = from.text_align_last; }, 0 },
            { "text-justify", true, [](S& to, S const& from) { to.text_justify = from.text_justify; }, 0 },
            { "direction", true, [](S& to, S const& from) { to.direction = from.direction; }, 0 },
            { "writing-mode", true, [](S& to, S const& from) { to.writing_mode = from.writing_mode; }, 0 },
            { "text-orientation", true,
                [](S& to, S const& from) { to.text_orientation = from.text_orientation; }, 0 },
            { "unicode-bidi", false, [](S& to, S const& from) { to.unicode_bidi = from.unicode_bidi; }, 0 },
            { "letter-spacing", true, [](S& to, S const& from) { to.letter_spacing = from.letter_spacing; }, 0 },
            { "word-spacing", true, [](S& to, S const& from) { to.word_spacing = from.word_spacing; }, 0 },
            { "text-indent", true, [](S& to, S const& from) { to.text_indent = from.text_indent; }, 0 },
            { "white-space", true, [](S& to, S const& from) { to.white_space = from.white_space; }, 0 },
            { "text-transform", true,
                [](S& to, S const& from) { to.text_transform = from.text_transform; }, 0 },
            { "list-style-type", true, [](S& to, S const& from) { to.list_style_type = from.list_style_type; }, 0 },
            { "list-style-position", true,
                [](S& to, S const& from) { to.list_style_position = from.list_style_position; }, 0 },
            { "list-style", true,
                [](S& to, S const& from) {
                    to.list_style_type = from.list_style_type;
                    to.list_style_position = from.list_style_position;
                },
                0 },
            { "text-decoration", false, [](S& to, S const& from) { to.text_decoration = from.text_decoration; }, 0 },
            { "text-decoration-line", false, [](S& to, S const& from) { to.text_decoration = from.text_decoration; }, 0 },
            { "content", false, [](S& to, S const& from) { to.content = from.content; }, 0 },
            { "quotes", true, [](S& to, S const& from) { to.quotes = from.quotes; }, 0 },
            { "counter-reset", false, [](S& to, S const& from) { to.counter_reset = from.counter_reset; }, 0 },
            { "counter-increment", false,
                [](S& to, S const& from) { to.counter_increment = from.counter_increment; }, 0 },
            { "counter-set", false, [](S& to, S const& from) { to.counter_set = from.counter_set; }, 0 },
        };
        return table;
    }

    // Applies a CSS-wide keyword to one property, or with `all` to every
    // property the table knows (custom properties aside, which cascade on
    // their own).
    static void apply_wide(ComputedStyle& style, ComputedStyle const& parent, std::string_view name,
        Wide wide, bool& border_top_color_set, bool& border_right_color_set,
        bool& border_bottom_color_set, bool& border_left_color_set)
    {
        static ComputedStyle const initial {};
        // The size was settled in the font-size pass, in cascade order with
        // every other font-size declaration; a copy here would undo a later
        // one.
        float const settled_font_size = style.font_size;
        auto const apply_one = [&](PropertyCopy const& property) {
            bool const from_parent = wide == Wide::Inherit || (wide == Wide::Unset && property.inherited);
            ComputedStyle const& from = from_parent ? parent : initial;
            property.copy(style, from);
            style.font_size = settled_font_size;
            // A parent's border color that was currentColor inherits as the
            // keyword: this element's own color, settled at the end.
            if (property.border_colors & 1)
                border_top_color_set = from_parent && !from.border_top.current_color;
            if (property.border_colors & 2)
                border_right_color_set = from_parent && !from.border_right.current_color;
            if (property.border_colors & 4)
                border_bottom_color_set = from_parent && !from.border_bottom.current_color;
            if (property.border_colors & 8)
                border_left_color_set = from_parent && !from.border_left.current_color;
        };
        if (name == "all") {
            for (PropertyCopy const& property : property_copies()) {
                // Longhands only: a shorthand's members are covered by them.
                if (property.name == "margin" || property.name == "padding" || property.name == "border"
                    || property.name == "border-top" || property.name == "border-right"
                    || property.name == "border-bottom" || property.name == "border-left"
                    || property.name == "border-width" || property.name == "border-style"
                    || property.name == "border-color" || property.name == "border-radius"
                    || property.name == "inset"
                    || property.name == "flex-flow" || property.name == "flex" || property.name == "gap"
                    || property.name == "grid-gap" || property.name == "grid-row-gap"
                    || property.name == "grid-column-gap" || property.name == "place-items"
                    || property.name == "place-self" || property.name == "place-content"
                    || property.name == "grid-template" || property.name == "grid"
                    || property.name == "grid-row" || property.name == "grid-column"
                    || property.name == "grid-area"
                    || property.name == "background" || property.name == "font"
                    || property.name == "list-style" || property.name == "translate"
                    || property.name == "overflow-x" || property.name == "overflow-y"
                    || property.name == "text-decoration-line")
                    continue;
                apply_one(property);
            }
            return;
        }
        for (PropertyCopy const& property : property_copies()) {
            if (property.name == name) {
                apply_one(property);
                return;
            }
        }
    }

    // --- Custom properties and var() ------------------------------------------

    static bool is_var(ComponentValue const& value)
    {
        return value.is_function() && ascii_ci_equals(value.function().name, "var");
    }

    static bool contains_var(std::vector<ComponentValue> const& values)
    {
        for (ComponentValue const& value : values) {
            if (value.is_function()) {
                if (is_var(value) || contains_var(value.function().values))
                    return true;
            } else if (value.is_block() && contains_var(value.block().values)) {
                return true;
            }
        }
        return false;
    }

    static std::vector<ComponentValue> trimmed(std::vector<ComponentValue> const& values)
    {
        std::size_t start = 0;
        std::size_t end = values.size();
        while (start < end && values[start].is_token(Token::Type::Whitespace))
            ++start;
        while (end > start && values[end - 1].is_token(Token::Type::Whitespace))
            --end;
        return std::vector<ComponentValue>(values.begin() + static_cast<std::ptrdiff_t>(start),
            values.begin() + static_cast<std::ptrdiff_t>(end));
    }

    // Replaces every var() in `in` with the named custom property's value,
    // or the fallback after the comma, into `out`. False when a reference
    // has neither (the declaration is then invalid at computed-value
    // time), when the nesting runs too deep, or when the result outgrows
    // the budget — the guard against a value that doubles itself.
    static bool substitute_vars(std::vector<ComponentValue> const& in, CustomProperties const* custom,
        std::vector<ComponentValue>& out, int depth, std::size_t& budget)
    {
        if (depth > 32)
            return false;
        for (ComponentValue const& value : in) {
            if (budget == 0)
                return false;
            --budget;
            if (value.is_function()) {
                FunctionValue const& function = value.function();
                if (is_var(value)) {
                    std::vector<ComponentValue> const& arguments = function.values;
                    std::size_t i = 0;
                    while (i < arguments.size() && arguments[i].is_token(Token::Type::Whitespace))
                        ++i;
                    if (i >= arguments.size() || !arguments[i].is_token(Token::Type::Ident)
                        || !arguments[i].token().value.starts_with("--"))
                        return false;
                    std::string const& name = arguments[i].token().value;
                    ++i;
                    while (i < arguments.size() && arguments[i].is_token(Token::Type::Whitespace))
                        ++i;
                    bool has_fallback = false;
                    std::vector<ComponentValue> fallback;
                    if (i < arguments.size()) {
                        if (!arguments[i].is_token(Token::Type::Comma))
                            return false;
                        has_fallback = true;
                        fallback.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i) + 1, arguments.end());
                    }
                    auto const it = custom ? custom->find(name) : CustomProperties::const_iterator {};
                    if (custom && it != custom->end()) {
                        if (it->second.size() > budget)
                            return false;
                        budget -= it->second.size();
                        out.insert(out.end(), it->second.begin(), it->second.end());
                    } else if (has_fallback) {
                        if (!substitute_vars(fallback, custom, out, depth + 1, budget))
                            return false;
                    } else {
                        return false;
                    }
                    continue;
                }
                FunctionValue copy;
                copy.name = function.name;
                if (!substitute_vars(function.values, custom, copy.values, depth + 1, budget))
                    return false;
                out.push_back(ComponentValue { std::move(copy) });
            } else if (value.is_block()) {
                SimpleBlock copy;
                copy.open = value.block().open;
                if (!substitute_vars(value.block().values, custom, copy.values, depth + 1, budget))
                    return false;
                out.push_back(ComponentValue { std::move(copy) });
            } else {
                out.push_back(value);
            }
        }
        return true;
    }

    // The custom properties an element declares, over the inherited set:
    // each value's own var() references resolved against the others (in
    // any order, with cycles and dead ends dropping the property, the
    // guaranteed-invalid value), then stored substituted.
    static std::shared_ptr<CustomProperties const> settle_custom_properties(
        CustomProperties&& own, CustomProperties const* inherited)
    {
        auto settled = std::make_shared<CustomProperties>();
        if (inherited)
            *settled = *inherited;
        // The declared ones override; resolve them against a working map
        // that holds the inherited values and the declared raw ones.
        CustomProperties working = *settled;
        for (auto const& [name, value] : own)
            working[name] = value;
        std::unordered_map<std::string, int> state; // 0 untouched, 1 in progress, 2 done
        std::function<bool(std::string const&)> const resolve = [&](std::string const& name) -> bool {
            auto const it = working.find(name);
            if (it == working.end())
                return false;
            int& mark = state[name];
            if (mark == 2)
                return true;
            if (mark == 1)
                return false; // a cycle
            mark = 1;
            if (contains_var(it->second)) {
                // Resolve what this value refers to first, so the working
                // map holds substituted values when this one is built.
                std::vector<std::string> referenced;
                std::function<void(std::vector<ComponentValue> const&)> const collect
                    = [&](std::vector<ComponentValue> const& values) {
                          for (ComponentValue const& value : values) {
                              if (value.is_function()) {
                                  if (is_var(value)) {
                                      for (ComponentValue const& argument : value.function().values) {
                                          if (argument.is_token(Token::Type::Ident)
                                              && argument.token().value.starts_with("--")) {
                                              referenced.push_back(argument.token().value);
                                              break;
                                          }
                                      }
                                  }
                                  collect(value.function().values);
                              } else if (value.is_block()) {
                                  collect(value.block().values);
                              }
                          }
                      };
                collect(it->second);
                bool ok = true;
                for (std::string const& other : referenced) {
                    if (working.count(other) && !resolve(other))
                        ok = false;
                }
                std::vector<ComponentValue> substituted;
                std::size_t budget = 65536;
                if (!ok || !substitute_vars(it->second, &working, substituted, 0, budget)) {
                    working.erase(name);
                    state[name] = 2;
                    return false;
                }
                it->second = std::move(substituted);
            }
            state[name] = 2;
            return true;
        };
        std::vector<std::string> names;
        for (auto const& [name, value] : own)
            names.push_back(name);
        for (std::string const& name : names)
            (void)resolve(name);
        for (std::string const& name : names) {
            auto const it = working.find(name);
            if (it != working.end())
                (*settled)[name] = it->second;
            else
                settled->erase(name);
        }
        return settled;
    }

    ComputedStyle compute_for(dom::Element const& element, ComputedStyle const& parent)
    {
        matching_rules(element);
        return cascade(0, element, parent, true);
    }

    // The cascade over the rules matched for one target — the element's
    // own (with its style attribute) or one of its generated boxes.
    ComputedStyle cascade(std::size_t target, dom::Element const& element, ComputedStyle const& parent,
        bool with_style_attribute)
    {
        ComputedStyle style = inherited_from(parent);

        std::vector<MatchedDeclaration> matched;
        for (std::uint32_t const index : matched_rules[target]) {
            CompiledRule const& rule = set.rules[index];
            Specificity const specificity = rule_best[target][index];
            for (Declaration const& declaration : rule.declarations) {
                MatchedDeclaration entry;
                entry.declaration = &declaration;
                entry.base = rule.base.get();
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
        dom::Attr const* const style_attribute
            = with_style_attribute ? element.find_attribute("style") : nullptr;
        if (style_attribute) {
            attribute_declarations = parse_declaration_list(style_attribute->value);
            int order = 1 << 20;
            for (Declaration const& declaration : attribute_declarations) {
                MatchedDeclaration entry;
                entry.declaration = &declaration;
                entry.base = set.document_url ? &*set.document_url : nullptr;
                entry.order = order++;
                entry.rank = static_cast<int>(declaration.important
                        ? CascadeRank::StyleAttributeImportant
                        : CascadeRank::StyleAttributeNormal);
                matched.push_back(entry);
            }
        }

        // The element's presentational attributes: author declarations of
        // no specificity, ahead of every author rule.
        std::vector<Declaration> hint_declarations;
        if (with_style_attribute) {
            std::string const hints = presentational_hints(element);
            if (!hints.empty()) {
                hint_declarations = parse_declaration_list(hints);
                for (Declaration const& declaration : hint_declarations) {
                    MatchedDeclaration entry;
                    entry.declaration = &declaration;
                    entry.base = set.document_url ? &*set.document_url : nullptr;
                    entry.order = -1;
                    entry.rank = static_cast<int>(CascadeRank::AuthorNormal);
                    matched.push_back(entry);
                }
            }
        }

        std::stable_sort(matched.begin(), matched.end(), cascades_before);

        // Custom properties first: they cascade like any property and the
        // var() references in everything else read the settled set.
        // `initial` drops one, `inherit` and `unset` keep the inherited.
        {
            CustomProperties own;
            std::vector<std::string> initial; // `--x: initial`: dropped from the inherited set too
            bool declared = false;
            for (MatchedDeclaration const& entry : matched) {
                Declaration const& declaration = *entry.declaration;
                if (!declaration.name.starts_with("--"))
                    continue;
                declared = true;
                std::string const& name = declaration.name;
                std::erase(initial, name); // the latest declaration in cascade order wins
                std::vector<ComponentValue> value = trimmed(declaration.value);
                if (value.size() == 1 && value[0].is_token(Token::Type::Ident)) {
                    std::string_view const keyword = value[0].token().value;
                    if (ascii_ci_equals(keyword, "initial")) {
                        own.erase(name);
                        initial.push_back(name);
                        continue;
                    }
                    if (ascii_ci_equals(keyword, "inherit") || ascii_ci_equals(keyword, "unset")) {
                        own.erase(name);
                        continue;
                    }
                }
                own[name] = std::move(value);
            }
            if (declared) {
                std::shared_ptr<CustomProperties const> settled
                    = settle_custom_properties(std::move(own), parent.custom.get());
                if (!initial.empty()) {
                    auto without = std::make_shared<CustomProperties>(*settled);
                    for (std::string const& name : initial)
                        without->erase(name);
                    settled = without;
                }
                style.custom = settled;
            }
        }
        // A declaration that reads custom properties is applied through its
        // substituted copy; one whose reference has no value and no fallback
        // is invalid at computed-value time and skipped.
        auto const with_vars = [&](Declaration const& declaration, auto&& use) {
            if (declaration.name.starts_with("--"))
                return;
            if (!contains_var(declaration.value)) {
                use(declaration);
                return;
            }
            Declaration copy;
            copy.name = declaration.name;
            copy.important = declaration.important;
            std::size_t budget = 65536;
            if (substitute_vars(declaration.value, style.custom.get(), copy.value, 0, budget))
                use(copy);
        };

        // The font goes first: `em` and the font-relative units in the same
        // element's other declarations resolve against its size, and `ex` and
        // `ch` are measurements of the face itself, which the family, the
        // weight and the slant choose between. All four are settled here,
        // whatever order they were written in, and applied again with the
        // rest below, to the same values.
        parent_ratios = ratios_for(parent);
        bool font_pass_top = false;
        bool font_pass_right = false;
        bool font_pass_bottom = false;
        bool font_pass_left = false;
        for (MatchedDeclaration const& entry : matched) {
            with_vars(*entry.declaration, [&](Declaration const& declaration) {
                bool const is_font_size = ascii_ci_equals(declaration.name, "font-size");
                bool const is_font = ascii_ci_equals(declaration.name, "font");
                bool const is_all = ascii_ci_equals(declaration.name, "all");
                bool const is_face = ascii_ci_equals(declaration.name, "font-family")
                    || ascii_ci_equals(declaration.name, "font-weight")
                    || ascii_ci_equals(declaration.name, "font-style");
                if (is_font_size || is_font || is_all) {
                    // A CSS-wide keyword: the parent's size, or medium.
                    if (std::optional<Wide> const wide = wide_keyword(significant(declaration.value))) {
                        style.font_size = *wide == Wide::Initial ? 16.0f : parent.font_size;
                        return;
                    }
                }
                if (is_face) {
                    if (wide_keyword(significant(declaration.value)))
                        return; // the keyword pass below settles it
                    apply(style, declaration, entry.base, font_pass_top, font_pass_right,
                        font_pass_bottom, font_pass_left);
                    return;
                }
                if (is_font_size) {
                    apply_font_size(style, parent, declaration);
                } else if (is_font) {
                    // The shorthand's size goes first too; its other parts follow in apply().
                    if (std::optional<FontShorthand> const parts
                        = split_font_shorthand(significant(declaration.value))) {
                        Declaration size_only;
                        size_only.name = "font-size";
                        size_only.value.push_back(*parts->size);
                        apply_font_size(style, parent, size_only);
                        // And the family it names, for the same reason.
                        if (!parts->family.empty()) {
                            Declaration family_only;
                            family_only.name = "font-family";
                            for (ComponentValue const* value : parts->family)
                                family_only.value.push_back(*value);
                            apply(style, family_only, entry.base, font_pass_top, font_pass_right,
                                font_pass_bottom, font_pass_left);
                        }
                    }
                }
            });
        }
        own_ratios = ratios_for(style);
        // `direction` goes next, before anything that depends on it: a
        // flow-relative property is the physical one this element's own
        // direction names, so the direction has to be settled first
        // whatever order the declarations were written in. It is applied
        // again with the rest below, to the same value.
        for (MatchedDeclaration const& entry : matched) {
            with_vars(*entry.declaration, [&](Declaration const& declaration) {
                bool const is_direction = ascii_ci_equals(declaration.name, "direction");
                // `writing-mode` settles with it, and for the same reason:
                // which physical edge a flow-relative name stands for is a
                // question about both.
                if (!is_direction && !ascii_ci_equals(declaration.name, "writing-mode"))
                    return;
                if (std::optional<Wide> const wide = wide_keyword(significant(declaration.value))) {
                    if (is_direction)
                        style.direction = *wide == Wide::Initial ? Direction::Ltr : parent.direction;
                    else
                        style.writing_mode
                            = *wide == Wide::Initial ? WritingMode::HorizontalTb : parent.writing_mode;
                    return;
                }
                bool unused_border_color = false;
                apply(style, declaration, entry.base, unused_border_color, unused_border_color,
                    unused_border_color, unused_border_color);
            });
        }
        bool border_top_color_set = false;
        bool border_right_color_set = false;
        bool border_bottom_color_set = false;
        bool border_left_color_set = false;
        for (MatchedDeclaration const& entry : matched) {
            with_vars(*entry.declaration, [&](Declaration const& declaration) {
                if (std::optional<Wide> const wide = wide_keyword(significant(declaration.value))) {
                    // A flow-relative property inherits and resets the
                    // physical ones it stands for.
                    std::string const name = lowercase_name(declaration.name);
                    if (std::optional<LogicalMap> const logical
                        = logical_mapping(name, style.direction == Direction::Rtl, style.writing_mode)) {
                        for (std::string const& physical : logical->names)
                            apply_wide(style, parent, physical, *wide, border_top_color_set,
                                border_right_color_set, border_bottom_color_set, border_left_color_set);
                        return;
                    }
                    apply_wide(style, parent, name, *wide, border_top_color_set,
                        border_right_color_set, border_bottom_color_set, border_left_color_set);
                    return;
                }
                apply(style, declaration, entry.base, border_top_color_set, border_right_color_set,
                    border_bottom_color_set, border_left_color_set);
            });
        }

        // currentColor is the border default.
        style.border_top.current_color = !border_top_color_set;
        style.border_right.current_color = !border_right_color_set;
        style.border_bottom.current_color = !border_bottom_color_set;
        style.border_left.current_color = !border_left_color_set;
        if (!border_top_color_set)
            style.border_top.color = style.color;
        if (!border_right_color_set)
            style.border_right.color = style.color;
        if (!border_bottom_color_set)
            style.border_bottom.color = style.color;
        if (!border_left_color_set)
            style.border_left.color = style.color;
        // A border that draws nothing has zero used width.
        for (BorderSide* side :
            { &style.border_top, &style.border_right, &style.border_bottom, &style.border_left }) {
            if (side->style == BorderStyle::None || side->style == BorderStyle::Hidden)
                side->width = 0;
        }
        // css-text-3 §7.1: `match-parent` computes to the parent's own
        // alignment, with `start` and `end` resolved against the PARENT's
        // direction — which is the whole point of it, since inheriting the
        // keyword would resolve it against this box's direction instead.
        // The parent's value is already settled, so it is never the keyword.
        auto const parent_side = [&](bool ending) {
            bool const rtl = parent.direction == Direction::Rtl;
            return rtl == ending ? TextAlign::Left : TextAlign::Right;
        };
        // On the root there is no parent to match, and the keyword computes
        // to `start` — which the root's own direction then resolves.
        bool const root = !element.parent() || !element.parent()->is_element();
        if (root) {
            if (style.text_align == TextAlign::MatchParent)
                style.text_align = TextAlign::Start;
            if (style.text_align_last == TextAlignLast::MatchParent)
                style.text_align_last = TextAlignLast::Start;
        }
        if (style.text_align == TextAlign::MatchParent) {
            style.text_align = parent.text_align;
            if (style.text_align == TextAlign::Start)
                style.text_align = parent_side(false);
            else if (style.text_align == TextAlign::End)
                style.text_align = parent_side(true);
        }
        if (style.text_align_last == TextAlignLast::MatchParent) {
            switch (parent.text_align_last) {
            case TextAlignLast::Start:
                style.text_align_last = parent_side(false) == TextAlign::Left ? TextAlignLast::Left
                                                                              : TextAlignLast::Right;
                break;
            case TextAlignLast::End:
                style.text_align_last = parent_side(true) == TextAlign::Left ? TextAlignLast::Left
                                                                            : TextAlignLast::Right;
                break;
            case TextAlignLast::MatchParent: // settled already; cannot happen
                style.text_align_last = TextAlignLast::Auto;
                break;
            default:
                style.text_align_last = parent.text_align_last;
                break;
            }
        }
        // CSS 2.1 §9.7: an absolutely positioned box does not float, and
        // an inline-level one becomes the block-level kind of itself; its
        // static position stays where the inline box would have begun.
        if (style.out_of_flow()) {
            style.floating = Float::None;
            switch (style.display) {
            case Display::Inline:
            case Display::InlineBlock:
                style.display = Display::Block;
                style.blockified = true;
                break;
            case Display::InlineFlex:
                style.display = Display::Flex;
                style.blockified = true;
                break;
            case Display::InlineGrid:
                style.display = Display::Grid;
                style.blockified = true;
                break;
            case Display::InlineTable:
                style.display = Display::Table;
                style.blockified = true;
                break;
            case Display::TableRowGroup:
            case Display::TableHeaderGroup:
            case Display::TableFooterGroup:
            case Display::TableRow:
            case Display::TableCell:
            case Display::TableCaption:
            case Display::TableColumnGroup:
            case Display::TableColumn:
                style.display = Display::Block;
                break;
            default:
                break;
            }
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
        LengthContext const context = length_context(parent, parent_ratios);
        auto length = parse_length_percent(value, context, false);
        if (!length)
            return;
        if (length->kind == LengthPercent::Kind::Px)
            style.font_size = length->value;
        else if (length->kind == LengthPercent::Kind::Percent)
            style.font_size = parent.font_size * length->value / 100.0f;
    }

    void apply(ComputedStyle& style, Declaration const& declaration, net::Url const* base,
        bool& border_top_color_set, bool& border_right_color_set, bool& border_bottom_color_set,
        bool& border_left_color_set)
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
        // A flow-relative property is the physical one it names for this
        // element's direction, written here on the way in so that nothing
        // below the cascade has to know the difference. Doing it in
        // cascade order is what lets a `margin-left` written afterwards
        // win, and `direction` is already settled by its own pass.
        if (std::optional<LogicalMap> const logical
            = logical_mapping(name, style.direction == Direction::Rtl, style.writing_mode)) {
            // A shorthand that shares its values out takes one per edge and
            // no more; anything longer is not a value it can have.
            if (logical->split && logical->names.size() > 1 && values.size() > logical->names.size())
                return;
            for (std::size_t i = 0; i < logical->names.size(); ++i) {
                Declaration physical;
                physical.name = logical->names[i];
                physical.important = declaration.important;
                if (!logical->split || logical->names.size() == 1 || values.size() < 2) {
                    for (ComponentValue const* const value : values)
                        physical.value.push_back(*value);
                } else {
                    physical.value.push_back(*values[i]);
                }
                apply(style, physical, base, border_top_color_set, border_right_color_set,
                    border_bottom_color_set, border_left_color_set);
            }
            return;
        }
        LengthContext const context = length_context(style, own_ratios);

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

        if (name == "content") {
            // normal | none | [ <string> | attr(<name>) | open-quote | close-quote
            // | no-open-quote | no-close-quote ]+ — anything else in the list
            // (url(), counters: not written yet) leaves the declaration unapplied.
            if (values.size() == 1 && is_ident(values[0], "normal")) {
                style.content = Content {};
                return;
            }
            if (values.size() == 1 && is_ident(values[0], "none")) {
                style.content = Content {};
                style.content.kind = Content::Kind::None;
                return;
            }
            Content content;
            content.kind = Content::Kind::Items;
            for (ComponentValue const* value : values) {
                ContentItem item;
                if (value->is_token(Token::Type::String)) {
                    item.text = value->token().value;
                } else if (value->is_function() && ascii_ci_equals(value->function().name, "attr")) {
                    // attr(<name>) or attr(<name>, <fallback>): the fallback
                    // stands in for an absent attribute — a string as
                    // itself, anything else as the empty string.
                    auto const arguments = significant(value->function().values);
                    if (arguments.empty() || !arguments[0]->is_token(Token::Type::Ident))
                        return;
                    if (arguments.size() > 1) {
                        if (!arguments[1]->is_token(Token::Type::Comma) || arguments.size() > 3)
                            return;
                        if (arguments.size() == 3 && arguments[2]->is_token(Token::Type::String))
                            item.fallback = arguments[2]->token().value;
                    }
                    item.kind = ContentItem::Kind::Attr;
                    item.text = RuleSet::lowercased(arguments[0]->token().value);
                } else if (value->is_function()
                    && (ascii_ci_equals(value->function().name, "counter")
                        || ascii_ci_equals(value->function().name, "counters"))) {
                    // counter(<name> [, <counter-style>]?) and
                    // counters(<name>, <string> [, <counter-style>]?): the
                    // separator is what goes between the nested values, so
                    // only the plural form takes one and it is not optional.
                    bool const nested = ascii_ci_equals(value->function().name, "counters");
                    auto const arguments = significant(value->function().values);
                    std::size_t const wanted = nested ? 3 : 1;
                    if (arguments.size() != wanted && arguments.size() != wanted + 2)
                        return;
                    if (!arguments[0]->is_token(Token::Type::Ident))
                        return;
                    if (nested) {
                        if (!arguments[1]->is_token(Token::Type::Comma)
                            || !arguments[2]->is_token(Token::Type::String))
                            return;
                        item.fallback = arguments[2]->token().value;
                    }
                    if (arguments.size() == wanted + 2) {
                        if (!arguments[wanted]->is_token(Token::Type::Comma))
                            return;
                        auto const type = parse_list_style_type(arguments[wanted + 1]);
                        if (!type)
                            return;
                        item.style = *type;
                    }
                    item.kind = nested ? ContentItem::Kind::Counters : ContentItem::Kind::Counter;
                    item.text = arguments[0]->token().value; // counter names are case-sensitive
                } else if (is_ident(value, "open-quote")) {
                    item.kind = ContentItem::Kind::OpenQuote;
                } else if (is_ident(value, "close-quote")) {
                    item.kind = ContentItem::Kind::CloseQuote;
                } else if (is_ident(value, "no-open-quote")) {
                    item.kind = ContentItem::Kind::NoOpenQuote;
                } else if (is_ident(value, "no-close-quote")) {
                    item.kind = ContentItem::Kind::NoCloseQuote;
                } else {
                    return;
                }
                content.items.push_back(std::move(item));
            }
            style.content = std::move(content);
            return;
        }
        if (name == "counter-reset" || name == "counter-increment" || name == "counter-set") {
            // none | [ <custom-ident> <integer>? ]+ — the integer left out
            // means one for counter-increment and nothing for the other two.
            // A name the list cannot hold (none, or a CSS-wide keyword, which
            // never reaches here) drops the whole declaration, as does a
            // stray value: the property is all-or-nothing.
            auto const list = std::make_shared<CounterOps>();
            if (!(values.size() == 1 && is_ident(values[0], "none"))) {
                int const written = name == "counter-increment" ? 1 : 0;
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if (!values[i]->is_token(Token::Type::Ident))
                        return;
                    std::string const& counter = values[i]->token().value;
                    if (ascii_ci_equals(counter, "none") || ascii_ci_equals(counter, "inherit")
                        || ascii_ci_equals(counter, "initial") || ascii_ci_equals(counter, "unset")
                        || ascii_ci_equals(counter, "revert") || ascii_ci_equals(counter, "default"))
                        return;
                    int value = written;
                    if (i + 1 < values.size() && values[i + 1]->is_token(Token::Type::Number)) {
                        auto const integer = parse_counter_integer(values[i + 1]);
                        if (!integer)
                            return;
                        value = *integer;
                        ++i;
                    }
                    list->push_back(CounterOp { counter, value });
                }
                if (list->empty())
                    return;
            }
            if (name == "counter-reset")
                style.counter_reset = list;
            else if (name == "counter-increment")
                style.counter_increment = list;
            else
                style.counter_set = list;
            return;
        }
        if (name == "quotes") {
            // none | auto | [ <open-string> <close-string> ]+
            if (values.size() == 1 && is_ident(values[0], "none")) {
                style.quotes = std::make_shared<QuotePairs const>();
                return;
            }
            if (values.size() == 1 && is_ident(values[0], "auto")) {
                style.quotes = nullptr;
                return;
            }
            if (values.size() % 2 != 0)
                return;
            QuotePairs pairs;
            for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
                if (!values[i]->is_token(Token::Type::String) || !values[i + 1]->is_token(Token::Type::String))
                    return;
                pairs.emplace_back(values[i]->token().value, values[i + 1]->token().value);
            }
            style.quotes = std::make_shared<QuotePairs const>(std::move(pairs));
            return;
        }
        if (name == "display") {
            // One keyword, or the two-value form: an outer type (block or
            // inline) with an inner one (flow, flow-root, flex, grid, table).
            if (values.empty() || values.size() > 2)
                return;
            for (ComponentValue const* value : values) {
                if (!value->is_token(Token::Type::Ident))
                    return;
            }
            std::string_view keyword = values[0]->token().value;
            if (values.size() == 2) {
                std::string_view outer = values[0]->token().value;
                std::string_view inner = values[1]->token().value;
                // Either order is allowed.
                if (ascii_ci_equals(inner, "block") || ascii_ci_equals(inner, "inline"))
                    std::swap(outer, inner);
                bool const inline_outer = ascii_ci_equals(outer, "inline");
                if (!inline_outer && !ascii_ci_equals(outer, "block"))
                    return;
                if (ascii_ci_equals(inner, "flow"))
                    keyword = inline_outer ? "inline" : "block";
                else if (ascii_ci_equals(inner, "flow-root"))
                    keyword = inline_outer ? "inline-block" : "flow-root";
                else if (ascii_ci_equals(inner, "flex"))
                    keyword = inline_outer ? "inline-flex" : "flex";
                else if (ascii_ci_equals(inner, "grid"))
                    keyword = inline_outer ? "inline-grid" : "grid";
                else if (ascii_ci_equals(inner, "table"))
                    keyword = inline_outer ? "inline-table" : "table";
                else
                    return;
            }
            if (ascii_ci_equals(keyword, "none"))
                style.display = Display::None;
            else if (ascii_ci_equals(keyword, "inline"))
                style.display = Display::Inline;
            else if (ascii_ci_equals(keyword, "list-item"))
                style.display = Display::ListItem;
            else if (ascii_ci_equals(keyword, "flow-root"))
                style.display = Display::FlowRoot;
            else if (ascii_ci_equals(keyword, "flex"))
                style.display = Display::Flex;
            else if (ascii_ci_equals(keyword, "inline-flex"))
                style.display = Display::InlineFlex;
            else if (ascii_ci_equals(keyword, "block"))
                style.display = Display::Block;
            else if (ascii_ci_equals(keyword, "grid"))
                // A grid container lays out as a block until grid lands, but
                // as a formatting context root whose items are roots too:
                // no margin passes through it or them, as the real thing has it.
                style.display = Display::Grid;
            else if (ascii_ci_equals(keyword, "inline-grid"))
                style.display = Display::InlineGrid;
            else if (ascii_ci_equals(keyword, "table"))
                style.display = Display::Table;
            else if (ascii_ci_equals(keyword, "inline-table"))
                style.display = Display::InlineTable;
            else if (ascii_ci_equals(keyword, "inline-block"))
                style.display = Display::InlineBlock;
            else if (ascii_ci_equals(keyword, "table-cell"))
                style.display = Display::TableCell;
            else if (ascii_ci_equals(keyword, "table-caption"))
                style.display = Display::TableCaption;
            else if (ascii_ci_equals(keyword, "table-column"))
                style.display = Display::TableColumn;
            else if (ascii_ci_equals(keyword, "table-column-group"))
                style.display = Display::TableColumnGroup;
            else if (ascii_ci_equals(keyword, "table-row-group"))
                style.display = Display::TableRowGroup;
            else if (ascii_ci_equals(keyword, "table-header-group"))
                style.display = Display::TableHeaderGroup;
            else if (ascii_ci_equals(keyword, "table-footer-group"))
                style.display = Display::TableFooterGroup;
            else if (ascii_ci_equals(keyword, "table-row"))
                style.display = Display::TableRow;
            if (is_table_internal(style.display))
                style.overflow_applies = false;
            return;
        }
        if (name == "transform" || name == "translate") {
            // The translations add up; other functions are passed over
            // (the box stays put, unrotated and unscaled). `none` clears.
            if (values.size() == 1 && is_ident(values[0], "none")) {
                style.translate_x = LengthPercent::px(0);
                style.translate_y = LengthPercent::px(0);
                style.transformed = false;
                return;
            }
            float px_x = 0;
            float px_y = 0;
            float percent_x = 0;
            float percent_y = 0;
            auto const add = [&](LengthPercent const& length, bool horizontal) {
                float& px = horizontal ? px_x : px_y;
                float& percent = horizontal ? percent_x : percent_y;
                if (length.kind == LengthPercent::Kind::Percent)
                    percent += length.value;
                else if (length.kind == LengthPercent::Kind::Calc) {
                    px += length.value;
                    percent += length.percent;
                } else
                    px += length.value;
            };
            bool any = false;
            if (name == "translate") {
                // translate: <x> [<y>]
                std::optional<LengthPercent> const x = parse_length_percent(*values[0], context, false);
                if (!x)
                    return;
                add(*x, true);
                if (values.size() > 1) {
                    std::optional<LengthPercent> const y = parse_length_percent(*values[1], context, false);
                    if (!y)
                        return;
                    add(*y, false);
                }
                any = true;
            } else {
                for (ComponentValue const* value : values) {
                    if (!value->is_function())
                        return;
                    std::string const function = lowercase_name(value->function().name);
                    auto const arguments = significant(value->function().values);
                    std::vector<ComponentValue const*> parts;
                    for (ComponentValue const* argument : arguments) {
                        if (!argument->is_token(Token::Type::Comma))
                            parts.push_back(argument);
                    }
                    if (function == "translate" || function == "translate3d") {
                        if (parts.empty())
                            return;
                        std::optional<LengthPercent> const x = parse_length_percent(*parts[0], context, false);
                        if (!x)
                            return;
                        add(*x, true);
                        if (parts.size() > 1) {
                            std::optional<LengthPercent> const y = parse_length_percent(*parts[1], context, false);
                            if (!y)
                                return;
                            add(*y, false);
                        }
                    } else if (function == "translatex" || function == "translatey") {
                        if (parts.size() != 1)
                            return;
                        std::optional<LengthPercent> const amount = parse_length_percent(*parts[0], context, false);
                        if (!amount)
                            return;
                        add(*amount, function == "translatex");
                    }
                    any = true;
                }
            }
            if (!any)
                return;
            style.translate_x = LengthPercent::calc(px_x, percent_x);
            style.translate_y = LengthPercent::calc(px_y, percent_y);
            style.transformed = true;
            return;
        }
        if (name == "visibility") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "visible"))
                style.visibility = Visibility::Visible;
            else if (ascii_ci_equals(keyword, "hidden") || ascii_ci_equals(keyword, "collapse"))
                style.visibility = Visibility::Hidden;
            return;
        }
        if (name == "opacity") {
            if (values.size() != 1 || !values[0]->is_token())
                return;
            Token const& token = values[0]->token();
            double amount;
            if (token.type == Token::Type::Number)
                amount = token.numeric_value;
            else if (token.type == Token::Type::Percentage)
                amount = token.numeric_value / 100.0;
            else
                return;
            style.opacity = static_cast<float>(std::clamp(amount, 0.0, 1.0));
            return;
        }
        if (name == "position") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "static"))
                style.position = Position::Static;
            else if (ascii_ci_equals(keyword, "relative"))
                style.position = Position::Relative;
            else if (ascii_ci_equals(keyword, "absolute"))
                style.position = Position::Absolute;
            else if (ascii_ci_equals(keyword, "fixed"))
                style.position = Position::Fixed;
            else if (ascii_ci_equals(keyword, "sticky") || ascii_ci_equals(keyword, "-webkit-sticky"))
                style.position = Position::Sticky;
            return;
        }
        if (name == "top" || name == "right" || name == "bottom" || name == "left") {
            LengthPercent& side = name == "top" ? style.top
                : name == "right"               ? style.right
                : name == "bottom"              ? style.bottom
                                                : style.left;
            one_length(side, true);
            return;
        }
        if (name == "inset") {
            four_lengths(style.top, style.right, style.bottom, style.left, true);
            return;
        }
        if (name == "z-index") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "auto")) {
                style.z_index = std::nullopt;
            } else if (values[0]->is_token(Token::Type::Number)) {
                Token const& token = values[0]->token();
                if (token.numeric_type == Token::NumericType::Integer)
                    style.z_index = static_cast<int>(token.numeric_value);
            }
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
        if (name == "box-sizing") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "border-box"))
                style.box_sizing = BoxSizing::BorderBox;
            else if (ascii_ci_equals(keyword, "content-box"))
                style.box_sizing = BoxSizing::ContentBox;
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
            // One keyword, or one per axis. Any axis that is not visible
            // makes the box contain its floats and clip what it paints.
            if (values.empty() || values.size() > 2)
                return;
            std::vector<Overflow> written;
            for (ComponentValue const* value : values) {
                if (!value->is_token(Token::Type::Ident))
                    return;
                std::string_view const keyword = value->token().value;
                if (ascii_ci_equals(keyword, "visible"))
                    written.push_back(Overflow::Visible);
                else if (ascii_ci_equals(keyword, "clip"))
                    written.push_back(Overflow::Clip);
                else if (ascii_ci_equals(keyword, "hidden") || ascii_ci_equals(keyword, "auto")
                    || ascii_ci_equals(keyword, "scroll")
                    || ascii_ci_equals(keyword, "overlay")) // the legacy spelling of auto
                    written.push_back(Overflow::Hidden);
                else
                    return;
            }
            if (name == "overflow") {
                style.overflow_x = written[0];
                style.overflow_y = written.size() > 1 ? written[1] : written[0];
            } else if (written.size() != 1) {
                return; // a single axis takes a single keyword
            } else if (name == "overflow-x") {
                style.overflow_x = written[0];
            } else {
                style.overflow_y = written[0];
            }
            settle_overflow(style);
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
        // The alignment keywords, with an optional safe/unsafe or
        // first/last word in front (a position keyword's overflow safety,
        // a baseline's choice — neither changes where a box goes yet).
        auto const alignment_words = [&](std::size_t first, std::size_t count) -> std::optional<std::string_view> {
            if (count == 0 || count > 2)
                return std::nullopt;
            for (std::size_t i = first; i < first + count; ++i) {
                if (!values[i]->is_token(Token::Type::Ident))
                    return std::nullopt;
            }
            if (count == 2) {
                std::string_view const prefix = values[first]->token().value;
                if (!ascii_ci_equals(prefix, "safe") && !ascii_ci_equals(prefix, "unsafe")
                    && !ascii_ci_equals(prefix, "first") && !ascii_ci_equals(prefix, "last"))
                    return std::nullopt;
                return values[first + 1]->token().value;
            }
            return values[first]->token().value;
        };
        auto const justify_content_of = [](std::string_view keyword) -> std::optional<JustifyContent> {
            if (ascii_ci_equals(keyword, "normal"))
                return JustifyContent::Normal;
            if (ascii_ci_equals(keyword, "stretch"))
                return JustifyContent::Stretch;
            if (ascii_ci_equals(keyword, "flex-start") || ascii_ci_equals(keyword, "start")
                || ascii_ci_equals(keyword, "left"))
                return JustifyContent::FlexStart;
            if (ascii_ci_equals(keyword, "flex-end") || ascii_ci_equals(keyword, "end")
                || ascii_ci_equals(keyword, "right"))
                return JustifyContent::FlexEnd;
            if (ascii_ci_equals(keyword, "center"))
                return JustifyContent::Center;
            if (ascii_ci_equals(keyword, "space-between"))
                return JustifyContent::SpaceBetween;
            if (ascii_ci_equals(keyword, "space-around"))
                return JustifyContent::SpaceAround;
            if (ascii_ci_equals(keyword, "space-evenly"))
                return JustifyContent::SpaceEvenly;
            return std::nullopt;
        };
        auto const align_content_of = [](std::string_view keyword) -> std::optional<AlignContent> {
            if (ascii_ci_equals(keyword, "stretch") || ascii_ci_equals(keyword, "normal"))
                return AlignContent::Stretch;
            if (ascii_ci_equals(keyword, "flex-start") || ascii_ci_equals(keyword, "start"))
                return AlignContent::FlexStart;
            if (ascii_ci_equals(keyword, "flex-end") || ascii_ci_equals(keyword, "end"))
                return AlignContent::FlexEnd;
            if (ascii_ci_equals(keyword, "center"))
                return AlignContent::Center;
            if (ascii_ci_equals(keyword, "space-between"))
                return AlignContent::SpaceBetween;
            if (ascii_ci_equals(keyword, "space-around"))
                return AlignContent::SpaceAround;
            if (ascii_ci_equals(keyword, "space-evenly"))
                return AlignContent::SpaceEvenly;
            return std::nullopt;
        };
        if (name == "justify-content") {
            std::optional<std::string_view> const keyword = alignment_words(0, values.size());
            if (!keyword)
                return;
            if (std::optional<JustifyContent> const justify = justify_content_of(*keyword))
                style.justify_content = *justify;
            return;
        }
        auto const alignment_of = [](std::string_view keyword) -> std::optional<AlignItems> {
            if (ascii_ci_equals(keyword, "normal"))
                return AlignItems::Normal;
            if (ascii_ci_equals(keyword, "stretch"))
                return AlignItems::Stretch;
            if (ascii_ci_equals(keyword, "flex-start") || ascii_ci_equals(keyword, "start")
                || ascii_ci_equals(keyword, "self-start") || ascii_ci_equals(keyword, "left"))
                return AlignItems::FlexStart;
            if (ascii_ci_equals(keyword, "flex-end") || ascii_ci_equals(keyword, "end")
                || ascii_ci_equals(keyword, "self-end") || ascii_ci_equals(keyword, "right"))
                return AlignItems::FlexEnd;
            if (ascii_ci_equals(keyword, "center"))
                return AlignItems::Center;
            if (ascii_ci_equals(keyword, "baseline"))
                return AlignItems::Baseline;
            return std::nullopt;
        };
        // *-items takes an alignment; *-self takes auto or an alignment.
        auto const items_of = [&](std::string_view keyword) -> std::optional<AlignItems> {
            if (ascii_ci_equals(keyword, "auto"))
                return std::nullopt;
            return alignment_of(keyword);
        };
        auto const self_of = [&](std::string_view keyword) -> std::optional<AlignItems> {
            if (ascii_ci_equals(keyword, "auto"))
                return AlignItems::Auto;
            return alignment_of(keyword);
        };
        if (name == "align-items" || name == "justify-items" || name == "align-self"
            || name == "justify-self") {
            std::optional<std::string_view> const keyword = alignment_words(0, values.size());
            if (!keyword)
                return;
            bool const self = name == "align-self" || name == "justify-self";
            std::optional<AlignItems> const alignment = self ? self_of(*keyword) : items_of(*keyword);
            if (!alignment)
                return;
            if (name == "align-items")
                style.align_items = *alignment;
            else if (name == "justify-items")
                style.justify_items = *alignment;
            else if (name == "align-self")
                style.align_self = *alignment;
            else
                style.justify_self = *alignment;
            return;
        }
        if (name == "place-items" || name == "place-self") {
            // The block-axis value, then the inline-axis one; one value
            // stands for both. Each may carry a prefix word.
            bool const self = name == "place-self";
            std::optional<std::string_view> first;
            std::optional<std::string_view> second;
            if (values.size() <= 2)
                first = alignment_words(0, values.size());
            if (first) {
                second = first;
            } else {
                for (std::size_t split = 1; split < values.size() && !first; ++split) {
                    first = alignment_words(0, split);
                    second = alignment_words(split, values.size() - split);
                    if (!second)
                        first = std::nullopt;
                }
            }
            if (!first || !second)
                return;
            std::optional<AlignItems> const block = self ? self_of(*first) : items_of(*first);
            std::optional<AlignItems> const inline_axis = self ? self_of(*second) : items_of(*second);
            if (!block || !inline_axis)
                return;
            if (self) {
                style.align_self = *block;
                style.justify_self = *inline_axis;
            } else {
                style.align_items = *block;
                style.justify_items = *inline_axis;
            }
            return;
        }
        if (name == "place-content") {
            std::optional<std::string_view> first;
            std::optional<std::string_view> second;
            if (values.size() <= 2)
                first = alignment_words(0, values.size());
            if (first) {
                second = first;
            } else {
                for (std::size_t split = 1; split < values.size() && !first; ++split) {
                    first = alignment_words(0, split);
                    second = alignment_words(split, values.size() - split);
                    if (!second)
                        first = std::nullopt;
                }
            }
            if (!first || !second)
                return;
            std::optional<AlignContent> const align = align_content_of(*first);
            std::optional<JustifyContent> const justify = justify_content_of(*second);
            if (!align || !justify)
                return;
            style.align_content = *align;
            style.justify_content = *justify;
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
            // `content` sizes from what is inside the item and ignores the
            // main size property, which is what `auto` falls back to: with
            // no ratio and no cross size binding, that is max-content.
            if (is_ident(&value, "content"))
                return LengthPercent::content(LengthPercent::Kind::MaxContent);
            if (is_ident(&value, "min-content"))
                return LengthPercent::content(LengthPercent::Kind::MinContent);
            if (is_ident(&value, "max-content"))
                return LengthPercent::content(LengthPercent::Kind::MaxContent);
            if (is_ident(&value, "fit-content"))
                return LengthPercent::content(LengthPercent::Kind::FitContent);
            auto length = parse_length_percent(value, context, true);
            if (!length || (!length->is_auto() && length->kind != LengthPercent::Kind::Calc && length->value < 0))
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
                    style.flex_basis = ascii_ci_equals(keyword, "content")
                        ? LengthPercent::content(LengthPercent::Kind::MaxContent)
                        : LengthPercent::auto_value();
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
        if (name == "gap" || name == "row-gap" || name == "column-gap" || name == "grid-gap"
            || name == "grid-row-gap" || name == "grid-column-gap") {
            // The grid-prefixed names are the old spellings of the same properties.
            auto const gap_of = [&](ComponentValue const& value) -> std::optional<LengthPercent> {
                if (is_ident(&value, "normal"))
                    return LengthPercent::px(0);
                auto length = parse_length_percent(value, context, false);
                if (!length)
                    return std::nullopt;
                if ((length->kind == LengthPercent::Kind::Px || length->kind == LengthPercent::Kind::Percent)
                    && length->value < 0)
                    return std::nullopt;
                return length;
            };
            if (name == "gap" || name == "grid-gap") {
                if (values.size() > 2)
                    return;
                std::optional<LengthPercent> const row = gap_of(*values[0]);
                std::optional<LengthPercent> const column = values.size() == 2 ? gap_of(*values[1]) : row;
                if (!row || !column)
                    return;
                style.row_gap = *row;
                style.column_gap = *column;
                return;
            }
            if (values.size() != 1)
                return;
            if (std::optional<LengthPercent> const gap = gap_of(*values[0]))
                (name == "row-gap" || name == "grid-row-gap" ? style.row_gap : style.column_gap) = *gap;
            return;
        }
        // --- Grid containers and items --------------------------------------
        auto const length_parser = [&](ComponentValue const& value) {
            return parse_length_percent(value, context, false);
        };
        if (name == "grid-template-columns" || name == "grid-template-rows") {
            std::optional<GridTrackList> list = parse_track_list(values, length_parser);
            if (!list)
                return;
            std::shared_ptr<GridTrackList const> shared;
            if (!list->empty())
                shared = std::make_shared<GridTrackList const>(std::move(*list));
            (name == "grid-template-columns" ? style.grid_template_columns : style.grid_template_rows)
                = std::move(shared);
            return;
        }
        if (name == "grid-template-areas") {
            std::optional<GridAreas> areas = parse_grid_template_areas(values);
            if (!areas)
                return;
            style.grid_template_areas
                = areas->rows == 0 ? nullptr : std::make_shared<GridAreas const>(std::move(*areas));
            return;
        }
        if (name == "grid-auto-columns" || name == "grid-auto-rows") {
            std::optional<std::vector<TrackSize>> sizes = parse_track_sizes(values, length_parser);
            if (!sizes)
                return;
            // A lone auto is the initial value.
            bool const plain_auto = sizes->size() == 1 && sizes->front().min.kind == TrackBreadth::Kind::Auto
                && sizes->front().max.kind == TrackBreadth::Kind::Auto && !sizes->front().fit_content;
            std::shared_ptr<std::vector<TrackSize> const> shared;
            if (!plain_auto)
                shared = std::make_shared<std::vector<TrackSize> const>(std::move(*sizes));
            (name == "grid-auto-columns" ? style.grid_auto_columns : style.grid_auto_rows) = std::move(shared);
            return;
        }
        if (name == "grid-auto-flow") {
            if (std::optional<GridAutoFlow> const flow = parse_grid_auto_flow(values))
                style.grid_auto_flow = *flow;
            return;
        }
        if (name == "grid-template" || name == "grid") {
            std::optional<GridShorthand> const parsed = name == "grid"
                ? parse_grid_shorthand(values, length_parser)
                : parse_grid_template(values, length_parser);
            if (!parsed)
                return;
            style.grid_template_rows = parsed->rows;
            style.grid_template_columns = parsed->columns;
            style.grid_template_areas = parsed->areas;
            if (name == "grid") {
                style.grid_auto_flow = parsed->auto_flow;
                style.grid_auto_rows = parsed->auto_rows;
                style.grid_auto_columns = parsed->auto_columns;
            }
            return;
        }
        if (name == "grid-row-start" || name == "grid-row-end" || name == "grid-column-start"
            || name == "grid-column-end") {
            std::optional<GridLine> line = parse_grid_line(values);
            if (!line)
                return;
            if (name == "grid-row-start")
                style.grid_row_start = std::move(*line);
            else if (name == "grid-row-end")
                style.grid_row_end = std::move(*line);
            else if (name == "grid-column-start")
                style.grid_column_start = std::move(*line);
            else
                style.grid_column_end = std::move(*line);
            return;
        }
        if (name == "grid-row" || name == "grid-column") {
            std::optional<std::pair<GridLine, GridLine>> lines = parse_grid_line_pair(values);
            if (!lines)
                return;
            if (name == "grid-row") {
                style.grid_row_start = std::move(lines->first);
                style.grid_row_end = std::move(lines->second);
            } else {
                style.grid_column_start = std::move(lines->first);
                style.grid_column_end = std::move(lines->second);
            }
            return;
        }
        if (name == "grid-area") {
            std::optional<std::array<GridLine, 4>> lines = parse_grid_area(values);
            if (!lines)
                return;
            style.grid_row_start = std::move((*lines)[0]);
            style.grid_column_start = std::move((*lines)[1]);
            style.grid_row_end = std::move((*lines)[2]);
            style.grid_column_end = std::move((*lines)[3]);
            return;
        }
        // --- Tables -------------------------------------------------------
        if (name == "border-collapse") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "collapse"))
                style.border_collapse = BorderCollapse::Collapse;
            else if (ascii_ci_equals(keyword, "separate"))
                style.border_collapse = BorderCollapse::Separate;
            return;
        }
        if (name == "border-spacing") {
            // One or two lengths, never negative, never a percentage.
            if (values.empty() || values.size() > 2)
                return;
            std::optional<LengthPercent> const horizontal = parse_length_percent(*values[0], context, false, false);
            std::optional<LengthPercent> const vertical
                = values.size() == 2 ? parse_length_percent(*values[1], context, false, false) : horizontal;
            if (!horizontal || !vertical || horizontal->value < 0 || vertical->value < 0)
                return;
            style.border_spacing_horizontal = *horizontal;
            style.border_spacing_vertical = *vertical;
            return;
        }
        if (name == "caption-side") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "top"))
                style.caption_side = CaptionSide::Top;
            else if (ascii_ci_equals(keyword, "bottom"))
                style.caption_side = CaptionSide::Bottom;
            return;
        }
        if (name == "empty-cells") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "show"))
                style.empty_cells = EmptyCells::Show;
            else if (ascii_ci_equals(keyword, "hide"))
                style.empty_cells = EmptyCells::Hide;
            return;
        }
        if (name == "table-layout") {
            if (values.size() != 1 || !values[0]->is_token(Token::Type::Ident))
                return;
            std::string_view const keyword = values[0]->token().value;
            if (ascii_ci_equals(keyword, "auto"))
                style.table_layout = TableLayout::Auto;
            else if (ascii_ci_equals(keyword, "fixed"))
                style.table_layout = TableLayout::Fixed;
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
        // --- Backgrounds: one value per layer, comma-separated ------------------
        if (name == "background-image") {
            std::vector<BackgroundImage> images;
            for (std::vector<ComponentValue const*> const& layer : split_commas(values)) {
                if (layer.size() != 1)
                    return;
                std::optional<BackgroundImage> image = parse_background_image(*layer[0], context, style.color, base);
                if (!image)
                    return;
                images.push_back(std::move(*image));
            }
            style.background_images = all_none(images)
                ? nullptr
                : std::make_shared<std::vector<BackgroundImage> const>(std::move(images));
            return;
        }
        if (name == "background-repeat") {
            std::vector<BackgroundRepeatPair> repeats;
            for (std::vector<ComponentValue const*> const& layer : split_commas(values)) {
                std::size_t taken = 0;
                std::optional<BackgroundRepeatPair> const repeat = parse_background_repeat(layer, 0, taken);
                if (!repeat || taken != layer.size())
                    return;
                repeats.push_back(*repeat);
            }
            style.background_repeats = std::make_shared<std::vector<BackgroundRepeatPair> const>(std::move(repeats));
            return;
        }
        if (name == "background-position") {
            std::vector<BackgroundPosition> positions;
            for (std::vector<ComponentValue const*> const& layer : split_commas(values)) {
                std::size_t taken = 0;
                std::optional<BackgroundPosition> const position
                    = parse_background_position(layer, 0, context, taken);
                if (!position || taken != layer.size())
                    return;
                positions.push_back(*position);
            }
            style.background_positions
                = std::make_shared<std::vector<BackgroundPosition> const>(std::move(positions));
            return;
        }
        if (name == "background-size") {
            std::vector<BackgroundSize> sizes;
            for (std::vector<ComponentValue const*> const& layer : split_commas(values)) {
                std::size_t taken = 0;
                std::optional<BackgroundSize> const size = parse_background_size(layer, 0, context, taken);
                if (!size || taken != layer.size())
                    return;
                sizes.push_back(*size);
            }
            style.background_sizes = std::make_shared<std::vector<BackgroundSize> const>(std::move(sizes));
            return;
        }
        if (name == "background-origin" || name == "background-clip") {
            std::vector<BackgroundBox> boxes;
            for (std::vector<ComponentValue const*> const& layer : split_commas(values)) {
                if (layer.size() != 1)
                    return;
                std::optional<BackgroundBox> const box = parse_background_box(*layer[0]);
                if (!box)
                    return;
                boxes.push_back(*box);
            }
            auto shared = std::make_shared<std::vector<BackgroundBox> const>(std::move(boxes));
            (name == "background-origin" ? style.background_origins : style.background_clips) = std::move(shared);
            return;
        }
        if (name == "background") {
            // Every layer's parts in any order: an image, a position (with a
            // size after a slash), the repeat, the boxes (one for both, or
            // origin then clip), an attachment (passed over), and in the
            // last layer the color. The shorthand resets what it does not set.
            std::vector<BackgroundImage> images;
            std::vector<BackgroundRepeatPair> repeats;
            std::vector<BackgroundPosition> positions;
            std::vector<BackgroundSize> sizes;
            std::vector<BackgroundBox> origins;
            std::vector<BackgroundBox> clips;
            Color color = Color::rgba(0, 0, 0, 0);
            std::vector<std::vector<ComponentValue const*>> const layers = split_commas(values);
            for (std::size_t l = 0; l < layers.size(); ++l) {
                std::vector<ComponentValue const*> const& layer = layers[l];
                BackgroundImage image;
                BackgroundRepeatPair repeat;
                BackgroundPosition position;
                BackgroundSize size;
                std::optional<BackgroundBox> origin;
                std::optional<BackgroundBox> clip;
                bool seen_image = false;
                bool seen_position = false;
                bool seen_repeat = false;
                bool seen_color = false;
                std::size_t i = 0;
                while (i < layer.size()) {
                    ComponentValue const& value = *layer[i];
                    std::size_t taken = 0;
                    if (value.is_token(Token::Type::Delim) && value.token().delim == U'/') {
                        if (!seen_position)
                            return;
                        std::optional<BackgroundSize> const parsed
                            = parse_background_size(layer, i + 1, context, taken);
                        if (!parsed || taken == 0)
                            return;
                        size = *parsed;
                        i += 1 + taken;
                        continue;
                    }
                    if (is_ident(&value, "scroll") || is_ident(&value, "fixed") || is_ident(&value, "local")) {
                        ++i;
                        continue;
                    }
                    if (!seen_repeat) {
                        if (std::optional<BackgroundRepeatPair> const parsed = parse_background_repeat(layer, i, taken)) {
                            repeat = *parsed;
                            seen_repeat = true;
                            i += taken;
                            continue;
                        }
                    }
                    if (std::optional<BackgroundBox> const box = parse_background_box(value)) {
                        if (!origin) {
                            origin = box;
                            clip = box;
                        } else {
                            clip = box;
                        }
                        ++i;
                        continue;
                    }
                    if (!seen_image) {
                        if (std::optional<BackgroundImage> parsed
                            = parse_background_image(value, context, style.color, base)) {
                            image = std::move(*parsed);
                            seen_image = true;
                            ++i;
                            continue;
                        }
                    }
                    if (!seen_position) {
                        if (std::optional<BackgroundPosition> const parsed
                            = parse_background_position(layer, i, context, taken)) {
                            position = *parsed;
                            seen_position = true;
                            i += taken;
                            continue;
                        }
                    }
                    if (!seen_color) {
                        if (std::optional<Color> const parsed = parse_color_component(value, style.color)) {
                            if (l + 1 != layers.size())
                                return; // a color belongs to the last layer alone
                            color = *parsed;
                            seen_color = true;
                            ++i;
                            continue;
                        }
                    }
                    return;
                }
                images.push_back(std::move(image));
                repeats.push_back(repeat);
                positions.push_back(position);
                sizes.push_back(size);
                origins.push_back(origin.value_or(BackgroundBox::PaddingBox));
                clips.push_back(clip.value_or(BackgroundBox::BorderBox));
            }
            style.background_color = color;
            style.background_images = all_none(images)
                ? nullptr
                : std::make_shared<std::vector<BackgroundImage> const>(std::move(images));
            style.background_repeats = std::make_shared<std::vector<BackgroundRepeatPair> const>(std::move(repeats));
            style.background_positions
                = std::make_shared<std::vector<BackgroundPosition> const>(std::move(positions));
            style.background_sizes = std::make_shared<std::vector<BackgroundSize> const>(std::move(sizes));
            style.background_origins = std::make_shared<std::vector<BackgroundBox> const>(std::move(origins));
            style.background_clips = std::make_shared<std::vector<BackgroundBox> const>(std::move(clips));
            return;
        }
        if (name == "width" || name == "height" || name == "min-width" || name == "min-height"
            || name == "max-width" || name == "max-height") {
            // A size is never negative: such a declaration is invalid and
            // leaves the property as it was. none is a maximum's auto.
            LengthPercent& target = name == "width" ? style.width
                : name == "height"                  ? style.height
                : name == "min-width"               ? style.min_width
                : name == "min-height"              ? style.min_height
                : name == "max-width"               ? style.max_width
                                                    : style.max_height;
            bool const maximum = name == "max-width" || name == "max-height";
            if (maximum && values.size() == 1 && is_ident(values[0], "none")) {
                target = LengthPercent::auto_value();
                return;
            }
            // The content-based keywords of css-sizing-3 §5.
            if (values.size() == 1) {
                if (is_ident(values[0], "min-content")) {
                    target = LengthPercent::content(LengthPercent::Kind::MinContent);
                    return;
                }
                if (is_ident(values[0], "max-content")) {
                    target = LengthPercent::content(LengthPercent::Kind::MaxContent);
                    return;
                }
                if (is_ident(values[0], "fit-content")) {
                    target = LengthPercent::content(LengthPercent::Kind::FitContent);
                    return;
                }
            }
            LengthPercent const previous = target;
            one_length(target, !maximum);
            if (!target.is_auto() && target.kind != LengthPercent::Kind::Calc && target.value < 0)
                target = previous;
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
        if (name == "font") {
            // The shorthand: [style || variant || weight] size [/ line-height]
            // family. The size went first (apply_font_size); the rest resets
            // to normal and takes what is written, all or nothing.
            std::optional<FontShorthand> const parts = split_font_shorthand(values);
            if (!parts)
                return;
            std::shared_ptr<std::vector<std::string>> families = parse_family_list(parts->family);
            if (!families)
                return;
            FontStyle font_style = FontStyle::Normal;
            int weight = 400;
            for (ComponentValue const* value : parts->before) {
                if (is_ident(value, "italic") || is_ident(value, "oblique"))
                    font_style = FontStyle::Italic;
                else if (is_ident(value, "bold"))
                    weight = 700;
                else if (is_ident(value, "bolder"))
                    weight = std::min(900, style.font_weight + 300);
                else if (is_ident(value, "lighter"))
                    weight = std::max(100, style.font_weight - 300);
                else if (value->is_token(Token::Type::Number)
                    && value->token().numeric_value >= 1 && value->token().numeric_value <= 1000)
                    weight = static_cast<int>(value->token().numeric_value);
                else if (!is_ident(value, "normal") && !is_ident(value, "small-caps"))
                    return;
            }
            LineHeight line_height;
            if (ComponentValue const* const height = parts->line_height) {
                if (is_ident(height, "normal")) {
                    line_height = LineHeight {};
                } else if (height->is_token(Token::Type::Number)) {
                    if (height->token().numeric_value < 0)
                        return;
                    line_height = LineHeight { LineHeight::Kind::Number,
                        static_cast<float>(height->token().numeric_value) };
                } else if (auto length = parse_length_percent(*height, context, false)) {
                    line_height = LineHeight { LineHeight::Kind::Px,
                        length->kind == LengthPercent::Kind::Percent
                            ? style.font_size * length->value / 100.0f
                            : length->value };
                } else {
                    return;
                }
            }
            style.font_style = font_style;
            style.font_weight = weight;
            style.line_height = line_height;
            style.font_family = std::move(families);
            return;
        }
        if (name == "font-family") {
            if (std::shared_ptr<std::vector<std::string>> families = parse_family_list(values))
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
        // The spacing pair: `normal` asks for no extra room, anything else
        // is a length. Neither takes a percentage or a bare number.
        if (name == "letter-spacing" || name == "word-spacing") {
            if (values.size() != 1)
                return;
            float spacing = 0;
            if (!is_ident(values[0], "normal")) {
                std::optional<LengthPercent> const length
                    = parse_length_percent(*values[0], context, false, false);
                if (!length || length->kind != LengthPercent::Kind::Px)
                    return;
                spacing = length->value;
            }
            if (name == "letter-spacing")
                style.letter_spacing = spacing;
            else
                style.word_spacing = spacing;
            return;
        }
        if (name == "text-indent") {
            // css-text-3's `each-line` and `hanging` are not written, and a
            // keyword beside the length drops the declaration.
            if (values.size() != 1)
                return;
            if (std::optional<LengthPercent> const length
                = parse_length_percent(*values[0], context, false))
                style.text_indent = *length;
            return;
        }
        if (name == "text-align") {
            if (values.size() != 1)
                return;
            // The shorthand sets both longhands: every value but
            // `justify-all` leaves the last line to `text-align-last: auto`,
            // and a `text-align-last` declared after this one still wins.
            // `match-parent` needs a direction to resolve start and end
            // against, so it is not read.
            if (is_ident(values[0], "start"))
                style.text_align = TextAlign::Start;
            else if (is_ident(values[0], "end"))
                style.text_align = TextAlign::End;
            else if (is_ident(values[0], "match-parent"))
                style.text_align = TextAlign::MatchParent;
            else if (is_ident(values[0], "left"))
                style.text_align = TextAlign::Left;
            else if (is_ident(values[0], "right"))
                style.text_align = TextAlign::Right;
            else if (is_ident(values[0], "center"))
                style.text_align = TextAlign::Center;
            else if (is_ident(values[0], "justify"))
                style.text_align = TextAlign::Justify;
            else if (is_ident(values[0], "justify-all")) {
                style.text_align = TextAlign::Justify;
                style.text_align_last = TextAlignLast::Justify;
                return;
            } else
                return;
            style.text_align_last = TextAlignLast::Auto;
            return;
        }
        if (name == "direction") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "ltr"))
                style.direction = Direction::Ltr;
            else if (is_ident(values[0], "rtl"))
                style.direction = Direction::Rtl;
            return;
        }
        if (name == "writing-mode") {
            if (values.size() != 1)
                return;
            // The SVG 1.1 names are aliases the specification keeps
            // (css-writing-modes-4 §3.1): `tb` and `tb-rl` are vertical-rl,
            // and the others all name the horizontal mode.
            if (is_ident(values[0], "horizontal-tb") || is_ident(values[0], "lr")
                || is_ident(values[0], "lr-tb") || is_ident(values[0], "rl")
                || is_ident(values[0], "rl-tb"))
                style.writing_mode = WritingMode::HorizontalTb;
            else if (is_ident(values[0], "vertical-rl") || is_ident(values[0], "tb")
                || is_ident(values[0], "tb-rl"))
                style.writing_mode = WritingMode::VerticalRl;
            else if (is_ident(values[0], "vertical-lr"))
                style.writing_mode = WritingMode::VerticalLr;
            else if (is_ident(values[0], "sideways-rl"))
                style.writing_mode = WritingMode::SidewaysRl;
            else if (is_ident(values[0], "sideways-lr"))
                style.writing_mode = WritingMode::SidewaysLr;
            return;
        }
        if (name == "text-orientation") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "mixed"))
                style.text_orientation = TextOrientation::Mixed;
            else if (is_ident(values[0], "upright"))
                style.text_orientation = TextOrientation::Upright;
            else if (is_ident(values[0], "sideways") || is_ident(values[0], "sideways-right"))
                style.text_orientation = TextOrientation::Sideways;
            return;
        }
        if (name == "unicode-bidi") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "normal"))
                style.unicode_bidi = UnicodeBidi::Normal;
            else if (is_ident(values[0], "embed"))
                style.unicode_bidi = UnicodeBidi::Embed;
            else if (is_ident(values[0], "isolate"))
                style.unicode_bidi = UnicodeBidi::Isolate;
            else if (is_ident(values[0], "bidi-override"))
                style.unicode_bidi = UnicodeBidi::BidiOverride;
            else if (is_ident(values[0], "isolate-override"))
                style.unicode_bidi = UnicodeBidi::IsolateOverride;
            else if (is_ident(values[0], "plaintext"))
                style.unicode_bidi = UnicodeBidi::Plaintext;
            return;
        }
        if (name == "text-justify") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "auto"))
                style.text_justify = TextJustify::Auto;
            else if (is_ident(values[0], "none"))
                style.text_justify = TextJustify::None;
            else if (is_ident(values[0], "inter-word"))
                style.text_justify = TextJustify::InterWord;
            else if (is_ident(values[0], "inter-character") || is_ident(values[0], "distribute"))
                style.text_justify = TextJustify::InterCharacter;
            return;
        }
        if (name == "text-align-last") {
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "auto"))
                style.text_align_last = TextAlignLast::Auto;
            else if (is_ident(values[0], "start"))
                style.text_align_last = TextAlignLast::Start;
            else if (is_ident(values[0], "end"))
                style.text_align_last = TextAlignLast::End;
            else if (is_ident(values[0], "match-parent"))
                style.text_align_last = TextAlignLast::MatchParent;
            else if (is_ident(values[0], "left"))
                style.text_align_last = TextAlignLast::Left;
            else if (is_ident(values[0], "right"))
                style.text_align_last = TextAlignLast::Right;
            else if (is_ident(values[0], "center"))
                style.text_align_last = TextAlignLast::Center;
            else if (is_ident(values[0], "justify"))
                style.text_align_last = TextAlignLast::Justify;
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
        if (name == "text-transform") {
            // none | capitalize | uppercase | lowercase. The full-width and
            // full-size-kana keywords are not written, and a declaration
            // naming one is dropped rather than half-applied.
            if (values.size() != 1)
                return;
            if (is_ident(values[0], "none"))
                style.text_transform = TextTransform::None;
            else if (is_ident(values[0], "capitalize"))
                style.text_transform = TextTransform::Capitalize;
            else if (is_ident(values[0], "uppercase"))
                style.text_transform = TextTransform::Uppercase;
            else if (is_ident(values[0], "lowercase"))
                style.text_transform = TextTransform::Lowercase;
            return;
        }
        if (name == "vertical-align") {
            if (values.size() != 1)
                return;
            struct Keyword {
                std::string_view name;
                VerticalAlign::Kind kind;
            };
            static constexpr Keyword keywords[] = {
                { "baseline", VerticalAlign::Kind::Baseline },
                { "sub", VerticalAlign::Kind::Sub },
                { "super", VerticalAlign::Kind::Super },
                { "text-top", VerticalAlign::Kind::TextTop },
                { "text-bottom", VerticalAlign::Kind::TextBottom },
                { "middle", VerticalAlign::Kind::Middle },
                { "top", VerticalAlign::Kind::Top },
                { "bottom", VerticalAlign::Kind::Bottom },
            };
            for (Keyword const& keyword : keywords) {
                if (is_ident(values[0], keyword.name)) {
                    style.vertical_align = VerticalAlign { keyword.kind, LengthPercent::px(0) };
                    return;
                }
            }
            if (auto length = parse_length_percent(*values[0], context, false))
                style.vertical_align = VerticalAlign { VerticalAlign::Kind::Length, *length };
            return;
        }
        if (name == "list-style-position" || name == "list-style") {
            // The shorthand carries the type and the image as well, so each
            // longhand takes the first value it recognises and leaves the
            // rest to the others; the shorthand resets what nothing names.
            if (name == "list-style")
                style.list_style_position = ListStylePosition::Outside;
            for (ComponentValue const* value : values) {
                if (is_ident(value, "inside")) {
                    style.list_style_position = ListStylePosition::Inside;
                    break;
                }
                if (is_ident(value, "outside")) {
                    style.list_style_position = ListStylePosition::Outside;
                    break;
                }
            }
            if (name == "list-style-position")
                return;
        }
        if (name == "list-style-type" || name == "list-style") {
            // The shorthand carries position and image too, so the first
            // value it holds that names a counter style is the type and the
            // rest are somebody else's business.
            for (ComponentValue const* value : values) {
                if (auto const type = parse_list_style_type(value)) {
                    style.list_style_type = *type;
                    return;
                }
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
        // Rounded corners. A radius is never negative and never auto; the
        // shorthand takes one to four horizontal radii, then a slash and
        // one to four vertical ones, in the order top-left, top-right,
        // bottom-right, bottom-left.
        if (name == "border-radius" || (name.starts_with("border-") && name.ends_with("-radius"))) {
            auto const radii = [&](std::vector<ComponentValue const*> const& list,
                                   std::vector<LengthPercent>& out) {
                for (ComponentValue const* value : list) {
                    std::optional<LengthPercent> const length
                        = parse_length_percent(*value, context, false, true);
                    if (!length)
                        return false;
                    // A written radius is never negative; what a calc()
                    // works out to is settled where it is used and clamped
                    // to zero there, so it is not thrown out here.
                    if (!value->is_function() && (length->value < 0 || length->percent < 0))
                        return false;
                    out.push_back(*length);
                }
                return !out.empty() && out.size() <= 4;
            };
            auto const corner = [](std::vector<LengthPercent> const& list, std::size_t at) {
                // One value covers every corner, two the diagonals, three
                // leave the fourth to match the second.
                switch (list.size()) {
                case 1: return list[0];
                case 2: return list[at % 2];
                case 3: return at == 3 ? list[1] : list[at];
                default: return list[at];
                }
            };
            std::vector<ComponentValue const*> horizontal;
            std::vector<ComponentValue const*> vertical;
            bool past_slash = false;
            for (ComponentValue const* value : values) {
                if (value->is_token(Token::Type::Delim) && value->token().delim == U'/') {
                    if (past_slash)
                        return;
                    past_slash = true;
                    continue;
                }
                (past_slash ? vertical : horizontal).push_back(value);
            }
            std::vector<LengthPercent> across;
            std::vector<LengthPercent> down;
            if (!radii(horizontal, across))
                return;
            if (!past_slash)
                down = across; // no slash: the corners are quarter circles
            else if (!radii(vertical, down))
                return;
            if (name == "border-radius") {
                CornerRadius* const corners[4] = { &style.border_top_left_radius,
                    &style.border_top_right_radius, &style.border_bottom_right_radius,
                    &style.border_bottom_left_radius };
                for (std::size_t at = 0; at < 4; ++at) {
                    corners[at]->x = corner(across, at);
                    corners[at]->y = corner(down, at);
                }
                return;
            }
            // A corner longhand: one length, or two for an ellipse.
            if (past_slash || across.size() > 2)
                return;
            CornerRadius* corner_style = nullptr;
            if (name == "border-top-left-radius")
                corner_style = &style.border_top_left_radius;
            else if (name == "border-top-right-radius")
                corner_style = &style.border_top_right_radius;
            else if (name == "border-bottom-right-radius")
                corner_style = &style.border_bottom_right_radius;
            else if (name == "border-bottom-left-radius")
                corner_style = &style.border_bottom_left_radius;
            if (!corner_style)
                return;
            corner_style->x = across[0];
            corner_style->y = across.size() > 1 ? across[1] : across[0];
            return;
        }
        if (name.starts_with("border-") && values.size() == 1) {
            // The side longhands: border-top-width, border-left-style,
            // border-bottom-color and the rest.
            std::string_view const rest = std::string_view(name).substr(7);
            BorderSide* side = nullptr;
            bool* color_flag = nullptr;
            std::string_view property;
            auto const pick = [&](std::string_view prefix, BorderSide& candidate, bool& flag) {
                if (!rest.starts_with(prefix))
                    return false;
                side = &candidate;
                color_flag = &flag;
                property = rest.substr(prefix.size());
                return true;
            };
            if (pick("top-", style.border_top, border_top_color_set)
                || pick("right-", style.border_right, border_right_color_set)
                || pick("bottom-", style.border_bottom, border_bottom_color_set)
                || pick("left-", style.border_left, border_left_color_set)) {
                if (property == "width") {
                    if (auto width = parse_border_width(*values[0], context))
                        side->width = *width;
                } else if (property == "style") {
                    if (auto border_style = parse_border_style(*values[0])) {
                        side->style = *border_style;
                        // A style with no width written yet is medium wide.
                        if (side->style == BorderStyle::Solid && side->width == 0)
                            side->width = 3;
                    }
                } else if (property == "color") {
                    if (auto color = parse_color_component(*values[0], style.color)) {
                        side->color = *color;
                        *color_flag = true;
                    }
                }
                return;
            }
        }
        // Unknown properties fall on the floor, by design.
    }
};

StyleSet::StyleSet(std::vector<SheetSource> const& sheets, MediaContext const& media, net::Url const* document_url)
    : m_rules(std::make_unique<RuleSet>())
{
    m_rules->media = media;
    if (document_url)
        m_rules->document_url = *document_url;
    int order = 0;
    m_rules->compile_sheet(ua_stylesheet, true, order, nullptr);
    for (SheetSource const& sheet : sheets) {
        std::shared_ptr<net::Url const> const base
            = sheet.url ? std::make_shared<net::Url const>(*sheet.url) : nullptr;
        m_rules->compile_sheet(sheet.text, false, order, base);
    }
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
    resolver.hand_down_first_letters(document);
    // CSS 2.1 §11.1.1: the root's overflow applies to the viewport, and when
    // the root's is visible, body's does instead — and the element it was
    // taken from is then visible itself, so body still lets margins through
    // and keeps no floats of its own.
    for (dom::Node const* child : document.children()) {
        if (!child->is_element() || !static_cast<dom::Element const*>(child)->is_html("html"))
            continue;
        auto const html_it = resolver.map.find(static_cast<dom::Element const*>(child));
        if (html_it == resolver.map.end())
            continue;
        if (html_it->second.overflow != Overflow::Visible) {
            html_it->second.overflow = Overflow::Visible;
            html_it->second.overflow_x = Overflow::Visible;
            html_it->second.overflow_y = Overflow::Visible;
            break;
        }
        for (dom::Node const* grandchild : child->children()) {
            if (!grandchild->is_element() || !static_cast<dom::Element const*>(grandchild)->is_html("body"))
                continue;
            if (auto const body_it = resolver.map.find(static_cast<dom::Element const*>(grandchild));
                body_it != resolver.map.end()) {
                body_it->second.overflow = Overflow::Visible;
                body_it->second.overflow_x = Overflow::Visible;
                body_it->second.overflow_y = Overflow::Visible;
            }
            break;
        }
        break;
    }
    // css-writing-modes-4 §3.1: the document's principal writing mode is
    // the root's, except that an HTML root with a body child takes its used
    // writing-mode and direction from that body instead. The computed
    // values are left alone — everything has already inherited from them —
    // so this settles only the mode the page is laid out in, and it is what
    // keeps a `body { writing-mode: vertical-rl }` from standing sideways
    // inside an upright root.
    for (dom::Node const* child : document.children()) {
        if (!child->is_element() || !static_cast<dom::Element const*>(child)->is_html("html"))
            continue;
        auto const html_it = resolver.map.find(static_cast<dom::Element const*>(child));
        if (html_it == resolver.map.end())
            break;
        for (dom::Node const* grandchild : child->children()) {
            if (!grandchild->is_element() || !static_cast<dom::Element const*>(grandchild)->is_html("body"))
                continue;
            if (auto const body_it = resolver.map.find(static_cast<dom::Element const*>(grandchild));
                body_it != resolver.map.end()) {
                html_it->second.writing_mode = body_it->second.writing_mode;
                html_it->second.direction = body_it->second.direction;
            }
            break;
        }
        break;
    }
    return std::move(resolver.map);
}

StyleMap resolve_styles(dom::Document const& document, std::vector<SheetSource> const& sheets,
    MediaContext const& media, net::Url const* document_url)
{
    return resolve_styles(document, StyleSet(sheets, media, document_url));
}

StyleMap resolve_styles(dom::Document const& document)
{
    return resolve_styles(document, collect_stylesheets(document, nullptr, {}));
}

ComputedStyle inherited_style(ComputedStyle const& parent)
{
    return Resolver::inherited_from(parent);
}

}
