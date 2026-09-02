#include "ui/SourceSet.h"

#include "core/Ascii.h"
#include "css/Parser.h"
#include "css/Token.h"
#include "dom/Dom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace sashfold::ui {

namespace {

bool is_html_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// A valid non-negative integer: ASCII digits only.
std::optional<float> non_negative_integer(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    double value = 0;
    for (char const c : text) {
        if (!is_digit(c))
            return std::nullopt;
        value = value * 10 + (c - '0');
        if (value > 1e9)
            return std::nullopt;
    }
    return static_cast<float>(value);
}

// A valid floating-point number: an optional minus, digits, an optional
// fraction with digits, an optional exponent with digits.
std::optional<float> floating_point_number(std::string_view text)
{
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && text[i] == '-') {
        negative = true;
        ++i;
    }
    std::size_t const digits_start = i;
    double value = 0;
    while (i < text.size() && is_digit(text[i])) {
        value = value * 10 + (text[i] - '0');
        ++i;
    }
    if (i == digits_start)
        return std::nullopt;
    if (i < text.size() && text[i] == '.') {
        ++i;
        std::size_t const fraction_start = i;
        double scale = 0.1;
        while (i < text.size() && is_digit(text[i])) {
            value += (text[i] - '0') * scale;
            scale *= 0.1;
            ++i;
        }
        if (i == fraction_start)
            return std::nullopt;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool exponent_negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exponent_negative = text[i] == '-';
            ++i;
        }
        std::size_t const exponent_start = i;
        int exponent = 0;
        while (i < text.size() && is_digit(text[i])) {
            exponent = std::min(exponent * 10 + (text[i] - '0'), 400);
            ++i;
        }
        if (i == exponent_start)
            return std::nullopt;
        value *= std::pow(10.0, exponent_negative ? -exponent : exponent);
    }
    if (i != text.size() || !std::isfinite(value) || value > 1e9)
        return std::nullopt;
    return static_cast<float>(negative ? -value : value);
}

// A sizes entry's <length> in CSS px; nullopt for anything else (a calc(),
// a keyword, a negative length, a unit this engine does not know).
std::optional<float> source_size_length(css::ComponentValue const& value,
    css::MediaContext const& media)
{
    if (!value.is_token())
        return std::nullopt;
    css::Token const& token = value.token();
    if (token.type == css::Token::Type::Number)
        return token.numeric_value == 0 ? std::optional<float>(0.0f) : std::nullopt;
    if (token.type != css::Token::Type::Dimension || token.numeric_value < 0)
        return std::nullopt;
    double const number = token.numeric_value;
    std::string_view const unit = token.unit;
    double px = 0;
    if (ascii_ci_equals(unit, "px"))
        px = number;
    else if (ascii_ci_equals(unit, "em") || ascii_ci_equals(unit, "rem"))
        px = number * 16; // the initial font size: sizes has no element to inherit from
    else if (ascii_ci_equals(unit, "ex") || ascii_ci_equals(unit, "ch"))
        px = number * 8;
    else if (ascii_ci_equals(unit, "vw"))
        px = number * static_cast<double>(media.width) / 100;
    else if (ascii_ci_equals(unit, "vh"))
        px = number * static_cast<double>(media.height) / 100;
    else if (ascii_ci_equals(unit, "vmin"))
        px = number * static_cast<double>(std::min(media.width, media.height)) / 100;
    else if (ascii_ci_equals(unit, "vmax"))
        px = number * static_cast<double>(std::max(media.width, media.height)) / 100;
    else if (ascii_ci_equals(unit, "pt"))
        px = number * 4 / 3;
    else if (ascii_ci_equals(unit, "pc"))
        px = number * 16;
    else if (ascii_ci_equals(unit, "in"))
        px = number * 96;
    else if (ascii_ci_equals(unit, "cm"))
        px = number * 96 / 2.54;
    else if (ascii_ci_equals(unit, "mm"))
        px = number * 96 / 25.4;
    else if (ascii_ci_equals(unit, "q"))
        px = number * 96 / 101.6;
    else
        return std::nullopt;
    return static_cast<float>(px);
}

