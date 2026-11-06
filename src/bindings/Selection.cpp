#include "bindings/NodeSupport.h"

// The Selection API (W3C Selection API, 2024 draft): the one selection a
// document's window has — none, or one live Range and which way it was made
// — read and changed by scripts through window.getSelection() and
// document.getSelection(), and by the window through Realm::selection and
// Realm::set_selection (a click in a field of rich text, a drag, a key that
// moves the caret). Every change queues one `selectionchange` at the
// document. The range is an ordinary Range object: a script that holds it
// and moves its boundaries moves the selection.

#include "core/Ascii.h"
#include "dom/Dom.h"

#include <optional>
#include <string>
#include <string_view>

namespace sashfold::bindings {

namespace {

class SelectionObject final : public js::Object {
public:
    SelectionObject(js::Object* prototype, js::RealmRecord* realm)
        : Object(prototype, Class::Host)
        , m_realm(realm)
    {
    }
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(range_object);
    }

    js::Object* range_object = nullptr; // a Range, or null for none
    bool backward = false; // the anchor is the range's end
    bool directionless = true; // made by addRange: no direction to tell

private:
    js::RealmRecord* m_realm;
};

dom::Range* range_of(SelectionObject const& selection)
{
    if (!selection.range_object)
        return nullptr;
    dom::Range* const range = live_range_of(js::Value::object(selection.range_object));
    // A range whose document ended holds nothing, and so neither does the
    // selection.
    return range && range->start_node && range->end_node ? range : nullptr;
}

SelectionObject& selection_object(Realm::Internals& in)
{
    return *static_cast<SelectionObject*>(same_object(in, "selection", [&] {
        return in.interpreter.heap().allocate<SelectionObject>(in.prototype("Selection"), in.realm_record);
    }));
}

// One `selectionchange` at the document per turn of changes (§3 "queue a
// task to fire selectionchange" while none is queued).
void selection_changed(Realm::Internals& in)
{
    ++in.selection_version;
    if (in.selectionchange_queued)
        return;
    in.selectionchange_queued = true;
    in.post_task([&in] {
        in.selectionchange_queued = false;
        Realm::Internals::Entry const entry(in);
        js::Interpreter::Roots const task_roots(in.interpreter);
        EventObject* const event = in.new_event("Event", "selectionchange", false, false);
        in.interpreter.root(js::Value::object(event));
        event->is_trusted = true;
        in.dispatch(*event, in.wrap(*in.document));
    });
}

// Sets the selection's range to a new one between the two points, ordered,
// the anchor the first of them.
void set_points(Realm::Internals& in, SelectionObject& selection, dom::Node& anchor, std::uint32_t anchor_offset, dom::Node& focus,
    std::uint32_t focus_offset)
{
    bool const backward = compare_points(anchor, anchor_offset, focus, focus_offset) > 0;
    selection.range_object = backward ? make_live_range(in, focus, focus_offset, anchor, anchor_offset)
                                      : make_live_range(in, anchor, anchor_offset, focus, focus_offset);
    selection.backward = backward;
    selection.directionless = false;
    selection_changed(in);
}

void set_none(Realm::Internals& in, SelectionObject& selection)
{
    if (!selection.range_object)
        return;
    selection.range_object = nullptr;
    selection.backward = false;
    selection.directionless = true;
    selection_changed(in);
}

// A point is the selection's to take only in the selection's document; one
// elsewhere is ignored, without an error (§3 "if node's root is not the
// document associated with this, return").
bool in_document(Realm::Internals& in, dom::Node const& node)
{
    return &node.root() == in.document;
}

std::optional<SelectionObject*> this_selection(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* const selection = dynamic_cast<SelectionObject*>(this_value.as_object()))
            return selection;
    }
    interpreter.throw_type_error("Illegal invocation");
    return std::nullopt;
}

// A node argument that may be null: nullopt when a TypeError was thrown,
// a null pointer for null.
std::optional<dom::Node*> nullable_node(Realm::Internals& in, Args args, std::size_t index, std::string_view method)
{
    js::Value const value = js::argument(args, index);
    if (value.is_null())
        return static_cast<dom::Node*>(nullptr);
    return node_argument(in, args, index, method);
}

