#include "bindings/NodeSupport.h"

// CSSOM for the page: element.style over the style attribute, the
// computed style getComputedStyle answers, classList and the other token
// lists, dataset, and the CSS namespace object. The style attribute is
// re-parsed on every access and written back declaration by declaration,
// so what a script sets is what the cascade reads.

#include "core/Unicode.h"
#include "core/Ascii.h"
#include "css/Animation.h"
#include "css/Parser.h"
#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "css/Token.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// --- Serialization ------------------------------------------------------------------

void serialize_values(std::vector<css::ComponentValue> const& values, std::string& out);

void serialize_token(css::Token const& token, std::string& out)
{
    using Type = css::Token::Type;
    switch (token.type) {
    case Type::Ident: out += token.value; break;
    case Type::Function: out += token.value + "("; break;
    case Type::AtKeyword: out += "@" + token.value; break;
    case Type::Hash: out += "#" + token.value; break;
    case Type::String: {
        out += '"';
        for (char const c : token.value) {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        out += '"';
        break;
    }
    case Type::BadString:
    case Type::BadUrl:
        break;
    case Type::Url: out += "url(" + token.value + ")"; break;
    case Type::Delim: append_utf8(out, token.delim); break;
    case Type::Number: out += js::number_to_utf8(token.numeric_value); break;
    case Type::Percentage: out += js::number_to_utf8(token.numeric_value) + "%"; break;
    case Type::Dimension: out += js::number_to_utf8(token.numeric_value) + token.unit; break;
    case Type::UnicodeRange: break;
    case Type::Whitespace: out += ' '; break;
    case Type::CDO: out += "<!--"; break;
    case Type::CDC: out += "-->"; break;
    case Type::Colon: out += ':'; break;
    case Type::Semicolon: out += ';'; break;
    case Type::Comma: out += ','; break;
    case Type::OpenSquare: out += '['; break;
    case Type::CloseSquare: out += ']'; break;
    case Type::OpenParen: out += '('; break;
    case Type::CloseParen: out += ')'; break;
    case Type::OpenBrace: out += '{'; break;
    case Type::CloseBrace: out += '}'; break;
    case Type::EndOfFile: break;
    }
}

void serialize_values(std::vector<css::ComponentValue> const& values, std::string& out)
{
    for (css::ComponentValue const& value : values) {
        if (value.is_token()) {
            serialize_token(value.token(), out);
        } else if (value.is_function()) {
            out += value.function().name + "(";
            serialize_values(value.function().values, out);
            out += ")";
        } else {
            css::SimpleBlock const& block = value.block();
            out += block.open == css::Token::Type::OpenBrace ? '{' : block.open == css::Token::Type::OpenSquare ? '[' : '(';
            serialize_values(block.values, out);
            out += block.open == css::Token::Type::OpenBrace ? '}' : block.open == css::Token::Type::OpenSquare ? ']' : ')';
        }
    }
}

std::string value_text(css::Declaration const& declaration)
{
    std::string out;
    serialize_values(declaration.value, out);
    std::size_t start = 0;
    std::size_t end = out.size();
    while (start < end && out[start] == ' ')
        ++start;
    while (end > start && out[end - 1] == ' ')
        --end;
    return out.substr(start, end - start);
}

std::string serialize_declarations(std::vector<css::Declaration> const& declarations)
{
    std::string out;
    for (css::Declaration const& declaration : declarations) {
        if (!out.empty())
            out += ' ';
        out += declaration.name + ": " + value_text(declaration);
        if (declaration.important)
            out += " !important";
        out += ';';
    }
    return out;
}

std::vector<css::Declaration> declarations_of(dom::Element const& element)
{
    dom::Attr const* attribute = element.find_attribute("style");
    if (!attribute)
        return {};
    return css::parse_declaration_list(attribute->value);
}

void write_declarations(Realm::Internals& in, dom::Element& element, std::vector<css::Declaration> const& declarations)
{
    if (declarations.empty()) {
        if (element.has_attribute("style"))
            set_attribute(in, element, "style", "");
    } else {
        set_attribute(in, element, "style", serialize_declarations(declarations));
    }
    // The CSSOM wrote it: not inline style, so a style policy lets it be.
    for (dom::Attr& attribute : element.attributes()) {
        if (attribute.local_name == "style" && attribute.prefix.empty())
            attribute.from_cssom = true;
    }
}

// camelCase to the dashed property name; cssFloat is float.
std::string css_property_name(std::string_view camel)
{
    if (camel == "cssFloat")
        return "float";
    std::string out;
    for (char const c : camel) {
        if (c >= 'A' && c <= 'Z') {
            out += '-';
            out += static_cast<char>(c - 'A' + 'a');
        } else {
            out += c;
        }
    }
    // -webkit-foo is written webkitFoo: the leading dash comes back.
    if (out.starts_with("webkit-") || out.starts_with("moz-") || out.starts_with("ms-"))
        out = "-" + out;
    return out;
}

// Sets or removes one declaration of a list; whether the list changed.
bool edit_declaration(std::vector<css::Declaration>& declarations, std::string const& name, std::string const& value, bool important)
{
    std::string const lower = ascii_lower(name);
    auto const existing = std::find_if(declarations.begin(), declarations.end(),
        [&lower](css::Declaration const& d) { return d.name == lower; });
    if (value.empty()) {
        if (existing == declarations.end())
            return false;
        declarations.erase(existing);
        return true;
    }
    std::vector<css::Declaration> parsed = css::parse_declaration_list(lower + ": " + value);
    if (parsed.empty())
        return false; // not a declaration: ignored, as the CSSOM says
    parsed.front().important = important;
    if (existing != declarations.end())
        *existing = parsed.front();
    else
        declarations.push_back(parsed.front());
    return true;
}

// The declarations a CSSStyleDeclaration that is not a computed style
// reads: its rule's, or its element's style attribute.
std::vector<css::Declaration> declarations_of(StyleDeclarationObject const& style)
{
    if (style.store)
        return style.store->read();
    dom::Element const* const element = style.element();
    return element ? declarations_of(*element) : std::vector<css::Declaration> {};
}

void write_declarations(Realm::Internals& in, StyleDeclarationObject& style, std::vector<css::Declaration> const& declarations)
{
    if (style.store)
        style.store->write(declarations);
    else if (dom::Element* const element = style.element())
        write_declarations(in, *element, declarations);
}

// Sets or removes one declaration of a style attribute or a rule.
void set_declaration(Realm::Internals& in, StyleDeclarationObject& style, std::string const& name, std::string const& value, bool important)
{
    std::vector<css::Declaration> declarations = declarations_of(style);
    if (edit_declaration(declarations, name, value, important))
        write_declarations(in, style, declarations);
}

std::string declaration_value(StyleDeclarationObject const& style, std::string const& name)
{
    std::string const lower = ascii_lower(name);
    for (css::Declaration const& declaration : declarations_of(style)) {
        if (declaration.name == lower)
            return value_text(declaration);
    }
    return "";
}

// --- Computed values ----------------------------------------------------------------

std::string px(float value)
{
    return js::number_to_utf8(std::round(static_cast<double>(value) * 1000) / 1000) + "px";
}

std::string color_text(Color const& color)
{
    if (color.a == 255)
        return "rgb(" + std::to_string(color.r) + ", " + std::to_string(color.g) + ", " + std::to_string(color.b) + ")";
    if (color.a == 0)
        return "rgba(0, 0, 0, 0)";
    return "rgba(" + std::to_string(color.r) + ", " + std::to_string(color.g) + ", " + std::to_string(color.b) + ", "
        + js::number_to_utf8(std::round(static_cast<double>(color.a) / 255 * 1000) / 1000) + ")";
}

std::string length_text(css::LengthPercent const& length)
{
    switch (length.kind) {
    case css::LengthPercent::Kind::Auto: return "auto";
    case css::LengthPercent::Kind::Px: return px(length.value);
    case css::LengthPercent::Kind::Percent: return js::number_to_utf8(static_cast<double>(length.percent == 0 ? length.value : length.percent)) + "%";
    case css::LengthPercent::Kind::Calc: return "calc(" + js::number_to_utf8(static_cast<double>(length.percent)) + "% + " + px(length.value) + ")";
    case css::LengthPercent::Kind::MinContent: return "min-content";
    case css::LengthPercent::Kind::MaxContent: return "max-content";
    case css::LengthPercent::Kind::FitContent: return "fit-content";
    }
    return "auto";
}

std::string display_text(css::Display display)
{
    using D = css::Display;
    switch (display) {
    case D::Block: return "block";
    case D::Inline: return "inline";
    case D::ListItem: return "list-item";
    case D::FlowRoot: return "flow-root";
    case D::Flex: return "flex";
    case D::Grid: return "grid";
    case D::InlineBlock: return "inline-block";
    case D::InlineFlex: return "inline-flex";
    case D::InlineGrid: return "inline-grid";
    case D::Table: return "table";
    case D::InlineTable: return "inline-table";
    case D::TableRowGroup: return "table-row-group";
    case D::TableHeaderGroup: return "table-header-group";
    case D::TableFooterGroup: return "table-footer-group";
    case D::TableRow: return "table-row";
    case D::TableCell: return "table-cell";
    case D::TableCaption: return "table-caption";
    case D::TableColumnGroup: return "table-column-group";
    case D::TableColumn: return "table-column";
    case D::Contents: return "contents";
    case D::None: return "none";
    }
    return "none";
}

std::string border_style_text(css::BorderStyle style)
{
    switch (static_cast<int>(style)) {
    case 0: return "none";
    case 1: return "hidden";
    case 2: return "solid";
    case 3: return "dotted";
    case 4: return "dashed";
    case 5: return "double";
    case 6: return "groove";
    case 7: return "ridge";
    case 8: return "inset";
    case 9: return "outset";
    default: return "none";
    }
}

// --- Flex, grid and alignment as getComputedStyle writes them ---

std::string number_text(float value) { return js::number_to_utf8(static_cast<double>(value)); }

std::string breadth_text(css::TrackBreadth const& breadth)
{
    using K = css::TrackBreadth::Kind;
    switch (breadth.kind) {
    case K::Length: return length_text(breadth.length);
    case K::Flex: return number_text(breadth.fr) + "fr";
    case K::Auto: return "auto";
    case K::MinContent: return "min-content";
    case K::MaxContent: return "max-content";
    }
    return "auto";
}

// A track size as written (css-grid-1 §7.2): one breadth when the minimum
// and maximum are the same one, a flex factor alone over its auto minimum,
// fit-content() by its cap, minmax() otherwise.
std::string track_size_text(css::TrackSize const& size)
{
    if (size.fit_content)
        return "fit-content(" + length_text(*size.fit_content) + ")";
    std::string const min = breadth_text(size.min);
    std::string const max = breadth_text(size.max);
    if (min == max || (size.max.is_flexible() && size.min.kind == css::TrackBreadth::Kind::Auto))
        return max;
    return "minmax(" + min + ", " + max + ")";
}

std::string line_names_text(std::vector<std::string> const& names)
{
    if (names.empty())
        return {};
    std::string out = "[";
    for (std::size_t i = 0; i < names.size(); ++i)
        out += (i ? " " : "") + names[i];
    return out + "]";
}

std::string track_list_text(std::shared_ptr<css::GridTrackList const> const& list)
{
    if (!list || list->empty())
        return "none";
    std::string out;
    auto const add = [&](std::string const& part) {
        if (part.empty())
            return;
        if (!out.empty())
            out += ' ';
        out += part;
    };
    auto const add_repeat = [&] {
        std::string inner;
        auto const inner_add = [&](std::string const& part) {
            if (part.empty())
                return;
            if (!inner.empty())
                inner += ' ';
            inner += part;
        };
        inner_add(line_names_text(list->auto_repeat_leading_names));
        for (css::GridTrackList::Track const& track : list->auto_repeat_tracks) {
            inner_add(line_names_text(track.names));
            inner_add(track_size_text(track.size));
        }
        inner_add(line_names_text(list->auto_repeat_trailing_names));
        add(std::string("repeat(") + (list->auto_repeat == css::GridTrackList::AutoRepeat::Fit ? "auto-fit" : "auto-fill") + ", " + inner + ")");
    };
    for (std::size_t i = 0; i < list->tracks.size(); ++i) {
        if (list->auto_repeat != css::GridTrackList::AutoRepeat::None && list->auto_repeat_at == i)
            add_repeat();
        add(line_names_text(list->tracks[i].names));
        add(track_size_text(list->tracks[i].size));
    }
    if (list->auto_repeat != css::GridTrackList::AutoRepeat::None && list->auto_repeat_at >= list->tracks.size())
        add_repeat();
    add(line_names_text(list->trailing_names));
    return out;
}

std::string grid_line_text(css::GridLine const& line)
{
    using K = css::GridLine::Kind;
    switch (line.kind) {
    case K::Auto: return "auto";
    case K::Line: return std::to_string(line.number) + (line.name.empty() ? "" : " " + line.name);
    case K::Name: return line.name;
    case K::Span: return "span " + (line.number == 1 && !line.name.empty() ? line.name : std::to_string(line.number) + (line.name.empty() ? "" : " " + line.name));
    }
    return "auto";
}

// The grid-row, grid-column and grid-area shorthands (css-grid-1 §8.4): a
// later line is left out where the shorthand would have given it anyway —
// the earlier line's name when that is a name alone, else auto.
std::string grid_lines_text(std::vector<css::GridLine const*> lines)
{
    auto const implied = [](css::GridLine const& by, css::GridLine const& line) {
        if (by.kind == css::GridLine::Kind::Name)
            return line.kind == css::GridLine::Kind::Name && line.name == by.name;
        return line.is_auto();
    };
    // grid-area: column-end by column-start, row-end by row-start, then
    // column-start by row-start, each only once the ones after it are gone.
    std::size_t kept = lines.size();
    if (lines.size() == 4) {
        if (implied(*lines[1], *lines[3])) {
            kept = 3;
            if (implied(*lines[0], *lines[2])) {
                kept = 2;
                if (implied(*lines[0], *lines[1]))
                    kept = 1;
            }
        }
    } else if (lines.size() == 2 && implied(*lines[0], *lines[1])) {
        kept = 1;
    }
    std::string out;
    for (std::size_t i = 0; i < kept; ++i)
        out += (i ? " / " : "") + grid_line_text(*lines[i]);
    return out;
}

// grid-template-areas: one string per row, a cell no area covers a dot.
std::string grid_areas_text(std::shared_ptr<css::GridAreas const> const& areas)
{
    if (!areas || areas->rows <= 0 || areas->columns <= 0)
        return "none";
    std::vector<std::vector<std::string>> cells(static_cast<std::size_t>(areas->rows),
        std::vector<std::string>(static_cast<std::size_t>(areas->columns), "."));
    for (css::GridAreas::Area const& area : areas->areas) {
        for (int row = area.row_start; row < area.row_end && row <= areas->rows; ++row) {
            for (int column = area.column_start; column < area.column_end && column <= areas->columns; ++column)
                cells[static_cast<std::size_t>(row - 1)][static_cast<std::size_t>(column - 1)] = area.name;
        }
    }
    std::string out;
    for (std::vector<std::string> const& row : cells) {
        if (!out.empty())
            out += ' ';
        out += '"';
        for (std::size_t i = 0; i < row.size(); ++i)
            out += (i ? " " : "") + row[i];
        out += '"';
    }
    return out;
}

std::string alignment_text(css::AlignmentKeyword keyword, bool safe = false, bool last = false)
{
    std::string const word = css::alignment_keywords[static_cast<std::size_t>(keyword)];
    if (last && keyword == css::AlignmentKeyword::Baseline)
        return "last baseline";
    return safe ? "safe " + word : word;
}

std::string gap_text(css::LengthPercent const& gap, bool normal) { return normal ? "normal" : length_text(gap); }

// One value for a pair that says the same twice, as the shorthands serialize.
std::string pair_text(std::string const& first, std::string const& second)
{
    return first == second ? first : first + " " + second;
}

// The four sides' values as a box shorthand writes them: as few as say them all.
std::string sides_text(std::string const& top, std::string const& right, std::string const& bottom, std::string const& left)
{
    if (left != right)
        return top + " " + right + " " + bottom + " " + left;
    if (top != bottom)
        return top + " " + right + " " + bottom;
    if (top != right)
        return top + " " + right;
    return top;
}

// The custom properties an element sees, by name: its own over its base,
// an invalid one hiding the inherited one of the same name.
void custom_property_names(css::CustomProperties const& set, std::vector<std::string>& names)
{
    for (css::CustomProperties::Entry const& entry : set.entries) {
        if (entry->valid && std::find(names.begin(), names.end(), entry->name) == names.end())
            names.push_back(entry->name);
    }
    if (set.base)
        custom_property_names(*set.base, names);
}

std::string custom_property_text(css::ComputedStyle const& style, std::string const& name)
{
    if (!style.custom)
        return {};
    std::vector<css::ComponentValue> const* const value = style.custom->find(name);
    if (!value)
        return {};
    std::string out;
    serialize_values(*value, out);
    // The value without the whitespace around it (css-variables-1 §2.1).
    std::size_t const begin = out.find_first_not_of(" \t\n\r\f");
    if (begin == std::string::npos)
        return {};
    return out.substr(begin, out.find_last_not_of(" \t\n\r\f") - begin + 1);
}

// The computed value of one property as getComputedStyle spells it; empty
// for a property the engine does not compute.
// --- The animation-* and transition-* properties as getComputedStyle writes them ---

std::string seconds_text(double ms) { return css::serialize_css_number(ms / 1000) + "s"; }

// CSSOM §2.1 "serialize an identifier".
std::string identifier_text(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned char const c = static_cast<unsigned char>(text[i]);
        bool const digit = c >= '0' && c <= '9';
        if (c < 0x20 || c == 0x7f || (digit && (i == 0 || (i == 1 && text[0] == '-')))) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\%x ", static_cast<unsigned>(c));
            out += buffer;
        } else if (i == 0 && c == '-' && text.size() == 1) {
            out += "\\-";
        } else if (c >= 0x80 || c == '-' || c == '_' || digit || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            out += static_cast<char>(c);
        } else {
            out += '\\';
            out += static_cast<char>(c);
        }
    }
    return out;
}

