#include "bindings/NodeSupport.h"

// Editing rich text (HTML §6.8 "Editing", Input Events Level 2, and the
// execCommand draft): the editing host a selection is in, the commands that
// change it — text typed or pasted, a character or a word deleted either
// way, a paragraph split, a line broken, the selection replaced — and the
// order a reader's edit is told to the page in: `beforeinput` at the host,
// which may cancel it, the change, then `input`. document.execCommand runs
// the same commands for a script, with `input` alone, as browsers do.
//
// The commands keep to what browsers make of a typed edit, so that an
// editor that reads the DOM back finds what it would find in them: a new
// paragraph is the block it was split from, or a <div> (the default
// paragraph separator); an empty block holds a <br> so it keeps a line; a
// typed space next to another or at a line's edge is a no-break space, made
// an ordinary one again when a letter follows.

#include "core/Unicode.h"
#include "dom/Dom.h"
#include "dom/Forms.h"
#include "html/Serializer.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

namespace {

struct Point {
    dom::Node* node = nullptr;
    std::uint32_t offset = 0;
};

constexpr char16_t no_break_space = 0x00A0;

dom::Element* parent_element_of(dom::Node const& node)
{
    dom::Node* const parent = node.parent();
    return parent && parent->is_element() ? static_cast<dom::Element*>(parent) : nullptr;
}

dom::Element* element_of(dom::Node& node)
{
    return node.is_element() ? static_cast<dom::Element*>(&node) : parent_element_of(node);
}

// The elements a paragraph is (the HTML elements whose default display is a
// block or a list item): what Enter splits and Backspace at a start joins.
bool is_block(dom::Node const& node)
{
    if (!node.is_element())
        return false;
    auto const& element = static_cast<dom::Element const&>(node);
    if (!element.is_html())
        return false;
    static constexpr std::string_view blocks[] = { "address", "article", "aside", "blockquote", "body", "center", "dd", "details",
        "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
        "header", "hgroup", "hr", "li", "listing", "main", "menu", "nav", "ol", "p", "pre", "section", "summary", "table", "tbody",
        "td", "tfoot", "th", "thead", "tr", "ul", "xmp", "html" };
    return std::find(std::begin(blocks), std::end(blocks), element.local_name()) != std::end(blocks);
}

// A node with no text of its own that still takes room on a line: a
// picture, a field, a rule, a line break.
bool is_atomic(dom::Node const& node)
{
    if (!node.is_element())
        return false;
    auto const& element = static_cast<dom::Element const&>(node);
    static constexpr std::string_view atoms[] = { "br", "img", "input", "textarea", "select", "button", "video", "audio", "canvas",
        "iframe", "embed", "object", "hr", "svg", "math", "meter", "progress" };
    return std::find(std::begin(atoms), std::end(atoms), element.local_name()) != std::end(atoms);
}

bool is_br(dom::Node const& node)
{
    return node.is_element() && static_cast<dom::Element const&>(node).is_html("br");
}

// Whether a node may be changed by an edit: in designMode every node of
// the document, otherwise one inside an element contenteditable makes so.
bool is_editable(Realm::Internals& in, dom::Node& node)
{
    if (&node.document() != in.document)
        return false;
    if (in.design_mode)
        return true;
    dom::Element const* const element = element_of(node);
    if (!element)
        return false;
    // The host itself is not editable as a node (its parent is not), but
    // what it holds is.
    return dom::forms::editing_host_editable(*element);
}

// The editing host of a node: the outermost editable element around it, or
// the body in designMode; null for a node nothing makes editable.
dom::Element* editing_host_of(Realm::Internals& in, dom::Node& node)
{
    if (!is_editable(in, node))
        return nullptr;
    if (in.design_mode) {
        dom::Element* const body = body_element(*in.document);
        return body ? body : document_element(*in.document);
    }
    dom::Element* host = element_of(node);
    while (dom::Element* const parent = parent_element_of(*host)) {
        if (!dom::forms::editing_host_editable(*parent))
            break;
        host = parent;
    }
    return host;
}

bool plaintext_only(dom::Element const& host)
{
    dom::Attr const* const attribute = host.find_attribute("contenteditable");
    if (!attribute)
        return false;
    std::string value = attribute->value;
    for (char& c : value)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return value == "plaintext-only";
}

// The block a point is in, inside the host: the host itself when no block
// stands between.
dom::Element* block_of(dom::Node& node, dom::Element& host)
{
    for (dom::Node* at = &node; at && at != &host; at = at->parent()) {
        if (is_block(*at))
            return static_cast<dom::Element*>(at);
    }
    return &host;
}

// The leaves of a block in tree order: its text with data and the atomic
// nodes, the descendants of the blocks inside it left out — they are their
// own paragraphs — and so are those of anything not editable.
void collect_leaves(dom::Node& node, std::vector<dom::Node*>& out, dom::Element const& block)
{
    for (dom::Node* child : node.children()) {
        if (child->is_text()) {
            if (!character_data(*child).empty())
                out.push_back(child);
            continue;
        }
        if (!child->is_element())
            continue;
        if (child != &block && is_block(*child))
            continue;
        if (is_atomic(*child) || !dom::forms::editing_host_editable(static_cast<dom::Element&>(*child))) {
            out.push_back(child);
            continue;
        }
        collect_leaves(*child, out, block);
    }
}

// What a block shows: true when it holds text or an atomic node other than
// a line break at its end.
bool block_has_content(dom::Element& block)
{
    std::vector<dom::Node*> leaves;
    collect_leaves(block, leaves, block);
    for (dom::Node* leaf : leaves) {
        if (!is_br(*leaf))
            return true;
    }
    return false;
}

dom::Element* make_element(Realm::Internals& in, std::string_view name)
{
    return in.document->create<dom::Element>(std::string(dom::ns::html), std::string(name));
}

// A block left with nothing on it keeps its line with a <br>, as one an
// edit emptied does in every browser.
void keep_line(Realm::Internals& in, dom::Element& block)
{
    std::vector<dom::Node*> leaves;
    collect_leaves(block, leaves, block);
    if (!leaves.empty())
        return;
    pre_insert(in, block, *make_element(in, "br"), nullptr);
}

std::u16string units_of(dom::Node const& node)
{
    return data_units(node);
}

std::uint32_t units_length(dom::Node const& node)
{
    return static_cast<std::uint32_t>(data_units(node).size());
}

// A point where text can be typed: in a Text node, made when the place is
// between nodes — the end of the text before, the start of the text after,
// or a new empty node.
Point text_point(Realm::Internals& in, Point point)
{
    if (point.node->is_text())
        return point;
    std::vector<dom::Node*> const& children = point.node->children();
    if (point.offset > 0 && point.offset <= children.size() && children[point.offset - 1]->is_text())
        return { children[point.offset - 1], units_length(*children[point.offset - 1]) };
    if (point.offset < children.size() && children[point.offset]->is_text())
        return { children[point.offset], 0 };
    dom::Text* const text = in.document->create<dom::Text>();
    dom::Node* const before = point.offset < children.size() ? children[point.offset] : nullptr;
    pre_insert(in, *point.node, *text, before);
    return { text, 0 };
}

// The selection's range, its anchor and focus.
struct Current {
    dom::Range* range = nullptr;
    bool backward = false;
    dom::Element* host = nullptr;
};

std::optional<Current> current_selection(Realm::Internals& in)
{
    Current current;
    if (!selection_range(in, current.range, current.backward))
        return std::nullopt;
    dom::Node* const focus = current.backward ? current.range->start_node : current.range->end_node;
    current.host = editing_host_of(in, *focus);
    if (!current.host)
        return std::nullopt;
    // Both ends must be in the one host.
    dom::Node* const other = current.backward ? current.range->end_node : current.range->start_node;
    if (editing_host_of(in, *other) != current.host)
        return std::nullopt;
    return current;
}

void place_caret(Realm::Internals& in, Point point)
{
    in.realm.set_selection(*point.node, point.offset, *point.node, point.offset);
}

bool collapsed(dom::Range const& range)
{
    return range.start_node == range.end_node && range.start_offset == range.end_offset;
}

// Two blocks made one: what follows `second` is moved to the end of
// `first`, which loses a <br> it ended with, and `second` is gone, with any
// ancestor it leaves empty inside the host. The caret goes to the join.
Point join_blocks(Realm::Internals& in, dom::Element& first, dom::Element& second, dom::Element& host)
{
    // A <br> keeping an empty block's line, or ending one, is not wanted
    // once more follows.
    std::vector<dom::Node*> first_leaves;
    collect_leaves(first, first_leaves, first);
    if (!first_leaves.empty() && is_br(*first_leaves.back()))
        remove_node(in, *first_leaves.back());
    std::vector<dom::Node*> second_leaves;
    collect_leaves(second, second_leaves, second);
    if (second_leaves.size() == 1 && is_br(*second_leaves.front()))
        remove_node(in, *second_leaves.front());
    Point join { &first, static_cast<std::uint32_t>(first.children().size()) };
    if (is_inclusive_ancestor(first, second)) {
        // A block inside the run of text it follows (`text<div>more</div>`
        // in the host): its children take its place.
        join = { second.parent(), second.index() };
        std::vector<dom::Node*> const children = second.children();
        for (dom::Node* child : children)
            pre_insert(in, *second.parent(), *child, &second);
    } else if (is_inclusive_ancestor(second, first)) {
        // The other way round (`<div>text</div>more`): what follows is moved in.
        dom::Node* after = next_sibling_of(first);
        while (after && !is_block(*after)) {
            dom::Node* const next = next_sibling_of(*after);
            pre_insert(in, first, *after, nullptr);
            after = next;
        }
        keep_line(in, first);
        return join;
    } else {
        std::vector<dom::Node*> const children = second.children();
        for (dom::Node* child : children)
            pre_insert(in, first, *child, nullptr);
    }
    dom::Node* gone = &second;
    while (gone->parent() && gone->parent() != &host && gone->parent()->children().size() == 1 && !is_inclusive_ancestor(*gone->parent(), first))
        gone = gone->parent();
    if (gone->parent() && gone != &host && !is_inclusive_ancestor(*gone, first))
        remove_node(in, *gone);
    keep_line(in, first);
    return join;
}

// The block before or after one, in the host: the block holding the leaf
// next to its edge.
dom::Element* neighbor_block(dom::Element& block, dom::Element& host, bool before)
{
    std::vector<dom::Node*> all;
    collect_descendants(host, all);
    std::uint32_t const length = boundary_length(block);
    dom::Node* found = nullptr;
    for (dom::Node* node : all) {
        if (is_inclusive_ancestor(block, *node))
            continue;
        bool const leaf = (node->is_text() && !character_data(*node).empty()) || is_atomic(*node) || (is_block(*node) && node->children().empty());
        if (!leaf)
            continue;
        if (before) {
            if (compare_points(*node, 0, block, 0) < 0)
                found = node;
        } else if (compare_points(*node, 0, block, length) > 0) {
            found = node;
            break;
        }
    }
    if (!found)
        return nullptr;
    return block_of(*found, host);
}

// Deletes what the range holds, and joins the blocks it ran across: the
// caret is where it began.
Point delete_range(Realm::Internals& in, dom::Range& range, dom::Element& host)
{
    dom::Element* const start_block = block_of(*range.start_node, host);
    dom::Element* const end_block = block_of(*range.end_node, host);
    delete_range_contents(in, range);
    Point caret { range.start_node, range.start_offset };
    if (start_block != end_block && end_block->parent() && is_inclusive_ancestor(host, *end_block)) {
        caret = join_blocks(in, *start_block, *end_block, host);
        // The join point moved with the start: the range's start is kept
        // live through the moves.
        caret = { range.start_node, range.start_offset };
    }
    if (dom::Element* const block = block_of(*caret.node, host); block->parent() || block == &host)
        keep_line(in, *block);
    return caret;
}

// --- Typing ---------------------------------------------------------------------

bool is_space_unit(char16_t c)
{
    return c == u' ' || c == no_break_space || c == u'\t' || c == u'\n';
}

// The character beside a Text node in its block, past its siblings of
// text (and empty inline elements' edges): a newline at the block's edge or
// at a line break, which is what a space next to it collapses against.
char16_t neighbor_unit(dom::Node& text, bool after)
{
    dom::Node* at = &text;
    while (true) {
        dom::Node* const sibling = after ? next_sibling_of(*at) : (at->index() > 0 ? at->parent()->children()[at->index() - 1] : nullptr);
        if (!sibling) {
            dom::Node* const parent = at->parent();
            if (!parent || is_block(*parent))
                return u'\n';
            at = parent;
            continue;
        }
        if (sibling->is_text()) {
            std::u16string const units = data_units(*sibling);
            if (units.empty()) {
                at = sibling;
                continue;
            }
            return after ? units.front() : units.back();
        }
        if (is_block(*sibling) || is_br(*sibling))
            return u'\n';
        if (is_atomic(*sibling))
            return u'x';
        // An inline element: its text nearest this side.
        std::vector<dom::Node*> inside;
        collect_descendants(*sibling, inside);
        if (!after)
            std::reverse(inside.begin(), inside.end());
        for (dom::Node* node : inside) {
            if (node->is_text() && !character_data(*node).empty()) {
                std::u16string const units = data_units(*node);
                return after ? units.front() : units.back();
            }
        }
        at = sibling;
    }
}

// Text typed at a point: a space where it would collapse away is a no-break
// space, and a no-break space typed before stops being one once a letter
// follows it.
Point insert_text(Realm::Internals& in, Point at, std::u16string_view text, bool preserve_spaces)
{
    dom::Element* const block = block_of(*at.node, *editing_host_of(in, *at.node));
    std::vector<dom::Node*> leaves;
    collect_leaves(*block, leaves, *block);
    dom::Node* const placeholder = leaves.size() == 1 && is_br(*leaves.front()) ? leaves.front() : nullptr;
    Point point = text_point(in, at);
    std::u16string const before = units_of(*point.node);
    std::u16string typed(text);
    if (!preserve_spaces) {
        for (std::size_t i = 0; i < typed.size(); ++i) {
            if (typed[i] != u' ')
                continue;
            char16_t const previous = i > 0 ? typed[i - 1] : point.offset > 0 ? before[point.offset - 1] : neighbor_unit(*point.node, false);
            char16_t const next = i + 1 < typed.size() ? typed[i + 1] : point.offset < before.size() ? before[point.offset] : neighbor_unit(*point.node, true);
            if (is_space_unit(previous) || next == u'\n' || is_space_unit(next))
                typed[i] = no_break_space;
        }
        // "a&nbsp;" + "b": the space before is an ordinary one now.
        if (!typed.empty() && !is_space_unit(typed.front()) && point.offset >= 2 && before[point.offset - 1] == no_break_space
            && !is_space_unit(before[point.offset - 2]))
            replace_data(in, *point.node, point.offset - 1, 1, u" ");
    }
    replace_data(in, *point.node, point.offset, 0, typed);
    Point const after { point.node, point.offset + static_cast<std::uint32_t>(typed.size()) };
    if (placeholder && placeholder->parent())
        remove_node(in, *placeholder);
    return after;
}

// --- Deleting -------------------------------------------------------------------

// The extent of one character before or after a point in a Text node, in
// code units: a surrogate pair together, and a base with the marks that
// combine with it.
std::uint32_t character_extent(std::u16string const& units, std::uint32_t offset, bool forward)
{
    auto const code_point_at = [&](std::uint32_t at, bool after) -> std::pair<char32_t, std::uint32_t> {
        if (after) {
            char16_t const unit = units[at];
            if (unit >= 0xD800 && unit <= 0xDBFF && at + 1 < units.size())
                return { 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) + (units[at + 1] - 0xDC00u), 2 };
            return { unit, 1 };
        }
        char16_t const unit = units[at - 1];
        if (unit >= 0xDC00 && unit <= 0xDFFF && at >= 2)
            return { 0x10000 + ((static_cast<char32_t>(units[at - 2]) - 0xD800) << 10) + (unit - 0xDC00u), 2 };
        return { unit, 1 };
    };
    std::uint32_t extent = 0;
    if (forward) {
        auto [c, size] = code_point_at(offset, true);
        extent = size;
        while (offset + extent < units.size()) {
            auto const [next, next_size] = code_point_at(offset + extent, true);
            if (!is_combining_mark(next) && next != 0x200D && !(next >= 0xFE00 && next <= 0xFE0F))
                break;
            extent += next_size;
        }
        (void)c;
    } else {
        while (offset - extent > 0) {
            auto const [c, size] = code_point_at(offset - extent, false);
            extent += size;
            if (!is_combining_mark(c) && c != 0x200D && !(c >= 0xFE00 && c <= 0xFE0F))
                break;
        }
    }
    return extent;
}

