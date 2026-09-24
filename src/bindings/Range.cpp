#include "bindings/NodeSupport.h"

// AbstractRange, Range and StaticRange (DOM §5), and Range's
// createContextualFragment (DOM Parsing §7). A range's boundary points are a
// dom::Range the documents of its nodes keep: the tree's own insertions and
// removals move a live range (dom/Dom.cpp), and the character data natives
// move it when they replace, split or merge data (Node.cpp).

#include "dom/Dom.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

class RangeObject final : public js::Object {
public:
    RangeObject(js::Object* prototype, bool live)
        : Object(prototype, Class::Host)
    {
        m_range.live = live;
    }
    ~RangeObject() override { dom::release_range(m_range); }

    dom::Range& range() { return m_range; }
    bool live() const { return m_range.live; }

    // A range holds its boundary nodes, and so their wrappers and whatever
    // scripts put on them.
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        if (m_range.start_node)
            tracer.visit(m_range.start_node->wrapper);
        if (m_range.end_node)
            tracer.visit(m_range.end_node->wrapper);
    }

private:
    dom::Range m_range;
};

// A range made for the length of one algorithm, let go of on every way out.
struct ScopedRange {
    dom::Range range;
    ScopedRange() = default;
    ScopedRange(ScopedRange const&) = delete;
    ScopedRange& operator=(ScopedRange const&) = delete;
    ~ScopedRange() { dom::release_range(range); }
};

// The range behind `this`: a Range, or with `any` a StaticRange too.
std::optional<RangeObject*> this_range(js::Interpreter& interpreter, js::Value const& this_value, bool any)
{
    if (this_value.is_object()) {
        if (auto* range = dynamic_cast<RangeObject*>(this_value.as_object()); range && (any || range->live()))
            return range;
    }
    interpreter.throw_type_error("Illegal invocation");
    return std::nullopt;
}

// A range whose document ended before it holds nothing (dom::release_range).
bool is_released(dom::Range const& range)
{
    return !range.start_node || !range.end_node;
}

dom::Node const* root_of(dom::Range const& range)
{
    return range.start_node ? &range.start_node->root() : nullptr;
}

bool is_collapsed(dom::Range const& range)
{
    return range.start_node == range.end_node && range.start_offset == range.end_offset;
}

std::uint32_t utf16_length(std::string_view utf8)
{
    std::uint32_t units = 0;
    for (char const c : utf8) {
        auto const byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) != 0x80)
            units += byte >= 0xF0 ? 2 : 1;
    }
    return units;
}

// A node's length (DOM §4.2): 0 for a doctype, the code units of a character
// data node's data, the children of any other.
std::uint32_t node_length(dom::Node const& node)
{
    if (node.type() == dom::NodeType::DocumentType)
        return 0;
    if (is_character_data(node))
        return utf16_length(character_data(node));
    return static_cast<std::uint32_t>(node.children().size());
}

std::u16string substring_data(dom::Node const& node, std::uint32_t offset, std::uint32_t count)
{
    std::u16string const units = data_units(node);
    return units.substr(std::min<std::size_t>(offset, units.size()), count);
}

enum class Position { Before, Equal, After };

// The position of one boundary point relative to another in the same tree
// (DOM §5.2).
Position position_of(dom::Node const& node_a, std::uint32_t offset_a, dom::Node const& node_b, std::uint32_t offset_b)
{
    if (&node_a == &node_b)
        return offset_a == offset_b ? Position::Equal : offset_a < offset_b ? Position::Before : Position::After;
    if (precedes_in_tree_order(node_b, node_a))
        return position_of(node_b, offset_b, node_a, offset_a) == Position::Before ? Position::After : Position::Before;
    if (is_inclusive_ancestor(node_a, node_b)) {
        dom::Node const* child = &node_b;
        while (child->parent() != &node_a)
            child = child->parent();
        if (child->index() < offset_a)
            return Position::After;
    }
    return Position::Before;
}

// A node wholly inside the range (DOM §5.5 "contained").
bool is_contained(dom::Node const& node, dom::Range const& range)
{
    return &node.root() == root_of(range) && position_of(node, 0, *range.start_node, range.start_offset) == Position::After
        && position_of(node, node_length(node), *range.end_node, range.end_offset) == Position::Before;
}

