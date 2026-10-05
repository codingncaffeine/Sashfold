#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "text/FontManager.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Incremental restyling against the resolver's own from-scratch answer: a
// large page is changed at random, one mutation at a time, and after each
// one the kept styles are brought up to date and compared field by field
// with a whole resolution. Zero differences, and far fewer elements
// computed than the page holds when the change is local.

using namespace sashfold;

namespace {

// A fixed sequence, the same on every machine and standard library.
struct Sequence {
    std::uint64_t state;
    std::uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::uint32_t>(state >> 16);
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : next() % n; }
};

constexpr std::string_view page_sheet = R"(
body { color: rgb(10, 20, 30); font-size: 15px; --accent: rgb(1, 2, 3) }
body.scroll { overflow: auto }
html.wide body { margin: 20px }
.box { padding: 2px; border: 1px solid rgb(9, 9, 9) }
.box.on { color: rgb(200, 0, 0); font-size: 18px }
.on .leaf { text-decoration: underline }
.group > .leaf { margin-left: 3px }
.leaf + .leaf { margin-top: 1px }
.leaf ~ .tail { padding-left: 4px }
.row:nth-child(3n) { background-color: rgb(240, 240, 240) }
.row:first-child { border-top: 2px solid rgb(0, 0, 255) }
[data-k] { outline-color: red; letter-spacing: 1px }
[data-k="2"] { word-spacing: 2px }
.themed { --accent: rgb(90, 80, 70) }
.leaf { color: var(--accent) }
.frame { border: 3px dotted rgb(5, 6, 7) }
.frame > .inner { border: inherit }
.e:empty { background-color: rgb(1, 1, 1) }
a:hover { color: rgb(255, 0, 255) }
#hero { font-weight: bold }
#hero .leaf { font-style: italic }
li.pick { color: rgb(0, 128, 0) }
)";

// The features tested elsewhere hidden in arguments, attributes tested on
// ancestors and siblings, counts of siblings `of S`, and the last child.
constexpr std::string_view argument_sheet = R"(
body { color: rgb(10, 20, 30); font-size: 15px }
:not(.on) > .leaf { color: rgb(0, 90, 0) }
:is(.themed, #hero) .row > .tail { font-weight: bold }
.group:where(.on) .leaf { font-size: 19px }
[data-k="1"] ~ .row { padding-left: 5px }
[data-k] .leaf:last-child { margin-right: 2px }
.row:nth-child(2 of .on) { margin-top: 4px }
.leaf:not(.row .pick) { letter-spacing: 1px }
a:any-link .leaf { color: rgb(1, 1, 200) }
.frame > * { border: inherit }
li.pick { color: rgb(0, 128, 0) }
.row:last-child .tail { padding-bottom: 3px }
)";

// :has() tested on the subject, or on an anchor above it, with descendants
// and children in its arguments — and siblings among those: what a change
// inside an element turns, among inherited and own properties, through :is()
// and :not().
constexpr std::string_view has_sheet = R"(
body { color: rgb(10, 20, 30); font-size: 15px }
.group:has(.on) { color: rgb(200, 0, 0) }
.group:has(> .row > .leaf.pick) { padding-left: 6px }
div:has([data-k="2"]) { font-size: 17px }
:is(.box, .row):has(#hero) { border-top: 1px solid rgb(0, 0, 9) }
.row:not(:has(.leaf)) { margin-top: 2px }
.row:has(.e:empty) { letter-spacing: 1px }
body:has(.frame .inner) { word-spacing: 3px }
.leaf { color: inherit }
.group:has(.on) .leaf { font-style: italic }
.row:has(> .tail.pick) > .leaf { word-spacing: 1px }
.row:has(.leaf + .tail.pick) { padding-right: 2px }
.group:has(.leaf.on ~ .tail) .tail { margin-left: 1px }
.group:not(:has(.leaf.on)) .tail { text-indent: 2px }
.box:is(:has(> .row.pick), .frame) > .row { padding-bottom: 1px }
)";

// ::first-letter on blocks, handed down the chain of first block children,
// with classes that make a box a flex container, take it out of the flow or
// out of the tree: what an update must hand down again.
constexpr std::string_view first_letter_sheet = R"(
body { color: rgb(10, 20, 30); font-size: 15px }
.group::first-letter { color: rgb(200, 0, 0) }
.box.on::first-letter { font-size: 20px }
.row::first-letter { text-transform: uppercase }
.leaf.pick { float: left }
.row.pick { display: flex }
.tail.on { display: none }
.group.pick { position: absolute }
)";

// The states a pointer and the focus set, tested on the element itself, on
// an ancestor and on an earlier sibling (selectors-4 §9).
constexpr std::string_view state_sheet = R"(
body { color: rgb(10, 20, 30); font-size: 15px }
.leaf:hover { color: rgb(1, 0, 0) }
.group:hover .tail { font-weight: bold }
.row:active > .leaf { padding-left: 3px }
.row:focus-within .tail { color: rgb(0, 2, 0) }
.leaf:focus + .leaf { color: rgb(0, 0, 3) }
.group:focus-visible { padding-top: 4px }
.box:not(:hover) > .row { margin-left: 1px }
:is(.frame:hover, .group:active) .inner { text-indent: 5px }
.leaf:hover ~ .tail { letter-spacing: 1px }
)";

constexpr std::string_view extra_sheet = R"(
.leaf { background-color: rgb(3, 3, 3) }
.group { padding-top: 7px }
)";

struct Page {
    std::unique_ptr<dom::Document> document;
    dom::Element* body = nullptr;
    std::vector<dom::Element*> elements; // every element made, in the tree or not
};

dom::Element* make(dom::Document& document, std::string_view tag, std::string_view classes)
{
    dom::Element* element = document.create<dom::Element>(std::string(dom::ns::html), std::string(tag));
    if (!classes.empty())
        element->attributes().push_back(dom::Attr { "class", std::string(classes), "", "" });
    return element;
}

dom::Text* make_text(dom::Document& document, std::string_view text)
{
    dom::Text* node = document.create<dom::Text>();
    node->data = std::string(text);
    return node;
}