bool is_word_unit(char16_t unit)
{
    return (unit >= u'0' && unit <= u'9') || (unit >= u'A' && unit <= u'Z') || (unit >= u'a' && unit <= u'z') || unit == u'_'
        || unit == u'\'' || (unit >= 0x00C0 && unit != 0x00D7 && unit != 0x00F7 && !(unit >= 0x2000 && unit <= 0x206F) && unit != 0x3000);
}

// Deletes a character or a word before (or after) a collapsed point in its
// block; at the block's edge the block is joined with the one before (or
// after) it. Returns where the caret goes.
Point delete_from(Realm::Internals& in, Point at, dom::Element& host, bool forward, bool word)
{
    dom::Element* const block = block_of(*at.node, host);
    std::vector<dom::Node*> leaves;
    collect_leaves(*block, leaves, *block);
    // The leaf next to the point, in the direction deleted.
    dom::Node* leaf = nullptr;
    std::uint32_t leaf_offset = 0; // inside a Text leaf: where the point is
    if (forward) {
        for (dom::Node* candidate : leaves) {
            if (candidate->is_text()) {
                std::uint32_t const length = units_length(*candidate);
                if (compare_points(*candidate, length, *at.node, at.offset) <= 0)
                    continue;
                leaf = candidate;
                leaf_offset = candidate == at.node ? at.offset : 0;
                if (leaf_offset == length) {
                    leaf = nullptr;
                    continue;
                }
                break;
            }
            if (compare_points(*candidate->parent(), candidate->index() + 1, *at.node, at.offset) <= 0)
                continue;
            leaf = candidate;
            break;
        }
    } else {
        for (auto it = leaves.rbegin(); it != leaves.rend(); ++it) {
            dom::Node* const candidate = *it;
            if (candidate->is_text()) {
                if (compare_points(*candidate, 0, *at.node, at.offset) >= 0)
                    continue;
                leaf = candidate;
                leaf_offset = candidate == at.node ? at.offset : units_length(*candidate);
                if (leaf_offset == 0) {
                    leaf = nullptr;
                    continue;
                }
                break;
            }
            if (compare_points(*candidate->parent(), candidate->index(), *at.node, at.offset) >= 0)
                continue;
            leaf = candidate;
            break;
        }
    }
    // A <br> at a block's very end only ends it: nothing to delete forward.
    if (forward && leaf && is_br(*leaf) && leaf == leaves.back() && leaves.size() > 1)
        leaf = nullptr;
    if (!leaf) {
        dom::Element* const other = neighbor_block(*block, host, !forward);
        if (!other || !is_editable(in, *other))
            return at;
        if (forward)
            return join_blocks(in, *block, *other, host);
        // Backward: this block joins the end of the one before.
        Point const join = join_blocks(in, *other, *block, host);
        return join;
    }
    if (!leaf->is_text()) {
        Point const caret { leaf->parent(), leaf->index() };
        remove_node(in, *leaf);
        keep_line(in, *block);
        return caret;
    }
    std::u16string const units = units_of(*leaf);
    std::uint32_t extent = character_extent(units, leaf_offset, forward);
    if (word) {
        // A word: the spaces next to the point, then the word's letters.
        extent = 0;
        auto const unit = [&](std::uint32_t k) { return forward ? units[leaf_offset + k] : units[leaf_offset - 1 - k]; };
        std::uint32_t const room = forward ? static_cast<std::uint32_t>(units.size()) - leaf_offset : leaf_offset;
        while (extent < room && !is_word_unit(unit(extent)))
            ++extent;
        while (extent < room && is_word_unit(unit(extent)))
            ++extent;
        extent = std::max<std::uint32_t>(extent, 1);
    }
    std::uint32_t const from = forward ? leaf_offset : leaf_offset - extent;
    replace_data(in, *leaf, from, extent, u"");
    Point caret { leaf, from };
    // A space left at the edge of the text would collapse away: it is a
    // no-break one.
    std::u16string const left = units_of(*leaf);
    if (from > 0 && left[from - 1] == u' ' && (from == left.size() || is_space_unit(left[from])))
        replace_data(in, *leaf, from - 1, 1, std::u16string(1, no_break_space));
    if (left.empty()) {
        caret = { leaf->parent(), leaf->index() };
        remove_node(in, *leaf);
    }
    keep_line(in, *block);
    return caret;
}