// An inclusive ancestor of one boundary's node and not of the other's.
bool is_partially_contained(dom::Node const& node, dom::Range const& range)
{
    return is_inclusive_ancestor(node, *range.start_node) != is_inclusive_ancestor(node, *range.end_node);
}

dom::Node* common_ancestor(dom::Range const& range)
{
    dom::Node* container = range.start_node;
    while (!is_inclusive_ancestor(*container, *range.end_node))
        container = container->parent();
    return container;
}

// Where extract and delete leave a range: the start, when its node holds the
// end; otherwise just after the start's ancestor below the common ancestor.
std::pair<dom::Node*, std::uint32_t> boundary_after_removal(dom::Range const& range)
{
    if (is_inclusive_ancestor(*range.start_node, *range.end_node))
        return { range.start_node, range.start_offset };
    dom::Node* reference = range.start_node;
    while (reference->parent() && !is_inclusive_ancestor(*reference->parent(), *range.end_node))
        reference = reference->parent();
    return { reference->parent(), reference->index() + 1 };
}

Native throw_released(Realm::Internals& in)
{
    return in.throw_dom_exception("InvalidStateError", "The range's document is gone.");
}

// DOM §5.5 "set the start or end".
Native set_boundary(Realm::Internals& in, dom::Range& range, dom::Node& node, std::uint32_t offset, bool start)
{
    if (node.type() == dom::NodeType::DocumentType)
        return in.throw_dom_exception("InvalidNodeTypeError", "The node provided is a doctype, which is not a boundary point.");
    std::uint32_t const length = node_length(node);
    if (offset > length)
        return in.throw_dom_exception("IndexSizeError",
            "The offset " + std::to_string(offset) + " is larger than the node's length (" + std::to_string(length) + ").");
    bool const same_tree = !is_released(range) && root_of(range) == &node.root();
    if (start) {
        if (!same_tree || position_of(node, offset, *range.end_node, range.end_offset) == Position::After)
            dom::set_range(range, &node, offset, &node, offset);
        else
            dom::set_range(range, &node, offset, range.end_node, range.end_offset);
    } else {
        if (!same_tree || position_of(node, offset, *range.start_node, range.start_offset) == Position::Before)
            dom::set_range(range, &node, offset, &node, offset);
        else
            dom::set_range(range, range.start_node, range.start_offset, &node, offset);
    }
    return js::Value::undefined();
}

// DOM §5.5 "select".
Native select_node(Realm::Internals& in, dom::Range& range, dom::Node& node)
{
    dom::Node* const parent = node.parent();
    if (!parent)
        return in.throw_dom_exception("InvalidNodeTypeError", "The node provided has no parent.");
    std::uint32_t const index = node.index();
    dom::set_range(range, parent, index, parent, index + 1);
    return js::Value::undefined();
}

// A boundary point's arguments: a node, then an offset as an unsigned long.
std::optional<std::pair<dom::Node*, std::uint32_t>> point_arguments(Realm::Internals& in, Args args, std::string_view method)
{
    if (args.size() < 2) {
        in.interpreter.throw_type_error("Failed to execute '" + std::string(method) + "' on 'Range': 2 arguments required.");
        return std::nullopt;
    }
    std::optional<dom::Node*> const node = node_argument(in, args, 0, method);
    if (!node)
        return std::nullopt;
    std::optional<double> const offset = in.interpreter.to_number(args[1]);
    if (!offset)
        return std::nullopt;
    return std::pair { *node, to_unsigned_long(*offset) };
}

Native append(Realm::Internals& in, dom::Node& parent, dom::Node& node)
{
    return pre_insert(in, parent, node, nullptr);
}

