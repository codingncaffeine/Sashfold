#include "Test.h"

#include "dom/Dom.h"
#include "dom/Focus.h"
#include "html/TreeBuilder.h"
#include "ui/Forms.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Focus (HTML §6.6) as dom::focus computes it: which elements are focusable
// areas, which Tab reaches, the order of a document's stops through shadow
// trees and slots, a starting point, radio groups, and the walk in and out
// of the documents of frames.

using namespace sashfold;
namespace focus = sashfold::dom::focus;

namespace {

std::unique_ptr<dom::Document> parse(std::string_view html)
{
    return html::parse_document(html);
}

std::string id_of(dom::Element const* element)
{
    if (!element)
        return "(none)";
    dom::Attr const* const id = element->find_attribute("id");
    return id ? id->value : "?";
}

std::string ids(std::vector<dom::Element const*> const& elements)
{
    std::string out;
    for (dom::Element const* element : elements)
        out += (out.empty() ? "" : " ") + id_of(element);
    return out;
}

// An element by id anywhere, shadow trees included.
dom::Element const* find(dom::Node const& root, std::string_view id)
{
    dom::Element const* found = nullptr;
    dom::for_each_shadow_including(root, [&](dom::Node const& node) {
        if (found || !node.is_element())
            return;
        auto const& element = static_cast<dom::Element const&>(node);
        if (dom::Attr const* const attribute = element.find_attribute("id"); attribute && attribute->value == id)
            found = &element;
    });
    return found;
}

// Tab from the start, all the way: the ids of the stops in order.
std::string walk(dom::Document const& document, focus::Environment const& env, bool backward = false)
{
    std::string out;
    dom::Node const* at = nullptr;
    for (int guard = 0; guard < 64; ++guard) {
        dom::Element const* const next = focus::sequential_next(document, at, backward, env);
        if (!next)
            break;
        out += (out.empty() ? "" : " ") + id_of(next);
        at = next;
    }
    return out;
}

} // namespace