// --- Paragraphs and line breaks -------------------------------------------------

bool is_heading(dom::Element const& element)
{
    std::string const& name = element.local_name();
    return element.is_html() && name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

// Enter: the block split at the caret. The part after it is a block of the
// same kind — a <div>, the default paragraph separator, where it would be
// a heading's empty tail or where the text sat in the host with no block
// of its own — and the caret goes to its start.
Point insert_paragraph(Realm::Internals& in, Point at, dom::Element& host)
{
    dom::Element* block = block_of(*at.node, host);
    // An empty list item ends the list: Enter in it leaves the list for a
    // paragraph after it.
    if (block->is_html("li") && !block_has_content(*block) && block->parent() && block->parent()->is_element() && block->parent() != &host
        && is_inclusive_ancestor(host, *block->parent()) && block->parent() != host.parent()) {
        dom::Node* const list = block->parent();
        if (block == list->children().back() && list->parent()) {
            remove_node(in, *block);
            dom::Element* const paragraph = make_element(in, "div");
            pre_insert(in, *list->parent(), *paragraph, next_sibling_of(*list));
            keep_line(in, *paragraph);
            if (list->children().empty())
                remove_node(in, *list);
            return { paragraph, 0 };
        }
    }
    if (block == &host) {
        // Text straight in the host: the run of inline nodes the caret is in
        // is cut, and what follows the caret goes into a <div> of its own.
        // The run ends at the host's next block child.
        std::uint32_t run_end = 0;
        if (at.node == &host) {
            run_end = at.offset;
        } else {
            dom::Node* top = at.node;
            while (top->parent() != &host)
                top = top->parent();
            run_end = top->index() + 1;
        }
        while (run_end < host.children().size() && !is_block(*host.children()[run_end]))
            ++run_end;
        js::Interpreter::Roots const roots(in.interpreter);
        js::Heap::NoCollect const no_collect(in.interpreter.heap());
        js::Object* const range_object = make_live_range(in, *at.node, at.offset, host, run_end);
        dom::Range* const tail = live_range_of(js::Value::object(range_object));
        std::optional<dom::Node*> const fragment = extract_range_contents(in, *tail);
        dom::Element* const paragraph = make_element(in, "div");
        if (fragment)
            pre_insert(in, *paragraph, **fragment, nullptr);
        // Where the run was cut: the extracted range collapsed there.
        dom::Node* after_run = nullptr;
        if (tail->start_node == &host) {
            after_run = tail->start_offset < host.children().size() ? host.children()[tail->start_offset] : nullptr;
        } else {
            dom::Node* top = tail->start_node;
            while (top->parent() && top->parent() != &host)
                top = top->parent();
            after_run = top->parent() == &host ? next_sibling_of(*top) : nullptr;
        }
        pre_insert(in, host, *paragraph, after_run);
        // The text before the caret, empty now, still takes its line.
        bool before_has_content = false;
        for (dom::Node* child : host.children()) {
            if (child == paragraph)
                break;
            if ((child->is_text() && !character_data(*child).empty()) || child->is_element())
                before_has_content = true;
        }
        if (!before_has_content)
            pre_insert(in, host, *make_element(in, "br"), paragraph);
        keep_line(in, *paragraph);
        return { paragraph, 0 };
    }
    // A block: the part from the caret to its end, extracted (the inline
    // elements around the caret cut in two), goes into a copy of it.
    js::Interpreter::Roots const roots(in.interpreter);
    js::Heap::NoCollect const no_collect(in.interpreter.heap());
    js::Object* const range_object = make_live_range(in, *at.node, at.offset, *block, static_cast<std::uint32_t>(block->children().size()));
    dom::Range* const tail = live_range_of(js::Value::object(range_object));
    std::optional<dom::Node*> const fragment = extract_range_contents(in, *tail);
    bool const tail_empty = !fragment || [&] {
        std::vector<dom::Node*> parts;
        collect_descendants(**fragment, parts);
        return std::none_of(parts.begin(), parts.end(), [](dom::Node* node) {
            return (node->is_text() && !character_data(*node).empty()) || (is_atomic(*node) && !is_br(*node));
        });
    }();
    dom::Element* next = nullptr;
    if (is_heading(*block) && tail_empty) {
        next = make_element(in, "div");
    } else {
        next = in.document->create<dom::Element>(block->namespace_uri(), block->local_name());
        // The copy keeps the block's attributes but its id, which one element alone may have.
        for (dom::Attr const& attribute : block->attributes()) {
            if (attribute.local_name != "id")
                set_attribute(in, *next, attribute.local_name, attribute.value);
        }
    }
    if (fragment && !tail_empty)
        pre_insert(in, *next, **fragment, nullptr);
    pre_insert(in, *block->parent(), *next, next_sibling_of(*block));
    keep_line(in, *block);
    keep_line(in, *next);
    // The caret at the start of the new block's first text.
    dom::Node* first = next;
    while (!first->children().empty() && !first->children().front()->is_text() && !is_atomic(*first->children().front()))
        first = first->children().front();
    return { first, 0 };
}

// Shift+Enter, and Enter where only text may be typed: a <br> at the
// caret, and a second one when nothing follows it on its line, so that the
// new line shows.
Point insert_line_break(Realm::Internals& in, Point at, dom::Element& host)
{
    dom::Element* const block = block_of(*at.node, host);
    if (at.node->is_text()) {
        std::uint32_t const length = units_length(*at.node);
        if (at.offset == 0)
            at = { at.node->parent(), at.node->index() };
        else if (at.offset >= length)
            at = { at.node->parent(), at.node->index() + 1 };
        else {
            split_text(in, static_cast<dom::Text&>(*at.node), at.offset);
            at = { at.node->parent(), at.node->index() + 1 };
        }
    }
    dom::Node* const before = at.offset < at.node->children().size() ? at.node->children()[at.offset] : nullptr;
    dom::Element* const br = make_element(in, "br");
    pre_insert(in, *at.node, *br, before);
    Point const caret { br->parent(), br->index() + 1 };
    std::vector<dom::Node*> leaves;
    collect_leaves(*block, leaves, *block);
    if (!leaves.empty() && leaves.back() == br)
        pre_insert(in, *br->parent(), *make_element(in, "br"), next_sibling_of(*br));
    return caret;
}

// --- Formatting -----------------------------------------------------------------

// The nearest element of one of `names` around a node, inside the host.
dom::Element* formatting_ancestor(dom::Node& node, dom::Element& host, std::initializer_list<std::string_view> names)
{
    for (dom::Node* at = &node; at && at != &host; at = at->parent()) {
        if (!at->is_element())
            continue;
        auto* const element = static_cast<dom::Element*>(at);
        for (std::string_view const name : names) {
            if (element->is_html(name))
                return element;
        }
    }
    return nullptr;
}

struct InlineStyle {
    std::string_view command;
    std::string_view tag; // what wraps the selection
    std::initializer_list<std::string_view> names; // the elements that already give the style
};

constexpr InlineStyle inline_styles[] = {
    { "bold", "b", { "b", "strong" } },
    { "italic", "i", { "i", "em" } },
    { "underline", "u", { "u", "ins" } },
    { "strikethrough", "strike", { "s", "strike", "del" } },
    { "subscript", "sub", { "sub" } },
    { "superscript", "sup", { "sup" } },
};

InlineStyle const* inline_style(std::string_view command)
{
    for (InlineStyle const& style : inline_styles) {
        if (style.command == command)
            return &style;
    }
    return nullptr;
}

// Bold and the rest: off where the selection's start already has it (the
// element unwrapped), else the selection wrapped in the style's element.
bool toggle_inline(Realm::Internals& in, Current const& current, InlineStyle const& style)
{
    dom::Range& range = *current.range;
    if (collapsed(range))
        return false;
    if (dom::Element* const already = formatting_ancestor(*range.start_node, *current.host, style.names)) {
        dom::Node* const parent = already->parent();
        std::vector<dom::Node*> const children = already->children();
        for (dom::Node* child : children)
            pre_insert(in, *parent, *child, already);
        remove_node(in, *already);
        return true;
    }
    js::Interpreter::Roots const roots(in.interpreter);
    js::Heap::NoCollect const no_collect(in.interpreter.heap());
    std::optional<dom::Node*> const fragment = extract_range_contents(in, range);
    if (!fragment)
        return false;
    dom::Element* const wrapper = make_element(in, style.tag);
    pre_insert(in, *wrapper, **fragment, nullptr);
    insert_at_range(in, range, *wrapper);
    in.realm.set_selection(*wrapper, 0, *wrapper, static_cast<std::uint32_t>(wrapper->children().size()));
    return true;
}

// The caret put back where it was after its block's children moved to a
// new block: a Text node moves whole, so a point in one is still good; any
// other goes to the new block's start.
void restore_caret(Realm::Internals& in, Point caret, dom::Element& block)
{
    if (caret.node->is_text() && is_inclusive_ancestor(block, *caret.node))
        place_caret(in, caret);
    else
        place_caret(in, { &block, 0 });
}

// formatBlock: the block around the caret given another name ("h1",
// "<p>", "blockquote").
bool format_block(Realm::Internals& in, Current const& current, std::string value)
{
    if (value.size() > 2 && value.front() == '<' && value.back() == '>')
        value = value.substr(1, value.size() - 2);
    for (char& c : value)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    static constexpr std::string_view allowed[] = { "address", "dd", "div", "dt", "h1", "h2", "h3", "h4", "h5", "h6", "p", "pre",
        "blockquote", "li" };
    if (std::find(std::begin(allowed), std::end(allowed), value) == std::end(allowed))
        return false;
    dom::Range& range = *current.range;
    Point const caret { range.start_node, range.start_offset };
    dom::Element* block = block_of(*range.start_node, *current.host);
    dom::Element* const renamed = make_element(in, value);
    if (block == current.host) {
        // Text straight in the host: wrapped whole.
        std::vector<dom::Node*> const children = block->children();
        for (dom::Node* child : children)
            pre_insert(in, *renamed, *child, nullptr);
        pre_insert(in, *block, *renamed, nullptr);
    } else {
        std::vector<dom::Node*> const children = block->children();
        for (dom::Node* child : children)
            pre_insert(in, *renamed, *child, nullptr);
        pre_insert(in, *block->parent(), *renamed, block);
        remove_node(in, *block);
    }
    keep_line(in, *renamed);
    restore_caret(in, caret, *renamed);
    return true;
}

// Lists: the block around the caret made an item of a new list of the kind,
// or, already in one, taken out of it.
bool toggle_list(Realm::Internals& in, Current const& current, std::string_view kind)
{
    dom::Element* const host = current.host;
    Point const caret { current.range->start_node, current.range->start_offset };
    dom::Element* const block = block_of(*current.range->start_node, *host);
    if (block->is_html("li") && block->parent() && block->parent()->is_element()
        && static_cast<dom::Element*>(block->parent())->is_html(kind)) {
        dom::Node* const list = block->parent();
        dom::Element* const paragraph = make_element(in, "div");
        std::vector<dom::Node*> const children = block->children();
        for (dom::Node* child : children)
            pre_insert(in, *paragraph, *child, nullptr);
        pre_insert(in, *list->parent(), *paragraph, next_sibling_of(*list));
        remove_node(in, *block);
        if (list->children().empty())
            remove_node(in, *list);
        keep_line(in, *paragraph);
        restore_caret(in, caret, *paragraph);
        return true;
    }
    dom::Element* const list = make_element(in, kind);
    dom::Element* const item = make_element(in, "li");
    pre_insert(in, *list, *item, nullptr);
    if (block == host) {
        std::vector<dom::Node*> const children = host->children();
        for (dom::Node* child : children)
            pre_insert(in, *item, *child, nullptr);
        pre_insert(in, *host, *list, nullptr);
    } else {
        std::vector<dom::Node*> const children = block->children();
        for (dom::Node* child : children)
            pre_insert(in, *item, *child, nullptr);
        pre_insert(in, *block->parent(), *list, block);
        remove_node(in, *block);
    }
    keep_line(in, *item);
    restore_caret(in, caret, *item);
    return true;
}

// --- Running a command ----------------------------------------------------------

// The commands an edit names, by its inputType (Input Events Level 2 §4).
enum class Command {
    InsertText,
    InsertParagraph,
    InsertLineBreak,
    DeleteBackward,
    DeleteForward,
    DeleteWordBackward,
    DeleteWordForward,
    DeleteSelection,
    InsertHtml,
    Unknown,
};

Command command_for(std::string_view input_type)
{
    if (input_type == "insertText" || input_type == "insertReplacementText" || input_type == "insertFromPaste"
        || input_type == "insertFromDrop" || input_type == "insertFromYank" || input_type == "insertCompositionText")
        return Command::InsertText;
    if (input_type == "insertParagraph")
        return Command::InsertParagraph;
    if (input_type == "insertLineBreak")
        return Command::InsertLineBreak;
    if (input_type == "deleteContentBackward")
        return Command::DeleteBackward;
    if (input_type == "deleteContentForward" || input_type == "deleteContent")
        return Command::DeleteForward;
    if (input_type == "deleteWordBackward" || input_type == "deleteSoftLineBackward" || input_type == "deleteHardLineBackward")
        return Command::DeleteWordBackward;
    if (input_type == "deleteWordForward" || input_type == "deleteSoftLineForward" || input_type == "deleteHardLineForward")
        return Command::DeleteWordForward;
    if (input_type == "deleteByCut" || input_type == "deleteByDrag")
        return Command::DeleteSelection;
    if (input_type == "insertHTML")
        return Command::InsertHtml;
    return Command::Unknown;
}

// The change an edit makes: false when there was nothing it could do.
bool perform(Realm::Internals& in, Current const& current, Command command, std::string const& data)
{
    dom::Element& host = *current.host;
    dom::Range& range = *current.range;
    bool const plain = plaintext_only(host);
    Point caret { range.start_node, range.start_offset };
    bool const had_selection = !collapsed(range);
    if (had_selection && command != Command::Unknown)
        caret = delete_range(in, range, host);
    switch (command) {
    case Command::InsertText: {
        std::u16string text = js::utf16_from_utf8(data);
        if (text.empty())
            break;
        // Text with line breaks in it: a paragraph (a line break in
        // plaintext-only) for each.
        std::size_t from = 0;
        while (true) {
            std::size_t const newline = text.find(u'\n', from);
            std::u16string_view const line = std::u16string_view(text).substr(from, newline == std::u16string::npos ? std::u16string::npos : newline - from);
            std::u16string clean(line);
            std::erase(clean, u'\r');
            if (!clean.empty())
                caret = insert_text(in, caret, clean, false);
            if (newline == std::u16string::npos)
                break;
            caret = plain ? insert_line_break(in, caret, host) : insert_paragraph(in, caret, host);
            from = newline + 1;
        }
        break;
    }
    case Command::InsertParagraph:
        caret = plain ? insert_line_break(in, caret, host) : insert_paragraph(in, caret, host);
        break;
    case Command::InsertLineBreak:
        caret = insert_line_break(in, caret, host);
        break;
    case Command::DeleteBackward:
    case Command::DeleteForward:
    case Command::DeleteWordBackward:
    case Command::DeleteWordForward:
        if (!had_selection)
            caret = delete_from(in, caret, host, command == Command::DeleteForward || command == Command::DeleteWordForward,
                command == Command::DeleteWordBackward || command == Command::DeleteWordForward);
        break;
    case Command::DeleteSelection:
        break;
    case Command::InsertHtml: {
        if (plain) {
            caret = insert_text(in, caret, js::utf16_from_utf8(data), false);
            break;
        }
        dom::Element* const context = element_of(*caret.node);
        std::vector<dom::Node*> const parsed = parse_markup(in, context ? *context : host, data);
        if (caret.node->is_text()) {
            if (caret.offset == 0)
                caret = { caret.node->parent(), caret.node->index() };
            else if (caret.offset >= units_length(*caret.node))
                caret = { caret.node->parent(), caret.node->index() + 1 };
            else {
                split_text(in, static_cast<dom::Text&>(*caret.node), caret.offset);
                caret = { caret.node->parent(), caret.node->index() + 1 };
            }
        }
        for (dom::Node* node : parsed) {
            dom::Node* const before = caret.offset < caret.node->children().size() ? caret.node->children()[caret.offset] : nullptr;
            pre_insert(in, *caret.node, *node, before);
            caret = { node->parent(), node->index() + 1 };
        }
        break;
    }
    case Command::Unknown:
        return false;
    }
    if (caret.node && caret.node->parent() == nullptr && caret.node != in.document && !is_inclusive_ancestor(host, *caret.node))
        caret = { &host, 0 };
    place_caret(in, caret);
    return true;
}

// The StaticRange an edit is about to change, for getTargetRanges(): the
// selection when it holds something, else the character next to the caret.
js::Value target_ranges(Realm::Internals& in, Current const& current, Command command)
{
    js::ArrayObject* const array = in.interpreter.new_array();
    dom::Range const& range = *current.range;
    if (!collapsed(range) || command == Command::InsertText || command == Command::InsertParagraph || command == Command::InsertLineBreak
        || command == Command::InsertHtml) {
        array->push(js::Value::object(make_static_range(in, *range.start_node, range.start_offset, *range.end_node, range.end_offset)));
        return js::Value::object(array);
    }
    bool const forward = command == Command::DeleteForward || command == Command::DeleteWordForward;
    if (range.start_node->is_text()) {
        std::u16string const units = units_of(*range.start_node);
        std::uint32_t const offset = range.start_offset;
        if (forward ? offset < units.size() : offset > 0) {
            std::uint32_t const extent = character_extent(units, offset, forward);
            std::uint32_t const from = forward ? offset : offset - extent;
            array->push(js::Value::object(make_static_range(in, *range.start_node, from, *range.start_node, from + extent)));
            return js::Value::object(array);
        }
    }
    array->push(js::Value::object(make_static_range(in, *range.start_node, range.start_offset, *range.end_node, range.end_offset)));
    return js::Value::object(array);
}

EventObject* input_event(Realm::Internals& in, std::string_view type, std::string_view input_type, std::optional<std::string> const& data)
{
    EventObject* const event = in.new_event("InputEvent", type, true, type == "beforeinput");
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    event->composed = true;
    event->input_type = std::string(input_type);
    event->data = data.value_or(std::string());
    event->data_null = !data.has_value();
    return event;
}

std::optional<std::string> event_data(Command command, std::string const& data)
{
    if (command == Command::InsertText || command == Command::InsertHtml)
        return data;
    return std::nullopt;
}

std::string lowered(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return text;
}

// The commands execCommand knows (lowercased) and the inputType each is told as.
struct ExecCommand {
    std::string_view name;
    std::string_view input_type;
};

constexpr ExecCommand exec_commands[] = {
    { "inserttext", "insertText" },
    { "delete", "deleteContentBackward" },
    { "forwarddelete", "deleteContentForward" },
    { "insertparagraph", "insertParagraph" },
    { "insertlinebreak", "insertLineBreak" },
    { "inserthtml", "insertHTML" },
    { "bold", "formatBold" },
    { "italic", "formatItalic" },
    { "underline", "formatUnderline" },
    { "strikethrough", "formatStrikeThrough" },
    { "subscript", "formatSubscript" },
    { "superscript", "formatSuperscript" },
    { "formatblock", "" },
    { "insertorderedlist", "insertOrderedList" },
    { "insertunorderedlist", "insertUnorderedList" },
    { "selectall", "" },
    { "undo", "historyUndo" },
    { "redo", "historyRedo" },
    { "copy", "" },
    { "cut", "deleteByCut" },
    { "paste", "insertFromPaste" },
    { "styleWithCSS", "" },
    { "defaultparagraphseparator", "" },
};

ExecCommand const* exec_command(std::string const& name)
{
    std::string const lower = lowered(name);
    for (ExecCommand const& command : exec_commands) {
        if (lowered(std::string(command.name)) == lower)
            return &command;
    }
    return nullptr;
}

// --- The history of edits -------------------------------------------------------
//
// Each edit the reader makes is kept as the host's markup before and after
// it and where the caret was, so that Ctrl+Z (historyUndo) puts the markup
// and the caret back and Ctrl+Y or Ctrl+Shift+Z (historyRedo) makes the
// edit again. Typing runs together into one step, as in every browser. A
// host whose markup a script changed since is no longer what a step was
// made against: its history is let go of rather than undone over the
// script's work.

// A place in a host as the child indexes down to it and the offset there.
std::vector<std::uint32_t> path_of(dom::Node const& host, dom::Node const* node, std::uint32_t offset)
{
    std::vector<std::uint32_t> path;
    for (dom::Node const* at = node; at && at != &host; at = at->parent())
        path.push_back(at->index());
    std::reverse(path.begin(), path.end());
    path.push_back(offset);
    return path;
}

Point point_of(dom::Node& host, std::vector<std::uint32_t> const& path)
{
    dom::Node* node = &host;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        if (path[i] >= node->children().size())
            return { &host, 0 };
        node = node->children()[path[i]];
    }
    std::uint32_t const offset = path.empty() ? 0 : std::min(path.back(), boundary_length(*node));
    return { node, offset };
}