// DOM §5.5 "extract".
std::optional<dom::Node*> extract(Realm::Internals& in, dom::Range& range)
{
    dom::Node* const fragment = range.start_node->document().create<dom::DocumentFragment>();
    if (is_collapsed(range))
        return fragment;
    dom::Node* const start = range.start_node;
    std::uint32_t const start_offset = range.start_offset;
    dom::Node* const end = range.end_node;
    std::uint32_t const end_offset = range.end_offset;
    if (start == end && is_character_data(*start)) {
        dom::Node* const clone = clone_node(in, *start, false);
        set_data(in, *clone, substring_data(*start, start_offset, end_offset - start_offset));
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        replace_data(in, *start, start_offset, end_offset - start_offset, u"");
        return fragment;
    }
    dom::Node* const common = common_ancestor(range);
    dom::Node* first_partial = nullptr;
    if (!is_inclusive_ancestor(*start, *end)) {
        for (dom::Node* child : common->children()) {
            if (is_partially_contained(*child, range)) {
                first_partial = child;
                break;
            }
        }
    }
    dom::Node* last_partial = nullptr;
    if (!is_inclusive_ancestor(*end, *start)) {
        auto const& children = common->children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (is_partially_contained(**it, range)) {
                last_partial = *it;
                break;
            }
        }
    }
    std::vector<dom::Node*> contained;
    for (dom::Node* child : common->children()) {
        if (is_contained(*child, range)) {
            if (child->type() == dom::NodeType::DocumentType) {
                in.throw_dom_exception("HierarchyRequestError", "A doctype cannot be extracted.");
                return std::nullopt;
            }
            contained.push_back(child);
        }
    }
    auto const [new_node, new_offset] = boundary_after_removal(range);

    if (first_partial && is_character_data(*first_partial)) {
        std::uint32_t const count = node_length(*start) - start_offset;
        dom::Node* const clone = clone_node(in, *start, false);
        set_data(in, *clone, substring_data(*start, start_offset, count));
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        replace_data(in, *start, start_offset, count, u"");
    } else if (first_partial) {
        dom::Node* const clone = clone_node(in, *first_partial, false);
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        ScopedRange subrange;
        dom::set_range(subrange.range, start, start_offset, first_partial, node_length(*first_partial));
        std::optional<dom::Node*> const subfragment = extract(in, subrange.range);
        if (!subfragment || !append(in, *clone, **subfragment))
            return std::nullopt;
    }
    for (dom::Node* child : contained) {
        if (!append(in, *fragment, *child))
            return std::nullopt;
    }
    if (last_partial && is_character_data(*last_partial)) {
        dom::Node* const clone = clone_node(in, *end, false);
        set_data(in, *clone, substring_data(*end, 0, end_offset));
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        replace_data(in, *end, 0, end_offset, u"");
    } else if (last_partial) {
        dom::Node* const clone = clone_node(in, *last_partial, false);
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        ScopedRange subrange;
        dom::set_range(subrange.range, last_partial, 0, end, end_offset);
        std::optional<dom::Node*> const subfragment = extract(in, subrange.range);
        if (!subfragment || !append(in, *clone, **subfragment))
            return std::nullopt;
    }
    dom::set_range(range, new_node, new_offset, new_node, new_offset);
    return fragment;
}