std::optional<std::uint32_t> offset_argument(Realm::Internals& in, Args args, std::size_t index)
{
    std::optional<double> const number = in.interpreter.to_number(js::argument(args, index));
    if (!number)
        return std::nullopt;
    return to_unsigned_long(*number);
}

template<typename Body>
void selection_method(Realm::Internals& in, js::Object& prototype, std::string_view name, int length, Body body)
{
    define_operation(in.interpreter, prototype, name, length,
        [body](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<SelectionObject*> const selection = this_selection(interpreter, this_value);
            if (!selection)
                return std::nullopt;
            return body(internals_of(interpreter), **selection, args);
        });
}

template<typename Read>
void selection_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    define_getter(in, prototype, name, [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
        std::optional<SelectionObject*> const selection = this_selection(interpreter, this_value);
        if (!selection)
            return std::nullopt;
        return read(internals_of(interpreter), **selection);
    });
}

// collapse() and setPosition() (§3).
Native collapse_to(Realm::Internals& in, SelectionObject& selection, Args args, std::string_view method)
{
    std::optional<dom::Node*> const node = nullable_node(in, args, 0, method);
    if (!node)
        return std::nullopt;
    std::optional<std::uint32_t> const offset = offset_argument(in, args, 1);
    if (!offset)
        return std::nullopt;
    if (!*node) {
        set_none(in, selection);
        return js::Value::undefined();
    }
    if ((*node)->type() == dom::NodeType::DocumentType)
        return in.throw_dom_exception("InvalidNodeTypeError", "A doctype cannot hold the selection.");
    if (*offset > boundary_length(**node))
        return in.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length.");
    if (!in_document(in, **node))
        return js::Value::undefined();
    set_points(in, selection, **node, *offset, **node, *offset);
    return js::Value::undefined();
}

// The text a character step crosses: the Text nodes of the document in tree
// order, which modify() walks over.
dom::Node* next_text(dom::Node* node, bool forward, dom::Node const* stop)
{
    auto const next_in_order = [&](dom::Node* at) -> dom::Node* {
        if (forward) {
            if (!at->children().empty())
                return at->children().front();
            while (at && at != stop) {
                if (dom::Node* const sibling = next_sibling_of(*at))
                    return sibling;
                at = at->parent();
            }
            return nullptr;
        }
        if (at == stop)
            return nullptr;
        dom::Node* const parent = at->parent();
        if (!parent)
            return nullptr;
        std::uint32_t const index = at->index();
        if (index == 0)
            return parent;
        dom::Node* deepest = parent->children()[index - 1];
        while (!deepest->children().empty())
            deepest = deepest->children().back();
        return deepest;
    };
    for (dom::Node* at = next_in_order(node); at; at = next_in_order(at)) {
        if (at->is_text() && !character_data(*at).empty())
            return at;
    }
    return nullptr;
}

bool is_word_unit(char16_t unit)
{
    return (unit >= u'0' && unit <= u'9') || (unit >= u'A' && unit <= u'Z') || (unit >= u'a' && unit <= u'z') || unit == u'_'
        || (unit >= 0x00C0 && unit != 0x00D7 && unit != 0x00F7 && !(unit >= 0x2000 && unit <= 0x206F) && unit != 0x3000
            && unit != 0x3001 && unit != 0x3002);
}