std::vector<std::uint32_t> caret_path(Realm::Internals& in, dom::Element& host)
{
    dom::Range* range = nullptr;
    bool backward = false;
    if (!selection_range(in, range, backward))
        return { 0 };
    dom::Node* const node = backward ? range->start_node : range->end_node;
    std::uint32_t const offset = backward ? range->start_offset : range->end_offset;
    if (!is_inclusive_ancestor(host, *node))
        return { 0 };
    return path_of(host, node, offset);
}

void record_edit(Realm::Internals& in, dom::Element& host, std::string_view input_type, std::string before,
    std::vector<std::uint32_t> caret_before)
{
    std::string after = html::serialize_children(host);
    if (after == before)
        return;
    std::vector<Realm::Internals::EditStep>& undo = in.undo_steps;
    in.redo_steps.clear();
    bool const typing = input_type == "insertText";
    if (typing && !undo.empty() && undo.back().host == &host && undo.back().typing && undo.back().after == before) {
        undo.back().after = std::move(after);
        undo.back().caret_after = caret_path(in, host);
        return;
    }
    undo.push_back(Realm::Internals::EditStep { &host, std::move(before), std::move(after), std::move(caret_before), caret_path(in, host), typing });
    constexpr std::size_t kept = 100;
    if (undo.size() > kept)
        undo.erase(undo.begin());
}

