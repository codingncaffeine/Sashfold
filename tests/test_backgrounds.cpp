#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "paint/Painter.h"
#include "text/FontManager.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Backgrounds: the background-image list and its companions through the
// cascade (url() resolved against the sheet, gradients, the shorthand's
// layers), and what the painter puts on the canvas — a gradient's colors
// along its line, a tiled and a placed picture, the clip and origin boxes.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document;
    css::StyleMap styles;
    layout::LayoutResult result;
};

Page lay_out(std::string_view html, net::Url const* url = nullptr)
{
    Page page;
    page.document = html::parse_document(html);
    page.styles = css::resolve_styles(*page.document, css::collect_stylesheets(*page.document, url, {}), {}, url);
    page.result = layout::layout_document(*page.document, page.styles, 400);
    return page;
}

dom::Element const* find_element(dom::Node const& node, std::string_view id)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (dom::Attr const* attribute = element.find_attribute("id"); attribute && attribute->value == id)
            return &element;
    }
    for (dom::Node const* child : node.children()) {
        if (dom::Element const* found = find_element(*child, id))
            return found;
    }
    return nullptr;
}

css::ComputedStyle const* style_of(Page const& page, std::string_view id)
{
    dom::Element const* element = find_element(*page.document, id);
    if (!element)
        return nullptr;
    auto const it = page.styles.find(element);
    return it == page.styles.end() ? nullptr : &it->second;
}

Bitmap paint_canvas(Page const& page, int width, int height, layout::BackgroundImages const* backgrounds = nullptr)
{
    Bitmap canvas(width, height, page.result.canvas_background);
    paint::paint_page(canvas, page.result, 0, 0, backgrounds);
    return canvas;
}

bool same(Color a, Color b, int tolerance = 2)
{
    auto const near = [tolerance](std::uint8_t x, std::uint8_t y) {
        return (x > y ? x - y : y - x) <= tolerance;
    };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

constexpr std::string_view head = R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
</style></head><body>)HTML";

std::string page_with(std::string_view body)
{
    return std::string(head) + std::string(body) + "</body></html>";
}

} // namespace

