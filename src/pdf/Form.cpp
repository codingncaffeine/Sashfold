#include "pdf/Form.h"

#include "core/Unicode.h"
#include "pdf/Encoding.h"
#include "pdf/Font.h"
#include "pdf/Parser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>

namespace sashfold::pdf {

namespace {

constexpr int k_max_parents = 32;
constexpr double k_padding = 2; // inside the border, as Acrobat and pdf.js lay fields out
constexpr double k_line_factor = 1.15; // a line's height in font sizes

std::string number(double v)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.4f", v);
    std::string s = buffer;
    while (s.find('.') != std::string::npos && (s.back() == '0' || s.back() == '.')) {
        bool const dot = s.back() == '.';
        s.pop_back();
        if (dot)
            break;
    }
    if (s == "-0")
        s = "0";
    return s;
}

// A color operator for a /MK color array by its component count; empty
// for none (transparent).
std::string color_operator(Document const& doc, Object const& array_in, bool stroke)
{
    Array const& array = doc.resolve(array_in).as_array();
    std::string out;
    for (Object const& c : array)
        out += number(std::clamp(doc.resolve(c).as_number(0), 0.0, 1.0)) + " ";
    switch (array.size()) {
    case 1: return out + (stroke ? "G" : "g");
    case 3: return out + (stroke ? "RG" : "rg");
    case 4: return out + (stroke ? "K" : "k");
    default: return "";
    }
}

// A fill operator rewritten as a stroke one, for text whose DA gives a fill.
std::string as_stroke(std::string const& fill)
{
    if (fill.empty())
        return fill;
    std::string out = fill;
    if (out.ends_with("rg"))
        out.replace(out.size() - 2, 2, "RG");
    else if (out.back() == 'g')
        out.back() = 'G';
    else if (out.back() == 'k')
        out.back() = 'K';
    return out;
}

// An entry inherited down the field tree (§12.7.4.2 "inheritable").
Object inherited(Document const& doc, Dict const& dict, std::string_view key)
{
    Dict current = dict;
    for (int depth = 0; depth < k_max_parents; ++depth) {
        if (Object const* found = current.find(key))
            return *found;
        Object const parent = doc.resolve(current.get("Parent"));
        if (!parent.is_dict())
            break;
        current = parent.as_dict();
    }
    return {};
}

std::string literal_string(std::string const& bytes)
{
    std::string out = "(";
    for (char const c : bytes) {
        auto const u = static_cast<unsigned char>(c);
        if (c == '(' || c == ')' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20 || u >= 0x7F) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\%03o", u);
            out += buffer;
        } else {
            out.push_back(c);
        }
    }
    return out + ")";
}

// Unicode to a WinAnsi code, built once from the encoding's glyph names.
std::map<char32_t, int> const& win_ansi_codes()
{
    static std::map<char32_t, int> const codes = [] {
        std::map<char32_t, int> out;
        std::array<char const*, 256> const& names = base_encoding(BaseEncoding::WinAnsi);
        for (int code = 255; code >= 32; --code) {
            if (!names[static_cast<std::size_t>(code)])
                continue;
            std::u32string const text = glyph_name_unicode(names[static_cast<std::size_t>(code)]);
            if (text.size() == 1)
                out[text[0]] = code;
        }
        out[U' '] = 32;
        return out;
    }();
    return codes;
}

// A value's text as the bytes a WinAnsi-encoded simple font shows: what
// the encoding lacks becomes a bullet, so the field is seen to hold
// something rather than nothing.
std::string win_ansi_bytes(std::u32string const& text)
{
    std::string out;
    std::map<char32_t, int> const& codes = win_ansi_codes();
    for (char32_t const c : text) {
        auto const it = codes.find(c);
        out.push_back(static_cast<char>(it != codes.end() ? it->second : 0x95));
    }
    return out;
}

Object helvetica_dict()
{
    Dict font;
    font.set("Type", Object::name("Font"));
    font.set("Subtype", Object::name("Type1"));
    font.set("BaseFont", Object::name("Helvetica"));
    font.set("Encoding", Object::name("WinAnsiEncoding"));
    return Object::dict(std::move(font));
}

Object zapf_dict()
{
    Dict font;
    font.set("Type", Object::name("Font"));
    font.set("Subtype", Object::name("Type1"));
    font.set("BaseFont", Object::name("ZapfDingbats"));
    return Object::dict(std::move(font));
}

// The font a default appearance names in the form's resources, or
// Helvetica in its place when there is none or it is one a value's text
// cannot be encoded for (a composite font): the object to put under the
// resource name, and the loaded font for measuring.
struct ChosenFont {
    Object object;
    std::shared_ptr<Font const> font;
    bool substituted = false;
};

ChosenFont choose_font(Document const& doc, std::string const& name)
{
    ChosenFont chosen;
    Dict const acroform = doc.resolve(doc.catalog().get("AcroForm")).as_dict();
    Dict const dr = doc.resolve(acroform.get("DR")).as_dict();
    Dict const fonts = doc.resolve(dr.get("Font")).as_dict();
    if (Object const* entry = fonts.find(name)) {
        std::shared_ptr<Font const> font = doc.font(*entry);
        if (font && font->kind() == Font::Kind::Simple) {
            chosen.object = *entry;
            chosen.font = font;
            return chosen;
        }
    }
    chosen.object = helvetica_dict();
    chosen.font = doc.font(chosen.object);
    chosen.substituted = true;
    return chosen;
}