dom::Element* find_body(dom::Node& node)
{
    if (node.is_element() && static_cast<dom::Element&>(node).is_html("body"))
        return static_cast<dom::Element*>(&node);
    for (dom::Node* child : node.children()) {
        if (dom::Element* found = find_body(*child))
            return found;
    }
    return nullptr;
}

std::size_t count_elements(dom::Node const& node)
{
    std::size_t count = node.is_element() ? 1 : 0;
    for (dom::Node const* child : node.children())
        count += count_elements(*child);
    return count;
}

// A group: a box holding rows, each row a few leaves with text, a tail,
// now and then an empty element, a framed pair, a list.
dom::Element* make_group(Page& page, Sequence& random, int index)
{
    dom::Document& document = *page.document;
    dom::Element* group = make(document, "div", index % 5 == 0 ? "box group themed" : "box group");
    page.elements.push_back(group);
    for (int r = 0; r < 6; ++r) {
        dom::Element* row = make(document, "div", "row");
        page.elements.push_back(row);
        group->append_child(*row);
        int const leaves = 2 + static_cast<int>(random.below(3));
        for (int l = 0; l < leaves; ++l) {
            dom::Element* leaf = make(document, "span", "leaf");
            page.elements.push_back(leaf);
            leaf->append_child(*make_text(document, "word"));
            row->append_child(*leaf);
        }
        dom::Element* tail = make(document, "b", "tail");
        page.elements.push_back(tail);
        row->append_child(*tail);
        if (r == 2) {
            dom::Element* empty = make(document, "i", "e");
            page.elements.push_back(empty);
            row->append_child(*empty);
        }
    }
    if (index % 4 == 1) {
        dom::Element* frame = make(document, "div", "frame");
        dom::Element* inner = make(document, "p", "inner");
        page.elements.push_back(frame);
        page.elements.push_back(inner);
        inner->append_child(*make_text(document, "framed"));
        frame->append_child(*inner);
        group->append_child(*frame);
    }
    if (index % 6 == 3) {
        dom::Element* list = make(document, "ul", "");
        page.elements.push_back(list);
        for (int i = 0; i < 3; ++i) {
            dom::Element* item = make(document, "li", "");
            page.elements.push_back(item);
            item->append_child(*make_text(document, "item"));
            list->append_child(*item);
        }
        group->append_child(*list);
    }
    if (index % 7 == 2) {
        dom::Element* auto_dir = make(document, "p", "");
        auto_dir->attributes().push_back(dom::Attr { "dir", "auto", "", "" });
        auto_dir->append_child(*make_text(document, "left to right"));
        page.elements.push_back(auto_dir);
        group->append_child(*auto_dir);
    }
    return group;
}

Page make_page(std::size_t at_least)
{
    Page page;
    page.document = html::parse_document(std::string_view("<!doctype html><html><head></head><body></body></html>"));
    page.body = find_body(*page.document);
    Sequence random { 0x5a5f01d5u };
    int index = 0;
    while (count_elements(*page.document) < at_least)
        page.body->append_child(*make_group(page, random, index++));
    return page;
}

bool connected_and_movable(dom::Element const* element)
{
    if (!element->is_connected())
        return false;
    return !element->is_html("html") && !element->is_html("head") && !element->is_html("body");
}

void toggle_class(dom::Element& element, std::string_view name)
{
    std::vector<dom::Attr>& attributes = element.attributes();
    for (dom::Attr& attribute : attributes) {
        if (attribute.local_name != "class")
            continue;
        std::string const padded = " " + attribute.value + " ";
        std::string const token = " " + std::string(name) + " ";
        if (std::size_t const at = padded.find(token); at != std::string::npos) {
            std::string const rest = padded.substr(0, at) + " " + padded.substr(at + token.size());
            std::size_t first = rest.find_first_not_of(' ');
            std::size_t last = rest.find_last_not_of(' ');
            attribute.value = first == std::string::npos ? "" : rest.substr(first, last - first + 1);
        } else {
            attribute.value = attribute.value.empty() ? std::string(name) : attribute.value + " " + std::string(name);
        }
        return;
    }
    attributes.push_back(dom::Attr { "class", std::string(name), "", "" });
}

void set_attribute(dom::Element& element, std::string_view name, std::string value)
{
    for (dom::Attr& attribute : element.attributes()) {
        if (attribute.local_name == name) {
            attribute.value = std::move(value);
            return;
        }
    }
    element.attributes().push_back(dom::Attr { std::string(name), std::move(value), "", "" });
}

void remove_attribute(dom::Element& element, std::string_view name)
{
    std::vector<dom::Attr>& attributes = element.attributes();
    std::erase_if(attributes, [name](dom::Attr const& attribute) { return attribute.local_name == name; });
}

dom::Text* first_text(dom::Node& node)
{
    for (dom::Node* child : node.children()) {
        if (child->is_text())
            return static_cast<dom::Text*>(child);
        if (dom::Text* found = first_text(*child))
            return found;
    }
    return nullptr;
}

// One kept map and its record, as the shell and a script's questions each
// keep their own over the same document.
struct Kept {
    css::StyleMap styles;
    css::StyleRecord record;
};

struct Step {
    css::RestyleOutcome outcome;
    std::optional<std::string> difference;
};

Step update_and_check(dom::Document const& document, css::StyleSet const& set, Kept& kept)
{
    Step step;
    step.outcome = css::update_styles(document, set, kept.styles, kept.record);
    step.difference = css::check_incremental(document, set, kept.styles);
    return step;
}

