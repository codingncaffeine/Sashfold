#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "text/FontManager.h"

#include <memory>
#include <string>

using namespace sashfold;
using css::ComputedStyle;
using css::Display;
using css::LengthPercent;

namespace {

std::unique_ptr<dom::Document> g_document;
css::StyleMap g_styles;

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

ComputedStyle const& style_of(std::string_view id)
{
    static ComputedStyle const fallback;
    dom::Element* element = find_by_id(*g_document, id);
    if (!element)
        return fallback;
    auto const it = g_styles.find(element);
    return it == g_styles.end() ? fallback : it->second;
}

bool close(float a, float b)
{
    return a > b - 0.01f && a < b + 0.01f;
}

} // namespace

int main()
{
    g_document = html::parse_document(std::string_view(R"(
<!doctype html>
<html><head><style>
  body { color: rgb(20, 30, 40); font-size: 20px }
  .boxed { margin: 1em 2em; padding: 4px; border: 2px solid red; width: 50% }
  #special { color: #abc; background-color: #11223380 }
  p { margin-top: 5px !important }
  p { margin-top: 9px }
  .quiet { color: blue }
  p.quiet { color: green }
  #deep em { font-size: 150% }
  .sized { font-size: 2em; line-height: 1.5 }
  .hidden { display: none }
  .units { margin: 1in 2.54cm 10mm 1pc; padding: 4Q 1.5pt 0 0 }
  .bounds { min-width: 5px; max-width: none; min-height: 1em; max-height: 50% }
  #gen { quotes: "<" ">" "«" "»" }
  #gen::before { content: "B" attr(data-x) open-quote; color: red }
  #gen::after { content: close-quote " Z"; display: block }
  #plainq::before { content: open-quote open-quote close-quote close-quote }
  #nogen::before { content: none }
  #url::before { content: url(x.png) }
  #normal::before { color: red }
  #legacy:before { content: "L" }
  #legacy::before span, #legacy { color: blue }
  :root { --main: rgb(1, 2, 3); --gap: 4px 6px; --chain: var(--main); --loop-a: var(--loop-b); --loop-b: var(--loop-a) }
  #vars { color: var(--main); margin: var(--gap); padding-left: var(--missing, 7px); background-color: var(--chain) }
  #vars-child { --main: rgb(9, 9, 9); color: var(--main) }
  #vars-grandchild { background-color: var(--main); padding-top: var(--missing) }
  #vars-loop { color: var(--loop-a, rgb(5, 5, 5)) }
  #vars-initial { --main: initial; color: var(--main, rgb(8, 8, 8)) }
  #calc { width: calc(100% - 20px); margin-left: calc(2em + 4px); padding-top: calc(10px * 2); padding-bottom: calc((10px + 5px) / 3);
          height: 50vh; min-width: min(100px, 50px); max-width: clamp(10px, 5px, 20px); margin-top: max(10%, 30%); margin-right: calc(3em * 2 - 6px) }
  #calc-bad { width: calc(10px * 10px); height: calc(10px + 5); margin-top: calc(5px }
</style></head>
<body id="body">
  <p id="plain">plain</p>
  <div id="boxed" class="boxed">boxed</div>
  <p id="special" class="quiet">special</p>
  <p id="quiet" class="quiet" style="color: brown">styled</p>
  <div id="deep"><em id="em">big</em></div>
  <span id="sized" class="sized">sized</span>
  <div id="hidden" class="hidden">gone</div>
  <div id="units" class="units">units</div>
  <div id="bounds" class="bounds">bounds</div>
  <h1 id="h1">title</h1>
  <a id="link" href="/x">link</a>
  <pre id="pre">   pre   </pre>
  <ul id="ul"><li id="li">item</li></ul>
  <div id="gen" data-x="attr">gen</div>
  <div id="plainq">q</div>
  <div id="nogen">n</div>
  <div id="url">u</div>
  <div id="normal">n</div>
  <div id="legacy">l</div>
  <div id="vars"><div id="vars-child"><span id="vars-grandchild">g</span></div></div>
  <div id="vars-loop">l</div>
  <div id="vars-initial">i</div>
  <div id="calc">c</div>
  <div id="calc-bad">b</div>
</body></html>)"));
    g_styles = css::resolve_styles(*g_document);

    // --- calc(), min(), max(), clamp(), the viewport units ---------------------
    {
        ComputedStyle const& c = style_of("calc");
        CHECK(c.width.kind == LengthPercent::Kind::Calc && close(c.width.percent, 100) && close(c.width.value, -20));
        CHECK(c.margin_left.kind == LengthPercent::Kind::Px && close(c.margin_left.value, 44)); // 2em of 20px, plus 4
        CHECK(close(c.padding_top.value, 20));
        CHECK(close(c.padding_bottom.value, 5)); // (10 + 5) / 3
        CHECK(c.height.kind == LengthPercent::Kind::Px && close(c.height.value, 384)); // 50vh of the default 768
        CHECK(close(c.min_width.value, 50));
        CHECK(close(c.max_width.value, 10)); // clamp holds 5 up to its 10 floor
        CHECK(c.margin_top.kind == LengthPercent::Kind::Percent && close(c.margin_top.value, 30)); // max in one currency
        CHECK(close(c.margin_right.value, 114)); // 3em of 20 is 60, twice, less 6
        ComputedStyle const& bad = style_of("calc-bad");
        CHECK(bad.width.is_auto()); // a length times a length is no length
        CHECK(bad.height.is_auto()); // a length plus a number neither
        CHECK(close(bad.margin_top.value, 0)); // an unclosed calc is dropped by the parser
    }

    // --- Custom properties and var() ------------------------------------------
    {
        ComputedStyle const& vars = style_of("vars");
        CHECK(vars.color == Color::rgb(1, 2, 3)); // var() from the root
        CHECK(close(vars.margin_top.value, 4) && close(vars.margin_right.value, 6)); // two tokens through a shorthand
        CHECK(close(vars.padding_left.value, 7)); // the fallback of a missing property
        CHECK(vars.background_color == Color::rgb(1, 2, 3)); // a custom property made of another
        ComputedStyle const& child = style_of("vars-child");
        CHECK(child.color == Color::rgb(9, 9, 9)); // its own value overrides the inherited one
        ComputedStyle const& grandchild = style_of("vars-grandchild");
        CHECK(grandchild.background_color == Color::rgb(9, 9, 9)); // custom properties inherit
        CHECK(close(grandchild.padding_top.value, 0)); // no value, no fallback: the declaration is dropped
        CHECK(style_of("vars-loop").color == Color::rgb(5, 5, 5)); // a cycle drops both; the fallback stands
        CHECK(style_of("vars-initial").color == Color::rgb(8, 8, 8)); // initial drops the inherited value
    }

    // --- Generated content: ::before and ::after ------------------------------
    {
        ComputedStyle const& gen = style_of("gen");
        if (CHECK(gen.generated && gen.generated->before && gen.generated->after)) {
            css::GeneratedBox const& before = *gen.generated->before;
            CHECK(before.text == "Battr<"); // the string, the attribute, the outer open quote
            CHECK(before.style.color == Color::rgb(255, 0, 0));
            CHECK(close(before.style.font_size, 20)); // inherited from the element
            CHECK(before.style.display == Display::Inline);
            css::GeneratedBox const& after = *gen.generated->after;
            CHECK(after.text == "> Z"); // the depth came back to the outer pair
            CHECK(after.style.display == Display::Block);
            CHECK(after.style.color == gen.color); // the ::before rule's color is its own
        }
        ComputedStyle const& plainq = style_of("plainq");
        if (CHECK(plainq.generated && plainq.generated->before))
            CHECK(plainq.generated->before->text == "“‘’”"); // the language's marks, nested
        CHECK(!style_of("nogen").generated); // content: none
        CHECK(!style_of("url").generated); // url() is not written: the declaration is dropped
        CHECK(!style_of("normal").generated); // no content: no box
        ComputedStyle const& legacy = style_of("legacy");
        if (CHECK(legacy.generated && legacy.generated->before))
            CHECK(legacy.generated->before->text == "L"); // the one-colon spelling
        // A pseudo-element anywhere but last invalidates its selector, and
        // with it the whole list: #legacy did not turn blue.
        CHECK(!(legacy.color == Color::rgb(0, 0, 255)));
    }

    // --- UA defaults ---------------------------------------------------------
    CHECK(style_of("body").display == Display::Block);
    CHECK(close(style_of("body").font_size, 20));
    CHECK(style_of("h1").display == Display::Block);
    CHECK(style_of("h1").bold());
    CHECK(close(style_of("h1").font_size, 40)); // 2em of body's 20px
    CHECK(close(style_of("h1").margin_top.value, 0.67f * 40)); // em of its own size
    CHECK(style_of("link").text_decoration == css::TextDecorationLine::Underline);
    CHECK(style_of("link").color.b == 238);
    CHECK(style_of("pre").white_space == css::WhiteSpace::Pre);
    CHECK(style_of("li").display == Display::ListItem);
    CHECK(style_of("ul").display == Display::Block);
    CHECK(style_of("hidden").display == Display::None);

    // --- Inheritance ---------------------------------------------------------
    CHECK(style_of("plain").color.r == 20);
    CHECK(style_of("plain").color.g == 30);
    CHECK(close(style_of("plain").font_size, 20));
    CHECK(style_of("plain").text_decoration == css::TextDecorationLine::None); // not inherited

    // --- Shorthands and units ------------------------------------------------
    {
        ComputedStyle const& boxed = style_of("boxed");
        CHECK(close(boxed.margin_top.value, 20)); // 1em at 20px
        CHECK(close(boxed.margin_right.value, 40)); // 2em
        CHECK(close(boxed.margin_bottom.value, 20));
        CHECK(close(boxed.margin_left.value, 40));
        CHECK(close(boxed.padding_top.value, 4));
        CHECK(close(boxed.border_top.width, 2));
        CHECK(boxed.border_top.style == css::BorderStyle::Solid);
        CHECK(boxed.border_left.color.r == 255); // red
        CHECK(boxed.width.kind == LengthPercent::Kind::Percent);
        CHECK(close(boxed.width.value, 50));
        // The absolute units, at 96 px to the inch.
        ComputedStyle const& units = style_of("units");
        CHECK(close(units.margin_top.value, 96)); // 1in
        CHECK(close(units.margin_right.value, 96)); // 2.54cm
        CHECK(close(units.margin_bottom.value, 96.0f / 25.4f * 10)); // 10mm
        CHECK(close(units.margin_left.value, 16)); // 1pc
        CHECK(close(units.padding_top.value, 96.0f / 101.6f * 4)); // 4Q
        CHECK(close(units.padding_right.value, 2)); // 1.5pt
        // The size bounds: none and auto read as auto, lengths and percentages as themselves.
        ComputedStyle const& bounds = style_of("bounds");
        CHECK(bounds.min_width.kind == LengthPercent::Kind::Px);
        CHECK(close(bounds.min_width.value, 5));
        CHECK(bounds.max_width.is_auto());
        CHECK(close(bounds.min_height.value, 20)); // 1em of body's 20px
        CHECK(bounds.max_height.kind == LengthPercent::Kind::Percent);
        CHECK(close(bounds.max_height.value, 50));
        CHECK(style_of("plain").max_width.is_auto()); // the initial value
        CHECK(style_of("plain").min_width.is_auto());
    }

    // --- Hex colors ----------------------------------------------------------
    CHECK(style_of("special").color.r == 0xAA); // #abc expands
    CHECK(style_of("special").color.g == 0xBB);
    CHECK(style_of("special").background_color.a == 0x80); // 8-digit hex alpha

    // --- Cascade: importance, specificity, style attribute ------------------
    CHECK(close(style_of("plain").margin_top.value, 5)); // !important beats later rule
    CHECK(style_of("special").color.r == 0xAA); // #special (1,0,0) beats p.quiet (0,1,1)
    CHECK(style_of("quiet").color.r == 165); // style attribute brown beats sheets
    CHECK(style_of("quiet").color.g == 42);

    // --- Percentage font-size against the parent ----------------------------
    CHECK(close(style_of("em").font_size, 30)); // 150% of 20px
    CHECK(style_of("em").font_style == css::FontStyle::Italic); // UA em rule

    // --- em on font-size uses parent; line-height number scales own ---------
    CHECK(close(style_of("sized").font_size, 40)); // 2em of 20
    CHECK(close(style_of("sized").line_height_px(), 60)); // 1.5 x 40

    // --- font-family: lists, strings, spaced identifiers, inheritance ---------
    g_document = html::parse_document(std::string_view(R"(
<!doctype html>
<html><head><style>
  body { font-family: "Times New Roman", Georgia , serif }
  .sans { font-family: Segoe UI, sans-serif }
  .bad { font-family: "Foo" Bar }
  .empty { font-family: , serif }
</style></head>
<body>
  <p id="fam">x<span id="inherit">y</span></p>
  <p id="sans" class="sans">z</p>
  <p id="bad" class="bad">w</p>
  <p id="empty" class="empty">v</p>
  <pre id="pre">c</pre>
  <p id="code"><code id="code-inner">c</code></p>
</body></html>)"));
    g_styles = css::resolve_styles(*g_document);
    {
        auto const& families = style_of("fam").font_family;
        if (CHECK(families != nullptr) && CHECK_EQ(families->size(), 3u)) {
            CHECK_EQ((*families)[0], std::string("Times New Roman"));
            CHECK_EQ((*families)[1], std::string("Georgia"));
            CHECK_EQ((*families)[2], std::string("serif"));
        }
        CHECK(style_of("inherit").font_family == families); // shared down the tree
        auto const& sans = style_of("sans").font_family;
        if (CHECK(sans != nullptr) && CHECK_EQ(sans->size(), 2u)) {
            CHECK_EQ((*sans)[0], std::string("Segoe UI"));
            CHECK_EQ((*sans)[1], std::string("sans-serif"));
        }
        CHECK(style_of("bad").font_family == families); // invalid: the inherited list stays
        CHECK(style_of("empty").font_family == families);
        auto const& pre = style_of("pre").font_family;
        CHECK(pre != nullptr && pre->size() == 1 && (*pre)[0] == "monospace"); // UA rule
        auto const& code = style_of("code-inner").font_family;
        CHECK(code != nullptr && (*code)[0] == "monospace");
        CHECK(style_of("code").font_family == families);
    }

    // --- The CSS-wide keywords: inherit, initial, unset, all ---------------------
    g_document = html::parse_document(std::string_view(R"(<!doctype html>
<html><head><style>
  #parent { color: red; width: 100px; margin-left: 7px; border: 2px solid blue; font-size: 20px; text-align: center }
  #inh { width: inherit; margin: inherit; border-left: inherit; text-align: initial; font-size: inherit }
  #ini { color: initial; font-size: initial }
  #uns { color: unset; width: unset; text-align: unset }
  #all { all: initial }
  #allinh { all: inherit }
  #bcp { color: red; border: none }
  #bc { border-color: inherit; border-style: solid; color: green }
  p { font: inherit }
  #fs { font: 20px serif }
</style></head>
<body><div id="parent"><p id="inh">a</p><p id="ini">b</p><p id="uns">c</p><p id="all">d</p><p id="allinh">e</p></div>
<div id="bcp"><div id="bc">f</div></div><p id="fs">g</p></body></html>)"));
    g_styles = css::resolve_styles(*g_document);
    {
        css::ComputedStyle const& inh = style_of("inh");
        CHECK(inh.width.kind == css::LengthPercent::Kind::Px && close(inh.width.value, 100)); // the parent's
        CHECK(close(inh.margin_left.value, 7)); // a shorthand copies every side
        CHECK(close(inh.margin_top.value, 0));
        CHECK(close(inh.border_left.width, 2));
        CHECK(inh.border_left.color == Color::rgb(0, 0, 255));
        CHECK(close(inh.border_top.width, 0));
        CHECK(inh.text_align == css::TextAlign::Left); // initial, though the parent centers
        CHECK(close(inh.font_size, 20));
        css::ComputedStyle const& ini = style_of("ini");
        CHECK(ini.color == Color::rgb(0, 0, 0));
        CHECK(close(ini.font_size, 16));
        css::ComputedStyle const& uns = style_of("uns");
        CHECK(uns.color == Color::rgb(255, 0, 0)); // unset on an inherited property inherits
        CHECK(uns.width.is_auto()); // and resets one that does not
        CHECK(uns.text_align == css::TextAlign::Center);
        css::ComputedStyle const& all = style_of("all");
        CHECK(all.color == Color::rgb(0, 0, 0)); // all: initial resets the inherited too
        CHECK(all.display == css::Display::Inline); // the UA sheet's block gives way
        CHECK(close(all.font_size, 16));
        CHECK(close(all.margin_top.value, 0));
        css::ComputedStyle const& allinh = style_of("allinh");
        CHECK(allinh.width.kind == css::LengthPercent::Kind::Px && close(allinh.width.value, 100));
        CHECK(allinh.color == Color::rgb(255, 0, 0));
        CHECK(allinh.text_align == css::TextAlign::Center);
        CHECK(allinh.display == css::Display::Block); // the parent's
        // A parent's border color that was currentColor inherits as the
        // keyword — this element's own green, not the parent's red.
        CHECK(style_of("bc").border_top.color == Color::rgb(0, 128, 0));
        // A lower-ranked font: inherit does not undo a later shorthand's size.
        CHECK(close(style_of("fs").font_size, 20));
    }

    // --- hsl() and hsla() ------------------------------------------------------------
    g_document = html::parse_document(std::string_view(R"(<!doctype html>
<html><head><style>
  #a { color: hsl(60, 100%, 50%) }
  #b { color: hsl(120deg 100% 25%) }
  #c { color: hsla(240, 100%, 50%, 0.5) }
  #d { color: hsl(0.5turn 50% 50% / 20%) }
  #e { color: hsl(-60, 20%, 50%) }
  #f { color: hsl(60, 20%, 50%) }
  #g { color: hsl(10px, 1, 1) }
</style></head>
<body><p id="a">a</p><p id="b">b</p><p id="c">c</p><p id="d">d</p><p id="e">e</p><p id="f">f</p><p id="g">g</p></body></html>)"));
    g_styles = css::resolve_styles(*g_document);
    {
        CHECK(style_of("a").color == Color::rgb(255, 255, 0)); // yellow
        CHECK(style_of("b").color == Color::rgb(0, 128, 0)); // green, the modern syntax
        CHECK(style_of("c").color == Color::rgba(0, 0, 255, 128));
        CHECK(style_of("d").color == Color::rgba(64, 191, 191, 51)); // half a turn is 180: a cyan
        CHECK(style_of("e").color == Color::rgb(153, 102, 153)); // a negative hue wraps: 300
        CHECK(style_of("f").color == Color::rgb(153, 153, 102)); // the specification's own example
        CHECK(style_of("g").color == Color::rgb(0, 0, 0)); // a length is no hue: the declaration is ignored
    }

    // --- The border side longhands ---------------------------------------------
    g_document = html::parse_document(std::string_view(R"(<!doctype html>
<html><head><style>
  #a { border-left-style: solid; border-left-width: 2px; border-left-color: red }
  #b { border-top-style: solid }
  #c { border-right-width: 4px }
  #d { border-bottom-style: solid; border-bottom-width: -1px }
  #e { border: 1px solid blue; border-right-color: lime; border-right-width: thick }
</style></head>
<body><p id="a">a</p><p id="b">b</p><p id="c">c</p><p id="d">d</p><p id="e">e</p></body></html>)"));
    g_styles = css::resolve_styles(*g_document);
    {
        css::ComputedStyle const& a = style_of("a");
        CHECK(a.border_left.style == css::BorderStyle::Solid);
        CHECK(close(a.border_left.width, 2));
        CHECK(a.border_left.color == Color::rgb(255, 0, 0));
        CHECK(close(a.border_top.width, 0)); // the other sides untouched
        CHECK(close(style_of("b").border_top.width, 3)); // a style alone is medium wide
        CHECK(close(style_of("c").border_right.width, 0)); // a width with no style has no used width
        CHECK(close(style_of("d").border_bottom.width, 3)); // a negative width is ignored: medium stands
        css::ComputedStyle const& e = style_of("e");
        CHECK(close(e.border_right.width, 5));
        CHECK(e.border_right.color == Color::rgb(0, 255, 0));
        CHECK(e.border_left.color == Color::rgb(0, 0, 255));
        CHECK(close(e.border_left.width, 1));
    }

    // --- The font shorthand ---------------------------------------------------
    g_document = html::parse_document(std::string_view(R"(<!doctype html>
<html><head><style>
  body { font: 20px serif }
  #full { font: italic bold 12px/1.5 Verdana, sans-serif }
  #ratio { font: 10px/1 Verdana }
  #weight { font: 300 2em "Segoe UI" }
  #bad { font: bold }
  #system { font: menu }
</style></head>
<body><p id="base">a</p><p id="full">b</p><p id="ratio">c</p><p id="weight">d</p><p id="bad">e</p><p id="system">f</p></body></html>)"));
    g_styles = css::resolve_styles(*g_document);
    {
        CHECK(close(style_of("base").font_size, 20));
        css::ComputedStyle const& full = style_of("full");
        CHECK(close(full.font_size, 12));
        CHECK_EQ(full.font_weight, 700);
        CHECK(full.font_style == css::FontStyle::Italic);
        CHECK(close(full.line_height_px(), 18)); // 1.5 x 12
        CHECK(full.font_family && full.font_family->size() == 2 && (*full.font_family)[0] == "Verdana");
        css::ComputedStyle const& ratio = style_of("ratio");
        CHECK(close(ratio.font_size, 10));
        CHECK(close(ratio.line_height_px(), 10));
        CHECK_EQ(ratio.font_weight, 400); // the shorthand resets what it does not name
        css::ComputedStyle const& weight = style_of("weight");
        CHECK_EQ(weight.font_weight, 300);
        CHECK(close(weight.font_size, 40)); // 2em of the body's 20
        CHECK(close(style_of("bad").font_size, 20)); // no size and no family: not a shorthand
        CHECK_EQ(style_of("bad").font_weight, 400);
        CHECK(close(style_of("system").font_size, 20)); // the system fonts are not written
    }

    // --- ex and ch: the face's own measurements --------------------------------
    {
        // With the machine's fonts out of the way every family answers with
        // the built-in face, whose x-height is 15 of its 32 units per em and
        // whose every glyph advances 20 of them. So one ex is 15/32 of the
        // font size and one ch is 20/32 — neither the half both units used to
        // stand for.
        text::FontManager::instance().set_system_fonts(false);
        g_document = html::parse_document(std::string_view(R"(
<!doctype html>
<html><head><style>
  #plain { width: 10ex; height: 10ch; font-size: 32px }
  #sized { width: 2ex; font-size: 1ex }
  #late { width: 10ch; font-size: 32px }
  #calc { width: calc(10ch + 2px); font-size: 32px }
</style></head><body>
  <div id="plain"></div>
  <div id="sized"></div>
  <div id="late" style="font-family: monospace"></div>
  <div id="calc"></div>
</body></html>)"));
        g_styles = css::resolve_styles(*g_document);
        ComputedStyle const& plain = style_of("plain");
        CHECK(close(plain.width.value, 150)); // 10ex of 32px: 10 x 15/32 x 32
        CHECK(close(plain.height.value, 200)); // 10ch of 32px: 10 x 20/32 x 32
        // On font-size itself the unit resolves against the parent's font, so
        // 1ex of the initial 16px settles the size and the width follows it.
        ComputedStyle const& sized = style_of("sized");
        CHECK(close(sized.font_size, 7.5)); // 15/32 of 16
        CHECK(close(sized.width.value, 7.03125)); // 2ex of 7.5
        // The family is settled before the lengths are, whatever order the
        // cascade met them in: the style attribute here is applied after the
        // rule asking for 10ch, and the length still measures the face the
        // attribute names.
        CHECK(close(style_of("late").width.value, 200));
        ComputedStyle const& calc = style_of("calc");
        CHECK(calc.width.kind == LengthPercent::Kind::Px && close(calc.width.value, 202));
    }

    return sashfold::test::report("style-resolver");
}