// The width of encoded bytes in text space per unit of font size.
double measure(Font const& font, std::string const& bytes)
{
    double width = 0;
    std::size_t at = 0;
    ShownGlyph glyph;
    while (font.next(bytes, at, glyph))
        width += glyph.width;
    return width;
}

// Lines of a text as a field shows them: one line, or wrapped at a width
// (in text space at the font size) by words, a word too long for a line
// broken where it must be.
std::vector<std::u32string> wrap_lines(Font const& font, std::u32string const& text, double size, double width, bool multiline)
{
    std::vector<std::u32string> lines;
    std::u32string current;
    auto const width_of = [&](std::u32string const& s) { return measure(font, win_ansi_bytes(s)) * size; };
    std::vector<std::u32string> paragraphs;
    if (!multiline) {
        std::u32string one;
        for (char32_t const c : text)
            one.push_back(c == U'\n' || c == U'\r' ? U' ' : c);
        return { one };
    }
    std::u32string paragraph;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == U'\r' || text[i] == U'\n') {
            if (text[i] == U'\r' && i + 1 < text.size() && text[i + 1] == U'\n')
                ++i;
            paragraphs.push_back(paragraph);
            paragraph.clear();
        } else {
            paragraph.push_back(text[i]);
        }
    }
    paragraphs.push_back(paragraph);
    for (std::u32string const& p : paragraphs) {
        current.clear();
        std::size_t at = 0;
        while (at < p.size()) {
            std::size_t end = p.find(U' ', at);
            if (end == std::u32string::npos)
                end = p.size();
            std::u32string const word = p.substr(at, end - at);
            std::u32string const candidate = current.empty() ? word : current + U" " + word;
            if (width_of(candidate) <= width || (current.empty() && width_of(word) <= width)) {
                current = candidate;
            } else if (current.empty()) {
                // A word wider than the field: by characters.
                for (char32_t const c : word) {
                    if (!current.empty() && width_of(current + c) > width) {
                        lines.push_back(current);
                        current.clear();
                    }
                    current.push_back(c);
                }
            } else {
                lines.push_back(current);
                current = word;
                if (width_of(current) > width) {
                    std::u32string const long_word = current;
                    current.clear();
                    for (char32_t const c : long_word) {
                        if (!current.empty() && width_of(current + c) > width) {
                            lines.push_back(current);
                            current.clear();
                        }
                        current.push_back(c);
                    }
                }
            }
            at = end + 1;
        }
        lines.push_back(current);
    }
    return lines;
}

FormField::Kind kind_of(std::string const& ft, std::uint32_t flags)
{
    if (ft == "Tx")
        return FormField::Kind::Text;
    if (ft == "Btn") {
        if (flags & (1u << 16))
            return FormField::Kind::PushButton;
        if (flags & (1u << 15))
            return FormField::Kind::Radio;
        return FormField::Kind::CheckBox;
    }
    if (ft == "Ch")
        return (flags & (1u << 17)) ? FormField::Kind::ComboBox : FormField::Kind::ListBox;
    if (ft == "Sig")
        return FormField::Kind::Signature;
    return FormField::Kind::Unknown;
}

std::string family_of(Document const& doc, std::string const& name)
{
    Dict const acroform = doc.resolve(doc.catalog().get("AcroForm")).as_dict();
    Dict const dr = doc.resolve(acroform.get("DR")).as_dict();
    Dict const fonts = doc.resolve(dr.get("Font")).as_dict();
    std::string base = name;
    if (Object const* entry = fonts.find(name))
        base = doc.resolve(doc.resolve(*entry).as_dict().get("BaseFont")).as_name();
    std::string lowered;
    for (char const c : base)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lowered.find("cour") != std::string::npos || lowered.find("mono") != std::string::npos)
        return "monospace";
    if (lowered.find("times") != std::string::npos || lowered.find("tiro") != std::string::npos || lowered.find("serif") != std::string::npos
        || lowered.find("georgia") != std::string::npos || lowered.find("garamond") != std::string::npos || lowered.find("book") != std::string::npos)
        return "serif";
    return "sans-serif";
}

// The border, background and the box text goes in, shared by every kind.
struct Frame {
    double width = 0; // the widget's, after /MK /R
    double height = 0;
    double inset = 0; // border width + padding
    std::string content; // background and border operators
};

