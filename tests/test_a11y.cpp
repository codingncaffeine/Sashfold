#include "Test.h"

#include "a11y/PageSources.h"
#include "a11y/Semantics.h"
#include "a11y/Tree.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The accessibility model where the Web Platform Tests cannot see it: which
// nodes the tree includes and in what order, the table and list numbers,
// the events an update posts for each kind of change, the stable ids, the
// thread boundary, and that a page nobody inspects never makes a tree.

using namespace sashfold;

namespace {

// A page parsed, styled and laid out; refresh() does both again after a
// change to the tree, as a host does before it draws.
struct Page {
    std::unique_ptr<dom::Document> document;
    css::StyleMap styles;
    layout::LayoutResult layout;
    std::unique_ptr<a11y::StyleMapSource> source;

    explicit Page(std::string_view html)
    {
        document = html::parse_document(html);
        styles = css::resolve_styles(*document);
        layout = layout::layout_document(*document, styles, 800);
        source = std::make_unique<a11y::StyleMapSource>(styles, &layout.root);
    }
    void refresh()
    {
        styles = css::resolve_styles(*document);
        layout = layout::layout_document(*document, styles, 800);
        source->set(styles, &layout.root);
    }
    dom::Element* by_id(std::string_view id) const
    {
        dom::Element* found = nullptr;
        dom::for_each_shadow_including(static_cast<dom::Node&>(*document), [&](dom::Node& node) {
            if (!found && node.is_element()) {
                if (dom::Attr const* attr = static_cast<dom::Element&>(node).find_attribute("id"); attr && attr->value == id)
                    found = &static_cast<dom::Element&>(node);
            }
        });
        return found;
    }
};

void set_attribute(dom::Element& element, std::string_view name, std::string_view value)
{
    for (dom::Attr& attr : element.attributes()) {
        if (attr.local_name == name) {
            attr.value = std::string(value);
            return;
        }
    }
    element.attributes().push_back(dom::Attr { std::string(name), std::string(value), "", "", false });
}

struct Recorder final : a11y::Observer {
    std::vector<a11y::Event> events;
    int bounds = 0; // kept apart: what moves on the page is layout's business
    void on_event(a11y::Event const& event) override
    {
        if (event.type == a11y::Event::Type::BoundsChanged)
            ++bounds;
        else
            events.push_back(event);
    }
    // The events of one type, named for a readable failure.
    std::string list(a11y::Tree& tree) const
    {
        std::string out;
        for (a11y::Event const& event : events) {
            out += std::string(a11y::event_name(event.type));
            if (a11y::NodeData const* node = tree.node(event.node))
                out += ":" + node->name;
            if (event.type == a11y::Event::Type::StateChanged)
                out += ":" + std::string(a11y::state_name(event.state)) + (event.on ? "+" : "-");
            if (!event.text.empty())
                out += ":'" + event.text + "'@" + std::to_string(event.offset);
            out += ";";
        }
        return out;
    }
    bool has(a11y::Event::Type type) const
    {
        for (a11y::Event const& event : events) {
            if (event.type == type)
                return true;
        }
        return false;
    }
};

void zero_cost()
{
    // Parsing, styling and laying out a page makes no tree, nor does
    // anything else here before a test asks for one.
    std::uint64_t const before = a11y::Tree::instances();
    Page page("<!doctype html><h1>Title</h1><button>Go</button>");
    CHECK_EQ(a11y::Tree::instances(), before);
    a11y::Tree tree(*page.document, page.source.get());
    CHECK_EQ(a11y::Tree::instances(), before + 1);
}

void inclusion_and_order()
{
    Page page(R"(<!doctype html><title>T</title>
<div><div><span>one</span> <b>two</b></div></div>
<div style="display:none"><button>hidden</button></div>
<div aria-hidden="true">gone</div>
<div style="visibility:hidden">x<span style="visibility:visible">back</span></div>
<ul role="none"><li>item</li></ul>
<div role="presentation"><a href="#">link</a></div>
<button><span>in</span> <img alt="pic"></button>
<p id="owner" aria-owns="moved">para</p><span id="moved">moved</span>
<input type="text" value="field">
<slot>slotless</slot>)");
    a11y::Tree tree(*page.document, page.source.get());
    std::string const dump = tree.dump(false);
    std::string const expected = R"(RootWebArea "T" focusable focused
  StaticText "one"
  StaticText " "
  StaticText "two"
  StaticText "back"
  StaticText "item"
  link "link" focusable linked url="#"
    StaticText "link"
  button "in pic" focusable
    StaticText "in"
    StaticText " "
  paragraph owns=generic
    StaticText "para"
    generic owned-by=paragraph
      StaticText "moved"
  StaticText " "
  textbox focusable editable valuetext="field"
  StaticText " "
  StaticText "slotless"
)";
    CHECK_EQ(dump, expected);
}