// One step of modify() over the document's text: a character (a surrogate
// pair together) or a word, from a boundary point. Steps that leave a Text
// node go on in the next one with any text.
std::pair<dom::Node*, std::uint32_t> step_point(dom::Node* node, std::uint32_t offset, bool forward, bool word, dom::Node const* stop)
{
    // A point between nodes stands at the start of the text after it, or
    // the end of the text before it.
    if (!node->is_text()) {
        dom::Node* const child = offset < node->children().size() ? node->children()[offset] : nullptr;
        dom::Node* text = nullptr;
        if (forward)
            text = child && child->is_text() ? child : next_text(child ? child : node, true, stop);
        else
            text = next_text(child ? child : node, false, stop);
        if (!text)
            return { node, offset };
        node = text;
        offset = forward ? 0 : static_cast<std::uint32_t>(data_units(*text).size());
    }
    std::u16string units = data_units(*node);
    auto const move_one = [&]() -> bool {
        if (forward ? offset >= units.size() : offset == 0) {
            dom::Node* const text = next_text(node, forward, stop);
            if (!text)
                return false;
            node = text;
            units = data_units(*text);
            offset = forward ? 0 : static_cast<std::uint32_t>(units.size());
            if (units.empty())
                return false;
        }
        if (forward) {
            offset += (offset + 1 < units.size() && units[offset] >= 0xD800 && units[offset] <= 0xDBFF) ? 2 : 1;
        } else {
            offset -= (offset >= 2 && units[offset - 1] >= 0xDC00 && units[offset - 1] <= 0xDFFF) ? 2 : 1;
        }
        return true;
    };
    auto const unit_ahead = [&]() -> std::optional<char16_t> {
        if (forward)
            return offset < units.size() ? std::optional<char16_t>(units[offset]) : std::nullopt;
        return offset > 0 ? std::optional<char16_t>(units[offset - 1]) : std::nullopt;
    };
    if (!word) {
        move_one();
        return { node, offset };
    }
    // A word: forward, past the spaces to the end of the next word; back,
    // past the spaces to the start of the word before.
    while (true) {
        std::optional<char16_t> const ahead = unit_ahead();
        if (ahead && is_word_unit(*ahead))
            break;
        if (!move_one())
            return { node, offset };
    }
    while (true) {
        std::optional<char16_t> const ahead = unit_ahead();
        if (!ahead || !is_word_unit(*ahead))
            break;
        if (!move_one())
            break;
    }
    return { node, offset };
}

} // namespace

std::optional<Realm::SelectionPoints> Realm::selection() const
{
    Internals& in = *m_internals;
    auto const kept = in.same_objects.find("selection");
    if (kept == in.same_objects.end())
        return std::nullopt;
    auto const& selection = *static_cast<SelectionObject*>(kept->second);
    dom::Range const* const range = range_of(selection);
    if (!range)
        return std::nullopt;
    SelectionPoints points { range->start_node, range->start_offset, range->end_node, range->end_offset };
    if (selection.backward) {
        std::swap(points.anchor, points.focus);
        std::swap(points.anchor_offset, points.focus_offset);
    }
    return points;
}

void Realm::set_selection(dom::Node& anchor, std::uint32_t anchor_offset, dom::Node& focus, std::uint32_t focus_offset)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    js::Heap::NoCollect const no_collect(in.interpreter.heap());
    if (!in_document(in, anchor) || !in_document(in, focus))
        return;
    anchor_offset = std::min(anchor_offset, boundary_length(anchor));
    focus_offset = std::min(focus_offset, boundary_length(focus));
    if (std::optional<SelectionPoints> const now = selection(); now && now->anchor == &anchor && now->anchor_offset == anchor_offset
        && now->focus == &focus && now->focus_offset == focus_offset)
        return;
    set_points(in, selection_object(in), anchor, anchor_offset, focus, focus_offset);
}

void Realm::clear_selection()
{
    Internals& in = *m_internals;
    auto const kept = in.same_objects.find("selection");
    if (kept == in.same_objects.end())
        return;
    Internals::HostEntry const host(in.agent);
    set_none(in, *static_cast<SelectionObject*>(kept->second));
}

std::uint64_t Realm::selection_version() const
{
    return m_internals->selection_version;
}

js::Value selection_of(Realm::Internals& in)
{
    return js::Value::object(&selection_object(in));
}

void note_selection_changed(Realm::Internals& in)
{
    selection_changed(in);
}