// An animation name: an identifier, but a string for a name that would
// read back as a keyword.
std::string animation_name_text(std::optional<std::string> const& name)
{
    if (!name)
        return "none";
    for (std::string_view const reserved : { "none", "initial", "inherit", "unset", "revert", "revert-layer", "default" }) {
        if (ascii_ci_equals(*name, reserved)) {
            std::string out = "\"";
            for (char const c : *name) {
                if (c == '"' || c == '\\')
                    out += '\\';
                out += c;
            }
            return out + "\"";
        }
    }
    return identifier_text(*name);
}

std::string_view direction_text(css::PlaybackDirection direction)
{
    switch (direction) {
    case css::PlaybackDirection::Normal: return "normal";
    case css::PlaybackDirection::Reverse: return "reverse";
    case css::PlaybackDirection::Alternate: return "alternate";
    case css::PlaybackDirection::AlternateReverse: return "alternate-reverse";
    }
    return "normal";
}

std::string_view fill_text(css::FillMode fill)
{
    switch (fill) {
    case css::FillMode::Forwards: return "forwards";
    case css::FillMode::Backwards: return "backwards";
    case css::FillMode::Both: return "both";
    default: return "none";
    }
}

std::string_view composition_text(css::CompositeOperation operation)
{
    switch (operation) {
    case css::CompositeOperation::Add: return "add";
    case css::CompositeOperation::Accumulate: return "accumulate";
    default: return "replace";
    }
}