void random_mutations(std::string_view sheet, std::uint64_t seed)
{
    Page page = make_page(3000);
    std::size_t const size = count_elements(*page.document);
    CHECK(size >= 3000);
    auto set = std::make_unique<css::StyleSet>(std::vector<css::SheetSource> { { std::string(sheet), std::nullopt } });
    Kept shell;
    Kept script;
    Step first = update_and_check(*page.document, *set, shell);
    CHECK(first.outcome.whole);
    CHECK_EQ(first.outcome.computed, size);
    CHECK_EQ(first.difference.value_or(""), std::string());

    Sequence random { seed };
    std::size_t differences = 0;
    std::size_t computed_total = 0;
    std::size_t whole_updates = 0;
    std::string whole_reasons;
    std::string first_difference;
    int const steps = 300;
    for (int step = 0; step < steps; ++step) {
        std::vector<dom::Element*> live;
        for (dom::Element* element : page.elements) {
            if (element->is_connected())
                live.push_back(element);
        }
        dom::Element& target = *live[random.below(live.size())];
        bool sheet_step = false;
        switch (random.below(11)) {
        case 0:
            toggle_class(target, "on");
            break;
        case 1:
            toggle_class(target, random.below(2) ? "leaf" : "themed");
            break;
        case 2:
            if (random.below(3) == 0)
                remove_attribute(target, "id");
            else
                set_attribute(target, "id", random.below(4) == 0 ? "hero" : "n" + std::to_string(step));
            break;
        case 3:
            set_attribute(target, "style", "font-size: " + std::to_string(10 + random.below(10)) + "px; --accent: rgb(" + std::to_string(random.below(255)) + ", 0, 0)");
            break;
        case 4: {
            // A new subtree, anywhere below the body.
            dom::Element* group = make_group(page, random, static_cast<int>(random.below(40)));
            dom::Node* parent = connected_and_movable(&target) ? target.parent() : page.body;
            parent->insert_before(*group, connected_and_movable(&target) ? &target : nullptr);
            break;
        }
        case 5:
            if (connected_and_movable(&target))
                target.remove();
            break;
        case 6:
            // A subtree moved elsewhere.
            if (connected_and_movable(&target) && live.size() > 1) {
                dom::Element* to = live[random.below(live.size())];
                bool inside = false;
                for (dom::Node const* up = to; up; up = up->parent())
                    inside = inside || up == &target;
                if (!inside && !to->is_html("html") && !to->is_html("head"))
                    to->append_child(target);
            }
            break;
        case 7: {
            // Text: some changed, some emptied, some given right-to-left words.
            if (dom::Text* text = first_text(target)) {
                std::size_t const pick = random.below(3);
                text->data = pick == 0 ? "" : pick == 1 ? "changed" : "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
                text->mark_style_data();
            } else {
                target.append_child(*make_text(*page.document, "new text"));
            }
            break;
        }
        case 8: {
            // The pointer moves, a button goes down or up, the focus moves:
            // the document's states, which the selectors read.
            std::size_t const pick = random.below(5);
            if (pick == 0)
                page.document->set_hovered(&target);
            else if (pick == 1)
                page.document->set_active(random.below(2) ? &target : nullptr);
            else if (pick == 2)
                page.document->set_focused(random.below(4) ? &target : nullptr, random.below(2) == 0);
            else if (random.below(2))
                set_attribute(target, "data-k", std::to_string(random.below(3)));
            else
                remove_attribute(target, "data-k");
            break;
        }
        case 9:
            if (random.below(4) == 0)
                toggle_class(*page.body, "scroll");
            else if (random.below(3) == 0)
                toggle_class(static_cast<dom::Element&>(*page.body->parent()), "wide");
            else
                toggle_class(target, "row");
            break;
        case 10:
            toggle_class(target, "pick");
            break;
        }
        if (step == 150) {
            // A sheet arrives: the set is built again, and everything with it.
            set = std::make_unique<css::StyleSet>(std::vector<css::SheetSource> {
                { std::string(sheet), std::nullopt }, { std::string(extra_sheet), std::nullopt } });
            sheet_step = true;
        }
        Step const result = update_and_check(*page.document, *set, shell);
        computed_total += result.outcome.computed;
        whole_updates += result.outcome.whole ? 1 : 0;
        if (result.outcome.whole)
            whole_reasons += std::string(whole_reasons.empty() ? "" : "; ") + std::string(result.outcome.reason);
        if (result.difference) {
            ++differences;
            if (first_difference.empty())
                first_difference = "step " + std::to_string(step) + ": " + *result.difference;
        }
        if (sheet_step) {
            CHECK(result.outcome.whole);
            CHECK_EQ(result.outcome.computed, count_elements(*page.document));
        }
        // The second holder looks now and then, so that it sees many
        // changes at once, some of them undone again.
        if (step % 7 == 3) {
            Step const other = update_and_check(*page.document, *set, script);
            if (other.difference) {
                ++differences;
                if (first_difference.empty())
                    first_difference = "step " + std::to_string(step) + " (second holder): " + *other.difference;
            }
        }
    }
    CHECK_EQ(first_difference, std::string());
    CHECK_EQ(differences, std::size_t(0));
    std::size_t const final_size = count_elements(*page.document);
    std::cout << "  " << steps << " mutations over " << final_size << " elements: " << computed_total
              << " computed in all, " << whole_updates << " whole (" << whole_reasons << ")\n";
    // Most of the changes are local: far less than a whole restyle each.
    CHECK(computed_total < static_cast<std::size_t>(steps) * final_size / 4);
}

