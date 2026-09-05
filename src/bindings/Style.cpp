#include "bindings/NodeSupport.h"

// CSSOM for the page: element.style over the style attribute, the
// computed style getComputedStyle answers, classList and the other token
// lists, dataset, and the CSS namespace object. The style attribute is
// re-parsed on every access and written back declaration by declaration,
// so what a script sets is what the cascade reads.

#include "core/Unicode.h"
#include "css/Parser.h"
#include "css/Stylesheets.h"
#include "css/Token.h"

#include <algorithm>
#include <cmath>
#include <string>
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
        return;
    }
    set_attribute(in, element, "style", serialize_declarations(declarations));
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

// Sets or removes one declaration of an element's style attribute.
void set_declaration(Realm::Internals& in, dom::Element& element, std::string const& name, std::string const& value, bool important)
{
    std::vector<css::Declaration> declarations = declarations_of(element);
    std::string const lower = ascii_lower(name);
    auto const existing = std::find_if(declarations.begin(), declarations.end(),
        [&lower](css::Declaration const& d) { return d.name == lower; });
    if (value.empty()) {
        if (existing != declarations.end()) {
            declarations.erase(existing);
            write_declarations(in, element, declarations);
        }
        return;
    }
    std::vector<css::Declaration> parsed = css::parse_declaration_list(lower + ": " + value);
    if (parsed.empty())
        return; // not a declaration: ignored, as the CSSOM says
    parsed.front().important = important;
    if (existing != declarations.end())
        *existing = parsed.front();
    else
        declarations.push_back(parsed.front());
    write_declarations(in, element, declarations);
}

std::string declaration_value(dom::Element const& element, std::string const& name)
{
    std::string const lower = ascii_lower(name);
    for (css::Declaration const& declaration : declarations_of(element)) {
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
    default: break;
    }
    // The remaining table-internal kinds and none: read the enumerator
    // order past TableRow.
    switch (static_cast<int>(display) - static_cast<int>(D::TableRow)) {
    case 1: return "table-column-group";
    case 2: return "table-column";
    case 3: return "table-cell";
    case 4: return "table-caption";
    default: return "none";
    }
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

// The computed value of one property as getComputedStyle spells it; empty
// for a property the engine does not compute.
std::string computed_property(Realm::Internals& in, dom::Element& element, css::ComputedStyle const& style, std::string const& name)
{
    using namespace css;
    std::optional<LayoutBox> box;
    auto const box_of = [&]() -> std::optional<LayoutBox> {
        if (!box && in.hooks.layout_box)
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
        if (std::optional<LayoutBox> const b = box_of())
            return px(name == "width" ? b->width : b->height);
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
    if (name == "color")
        return color_text(style.color);
    if (name == "background-color")
        return color_text(style.background_color);
    if (name == "font-size")
        return px(style.font_size);
    if (name == "font-weight")
        return std::to_string(style.font_weight);
    if (name == "font-style")
        return style.font_style == FontStyle::Italic ? "italic" : "normal";
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
        }
    }
    if (name == "white-space") {
        switch (style.white_space) {
        case WhiteSpace::Normal: return "normal";
        case WhiteSpace::Pre: return "pre";
        case WhiteSpace::NoWrap: return "nowrap";
        case WhiteSpace::PreWrap: return "pre-wrap";
        case WhiteSpace::PreLine: return "pre-line";
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
        if (name.starts_with(prefix))
            return border(*side, name.substr(std::string_view(prefix).size()));
    }
    if (name == "transform")
        return style.transformed ? "matrix(1, 0, 0, 1, " + js::number_to_utf8(static_cast<double>(style.translate_x.value)) + ", " + js::number_to_utf8(static_cast<double>(style.translate_y.value)) + ")" : "none";
    if (name == "pointer-events")
        return "auto";
    if (name == "cursor")
        return "auto";
    if (name == "content")
        return "normal";
    if (name == "transition" || name == "animation")
        return "none";
    if (name == "background-image")
        return style.background_images && !style.background_images->empty() ? "url()" : "none";
    return "";
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
    std::vector<std::string> tokens = split_tokens(attribute_or_empty(*list.element, list.attribute));
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
    set_attribute(*&list.realm->internals(), *list.element, list.attribute, join_tokens(tokens));
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

std::optional<js::Value> StyleDeclarationObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (!key.is_atom() || has_property(key))
        return Object::get(interpreter, key, receiver);
    std::string const name = css_property_name(key.as_atom()->to_utf8());
    if (name.empty() || name.starts_with("_") || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos)
        return js::Value::undefined();
    Realm::Internals& in = realm->internals();
    if (!element)
        return in.string("");
    if (computed) {
        css::ComputedStyle const* style = in.hooks.computed_style ? in.hooks.computed_style(*element) : nullptr;
        return in.string(style ? computed_property(in, *element, *style, name) : "");
    }
    return in.string(declaration_value(*element, name));
}

std::optional<bool> StyleDeclarationObject::set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver)
{
    if (!key.is_atom() || has_property(key))
        return Object::set(interpreter, key, value, receiver);
    std::string const name = css_property_name(key.as_atom()->to_utf8());
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos)
        return Object::set(interpreter, key, value, receiver);
    Realm::Internals& in = realm->internals();
    if (computed || !element) {
        in.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore the '" + name + "' property is read-only.");
        return std::nullopt;
    }
    std::optional<std::string> const text = value.is_nullish() ? std::optional<std::string>("") : in.to_utf8(value);
    if (!text)
        return std::nullopt;
    set_declaration(in, *element, name, *text, false);
    return true;
}