// Undoes (or redoes) the last step made in the host: false when there is
// none to take.
bool step_history(Realm::Internals& in, dom::Element& host, bool undo)
{
    std::vector<Realm::Internals::EditStep>& from = undo ? in.undo_steps : in.redo_steps;
    std::vector<Realm::Internals::EditStep>& to = undo ? in.redo_steps : in.undo_steps;
    auto const last = std::find_if(from.rbegin(), from.rend(), [&](auto const& step) { return step.host == &host; });
    if (last == from.rend())
        return false;
    std::string const now = html::serialize_children(host);
    if (now != (undo ? last->after : last->before)) {
        // A script changed the host since: nothing here can be put back.
        std::erase_if(in.undo_steps, [&](auto const& step) { return step.host == &host; });
        std::erase_if(in.redo_steps, [&](auto const& step) { return step.host == &host; });
        return false;
    }
    Realm::Internals::EditStep step = *last;
    from.erase(std::next(last).base());
    replace_children_with_markup(in, host, host, undo ? step.before : step.after);
    place_caret(in, point_of(host, undo ? step.caret_before : step.caret_after));
    step.typing = false; // a step undone is never typed on to
    to.push_back(std::move(step));
    return true;
}

bool is_format(std::string_view input_type)
{
    return input_type.starts_with("format");
}

