#include "Test.h"

#include "css/Selector.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"

#include <memory>
#include <string>
#include <vector>

using namespace sashfold;
using css::Specificity;

namespace {

std::unique_ptr<dom::Document> g_document;

// Finds the first element whose id attribute equals `id`, depth-first.
dom::Element* find_by_id(dom::Node& node, std::string_view id)
{
    if (node.is_element()) {
        auto& element = static_cast<dom::Element&>(node);
        if (dom::Attr const* attribute = element.find_attribute("id"); attribute
            && attribute->value == id)
            return &element;
    }
    for (dom::Node* child : node.children()) {
        if (dom::Element* found = find_by_id(*child, id))
            return found;
    }
    return nullptr;
}

// Parses `selector_text` as a selector list via a synthetic rule prelude.
std::optional<css::SelectorList> parse_selectors(std::string const& selector_text)
{
    auto sheet = css::parse_stylesheet(selector_text + "{}");
    if (sheet.rules.size() != 1 || !sheet.rules[0].is_qualified())
        return std::nullopt;
    return css::parse_selector_list(sheet.rules[0].qualified().prelude);
}

// True when `selector_text` parses and matches the element with the given id.
bool hits(std::string const& selector_text, std::string_view id)
{
    auto list = parse_selectors(selector_text);
    if (!list)
        return false;
    dom::Element* element = find_by_id(*g_document, id);
    if (!element)
        return false;
    return css::matches(*list, *element);
}

bool valid(std::string const& selector_text)
{
    return parse_selectors(selector_text).has_value();
}

Specificity spec_of(std::string const& selector_text)
{
    auto list = parse_selectors(selector_text);
    if (!list || list->selectors.empty())
        return {};
    return list->selectors[0].specificity;
}

} // namespace