// The candidates' densities settled against the source size, duplicates of
// an earlier density dropped, then the smallest density at or above the
// device's — 1 — or the largest when none reaches it.
std::optional<ImageSource> select_from(std::vector<ImageCandidate> const& candidates,
    float source_size)
{
    std::vector<ImageSource> sources;
    for (ImageCandidate const& candidate : candidates) {
        float density = 1;
        if (candidate.width)
            density = source_size > 0 ? *candidate.width / source_size
                                      : std::numeric_limits<float>::max();
        else if (candidate.density)
            density = *candidate.density;
        bool const seen = std::any_of(sources.begin(), sources.end(),
            [&](ImageSource const& source) { return source.density == density; });
        if (!seen)
            sources.push_back(ImageSource { candidate.url, density });
    }
    ImageSource const* best = nullptr;
    for (ImageSource const& source : sources) {
        if (source.density >= 1 && (!best || source.density < best->density))
            best = &source;
    }
    if (!best) {
        for (ImageSource const& source : sources) {
            if (!best || source.density > best->density)
                best = &source;
        }
    }
    if (!best)
        return std::nullopt;
    return *best;
}

} // namespace

std::vector<ImageCandidate> parse_srcset(std::string_view input, net::Url const* base)
{
    std::vector<ImageCandidate> candidates;
    std::size_t position = 0;
    while (true) {
        // Splitting loop: past whitespace and commas to the next URL.
        while (position < input.size()
            && (is_html_whitespace(input[position]) || input[position] == ','))
            ++position;
        if (position >= input.size())
            break;
        std::size_t const url_start = position;
        while (position < input.size() && !is_html_whitespace(input[position]))
            ++position;
        std::string_view url = input.substr(url_start, position - url_start);

        std::vector<std::string> descriptors;
        if (url.ends_with(',')) {
            // A URL ending in commas has no descriptors: the commas were the
            // separator.
            while (url.ends_with(','))
                url.remove_suffix(1);
        } else {
            // Descriptor tokenizer: whitespace separates descriptors, a comma
            // ends them, and parentheses hold their contents together.
            while (position < input.size() && is_html_whitespace(input[position]))
                ++position;
            std::string current;
            enum class State { InDescriptor, InParens, AfterDescriptor };
            State state = State::InDescriptor;
            while (true) {
                bool const at_end = position >= input.size();
                char const c = at_end ? '\0' : input[position];
                if (state == State::InDescriptor) {
                    if (at_end) {
                        if (!current.empty())
                            descriptors.push_back(current);
                        break;
                    }
                    if (is_html_whitespace(c)) {
                        if (!current.empty()) {
                            descriptors.push_back(current);
                            current.clear();
                        }
                        state = State::AfterDescriptor;
                    } else if (c == ',') {
                        ++position;
                        if (!current.empty())
                            descriptors.push_back(current);
                        break;
                    } else if (c == '(') {
                        current += c;
                        state = State::InParens;
                    } else {
                        current += c;
                    }
                } else if (state == State::InParens) {
                    if (at_end) {
                        if (!current.empty())
                            descriptors.push_back(current);
                        break;
                    }
                    current += c;
                    if (c == ')')
                        state = State::InDescriptor;
                } else {
                    if (at_end)
                        break;
                    if (!is_html_whitespace(c)) {
                        state = State::InDescriptor;
                        continue; // the same character starts the next descriptor
                    }
                }
                ++position;
            }
        }

        // Descriptor parser: at most one width or one density, w and x never
        // together; an h descriptor is tolerated for the future and ignored.
        std::optional<float> width;
        std::optional<float> density;
        std::optional<float> future_height;
        bool error = false;
        for (std::string const& descriptor : descriptors) {
            char const kind = descriptor.back();
            std::string_view const number
                = std::string_view(descriptor).substr(0, descriptor.size() - 1);
            if (kind == 'w') {
                std::optional<float> const value = non_negative_integer(number);
                if (!value || width || density || *value == 0) {
                    error = true;
                    break;
                }
                width = value;
            } else if (kind == 'x') {
                std::optional<float> const value = floating_point_number(number);
                if (!value || width || density || future_height || *value < 0) {
                    error = true;
                    break;
                }
                density = value;
            } else if (kind == 'h') {
                std::optional<float> const value = non_negative_integer(number);
                if (!value || future_height || density || *value == 0) {
                    error = true;
                    break;
                }
                future_height = value;
            } else {
                error = true;
                break;
            }
        }
        if (error)
            continue;
        std::optional<net::Url> resolved = net::parse_url(std::string(url), base);
        if (!resolved)
            continue;
        candidates.push_back(ImageCandidate { std::move(*resolved), width, density });
    }
    return candidates;
}