int main()
{
    focus::Environment const plain;

    // --- The tabindex attribute, by the rules for parsing integers ----------
    {
        auto const document = parse(R"(<div id=a tabindex=""></div><div id=b tabindex="x1"></div>
<div id=c tabindex=" 3px"></div><div id=d tabindex="-1"></div><div id=e tabindex="+2"></div><div id=f></div>
<div id=g tabindex="007"></div><div id=h tabindex="99999999999999999999"></div>)");
        CHECK(!focus::tabindex_value(*find(*document, "a")));
        CHECK(!focus::tabindex_value(*find(*document, "b")));
        CHECK_EQ(focus::tabindex_value(*find(*document, "c")).value_or(-99), 3L);
        CHECK_EQ(focus::tabindex_value(*find(*document, "d")).value_or(-99), -1L);
        CHECK_EQ(focus::tabindex_value(*find(*document, "e")).value_or(-99), 2L);
        CHECK(!focus::tabindex_value(*find(*document, "f")));
        CHECK_EQ(focus::tabindex_value(*find(*document, "g")).value_or(-99), 7L);
        CHECK(focus::tabindex_value(*find(*document, "h")).value_or(-99) > 0);
    }

    // --- Positive values first, ascending, ties in tree order; then zero --
    {
        auto const document = parse(R"(<form><button id=btn0>0</button><button id=btn1 tabindex="">1</button>
<button id=btn2 tabindex="a">2</button><button id=btn3 tabindex="-1">3</button><button id=btn4 tabindex="0">4</button>
<button id=btn5 tabindex="3">5</button><button id=btn6 tabindex="2">6</button><button id=btn7 tabindex="2">7</button>
<button id=btn8 tabindex="2">8</button><button id=btn9 tabindex="1">9</button></form>)");
        CHECK_EQ(ids(focus::navigation_order(*document, plain)), std::string("btn9 btn6 btn7 btn8 btn5 btn0 btn1 btn2 btn4"));
        CHECK_EQ(walk(*document, plain, true), std::string("btn4 btn2 btn1 btn0 btn5 btn8 btn7 btn6 btn9"));
        // tabindex=-1: focusable, not a stop.
        dom::Element const& minus = *find(*document, "btn3");
        CHECK(focus::is_focusable_area(minus, plain));
        CHECK(!focus::is_sequentially_focusable(minus, plain));
    }

    // --- What is a focusable area -------------------------------------------
    {
        auto const document = parse(R"(<a id=nohref>x</a><a id=link href=#>x</a><input id=hidden type=hidden>
<input id=field><button id=off disabled>x</button><div id=plain>x</div><div id=ti tabindex=0>x</div>
<fieldset disabled><legend><input id=inlegend></legend><input id=infieldset><legend><input id=secondlegend></legend></fieldset>
<details id=closed><summary id=s1>a</summary><summary id=s2>b</summary><input id=folded></details>
<details open><summary id=s3>a</summary><input id=unfolded></details>
<div hidden><input id=underhidden></div><div contenteditable id=host><p id=inside contenteditable>x</p></div>
<video id=withcontrols controls></video><video id=without></video><iframe id=frame></iframe>
<button id=offtab disabled tabindex=0>x</button><svg><a id=svglink href=#><text>x</text></a><g id=svgg tabindex=0></g></svg>)");
        auto const focusable = [&](std::string_view id) { return focus::is_focusable_area(*find(*document, id), plain); };
        CHECK(!focusable("nohref"));
        CHECK(focusable("link"));
        CHECK(!focusable("hidden"));
        CHECK(focusable("field"));
        CHECK(!focusable("off"));
        CHECK(!focusable("offtab")); // disabled wins over tabindex
        CHECK(!focusable("plain"));
        CHECK(focusable("ti"));
        CHECK(focusable("inlegend")); // the first legend of a disabled fieldset is not disabled by it
        CHECK(!focusable("infieldset"));
        CHECK(!focusable("secondlegend"));
        CHECK(focusable("s1"));
        CHECK(!focusable("s2")); // not the first summary
        CHECK(!focusable("folded")); // inside a closed details
        CHECK(focusable("s3"));
        CHECK(focusable("unfolded"));
        CHECK(!focusable("underhidden"));
        CHECK(focusable("host"));
        CHECK(!focusable("inside")); // editable, but its parent is too: no host
        CHECK(focusable("withcontrols"));
        CHECK(!focusable("without"));
        CHECK(focusable("frame"));
        CHECK(focusable("svglink"));
        CHECK(focusable("svgg"));
    }

    // --- Style and layout's say: display, visibility, inert, scrolling -------
    {
        auto const document = parse(R"(<div id=none><a id=a1 href=#>x</a></div><a id=a2 href=#>x</a>
<a id=a3 href=#>x</a><a id=a4 href=#>x</a><div id=scroller><p>text</p></div><div id=scroller2><a id=a5 href=#>x</a></div>
<span id=contents tabindex=0><a id=a6 href=#>x</a></span>)");
        std::map<std::string, focus::ElementFacts> facts;
        facts["none"].display_none = true;
        facts["a2"].visible = false;
        facts["a3"].inert = true;
        facts["scroller"].scrollable = true;
        facts["scroller2"].scrollable = true;
        facts["contents"].display_contents = true;
        focus::Environment env;
        env.facts = [&](dom::Element const& element) {
            auto const it = facts.find(id_of(&element));
            return it == facts.end() ? focus::ElementFacts {} : it->second;
        };
        // A scroller with nothing focusable inside is a stop; one holding a
        // link is not (the link is); display: contents has no box.
        CHECK_EQ(ids(focus::navigation_order(*document, env)), std::string("a4 scroller a5 a6"));
    }

    // --- Shadow trees and slots: scopes ordered on their own ---------------
    {
        // The host's scope: slots (tabindex 1) before the shadow tree's own
        // elements (2); the slotted elements inside the slots' scopes.
        auto const document = parse(R"(<div id=aboveHost tabindex=3>a</div><div id=host tabindex=3><template shadowrootmode=open>
<div id=aboveSlot tabindex=2>x</div><slot id=slotAbove name=above tabindex=1></slot><slot id=slotBelow name=below tabindex=1></slot>
<div id=belowSlot tabindex=2>y</div></template><div id=slottedBelow slot=below tabindex=4>b</div><div id=slottedAbove slot=above tabindex=4>c</div></div>
<div id=belowHost tabindex=3>z</div>)");
        CHECK_EQ(walk(*document, plain), std::string("aboveHost host slottedAbove slottedBelow aboveSlot belowSlot belowHost"));
        CHECK_EQ(walk(*document, plain, true), std::string("belowHost belowSlot aboveSlot slottedBelow slottedAbove host aboveHost"));
    }
    {
        // A host with tabindex=-1 takes its whole scope out of the order.
        auto const document = parse(R"(<div id=a tabindex=0>a</div><div id=host tabindex=-1><template shadowrootmode=open>
<div id=in tabindex=0>x</div></template></div><div id=b tabindex=0>b</div>)");
        CHECK_EQ(walk(*document, plain), std::string("a b"));
    }
    {
        // A host that delegates focus is no stop itself, and gives a press
        // or focus() to its first focusable area; a host without tabindex
        // counts as zero, its tree kept in place.
        auto const document = parse(R"(<div id=a tabindex=0>a</div><div id=host tabindex=0><template shadowrootmode=open shadowrootdelegatesfocus>
<span id=label>label</span><input id=inner></template></div><div id=plainhost><template shadowrootmode=open><input id=inner2></template></div>
<div id=b tabindex=0>b</div>)");
        CHECK_EQ(walk(*document, plain), std::string("a inner inner2 b"));
        dom::Element const& host = *find(*document, "host");
        CHECK_EQ(id_of(focus::focus_delegate(host, plain)), std::string("inner"));
        CHECK_EQ(id_of(focus::click_focus_target(*find(*document, "label"), plain)), std::string("inner"));
        CHECK_EQ(id_of(focus::click_focus_target(*find(*document, "a")->children().front(), plain)), std::string("a"));
        CHECK(focus::click_focus_target(*find(*document, "plainhost"), plain) == nullptr);
    }

    // --- Radio groups are one stop -----------------------------------------
    {
        auto const document = parse(R"(<input id=before><input type=radio name=g id=r1><input type=radio name=g id=r2>
<input type=radio name=g id=r3><input id=after>)");
        // None checked: the first going forward, the last going back.
        CHECK_EQ(walk(*document, plain), std::string("before r1 after"));
        CHECK_EQ(walk(*document, plain, true), std::string("after r3 before"));
    }
    {
        auto const document = parse(R"(<input id=before><input type=radio name=g id=r1><input type=radio name=g id=r2 checked>
<input type=radio name=g id=r3><input type=radio name=other id=o1><input id=after>)");
        CHECK_EQ(walk(*document, plain), std::string("before r2 o1 after"));
        CHECK_EQ(walk(*document, plain, true), std::string("after o1 r2 before"));
    }

    // --- A starting point that is no stop -----------------------------------
    {
        auto const document = parse(R"(<a id=one href=#>1</a><p id=para>words</p><a id=two href=#>2</a><a id=three href=#>3</a>)");
        dom::Node const& text = *find(*document, "para")->children().front();
        CHECK_EQ(id_of(focus::next_in_document(*document, &text, false, plain)), std::string("two"));
        CHECK_EQ(id_of(focus::next_in_document(*document, &text, true, plain)), std::string("one"));
        CHECK_EQ(id_of(focus::next_in_document(*document, find(*document, "three"), false, plain)), std::string("(none)"));
    }

    // --- Frames: in at the edge, out after the container --------------------
    {
        auto const top = parse(R"(<a id=t1 href=#>1</a><iframe id=f1></iframe><a id=t2 href=#>2</a><iframe id=empty></iframe><a id=t3 href=#>3</a>)");
        auto const middle = parse(R"(<a id=m1 href=#>m</a><iframe id=f2></iframe><a id=m2 href=#>n</a>)");
        auto const inner = parse(R"(<input id=i1><input id=i2>)");
        auto const nothing = parse(R"(<p>no stops</p>)");
        dom::Element const* const f1 = find(*top, "f1");
        dom::Element const* const f2 = find(*middle, "f2");
        dom::Element const* const empty = find(*top, "empty");
        focus::Environment env;
        env.content_document = [&](dom::Element const& frame) -> dom::Document const* {
            if (&frame == f1)
                return middle.get();
            if (&frame == f2)
                return inner.get();
            if (&frame == empty)
                return nothing.get();
            return nullptr;
        };
        env.container_of = [&](dom::Document const& document) -> dom::Element const* {
            if (&document == middle.get())
                return f1;
            if (&document == inner.get())
                return f2;
            if (&document == nothing.get())
                return empty;
            return nullptr;
        };
        // Nested frames entered and left both ways; a frame with nothing to
        // stop on is a stop itself.
        CHECK_EQ(walk(*top, env), std::string("t1 m1 i1 i2 m2 t2 empty t3"));
        CHECK_EQ(walk(*top, env, true), std::string("t3 empty t2 m2 i2 i1 m1 t1"));
    }

    return test::report("focus");
}