InlineStyle const* style_for_input_type(std::string_view input_type)
{
    if (input_type == "formatBold")
        return inline_style("bold");
    if (input_type == "formatItalic")
        return inline_style("italic");
    if (input_type == "formatUnderline")
        return inline_style("underline");
    if (input_type == "formatStrikeThrough")
        return inline_style("strikethrough");
    if (input_type == "formatSubscript")
        return inline_style("subscript");
    if (input_type == "formatSuperscript")
        return inline_style("superscript");
    return nullptr;
}

} // namespace

bool Realm::edit(std::string_view input_type, std::string const& data)
{
    Internals& in = *m_internals;
    Internals::Entry const entry(in);
    js::Interpreter::Roots const roots(in.interpreter);
    std::optional<Current> current = current_selection(in);
    if (!current)
        return false;
    Command const command = command_for(input_type);
    dom::Element* const host = current->host;
    in.interpreter.root(js::Value::object(in.wrap(*host)));
    // beforeinput: the page may take the edit as its own and cancel it.
    EventObject* const before = input_event(in, "beforeinput", input_type, event_data(command, data));
    before->target_ranges = target_ranges(in, *current, command);
    bool const proceed = in.dispatch(*before, in.wrap(*host));
    if (!proceed)
        return true;
    // What the listeners did may have moved the selection, or taken the
    // host away.
    current = current_selection(in);
    if (!current || current->host != host)
        return true;
    bool const history = input_type == "historyUndo" || input_type == "historyRedo";
    std::string markup_before;
    std::vector<std::uint32_t> caret_before;
    if (!history) {
        markup_before = html::serialize_children(*host);
        caret_before = caret_path(in, *host);
    }
    bool done = false;
    if (history)
        done = step_history(in, *host, input_type == "historyUndo");
    else if (is_format(input_type))
        done = !plaintext_only(*host) && style_for_input_type(input_type) && toggle_inline(in, *current, *style_for_input_type(input_type));
    else
        done = perform(in, *current, command, data);
    if (!done)
        return true;
    if (!history)
        record_edit(in, *host, input_type, std::move(markup_before), std::move(caret_before));
    EventObject* const after = input_event(in, "input", input_type, event_data(command, data));
    in.dispatch(*after, in.wrap(*host));
    return true;
}