// DOM §5.5 "clone the contents".
std::optional<dom::Node*> clone_contents(Realm::Internals& in, dom::Range const& range)
{
    dom::Node* const fragment = range.start_node->document().create<dom::DocumentFragment>();
    if (is_collapsed(range))
        return fragment;
    dom::Node* const start = range.start_node;
    std::uint32_t const start_offset = range.start_offset;
    dom::Node* const end = range.end_node;
    std::uint32_t const end_offset = range.end_offset;
    if (start == end && is_character_data(*start)) {
        dom::Node* const clone = clone_node(in, *start, false);
        set_data(in, *clone, substring_data(*start, start_offset, end_offset - start_offset));
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        return fragment;
    }
    dom::Node* const common = common_ancestor(range);
    dom::Node* first_partial = nullptr;
    if (!is_inclusive_ancestor(*start, *end)) {
        for (dom::Node* child : common->children()) {
            if (is_partially_contained(*child, range)) {
                first_partial = child;
                break;
            }
        }
    }
    dom::Node* last_partial = nullptr;
    if (!is_inclusive_ancestor(*end, *start)) {
        auto const& children = common->children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (is_partially_contained(**it, range)) {
                last_partial = *it;
                break;
            }
        }
    }
    std::vector<dom::Node*> contained;
    for (dom::Node* child : common->children()) {
        if (is_contained(*child, range)) {
            if (child->type() == dom::NodeType::DocumentType) {
                in.throw_dom_exception("HierarchyRequestError", "A doctype cannot be cloned into a fragment.");
                return std::nullopt;
            }
            contained.push_back(child);
        }
    }

    if (first_partial && is_character_data(*first_partial)) {
        dom::Node* const clone = clone_node(in, *start, false);
        set_data(in, *clone, substring_data(*start, start_offset, node_length(*start) - start_offset));
        if (!append(in, *fragment, *clone))
            return std::nullopt;
    } else if (first_partial) {
        dom::Node* const clone = clone_node(in, *first_partial, false);
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        ScopedRange subrange;
        dom::set_range(subrange.range, start, start_offset, first_partial, node_length(*first_partial));
        std::optional<dom::Node*> const subfragment = clone_contents(in, subrange.range);
        if (!subfragment || !append(in, *clone, **subfragment))
            return std::nullopt;
    }
    for (dom::Node* child : contained) {
        if (!append(in, *fragment, *clone_node(in, *child, true)))
            return std::nullopt;
    }
    if (last_partial && is_character_data(*last_partial)) {
        dom::Node* const clone = clone_node(in, *end, false);
        set_data(in, *clone, substring_data(*end, 0, end_offset));
        if (!append(in, *fragment, *clone))
            return std::nullopt;
    } else if (last_partial) {
        dom::Node* const clone = clone_node(in, *last_partial, false);
        if (!append(in, *fragment, *clone))
            return std::nullopt;
        ScopedRange subrange;
        dom::set_range(subrange.range, last_partial, 0, end, end_offset);
        std::optional<dom::Node*> const subfragment = clone_contents(in, subrange.range);
        if (!subfragment || !append(in, *clone, **subfragment))
            return std::nullopt;
    }
    return fragment;
}

// DOM §5.5 deleteContents().
Native delete_contents(Realm::Internals& in, dom::Range& range)
{
    if (is_collapsed(range))
        return js::Value::undefined();
    dom::Node* const start = range.start_node;
    std::uint32_t const start_offset = range.start_offset;
    dom::Node* const end = range.end_node;
    std::uint32_t const end_offset = range.end_offset;
    if (start == end && is_character_data(*start)) {
        replace_data(in, *start, start_offset, end_offset - start_offset, u"");
        return js::Value::undefined();
    }
    // The contained nodes, less those whose parent goes with them.
    std::vector<dom::Node*> descendants;
    collect_descendants(*common_ancestor(range), descendants);
    std::vector<dom::Node*> to_remove;
    for (dom::Node* node : descendants) {
        if (is_contained(*node, range) && !(node->parent() && is_contained(*node->parent(), range)))
            to_remove.push_back(node);
    }
    auto const [new_node, new_offset] = boundary_after_removal(range);
    if (is_character_data(*start))
        replace_data(in, *start, start_offset, node_length(*start) - start_offset, u"");
    for (dom::Node* node : to_remove)
        remove_node(in, *node);
    if (is_character_data(*end))
        replace_data(in, *end, 0, end_offset, u"");
    dom::set_range(range, new_node, new_offset, new_node, new_offset);
    return js::Value::undefined();
}

// DOM §5.5 "insert".
Native insert_node(Realm::Internals& in, dom::Range& range, dom::Node& node)
{
    dom::Node& start = *range.start_node;
    if (start.type() == dom::NodeType::ProcessingInstruction || start.type() == dom::NodeType::Comment || (start.is_text() && !start.parent())
        || &start == &node)
        return in.throw_dom_exception("HierarchyRequestError", "Nodes may not be inserted at the range's start.");
    dom::Node* reference = nullptr;
    if (start.is_text())
        reference = &start;
    else if (range.start_offset < start.children().size())
        reference = start.children()[range.start_offset];
    dom::Node& parent = reference ? *reference->parent() : start;
    if (!ensure_pre_insertion_validity(in, parent, node, reference))
        return std::nullopt;
    if (start.is_text())
        reference = split_text(in, static_cast<dom::Text&>(start), range.start_offset);
    if (&node == reference)
        reference = next_sibling_of(node);
    if (node.parent())
        remove_node(in, node);
    std::uint32_t new_offset = reference ? reference->index() : node_length(parent);
    new_offset += node.type() == dom::NodeType::DocumentFragment ? node_length(node) : 1;
    if (!pre_insert(in, parent, node, reference))
        return std::nullopt;
    if (is_collapsed(range))
        dom::set_range(range, range.start_node, range.start_offset, &parent, new_offset);
    return js::Value::undefined();
}

