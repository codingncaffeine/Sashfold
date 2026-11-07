#pragma once

// A document's interactive form (ISO 32000-2 §12.7): its fields as the
// widgets that show them on a page, each with what it inherits down the
// field tree (its kind, flags, value, options, default appearance), the
// values a viewer has typed over the file's own, and the appearance a
// viewer draws for a field from those — as Acrobat, Chrome and Firefox
// draw one when the file carries none, asks for them to be regenerated
// (NeedAppearances), or the value was changed. The same appearance is what
// a saved copy carries (Writer.h), so every other reader shows the value.

#include "pdf/Document.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::pdf {

// A value a viewer has given a field, over the file's: text for a text
// field or a combo box, the chosen export values for a list box, the
// appearance state (an on-state's name, or Off) for a check box or a radio
// group. Keyed by the terminal field's reference.
struct FormValue {
    std::string text; // UTF-8
    std::vector<std::string> selections; // list boxes: export values
    std::string state; // check boxes and radio groups
};
using FormValues = std::map<Ref, FormValue>;

// The default appearance string (/DA): the font resource it names, its size
// (0: fit the field), and the color operators as written.
struct DefaultAppearance {
    std::string font_name = "Helv";
    double font_size = 0;
    std::string color_operators = "0 g";
};

struct FormField {
    enum class Kind { Text, CheckBox, Radio, PushButton, ComboBox, ListBox, Signature, Unknown };
    Kind kind = Kind::Unknown;
    Ref widget; // the widget annotation
    Ref field; // the terminal field (the widget itself when they are one)
    std::string name; // fully qualified, UTF-8
    std::uint32_t flags = 0; // /Ff
    bool read_only() const { return flags & 1; }
    bool required() const { return flags & 2; }
    bool multiline() const { return kind == Kind::Text && (flags & (1u << 12)); }
    bool password() const { return kind == Kind::Text && (flags & (1u << 13)); }
    bool comb() const;
    bool editable_combo() const { return kind == Kind::ComboBox && (flags & (1u << 18)); }
    bool multi_select() const { return kind == Kind::ListBox && (flags & (1u << 21)); }
    bool do_not_spell_check() const { return flags & (1u << 22); }
    bool do_not_scroll() const { return kind == Kind::Text && (flags & (1u << 23)); }
    int max_len = 0; // 0: none
    int quadding = 0; // 0 left, 1 centered, 2 right
    int top_index = 0; // list boxes: the first option shown
    // The value as the file has it, in the form the viewer gives it back.
    FormValue value;
    // The widget's own on-state (check boxes, radio buttons): the name in
    // /AP /N that is not Off; empty when it has no appearance states.
    std::string on_state;
    // Options as (export value, display text), both UTF-8.
    std::vector<std::pair<std::string, std::string>> options;
    DefaultAppearance da;
    // /MK: the border and background colors as operators ("1 0 0 rg"),
    // empty for none; the caption (check mark character for buttons);
    // the rotation of the content within the widget.
    std::string border_color;
    std::string background_color;
    std::string caption;
    int rotation = 0;
    double border_width = 1;
    std::string border_style = "S"; // S, D, B, I, U
    std::vector<double> border_dash;
    Box rect; // page space
    std::uint32_t annotation_flags = 0; // /F
    bool hidden() const { return annotation_flags & 2; }
    bool has_appearance = false; // an /AP /N stream (or states) to draw
    // The font family of the default appearance, for a widget drawn by
    // HTML: "sans-serif", "serif" or "monospace".
    std::string font_family = "sans-serif";
};

// The widgets on a page, in the page's /Annots order.
std::vector<FormField> page_fields(Document const& doc, std::size_t index);
// One widget annotation read as a field; nullopt when it is not a widget
// (or belongs to no field).
std::optional<FormField> widget_field(Document const& doc, Dict const& annotation, Ref widget_ref);
// Whether the file asks viewers to make every field's appearance themselves.
bool need_appearances(Document const& doc);

// The appearance a viewer draws for a widget: a content stream in the
// widget's own space (its rectangle's size from the origin, /MK /R
// applied by the matrix), the resources it uses, and the box and matrix a
// form XObject of it carries. `value` is the viewer's, or the file's when
// nullopt.
struct Appearance {
    std::string content;
    Dict resources;
    Box bbox;
    std::array<double, 6> matrix { 1, 0, 0, 1, 0, 0 };
};
Appearance widget_appearance(Document const& doc, FormField const& field, FormValue const* value);

// The appearance state a widget shows for a value (its on-state or Off).
std::string widget_state(FormField const& field, FormValue const* value);

// A FreeText annotation's text, drawn from its /DA and /Contents when it
// carries no appearance (§12.5.6.6), in page space.
std::string free_text_appearance(Document const& doc, Dict const& annotation, Box const& rect, Dict& resources);

// A Text annotation's icon (§12.5.6.4: Comment, Key, Note, Help,
// NewParagraph, Paragraph, Insert), as viewers draw one, in page space.
std::string note_icon_appearance(Document const& doc, Dict const& annotation, Box const& rect, Dict& resources);

// A text string (§7.9.2.2) from UTF-8: PDFDocEncoding where it suffices,
// else UTF-16BE with its byte order mark.
std::string utf8_to_text_string(std::string const& utf8);
DefaultAppearance parse_default_appearance(std::string const& da);

}