dom::Element* Realm::editing_host(dom::Node& node)
{
    return editing_host_of(*m_internals, node);
}

void install_editing(Realm::Internals& in, js::Object& document)
{
    // execCommand(command, showUI, value): the command run on the selection
    // in its editing host, `input` told after (no beforeinput: Input Events
    // §4.2); false when the command is unknown, not enabled, or changed
    // nothing.
    document_method(in, document, "execCommand", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::optional<std::string> value = std::string();
        if (args.size() > 2) {
            value = internals.to_utf8(args[2]);
            if (!value)
                return std::nullopt;
        }
        ExecCommand const* const command = exec_command(*name);
        if (!command || &d != internals.document)
            return js::Value::boolean(false);
        std::string const lower = lowered(*name);
        if (lower == "defaultparagraphseparator" || lower == "stylewithcss")
            return js::Value::boolean(true);
        js::Interpreter::Roots const roots(internals.interpreter);
        std::optional<Current> const current = current_selection(internals);
        if (lower == "selectall") {
            if (current)
                internals.realm.set_selection(*current->host, 0, *current->host, static_cast<std::uint32_t>(current->host->children().size()));
            else if (dom::Element* const body = body_element(d))
                internals.realm.set_selection(*body, 0, *body, static_cast<std::uint32_t>(body->children().size()));
            return js::Value::boolean(true);
        }
        if (!current)
            return js::Value::boolean(false);
        // copy, cut and paste reach the clipboard only from the reader's
        // own keys, never from a script's call; undo and redo are the
        // window's (⛔ not yet a history of edits here).
        if (lower == "copy" || lower == "cut" || lower == "paste")
            return js::Value::boolean(false);
        dom::Element* const host = current->host;
        if (lower == "undo" || lower == "redo") {
            internals.interpreter.root(js::Value::object(internals.wrap(*host)));
            if (!step_history(internals, *host, lower == "undo"))
                return js::Value::boolean(false);
            EventObject* const told = input_event(internals, "input", lower == "undo" ? "historyUndo" : "historyRedo", std::nullopt);
            internals.dispatch(*told, internals.wrap(*host));
            return js::Value::boolean(true);
        }
        internals.interpreter.root(js::Value::object(internals.wrap(*host)));
        bool done = false;
        if (InlineStyle const* const style = inline_style(lower))
            done = !plaintext_only(*host) && toggle_inline(internals, *current, *style);
        else if (lower == "formatblock")
            done = !plaintext_only(*host) && format_block(internals, *current, *value);
        else if (lower == "insertorderedlist" || lower == "insertunorderedlist")
            done = !plaintext_only(*host) && toggle_list(internals, *current, lower == "insertorderedlist" ? "ol" : "ul");
        else {
            Command const kind = command_for(command->input_type);
            done = perform(internals, *current, kind, *value);
        }
        if (!done)
            return js::Value::boolean(false);
        Command const kind = command_for(command->input_type);
        EventObject* const after = input_event(internals, "input", command->input_type, event_data(kind, *value));
        internals.dispatch(*after, internals.wrap(*host));
        return js::Value::boolean(true);
    });
    document_method(in, document, "queryCommandSupported", 1, [](Realm::Internals& internals, dom::Document&, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        return js::Value::boolean(exec_command(*name) != nullptr);
    });
    document_method(in, document, "queryCommandEnabled", 1, [](Realm::Internals& internals, dom::Document&, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        ExecCommand const* const command = exec_command(*name);
        if (!command)
            return js::Value::boolean(false);
        std::string const lower = lowered(*name);
        if (lower == "selectall" || lower == "defaultparagraphseparator" || lower == "stylewithcss")
            return js::Value::boolean(true);
        if (lower == "copy" || lower == "cut" || lower == "paste")
            return js::Value::boolean(false);
        if (lower == "undo" || lower == "redo") {
            std::optional<Current> const current = current_selection(internals);
            auto const& steps = lower == "undo" ? internals.undo_steps : internals.redo_steps;
            return js::Value::boolean(current && std::any_of(steps.begin(), steps.end(), [&](auto const& step) { return step.host == current->host; }));
        }
        return js::Value::boolean(current_selection(internals).has_value());
    });
    document_method(in, document, "queryCommandState", 1, [](Realm::Internals& internals, dom::Document&, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::optional<Current> const current = current_selection(internals);
        InlineStyle const* const style = inline_style(lowered(*name));
        if (!current || !style)
            return js::Value::boolean(false);
        return js::Value::boolean(formatting_ancestor(*current->range->start_node, *current->host, style->names) != nullptr);
    });
    document_method(in, document, "queryCommandIndeterm", 1, [](Realm::Internals&, dom::Document&, Args) -> Native {
        return js::Value::boolean(false);
    });
    document_method(in, document, "queryCommandValue", 1, [](Realm::Internals& internals, dom::Document&, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::string const lower = lowered(*name);
        if (lower == "defaultparagraphseparator")
            return internals.string("div");
        if (lower == "formatblock") {
            if (std::optional<Current> const current = current_selection(internals)) {
                dom::Element* const block = block_of(*current->range->start_node, *current->host);
                if (block != current->host)
                    return internals.string(block->local_name());
            }
        }
        return internals.string("");
    });
}

}