float parse_sizes(std::string_view sizes, css::MediaContext const& media)
{
    std::vector<css::ComponentValue> const values = css::parse_component_value_list(sizes);
    std::vector<std::vector<css::ComponentValue>> entries(1);
    for (css::ComponentValue const& value : values) {
        if (value.is_token(css::Token::Type::Comma))
            entries.emplace_back();
        else
            entries.back().push_back(value);
    }
    for (std::vector<css::ComponentValue>& entry : entries) {
        while (!entry.empty() && entry.back().is_token(css::Token::Type::Whitespace))
            entry.pop_back();
        if (entry.empty())
            continue;
        std::optional<float> const length = source_size_length(entry.back(), media);
        entry.pop_back();
        if (!length)
            continue;
        while (!entry.empty() && entry.back().is_token(css::Token::Type::Whitespace))
            entry.pop_back();
        if (entry.empty() || css::media_prelude_matches(entry, media))
            return *length;
    }
    return media.width; // 100vw
}

bool supports_image_type(std::string_view type)
{
    std::string_view essence = type.substr(0, type.find(';'));
    while (!essence.empty() && is_html_whitespace(essence.front()))
        essence.remove_prefix(1);
    while (!essence.empty() && is_html_whitespace(essence.back()))
        essence.remove_suffix(1);
    if (essence.empty())
        return true;
    for (char const* known : { "image/png", "image/x-png", "image/jpeg", "image/jpg",
             "image/pjpeg", "image/gif" }) {
        if (ascii_ci_equals(essence, known))
            return true;
    }
    return false;
}

std::optional<ImageSource> select_image_source(dom::Element const& img, net::Url const* base,
    css::MediaContext const& media)
{
    // A <picture> parent's <source> elements before the img, in order: the
    // first one with candidates whose media and type both pass decides.
    dom::Node const* parent = img.parent();
    if (parent && parent->is_element()
        && static_cast<dom::Element const&>(*parent).is_html("picture")) {
        for (dom::Node const* child : parent->children()) {
            if (child == &img)
                break;
            if (!child->is_element())
                continue;
            auto const& source = static_cast<dom::Element const&>(*child);
            if (!source.is_html("source"))
                continue;
            dom::Attr const* srcset = source.find_attribute("srcset");
            if (!srcset)
                continue;
            std::vector<ImageCandidate> const candidates = parse_srcset(srcset->value, base);
            if (candidates.empty())
                continue;
            if (dom::Attr const* condition = source.find_attribute("media");
                condition && !css::media_query_matches(condition->value, media))
                continue;
            if (dom::Attr const* type = source.find_attribute("type");
                type && !supports_image_type(type->value))
                continue;
            dom::Attr const* sizes = source.find_attribute("sizes");
            return select_from(candidates, parse_sizes(sizes ? sizes->value : "", media));
        }
    }

    // The img's own attributes: srcset, with src as the 1x candidate unless
    // a candidate already claims 1x or widths are in play.
    std::vector<ImageCandidate> candidates;
    if (dom::Attr const* srcset = img.find_attribute("srcset"))
        candidates = parse_srcset(srcset->value, base);
    if (dom::Attr const* src = img.find_attribute("src"); src && !src->value.empty()) {
        bool const claimed = std::any_of(candidates.begin(), candidates.end(),
            [](ImageCandidate const& candidate) {
                return candidate.width || (candidate.density && *candidate.density == 1);
            });
        if (!claimed) {
            if (std::optional<net::Url> url = net::parse_url(src->value, base))
                candidates.push_back(ImageCandidate { std::move(*url), std::nullopt, std::nullopt });
        }
    }
    dom::Attr const* sizes = img.find_attribute("sizes");
    return select_from(candidates, parse_sizes(sizes ? sizes->value : "", media));
}

}