// --- DatasetObject -------------------------------------------------------------------------

std::optional<js::PropertyDescriptor> DatasetObject::get_own_property(js::PropertyKey const& key) const
{
    if (key.is_atom()) {
        std::string const attribute = dataset_attribute(key.as_atom()->to_utf8());
        if (dom::Attr const* found = element->find_attribute(attribute))
            return js::PropertyDescriptor::data(js::Value::string(heap()->string(found->value)), js::default_attributes);
    }
    return Object::get_own_property(key);
}

std::optional<js::Value> DatasetObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (key.is_atom()) {
        std::string const attribute = dataset_attribute(key.as_atom()->to_utf8());
        if (dom::Attr const* found = element->find_attribute(attribute))
            return js::Value::string(interpreter.string(found->value));
    }
    return Object::get(interpreter, key, receiver);
}

std::optional<bool> DatasetObject::set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver)
{
    if (!key.is_atom())
        return Object::set(interpreter, key, value, receiver);
    Realm::Internals& in = realm->internals();
    std::optional<std::string> text = in.to_utf8(value);
    if (!text)
        return std::nullopt;
    set_attribute(in, *element, dataset_attribute(key.as_atom()->to_utf8()), std::move(*text));
    return true;
}

bool DatasetObject::delete_property(js::PropertyKey const& key)
{
    if (key.is_atom()) {
        remove_attribute(realm->internals(), *element, dataset_attribute(key.as_atom()->to_utf8()));
        return true;
    }
    return Object::delete_property(key);
}

std::vector<js::PropertyKey> DatasetObject::own_keys() const
{
    std::vector<js::PropertyKey> keys;
    for (dom::Attr const& attribute : element->attributes()) {
        if (attribute.local_name.starts_with("data-") && attribute.prefix.empty())
            keys.push_back(heap()->key(dataset_name(attribute.local_name)));
    }
    return keys;
}

// --- Factories -------------------------------------------------------------------------------

js::Value make_token_list(Realm::Internals& in, dom::Element& element, std::string attribute)
{
    TokenListObject* list = in.interpreter.heap().allocate<TokenListObject>(in.prototype("DOMTokenList"), in.realm, element, std::move(attribute));
    return js::Value::object(list);
}

