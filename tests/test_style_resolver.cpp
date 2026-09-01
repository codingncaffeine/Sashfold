#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"

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
</style></head>
<body id="body">
  <p id="plain">plain</p>
  <div id="boxed" class="boxed">boxed</div>
  <p id="special" class="quiet">special</p>
  <p id="quiet" class="quiet" style="color: brown">styled</p>
  <div id="deep"><em id="em">big</em></div>
  <span id="sized" class="sized">sized</span>
  <div id="hidden" class="hidden">gone</div>
  <h1 id="h1">title</h1>
  <a id="link" href="/x">link</a>
  <pre id="pre">   pre   </pre>
  <ul id="ul"><li id="li">item</li></ul>
</body></html>)"));
    g_styles = css::resolve_styles(*g_document);

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

    return sashfold::test::report("style-resolver");
}
