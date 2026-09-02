#include "css/Stylesheets.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "css/Parser.h"
#include "css/Tokenizer.h"
#include "dom/Dom.h"
#include "html/Encoding.h"

#include <algorithm>
#include <set>

namespace sashfold::css {

namespace {

constexpr int max_import_depth = 4;
constexpr std::size_t max_sheets = 64;

std::string lowercased(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        out += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

// Whitespace- and comma-separated tokens, lowercased.
std::vector<std::string> tokens_of(std::string_view text)
{
    std::vector<std::string> out;
    std::string current;
    for (char const c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ',') {
            if (!current.empty())
                out.push_back(lowercased(current));
            current.clear();
            if (c == ',')
                out.push_back(",");
        } else {
            current += c;
        }
    }
    if (!current.empty())
        out.push_back(lowercased(current));
    return out;
}

// The charset parameter of a Content-Type value, unquoted; empty when none.
std::string charset_parameter(std::string_view content_type)
{
    std::string const lower = lowercased(content_type);
    std::size_t at = lower.find("charset=");
    if (at == std::string::npos)
        return {};
    at += 8;
    std::size_t end = at;
    while (end < lower.size() && lower[end] != ';' && lower[end] != ' ' && lower[end] != '\t')
        ++end;
    std::string value = lower.substr(at, end - at);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::string attribute(dom::Element const& element, char const* name)
{
    dom::Attr const* attr = element.find_attribute(name);
    return attr ? attr->value : std::string();
}

struct Collector {
    SheetFetcher const& fetch;
    std::vector<SheetSource>& out;
    MediaContext const& media;
    std::set<std::string> visited;

    void add_fetched(net::Url const& url, int depth)
    {
        std::string const key = url.serialize(true);
        if (visited.contains(key) || out.size() >= max_sheets || !fetch)
            return;
        visited.insert(key);
        std::optional<FetchedSheet> const fetched = fetch(url);
        if (!fetched)
            return;
        add_text(decode_stylesheet(fetched->bytes, fetched->content_type), url, depth);
    }

    void add_text(std::string text, std::optional<net::Url> const& url, int depth)
    {
        if (depth < max_import_depth) {
            for (std::string const& href : import_urls(text, media)) {
                if (std::optional<net::Url> const target = net::parse_url(href, url ? &*url : nullptr))
                    add_fetched(*target, depth + 1);
            }
        }
        out.push_back(SheetSource { std::move(text), url });
    }
};

void walk(dom::Node const& node, net::Url const* base, Collector& collector)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("style")) {
            if (!media_query_matches(attribute(element, "media"), collector.media))
                return;
            std::string text;
            for (dom::Node const* child : element.children()) {
                if (child->is_text())
                    text += static_cast<dom::Text const*>(child)->data;
            }
            // XHTML pages wrap a sheet in a CDATA section; parsed as HTML,
            // the markers arrive as text, and the opening one would swallow
            // the sheet into a bracket block. An XML parser would drop them.
            std::size_t const start = text.find_first_not_of(" \t\n\r\f");
            if (start != std::string::npos && text.compare(start, 9, "<![CDATA[") == 0) {
                std::size_t const end = text.rfind("]]>");
                if (end != std::string::npos && end >= start + 9)
                    text = text.substr(start + 9, end - (start + 9));
            }
            collector.add_text(std::move(text),
                base ? std::optional<net::Url>(*base) : std::nullopt, 0);
            return;
        }
        if (element.is_html("link")) {
            bool is_stylesheet = false;
            bool is_alternate = false;
            for (std::string const& token : tokens_of(attribute(element, "rel"))) {
                is_stylesheet = is_stylesheet || token == "stylesheet";
                is_alternate = is_alternate || token == "alternate";
            }
            if (!is_stylesheet || is_alternate || element.find_attribute("disabled"))
                return;
            std::string const type = lowercased(attribute(element, "type"));
            if (!type.empty() && type != "text/css")
                return;
            if (!media_query_matches(attribute(element, "media"), collector.media))
                return;
            std::string const href = attribute(element, "href");
            if (href.empty())
                return;
            if (std::optional<net::Url> const target = net::parse_url(href, base))
                collector.add_fetched(*target, 0);
            return;
        }
    }
    for (dom::Node const* child : node.children())
        walk(*child, base, collector);
}

} // namespace

std::vector<SheetSource> collect_stylesheets(dom::Document const& document, net::Url const* base,
    SheetFetcher const& fetch, MediaContext const& media)
{
    std::vector<SheetSource> sheets;
    Collector collector { fetch, sheets, media, {} };
    walk(document, base, collector);
    return sheets;
}

