#include "css/StyleResolver.h"

#include "core/Ascii.h"
#include "css/Parser.h"
#include "css/Selector.h"
#include "dom/Dom.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
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
sub { vertical-align: sub; font-size: smaller }
sup { vertical-align: super; font-size: smaller }
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
    if (ascii_ci_equals(unit, "ex") || ascii_ci_equals(unit, "ch"))
        return LengthPercent::px(static_cast<float>(number * static_cast<double>(context.font_size) * 0.5));
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
    // Rules are matched for three targets at once — the element itself, its
    // ::before and its ::after — since one selector walk serves all three.
    // Per target and rule: the element (stamp) it last matched and its best
    // matching selector for that element.
    static constexpr int target_count = 3;
    std::array<std::vector<int>, target_count> rule_stamp;
    std::array<std::vector<Specificity>, target_count> rule_best;
    int stamp = 0;
    std::array<std::vector<std::uint32_t>, target_count> matched_rules; // scratch, reused per element
    AncestorFilter ancestors; // the identifiers of the elements above the one being styled
    int quote_depth = 0; // the nesting of quotation marks so far, in tree order

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
            && pseudo.display != Display::TableColumn;
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
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            ComputedStyle style = compute_for(element, parent_style);
            bool const is_root = node.parent() && !node.parent()->is_element();
            if (is_root)
                root_font_size = style.font_size; // rem resolves against this
            // The generated boxes: each cascades from the rules matched for
            // its target (still in the scratch lists), inheriting from the
            // element. The ::before text is resolved here, in tree order;
            // the ::after text once the children have had their turn at the
            // quotation marks.
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
                    if (has_before) {
                        std::string text = content_text(element, *before);
                        boxes->before = GeneratedBox { std::move(*before), std::move(text) };
                    }
                    if (has_after)
                        boxes->after = GeneratedBox { std::move(*after), {} };
                    generated = boxes.get();
                    owner = &element;
                    style.generated = std::move(boxes);
                }
            }
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
        if (generated && generated->after)
            generated->after->text = content_text(*owner, generated->after->style);
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
        style.text_align = parent.text_align;
        style.white_space = parent.white_space;
        style.list_style_type = parent.list_style_type;
        style.quotes = parent.quotes;
        style.custom = parent.custom;
        style.visibility = parent.visibility;
        return style;
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
                entry.order = order++;
                entry.rank = static_cast<int>(declaration.important
                        ? CascadeRank::StyleAttributeImportant
                        : CascadeRank::StyleAttributeNormal);
                matched.push_back(entry);
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

        // font-size first: em and font-relative units in the same element's
        // other declarations resolve against it.
        for (MatchedDeclaration const& entry : matched) {
            with_vars(*entry.declaration, [&](Declaration const& declaration) {
                if (ascii_ci_equals(declaration.name, "font-size")) {
                    apply_font_size(style, parent, declaration);
                } else if (ascii_ci_equals(declaration.name, "font")) {
                    // The shorthand's size goes first too; its other parts follow in apply().
                    if (std::optional<FontShorthand> const parts
                        = split_font_shorthand(significant(declaration.value))) {
                        Declaration size_only;
                        size_only.name = "font-size";
                        size_only.value.push_back(*parts->size);
                        apply_font_size(style, parent, size_only);
                    }
                }
            });
        }
        bool border_top_color_set = false;
        bool border_right_color_set = false;
        bool border_bottom_color_set = false;
        bool border_left_color_set = false;
        for (MatchedDeclaration const& entry : matched) {
            with_vars(*entry.declaration, [&](Declaration const& declaration) {
                apply(style, declaration, border_top_color_set, border_right_color_set,
                    border_bottom_color_set, border_left_color_set);
            });
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
        LengthContext const context { parent.font_size, root_font_size, set.media.width, set.media.height };
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
        LengthContext const context { style.font_size, root_font_size, set.media.width, set.media.height };

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
                style.display = Display::FlowRoot; // a table is a block-level root until tables land
            else if (ascii_ci_equals(keyword, "inline-block") || ascii_ci_equals(keyword, "inline-table"))
                style.display = Display::InlineBlock; // an inline table is an inline-level root until then
            else if (ascii_ci_equals(keyword, "table-column") || ascii_ci_equals(keyword, "table-column-group"))
                style.display = Display::TableColumn;
            else if (ascii_ci_equals(keyword, "table-row-group") || ascii_ci_equals(keyword, "table-header-group")
                || ascii_ci_equals(keyword, "table-footer-group") || ascii_ci_equals(keyword, "table-row"))
                style.overflow_applies = false; // display stays as it was until tables land
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
                    || ascii_ci_equals(keyword, "auto") || ascii_ci_equals(keyword, "scroll")
                    || ascii_ci_equals(keyword, "overlay")) // the legacy spelling of auto
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
                if (length->kind == LengthPercent::Kind::Percent || length->kind == LengthPercent::Kind::Calc)
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
            break;
        }
        for (dom::Node const* grandchild : child->children()) {
            if (!grandchild->is_element() || !static_cast<dom::Element const*>(grandchild)->is_html("body"))
                continue;
            if (auto const body_it = resolver.map.find(static_cast<dom::Element const*>(grandchild));
                body_it != resolver.map.end())
                body_it->second.overflow = Overflow::Visible;
            break;
        }
        break;
    }
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