int main()
{
    g_document = html::parse_document(std::string_view(R"(
<!doctype html>
<html id="root"><head></head><body id="body">
  <div id="wrap" class="outer main-area" data-role="Container" lang="en-US">
    <p id="p1" class="intro">first</p>
    <p id="p2">second <a id="link" href="/x">go</a> <a id="anchor">no href</a></p>
    <p id="p3" class="intro outro">third</p>
    <ul id="list">
      <li id="li1" class="odd">one</li>
      <li id="li2">two</li>
      <li id="li3" class="odd">three</li>
      <li id="li4">four</li>
      <li id="li5" class="odd">five</li>
    </ul>
    <span id="empty"></span>
  </div>
</body></html>)"));

    // --- Simple selectors ----------------------------------------------------
    CHECK(hits("p", "p1"));
    CHECK(hits("P", "p1")); // type is case-insensitive for HTML elements
    CHECK(!hits("div", "p1"));
    CHECK(hits("*", "p1"));
    CHECK(hits(".intro", "p1"));
    CHECK(hits(".intro.outro", "p3"));
    CHECK(!hits(".intro.outro", "p1"));
    CHECK(!hits(".Intro", "p1")); // class is case-sensitive in no-quirks
    CHECK(hits("#wrap", "wrap"));
    CHECK(hits("div#wrap.outer", "wrap"));

    // --- Attribute selectors -------------------------------------------------
    CHECK(hits("[data-role]", "wrap"));
    CHECK(hits("[data-role=Container]", "wrap"));
    CHECK(!hits("[data-role=container]", "wrap"));
    CHECK(hits("[data-role=container i]", "wrap"));
    CHECK(hits("[class~=outer]", "wrap"));
    CHECK(!hits("[class~=out]", "wrap"));
    CHECK(hits("[lang|=en]", "wrap"));
    CHECK(hits("[data-role^=Cont]", "wrap"));
    CHECK(hits("[data-role$=ainer]", "wrap"));
    CHECK(hits("[data-role*=tain]", "wrap"));
    CHECK(hits("[class*=main-a]", "wrap"));

    // --- Combinators ---------------------------------------------------------
    CHECK(hits("div p", "p1"));
    CHECK(hits("body p", "p1"));
    CHECK(hits("div > p", "p1"));
    CHECK(!hits("body > p", "p1"));
    CHECK(hits("p + p", "p2"));
    CHECK(!hits("p + p", "p1"));
    CHECK(hits("p ~ ul", "list"));
    CHECK(hits("#p1 ~ #p3", "p3"));
    CHECK(!hits("#p3 ~ #p1", "p1"));

    // --- Structural pseudo-classes -------------------------------------------
    CHECK(hits(":root", "root"));
    CHECK(!hits(":root", "body"));
    CHECK(hits("p:first-child", "p1"));
    CHECK(!hits("p:first-child", "p2"));
    CHECK(hits("li:last-child", "li5"));
    CHECK(hits("span:empty", "empty"));
    CHECK(!hits("p:empty", "p1"));
    CHECK(hits("li:nth-child(2)", "li2"));
    CHECK(hits("li:nth-child(odd)", "li3"));
    CHECK(!hits("li:nth-child(odd)", "li2"));
    CHECK(hits("li:nth-child(2n)", "li4"));
    CHECK(hits("li:nth-child(2n+1)", "li5"));
    CHECK(hits("li:nth-child(3n-1)", "li2")); // 3k-1: 2, 5
    CHECK(hits("li:nth-child(3n-1)", "li5"));
    CHECK(!hits("li:nth-child(3n-1)", "li3"));
    CHECK(hits("li:nth-child(-n+2)", "li1"));
    CHECK(hits("li:nth-child(-n+2)", "li2"));
    CHECK(!hits("li:nth-child(-n+2)", "li3"));
    CHECK(hits("li:nth-child( 2n + 1 )", "li3")); // whitespace variants
    CHECK(hits("li:nth-child(n)", "li4"));
    CHECK(hits("li:nth-last-child(1)", "li5"));
    CHECK(hits("li:nth-last-child(2)", "li4"));
    CHECK(hits("a:first-of-type", "link"));
    CHECK(hits("a:last-of-type", "anchor"));
    CHECK(!hits("a:last-of-type", "link"));
    CHECK(hits("ul:only-of-type", "list"));

    // --- Links ---------------------------------------------------------------
    CHECK(hits("a:link", "link"));
    CHECK(hits(":any-link", "link"));
    CHECK(!hits("a:link", "anchor")); // no href
    CHECK(!hits("a:visited", "link")); // parses; never matches yet

    // --- Logical pseudo-classes ----------------------------------------------
    CHECK(hits("p:not(.intro)", "p2"));
    CHECK(!hits("p:not(.intro)", "p1"));
    CHECK(hits(":is(ul, ol) li", "li1"));
    CHECK(hits(":where(.intro)", "p1"));
    CHECK(hits("p:not(:first-child)", "p2"));
    CHECK(hits(":is(bogus:pseudo, p)", "p1")); // forgiving: bad arm dropped
    CHECK(!valid(":not(bogus:pseudo)")); // :not is strict
    CHECK(!valid("p:unknown-pseudo"));
    CHECK(!valid("p,, q")); // empty selector in a list
    CHECK(!valid("p >")); // dangling combinator

    // --- :has(), a relative selector list ------------------------------------
    CHECK(hits("p:has(a)", "p2")); // a descendant, however deep
    CHECK(!hits("p:has(a)", "p1"));
    CHECK(hits("ul:has(> li)", "list")); // a child
    CHECK(!hits("ul:has(> a)", "list"));
    CHECK(hits("p:has(+ p)", "p1")); // the very next sibling
    CHECK(!hits("p:has(+ ul)", "p1"));
    CHECK(hits("p:has(~ ul)", "p1")); // any later sibling
    CHECK(hits("p:has(+ ul li)", "p3")); // a li inside the very next sibling
    CHECK(!hits("p:has(+ ul li)", "p1")); // p1 has p2 next, which holds no li
    CHECK(!hits("li:has(li)", "li1"));
    CHECK(hits("p:has(.missing, a)", "p2")); // a list: any arm answers
    CHECK(hits("div:has(ul li)", "wrap")); // a complex selector inside
    CHECK(!hits("div:has(ol li)", "wrap"));
    CHECK(hits("p:not(:has(a))", "p1")); // and it composes
    CHECK(!hits("p:not(:has(a))", "p2"));
    CHECK(!valid("p:has(a")); // unclosed
    CHECK(!valid("p:has()"));
    CHECK(!valid("p:has(>)")); // a combinator with nothing after it
    CHECK(!valid("p:has(a, )"));

    // --- :nth-child(An+B of S) -----------------------------------------------
    CHECK(hits("li:nth-child(1 of .odd)", "li1"));
    CHECK(hits("li:nth-child(2 of .odd)", "li3"));
    CHECK(hits("li:nth-child(3 of .odd)", "li5"));
    CHECK(!hits("li:nth-child(2 of .odd)", "li2")); // not in the list at all
    CHECK(!hits("li:nth-child(2 of .odd)", "li4"));
    CHECK(hits("li:nth-last-child(1 of .odd)", "li5"));
    CHECK(hits("li:nth-child(2n of .odd)", "li3")); // second of the three
    CHECK(!hits("li:nth-child(2n of .odd)", "li2")); // an+b cannot name a nothing
    CHECK(valid("li:nth-last-child(even of even)")); // a type selector, matching none
    CHECK(!hits("li:nth-last-child(even of even)", "li2"));
    CHECK(!valid("li:nth-child(1 of)")); // `of` with no selector
    CHECK(!valid("li:nth-child(of .odd)")); // no an+b
    CHECK(!valid("li:nth-of-type(2 of .odd)")); // the of-type forms take no list

    // --- Pseudo-elements parse, never match ----------------------------------
    CHECK(valid("p::before"));
    CHECK(valid("p:before")); // legacy single-colon form
    CHECK(!hits("p::before", "p1"));
    CHECK(valid("p::first-line"));
    CHECK(!valid("p::bogus"));

    // --- Specificity ---------------------------------------------------------
    CHECK(spec_of("#a") == (Specificity { 1, 0, 0 }));
    CHECK(spec_of("div p.x") == (Specificity { 0, 1, 2 }));
    CHECK(spec_of("#a .b [c] :hover div::after") == (Specificity { 1, 3, 2 }));
    CHECK(spec_of(":is(#a, .b)") == (Specificity { 1, 0, 0 })); // max of arms
    CHECK(spec_of(":where(#a, .b)") == (Specificity { 0, 0, 0 }));
    CHECK(spec_of(":not(#a)") == (Specificity { 1, 0, 0 }));
    CHECK(spec_of("li:nth-child(2n)") == (Specificity { 0, 1, 1 }));
    CHECK((Specificity { 1, 0, 0 }) > (Specificity { 0, 9, 9 }));

    return sashfold::test::report("css-selectors");
}