template<typename T, typename Text>
std::string list_text(std::vector<T> const& list, Text&& text)
{
    std::string out;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i)
            out += ", ";
        out += text(list[i]);
    }
    return out;
}

std::string count_text(double count) { return std::isinf(count) ? "infinite" : css::serialize_css_number(count); }

std::string animation_property_text(css::ComputedStyle const& style, std::string const& name)
{
    css::AnimationLists const animation = style.animation ? *style.animation : css::AnimationLists {};
    css::TransitionLists const transition = style.transition ? *style.transition : css::TransitionLists {};
    auto const duration = [](std::optional<double> const& ms) { return seconds_text(ms.value_or(0)); };
    auto const easing = [](css::Easing const& e) { return e.serialize(); };
    if (name == "animation-name")
        return list_text(animation.names, animation_name_text);
    if (name == "animation-duration")
        return list_text(animation.durations, duration);
    if (name == "animation-timing-function")
        return list_text(animation.timing_functions, easing);
    if (name == "animation-delay")
        return list_text(animation.delays, seconds_text);
    if (name == "animation-iteration-count")
        return list_text(animation.iteration_counts, count_text);
    if (name == "animation-direction")
        return list_text(animation.directions, [](css::PlaybackDirection d) { return std::string(direction_text(d)); });
    if (name == "animation-fill-mode")
        return list_text(animation.fill_modes, [](css::FillMode f) { return std::string(fill_text(f)); });
    if (name == "animation-play-state")
        return list_text(animation.paused, [](bool paused) { return std::string(paused ? "paused" : "running"); });
    if (name == "animation-composition")
        return list_text(animation.compositions, [](css::CompositeOperation c) { return std::string(composition_text(c)); });
    if (name == "animation-timeline")
        return list_text(animation.names, [](auto const&) { return std::string("auto"); });
    if (name == "animation") {
        // Each animation's parts in the shorthand's order, the initial ones
        // left out; nothing when a longhand's list does not match the
        // names' or holds what the shorthand cannot say.
        std::size_t const n = animation.names.size();
        if (animation.durations.size() != n || animation.timing_functions.size() != n || animation.delays.size() != n
            || animation.iteration_counts.size() != n || animation.directions.size() != n || animation.fill_modes.size() != n
            || animation.paused.size() != n || animation.compositions.size() != n)
            return "";
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            if (animation.compositions[i] != css::CompositeOperation::Replace)
                return "";
            std::vector<std::string> parts;
            double const delay = animation.delays[i];
            if (animation.durations[i].value_or(0) != 0 || delay != 0)
                parts.push_back(duration(animation.durations[i]));
            if (!(animation.timing_functions[i] == css::Easing::ease()))
                parts.push_back(animation.timing_functions[i].serialize());
            if (delay != 0)
                parts.push_back(seconds_text(delay));
            if (animation.iteration_counts[i] != 1)
                parts.push_back(count_text(animation.iteration_counts[i]));
            if (animation.directions[i] != css::PlaybackDirection::Normal)
                parts.push_back(std::string(direction_text(animation.directions[i])));
            if (animation.fill_modes[i] != css::FillMode::None)
                parts.push_back(std::string(fill_text(animation.fill_modes[i])));
            if (animation.paused[i])
                parts.push_back("paused");
            if (animation.names[i] || parts.empty())
                parts.push_back(animation_name_text(animation.names[i]));
            if (i)
                out += ", ";
            for (std::size_t p = 0; p < parts.size(); ++p)
                out += (p ? " " : "") + parts[p];
        }
        return out;
    }
    if (name == "transition-property")
        return transition.properties.empty() ? "none" : list_text(transition.properties, identifier_text);
    if (name == "transition-duration")
        return list_text(transition.durations, seconds_text);
    if (name == "transition-timing-function")
        return list_text(transition.timing_functions, easing);
    if (name == "transition-delay")
        return list_text(transition.delays, seconds_text);
    if (name == "transition-behavior")
        return list_text(transition.allow_discrete, [](bool allow) { return std::string(allow ? "allow-discrete" : "normal"); });
    if (name == "transition") {
        std::size_t const n = std::max<std::size_t>(transition.properties.size(), 1);
        if (transition.durations.size() != n || transition.timing_functions.size() != n || transition.delays.size() != n
            || transition.allow_discrete.size() != n)
            return "";
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<std::string> parts;
            std::string const property = transition.properties.empty() ? "none" : identifier_text(transition.properties[i]);
            if (property != "all")
                parts.push_back(property);
            double const delay = transition.delays[i];
            if (transition.durations[i] != 0 || delay != 0)
                parts.push_back(seconds_text(transition.durations[i]));
            if (!(transition.timing_functions[i] == css::Easing::ease()))
                parts.push_back(transition.timing_functions[i].serialize());
            if (delay != 0)
                parts.push_back(seconds_text(delay));
            if (transition.allow_discrete[i])
                parts.push_back("allow-discrete");
            if (parts.empty())
                parts.push_back("all");
            if (i)
                out += ", ";
            for (std::size_t p = 0; p < parts.size(); ++p)
                out += (p ? " " : "") + parts[p];
        }
        return out;
    }
    return "";
}