// The bounds, each measured on a fresh page: a leaf's class, the root's
// class, and a new set.
void bounds()
{
    Page page = make_page(3000);
    std::size_t const size = count_elements(*page.document);
    css::StyleSet set(std::vector<css::SheetSource> { { std::string(page_sheet), std::nullopt } });
    Kept kept;
    update_and_check(*page.document, set, kept);

    // Nothing changed: nothing computed.
    Step const idle = update_and_check(*page.document, set, kept);
    CHECK(!idle.outcome.whole);
    CHECK_EQ(idle.outcome.computed, std::size_t(0));
    CHECK_EQ(idle.difference.value_or(""), std::string());

    // A leaf's class: the leaf, and through + and ~ its siblings after it.
    dom::Element* leaf = nullptr;
    for (dom::Element* element : page.elements) {
        if (element->is_html("span") && element->parent() && element->parent()->children().front() == element) {
            leaf = element;
            break;
        }
    }
    CHECK(leaf != nullptr);
    std::size_t after = 0;
    bool past = false;
    for (dom::Node const* sibling : leaf->parent()->children()) {
        if (past)
            after += count_elements(*sibling);
        past = past || sibling == leaf;
    }
    toggle_class(*leaf, "on");
    Step const local = update_and_check(*page.document, set, kept);
    CHECK(!local.outcome.whole);
    CHECK(local.outcome.computed >= 1);
    CHECK(local.outcome.computed <= 1 + after);
    CHECK(local.outcome.computed <= 8);
    CHECK_EQ(local.difference.value_or(""), std::string());

    // A text change in a leaf: the sheet reads :empty, so the leaf holding
    // the text is its own change, and it reaches the siblings after it.
    dom::Text* text = first_text(*leaf);
    text->data = "other";
    text->mark_style_data();
    Step const worded = update_and_check(*page.document, set, kept);
    CHECK(worded.outcome.computed >= 1);
    CHECK(worded.outcome.computed <= 1 + after);
    CHECK_EQ(worded.difference.value_or(""), std::string());

    // A text change in a sheet without :empty or dir=auto computes nothing.
    css::StyleSet plain(std::vector<css::SheetSource> { { ".leaf { color: red }", std::nullopt } });
    Kept plain_kept;
    update_and_check(*page.document, plain, plain_kept);
    text->data = "again";
    text->mark_style_data();
    Step const unread = update_and_check(*page.document, plain, plain_kept);
    CHECK(!unread.outcome.whole);
    CHECK_EQ(unread.outcome.computed, std::size_t(0));
    CHECK_EQ(unread.difference.value_or(""), std::string());
    // The first holder has not seen that text change yet, and does now.
    Step const caught_up = update_and_check(*page.document, set, kept);
    CHECK(caught_up.outcome.computed >= 1);
    CHECK_EQ(caught_up.difference.value_or(""), std::string());

    // The root's class reaches what the selectors testing it name, and no
    // further: `html.wide body` names the body, which changes nothing it
    // hands down.
    dom::Element& html = static_cast<dom::Element&>(*page.body->parent());
    toggle_class(html, "wide");
    Step const root = update_and_check(*page.document, set, kept);
    CHECK(!root.outcome.whole);
    CHECK_EQ(root.outcome.computed, std::size_t(2));
    CHECK_EQ(root.difference.value_or(""), std::string());

    // `.on .leaf` names every leaf: the root and each leaf.
    std::size_t leaves = 0;
    for (dom::Element const* element : page.elements) {
        dom::Attr const* classes = element->find_attribute("class");
        leaves += element->is_connected() && classes && (" " + classes->value + " ").find(" leaf ") != std::string::npos ? 1 : 0;
    }
    toggle_class(html, "on");
    Step const named = update_and_check(*page.document, set, kept);
    CHECK(!named.outcome.whole);
    CHECK_EQ(named.outcome.computed, 1 + leaves);
    CHECK_EQ(named.difference.value_or(""), std::string());

    // A class the rules test on ancestors of any element at all: the root
    // and everything in it.
    css::StyleSet wide(std::vector<css::SheetSource> { { ".all * { color: red }", std::nullopt } });
    Kept wide_kept;
    update_and_check(*page.document, wide, wide_kept);
    toggle_class(html, "all");
    Step const everything = update_and_check(*page.document, wide, wide_kept);
    CHECK(!everything.outcome.whole);
    CHECK_EQ(everything.outcome.computed, size);
    CHECK_EQ(everything.difference.value_or(""), std::string());

    // A new viewport is another state of the set: everything.
    set.set_viewport(640, 480);
    Step const resized = update_and_check(*page.document, set, kept);
    CHECK(resized.outcome.whole);
    CHECK_EQ(resized.outcome.computed, size);
    CHECK_EQ(resized.difference.value_or(""), std::string());

    // The document asking for everything (the fonts it is measured in).
    page.document->mark_style_everything();
    Step const asked = update_and_check(*page.document, set, kept);
    CHECK(asked.outcome.whole);
    CHECK_EQ(asked.difference.value_or(""), std::string());
}

