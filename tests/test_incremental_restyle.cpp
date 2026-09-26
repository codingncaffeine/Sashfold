#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "text/FontManager.h"

#include <cstddef>
#include <cstdint>
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

void random_mutations()
{
    Page page = make_page(3000);
    std::size_t const size = count_elements(*page.document);
    CHECK(size >= 3000);
    auto set = std::make_unique<css::StyleSet>(std::vector<css::SheetSource> { { std::string(page_sheet), std::nullopt } });
    Kept shell;
    Kept script;
    Step first = update_and_check(*page.document, *set, shell);
    CHECK(first.outcome.whole);
    CHECK_EQ(first.outcome.computed, size);
    CHECK_EQ(first.difference.value_or(""), std::string());

    Sequence random { 0x1234abcdu };
    std::size_t differences = 0;
    std::size_t computed_total = 0;
    std::size_t whole_updates = 0;
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
        case 8:
            // Interactive state: nothing in the engine matches :hover or
            // :focus yet, so the state a pointer or focus change would set
            // stands in as an attribute selectors can see.
            if (random.below(2))
                set_attribute(target, "data-k", std::to_string(random.below(3)));
            else
                remove_attribute(target, "data-k");
            break;
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
                { std::string(page_sheet), std::nullopt }, { std::string(extra_sheet), std::nullopt } });
            sheet_step = true;
        }
        Step const result = update_and_check(*page.document, *set, shell);
        computed_total += result.outcome.computed;
        whole_updates += result.outcome.whole ? 1 : 0;
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
              << " computed in all, " << whole_updates << " whole\n";
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

    // The root's class: the root and everything in it.
    toggle_class(static_cast<dom::Element&>(*page.body->parent()), "wide");
    Step const root = update_and_check(*page.document, set, kept);
    CHECK(!root.outcome.whole);
    CHECK_EQ(root.outcome.computed, size);
    CHECK_EQ(root.difference.value_or(""), std::string());

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

    // :has() can reach anything: every update is whole.
    css::StyleSet with_has(std::vector<css::SheetSource> { { "div:has(.on) { color: red }", std::nullopt } });
    Kept has_kept;
    update_and_check(*document, with_has, has_kept);
    toggle_class(*by_id("s2"), "on");
    Step const has = update_and_check(*document, with_has, has_kept);
    CHECK(has.outcome.whole);
    CHECK_EQ(has.difference.value_or(""), std::string());
}

}

int main()
{
    text::FontManager::instance().set_system_fonts(false);
    bounds();
    reaches();
    random_mutations();
    return test::report("test_incremental_restyle");
}