// `resolved` false: the computed value alone, never what layout made of it.
std::string computed_property(Realm::Internals& in, dom::Element& element, css::ComputedStyle const& style, std::string const& name,
    bool resolved = true)
{
    using namespace css;
    std::optional<LayoutBox> box;
    auto const box_of = [&]() -> std::optional<LayoutBox> {
        if (!box && resolved && in.hooks.layout_box)
            box = in.hooks.layout_box(element);
        return box;
    };
    if (name == "display")
        return display_text(style.display);
    if (name == "position") {
        switch (style.position) {
        case Position::Static: return "static";
        case Position::Relative: return "relative";
        case Position::Absolute: return "absolute";
        case Position::Fixed: return "fixed";
        case Position::Sticky: return "sticky";
        }
    }
    if (name == "visibility")
        return style.visibility == Visibility::Visible ? "visible" : "hidden";
    if (name == "opacity")
        return js::number_to_utf8(static_cast<double>(style.opacity));
    if (name == "float")
        return style.floating == Float::None ? "none" : style.floating == Float::Left ? "left" : "right";
    if (name == "clear") {
        switch (style.clear) {
        case Clear::None: return "none";
        case Clear::Left: return "left";
        case Clear::Right: return "right";
        case Clear::Both: return "both";
        }
    }
    auto const overflow_text = [](Overflow overflow) -> std::string {
        switch (overflow) {
        case Overflow::Visible: return "visible";
        case Overflow::Clip: return "clip";
        case Overflow::Hidden: return "hidden";
        case Overflow::Auto: return "auto";
        case Overflow::Scroll: return "scroll";
        }
        return "visible";
    };
    if (name == "overflow")
        return overflow_text(style.overflow);
    if (name == "overflow-x")
        return overflow_text(style.overflow_x);
    if (name == "overflow-y")
        return overflow_text(style.overflow_y);
    if (name == "width" || name == "height") {
        // The used value, which is what the property sizes: the content box,
        // or the border box under box-sizing: border-box. The style's own
        // lengths are the engine's px; the box is CSS px.
        if (std::optional<LayoutBox> const b = box_of()) {
            bool const wide = name == "width";
            float used = wide ? b->width : b->height;
            if (style.box_sizing != BoxSizing::BorderBox) {
                float const scale = in.hooks.device_scale > 0 ? in.hooks.device_scale : 1.0f;
                auto const fixed = [](LengthPercent const& length) {
                    return length.kind == LengthPercent::Kind::Px ? length.value : 0.0f;
                };
                float const edges = wide
                    ? style.border_left.width + style.border_right.width + fixed(style.padding_left) + fixed(style.padding_right)
                    : style.border_top.width + style.border_bottom.width + fixed(style.padding_top) + fixed(style.padding_bottom);
                used = std::max(0.0f, used - edges / scale);
            }
            return px(used);
        }
        return length_text(name == "width" ? style.width : style.height);
    }
    if (name == "min-width") return length_text(style.min_width);
    if (name == "max-width") return length_text(style.max_width);
    if (name == "min-height") return length_text(style.min_height);
    if (name == "max-height") return length_text(style.max_height);
    if (name == "margin-top") return length_text(style.margin_top);
    if (name == "margin-right") return length_text(style.margin_right);
    if (name == "margin-bottom") return length_text(style.margin_bottom);
    if (name == "margin-left") return length_text(style.margin_left);
    if (name == "padding-top") return length_text(style.padding_top);
    if (name == "padding-right") return length_text(style.padding_right);
    if (name == "padding-bottom") return length_text(style.padding_bottom);
    if (name == "padding-left") return length_text(style.padding_left);
    if (name == "top") return length_text(style.top);
    if (name == "right") return length_text(style.right);
    if (name == "bottom") return length_text(style.bottom);
    if (name == "left") return length_text(style.left);
    if (name == "z-index")
        return style.z_index ? std::to_string(*style.z_index) : "auto";
    if (name == "box-sizing")
        return style.box_sizing == BoxSizing::BorderBox ? "border-box" : "content-box";
    if (name == "appearance" || name == "-webkit-appearance")
        return appearance_keywords[static_cast<std::size_t>(style.appearance)];
    if (name == "aspect-ratio") {
        if (style.aspect_ratio.ratio <= 0)
            return "auto";
        std::string const ratio = js::number_to_utf8(static_cast<double>(style.aspect_ratio.ratio)) + " / 1";
        return style.aspect_ratio.with_auto ? "auto " + ratio : ratio;
    }
    if (name == "object-fit") {
        switch (style.object_fit) {
        case ObjectFit::Fill: return "fill";
        case ObjectFit::Contain: return "contain";
        case ObjectFit::Cover: return "cover";
        case ObjectFit::None: return "none";
        case ObjectFit::ScaleDown: return "scale-down";
        }
    }
    if (name == "object-position")
        return length_text(style.object_position_x) + " " + length_text(style.object_position_y);
    if (name == "color")
        return color_text(style.color);
    if (name == "background-color")
        return color_text(style.background_color);
    if (name == "font-size")
        return px(style.font_size);
    if (name == "font-weight")
        return std::to_string(style.font_weight);
    if (name == "font-style") {
        switch (style.font_style) {
        case FontStyle::Normal: return "normal";
        case FontStyle::Italic: return "italic";
        case FontStyle::Oblique: return "oblique";
        }
    }
    if (name == "font-family") {
        if (!style.font_family)
            return "serif";
        std::string out;
        for (std::string const& family : *style.font_family) {
            if (!out.empty())
                out += ", ";
            bool const spaces = family.find(' ') != std::string::npos;
            out += spaces ? "\"" + family + "\"" : family;
        }
        return out;
    }
    if (name == "line-height") {
        switch (style.line_height.kind) {
        case LineHeight::Kind::Normal: return "normal";
        case LineHeight::Kind::Number: return px(style.line_height.value * style.font_size);
        case LineHeight::Kind::Px: return px(style.line_height.value);
        }
    }
    if (name == "text-align") {
        switch (style.text_align) {
        case TextAlign::Start: return "start";
        case TextAlign::End: return "end";
        case TextAlign::Left: return "left";
        case TextAlign::Right: return "right";
        case TextAlign::Center: return "center";
        case TextAlign::Justify: return "justify";
        case TextAlign::MatchParent: return "match-parent";
        }
    }
    if (name == "text-decoration" || name == "text-decoration-line") {
        switch (style.text_decoration) {
        case TextDecorationLine::None: return "none";
        case TextDecorationLine::Underline: return "underline";
        case TextDecorationLine::LineThrough: return "line-through";
        }
    }
    if (name == "text-transform") {
        switch (style.text_transform) {
        case TextTransform::None: return "none";
        case TextTransform::Capitalize: return "capitalize";
        case TextTransform::Uppercase: return "uppercase";
        case TextTransform::Lowercase: return "lowercase";
        case TextTransform::MathAuto: return "math-auto";
        }
    }
    if (name == "white-space") {
        switch (style.white_space) {
        case WhiteSpace::Normal: return "normal";
        case WhiteSpace::Pre: return "pre";
        case WhiteSpace::NoWrap: return "nowrap";
        case WhiteSpace::PreWrap: return "pre-wrap";
        case WhiteSpace::PreLine: return "pre-line";
        case WhiteSpace::BreakSpaces: return "break-spaces";
        }
    }
    if (name == "word-break") {
        switch (style.word_break) {
        case WordBreak::Normal: return "normal";
        case WordBreak::BreakAll: return "break-all";
        case WordBreak::KeepAll: return "keep-all";
        case WordBreak::BreakWord: return "break-word";
        case WordBreak::Manual: return "manual";
        case WordBreak::AutoPhrase: return "auto-phrase";
        }
    }
    if (name == "hyphens") {
        switch (style.hyphens) {
        case Hyphens::None: return "none";
        case Hyphens::Manual: return "manual";
        case Hyphens::Auto: return "auto";
        }
    }
    if (name == "hyphenate-character") {
        if (!style.hyphenate_character)
            return "auto";
        std::string quoted = "\"";
        for (char const c : *style.hyphenate_character) {
            if (c == '"' || c == '\\')
                quoted.push_back('\\');
            quoted.push_back(c);
        }
        return quoted + "\"";
    }
    if (name == "font-stretch")
        return std::to_string(style.font_stretch) + "%";
    if (name == "font-kerning") {
        switch (style.font_kerning) {
        case FontKerning::Auto: return "auto";
        case FontKerning::Normal: return "normal";
        case FontKerning::None: return "none";
        }
    }
    if (name == "font-synthesis-weight")
        return style.font_synthesis_weight ? "auto" : "none";
    if (name == "font-synthesis-small-caps")
        return style.font_synthesis_small_caps ? "auto" : "none";
    if (name == "font-synthesis-position")
        return style.font_synthesis_position ? "auto" : "none";
    if (name == "font-synthesis-style") {
        switch (style.font_synthesis_style) {
        case FontSynthesisStyle::Auto: return "auto";
        case FontSynthesisStyle::None: return "none";
        case FontSynthesisStyle::ObliqueOnly: return "oblique-only";
        }
    }
    if (name == "font-synthesis") {
        // What may be faked, named in the grammar's order; none when nothing may.
        std::string out;
        auto const add = [&](char const* word) {
            if (!out.empty())
                out += ' ';
            out += word;
        };
        if (style.font_synthesis_weight)
            add("weight");
        if (style.font_synthesis_style == FontSynthesisStyle::Auto)
            add("style");
        else if (style.font_synthesis_style == FontSynthesisStyle::ObliqueOnly)
            add("oblique-only");
        if (style.font_synthesis_small_caps)
            add("small-caps");
        if (style.font_synthesis_position)
            add("position");
        return out.empty() ? "none" : out;
    }
    if (name == "overflow-wrap" || name == "word-wrap") {
        switch (style.overflow_wrap) {
        case OverflowWrap::Normal: return "normal";
        case OverflowWrap::BreakWord: return "break-word";
        case OverflowWrap::Anywhere: return "anywhere";
        }
    }
    if (name == "line-break") {
        switch (style.line_break) {
        case LineBreakMode::Auto: return "auto";
        case LineBreakMode::Loose: return "loose";
        case LineBreakMode::Normal: return "normal";
        case LineBreakMode::Strict: return "strict";
        case LineBreakMode::Anywhere: return "anywhere";
        }
    }
    if (name == "direction")
        return style.direction == Direction::Rtl ? "rtl" : "ltr";
    if (name == "letter-spacing")
        return style.letter_spacing == 0 ? "normal" : px(style.letter_spacing);
    if (name == "word-spacing")
        return px(style.word_spacing);
    auto const border = [&](BorderSide const& side, std::string_view part) -> std::string {
        if (part == "width")
            return px(side.width);
        if (part == "style")
            return border_style_text(side.style);
        return color_text(side.current_color ? style.color : side.color);
    };
    for (auto const& [prefix, side] : { std::pair { "border-top-", &style.border_top }, std::pair { "border-right-", &style.border_right },
             std::pair { "border-bottom-", &style.border_bottom }, std::pair { "border-left-", &style.border_left } }) {
        if (std::string_view const part = std::string_view(name).substr(std::min(name.size(), std::string_view(prefix).size()));
            name.starts_with(prefix) && (part == "width" || part == "style" || part == "color"))
            return border(*side, part);
    }
    if (name.starts_with("--"))
        return custom_property_text(style, name);
    // The shorthands of the box: each side's value, as few as say them all.
    if (name == "margin" || name == "padding") {
        auto const side = [&](std::string const& which) { return computed_property(in, element, style, name + "-" + which, resolved); };
        return sides_text(side("top"), side("right"), side("bottom"), side("left"));
    }
    if (name == "border-width" || name == "border-style" || name == "border-color") {
        std::string const part = name.substr(7);
        return sides_text(border(style.border_top, part), border(style.border_right, part), border(style.border_bottom, part),
            border(style.border_left, part));
    }
    if (name == "border" || name == "border-top" || name == "border-right" || name == "border-bottom" || name == "border-left") {
        auto const whole = [&](BorderSide const& side) { return border(side, "width") + " " + border(side, "style") + " " + border(side, "color"); };
        if (name == "border-top") return whole(style.border_top);
        if (name == "border-right") return whole(style.border_right);
        if (name == "border-bottom") return whole(style.border_bottom);
        if (name == "border-left") return whole(style.border_left);
        std::string const top = whole(style.border_top);
        // The shorthand stands for all four sides only when they agree.
        return top == whole(style.border_right) && top == whole(style.border_bottom) && top == whole(style.border_left) ? top : "";
    }
    auto const corner = [](CornerRadius const& radius) { return pair_text(length_text(radius.x), length_text(radius.y)); };
    if (name == "border-top-left-radius") return corner(style.border_top_left_radius);
    if (name == "border-top-right-radius") return corner(style.border_top_right_radius);
    if (name == "border-bottom-right-radius") return corner(style.border_bottom_right_radius);
    if (name == "border-bottom-left-radius") return corner(style.border_bottom_left_radius);
    if (name == "border-radius") {
        CornerRadius const* const corners[] = { &style.border_top_left_radius, &style.border_top_right_radius,
            &style.border_bottom_right_radius, &style.border_bottom_left_radius };
        std::string const horizontal = sides_text(length_text(corners[0]->x), length_text(corners[1]->x), length_text(corners[2]->x), length_text(corners[3]->x));
        std::string const vertical = sides_text(length_text(corners[0]->y), length_text(corners[1]->y), length_text(corners[2]->y), length_text(corners[3]->y));
        return horizontal == vertical ? horizontal : horizontal + " / " + vertical;
    }
    if (name == "border-collapse")
        return style.border_collapse == BorderCollapse::Collapse ? "collapse" : "separate";
    if (name == "border-spacing")
        return length_text(style.border_spacing_horizontal) + " " + length_text(style.border_spacing_vertical);
    // The outline: a width of zero while it has no style (css-ui-4 §3.2).
    bool const outlined = style.outline.automatic || style.outline.style != BorderStyle::None;
    std::string const outline_style = style.outline.automatic ? "auto" : border_style_text(style.outline.style);
    std::string const outline_color = color_text(style.outline.current_color ? style.color : style.outline.color);
    std::string const outline_width = px(outlined ? style.outline.width : 0);
    if (name == "outline-style") return outline_style;
    if (name == "outline-color") return outline_color;
    if (name == "outline-width") return outline_width;
    if (name == "outline-offset") return px(style.outline.offset);
    if (name == "outline") return outline_color + " " + outline_style + " " + outline_width;
    // Flex containers and items.
    auto const direction_text = [&] {
        switch (style.flex_direction) {
        case FlexDirection::Row: return "row";
        case FlexDirection::RowReverse: return "row-reverse";
        case FlexDirection::Column: return "column";
        case FlexDirection::ColumnReverse: return "column-reverse";
        }
        return "row";
    };
    auto const wrap_text = [&] {
        switch (style.flex_wrap) {
        case FlexWrap::NoWrap: return "nowrap";
        case FlexWrap::Wrap: return "wrap";
        case FlexWrap::WrapReverse: return "wrap-reverse";
        }
        return "nowrap";
    };
    if (name == "flex-direction") return direction_text();
    if (name == "flex-wrap") return wrap_text();
    if (name == "flex-flow") return std::string(direction_text()) + " " + wrap_text();
    if (name == "flex-grow") return number_text(style.flex_grow);
    if (name == "flex-shrink") return number_text(style.flex_shrink);
    if (name == "flex-basis") return length_text(style.flex_basis);
    if (name == "flex") return number_text(style.flex_grow) + " " + number_text(style.flex_shrink) + " " + length_text(style.flex_basis);
    if (name == "order") return std::to_string(style.order);
    // Alignment, as written.
    if (name == "justify-content") return alignment_text(style.justify_content_keyword);
    if (name == "align-content") return alignment_text(style.align_content_keyword);
    if (name == "align-items") return alignment_text(style.align_items_keyword);
    if (name == "justify-items") return alignment_text(style.justify_items_keyword);
    if (name == "align-self") return alignment_text(style.align_self_keyword, style.align_self_safe, style.align_self_last);
    if (name == "justify-self") return alignment_text(style.justify_self_keyword, style.justify_self_safe, style.justify_self_last);
    if (name == "place-content") return pair_text(alignment_text(style.align_content_keyword), alignment_text(style.justify_content_keyword));
    if (name == "place-items") return pair_text(alignment_text(style.align_items_keyword), alignment_text(style.justify_items_keyword));
    if (name == "place-self")
        return pair_text(alignment_text(style.align_self_keyword, style.align_self_safe, style.align_self_last),
            alignment_text(style.justify_self_keyword, style.justify_self_safe, style.justify_self_last));
    // The gutters.
    if (name == "row-gap" || name == "grid-row-gap") return gap_text(style.row_gap, style.row_gap_normal);
    if (name == "column-gap" || name == "grid-column-gap") return gap_text(style.column_gap, style.column_gap_normal);
    if (name == "gap" || name == "grid-gap")
        return pair_text(gap_text(style.row_gap, style.row_gap_normal), gap_text(style.column_gap, style.column_gap_normal));
    // Grid containers and items.
    if (name == "grid-template-columns") return track_list_text(style.grid_template_columns);
    if (name == "grid-template-rows") return track_list_text(style.grid_template_rows);
    if (name == "grid-template-areas") return grid_areas_text(style.grid_template_areas);
    if (name == "grid-auto-columns" || name == "grid-auto-rows") {
        std::shared_ptr<std::vector<TrackSize> const> const& sizes = name == "grid-auto-columns" ? style.grid_auto_columns : style.grid_auto_rows;
        if (!sizes || sizes->empty())
            return "auto";
        std::string out;
        for (TrackSize const& size : *sizes)
            out += (out.empty() ? "" : " ") + track_size_text(size);
        return out;
    }
    if (name == "grid-auto-flow") {
        switch (style.grid_auto_flow) {
        case GridAutoFlow::Row: return "row";
        case GridAutoFlow::Column: return "column";
        case GridAutoFlow::RowDense: return "row dense";
        case GridAutoFlow::ColumnDense: return "column dense";
        }
    }
    if (name == "grid-row-start") return grid_line_text(style.grid_row_start);
    if (name == "grid-row-end") return grid_line_text(style.grid_row_end);
    if (name == "grid-column-start") return grid_line_text(style.grid_column_start);
    if (name == "grid-column-end") return grid_line_text(style.grid_column_end);
    if (name == "grid-row") return grid_lines_text({ &style.grid_row_start, &style.grid_row_end });
    if (name == "grid-column") return grid_lines_text({ &style.grid_column_start, &style.grid_column_end });
    if (name == "grid-area")
        return grid_lines_text({ &style.grid_row_start, &style.grid_column_start, &style.grid_row_end, &style.grid_column_end });
    if (name == "transform")
        return style.transformed ? "matrix(1, 0, 0, 1, " + js::number_to_utf8(static_cast<double>(style.translate_x.value)) + ", " + js::number_to_utf8(static_cast<double>(style.translate_y.value)) + ")" : "none";
    if (name == "pointer-events")
        return style.pointer_events ? "auto" : "none";
    if (name == "cursor")
        return "auto";
    if (name == "content")
        return "normal";
    if (name.starts_with("animation") || name.starts_with("transition"))
        return animation_property_text(style, name);
    if (name == "background-image")
        return style.background_images && !style.background_images->empty() ? "url()" : "none";
    if (name == "box-shadow" || name == "text-shadow") {
        bool const of_box = name == "box-shadow";
        std::shared_ptr<Shadows const> const& list = of_box ? style.box_shadow : style.text_shadow;
        if (!list || list->empty())
            return "none";
        std::string out;
        for (Shadow const& shadow : *list) {
            if (!out.empty())
                out += ", ";
            out += color_text(shadow.current_color ? style.color : shadow.color) + " " + px(shadow.x) + " " + px(shadow.y) + " "
                + px(shadow.blur);
            if (of_box) {
                out += " " + px(shadow.spread);
                if (shadow.inset)
                    out += " inset";
            }
        }
        return out;
    }
    return "";
}