// DOM §5.5 surroundContents().
Native surround_contents(Realm::Internals& in, dom::Range& range, dom::Node& new_parent)
{
    for (dom::Node* node = range.start_node; node; node = node->parent()) {
        if (!node->is_text() && !is_inclusive_ancestor(*node, *range.end_node))
            return in.throw_dom_exception("InvalidStateError", "The range partially contains a node that is not a Text node.");
    }
    for (dom::Node* node = range.end_node; node; node = node->parent()) {
        if (!node->is_text() && !is_inclusive_ancestor(*node, *range.start_node))
            return in.throw_dom_exception("InvalidStateError", "The range partially contains a node that is not a Text node.");
    }
    dom::NodeType const type = new_parent.type();
    if (type == dom::NodeType::Document || type == dom::NodeType::DocumentType || type == dom::NodeType::DocumentFragment)
        return in.throw_dom_exception("InvalidNodeTypeError", "A document, doctype or fragment cannot surround a range.");
    std::optional<dom::Node*> const fragment = extract(in, range);
    if (!fragment)
        return std::nullopt;
    std::vector<dom::Node*> const children = new_parent.children();
    for (dom::Node* child : children)
        remove_node(in, *child);
    if (!insert_node(in, range, new_parent) || !append(in, new_parent, **fragment))
        return std::nullopt;
    return select_node(in, range, new_parent);
}

// The stringifier (DOM §5.5): the data of the Text in the range.
std::string range_text(dom::Range const& range)
{
    if (is_released(range))
        return "";
    dom::Node const& start = *range.start_node;
    dom::Node const& end = *range.end_node;
    if (&start == &end && start.is_text())
        return js::utf8_from_utf16(substring_data(start, range.start_offset, range.end_offset - range.start_offset));
    std::u16string text;
    if (start.is_text())
        text += substring_data(start, range.start_offset, node_length(start) - range.start_offset);
    std::vector<dom::Node*> descendants;
    collect_descendants(*common_ancestor(range), descendants);
    for (dom::Node const* node : descendants) {
        if (node->is_text() && is_contained(*node, range))
            text += data_units(*node);
    }
    if (end.is_text())
        text += substring_data(end, 0, range.end_offset);
    return js::utf8_from_utf16(text);
}

template<typename Read>
void range_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    define_getter(in, prototype, name, [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
        std::optional<RangeObject*> const range = this_range(interpreter, this_value, true);
        if (!range)
            return std::nullopt;
        return read(internals_of(interpreter), (*range)->range());
    });
}

// A Range method; one that reads the boundaries refuses a range whose
// document is gone.
template<typename Body>
void range_method(Realm::Internals& in, js::Object& prototype, std::string_view name, int length, bool needs_boundaries, Body body)
{
    define_operation(in.interpreter, prototype, name, length,
        [body, needs_boundaries](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<RangeObject*> const range = this_range(interpreter, this_value, false);
            if (!range)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interpreter);
            if (needs_boundaries && is_released((*range)->range()))
                return throw_released(internals);
            return body(internals, (*range)->range(), args);
        });
}

} // namespace

js::Value new_range(Realm::Internals& in, dom::Node& container)
{
    auto* range = in.interpreter.heap().allocate<RangeObject>(in.prototype("Range"), true);
    dom::set_range(range->range(), &container, 0, &container, 0);
    return js::Value::object(range);
}