void tables_and_lists()
{
    Page page(R"(<!doctype html>
<table id="t"><tr><th>A</th><th>B</th><th>C</th></tr>
<tr><td id="span" rowspan="2" colspan="2">x</td><td id="c">y</td></tr>
<tr><td id="after">z</td></tr></table>
<div role="grid" aria-rowcount="100"><div role="row" aria-rowindex="7"><div role="gridcell" id="g">q</div></div></div>
<ol><li id="l1">a</li><li id="l2">b<ul><li id="nested">c</li></ul></li></ol>
<div role="listbox"><div role="option" id="o1">x</div><div role="option" id="o2" aria-posinset="5" aria-setsize="9">y</div></div>
<form><input type="radio" name="r" id="r1"><input type="radio" name="r" id="r2"><input type="radio" name="r" id="r3"></form>
<h3 id="h3">x</h3><div role="heading" id="h0">x</div><div role="heading" aria-level="5" id="h5">x</div>)");
    a11y::Tree tree(*page.document, page.source.get());
    auto const node = [&](std::string_view id) { return tree.node(tree.id_of(*page.by_id(id))); };
    a11y::NodeData const* table = node("t");
    CHECK(table);
    CHECK_EQ(table->row_count, 3);
    CHECK_EQ(table->column_count, 3);
    CHECK_EQ(node("span")->row_index, 1);
    CHECK_EQ(node("span")->column_index, 0);
    CHECK_EQ(node("span")->column_span, 2);
    CHECK_EQ(node("span")->row_span, 2);
    CHECK_EQ(node("c")->column_index, 2);
    // The cell below a two-row span starts past it.
    CHECK_EQ(node("after")->row_index, 2);
    CHECK_EQ(node("after")->column_index, 2);
    // Its column header is C.
    CHECK_EQ(node("after")->header_cells.size(), std::size_t { 1 });
    CHECK_EQ(tree.node(node("after")->header_cells[0])->name, std::string("C"));
    // The author's counts and indices win.
    a11y::NodeData const* grid_cell = node("g");
    CHECK_EQ(grid_cell->row_index, 6);
    CHECK_EQ(tree.node(tree.node(grid_cell->parent)->parent)->row_count, 100);
    CHECK_EQ(node("l1")->pos_in_set, 1);
    CHECK_EQ(node("l2")->pos_in_set, 2);
    CHECK_EQ(node("l2")->set_size, 2);
    CHECK_EQ(node("l1")->level, 1);
    CHECK_EQ(node("nested")->level, 2);
    CHECK_EQ(node("nested")->set_size, 1);
    CHECK_EQ(node("o1")->pos_in_set, 1);
    CHECK_EQ(node("o1")->set_size, 2);
    CHECK_EQ(node("o2")->pos_in_set, 5);
    CHECK_EQ(node("o2")->set_size, 9);
    CHECK_EQ(node("r2")->pos_in_set, 2);
    CHECK_EQ(node("r3")->set_size, 3);
    CHECK_EQ(node("h3")->level, 3);
    CHECK_EQ(node("h0")->level, 2);
    CHECK_EQ(node("h5")->level, 5);
}