// The longhands a computed style lists, in the alphabetical order the
// shipping engines list theirs in: what its length counts and item() names,
// before the custom properties in force.
constexpr std::string_view computed_longhands[] = { "align-content", "align-items", "align-self", "animation-composition", "animation-delay",
    "animation-direction", "animation-duration", "animation-fill-mode", "animation-iteration-count", "animation-name",
    "animation-play-state", "animation-timeline", "animation-timing-function", "appearance", "aspect-ratio", "background-color", "background-image",
    "border-bottom-color", "border-bottom-left-radius", "border-bottom-right-radius", "border-bottom-style", "border-bottom-width",
    "border-collapse", "border-left-color", "border-left-style", "border-left-width", "border-right-color", "border-right-style",
    "border-right-width", "border-spacing", "border-top-color", "border-top-left-radius", "border-top-right-radius",
    "border-top-style", "border-top-width", "bottom", "box-shadow", "box-sizing", "clear", "color", "column-gap", "content",
    "cursor", "direction", "display", "flex-basis", "flex-direction", "flex-grow", "flex-shrink", "flex-wrap", "float",
    "font-family", "font-kerning", "font-size", "font-stretch", "font-style", "font-synthesis-position",
    "font-synthesis-small-caps", "font-synthesis-style", "font-synthesis-weight", "font-weight", "grid-auto-columns",
    "grid-auto-flow", "grid-auto-rows", "grid-column-end", "grid-column-start", "grid-row-end", "grid-row-start",
    "grid-template-areas", "grid-template-columns", "grid-template-rows", "height", "hyphenate-character", "hyphens",
    "justify-content", "justify-items", "justify-self", "left", "letter-spacing", "line-break", "line-height", "margin-bottom",
    "margin-left", "margin-right", "margin-top", "max-height", "max-width", "min-height", "min-width", "object-fit",
    "object-position", "opacity", "order", "outline-color", "outline-offset", "outline-style", "outline-width", "overflow-wrap",
    "overflow-x", "overflow-y", "padding-bottom", "padding-left", "padding-right", "padding-top", "pointer-events", "position",
    "right", "row-gap", "text-align", "text-decoration-line", "text-shadow", "text-transform", "top", "transform",
    "transition-behavior", "transition-delay", "transition-duration", "transition-property", "transition-timing-function",
    "visibility", "white-space", "width", "word-break", "word-spacing", "z-index" };