Frame frame_of(FormField const& field)
{
    Frame frame;
    bool const turned = field.rotation == 90 || field.rotation == 270;
    frame.width = turned ? field.rect.height() : field.rect.width();
    frame.height = turned ? field.rect.width() : field.rect.height();
    double const bw = field.border_width;
    frame.inset = (field.border_color.empty() && field.border_style != "B" && field.border_style != "I" ? 0 : bw) + k_padding;
    std::string& out = frame.content;
    if (!field.background_color.empty())
        out += field.background_color + " 0 0 " + number(frame.width) + " " + number(frame.height) + " re f\n";
    if (bw > 0 && frame.width > 2 * bw && frame.height > 2 * bw) {
        if (!field.border_color.empty()) {
            out += as_stroke(field.border_color) + " " + number(bw) + " w ";
            if (field.border_style == "D") {
                out += "[";
                for (double const d : field.border_dash)
                    out += number(d) + " ";
                out += field.border_dash.empty() ? "3] 0 d " : "] 0 d ";
            }
            if (field.border_style == "U") {
                out += "0 " + number(bw / 2) + " m " + number(frame.width) + " " + number(bw / 2) + " l S\n";
            } else {
                out += number(bw / 2) + " " + number(bw / 2) + " " + number(frame.width - bw) + " " + number(frame.height - bw) + " re S\n";
            }
        }
        if (field.border_style == "B" || field.border_style == "I") {
            // The bevel inside the border: light along the top and left, dark
            // along the bottom and right (§12.5.4), each a border's width.
            std::string const light = field.border_style == "B" ? "1 g" : "0.5 g";
            std::string const dark = field.border_style == "B" ? "0.5 g" : "0.75 g";
            double const w = frame.width, h = frame.height;
            out += light + " " + number(bw) + " " + number(bw) + " m " + number(bw) + " " + number(h - bw) + " l " + number(w - bw) + " "
                + number(h - bw) + " l " + number(w - 2 * bw) + " " + number(h - 2 * bw) + " l " + number(2 * bw) + " " + number(h - 2 * bw)
                + " l " + number(2 * bw) + " " + number(2 * bw) + " l h f\n";
            out += dark + " " + number(w - bw) + " " + number(h - bw) + " m " + number(w - bw) + " " + number(bw) + " l " + number(bw) + " "
                + number(bw) + " l " + number(2 * bw) + " " + number(2 * bw) + " l " + number(w - 2 * bw) + " " + number(2 * bw) + " l "
                + number(w - 2 * bw) + " " + number(h - 2 * bw) + " l h f\n";
        }
    }
    return frame;
}

std::array<double, 6> rotation_matrix(FormField const& field)
{
    double const w = field.rect.width(), h = field.rect.height();
    switch (field.rotation) {
    case 90: return { 0, 1, -1, 0, w, 0 };
    case 180: return { -1, 0, 0, -1, w, h };
    case 270: return { 0, -1, 1, 0, 0, h };
    default: return { 1, 0, 0, 1, 0, 0 };
    }
}

// Text shown in a box: the lines placed by the quadding, from the top
// for several lines, centered on the box's height for one.
std::string show_lines(Font const& font, std::string const& font_name, double size, std::string const& color, std::vector<std::u32string> const& lines,
    double x0, double y_top, double box_width, double box_height, int quadding, bool one_line, bool comb, int comb_cells)
{
    std::string out = "BT /" + font_name + " " + number(size) + " Tf " + color + "\n";
    double const ascent = font.ascent() > 0 ? font.ascent() : 0.8;
    double const descent = font.descent() < 0 ? font.descent() : -0.2;
    double const leading = size * k_line_factor;
    double y = one_line ? y_top - box_height / 2 - (ascent + descent) * size / 2 : y_top - ascent * size;
    for (std::u32string const& line : lines) {
        if (comb && comb_cells > 0) {
            double const cell = box_width / comb_cells;
            std::size_t i = 0;
            for (char32_t const c : line) {
                if (static_cast<int>(i) >= comb_cells)
                    break;
                std::string const bytes = win_ansi_bytes(std::u32string(1, c));
                double const w = measure(font, bytes) * size;
                out += "1 0 0 1 " + number(x0 + cell * static_cast<double>(i) + (cell - w) / 2) + " " + number(y) + " Tm " + literal_string(bytes) + " Tj\n";
                ++i;
            }
        } else {
            std::string const bytes = win_ansi_bytes(line);
            double const w = measure(font, bytes) * size;
            double x = x0;
            if (quadding == 1)
                x = x0 + (box_width - w) / 2;
            else if (quadding == 2)
                x = x0 + box_width - w;
            out += "1 0 0 1 " + number(x) + " " + number(y) + " Tm " + literal_string(bytes) + " Tj\n";
        }
        y -= leading;
        if (y < -leading)
            break;
    }
    return out + "ET\n";
}

std::u32string stars(std::size_t n)
{
    return std::u32string(n, U'*');
}

} // namespace

bool FormField::comb() const
{
    return kind == Kind::Text && (flags & (1u << 24)) && !multiline() && !password() && max_len > 0 && !(flags & (1u << 20));
}

DefaultAppearance parse_default_appearance(std::string const& da)
{
    DefaultAppearance out;
    std::vector<std::uint8_t> const bytes(da.begin(), da.end());
    Lexer lexer(bytes);
    Parser parser(lexer, false);
    std::vector<Object> operands;
    std::string color;
    for (;;) {
        Token const token = lexer.peek();
        if (token.kind == Token::Kind::Eof)
            break;
        std::string keyword;
        Object const object = parser.parse(&keyword);
        if (keyword.empty()) {
            operands.push_back(object);
            continue;
        }
        if (keyword == "Tf" && operands.size() >= 2) {
            if (operands[operands.size() - 2].is_name())
                out.font_name = operands[operands.size() - 2].as_name();
            out.font_size = std::max(0.0, operands.back().as_number(0));
        } else if (keyword == "g" || keyword == "rg" || keyword == "k") {
            color.clear();
            for (Object const& o : operands)
                color += number(std::clamp(o.as_number(0), 0.0, 1.0)) + " ";
            color += keyword;
        }
        operands.clear();
    }
    if (!color.empty())
        out.color_operators = color;
    return out;
}