void events()
{
    Page page(R"(<!doctype html>
<div id="box"><button id="b" aria-label="Save">s</button></div>
<div role="checkbox" id="c" aria-checked="false" tabindex="0">Agree</div>
<div aria-live="polite" id="live"><span id="text">old</span></div>
<input id="field" value="ab">)");
    a11y::Tree tree(*page.document, page.source.get());
    Recorder recorder;
    tree.set_observer(&recorder);
    a11y::NodeId const button = tree.id_of(*page.by_id("b"));
    CHECK(button != 0);
    CHECK(recorder.events.empty());

    // An attribute: the name changes.
    set_attribute(*page.by_id("b"), "aria-label", "Store");
    page.refresh();
    tree.update();
    CHECK_EQ(recorder.list(tree), std::string("name-changed:Store;"));
    // The id stayed.
    CHECK_EQ(tree.id_of(*page.by_id("b")), button);

    // A state.
    recorder.events.clear();
    set_attribute(*page.by_id("c"), "aria-checked", "true");
    page.refresh();
    tree.update();
    CHECK_EQ(recorder.list(tree), std::string("state-changed:Agree:checked+;"));

    // Text in a live region: removed, inserted, and announced politely.
    recorder.events.clear();
    auto& text = static_cast<dom::Text&>(*page.by_id("text")->children().front());
    text.data = "new";
    text.mark_style_data();
    CHECK(tree.stale());
    page.refresh();
    tree.update();
    CHECK_EQ(recorder.list(tree), std::string("text-removed:new:'old'@0;text-inserted:new:'new'@0;announcement::'new'@0;"));
    CHECK_EQ(recorder.events.back().politeness, std::string("polite"));

    // A child appended: the parent's children changed, and the live region
    // announces the addition.
    recorder.events.clear();
    dom::Element* added = page.document->create<dom::Element>(std::string(dom::ns::html), "button");
    auto* label = page.document->create<dom::Text>();
    label->data = "More";
    added->append_child(*label);
    page.by_id("live")->append_child(*added);
    page.refresh();
    tree.update();
    CHECK(recorder.has(a11y::Event::Type::ChildrenChanged));
    CHECK_EQ(recorder.list(tree), std::string("children-changed:;announcement::'More'@0;"));

    // Focus.
    recorder.events.clear();
    page.document->set_focused(page.by_id("b"), true);
    CHECK(tree.stale());
    page.refresh();
    tree.update();
    CHECK_EQ(recorder.list(tree), std::string("state-changed:Store:focused+;focus:Store;"));

    // Typing into a field: its value is the host's.
    recorder.events.clear();
    std::string value = "abc";
    page.document->live_value = [&](dom::Element const& element) -> std::optional<std::string> {
        if (&element == page.by_id("field"))
            return value;
        return std::nullopt;
    };
    page.by_id("field")->mark_style_self();
    page.refresh();
    tree.update();
    CHECK_EQ(recorder.list(tree), std::string("value-changed:;text-inserted::'c'@2;"));

    // Removal: the parent's children changed, and the node is gone.
    recorder.events.clear();
    page.by_id("b")->remove();
    page.refresh();
    tree.update();
    CHECK_EQ(recorder.list(tree).substr(0, 17), std::string("children-changed:"));
    CHECK(tree.node(button) == nullptr);
    // Put back, it is a new node with a new id: ids are never reused.
    page.by_id("box")->append_child(*page.document->create<dom::Element>(std::string(dom::ns::html), "hr"));
    page.refresh();
    tree.update();
    a11y::NodeId const separator = tree.id_of(*page.by_id("box")->children().back());
    CHECK(separator > button);

    // Nothing changed: no events, nothing recomputed.
    recorder.events.clear();
    CHECK(!tree.stale());
    tree.update();
    CHECK(recorder.events.empty());
}

void stable_ids()
{
    Page page("<!doctype html><p id=a>one</p><p id=b>two</p>");
    a11y::Tree tree(*page.document, page.source.get());
    a11y::NodeId const a = tree.id_of(*page.by_id("a"));
    a11y::NodeId const b = tree.id_of(*page.by_id("b"));
    CHECK(a != 0 && b != 0 && a != b);
    // Inserting before them moves them, and leaves their ids.
    dom::Element* first = page.document->create<dom::Element>(std::string(dom::ns::html), "p");
    auto* text = page.document->create<dom::Text>();
    text->data = "zero";
    first->append_child(*text);
    page.by_id("a")->parent()->insert_before(*first, page.by_id("a"));
    page.refresh();
    CHECK_EQ(tree.id_of(*page.by_id("a")), a);
    CHECK_EQ(tree.id_of(*page.by_id("b")), b);
}