// Whether getComputedStyle's second argument leaves a style to answer
// (CSSOM §9.1): one that does not start with a colon is ignored, so the
// element's own style is the answer; one that does must be a pseudo-element
// selector naming a pseudo-element the engine knows — the legacy four with
// one colon, the rest with two, and the functional ones with one identifier
// inside, written as CSS tokenizes it (spaces inside, escapes, a closing
// parenthesis the end of the text may stand in for). Prefixed names, an
// argument a name does not take and anything after the selector fail.
bool computed_style_pseudo_ok(std::string_view text)
{
    if (text.empty() || text.front() != ':')
        return true;
    std::size_t at = 0;
    auto const ident = [&](std::string& out) {
        out.clear();
        while (at < text.size()) {
            unsigned char const c = static_cast<unsigned char>(text[at]);
            if (c == '\\' && at + 1 < text.size() && text[at + 1] != '\n') {
                ++at;
                std::size_t digits = 0;
                unsigned code = 0;
                while (at < text.size() && digits < 6 && std::isxdigit(static_cast<unsigned char>(text[at]))) {
                    char const h = text[at];
                    code = code * 16 + static_cast<unsigned>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                    ++at;
                    ++digits;
                }
                if (digits == 0) {
                    out += text[at++];
                } else {
                    if (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n'))
                        ++at;
                    out += code < 0x80 ? static_cast<char>(code) : '?';
                }
            } else if (std::isalnum(c) || c == '-' || c == '_' || c >= 0x80) {
                out += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                ++at;
            } else {
                break;
            }
        }
        // A digit, or a hyphen and a digit, cannot start an identifier.
        return !out.empty() && !std::isdigit(static_cast<unsigned char>(out[0]))
            && !(out[0] == '-' && out.size() > 1 && std::isdigit(static_cast<unsigned char>(out[1]))) && out != "-";
    };
    auto const spaces = [&] {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r' || text[at] == '\f'))
            ++at;
    };
    bool const two = text.size() > 1 && text[1] == ':';
    at = two ? 2 : 1;
    std::string name;
    if (!ident(name))
        return false;
    if (!two)
        return at == text.size() && (name == "before" || name == "after" || name == "first-line" || name == "first-letter");
    static constexpr std::string_view plain[] = { "before", "after", "marker", "placeholder", "first-line", "first-letter",
        "selection", "backdrop", "file-selector-button", "target-text", "spelling-error", "grammar-error" };
    static constexpr std::string_view functional[] = { "highlight", "picker", "view-transition-group",
        "view-transition-image-pair", "view-transition-old", "view-transition-new" };
    if (at == text.size())
        return std::find(std::begin(plain), std::end(plain), name) != std::end(plain);
    if (text[at] != '(' || std::find(std::begin(functional), std::end(functional), name) == std::end(functional))
        return false;
    ++at;
    spaces();
    std::string argument;
    if (!ident(argument))
        return false;
    spaces();
    if (at < text.size()) {
        if (text[at] != ')')
            return false;
        ++at;
    }
    if (at != text.size())
        return false;
    return name != "picker" || argument == "select";
}

// What a computed style's length counts and item() names: the longhands,
// then the custom properties the element sees.
std::vector<std::string> computed_names(Realm::Internals& in, StyleDeclarationObject const& style)
{
    // No style at all — an element outside the document or the flat tree,
    // a pseudo-element that is not one — lists nothing, as its every
    // property reads "" (CSSOM §9: the declarations are empty).
    css::ComputedStyle const* const computed = in.hooks.computed_style && style.element() ? in.hooks.computed_style(*style.element()) : nullptr;
    if (!computed)
        return {};
    std::vector<std::string> names(std::begin(computed_longhands), std::end(computed_longhands));
    if (computed->custom) {
        std::vector<std::string> custom;
        custom_property_names(*computed->custom, custom);
        std::sort(custom.begin(), custom.end());
        names.insert(names.end(), custom.begin(), custom.end());
    }
    return names;
}

// --- Token lists ----------------------------------------------------------------------

std::optional<TokenListObject*> this_token_list(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* list = dynamic_cast<TokenListObject*>(this_value.as_object()))
            return list;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

std::vector<std::string> tokens_of(TokenListObject const& list)
{
    dom::Element const* const element = list.element();
    if (!element)
        return {}; // its document is gone with a frame
    std::vector<std::string> tokens = split_tokens(attribute_or_empty(*element, list.attribute));
    // The ordered set: duplicates dropped.
    std::vector<std::string> unique;
    for (std::string& token : tokens) {
        if (std::find(unique.begin(), unique.end(), token) == unique.end())
            unique.push_back(std::move(token));
    }
    return unique;
}

void write_tokens(TokenListObject const& list, std::vector<std::string> const& tokens)
{
    if (dom::Element* const element = list.element())
        set_attribute(list.internals(), *element, list.attribute, join_tokens(tokens));
}

// A token argument must be non-empty and hold no whitespace (§7.1).
std::optional<std::string> token_argument(Realm::Internals& in, js::Value const& value)
{
    std::optional<std::string> token = in.to_utf8(value);
    if (!token)
        return std::nullopt;
    if (token->empty()) {
        in.throw_dom_exception("SyntaxError", "The token provided must not be empty.");
        return std::nullopt;
    }
    for (char const c : *token) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
            in.throw_dom_exception("InvalidCharacterError", "The token provided ('" + *token + "') contains HTML space characters, which are not valid in tokens.");
            return std::nullopt;
        }
    }
    return token;
}

std::optional<StyleDeclarationObject*> this_style(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* style = dynamic_cast<StyleDeclarationObject*>(this_value.as_object()))
            return style;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

// The dataset's camelCase name for a data-* attribute, and back.
std::string dataset_name(std::string_view attribute)
{
    std::string out;
    bool upper = false;
    for (char const c : attribute.substr(5)) {
        if (c == '-') {
            upper = true;
            continue;
        }
        out += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        upper = false;
    }
    return out;
}

std::string dataset_attribute(std::string_view name)
{
    std::string out = "data-";
    for (char const c : name) {
        if (c >= 'A' && c <= 'Z') {
            out += '-';
            out += static_cast<char>(c - 'A' + 'a');
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

// --- TokenListObject --------------------------------------------------------------------

std::optional<js::PropertyDescriptor> TokenListObject::get_own_property(js::PropertyKey const& key) const
{
    if (key.is_index()) {
        js::Heap::NoCollect const guard(*heap()); // a fresh string the caller has not rooted yet
        std::vector<std::string> const tokens = tokens_of(*this);
        if (key.as_index() < tokens.size())
            return js::PropertyDescriptor::data(js::Value::string(heap()->string(tokens[key.as_index()])), js::Enumerable);
        return std::nullopt;
    }
    return Object::get_own_property(key);
}

std::optional<js::Value> TokenListObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (key.is_index()) {
        std::vector<std::string> const tokens = tokens_of(*this);
        if (key.as_index() < tokens.size())
            return js::Value::string(interpreter.string(tokens[key.as_index()]));
        return js::Value::undefined();
    }
    return Object::get(interpreter, key, receiver);
}

// --- StyleDeclarationObject -----------------------------------------------------------------

// Whether a name is one of a CSSStyleDeclaration's attributes (CSSOM
// §6.7.1): the camel-cased attribute of each property the engine supports,
// the webkit-cased one of each -webkit- property, and the dashed one of each
// property whose name has a dash. Supported means what CSS.supports says, so
// the two never disagree.
static bool names_style_attribute(std::string_view key)
{
    if (key.empty() || key.starts_with("--"))
        return false;
    bool const dashed = key.find('-') != std::string_view::npos;
    std::string const property = dashed ? std::string(key) : css_property_name(key);
    if (property.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos)
        return false;
    // A capital first letter is the webkit-cased spelling, which only the
    // -webkit- properties have.
    if (key[0] >= 'A' && key[0] <= 'Z' && !property.starts_with("-webkit-"))
        return false;
    // The engine's properties do not change while it runs, so each name is
    // parsed once; the answer is kept per thread, as the parser's own probe is.
    // A script can ask after any number of names that are none, so the store
    // is emptied when it outgrows every name the engine has several times
    // over, and a page probing made-up names cannot grow it without end.
    thread_local std::unordered_map<std::string, bool> known;
    auto const found = known.find(property);
    if (found != known.end())
        return found->second;
    bool const supported = css::supports_condition_text_matches(property + ": inherit");
    constexpr std::size_t most_names = 4096;
    if (known.size() >= most_names)
        known.clear();
    known.emplace(property, supported);
    return supported;
}

bool StyleDeclarationObject::ordinary_property(js::PropertyKey const& key) const
{
    if (ElementBackedObject::get_own_property(key))
        return true;
    return prototype() && prototype()->has_property(key);
}

std::string StyleDeclarationObject::value_of(std::string const& name) const
{
    if (!element() && !store)
        return {};
    Realm::Internals& in = internals();
    if (computed) {
        css::ComputedStyle const* style = in.hooks.computed_style ? in.hooks.computed_style(*element()) : nullptr;
        return style ? computed_property(in, *element(), *style, name) : std::string();
    }
    return declaration_value(*this, name);
}

std::optional<bool> StyleDeclarationObject::write(std::string const& name, js::Value const& value)
{
    Realm::Internals& in = internals();
    if (!writable()) {
        in.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore the '" + name + "' property is read-only.");
        return std::nullopt;
    }
    std::optional<std::string> const text = value.is_nullish() ? std::optional<std::string>("") : in.to_utf8(value);
    if (!text)
        return std::nullopt;
    set_declaration(in, *this, name, *text, false);
    return true;
}

// --- StyleDeclarationPrototype --------------------------------------------------------------

std::optional<js::PropertyDescriptor> StyleDeclarationPrototype::get_own_property(js::PropertyKey const& key) const
{
    if (std::optional<js::PropertyDescriptor> own = Object::get_own_property(key))
        return own;
    if (!key.is_atom())
        return std::nullopt;
    std::string const spelled = key.as_atom()->to_utf8();
    if (settled.contains(spelled) || !names_style_attribute(spelled))
        return std::nullopt;
    // Settled first: the pair's own definition must not come back here.
    settled.insert(spelled);
    std::string const name = css_property_name(spelled);
    // An attribute of the prototype: enumerable and configurable (WebIDL
    // §3.7.6). The pair is made on a lookup the caller has not rooted
    // around, so nothing is collected while it is.
    js::Heap::NoCollect const guard(interpreter->heap());
    define_attribute(
        *interpreter, *const_cast<StyleDeclarationPrototype*>(this), spelled,
        [name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
            if (!s)
                return std::nullopt;
            return (*s)->internals().string((*s)->value_of(name));
        },
        [name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
            if (!s || !(*s)->write(name, js::argument(args, 0)))
                return std::nullopt;
            return js::Value::undefined();
        });
    return Object::get_own_property(key);
}

// A script redefining or deleting an attribute finds it there first, as it
// would on a browser's prototype; once made it is not made again, so a
// deleted one stays deleted.
bool StyleDeclarationPrototype::define_own_property(js::PropertyKey const& key, js::PropertyDescriptor const& descriptor)
{
    static_cast<void>(get_own_property(key));
    return Object::define_own_property(key, descriptor);
}

bool StyleDeclarationPrototype::delete_property(js::PropertyKey const& key)
{
    static_cast<void>(get_own_property(key));
    return Object::delete_property(key);
}

std::optional<js::Value> StyleDeclarationObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (!key.is_atom() || ordinary_property(key))
        return Object::get(interpreter, key, receiver);
    std::string const name = css_property_name(key.as_atom()->to_utf8());
    if (name.empty() || name.starts_with("_") || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos)
        return js::Value::undefined();
    return internals().string(value_of(name));
}

std::optional<bool> StyleDeclarationObject::set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver)
{
    if (!key.is_atom() || ordinary_property(key))
        return Object::set(interpreter, key, value, receiver);
    std::string const name = css_property_name(key.as_atom()->to_utf8());
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos)
        return Object::set(interpreter, key, value, receiver);
    return write(name, value);
}