js::Value make_style_declaration(Realm::Internals& in, dom::Element* element, bool computed)
{
    StyleDeclarationObject* style = in.interpreter.heap().allocate<StyleDeclarationObject>(in.prototype("CSSStyleDeclaration"), in.realm, element, computed);
    return js::Value::object(style);
}

js::Value make_dataset(Realm::Internals& in, dom::Element& element)
{
    DatasetObject* dataset = in.interpreter.heap().allocate<DatasetObject>(in.prototype("DOMStringMap"), in.realm, element);
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
            return internals_of(interp).string(attribute_or_empty(*(*list)->element, (*list)->attribute));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
            if (!list)
                return std::nullopt;
            std::optional<std::string> text = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            set_attribute(internals_of(interp), *(*list)->element, (*list)->attribute, std::move(*text));
            return js::Value::undefined();
        });
    js::define_method(interpreter, *token_list, "toString", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        return internals_of(interp).string(attribute_or_empty(*(*list)->element, (*list)->attribute));
    });
    js::define_method(interpreter, *token_list, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    js::define_method(interpreter, *token_list, "contains", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        std::optional<std::string> const token = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!token)
            return std::nullopt;
        std::vector<std::string> const tokens = tokens_of(**list);
        return js::Value::boolean(std::find(tokens.begin(), tokens.end(), *token) != tokens.end());
    });
    js::define_method(interpreter, *token_list, "add", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    js::define_method(interpreter, *token_list, "remove", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    js::define_method(interpreter, *token_list, "toggle", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    js::define_method(interpreter, *token_list, "replace", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    js::define_method(interpreter, *token_list, "supports", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(true); });
    js::define_method(interpreter, *token_list, "forEach", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
        if (!list)
            return std::nullopt;
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("parameter 1 is not of type 'Function'");
        std::vector<std::string> const tokens = tokens_of(**list);
        js::Interpreter::Roots const roots(interp);
        interp.root(callback);
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            js::Value const arguments[3] = { internals_of(interp).string(tokens[i]), js::Value::number(static_cast<double>(i)), this_value };
            if (!interp.call(callback, js::argument(args, 1), arguments))
                return std::nullopt;
        }
        return js::Value::undefined();
    });
    for (std::string_view const name : { "keys", "values", "entries" }) {
        bool const pairs = name == "entries";
        bool const keys = name == "keys";
        js::define_method(interpreter, *token_list, name, 0, [pairs, keys](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<TokenListObject*> const list = this_token_list(interp, this_value);
            if (!list)
                return std::nullopt;
            std::vector<std::string> const tokens = tokens_of(**list);
            js::Interpreter::Roots const roots(interp);
            js::ArrayObject* out = interp.new_array();
            interp.root(js::Value::object(out));
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                if (keys) {
                    out->push(js::Value::number(static_cast<double>(i)));
                } else if (pairs) {
                    js::Value const pair[2] = { js::Value::number(static_cast<double>(i)), internals_of(interp).string(tokens[i]) };
                    out->push(js::Value::object(interp.new_array(pair)));
                } else {
                    out->push(internals_of(interp).string(tokens[i]));
                }
            }
            return js::Value::object(out);
        });
    }

    // CSSStyleDeclaration.
    js::Object* style = define_interface(in, "CSSStyleDeclaration", nullptr);
    define_getter(
        in, *style, "cssText",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
            if (!s)
                return std::nullopt;
            if ((*s)->computed || !(*s)->element)
                return internals_of(interp).string("");
            return internals_of(interp).string(serialize_declarations(declarations_of(*(*s)->element)));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
            if (!s)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            if ((*s)->computed || !(*s)->element)
                return internals.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore read-only.");
            std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            write_declarations(internals, *(*s)->element, css::parse_declaration_list(*text));
            return js::Value::undefined();
        });
    define_getter(in, *style, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        if ((*s)->computed || !(*s)->element)
            return js::Value::number(0);
        return js::Value::number(static_cast<double>(declarations_of(*(*s)->element).size()));
    });
    define_getter(in, *style, "parentRule", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::null(); });
    js::define_method(interpreter, *style, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        if ((*s)->computed || !(*s)->element)
            return internals_of(interp).string("");
        std::vector<css::Declaration> const declarations = declarations_of(*(*s)->element);
        if (*index < 0 || *index >= static_cast<double>(declarations.size()))
            return internals_of(interp).string("");
        return internals_of(interp).string(declarations[static_cast<std::size_t>(*index)].name);
    });
    js::define_method(interpreter, *style, "getPropertyValue", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        if (!(*s)->element)
            return internals.string("");
        if ((*s)->computed) {
            css::ComputedStyle const* computed = internals.hooks.computed_style ? internals.hooks.computed_style(*(*s)->element) : nullptr;
            return internals.string(computed ? computed_property(internals, *(*s)->element, *computed, ascii_lower(*name)) : "");
        }
        return internals.string(declaration_value(*(*s)->element, *name));
    });
    js::define_method(interpreter, *style, "getPropertyPriority", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        if ((*s)->computed || !(*s)->element)
            return internals.string("");
        for (css::Declaration const& declaration : declarations_of(*(*s)->element)) {
            if (declaration.name == ascii_lower(*name))
                return internals.string(declaration.important ? "important" : "");
        }
        return internals.string("");
    });
    js::define_method(interpreter, *style, "setProperty", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if ((*s)->computed || !(*s)->element)
            return internals.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore read-only.");
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        js::Value const value_argument = js::argument(args, 1);
        std::optional<std::string> const value = value_argument.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value_argument);
        std::optional<std::string> const priority = js::argument(args, 2).is_undefined() ? std::optional<std::string>("") : internals.to_utf8(js::argument(args, 2));
        if (!name || !value || !priority)
            return std::nullopt;
        set_declaration(internals, *(*s)->element, *name, *value, ascii_lower(*priority) == "important");
        return js::Value::undefined();
    });
    js::define_method(interpreter, *style, "removeProperty", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StyleDeclarationObject*> const s = this_style(interp, this_value);
        if (!s)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if ((*s)->computed || !(*s)->element)
            return internals.throw_dom_exception("NoModificationAllowedError", "These styles are computed, and therefore read-only.");
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::string const previous = declaration_value(*(*s)->element, *name);
        set_declaration(internals, *(*s)->element, *name, "", false);
        return internals.string(previous);
    });

    define_interface(in, "DOMStringMap", nullptr);

    // getComputedStyle on the window.
    js::define_method(interpreter, *interpreter.global(), "getComputedStyle", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        dom::Node* node = internals.realm.node_of(js::argument(args, 0));
        if (!node || !node->is_element())
            return interp.throw_type_error("Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
        return make_style_declaration(internals, static_cast<dom::Element*>(node), true);
    });

    // The CSS namespace: supports() answers from what the engine parses.
    js::Object* css = interpreter.new_object();
    interpreter.global()->put(interpreter.key("CSS"), js::Value::object(css), js::builtin_attributes);
    js::define_method(interpreter, *css, "supports", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const first = internals.to_utf8(js::argument(args, 0));
        if (!first)
            return std::nullopt;
        std::string declaration = *first;
        if (args.size() > 1) {
            std::optional<std::string> const second = internals.to_utf8(args[1]);
            if (!second)
                return std::nullopt;
            declaration = *first + ": " + *second;
        }
        // A condition in parentheses is stripped to its declaration.
        while (!declaration.empty() && declaration.front() == '(' && declaration.back() == ')')
            declaration = declaration.substr(1, declaration.size() - 2);
        return js::Value::boolean(!css::parse_declaration_list(declaration).empty());
    });
    js::define_method(interpreter, *css, "escape", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
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
        js::define_method(interpreter, *css, name, 1, [unit](js::Interpreter& interp, js::Value const&, Args args) -> Native {
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