void thread_boundary()
{
    Page page(R"(<!doctype html><div aria-live="assertive" id="live"><span id="t">0</span></div><button id="go">go</button>)");
    a11y::Tree tree(*page.document, page.source.get());
    std::atomic<int> wakes { 0 };
    a11y::Bridge bridge(tree, [&] { wakes.fetch_add(1); });
    tree.update();
    std::atomic<bool> done { false };
    std::atomic<int> seen { 0 };
    std::atomic<bool> query_ran_on_page { false };
    std::thread::id const page_thread = std::this_thread::get_id();
    std::atomic<int> page_wakes { 0 };
    tree.set_page_wake([&] { page_wakes.fetch_add(1); });
    // The bridge's thread: reads snapshots, takes events and posts a query,
    // never touching the page.
    std::thread reader([&] {
        bool posted = false;
        while (!done.load()) {
            std::shared_ptr<a11y::Snapshot const> const snapshot = bridge.snapshot();
            if (snapshot && snapshot->find(snapshot->root))
                (void)snapshot->find(snapshot->root)->children.size();
            for (a11y::Event const& event : bridge.take_events()) {
                if (event.type == a11y::Event::Type::Announcement)
                    seen.fetch_add(1);
            }
            if (!posted) {
                bridge.post([&](a11y::Tree&) { query_ran_on_page = std::this_thread::get_id() == page_thread; });
                posted = true;
            }
            std::this_thread::yield();
        }
    });
    auto& text = static_cast<dom::Text&>(*page.by_id("t")->children().front());
    for (int i = 1; i <= 20; ++i) {
        text.data = std::to_string(i);
        text.mark_style_data();
        page.refresh();
        tree.update();
        tree.serve();
    }
    // Wait (bounded) for the reader to take the last ones.
    for (int spin = 0; spin < 2000 && seen.load() < 20; ++spin)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tree.serve();
    done = true;
    reader.join();
    CHECK_EQ(seen.load(), 20);
    CHECK(wakes.load() >= 20);
    CHECK(page_wakes.load() >= 1);
    CHECK(query_ran_on_page.load());
    // A snapshot keeps the nodes that did not change: the button's data is
    // the same object from one update to the next.
    std::shared_ptr<a11y::Snapshot const> const before = tree.snapshot();
    text.data = "x";
    text.mark_style_data();
    page.refresh();
    tree.update();
    std::shared_ptr<a11y::Snapshot const> const after = tree.snapshot();
    a11y::NodeId const go = tree.id_of(*page.by_id("go"));
    CHECK(before->nodes.at(go).get() == after->nodes.at(go).get());
}

void hit_test_and_actions()
{
    Page page(R"(<!doctype html><body style="margin:0"><button id="b" style="width:100px;height:40px">Press</button>
<details id="d"><summary id="s">More</summary>x</details><a id="a" href="#x">go</a>)");
    a11y::Tree tree(*page.document, page.source.get());
    a11y::NodeId const button = tree.id_of(*page.by_id("b"));
    a11y::NodeId const hit = tree.hit_test(50, 20);
    // The text leaf inside the button, or the button itself.
    CHECK(hit == button || tree.node(hit)->parent == button);
    CHECK(tree.node(button)->bounds.has_value());
    struct Sink final : a11y::ActionSink {
        std::vector<std::string> done;
        void click(dom::Element& element) override { done.push_back("click " + element.local_name()); }
        void focus(dom::Element& element) override { done.push_back("focus " + element.local_name()); }
    } sink;
    tree.set_action_sink(&sink);
    CHECK(tree.do_action(button, "press"));
    CHECK(tree.do_action(tree.id_of(*page.by_id("s")), "expand"));
    CHECK(tree.do_action(tree.id_of(*page.by_id("a")), "jump"));
    CHECK(tree.focus(tree.id_of(*page.by_id("a"))));
    CHECK(!tree.do_action(button, "jump"));
    CHECK_EQ(sink.done.size(), std::size_t { 4 });
    CHECK_EQ(sink.done[0], std::string("click button"));
    CHECK_EQ(sink.done[1], std::string("click summary"));
    CHECK_EQ(sink.done[3], std::string("focus a"));
    // A text leaf's character boxes.
    a11y::NodeData const* link = tree.node(tree.id_of(*page.by_id("a")));
    CHECK(!link->children.empty());
    std::vector<a11y::Rect> const boxes = tree.text_bounds(link->children.front(), 0, 2);
    CHECK(!boxes.empty());
    CHECK(!tree.inline_text_boxes(link->children.front()).empty());
}

}

int main()
{
    zero_cost();
    inclusion_and_order();
    tables_and_lists();
    events();
    stable_ids();
    thread_boundary();
    hit_test_and_actions();
    return test::report("test_a11y");
}