bool need_appearances(Document const& doc)
{
    Dict const acroform = doc.resolve(doc.catalog().get("AcroForm")).as_dict();
    return doc.resolve(acroform.get("NeedAppearances")).as_bool(false);
}

std::string utf8_to_text_string(std::string const& utf8)
{
    bool ascii = true;
    for (char const c : utf8) {
        if (static_cast<unsigned char>(c) >= 0x80 || (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\r' && c != '\t')) {
            ascii = false;
            break;
        }
    }
    if (ascii)
        return utf8;
    std::string out = "\xFE\xFF";
    for (char32_t const c : decode_utf8(utf8)) {
        auto const put = [&](std::uint32_t unit) {
            out.push_back(static_cast<char>(unit >> 8));
            out.push_back(static_cast<char>(unit & 0xFF));
        };
        if (c >= 0x10000) {
            std::uint32_t const v = c - 0x10000;
            put(0xD800 + (v >> 10));
            put(0xDC00 + (v & 0x3FF));
        } else {
            put(c);
        }
    }
    return out;
}

std::optional<FormField> widget_field(Document const& doc, Dict const& annotation, Ref widget_ref)
{
    if (!doc.resolve(annotation.get("Subtype")).is_name("Widget"))
        return std::nullopt;
    FormField field;
    field.widget = widget_ref;
    // The terminal field: the widget itself when it carries a field's
    // entries, else the parent whose Kids it is among.
    field.field = widget_ref;
    if (!annotation.has("T") && !annotation.has("FT") && annotation.get("Parent").is_ref())
        field.field = annotation.get("Parent").as_ref();
    Dict const terminal = field.field == widget_ref ? annotation : doc.resolve(Object::ref(field.field)).as_dict();
    std::string const ft = doc.resolve(inherited(doc, annotation, "FT")).as_name();
    field.flags = static_cast<std::uint32_t>(doc.resolve(inherited(doc, annotation, "Ff")).as_int(0));
    field.kind = kind_of(ft, field.flags);
    if (field.kind == FormField::Kind::Unknown)
        return std::nullopt;
    // The fully qualified name: the partial names up the tree, joined by periods.
    {
        std::vector<std::string> parts;
        Dict current = annotation;
        for (int depth = 0; depth < k_max_parents; ++depth) {
            Object const t = doc.resolve(current.get("T"));
            if (t.is_string())
                parts.push_back(text_string_to_utf8(t.as_string()));
            Object const parent = doc.resolve(current.get("Parent"));
            if (!parent.is_dict())
                break;
            current = parent.as_dict();
        }
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            field.name += (field.name.empty() ? "" : ".") + *it;
    }
    field.max_len = static_cast<int>(std::max<std::int64_t>(0, doc.resolve(inherited(doc, annotation, "MaxLen")).as_int(0)));
    field.top_index = static_cast<int>(std::max<std::int64_t>(0, doc.resolve(inherited(doc, annotation, "TI")).as_int(0)));
    Dict const acroform = doc.resolve(doc.catalog().get("AcroForm")).as_dict();
    {
        Object q = doc.resolve(inherited(doc, annotation, "Q"));
        if (q.is_null())
            q = doc.resolve(acroform.get("Q"));
        field.quadding = static_cast<int>(std::clamp<std::int64_t>(q.as_int(0), 0, 2));
        Object da = doc.resolve(inherited(doc, annotation, "DA"));
        if (!da.is_string())
            da = doc.resolve(acroform.get("DA"));
        if (da.is_string())
            field.da = parse_default_appearance(da.as_string());
        field.font_family = family_of(doc, field.da.font_name);
    }
    // Options: strings, or [export display] pairs.
    for (Object const& option : doc.resolve(inherited(doc, annotation, "Opt")).as_array()) {
        Object const o = doc.resolve(option);
        if (o.is_string()) {
            std::string const text = text_string_to_utf8(o.as_string());
            field.options.emplace_back(text, text);
        } else if (o.is_array() && o.as_array().size() >= 2) {
            field.options.emplace_back(text_string_to_utf8(doc.resolve(o.as_array()[0]).as_string()),
                text_string_to_utf8(doc.resolve(o.as_array()[1]).as_string()));
        }
    }
    // The value.
    Object const v = doc.resolve(inherited(doc, annotation, "V"));
    switch (field.kind) {
    case FormField::Kind::Text:
    case FormField::Kind::ComboBox:
        if (v.is_string())
            field.value.text = text_string_to_utf8(v.as_string());
        else if (v.is_name())
            field.value.text = v.as_name();
        else if (v.is_array() && !v.as_array().empty())
            field.value.text = text_string_to_utf8(doc.resolve(v.as_array()[0]).as_string());
        else if (Stream const* stream = v.as_stream()) {
            std::vector<std::uint8_t> const bytes = doc.decode(*stream);
            field.value.text = text_string_to_utf8(std::string(bytes.begin(), bytes.end()));
        }
        break;
    case FormField::Kind::ListBox:
        if (v.is_string())
            field.value.selections.push_back(text_string_to_utf8(v.as_string()));
        else if (v.is_array()) {
            for (Object const& item : v.as_array()) {
                Object const s = doc.resolve(item);
                if (s.is_string())
                    field.value.selections.push_back(text_string_to_utf8(s.as_string()));
            }
        }
        break;
    case FormField::Kind::CheckBox:
    case FormField::Kind::Radio: {
        Dict const ap = doc.resolve(annotation.get("AP")).as_dict();
        Dict const normal = doc.resolve(ap.get("N")).as_dict();
        for (auto const& [state, stream] : normal.entries()) {
            if (state != "Off") {
                field.on_state = state;
                break;
            }
        }
        if (v.is_name())
            field.value.state = v.as_name();
        else if (v.is_string())
            field.value.state = v.as_string();
        else {
            // No value: the widget's own state says.
            std::string const as = doc.resolve(annotation.get("AS")).as_name();
            field.value.state = as.empty() ? "Off" : as;
        }
        if (field.value.state.empty())
            field.value.state = "Off";
        break;
    }
    default:
        break;
    }
    // /MK and the border.
    Dict const mk = doc.resolve(annotation.get("MK")).as_dict();
    field.border_color = color_operator(doc, mk.get("BC"), false);
    field.background_color = color_operator(doc, mk.get("BG"), false);
    field.caption = doc.resolve(mk.get("CA")).as_string();
    field.rotation = ((static_cast<int>(doc.resolve(mk.get("R")).as_int(0)) % 360) + 360) % 360 / 90 * 90;
    Dict const bs = doc.resolve(annotation.get("BS")).as_dict();
    field.border_width = bs.has("W") ? std::max(0.0, doc.resolve(bs.get("W")).as_number(1)) : 1;
    std::string const style = doc.resolve(bs.get("S")).as_name();
    if (style == "D" || style == "B" || style == "I" || style == "U")
        field.border_style = style;
    for (Object const& d : doc.resolve(bs.get("D")).as_array())
        field.border_dash.push_back(doc.resolve(d).as_number(3));
    if (!bs.has("W") && annotation.has("Border")) {
        Array const& border = doc.resolve(annotation.get("Border")).as_array();
        if (border.size() >= 3)
            field.border_width = std::max(0.0, doc.resolve(border[2]).as_number(1));
    }
    Array const& rect = doc.resolve(annotation.get("Rect")).as_array();
    if (rect.size() != 4)
        return std::nullopt;
    double const r0 = doc.resolve(rect[0]).as_number(0), r1 = doc.resolve(rect[1]).as_number(0);
    double const r2 = doc.resolve(rect[2]).as_number(0), r3 = doc.resolve(rect[3]).as_number(0);
    field.rect = { std::min(r0, r2), std::min(r1, r3), std::max(r0, r2), std::max(r1, r3) };
    if (!std::isfinite(field.rect.x0 + field.rect.y0 + field.rect.x1 + field.rect.y1))
        return std::nullopt;
    field.annotation_flags = static_cast<std::uint32_t>(doc.resolve(annotation.get("F")).as_int(0));
    {
        Dict const ap = doc.resolve(annotation.get("AP")).as_dict();
        Object const n = doc.resolve(ap.get("N"));
        field.has_appearance = n.as_stream() != nullptr || (n.is_dict() && !n.as_dict().empty());
    }
    (void)terminal;
    return field;
}

std::vector<FormField> page_fields(Document const& doc, std::size_t index)
{
    std::vector<FormField> fields;
    if (index >= doc.page_count())
        return fields;
    Page const& page = doc.page(index);
    for (Object const& annotation_ref : doc.resolve(page.dict->get("Annots")).as_array()) {
        Dict const annotation = doc.resolve(annotation_ref).as_dict();
        if (annotation.empty())
            continue;
        if (std::optional<FormField> field = widget_field(doc, annotation, annotation_ref.is_ref() ? annotation_ref.as_ref() : Ref {}))
            fields.push_back(std::move(*field));
    }
    return fields;
}

std::string widget_state(FormField const& field, FormValue const* value)
{
    std::string const state = value ? value->state : field.value.state;
    if (field.kind == FormField::Kind::CheckBox || field.kind == FormField::Kind::Radio) {
        if (!field.on_state.empty())
            return state == field.on_state ? field.on_state : "Off";
        return state.empty() ? "Off" : state;
    }
    return "";
}

Appearance widget_appearance(Document const& doc, FormField const& field, FormValue const* value_in)
{
    Appearance out;
    FormValue const& value = value_in ? *value_in : field.value;
    Frame const frame = frame_of(field);
    bool const turned = field.rotation == 90 || field.rotation == 270;
    out.bbox = { 0, 0, turned ? field.rect.height() : field.rect.width(), turned ? field.rect.width() : field.rect.height() };
    out.matrix = rotation_matrix(field);
    std::string content = frame.content;
    Dict fonts;
    double const inner_w = std::max(0.0, frame.width - 2 * frame.inset);
    double const inner_h = std::max(0.0, frame.height - 2 * frame.inset);

    auto const text_font = [&](std::string& resource_name) {
        ChosenFont chosen = choose_font(doc, field.da.font_name);
        resource_name = chosen.substituted ? "Helv" : field.da.font_name;
        if (resource_name.empty())
            resource_name = "Helv";
        fonts.set(resource_name, chosen.object);
        return chosen.font;
    };

    switch (field.kind) {
    case FormField::Kind::Text:
    case FormField::Kind::ComboBox: {
        std::string text_utf8 = value.text;
        if (field.kind == FormField::Kind::ComboBox) {
            // The display text of the chosen export value.
            for (auto const& [export_value, display] : field.options) {
                if (export_value == value.text) {
                    text_utf8 = display;
                    break;
                }
            }
        }
        std::u32string text = decode_utf8(text_utf8);
        if (field.max_len > 0 && text.size() > static_cast<std::size_t>(field.max_len))
            text.resize(static_cast<std::size_t>(field.max_len));
        if (field.password())
            text = stars(text.size());
        if (text.empty())
            break;
        std::string font_name;
        std::shared_ptr<Font const> const font = text_font(font_name);
        if (!font)
            break;
        bool const multiline = field.multiline();
        double size = field.da.font_size;
        if (size <= 0) {
            if (multiline) {
                // Twelve points, smaller when the lines do not fit.
                size = 12;
                while (size > 4) {
                    std::vector<std::u32string> const lines = wrap_lines(*font, text, size, inner_w, true);
                    if (static_cast<double>(lines.size()) * size * k_line_factor <= inner_h)
                        break;
                    size -= 1;
                }
            } else {
                size = inner_h / k_line_factor;
                double const w = measure(*font, win_ansi_bytes(text));
                if (w > 0 && !field.comb())
                    size = std::min(size, inner_w / w);
                size = std::max(1.0, std::floor(size * 100) / 100);
            }
        }
        std::vector<std::u32string> const lines = wrap_lines(*font, text, size, inner_w, multiline);
        content += "/Tx BMC q " + number(frame.inset) + " " + number(frame.inset) + " " + number(inner_w) + " " + number(inner_h) + " re W n\n";
        content += show_lines(*font, font_name, size, field.da.color_operators, lines, frame.inset, frame.height - frame.inset, inner_w, inner_h,
            field.quadding, !multiline, field.comb(), field.max_len);
        content += "Q EMC\n";
        break;
    }
    case FormField::Kind::ListBox: {
        std::string font_name;
        std::shared_ptr<Font const> const font = text_font(font_name);
        if (!font || field.options.empty())
            break;
        double const size = field.da.font_size > 0 ? field.da.font_size : 12;
        double const leading = size * k_line_factor;
        double const ascent = font->ascent() > 0 ? font->ascent() : 0.8;
        content += "/Tx BMC q " + number(frame.inset) + " " + number(frame.inset) + " " + number(inner_w) + " " + number(inner_h) + " re W n\n";
        double y_top = frame.height - frame.inset;
        std::string text_ops;
        for (std::size_t i = static_cast<std::size_t>(std::min<int>(field.top_index, static_cast<int>(field.options.size()))); i < field.options.size(); ++i) {
            if (y_top - leading < frame.inset - leading)
                break;
            auto const& [export_value, display] = field.options[i];
            bool const selected = std::find(value.selections.begin(), value.selections.end(), export_value) != value.selections.end()
                || (value.selections.empty() && !value.text.empty() && value.text == export_value);
            if (selected) {
                // Acrobat's and pdf.js's selection blue behind the row.
                content += "0.6 0.7569 0.8549 rg " + number(frame.inset) + " " + number(y_top - leading) + " " + number(inner_w) + " " + number(leading) + " re f\n";
            }
            std::string const bytes = win_ansi_bytes(decode_utf8(display));
            text_ops += "1 0 0 1 " + number(frame.inset + 1) + " " + number(y_top - ascent * size) + " Tm " + literal_string(bytes) + " Tj\n";
            y_top -= leading;
        }
        content += "BT /" + font_name + " " + number(size) + " Tf " + field.da.color_operators + "\n" + text_ops + "ET\nQ EMC\n";
        break;
    }
    case FormField::Kind::CheckBox:
    case FormField::Kind::Radio: {
        std::string const state = widget_state(field, &value);
        if (state == "Off")
            break;
        // The mark: ZapfDingbats' check (4) for a box, its circle (l) for a
        // button, or the caption's own character.
        std::string mark = field.caption.empty() ? (field.kind == FormField::Kind::Radio ? "l" : "4") : field.caption.substr(0, 1);
        if (field.kind == FormField::Kind::Radio && field.border_style != "D" && field.caption.empty() && inner_w > 0) {
            // A round button: a dot the field's color, as Acrobat draws one.
            double const cx = frame.width / 2, cy = frame.height / 2;
            double const r = std::min(inner_w, inner_h) / 4;
            double const k = 0.5523 * r;
            content += field.da.color_operators + " " + number(cx + r) + " " + number(cy) + " m " + number(cx + r) + " " + number(cy + k) + " " + number(cx + k) + " "
                + number(cy + r) + " " + number(cx) + " " + number(cy + r) + " c " + number(cx - k) + " " + number(cy + r) + " " + number(cx - r) + " "
                + number(cy + k) + " " + number(cx - r) + " " + number(cy) + " c " + number(cx - r) + " " + number(cy - k) + " " + number(cx - k) + " "
                + number(cy - r) + " " + number(cx) + " " + number(cy - r) + " c " + number(cx + k) + " " + number(cy - r) + " " + number(cx + r) + " "
                + number(cy - k) + " " + number(cx + r) + " " + number(cy) + " c f\n";
            break;
        }
        fonts.set("ZaDb", zapf_dict());
        std::shared_ptr<Font const> const font = doc.font(fonts.get("ZaDb"));
        double size = field.da.font_size;
        double const glyph_w = font ? measure(*font, mark) : 0.75;
        if (size <= 0) {
            size = std::min(inner_h / 1.0, glyph_w > 0 ? inner_w / glyph_w : inner_h) * 0.8;
            size = std::max(1.0, size);
        }
        double const w = glyph_w * size;
        // ZapfDingbats' marks sit on the baseline and reach about 0.7 up.
        double const x = (frame.width - w) / 2;
        double const y = (frame.height - 0.7 * size) / 2;
        content += "q BT /ZaDb " + number(size) + " Tf " + field.da.color_operators + " 1 0 0 1 " + number(x) + " " + number(y) + " Tm " + literal_string(mark)
            + " Tj ET Q\n";
        break;
    }
    case FormField::Kind::PushButton: {
        std::string const caption = field.caption;
        if (caption.empty())
            break;
        std::string font_name;
        std::shared_ptr<Font const> const font = text_font(font_name);
        if (!font)
            break;
        double size = field.da.font_size > 0 ? field.da.font_size : std::max(1.0, std::min(inner_h / k_line_factor, 12.0));
        std::u32string const text = decode_utf8(caption);
        double const w = measure(*font, win_ansi_bytes(text));
        if (w > 0 && w * size > inner_w)
            size = std::max(1.0, inner_w / w);
        content += "q " + number(frame.inset) + " " + number(frame.inset) + " " + number(inner_w) + " " + number(inner_h) + " re W n\n";
        content += show_lines(*font, font_name, size, field.da.color_operators, { text }, frame.inset, frame.height - frame.inset, inner_w, inner_h, 1, true, false, 0);
        content += "Q\n";
        break;
    }
    default:
        break;
    }
    out.content = std::move(content);
    if (!fonts.empty())
        out.resources.set("Font", Object::dict(std::move(fonts)));
    return out;
}

std::string free_text_appearance(Document const& doc, Dict const& annotation, Box const& rect, Dict& resources)
{
    std::string const contents = text_string_to_utf8(doc.resolve(annotation.get("Contents")).as_string());
    Object const da_object = doc.resolve(annotation.get("DA"));
    DefaultAppearance const da = parse_default_appearance(da_object.is_string() ? da_object.as_string() : std::string("/Helv 12 Tf 0 g"));
    ChosenFont chosen = choose_font(doc, da.font_name);
    if (!chosen.font)
        return "";
    std::string const font_name = chosen.substituted || da.font_name.empty() ? "Helv" : da.font_name;
    Dict fonts;
    fonts.set(font_name, chosen.object);
    resources.set("Font", Object::dict(std::move(fonts)));
    // The border: /BS width (one by default) in the text's color, and a
    // background where /C gives one.
    Dict const bs = doc.resolve(annotation.get("BS")).as_dict();
    double const bw = bs.has("W") ? std::max(0.0, doc.resolve(bs.get("W")).as_number(1)) : 1;
    std::string out;
    std::string const fill = color_operator(doc, annotation.get("C"), false);
    if (!fill.empty())
        out += fill + " " + number(rect.x0) + " " + number(rect.y0) + " " + number(rect.width()) + " " + number(rect.height()) + " re f\n";
    if (bw > 0 && rect.width() > 2 * bw && rect.height() > 2 * bw) {
        out += as_stroke(da.color_operators) + " " + number(bw) + " w " + number(rect.x0 + bw / 2) + " " + number(rect.y0 + bw / 2) + " "
            + number(rect.width() - bw) + " " + number(rect.height() - bw) + " re S\n";
    }
    if (contents.empty())
        return out;
    double const inset = bw + k_padding;
    double const inner_w = std::max(0.0, rect.width() - 2 * inset);
    double const inner_h = std::max(0.0, rect.height() - 2 * inset);
    double size = da.font_size > 0 ? da.font_size : 12;
    std::u32string const text = decode_utf8(contents);
    std::vector<std::u32string> const lines = wrap_lines(*chosen.font, text, size, inner_w, true);
    int const quadding = static_cast<int>(std::clamp<std::int64_t>(doc.resolve(annotation.get("Q")).as_int(0), 0, 2));
    out += "q " + number(rect.x0 + inset) + " " + number(rect.y0 + inset) + " " + number(inner_w) + " " + number(inner_h) + " re W n\n";
    out += show_lines(*chosen.font, font_name, size, da.color_operators, lines, rect.x0 + inset, rect.y1 - inset, inner_w, inner_h, quadding, false, false, 0);
    return out + "Q\n";
}

std::string note_icon_appearance(Document const& doc, Dict const& annotation, Box const& rect, Dict& resources)
{
    // The icon at the rectangle's top left, its size the rectangle's up to
    // the twenty points Acrobat draws (NoZoom icons keep that size).
    std::string name = doc.resolve(annotation.get("Name")).as_name();
    if (name.empty())
        name = "Note";
    double const s = std::min({ rect.width(), rect.height(), 20.0 });
    if (s <= 1)
        return "";
    double const x = rect.x0, y = rect.y1 - s;
    std::string fill = color_operator(doc, annotation.get("C"), false);
    if (fill.empty())
        fill = "1 1 0 rg";
    // In a unit square from (x, y), scaled by s.
    std::string out = "q 1 0 0 1 " + number(x) + " " + number(y) + " cm " + number(s) + " 0 0 " + number(s) + " 0 0 cm 0.05 w 0.2 G " + fill + " 1 j 1 J\n";
    auto const rounded = [&](double x0, double y0, double w, double h, double r) {
        std::string p;
        double const k = 0.5523 * r;
        p += number(x0 + r) + " " + number(y0) + " m " + number(x0 + w - r) + " " + number(y0) + " l ";
        p += number(x0 + w - r + k) + " " + number(y0) + " " + number(x0 + w) + " " + number(y0 + r - k) + " " + number(x0 + w) + " " + number(y0 + r) + " c ";
        p += number(x0 + w) + " " + number(y0 + h - r) + " l ";
        p += number(x0 + w) + " " + number(y0 + h - r + k) + " " + number(x0 + w - r + k) + " " + number(y0 + h) + " " + number(x0 + w - r) + " " + number(y0 + h) + " c ";
        p += number(x0 + r) + " " + number(y0 + h) + " l ";
        p += number(x0 + r - k) + " " + number(y0 + h) + " " + number(x0) + " " + number(y0 + h - r + k) + " " + number(x0) + " " + number(y0 + h - r) + " c ";
        p += number(x0) + " " + number(y0 + r) + " l ";
        p += number(x0) + " " + number(y0 + r - k) + " " + number(x0 + r - k) + " " + number(y0) + " " + number(x0 + r) + " " + number(y0) + " c h ";
        return p;
    };
    if (name == "Comment") {
        // A speech bubble with its tail at the lower left.
        out += rounded(0.05, 0.25, 0.9, 0.7, 0.15) + "B\n";
        out += fill + " 0.2 0.25 m 0.15 0.05 l 0.4 0.25 l h B\n";
        out += "0.2 G 0.25 0.7 m 0.75 0.7 l S 0.25 0.55 m 0.75 0.55 l S 0.25 0.4 m 0.6 0.4 l S\n";
    } else if (name == "Key") {
        // A key: a ring and a shaft with two teeth.
        double const k = 0.5523 * 0.2;
        out += "0.5 0.75 m 0.5 " + number(0.75 + k) + " " + number(0.3 + k) + " 0.95 0.3 0.95 c " + number(0.3 - k) + " 0.95 0.1 " + number(0.75 + k)
            + " 0.1 0.75 c 0.1 " + number(0.75 - k) + " " + number(0.3 - k) + " 0.55 0.3 0.55 c " + number(0.3 + k) + " 0.55 0.5 " + number(0.75 - k)
            + " 0.5 0.75 c h B\n";
        out += "0.12 w 0.2 G 0.42 0.63 m 0.9 0.15 l S 0.75 0.3 m 0.85 0.4 l S 0.62 0.17 m 0.72 0.27 l S\n";
    } else if (name == "Help") {
        out += rounded(0.05, 0.05, 0.9, 0.9, 0.45) + "B\n";
        Dict fonts;
        fonts.set("Helv", helvetica_dict());
        resources.set("Font", Object::dict(std::move(fonts)));
        out += "0.2 g BT /Helv 0.6 Tf 0.33 0.22 Td (?) Tj ET\n";
    } else if (name == "NewParagraph") {
        out += "0.5 0.95 m 0.95 0.35 l 0.05 0.35 l h B\n";
        out += "0.2 g 0.3 0.05 0.12 0.25 re f 0.58 0.05 0.12 0.25 re f 0.3 0.3 m 0.7 0.05 l 0.7 0.17 l 0.3 0.42 l h f\n";
    } else if (name == "Paragraph") {
        out += rounded(0.05, 0.05, 0.9, 0.9, 0.1) + "B\n";
        out += "0.2 g 0.45 0.2 0.1 0.6 re f 0.62 0.2 0.1 0.6 re f 0.45 0.5 m 0.45 0.8 l 0.72 0.8 l 0.72 0.5 l h f\n";
        out += fill + " 0.3 0.55 m 0.3 " + number(0.55 + 0.5523 * 0.15) + " " + number(0.45 - 0.5523 * 0.15) + " 0.8 0.45 0.8 c 0.45 0.5 l "
            + number(0.45 - 0.5523 * 0.15) + " 0.5 0.3 " + number(0.55 - 0.5523 * 0.05) + " 0.3 0.55 c h f\n";
    } else if (name == "Insert") {
        out += "0.5 0.95 m 0.95 0.1 l 0.05 0.1 l h B\n";
    } else {
        // Note: a page with a folded corner and lines of text.
        out += "0.1 0.05 m 0.1 0.95 l 0.65 0.95 l 0.9 0.7 l 0.9 0.05 l h B\n";
        out += "0.65 0.95 m 0.65 0.7 l 0.9 0.7 l S\n";
        out += "0.2 G 0.25 0.55 m 0.75 0.55 l S 0.25 0.4 m 0.75 0.4 l S 0.25 0.25 m 0.6 0.25 l S\n";
    }
    return out + "Q\n";
}

}