int main()
{
    text::FontManager::instance().set_system_fonts(false);

    // --- The values through the cascade ------------------------------------------
    {
        std::optional<net::Url> const url = net::parse_url("https://example.org/css/page.html");
        Page const page = lay_out(page_with(R"HTML(<style>
  #a { background-image: url(../pics/a.png), linear-gradient(to right, red, blue 30%, green); background-repeat: no-repeat, repeat-x; background-position: center, 10px 20%; background-size: cover, 50px auto }
  #b { background: #123 url("b.png") no-repeat right bottom / 40px 30px padding-box content-box }
  #c { background: radial-gradient(circle closest-side at 20% 80%, white, black), linear-gradient(45deg, #000, #fff) }
  #d { background-image: none; background-clip: content-box; background-origin: border-box }
  #bad { background-image: url(x.png); background-image: linear-gradient(red); background-repeat: sideways }
</style>
<div id="a"></div><div id="b"></div><div id="c"></div><div id="d"></div><div id="bad"></div>)HTML"), &*url);
        css::ComputedStyle const* a = style_of(page, "a");
        if (CHECK(a && a->background_images && a->background_images->size() == 2)) {
            CHECK_EQ((*a->background_images)[0].url, "https://example.org/pics/a.png");
            css::Gradient const* gradient = (*a->background_images)[1].gradient.get();
            if (CHECK(gradient)) {
                CHECK(gradient->kind == css::Gradient::Kind::Linear);
                CHECK_EQ(gradient->angle, 90.0f);
                CHECK_EQ(gradient->stops.size(), std::size_t(3));
                CHECK(gradient->stops[1].position && gradient->stops[1].position->value == 30.0f);
            }
            CHECK(a->background_repeats && a->background_repeats->size() == 2);
            CHECK((*a->background_repeats)[0].x == css::BackgroundRepeat::NoRepeat);
            CHECK((*a->background_repeats)[1].x == css::BackgroundRepeat::Repeat
                && (*a->background_repeats)[1].y == css::BackgroundRepeat::NoRepeat);
            CHECK(a->background_positions && (*a->background_positions)[0].x.value == 50.0f
                && (*a->background_positions)[1].x.value == 10.0f && (*a->background_positions)[1].y.value == 20.0f);
            CHECK(a->background_sizes && (*a->background_sizes)[0].kind == css::BackgroundSize::Kind::Cover
                && (*a->background_sizes)[1].kind == css::BackgroundSize::Kind::Lengths
                && (*a->background_sizes)[1].width.value == 50.0f && (*a->background_sizes)[1].height.is_auto());
        }
        css::ComputedStyle const* b = style_of(page, "b");
        if (CHECK(b && b->background_images && b->background_images->size() == 1)) {
            CHECK_EQ(b->background_color.r, 0x11);
            CHECK_EQ((*b->background_images)[0].url, "https://example.org/css/b.png");
            CHECK((*b->background_repeats)[0].x == css::BackgroundRepeat::NoRepeat);
            CHECK((*b->background_positions)[0].x.value == 100.0f && (*b->background_positions)[0].y.value == 100.0f);
            CHECK((*b->background_sizes)[0].width.value == 40.0f && (*b->background_sizes)[0].height.value == 30.0f);
            CHECK((*b->background_origins)[0] == css::BackgroundBox::PaddingBox);
            CHECK((*b->background_clips)[0] == css::BackgroundBox::ContentBox);
        }
        css::ComputedStyle const* c = style_of(page, "c");
        if (CHECK(c && c->background_images && c->background_images->size() == 2)) {
            css::Gradient const* radial = (*c->background_images)[0].gradient.get();
            if (CHECK(radial)) {
                CHECK(radial->kind == css::Gradient::Kind::Radial);
                CHECK(radial->shape == css::Gradient::Shape::Circle);
                CHECK(radial->extent == css::Gradient::Extent::ClosestSide);
                CHECK(radial->center_x.value == 20.0f && radial->center_y.value == 80.0f);
            }
            css::Gradient const* linear = (*c->background_images)[1].gradient.get();
            CHECK(linear && linear->angle == 45.0f);
        }
        css::ComputedStyle const* d = style_of(page, "d");
        if (CHECK(d)) {
            CHECK(!d->background_images);
            CHECK(d->background_clips && (*d->background_clips)[0] == css::BackgroundBox::ContentBox);
            CHECK(d->background_origins && (*d->background_origins)[0] == css::BackgroundBox::BorderBox);
        }
        css::ComputedStyle const* bad = style_of(page, "bad");
        if (CHECK(bad)) {
            // A gradient with one stop is invalid; the earlier url stands.
            CHECK(bad->background_images && (*bad->background_images)[0].url == "https://example.org/css/x.png");
            CHECK(!bad->background_repeats);
        }
    }

    // --- Painting: a gradient's colors, and a picture placed, tiled, clipped ----
    {
        Page const page = lay_out(page_with(R"HTML(<div id="g" style="width: 100px; height: 20px; background: linear-gradient(to right, rgb(0,0,0), rgb(200,0,0))"></div>
<div id="v" style="width: 20px; height: 100px; background: linear-gradient(rgb(0,0,255) 50%, rgb(0,255,0) 50%)"></div>
<div id="p" style="width: 100px; height: 40px; background: #fff url(pic) no-repeat 10px 5px / 20px 10px"></div>
<div id="t" style="width: 100px; height: 40px; background: #fff url(pic) repeat 0 0 / 30px 20px"></div>
<div id="c" style="width: 100px; height: 40px; padding: 10px; border: 5px solid rgba(0,0,0,0); background: url(pic) repeat; background-clip: content-box"></div>)HTML"));
        layout::BackgroundImages backgrounds;
        {
            Bitmap picture(2, 2, Color::rgb(255, 0, 0));
            picture.set_pixel(1, 1, Color::rgb(0, 0, 255));
            backgrounds.emplace("pic", std::make_shared<Bitmap const>(std::move(picture)));
        }
        Bitmap const canvas = paint_canvas(page, 400, 300, &backgrounds);
        // The horizontal gradient: black at the left edge, red at the right,
        // half way between in the middle.
        CHECK(same(canvas.pixel(0, 10), Color::rgb(1, 0, 0), 4));
        CHECK(same(canvas.pixel(99, 10), Color::rgb(199, 0, 0), 4));
        CHECK(same(canvas.pixel(50, 10), Color::rgb(100, 0, 0), 4));
        // A hard stop half way down: blue above, green below.
        CHECK(same(canvas.pixel(10, 20 + 25), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(10, 20 + 75), Color::rgb(0, 255, 0)));
        // The placed picture: 20 by 10 at (10, 5) in its box, red with a blue quarter.
        int const p_top = 120;
        CHECK(same(canvas.pixel(5, p_top + 2), Color::rgb(255, 255, 255)));
        CHECK(same(canvas.pixel(12, p_top + 6), Color::rgb(255, 0, 0)));
        CHECK(same(canvas.pixel(27, p_top + 12), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(35, p_top + 6), Color::rgb(255, 255, 255)));
        // Tiles of 30 by 20 from the corner: the blue quarter repeats at
        // (15..30, 10..20) in every tile.
        int const t_top = 160;
        CHECK(same(canvas.pixel(5, t_top + 5), Color::rgb(255, 0, 0)));
        CHECK(same(canvas.pixel(20, t_top + 15), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(50, t_top + 35), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(95, t_top + 5), Color::rgb(255, 0, 0)));
        // Clipped to the content box: nothing under the padding, the picture inside.
        int const c_top = 200;
        CHECK(same(canvas.pixel(7, c_top + 7), Color::rgb(255, 255, 255)));
        // The tiles start at the padding box's corner (5, 5): the content
        // box's first pixel is a tile's red corner.
        CHECK(same(canvas.pixel(15, c_top + 15), Color::rgb(255, 0, 0)));
        CHECK(same(canvas.pixel(16, c_top + 16), Color::rgb(0, 0, 255)));
    }

    return sashfold::test::report("backgrounds");
}