std::string decode_stylesheet(std::vector<std::uint8_t> const& bytes, std::string_view content_type)
{
    std::string_view view(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    std::optional<html::Encoding> encoding;
    if (view.starts_with("\xEF\xBB\xBF")) {
        encoding = html::Encoding::Utf8;
        view.remove_prefix(3);
    } else if (view.starts_with("\xFE\xFF")) {
        encoding = html::Encoding::Utf16Be;
        view.remove_prefix(2);
    } else if (view.starts_with("\xFF\xFE")) {
        encoding = html::Encoding::Utf16Le;
        view.remove_prefix(2);
    }
    if (!encoding) {
        if (std::string const label = charset_parameter(content_type); !label.empty())
            encoding = html::encoding_from_label(label);
    }
    if (!encoding && view.starts_with("@charset \"")) {
        std::size_t const end = view.find('"', 10);
        if (end != std::string_view::npos && end < 10 + 128 && view.substr(end, 2) == "\";")
            encoding = html::encoding_from_label(view.substr(10, end - 10));
    }
    std::u32string decoded = html::decode(view, encoding.value_or(html::Encoding::Utf8));
    if (!decoded.empty() && decoded[0] == 0xFEFF)
        decoded.erase(0, 1);
    return to_utf8(decoded);
}

std::vector<std::string> import_urls(std::string_view sheet_text, MediaContext const& media)
{
    std::vector<std::string> urls;
    Stylesheet const sheet = parse_stylesheet(sheet_text);
    for (Rule const& rule : sheet.rules) {
        if (!rule.is_at_rule())
            break; // imports precede every other rule
        AtRule const& at = rule.at_rule();
        std::string const name = lowercased(at.name);
        if (name == "charset" || name == "layer")
            continue;
        if (name != "import")
            break;
        std::optional<std::string> url;
        std::vector<ComponentValue> conditions;
        bool first = true;
        for (ComponentValue const& value : at.prelude) {
            if (value.is_token(Token::Type::Whitespace))
                continue;
            if (first) {
                first = false;
                if (value.is_token(Token::Type::String) || value.is_token(Token::Type::Url)) {
                    url = value.token().value;
                } else if (value.is_function() && lowercased(value.function().name) == "url") {
                    for (ComponentValue const& inner : value.function().values) {
                        if (inner.is_token(Token::Type::String) || inner.is_token(Token::Type::Ident)) {
                            url = inner.token().value;
                            break;
                        }
                    }
                }
                continue;
            }
            // layer / layer() / supports() precede the media list and are
            // not conditions this cascade evaluates.
            if (value.is_token(Token::Type::Ident) && lowercased(value.token().value) == "layer"
                && conditions.empty())
                continue;
            if (value.is_function() && conditions.empty()) {
                std::string const function = lowercased(value.function().name);
                if (function == "layer" || function == "supports")
                    continue;
            }
            conditions.push_back(value);
        }
        if (url && media_prelude_matches(conditions, media))
            urls.push_back(*url);
    }
    return urls;
}

// --- Media queries -------------------------------------------------------------

namespace {

// Media Queries 4 evaluate in three values: an unknown feature poisons
// its query rather than passing it.
enum class Truth {
    False,
    True,
    Unknown,
};

Truth truth_of(bool value) { return value ? Truth::True : Truth::False; }

Truth truth_not(Truth value)
{
    if (value == Truth::Unknown)
        return Truth::Unknown;
    return value == Truth::True ? Truth::False : Truth::True;
}

Truth truth_and(Truth a, Truth b)
{
    if (a == Truth::False || b == Truth::False)
        return Truth::False;
    if (a == Truth::Unknown || b == Truth::Unknown)
        return Truth::Unknown;
    return Truth::True;
}

Truth truth_or(Truth a, Truth b)
{
    if (a == Truth::True || b == Truth::True)
        return Truth::True;
    if (a == Truth::Unknown || b == Truth::Unknown)
        return Truth::Unknown;
    return Truth::False;
}

bool opens_group(Token const& token)
{
    return token.is(Token::Type::OpenParen) || token.is(Token::Type::Function);
}

class MediaEvaluator {
public:
    MediaEvaluator(std::vector<Token> const& tokens, MediaContext const& media)
        : m_tokens(tokens)
        , m_media(media)
    {
    }

    // A list is true when any query is; an empty list is true.
    bool query_list()
    {
        skip_whitespace();
        if (at_end())
            return true;
        bool any = false;
        while (true) {
            any = any || query() == Truth::True;
            while (!at_end() && !peek().is(Token::Type::Comma))
                ++m_pos;
            if (at_end())
                break;
            ++m_pos;
        }
        return any;
    }

private:
    bool at_end() const { return m_pos >= m_tokens.size(); }
    Token const& peek() const { return m_tokens[m_pos]; }

    void skip_whitespace()
    {
        while (!at_end() && peek().is(Token::Type::Whitespace))
            ++m_pos;
    }

    bool peek_ident(std::string_view name)
    {
        skip_whitespace();
        return !at_end() && peek().is(Token::Type::Ident) && ascii_ci_equals(peek().value, name);
    }

    bool take_ident(std::string_view name)
    {
        if (!peek_ident(name))
            return false;
        ++m_pos;
        return true;
    }

    bool take(Token::Type type)
    {
        skip_whitespace();
        if (at_end() || !peek().is(type))
            return false;
        ++m_pos;
        return true;
    }

    static bool is_comparison(Token const& token)
    {
        return token.is(Token::Type::Delim)
            && (token.delim == U'<' || token.delim == U'>' || token.delim == U'=');
    }

    // <, <=, >, >=, or =; the two-character forms arrive as two delims.
    std::optional<std::string> take_comparison()
    {
        skip_whitespace();
        if (at_end() || !is_comparison(peek()))
            return std::nullopt;
        char32_t const first = peek().delim;
        ++m_pos;
        std::string op(1, static_cast<char>(first));
        if (first != U'=' && !at_end() && peek().is(Token::Type::Delim) && peek().delim == U'=') {
            ++m_pos;
            op += '=';
        }
        return op;
    }

    // Tokens up to the closing paren of the current group, whitespace dropped.
    std::vector<Token> values_until_close()
    {
        std::vector<Token> values;
        int depth = 0;
        while (!at_end()) {
            Token const& token = peek();
            if (token.is(Token::Type::CloseParen) && depth == 0)
                break;
            if (opens_group(token))
                ++depth;
            else if (token.is(Token::Type::CloseParen))
                --depth;
            if (!token.is(Token::Type::Whitespace))
                values.push_back(token);
            ++m_pos;
        }
        return values;
    }

    void close_group()
    {
        int depth = 1;
        while (!at_end() && depth > 0) {
            if (opens_group(peek()))
                ++depth;
            else if (peek().is(Token::Type::CloseParen))
                --depth;
            ++m_pos;
        }
    }

    Truth query()
    {
        skip_whitespace();
        bool negate = false;
        if (take_ident("not"))
            negate = true;
        else
            take_ident("only");
        skip_whitespace();
        Truth result;
        if (!at_end() && peek().is(Token::Type::Ident) && !ascii_ci_equals(peek().value, "and")
            && !ascii_ci_equals(peek().value, "not")) {
            std::string const type = lowercased(peek().value);
            ++m_pos;
            result = truth_of(type == "all" || type == "screen");
            while (take_ident("and"))
                result = truth_and(result, condition());
        } else {
            result = condition_list();
        }
        if (negate)
            result = truth_not(result);
        skip_whitespace();
        if (!at_end() && !peek().is(Token::Type::Comma))
            return Truth::False; // something this grammar does not know
        return result;
    }

    Truth condition_list()
    {
        Truth result = condition();
        while (true) {
            if (take_ident("and"))
                result = truth_and(result, condition());
            else if (take_ident("or"))
                result = truth_or(result, condition());
            else
                break;
        }
        return result;
    }

    Truth condition()
    {
        if (take_ident("not"))
            return truth_not(condition());
        if (!take(Token::Type::OpenParen))
            return Truth::Unknown;
        return parenthesized();
    }

    // After an opening paren: a nested condition, or one feature.
    Truth parenthesized()
    {
        skip_whitespace();
        if (at_end())
            return Truth::Unknown;
        if (peek().is(Token::Type::OpenParen) || peek_ident("not")) {
            Truth const inner = condition_list();
            close_group();
            return inner;
        }
        Truth result = Truth::Unknown;
        if (peek().is(Token::Type::Ident)) {
            std::string const name = lowercased(peek().value);
            ++m_pos;
            skip_whitespace();
            if (take(Token::Type::CloseParen))
                return boolean_feature(name);
            if (take(Token::Type::Colon)) {
                result = feature(name, values_until_close());
            } else if (std::optional<std::string> const op = take_comparison()) {
                result = compare(name, *op, values_until_close());
            }
        } else {
            // value op name [op value]
            std::vector<Token> left;
            while (!at_end() && !peek().is(Token::Type::CloseParen) && !is_comparison(peek())) {
                if (!peek().is(Token::Type::Whitespace))
                    left.push_back(peek());
                ++m_pos;
            }
            std::optional<std::string> const op = take_comparison();
            skip_whitespace();
            if (op && !at_end() && peek().is(Token::Type::Ident)) {
                std::string const name = lowercased(peek().value);
                ++m_pos;
                result = compare(name, flipped(*op), left);
                if (std::optional<std::string> const second = take_comparison())
                    result = truth_and(result, compare(name, *second, values_until_close()));
            }
        }
        close_group();
        return result;
    }

    static std::string flipped(std::string const& op)
    {
        if (op == "<")
            return ">";
        if (op == "<=")
            return ">=";
        if (op == ">")
            return "<";
        if (op == ">=")
            return "<=";
        return op;
    }

    // A single length in CSS px; nullopt for anything else.
    std::optional<double> length_px(std::vector<Token> const& values) const
    {
        if (values.size() != 1)
            return std::nullopt;
        Token const& token = values[0];
        if (token.is(Token::Type::Number))
            return token.numeric_value == 0 ? std::optional<double>(0) : std::nullopt;
        if (!token.is(Token::Type::Dimension))
            return std::nullopt;
        std::string const unit = lowercased(token.unit);
        double const value = token.numeric_value;
        if (unit == "px")
            return value;
        if (unit == "em" || unit == "rem")
            return value * 16;
        if (unit == "vw")
            return value * static_cast<double>(m_media.width) / 100;
        if (unit == "vh")
            return value * static_cast<double>(m_media.height) / 100;
        return std::nullopt;
    }

    // a/b or a number.
    static std::optional<double> ratio(std::vector<Token> const& values)
    {
        if (values.size() == 1 && values[0].is(Token::Type::Number) && values[0].numeric_value > 0)
            return values[0].numeric_value;
        if (values.size() == 3 && values[0].is(Token::Type::Number) && values[1].is(Token::Type::Delim)
            && values[1].delim == U'/' && values[2].is(Token::Type::Number)
            && values[2].numeric_value > 0)
            return values[0].numeric_value / values[2].numeric_value;
        return std::nullopt;
    }

    // Resolution in dots per CSS px.
    static std::optional<double> resolution(std::vector<Token> const& values)
    {
        if (values.size() != 1)
            return std::nullopt;
        Token const& token = values[0];
        if (token.is(Token::Type::Number))
            return token.numeric_value; // the -webkit- ratios are plain numbers
        if (!token.is(Token::Type::Dimension))
            return std::nullopt;
        std::string const unit = lowercased(token.unit);
        if (unit == "dppx" || unit == "x")
            return token.numeric_value;
        if (unit == "dpi")
            return token.numeric_value / 96;
        if (unit == "dpcm")
            return token.numeric_value / 37.8;
        return std::nullopt;
    }

    // The context's value of a range feature.
    std::optional<double> current(std::string const& name) const
    {
        if (name == "width" || name == "device-width")
            return static_cast<double>(m_media.width);
        if (name == "height" || name == "device-height")
            return static_cast<double>(m_media.height);
        if (name == "aspect-ratio" || name == "device-aspect-ratio")
            return static_cast<double>(m_media.width) / static_cast<double>(m_media.height);
        if (name == "resolution" || name == "-webkit-device-pixel-ratio")
            return 1;
        if (name == "color")
            return 8;
        if (name == "monochrome")
            return 0;
        return std::nullopt;
    }

    std::optional<double> parse_for(std::string const& name, std::vector<Token> const& values) const
    {
        if (name == "width" || name == "height" || name == "device-width" || name == "device-height")
            return length_px(values);
        if (name == "aspect-ratio" || name == "device-aspect-ratio")
            return ratio(values);
        if (name == "resolution" || name == "-webkit-device-pixel-ratio")
            return resolution(values);
        if (name == "color" || name == "monochrome") {
            if (values.size() == 1 && values[0].is(Token::Type::Number))
                return values[0].numeric_value;
            return std::nullopt;
        }
        return std::nullopt;
    }

    // name op value, e.g. width >= 600px.
    Truth compare(std::string const& name, std::string const& op, std::vector<Token> const& values) const
    {
        std::optional<double> const have = current(name);
        std::optional<double> const want = parse_for(name, values);
        if (!have || !want)
            return Truth::Unknown;
        constexpr double epsilon = 1e-6;
        if (op == "<")
            return truth_of(*have < *want - epsilon);
        if (op == "<=")
            return truth_of(*have <= *want + epsilon);
        if (op == ">")
            return truth_of(*have > *want + epsilon);
        if (op == ">=")
            return truth_of(*have >= *want - epsilon);
        return truth_of(*have > *want - epsilon && *have < *want + epsilon);
    }

    Truth feature(std::string const& name, std::vector<Token> const& values) const
    {
        if (name.starts_with("min-"))
            return compare(name.substr(4), ">=", values);
        if (name.starts_with("max-"))
            return compare(name.substr(4), "<=", values);
        if (name == "-webkit-min-device-pixel-ratio")
            return compare("resolution", ">=", values);
        if (name == "-webkit-max-device-pixel-ratio")
            return compare("resolution", "<=", values);
        if (current(name))
            return compare(name, "=", values);
        // Discrete features: one keyword.
        if (values.size() != 1 || !values[0].is(Token::Type::Ident))
            return Truth::Unknown;
        std::string const value = lowercased(values[0].value);
        if (name == "orientation")
            return truth_of(value == (m_media.width > m_media.height ? "landscape" : "portrait"));
        if (name == "prefers-color-scheme")
            return truth_of(value == "light");
        if (name == "prefers-reduced-motion" || name == "prefers-reduced-transparency"
            || name == "prefers-contrast" || name == "prefers-reduced-data")
            return truth_of(value == "no-preference");
        if (name == "forced-colors" || name == "inverted-colors")
            return truth_of(value == "none");
        if (name == "hover" || name == "any-hover")
            return truth_of(value == "hover");
        if (name == "pointer" || name == "any-pointer")
            return truth_of(value == "fine");
        if (name == "scripting")
            return truth_of(value == "none");
        if (name == "update")
            return truth_of(value == "fast");
        if (name == "overflow-block" || name == "overflow-inline")
            return truth_of(value == "scroll");
        if (name == "display-mode")
            return truth_of(value == "browser");
        if (name == "color-gamut")
            return truth_of(value == "srgb");
        if (name == "dynamic-range" || name == "video-dynamic-range")
            return truth_of(value == "standard");
        return Truth::Unknown;
    }

    // (name) alone: true when the feature's value is not zero or none.
    static Truth boolean_feature(std::string const& name)
    {
        if (name == "width" || name == "height" || name == "device-width" || name == "device-height"
            || name == "aspect-ratio" || name == "resolution" || name == "color" || name == "hover"
            || name == "any-hover" || name == "pointer" || name == "any-pointer"
            || name == "orientation")
            return Truth::True;
        if (name == "monochrome" || name == "grid" || name == "scripting"
            || name == "prefers-reduced-motion" || name == "prefers-contrast"
            || name == "forced-colors" || name == "inverted-colors")
            return Truth::False;
        return Truth::Unknown;
    }

    std::vector<Token> const& m_tokens;
    MediaContext const& m_media;
    std::size_t m_pos = 0;
};

void flatten(std::vector<ComponentValue> const& values, std::vector<Token>& out)
{
    for (ComponentValue const& value : values) {
        if (value.is_token()) {
            out.push_back(value.token());
            continue;
        }
        if (value.is_function()) {
            Token open;
            open.type = Token::Type::Function;
            open.value = value.function().name;
            out.push_back(open);
            flatten(value.function().values, out);
            Token close;
            close.type = Token::Type::CloseParen;
            out.push_back(close);
            continue;
        }
        SimpleBlock const& block = value.block();
        Token open;
        open.type = block.open;
        out.push_back(open);
        flatten(block.values, out);
        Token close;
        close.type = block.open == Token::Type::OpenParen ? Token::Type::CloseParen
            : block.open == Token::Type::OpenSquare          ? Token::Type::CloseSquare
                                                             : Token::Type::CloseBrace;
        out.push_back(close);
    }
}

} // namespace

bool media_query_matches(std::string_view query_list, MediaContext const& media)
{
    std::vector<Token> const tokens = Tokenizer::tokenize(query_list);
    return MediaEvaluator(tokens, media).query_list();
}

bool media_prelude_matches(std::vector<ComponentValue> const& prelude, MediaContext const& media)
{
    std::vector<Token> tokens;
    flatten(prelude, tokens);
    return MediaEvaluator(tokens, media).query_list();
}

}