bool selection_range(Realm::Internals& in, dom::Range*& range, bool& backward)
{
    auto const kept = in.same_objects.find("selection");
    if (kept == in.same_objects.end())
        return false;
    auto& selection = *static_cast<SelectionObject*>(kept->second);
    range = range_of(selection);
    backward = selection.backward;
    return range != nullptr;
}

void install_selection(Realm::Internals& in)
{
    js::Heap::NoCollect const guard(in.interpreter.heap());
    js::Object* const prototype = define_interface(in, "Selection", nullptr);

    // The anchor and the focus: the range's start and end, the other way
    // round when the selection was made backward.
    for (bool const anchor : { true, false }) {
        selection_getter(in, *prototype, anchor ? "anchorNode" : "focusNode", [anchor](Realm::Internals& internals, SelectionObject& s) -> Native {
            dom::Range const* const range = range_of(s);
            if (!range)
                return js::Value::null();
            return internals.realm.wrap_or_null(anchor != s.backward ? range->start_node : range->end_node);
        });
        selection_getter(in, *prototype, anchor ? "anchorOffset" : "focusOffset", [anchor](Realm::Internals&, SelectionObject& s) -> Native {
            dom::Range const* const range = range_of(s);
            if (!range)
                return js::Value::number(0);
            return js::Value::number(anchor != s.backward ? range->start_offset : range->end_offset);
        });
    }
    selection_getter(in, *prototype, "isCollapsed", [](Realm::Internals&, SelectionObject& s) -> Native {
        dom::Range const* const range = range_of(s);
        return js::Value::boolean(!range || (range->start_node == range->end_node && range->start_offset == range->end_offset));
    });
    selection_getter(in, *prototype, "rangeCount", [](Realm::Internals&, SelectionObject& s) -> Native {
        return js::Value::number(range_of(s) ? 1 : 0);
    });
    selection_getter(in, *prototype, "type", [](Realm::Internals& internals, SelectionObject& s) -> Native {
        dom::Range const* const range = range_of(s);
        if (!range)
            return internals.string("None");
        return internals.string(range->start_node == range->end_node && range->start_offset == range->end_offset ? "Caret" : "Range");
    });
    selection_getter(in, *prototype, "direction", [](Realm::Internals& internals, SelectionObject& s) -> Native {
        dom::Range const* const range = range_of(s);
        if (!range || s.directionless || (range->start_node == range->end_node && range->start_offset == range->end_offset))
            return internals.string("none");
        return internals.string(s.backward ? "backward" : "forward");
    });

    selection_method(in, *prototype, "getRangeAt", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        std::optional<std::uint32_t> const index = offset_argument(internals, args, 0);
        if (!index)
            return std::nullopt;
        if (*index != 0 || !range_of(s))
            return internals.throw_dom_exception("IndexSizeError", "The index is not a valid range index.");
        return js::Value::object(s.range_object);
    });
    selection_method(in, *prototype, "addRange", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        js::Value const value = js::argument(args, 0);
        dom::Range* const range = live_range_of(value);
        if (!range)
            return internals.interpreter.throw_type_error("Failed to execute 'addRange' on 'Selection': parameter 1 is not of type 'Range'.");
        if (!range->start_node || !in_document(internals, *range->start_node) || range_of(s))
            return js::Value::undefined();
        s.range_object = value.as_object();
        s.backward = false;
        s.directionless = true;
        selection_changed(internals);
        return js::Value::undefined();
    });
    selection_method(in, *prototype, "removeRange", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        js::Value const value = js::argument(args, 0);
        if (!live_range_of(value))
            return internals.interpreter.throw_type_error("Failed to execute 'removeRange' on 'Selection': parameter 1 is not of type 'Range'.");
        if (!s.range_object || value.as_object() != s.range_object)
            return internals.throw_dom_exception("NotFoundError", "The range is not the selection's.");
        set_none(internals, s);
        return js::Value::undefined();
    });
    for (std::string_view const name : { "removeAllRanges", "empty" }) {
        selection_method(in, *prototype, name, 0, [](Realm::Internals& internals, SelectionObject& s, Args) -> Native {
            set_none(internals, s);
            return js::Value::undefined();
        });
    }
    selection_method(in, *prototype, "collapse", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        return collapse_to(internals, s, args, "collapse");
    });
    selection_method(in, *prototype, "setPosition", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        return collapse_to(internals, s, args, "setPosition");
    });
    for (bool const start : { true, false }) {
        selection_method(in, *prototype, start ? "collapseToStart" : "collapseToEnd", 0,
            [start](Realm::Internals& internals, SelectionObject& s, Args) -> Native {
                dom::Range const* const range = range_of(s);
                if (!range)
                    return internals.throw_dom_exception("InvalidStateError", "There is no selection to collapse.");
                dom::Node& node = start ? *range->start_node : *range->end_node;
                std::uint32_t const offset = start ? range->start_offset : range->end_offset;
                set_points(internals, s, node, offset, node, offset);
                return js::Value::undefined();
            });
    }
    selection_method(in, *prototype, "extend", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "extend");
        if (!node)
            return std::nullopt;
        std::optional<std::uint32_t> const offset = offset_argument(internals, args, 1);
        if (!offset)
            return std::nullopt;
        if (!in_document(internals, **node))
            return js::Value::undefined();
        dom::Range const* const range = range_of(s);
        if (!range)
            return internals.throw_dom_exception("InvalidStateError", "There is no selection to extend.");
        if ((*node)->type() == dom::NodeType::DocumentType)
            return internals.throw_dom_exception("InvalidNodeTypeError", "A doctype cannot hold the selection.");
        if (*offset > boundary_length(**node))
            return internals.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length.");
        dom::Node& anchor = s.backward ? *range->end_node : *range->start_node;
        std::uint32_t const anchor_offset = s.backward ? range->end_offset : range->start_offset;
        set_points(internals, s, anchor, anchor_offset, **node, *offset);
        return js::Value::undefined();
    });
    selection_method(in, *prototype, "setBaseAndExtent", 4, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        std::optional<dom::Node*> const anchor = node_argument(internals, args, 0, "setBaseAndExtent");
        if (!anchor)
            return std::nullopt;
        std::optional<std::uint32_t> const anchor_offset = offset_argument(internals, args, 1);
        if (!anchor_offset)
            return std::nullopt;
        std::optional<dom::Node*> const focus = node_argument(internals, args, 2, "setBaseAndExtent");
        if (!focus)
            return std::nullopt;
        std::optional<std::uint32_t> const focus_offset = offset_argument(internals, args, 3);
        if (!focus_offset)
            return std::nullopt;
        if ((*anchor)->type() == dom::NodeType::DocumentType || (*focus)->type() == dom::NodeType::DocumentType)
            return internals.throw_dom_exception("InvalidNodeTypeError", "A doctype cannot hold the selection.");
        if (*anchor_offset > boundary_length(**anchor) || *focus_offset > boundary_length(**focus))
            return internals.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length.");
        if (!in_document(internals, **anchor) || !in_document(internals, **focus))
            return js::Value::undefined();
        set_points(internals, s, **anchor, *anchor_offset, **focus, *focus_offset);
        return js::Value::undefined();
    });
    selection_method(in, *prototype, "selectAllChildren", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "selectAllChildren");
        if (!node)
            return std::nullopt;
        if ((*node)->type() == dom::NodeType::DocumentType)
            return internals.throw_dom_exception("InvalidNodeTypeError", "A doctype cannot hold the selection.");
        if (!in_document(internals, **node))
            return js::Value::undefined();
        set_points(internals, s, **node, 0, **node, static_cast<std::uint32_t>((*node)->children().size()));
        return js::Value::undefined();
    });
    selection_method(in, *prototype, "deleteFromDocument", 0, [](Realm::Internals& internals, SelectionObject& s, Args) -> Native {
        dom::Range* const range = range_of(s);
        if (!range)
            return js::Value::undefined();
        Native const done = delete_range_contents(internals, *range);
        selection_changed(internals);
        return done;
    });
    selection_method(in, *prototype, "containsNode", 1, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "containsNode");
        if (!node)
            return std::nullopt;
        dom::Range const* const range = range_of(s);
        if (!range || !in_document(internals, **node))
            return js::Value::boolean(false);
        return js::Value::boolean(range_contains_node(*range, **node, js::Interpreter::to_boolean(js::argument(args, 1))));
    });
    selection_method(in, *prototype, "toString", 0, [](Realm::Internals& internals, SelectionObject& s, Args) -> Native {
        dom::Range const* const range = range_of(s);
        return internals.string(range ? text_in_range(*range) : std::string());
    });
    // getComposedRanges(): the range as a StaticRange (no shadow root is
    // passed in, so none is looked into).
    selection_method(in, *prototype, "getComposedRanges", 0, [](Realm::Internals& internals, SelectionObject& s, Args) -> Native {
        js::Heap::NoCollect const no_collect(internals.interpreter.heap());
        js::ArrayObject* const array = internals.interpreter.new_array();
        if (dom::Range const* const range = range_of(s))
            array->push(js::Value::object(make_static_range(internals, *range->start_node, range->start_offset, *range->end_node, range->end_offset)));
        return js::Value::object(array);
    });
    // modify(alter, direction, granularity): the caret or the focus moved
    // by a character or a word over the document's text, or to its start or
    // end. ⛔ A line, sentence or paragraph needs the page as laid out and
    // changes nothing yet.
    selection_method(in, *prototype, "modify", 3, [](Realm::Internals& internals, SelectionObject& s, Args args) -> Native {
        std::optional<std::string> const alter = internals.to_utf8(js::argument(args, 0));
        std::optional<std::string> const direction = alter ? internals.to_utf8(js::argument(args, 1)) : std::nullopt;
        std::optional<std::string> const granularity = direction ? internals.to_utf8(js::argument(args, 2)) : std::nullopt;
        if (!granularity)
            return std::nullopt;
        dom::Range const* const range = range_of(s);
        if (!range)
            return js::Value::undefined();
        auto const lowered = [](std::string text) {
            for (char& c : text)
                c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
            return text;
        };
        std::string const a = lowered(*alter);
        std::string const d = lowered(*direction);
        std::string const g = lowered(*granularity);
        if ((a != "move" && a != "extend") || (d != "forward" && d != "backward" && d != "left" && d != "right"))
            return js::Value::undefined();
        bool const forward = d == "forward" || d == "right";
        dom::Node* anchor = s.backward ? range->end_node : range->start_node;
        std::uint32_t anchor_offset = s.backward ? range->end_offset : range->start_offset;
        dom::Node* focus = s.backward ? range->start_node : range->end_node;
        std::uint32_t focus_offset = s.backward ? range->start_offset : range->end_offset;
        bool const collapsed = anchor == focus && anchor_offset == focus_offset;
        // Moving a selection that is not collapsed collapses it to the end
        // in the direction moved, without stepping.
        if (a == "move" && !collapsed && (g == "character" || g == "word")) {
            dom::Node& node = forward ? *range->end_node : *range->start_node;
            std::uint32_t const offset = forward ? range->end_offset : range->start_offset;
            set_points(internals, s, node, offset, node, offset);
            return js::Value::undefined();
        }
        std::pair<dom::Node*, std::uint32_t> moved { focus, focus_offset };
        if (g == "character" || g == "word") {
            moved = step_point(focus, focus_offset, forward, g == "word", nullptr);
        } else if (g == "documentboundary") {
            for (int steps = 0; steps < 1 << 20; ++steps) {
                std::pair<dom::Node*, std::uint32_t> const next = step_point(moved.first, moved.second, forward, false, nullptr);
                if (next == moved)
                    break;
                moved = next;
            }
        } else {
            // A line, a sentence or a paragraph: the page as laid out
            // decides, which the window has and a script's call does not.
            return js::Value::undefined();
        }
        if (a == "move")
            set_points(internals, s, *moved.first, moved.second, *moved.first, moved.second);
        else
            set_points(internals, s, *anchor, anchor_offset, *moved.first, moved.second);
        return js::Value::undefined();
    });
}

}