// --- DatasetObject -------------------------------------------------------------------------

std::optional<js::PropertyDescriptor> DatasetObject::get_own_property(js::PropertyKey const& key) const
{
    if (key.is_atom() && element()) {
        js::Heap::NoCollect const guard(*heap()); // a fresh string the caller has not rooted yet
        std::string const attribute = dataset_attribute(key.as_atom()->to_utf8());
        if (dom::Attr const* found = element()->find_attribute(attribute))
            return js::PropertyDescriptor::data(js::Value::string(heap()->string(found->value)), js::default_attributes);
    }
    return Object::get_own_property(key);
}

std::optional<js::Value> DatasetObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (key.is_atom() && element()) {
        std::string const attribute = dataset_attribute(key.as_atom()->to_utf8());
        if (dom::Attr const* found = element()->find_attribute(attribute))
            return js::Value::string(interpreter.string(found->value));
    }
    return Object::get(interpreter, key, receiver);
}

std::optional<bool> DatasetObject::set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver)
{
    if (!key.is_atom())
        return Object::set(interpreter, key, value, receiver);
    dom::Element* const target = element();
    if (!target)
        return interpreter.throw_type_error("Illegal invocation");
    Realm::Internals& in = internals();
    std::optional<std::string> text = in.to_utf8(value);
    if (!text)
        return std::nullopt;
    set_attribute(in, *target, dataset_attribute(key.as_atom()->to_utf8()), std::move(*text));
    return true;
}

bool DatasetObject::delete_property(js::PropertyKey const& key)
{
    if (key.is_atom()) {
        if (dom::Element* const target = element())
            remove_attribute(internals(), *target, dataset_attribute(key.as_atom()->to_utf8()));
        return true;
    }
    return Object::delete_property(key);
}

std::vector<js::PropertyKey> DatasetObject::own_keys() const
{
    std::vector<js::PropertyKey> keys;
    if (!element())
        return keys;
    for (dom::Attr const& attribute : element()->attributes()) {
        if (attribute.local_name.starts_with("data-") && attribute.prefix.empty())
            keys.push_back(heap()->key(dataset_name(attribute.local_name)));
    }
    return keys;
}

// --- Factories -------------------------------------------------------------------------------

// Each is made over the element's wrapper, rooted while the object is made.
js::Value make_token_list(Realm::Internals& in, dom::Element& element, std::string attribute)
{
    js::Interpreter::Roots const roots(in.interpreter);
    NodeWrapper& wrapper = wrapper_for(in, element);
    in.interpreter.root(js::Value::object(&wrapper));
    TokenListObject* list = in.interpreter.heap().allocate<TokenListObject>(in.prototype("DOMTokenList"), wrapper, std::move(attribute));
    return js::Value::object(list);
}

js::Value make_style_declaration(Realm::Internals& in, dom::Element* element, bool computed)
{
    js::Interpreter::Roots const roots(in.interpreter);
    NodeWrapper* const wrapper = element ? &wrapper_for(in, *element) : nullptr;
    if (wrapper)
        in.interpreter.root(js::Value::object(wrapper));
    StyleDeclarationObject* style = in.interpreter.heap().allocate<StyleDeclarationObject>(in.prototype("CSSStyleDeclaration"), *in.realm_record, wrapper, computed);
    return js::Value::object(style);
}

js::Value make_dataset(Realm::Internals& in, dom::Element& element)
{
    js::Interpreter::Roots const roots(in.interpreter);
    NodeWrapper& wrapper = wrapper_for(in, element);
    in.interpreter.root(js::Value::object(&wrapper));
    DatasetObject* dataset = in.interpreter.heap().allocate<DatasetObject>(in.prototype("DOMStringMap"), wrapper);
    return js::Value::object(dataset);
}

// --- install_style -------------------------------------------------------------------------------