void install_ranges(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());

    js::Object* abstract = define_interface(in, "AbstractRange", nullptr);
    range_getter(in, *abstract, "startContainer", [](Realm::Internals& internals, dom::Range& range) -> Native {
        return internals.realm.wrap_or_null(range.start_node);
    });
    range_getter(in, *abstract, "startOffset", [](Realm::Internals&, dom::Range& range) -> Native {
        return js::Value::number(range.start_offset);
    });
    range_getter(in, *abstract, "endContainer", [](Realm::Internals& internals, dom::Range& range) -> Native {
        return internals.realm.wrap_or_null(range.end_node);
    });
    range_getter(in, *abstract, "endOffset", [](Realm::Internals&, dom::Range& range) -> Native {
        return js::Value::number(range.end_offset);
    });
    range_getter(in, *abstract, "collapsed", [](Realm::Internals&, dom::Range& range) -> Native {
        return js::Value::boolean(is_collapsed(range));
    });

    // new StaticRange(init): the boundaries as given, only doctypes refused.
    define_interface(
        in, "StaticRange", abstract,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Value const init = js::argument(args, 0);
            if (!init.is_object())
                return interp.throw_type_error("Failed to construct 'StaticRange': parameter 1 is not of type 'StaticRangeInit'.");
            // The dictionary's members, every one required, in the order
            // WebIDL reads them.
            auto const container = [&](std::string_view member) -> std::optional<dom::Node*> {
                std::optional<js::Value> const value = interp.get(init, member);
                if (!value)
                    return std::nullopt;
                NodeWrapper* const wrapper = internals.wrapper_of(*value);
                if (!wrapper) {
                    interp.throw_type_error("Failed to construct 'StaticRange': member " + std::string(member) + " is not of type 'Node'.");
                    return std::nullopt;
                }
                return &wrapper->node();
            };
            auto const offset = [&](std::string_view member) -> std::optional<std::uint32_t> {
                std::optional<js::Value> const value = interp.get(init, member);
                if (!value)
                    return std::nullopt;
                if (value->is_undefined()) {
                    interp.throw_type_error("Failed to construct 'StaticRange': required member " + std::string(member) + " is undefined.");
                    return std::nullopt;
                }
                std::optional<double> const number = interp.to_number(*value);
                if (!number)
                    return std::nullopt;
                return to_unsigned_long(*number);
            };
            std::optional<dom::Node*> const end_container = container("endContainer");
            if (!end_container)
                return std::nullopt;
            std::optional<std::uint32_t> const end_offset = offset("endOffset");
            if (!end_offset)
                return std::nullopt;
            std::optional<dom::Node*> const start_container = container("startContainer");
            if (!start_container)
                return std::nullopt;
            std::optional<std::uint32_t> const start_offset = offset("startOffset");
            if (!start_offset)
                return std::nullopt;
            if ((*start_container)->type() == dom::NodeType::DocumentType || (*end_container)->type() == dom::NodeType::DocumentType)
                return internals.throw_dom_exception("InvalidNodeTypeError", "A static range's boundary cannot be a doctype.");
            auto* range = interp.heap().allocate<RangeObject>(internals.prototype("StaticRange"), false);
            dom::set_range(range->range(), *start_container, *start_offset, *end_container, *end_offset);
            return js::Value::object(range);
        },
        1);

    js::Object* range = define_interface(in, "Range", abstract, [](js::Interpreter& interp, Args, js::Object*) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return new_range(internals, *internals.document);
    });
    for (auto const& [name, value] : { std::pair { "START_TO_START", 0 }, std::pair { "START_TO_END", 1 }, std::pair { "END_TO_END", 2 },
             std::pair { "END_TO_START", 3 } }) {
        range->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        std::optional<js::Value> const constructor = range->get(interpreter, interpreter.key("constructor"), js::Value::object(range));
        if (constructor && constructor->is_object())
            constructor->as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }

    define_getter(in, *range, "commonAncestorContainer", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<RangeObject*> const r = this_range(interp, this_value, false);
        if (!r)
            return std::nullopt;
        dom::Range const& boundaries = (*r)->range();
        return internals_of(interp).realm.wrap_or_null(is_released(boundaries) ? nullptr : common_ancestor(boundaries));
    });
    for (bool const start : { true, false }) {
        range_method(in, *range, start ? "setStart" : "setEnd", 2, false, [start](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
            auto const point = point_arguments(internals, args, start ? "setStart" : "setEnd");
            if (!point)
                return std::nullopt;
            return set_boundary(internals, r, *point->first, point->second, start);
        });
    }
    struct Adjacent {
        char const* name;
        bool start;
        bool after;
    };
    for (Adjacent const adjacent : { Adjacent { "setStartBefore", true, false }, Adjacent { "setStartAfter", true, true },
             Adjacent { "setEndBefore", false, false }, Adjacent { "setEndAfter", false, true } }) {
        range_method(in, *range, adjacent.name, 1, false, [adjacent](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
            std::optional<dom::Node*> const node = node_argument(internals, args, 0, adjacent.name);
            if (!node)
                return std::nullopt;
            dom::Node* const parent = (*node)->parent();
            if (!parent)
                return internals.throw_dom_exception("InvalidNodeTypeError", "The node provided has no parent.");
            return set_boundary(internals, r, *parent, (*node)->index() + (adjacent.after ? 1 : 0), adjacent.start);
        });
    }
    range_method(in, *range, "collapse", 0, false, [](Realm::Internals&, dom::Range& r, Args args) -> Native {
        if (js::Interpreter::to_boolean(js::argument(args, 0)))
            dom::set_range(r, r.start_node, r.start_offset, r.start_node, r.start_offset);
        else
            dom::set_range(r, r.end_node, r.end_offset, r.end_node, r.end_offset);
        return js::Value::undefined();
    });
    range_method(in, *range, "selectNode", 1, false, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "selectNode");
        if (!node)
            return std::nullopt;
        return select_node(internals, r, **node);
    });
    range_method(in, *range, "selectNodeContents", 1, false, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "selectNodeContents");
        if (!node)
            return std::nullopt;
        if ((*node)->type() == dom::NodeType::DocumentType)
            return internals.throw_dom_exception("InvalidNodeTypeError", "The node provided is a doctype.");
        dom::set_range(r, *node, 0, *node, node_length(**node));
        return js::Value::undefined();
    });
    range_method(in, *range, "compareBoundaryPoints", 2, true, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        if (args.size() < 2)
            return internals.interpreter.throw_type_error("Failed to execute 'compareBoundaryPoints' on 'Range': 2 arguments required.");
        std::optional<double> const how_number = internals.interpreter.to_number(args[0]);
        if (!how_number)
            return std::nullopt;
        auto* const source = args[1].is_object() ? dynamic_cast<RangeObject*>(args[1].as_object()) : nullptr;
        if (!source || !source->live())
            return internals.interpreter.throw_type_error("Failed to execute 'compareBoundaryPoints' on 'Range': parameter 2 is not of type 'Range'.");
        std::uint32_t const how = to_unsigned_long(*how_number) & 0xFFFF; // unsigned short
        if (how > 3)
            return internals.throw_dom_exception("NotSupportedError", "The comparison method provided is not one of the four Range constants.");
        dom::Range const& other = source->range();
        if (is_released(other) || root_of(r) != root_of(other))
            return internals.throw_dom_exception("WrongDocumentError", "The two ranges are not in the same tree.");
        bool const this_start = how == 0 || how == 3; // START_TO_START, END_TO_START
        bool const other_start = how == 0 || how == 1; // START_TO_START, START_TO_END
        Position const position = position_of(this_start ? *r.start_node : *r.end_node, this_start ? r.start_offset : r.end_offset,
            other_start ? *other.start_node : *other.end_node, other_start ? other.start_offset : other.end_offset);
        return js::Value::number(position == Position::Before ? -1 : position == Position::Equal ? 0 : 1);
    });
    range_method(in, *range, "deleteContents", 0, true, [](Realm::Internals& internals, dom::Range& r, Args) -> Native {
        return delete_contents(internals, r);
    });
    range_method(in, *range, "extractContents", 0, true, [](Realm::Internals& internals, dom::Range& r, Args) -> Native {
        std::optional<dom::Node*> const fragment = extract(internals, r);
        if (!fragment)
            return std::nullopt;
        return js::Value::object(internals.wrap(**fragment));
    });
    range_method(in, *range, "cloneContents", 0, true, [](Realm::Internals& internals, dom::Range& r, Args) -> Native {
        std::optional<dom::Node*> const fragment = clone_contents(internals, r);
        if (!fragment)
            return std::nullopt;
        return js::Value::object(internals.wrap(**fragment));
    });
    range_method(in, *range, "insertNode", 1, true, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "insertNode");
        if (!node)
            return std::nullopt;
        return insert_node(internals, r, **node);
    });
    range_method(in, *range, "surroundContents", 1, true, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "surroundContents");
        if (!node)
            return std::nullopt;
        return surround_contents(internals, r, **node);
    });
    range_method(in, *range, "cloneRange", 0, false, [](Realm::Internals& internals, dom::Range& r, Args) -> Native {
        auto* clone = internals.interpreter.heap().allocate<RangeObject>(internals.prototype("Range"), true);
        dom::set_range(clone->range(), r.start_node, r.start_offset, r.end_node, r.end_offset);
        return js::Value::object(clone);
    });
    range_method(in, *range, "detach", 0, false, [](Realm::Internals&, dom::Range&, Args) -> Native { return js::Value::undefined(); });
    range_method(in, *range, "isPointInRange", 2, false, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        auto const point = point_arguments(internals, args, "isPointInRange");
        if (!point)
            return std::nullopt;
        auto const [node, offset] = *point;
        if (is_released(r) || &node->root() != root_of(r))
            return js::Value::boolean(false);
        if (node->type() == dom::NodeType::DocumentType)
            return internals.throw_dom_exception("InvalidNodeTypeError", "The node provided is a doctype.");
        if (offset > node_length(*node))
            return internals.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length.");
        return js::Value::boolean(position_of(*node, offset, *r.start_node, r.start_offset) != Position::Before
            && position_of(*node, offset, *r.end_node, r.end_offset) != Position::After);
    });
    range_method(in, *range, "comparePoint", 2, true, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        auto const point = point_arguments(internals, args, "comparePoint");
        if (!point)
            return std::nullopt;
        auto const [node, offset] = *point;
        if (&node->root() != root_of(r))
            return internals.throw_dom_exception("WrongDocumentError", "The node provided is not in the range's tree.");
        if (node->type() == dom::NodeType::DocumentType)
            return internals.throw_dom_exception("InvalidNodeTypeError", "The node provided is a doctype.");
        if (offset > node_length(*node))
            return internals.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length.");
        if (position_of(*node, offset, *r.start_node, r.start_offset) == Position::Before)
            return js::Value::number(-1);
        if (position_of(*node, offset, *r.end_node, r.end_offset) == Position::After)
            return js::Value::number(1);
        return js::Value::number(0);
    });
    range_method(in, *range, "intersectsNode", 1, false, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        std::optional<dom::Node*> const node = node_argument(internals, args, 0, "intersectsNode");
        if (!node)
            return std::nullopt;
        if (is_released(r) || &(*node)->root() != root_of(r))
            return js::Value::boolean(false);
        dom::Node* const parent = (*node)->parent();
        if (!parent)
            return js::Value::boolean(true);
        std::uint32_t const offset = (*node)->index();
        return js::Value::boolean(position_of(*parent, offset, *r.end_node, r.end_offset) == Position::Before
            && position_of(*parent, offset + 1, *r.start_node, r.start_offset) == Position::After);
    });
    range_method(in, *range, "toString", 0, false, [](Realm::Internals& internals, dom::Range& r, Args) -> Native {
        return internals.string(range_text(r));
    });
    // createContextualFragment (DOM Parsing §7): the markup parsed in the
    // start's element, a body for none or for html; its scripts are not
    // started, so they run once inserted.
    range_method(in, *range, "createContextualFragment", 1, true, [](Realm::Internals& internals, dom::Range& r, Args args) -> Native {
        std::optional<std::string> const markup = internals.to_utf8(js::argument(args, 0));
        if (!markup)
            return std::nullopt;
        dom::Node& node = *r.start_node;
        dom::Element* element = nullptr;
        if (node.is_element())
            element = static_cast<dom::Element*>(&node);
        else if ((node.is_text() || node.type() == dom::NodeType::Comment) && node.parent() && node.parent()->is_element())
            element = static_cast<dom::Element*>(node.parent());
        if (!element || (!node.document().xml && element->is_html("html")))
            element = node.document().create<dom::Element>(std::string(dom::ns::html), "body");
        dom::Node* const fragment = node.document().create<dom::DocumentFragment>();
        for (dom::Node* child : parse_markup(internals, *element, *markup, false))
            fragment->append_child(*child);
        return js::Value::object(internals.wrap(*fragment));
    });
}

}