// The cases that read more than the element and its ancestors, one by one.
void reaches()
{
    auto document = html::parse_document(std::string_view(R"(<!doctype html><html><head></head><body>
<div id=frame class=frame><p id=inner class=inner>x</p></div>
<div id=holder><i id=empty class=e></i></div>
<p id=auto dir=auto><span id=words>abc</span></p>
<ol id=list><li id=one>a</li><li id=two>b</li></ol>
<div id=sib><span id=s1 class=leaf>a</span><span id=s2 class=leaf>b</span><b id=s3 class=tail>c</b></div>
</body></html>)"));
    auto by_id = [&](std::string_view id) -> dom::Element* {
        std::vector<dom::Node*> pending { document.get() };
        while (!pending.empty()) {
            dom::Node* node = pending.back();
            pending.pop_back();
            if (node->is_element()) {
                dom::Attr const* attribute = static_cast<dom::Element*>(node)->find_attribute("id");
                if (attribute && attribute->value == id)
                    return static_cast<dom::Element*>(node);
            }
            for (dom::Node* child : node->children())
                pending.push_back(child);
        }
        return nullptr;
    };
    css::StyleSet set(std::vector<css::SheetSource> { { std::string(page_sheet), std::nullopt } });
    Kept kept;
    update_and_check(*document, set, kept);

    // `border: inherit` reads a property that does not inherit.
    toggle_class(*by_id("frame"), "frame");
    Step const inherit = update_and_check(*document, set, kept);
    CHECK_EQ(inherit.difference.value_or(""), std::string());

    // :empty: an element made non-empty by text alone.
    by_id("empty")->append_child(*make_text(*document, "now"));
    Step const filled = update_and_check(*document, set, kept);
    CHECK_EQ(filled.difference.value_or(""), std::string());

    // dir=auto: the direction follows text deep inside.
    dom::Text* words = first_text(*by_id("words"));
    words->data = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    words->mark_style_data();
    Step const turned = update_and_check(*document, set, kept);
    CHECK_EQ(turned.difference.value_or(""), std::string());

    // A list item's class changes nothing it counts: kept to itself.
    toggle_class(*by_id("two"), "pick");
    Step const picked = update_and_check(*document, set, kept);
    CHECK(!picked.outcome.whole);
    CHECK_EQ(picked.difference.value_or(""), std::string());

    // A new list item numbers the ones after it: everything.
    dom::Element* three = make(*document, "li", "");
    by_id("list")->insert_before(*three, by_id("one"));
    Step const numbered = update_and_check(*document, set, kept);
    CHECK(numbered.outcome.whole);
    CHECK_EQ(numbered.difference.value_or(""), std::string());

    // A sibling combinator reaches the siblings after the change.
    toggle_class(*by_id("s1"), "leaf");
    Step const sibling = update_and_check(*document, set, kept);
    CHECK(!sibling.outcome.whole);
    CHECK_EQ(sibling.outcome.computed, std::size_t(3));
    CHECK_EQ(sibling.difference.value_or(""), std::string());

    // Body's overflow goes to the viewport: body and root settled again.
    toggle_class(*find_body(*document), "scroll");
    Step const scrolled = update_and_check(*document, set, kept);
    CHECK(!scrolled.outcome.whole);
    CHECK_EQ(scrolled.difference.value_or(""), std::string());

    // A removed element keeps no style.
    by_id("holder")->remove();
    Step const removed = update_and_check(*document, set, kept);
    CHECK(!removed.outcome.whole);
    CHECK_EQ(removed.difference.value_or(""), std::string());

    // A :has() on the subject with descendants in its argument is turned
    // only by a change inside the element: the changed element and its
    // ancestors are computed again, and nothing else.
    css::StyleSet with_has(std::vector<css::SheetSource> { { "div:has(.on) { color: red }", std::nullopt } });
    Kept has_kept;
    update_and_check(*document, with_has, has_kept);
    toggle_class(*by_id("s2"), "on");
    Step const has = update_and_check(*document, with_has, has_kept);
    CHECK(!has.outcome.whole);
    CHECK_EQ(has.difference.value_or(""), std::string());
    std::size_t ancestors = 0;
    for (dom::Node const* up = by_id("s2"); up && up->is_element(); up = up->parent())
        ++ancestors;
    CHECK(has.outcome.computed < count_elements(*document));
    CHECK(has.outcome.computed >= ancestors);
    toggle_class(*by_id("s2"), "on");
    Step const has_back = update_and_check(*document, with_has, has_kept);
    CHECK(!has_back.outcome.whole);
    CHECK_EQ(has_back.difference.value_or(""), std::string());
    // On an anchor above the subject that its own selectors name: the
    // anchor with the change inside it is computed again, and what it holds.
    css::StyleSet anchored(std::vector<css::SheetSource> { { "div:has(.on) span { color: red }", std::nullopt } });
    Kept anchored_kept;
    update_and_check(*document, anchored, anchored_kept);
    toggle_class(*by_id("s2"), "on");
    Step const anchored_step = update_and_check(*document, anchored, anchored_kept);
    CHECK(!anchored_step.outcome.whole);
    CHECK_EQ(anchored_step.difference.value_or(""), std::string());
    toggle_class(*by_id("s2"), "on");
    Step const anchored_back = update_and_check(*document, anchored, anchored_kept);
    CHECK(!anchored_back.outcome.whole);
    CHECK_EQ(anchored_back.difference.value_or(""), std::string());
    // The same through :not() or :is(): a :has() on the subject of their
    // argument is tested on the anchor too.
    for (std::string_view const sheet : { "div:not(:has(.on)) span { color: red }", "div:is(.x, :has(> .on)) span { color: red }" }) {
        css::StyleSet through(std::vector<css::SheetSource> { { std::string(sheet), std::nullopt } });
        Kept through_kept;
        update_and_check(*document, through, through_kept);
        toggle_class(*by_id("s2"), "on");
        Step const through_step = update_and_check(*document, through, through_kept);
        CHECK(!through_step.outcome.whole);
        CHECK_EQ(through_step.difference.value_or(""), std::string());
        toggle_class(*by_id("s2"), "on");
        Step const through_back = update_and_check(*document, through, through_kept);
        CHECK(!through_back.outcome.whole);
        CHECK_EQ(through_back.difference.value_or(""), std::string());
    }
    // Siblings in the argument or after the anchor, an anchor that is any
    // element, or a :has() inside another selector's argument other than on
    // an anchor's own element: a change reaches further, and the update is
    // whole.
    for (std::string_view const sheet : { "div:has(+ .on) { color: red }", ":has(.on) p { color: red }",
             "div:has(~ p .on) { color: red }", ":not(div:has(.on)) > p { color: red }", "div:has(.on) + p { color: red }",
             ":not(:has(.on)) span { color: red }", "div:not(:has(.on) b) span { color: red }",
             "div:not(:has(.on)) + p { color: red }" }) {
        css::StyleSet wide(std::vector<css::SheetSource> { { std::string(sheet), std::nullopt } });
        Kept wide_kept;
        update_and_check(*document, wide, wide_kept);
        toggle_class(*by_id("s2"), "on");
        Step const reached = update_and_check(*document, wide, wide_kept);
        CHECK(reached.outcome.whole);
        CHECK_EQ(reached.difference.value_or(""), std::string());
    }
}

// A ::first-letter style handed down a chain of first block children,
// moved by changes the chain's own elements are not computed again for:
// the block stops asking, a box in the chain leaves the flow, one becomes
// a flex container. Each update is local and leaves what a whole one does.
void first_letters_handed_down()
{
    auto document = html::parse_document(std::string_view(R"(<!doctype html><html><head></head><body>
<div id=asker class=ask><div id=mid><div id=deep>first</div></div><div id=next>then</div></div>
</body></html>)"));
    auto by_id = [&](std::string_view id) -> dom::Element* {
        std::vector<dom::Node*> pending { document.get() };
        while (!pending.empty()) {
            dom::Node* node = pending.back();
            pending.pop_back();
            if (node->is_element()) {
                dom::Attr const* attribute = static_cast<dom::Element*>(node)->find_attribute("id");
                if (attribute && attribute->value == id)
                    return static_cast<dom::Element*>(node);
            }
            for (dom::Node* child : node->children())
                pending.push_back(child);
        }
        return nullptr;
    };
    css::StyleSet const set(std::vector<css::SheetSource> { { std::string(R"(
.ask::first-letter { color: rgb(200, 0, 0) }
.gone { display: none }
.out { float: left }
.flex { display: flex }
)"), std::nullopt } });
    Kept kept;
    Step const first = update_and_check(*document, set, kept);
    CHECK(first.outcome.whole);
    std::pair<std::string_view, std::string_view> const steps[] = {
        { "asker", "ask" }, { "asker", "ask" }, { "mid", "gone" }, { "mid", "gone" },
        { "mid", "out" }, { "mid", "out" }, { "mid", "flex" }, { "mid", "flex" },
    };
    for (auto const& [id, name] : steps) {
        dom::Element* const element = by_id(id);
        CHECK(element);
        if (!element)
            return;
        toggle_class(*element, name);
        Step const step = update_and_check(*document, set, kept);
        CHECK(!step.outcome.whole);
        CHECK_EQ(step.difference.value_or(""), std::string());
    }
}

// The focus moved from an element to one that holds it: the holder was in
// the old chain already (:focus-within) and is the focused one now, which
// its own :focus and :focus-visible rules read.
void focus_moves_up()
{
    Page page = make_page(200);
    css::StyleSet const set(std::vector<css::SheetSource> { { std::string(state_sheet), std::nullopt } });
    Kept kept;
    update_and_check(*page.document, set, kept);
    dom::Element* group = nullptr;
    dom::Element* leaf = nullptr;
    for (dom::Element* element : page.elements) {
        dom::Attr const* const classes = element->find_attribute("class");
        if (!group && classes && classes->value.find("group") != std::string::npos)
            group = element;
        if (group && !leaf && classes && classes->value == "leaf" && element->parent() && element->parent()->parent() == group)
            leaf = element;
    }
    CHECK(group != nullptr && leaf != nullptr);
    page.document->set_focused(leaf, true);
    Step const inside = update_and_check(*page.document, set, kept);
    CHECK_EQ(inside.difference.value_or(""), std::string());
    CHECK_EQ(kept.styles.at(group).padding_top.value, 0.0f); // not focused itself
    page.document->set_focused(group, true);
    Step const moved = update_and_check(*page.document, set, kept);
    CHECK_EQ(moved.difference.value_or(""), std::string());
    CHECK_EQ(kept.styles.at(group).padding_top.value, 4.0f); // .group:focus-visible
}

// --- Shadow trees ------------------------------------------------------------
//
// The same instrument over a page of components: hosts with a shadow tree
// each — a style element, slots by name and a default one, a nested host
// that hands a slot on — their own children slotted, unslotted and moved
// between slots, and a document sheet that styles hosts and their parts.
// What changes: the classes of hosts, of what is inside the trees and of
// what is slotted, the slot a child asks for, a slot's name, a tree's sheet,
// the children of hosts and of trees, new shadow roots on plain elements.

constexpr std::string_view shadow_page_sheet = R"(
body { color: rgb(10, 20, 30); font-size: 15px }
.card { margin: 1px }
x-card { color: rgb(7, 8, 9) }
.card.on { border: 1px solid rgb(1, 2, 3) }
p { color: rgb(200, 0, 0) }
.inner { background-color: rgb(250, 0, 250) }
.pick { font-weight: bold }
.dark .lit { text-indent: 2px }
x-card::part(head) { letter-spacing: 2px }
.card.on::part(head) { word-spacing: 1px }
.lit + .lit { margin-left: 2px }
)";

constexpr std::string_view shadow_tree_sheet = R"(
:host { display: block; padding: 2px; color: rgb(0, 0, 100) }
:host(.on) { padding: 5px }
:host(.on) .inner { font-style: italic }
:host-context(.dark) .inner { color: rgb(9, 9, 9) }
.inner { margin-left: 1px }
.inner.pick { margin-left: 4px }
p { color: rgb(0, 120, 0) }
::slotted(.lit) { text-decoration: underline }
::slotted(p) { margin-top: 3px }
:host(.on) ::slotted(.pick) { letter-spacing: 1px }
slot[name=head]::slotted(*) { font-size: 20px }
.inner:first-child { padding-top: 1px }
)";

constexpr std::string_view shadow_tree_sheet_other = R"(
:host { display: block; padding: 3px }
.inner { margin-left: 7px; color: rgb(50, 60, 70) }
::slotted(*) { margin-bottom: 2px }
)";

std::size_t count_shadow_including(dom::Node& node)
{
    std::size_t count = 0;
    dom::for_each_shadow_including(node, [&count](dom::Node& inside) { count += inside.is_element() ? 1 : 0; });
    return count;
}

struct ShadowPage {
    std::unique_ptr<dom::Document> document;
    dom::Element* body = nullptr;
    dom::Element* wrapper = nullptr;
    std::vector<dom::Element*> elements; // every element made, light or in a shadow tree
    std::vector<dom::Text*> sheets; // the text of each tree's style element
};

dom::Element* note(ShadowPage& page, dom::Element* element)
{
    page.elements.push_back(element);
    return element;
}

// A shadow tree on `host`: a sheet, a named slot inside a part, a paragraph
// of its own, and the default slot — in a nested host now and then, whose
// own tree hands the slot on.
void give_shadow_tree(ShadowPage& page, dom::Element& host, bool nested)
{
    dom::Document& document = *page.document;
    dom::ShadowRoot& shadow = host.attach_shadow();
    dom::Element* style = note(page, make(document, "style", ""));
    dom::Text* sheet = make_text(document, shadow_tree_sheet);
    page.sheets.push_back(sheet);
    style->append_child(*sheet);
    shadow.append_child(*style);
    dom::Element* head = note(page, make(document, "div", "inner"));
    head->attributes().push_back(dom::Attr { "part", "head", "", "" });
    dom::Element* head_slot = note(page, make(document, "slot", ""));
    head_slot->attributes().push_back(dom::Attr { "name", "head", "", "" });
    head_slot->append_child(*make_text(document, "no head"));
    head->append_child(*head_slot);
    shadow.append_child(*head);
    dom::Element* own = note(page, make(document, "p", "inner"));
    own->append_child(*make_text(document, "shadow text"));
    shadow.append_child(*own);
    dom::Element* rest = note(page, make(document, "div", "inner"));
    dom::Element* rest_slot = note(page, make(document, "slot", ""));
    if (nested) {
        dom::Element* inner_host = note(page, make(document, "x-card", "card"));
        inner_host->append_child(*rest_slot);
        rest->append_child(*inner_host);
        give_shadow_tree(page, *inner_host, false);
    } else {
        rest->append_child(*rest_slot);
    }
    shadow.append_child(*rest);
}

dom::Element* make_light_child(ShadowPage& page, Sequence& random)
{
    dom::Document& document = *page.document;
    std::size_t const pick = random.below(5);
    dom::Element* child = note(page, make(document, pick == 1 ? "p" : pick == 4 ? "b" : "span", pick == 0 || pick == 2 ? "lit" : ""));
    if (pick == 0)
        child->attributes().push_back(dom::Attr { "slot", "head", "", "" });
    if (pick == 4)
        child->attributes().push_back(dom::Attr { "slot", "nowhere", "", "" });
    child->append_child(*make_text(document, "light"));
    return child;
}

dom::Element* make_host(ShadowPage& page, Sequence& random, int index)
{
    dom::Element* host = note(page, make(*page.document, index % 2 == 0 ? "x-card" : "div", "card"));
    give_shadow_tree(page, *host, index % 5 == 2);
    int const children = 1 + static_cast<int>(random.below(4));
    for (int i = 0; i < children; ++i)
        host->append_child(*make_light_child(page, random));
    return host;
}

void shadow_trees(std::uint64_t seed)
{
    ShadowPage page;
    page.document = html::parse_document(std::string_view("<!doctype html><html><head></head><body></body></html>"));
    page.body = find_body(*page.document);
    page.wrapper = note(page, make(*page.document, "section", ""));
    page.body->append_child(*page.wrapper);
    Sequence random { seed };
    for (int index = 0; index < 60; ++index)
        page.wrapper->append_child(*make_host(page, random, index));
    css::StyleSet const set(std::vector<css::SheetSource> { { std::string(shadow_page_sheet), std::nullopt } });
    Kept shell;
    Kept script;
    Step const first = update_and_check(*page.document, set, shell);
    CHECK(first.outcome.whole);
    CHECK_EQ(first.difference.value_or(""), std::string());

    // What the rules say, read off one host: the tree's own rules inside
    // it and nowhere else, the document's on the host and on what is
    // slotted, and each across the boundary where a selector says so.
    {
        dom::Element* host = nullptr;
        for (dom::Element* element : page.elements) {
            if (element->shadow_root() != nullptr && element->parent() == page.wrapper && element->local_name() == "x-card") {
                host = element;
                break;
            }
        }
        CHECK(host != nullptr);
        dom::Element* inner_paragraph = nullptr;
        dom::Element* part = nullptr;
        for (dom::Node* child : host->shadow_root()->children()) {
            if (!child->is_element())
                continue;
            auto* element = static_cast<dom::Element*>(child);
            if (element->is_html("p"))
                inner_paragraph = element;
            if (element->has_attribute("part"))
                part = element;
        }
        CHECK(inner_paragraph != nullptr && part != nullptr);
        css::ComputedStyle const& host_style = shell.styles.at(host);
        CHECK_EQ(host_style.display, css::Display::Block); // :host
        CHECK_EQ(host_style.padding_top.value, 2.0f); // :host
        CHECK_EQ(host_style.margin_top.value, 1.0f); // the document's .card
        // The document's rule for the host wins over the tree's :host, which
        // is the more specific of the two: the outer tree first (css-cascade-5 §6.1).
        CHECK_EQ(host_style.color.r, 7);
        CHECK_EQ(host_style.color.b, 9);
        css::ComputedStyle const& paragraph_style = shell.styles.at(inner_paragraph);
        CHECK_EQ(paragraph_style.color.g, 120); // the tree's p, not the document's
        CHECK_EQ(paragraph_style.color.r, 0);
        CHECK_EQ(paragraph_style.background_color.a, 0); // the document's .inner does not reach in
        CHECK_EQ(shell.styles.at(part).letter_spacing, 2.0f); // x-card::part(head)
        dom::Element* slotted_paragraph = make(*page.document, "p", "");
        page.elements.push_back(slotted_paragraph);
        host->append_child(*slotted_paragraph);
        Step const added = update_and_check(*page.document, set, shell);
        CHECK_EQ(added.difference.value_or(""), std::string());
        css::ComputedStyle const& slotted_style = shell.styles.at(slotted_paragraph);
        CHECK_EQ(slotted_style.color.r, 200); // the document's p wins over what it inherits
        CHECK_EQ(slotted_style.margin_top.value, 3.0f); // ::slotted(p)
        // A child no slot takes is not styled at all.
        dom::Element* stray = make(*page.document, "i", "");
        stray->attributes().push_back(dom::Attr { "slot", "nowhere", "", "" });
        page.elements.push_back(stray);
        host->append_child(*stray);
        Step const strayed = update_and_check(*page.document, set, shell);
        CHECK_EQ(strayed.difference.value_or(""), std::string());
        CHECK(!shell.styles.contains(stray));
        // Until it asks for a slot that is there.
        remove_attribute(*stray, "slot");
        dom::slot_attribute_changed(*stray);
        Step const taken = update_and_check(*page.document, set, shell);
        CHECK_EQ(taken.difference.value_or(""), std::string());
        CHECK(shell.styles.contains(stray));
        // The host's class reaches the tree through :host().
        toggle_class(*host, "on");
        Step const turned = update_and_check(*page.document, set, shell);
        CHECK_EQ(turned.difference.value_or(""), std::string());
        CHECK(!turned.outcome.whole);
        CHECK_EQ(shell.styles.at(host).padding_top.value, 5.0f);
        CHECK(shell.styles.at(inner_paragraph).font_style != css::FontStyle::Normal);
        CHECK_EQ(shell.styles.at(part).word_spacing, 1.0f); // .card.on::part(head)
        CHECK(turned.outcome.computed < 40);
    }

    std::size_t differences = 0;
    std::size_t computed_total = 0;
    std::size_t whole_updates = 0;
    std::string whole_reasons;
    std::string first_difference;
    int const steps = 400;
    for (int step = 0; step < steps; ++step) {
        std::vector<dom::Element*> live;
        std::vector<dom::Element*> hosts;
        std::vector<dom::Element*> slots;
        for (dom::Element* element : page.elements) {
            if (!element->is_connected())
                continue;
            live.push_back(element);
            if (element->shadow_root() != nullptr)
                hosts.push_back(element);
            if (element->is_slot())
                slots.push_back(element);
        }
        dom::Element& target = *live[random.below(live.size())];
        bool const movable = &target != page.wrapper;
        std::size_t const mutation = random.below(11);
        std::string const about = "mutation " + std::to_string(mutation) + " on <" + target.local_name() + ">"
            + (target.shadow_root() != nullptr ? " (a host)" : "") + (target.root().is_shadow_root() ? " in a shadow tree" : "")
            + (target.assigned_slot() != nullptr ? " (slotted)" : "");
        switch (mutation) {
        case 0:
            toggle_class(target, "on");
            break;
        case 1:
            toggle_class(target, random.below(2) ? "pick" : "lit");
            break;
        case 2:
            toggle_class(random.below(3) ? *page.wrapper : target, "dark");
            break;
        case 3: {
            std::size_t const pick = random.below(3);
            if (pick == 0)
                remove_attribute(target, "slot");
            else
                set_attribute(target, "slot", pick == 1 ? "head" : "nowhere");
            dom::slot_attribute_changed(target);
            break;
        }
        case 4:
            if (!hosts.empty() && random.below(3) != 0) {
                dom::Element& host = *hosts[random.below(hosts.size())];
                host.insert_before(*make_light_child(page, random),
                    host.children().empty() ? nullptr : host.children()[random.below(host.children().size())]);
            } else if (!hosts.empty()) {
                dom::Element* inner = note(page, make(*page.document, "p", "inner"));
                inner->append_child(*make_text(*page.document, "more"));
                hosts[random.below(hosts.size())]->shadow_root()->append_child(*inner);
            }
            break;
        case 5:
            if (movable)
                target.remove();
            break;
        case 6:
            if (!slots.empty()) {
                dom::Element& slot = *slots[random.below(slots.size())];
                std::size_t const pick = random.below(3);
                if (pick == 0)
                    remove_attribute(slot, "name");
                else
                    set_attribute(slot, "name", pick == 1 ? "head" : "other");
                dom::slot_name_changed(slot);
            }
            break;
        case 7:
            if (!page.sheets.empty()) {
                dom::Text& sheet = *page.sheets[random.below(page.sheets.size())];
                sheet.data = std::string(sheet.data == shadow_tree_sheet ? shadow_tree_sheet_other : shadow_tree_sheet);
                sheet.mark_style_data();
            }
            break;
        case 8: {
            std::size_t const pick = random.below(3);
            if (pick == 0)
                page.document->set_hovered(&target);
            else if (pick == 1)
                page.document->set_focused(random.below(4) ? &target : nullptr, true);
            else
                page.document->set_active(random.below(2) ? &target : nullptr);
            break;
        }
        case 9:
            if (target.shadow_root() == nullptr && target.is_html() && dom::is_valid_shadow_host_name(target.local_name()))
                give_shadow_tree(page, target, false);
            else
                page.wrapper->append_child(*make_host(page, random, step));
            break;
        case 10:
            // Moved elsewhere, never into itself — through a shadow root either.
            if (movable && live.size() > 1) {
                dom::Element* to = live[random.below(live.size())];
                bool inside = false;
                for (dom::Node const* up = to; up; up = up->parent_or_host())
                    inside = inside || up == &target;
                if (!inside && !to->is_html("style"))
                    to->append_child(target);
            }
            break;
        }
        Step const result = update_and_check(*page.document, set, shell);
        computed_total += result.outcome.computed;
        whole_updates += result.outcome.whole ? 1 : 0;
        if (result.outcome.whole)
            whole_reasons += std::string(whole_reasons.empty() ? "" : "; ") + std::string(result.outcome.reason);
        if (result.difference) {
            ++differences;
            if (first_difference.empty())
                first_difference = "seed " + std::to_string(seed) + " step " + std::to_string(step) + " (" + about + "): " + *result.difference;
        }
        if (step % 7 == 3) {
            Step const other = update_and_check(*page.document, set, script);
            if (other.difference) {
                ++differences;
                if (first_difference.empty())
                    first_difference = "seed " + std::to_string(seed) + " step " + std::to_string(step) + " (second holder, " + about + "): " + *other.difference;
            }
        }
    }
    CHECK_EQ(first_difference, std::string());
    CHECK_EQ(differences, std::size_t(0));
    std::size_t const final_size = count_shadow_including(*page.document);
    std::cout << "  shadow trees: " << steps << " mutations over " << final_size << " elements: " << computed_total
              << " computed in all, " << whole_updates << " whole (" << whole_reasons << ")\n";
    CHECK(computed_total < static_cast<std::size_t>(steps) * final_size / 3);
}

}

int main()
{
    text::FontManager::instance().set_system_fonts(false);
    bounds();
    reaches();
    random_mutations(page_sheet, 0x1234abcdu);
    random_mutations(argument_sheet, 0x9e3779b9u);
    random_mutations(has_sheet, 0x51ed27f3u);
    random_mutations(state_sheet, 0x2545f491u);
    random_mutations(first_letter_sheet, 0x6a09e667u);
    first_letters_handed_down();
    focus_moves_up();
    shadow_trees(0x7f4a7c15u);
    shadow_trees(0x2c1b3c6du);
    shadow_trees(0x297a2d39u);
    // SASHFOLD_RESTYLE_SEEDS=n: n more pages of components, each from a
    // seed of its own — the instrument for a path three seeds never take.
    if (char const* const more = std::getenv("SASHFOLD_RESTYLE_SEEDS")) {
        for (int i = 0; i < std::atoi(more); ++i)
            shadow_trees(0x9e3779b97f4a7c15ull * static_cast<std::uint64_t>(i + 1) + 11);
    }
    return test::report("test_incremental_restyle");
}