void install_style(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());

    // DOMTokenList.
    js::Object* token_list = define_interface(in, "DOMTokenList", nullptr);
    define_getter(in, *token_list, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        return js::Value::number(static_cast<double>(tokens_of(**list).size()));
    });
    define_getter(
        in, *token_list, "value",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
            if (!list)
                return std::nullopt;
            dom::Element const* const element = (*list)->element();
            return internals_of(interp).string(element ? attribute_or_empty(*element, (*list)->attribute) : "");
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
            if (!list)
                return std::nullopt;
            std::optional<std::string> text = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            if (dom::Element* const element = (*list)->element())
                set_attribute((*list)->internals(), *element, (*list)->attribute, std::move(*text));
            return js::Value::undefined();
        });
    define_operation(interpreter, *token_list, "toString", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        dom::Element const* const element = (*list)->element();
        return internals_of(interp).string(element ? attribute_or_empty(*element, (*list)->attribute) : "");
    });
    define_operation(interpreter, *token_list, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        std::vector<std::string> const tokens = tokens_of(**list);
        if (*index < 0 || *index >= static_cast<double>(tokens.size()))
            return js::Value::null();
        return internals_of(interp).string(tokens[static_cast<std::size_t>(*index)]);
    });
    define_operation(interpreter, *token_list, "contains", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        std::optional<std::string> const token = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!token)
            return std::nullopt;
        std::vector<std::string> const tokens = tokens_of(**list);
        return js::Value::boolean(std::find(tokens.begin(), tokens.end(), *token) != tokens.end());
    });
    define_operation(interpreter, *token_list, "add", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        std::vector<std::string> tokens = tokens_of(**list);
        for (js::Value const& argument : args) {
            std::optional<std::string> token = token_argument(internals_of(interp), argument);
            if (!token)
                return std::nullopt;
            if (std::find(tokens.begin(), tokens.end(), *token) == tokens.end())
                tokens.push_back(std::move(*token));
        }
        write_tokens(**list, tokens);
        return js::Value::undefined();
    });
    define_operation(interpreter, *token_list, "remove", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        std::vector<std::string> tokens = tokens_of(**list);
        for (js::Value const& argument : args) {
            std::optional<std::string> const token = token_argument(internals_of(interp), argument);
            if (!token)
                return std::nullopt;
            tokens.erase(std::remove(tokens.begin(), tokens.end(), *token), tokens.end());
        }
        write_tokens(**list, tokens);
        return js::Value::undefined();
    });
    define_operation(interpreter, *token_list, "toggle", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        std::optional<std::string> const token = token_argument(internals_of(interp), js::argument(args, 0));
        if (!token)
            return std::nullopt;
        std::vector<std::string> tokens = tokens_of(**list);
        bool const present = std::find(tokens.begin(), tokens.end(), *token) != tokens.end();
        js::Value const force = js::argument(args, 1);
        bool const want = force.is_undefined() ? !present : js::Interpreter::to_boolean(force);
        if (want && !present)
            tokens.push_back(*token);
        else if (!want && present)
            tokens.erase(std::remove(tokens.begin(), tokens.end(), *token), tokens.end());
        if (want != present)
            write_tokens(**list, tokens);
        return js::Value::boolean(want);
    });
    define_operation(interpreter, *token_list, "replace", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        std::optional<std::string> const token = token_argument(internals_of(interp), js::argument(args, 0));
        std::optional<std::string> const replacement = token_argument(internals_of(interp), js::argument(args, 1));
        if (!token || !replacement)
            return std::nullopt;
        std::vector<std::string> tokens = tokens_of(**list);
        auto const it = std::find(tokens.begin(), tokens.end(), *token);
        if (it == tokens.end())
            return js::Value::boolean(false);
        if (std::find(tokens.begin(), tokens.end(), *replacement) != tokens.end())
            tokens.erase(it);
        else
            *it = *replacement;
        write_tokens(**list, tokens);
        return js::Value::boolean(true);
    });
    define_operation(interpreter, *token_list, "supports", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(true); });
    // A value iterator's entries, keys, values, forEach and @@iterator are
    // the Array prototype's own functions (WebIDL §3.7.10.1): generic over
    // the list's length and indices, and so answering any array-like.
    {
        js::Object* const array_prototype = interpreter.intrinsics().array_prototype;
        for (std::string_view const name : { "entries", "keys", "values", "forEach" }) {
            if (std::optional<js::PropertyDescriptor> const own = array_prototype->get_own_property(interpreter.key(name)); own && own->value)
                token_list->put(interpreter.key(name), *own->value, js::Writable | js::Enumerable | js::Configurable);
        }
        if (std::optional<js::PropertyDescriptor> const values = array_prototype->get_own_property(interpreter.key("values")); values && values->value)
            token_list->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), *values->value, js::Writable | js::Configurable);
    }

    // CSSStyleDeclaration.
    js::Object* style = define_interface_with(in, "CSSStyleDeclaration",
        *interpreter.heap().allocate<StyleDeclarationPrototype>(interpreter.current_realm()->intrinsics.object_prototype, interpreter));
    define_getter(
        in, *style, "cssText",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
            if (!s)
                return std::nullopt;
            if (!(*s)->writable())
                return internals_of(interp).string("");
            return internals_of(interp).string(serialize_declarations(declarations_of(**s)));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
            if (!s)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            if (!(*s)->writable())
                return internals.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore read-only.");
            std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            write_declarations(internals, **s, css::parse_declaration_list(*text));
            return js::Value::undefined();
        });
    define_getter(in, *style, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        if ((*s)->computed && (*s)->element())
            return js::Value::number(static_cast<double>(computed_names(internals_of(interp), **s).size()));
        if (!(*s)->writable())
            return js::Value::number(0);
        return js::Value::number(static_cast<double>(declarations_of(**s).size()));
    });
    define_getter(in, *style, "parentRule", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        return (*s)->owner_rule ? js::Value::object((*s)->owner_rule) : js::Value::null();
    });
    define_operation(interpreter, *style, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        if ((*s)->computed && (*s)->element()) {
            std::vector<std::string> const names = computed_names(internals_of(interp), **s);
            if (*index < 0 || *index >= static_cast<double>(names.size()))
                return internals_of(interp).string("");
            return internals_of(interp).string(names[static_cast<std::size_t>(*index)]);
        }
        if (!(*s)->writable())
            return internals_of(interp).string("");
        std::vector<css::Declaration> const declarations = declarations_of(**s);
        if (*index < 0 || *index >= static_cast<double>(declarations.size()))
            return internals_of(interp).string("");
        return internals_of(interp).string(declarations[static_cast<std::size_t>(*index)].name);
    });
    define_operation(interpreter, *style, "getPropertyValue", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        if (!(*s)->element() && !(*s)->store)
            return internals.string("");
        if ((*s)->computed) {
            css::ComputedStyle const* computed = internals.hooks.computed_style ? internals.hooks.computed_style(*(*s)->element()) : nullptr;
            // A custom property's name is case-sensitive; every other one is not.
            std::string const asked = name->starts_with("--") ? *name : ascii_lower(*name);
            return internals.string(computed ? computed_property(internals, *(*s)->element(), *computed, asked) : "");
        }
        return internals.string(declaration_value(**s, *name));
    });
    define_operation(interpreter, *style, "getPropertyPriority", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        if (!(*s)->writable())
            return internals.string("");
        for (css::Declaration const& declaration : declarations_of(**s)) {
            if (declaration.name == ascii_lower(*name))
                return internals.string(declaration.important ? "important" : "");
        }
        return internals.string("");
    });
    define_operation(interpreter, *style, "setProperty", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if (!(*s)->writable())
            return internals.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore read-only.");
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        js::Value const value_argument = js::argument(args, 1);
        std::optional<std::string> const value = value_argument.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value_argument);
        std::optional<std::string> const priority = js::argument(args, 2).is_undefined() ? std::optional<std::string>("") : internals.to_utf8(js::argument(args, 2));
        if (!name || !value || !priority)
            return std::nullopt;
        set_declaration(internals, **s, *name, *value, ascii_lower(*priority) == "important");
        return js::Value::undefined();
    });
    define_operation(interpreter, *style, "removeProperty", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if (!(*s)->writable())
            return internals.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore read-only.");
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::string const previous = declaration_value(**s, *name);
        set_declaration(internals, **s, *name, "", false);
        return internals.string(previous);
    });

    define_interface(in, "DOMStringMap", nullptr);

    // getComputedStyle on the window.
    define_operation(interpreter, *interpreter.global(), "getComputedStyle", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        dom::Node* node = internals.realm.node_of(js::argument(args, 0));
        if (!node || !node->is_element())
            return interp.throw_type_error("Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
        // A pseudo-element named that is not one gives a style with nothing
        // in it (CSSOM §9.1). One that is gives, for now, the element's own.
        if (args.size() > 1 && !args[1].is_nullish()) {
            std::optional<std::string> const pseudo = internals.to_utf8(args[1]);
            if (!pseudo)
                return std::nullopt;
            if (!computed_style_pseudo_ok(*pseudo))
                return make_style_declaration(internals, nullptr, true);
        }
        return make_style_declaration(internals, static_cast<dom::Element*>(node), true);
    });

    // The CSS namespace: supports() answers from the same <supports-condition>
    // evaluator @supports itself uses (css-conditional-3 §8) — not just
    // whether the text parses as some declaration, but whether the style
    // resolver actually knows the property and accepts the value.
    js::Object* css = interpreter.new_object();
    interpreter.global()->put(interpreter.key("CSS"), js::Value::object(css), js::builtin_attributes);
    define_operation(interpreter, *css, "supports", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const first = internals.to_utf8(js::argument(args, 0));
        if (!first)
            return std::nullopt;
        std::string condition_text = *first;
        if (args.size() > 1) {
            std::optional<std::string> const second = internals.to_utf8(args[1]);
            if (!second)
                return std::nullopt;
            // The two-argument form is the one-argument form given
            // "property: value" — itself supported only via §8's implicit
            // parentheses, since a bare declaration is not a
            // <supports-condition> on its own.
            condition_text = *first + ": " + *second;
        }
        return js::Value::boolean(css::supports_condition_text_matches(condition_text));
    });
    define_operation(interpreter, *css, "escape", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        std::string out;
        for (std::size_t i = 0; i < text->size(); ++i) {
            char const c = (*text)[i];
            bool const digit = c >= '0' && c <= '9';
            bool const letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (c == '\0') {
                out += "\xEF\xBF\xBD";
            } else if (digit && (i == 0 || (i == 1 && (*text)[0] == '-'))) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\%x ", static_cast<unsigned>(c));
                out += buffer;
            } else if (letter || digit || c == '-' || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
                out += c;
            } else {
                out += '\\';
                out += c;
            }
        }
        return internals.string(out);
    });
    for (std::string_view const name : { "px", "em", "rem", "percent", "vw", "vh", "number" }) {
        std::string const unit(name == "percent" ? "percent" : name);
        define_operation(interpreter, *css, name, 1, [unit](js::Interpreter& interp, js::Value const&, Args args) -> Native {
            std::optional<double> const number = interp.to_number(js::argument(args, 0));
            if (!number)
                return std::nullopt;
            js::Heap::NoCollect const no_collect(interp.heap());
            js::Object* value = interp.new_object();
            value->put(interp.key("value"), js::Value::number(*number));
            value->put(interp.key("unit"), internals_of(interp).string(unit));
            return js::Value::object(value);
        });
    }
}

}

namespace sashfold::bindings {

std::string css_declarations_text(std::vector<css::Declaration> const& declarations)
{
    return serialize_declarations(declarations);
}

std::string css_values_text(std::vector<css::ComponentValue> const& values)
{
    std::string out;
    serialize_values(values, out);
    return out;
}

std::string computed_value_text(Realm::Internals& in, dom::Element& element, css::ComputedStyle const& style, std::string const& name)
{
    return computed_property(in, element, style, name, false);
}

void write_inline_declaration(Realm::Internals& in, dom::Element& element, std::string const& name, std::string const& value)
{
    std::vector<css::Declaration> declarations = declarations_of(element);
    std::string const before = serialize_declarations(declarations);
    // A write that changes nothing is not made: no attribute change, no
    // mutation record.
    if (edit_declaration(declarations, name, value, false) && serialize_declarations(declarations) != before)
        write_declarations(in, element, declarations);
}

}

namespace sashfold::bindings {

js::Value make_rule_style_declaration(Realm::Internals& in, std::shared_ptr<DeclarationStore> store, js::Object* owner_rule)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(owner_rule));
    StyleDeclarationObject* style = in.interpreter.heap().allocate<StyleDeclarationObject>(in.prototype("CSSStyleDeclaration"), *in.realm_record, nullptr, false);
    style->store = std::move(store);
    style->owner_rule = owner_rule;
    return js::Value::object(style);
}

}
