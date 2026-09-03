#include "Test.h"

#include "core/Bitmap.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "paint/Painter.h"
#include "text/FontManager.h"

#include <memory>
#include <string>
#include <string_view>

// Rounded corners: the shape's own geometry (settling overlapping radii,
// the curve's span at a height, coverage, the inner curve a border
// leaves), border-radius through the cascade, and what the painter puts
// on the canvas — a corner cut away, a border's ring, a curved clip.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document;
    css::StyleMap styles;
    layout::LayoutResult result;
};

Page lay_out(std::string_view html)
{
    Page page;
    page.document = html::parse_document(html);
    page.styles = css::resolve_styles(*page.document, css::collect_stylesheets(*page.document, nullptr, {}), {});
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

Bitmap paint_canvas(Page const& page, int width, int height)
{
    Bitmap canvas(width, height, page.result.canvas_background);
    paint::paint_page(canvas, page.result, 0, 0, nullptr);
    return canvas;
}

bool same(Color a, Color b, int tolerance = 2)
{
    auto const near = [tolerance](std::uint8_t x, std::uint8_t y) {
        return (x > y ? x - y : y - x) <= tolerance;
    };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

bool near_enough(float a, float b, float tolerance = 0.01f)
{
    float const difference = a > b ? a - b : b - a;
    return difference <= tolerance;
}

RoundedRect square_corners(float x, float y, float width, float height, float radius)
{
    RoundedRect shape { x, y, width, height, radius, radius, radius, radius, radius, radius,
        radius, radius };
    shape.settle();
    return shape;
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

    // --- The shape ----------------------------------------------------------------
    {
        RoundedRect plain = RoundedRect::of(Rect { 3, 4, 20, 10 });
        CHECK(plain.is_rectangular());
        CHECK_EQ(plain.coverage(3, 4), std::uint8_t(255));
        CHECK_EQ(plain.coverage(2, 4), std::uint8_t(0));
        CHECK_EQ(plain.coverage(22, 13), std::uint8_t(255));
        CHECK_EQ(plain.coverage(23, 13), std::uint8_t(0));
        Rect const bounds = plain.bounds();
        CHECK_EQ(bounds.x, 3);
        CHECK_EQ(bounds.width, 20);

        // A radius past the half of an edge shrinks with all the others by
        // the same factor: 40 and 40 over a width of 20 halves both, then
        // the height of 10 halves them again.
        RoundedRect big = square_corners(0, 0, 20, 10, 40);
        CHECK(near_enough(big.top_left_x, 5.0f));
        CHECK(near_enough(big.bottom_right_y, 5.0f));
        CHECK(!big.is_rectangular());

        // A corner with one radius at zero is square.
        RoundedRect flat { 0, 0, 40, 40, 10, 0, 0, 0, 0, 0, 0, 0 };
        flat.settle();
        CHECK(flat.is_rectangular());
    }

    // --- The curve's span -----------------------------------------------------------
    {
        // A circle of radius 10 in a 20 by 20 box: at its middle the span
        // is the whole width, at its very top a point.
        RoundedRect circle = square_corners(0, 0, 20, 20, 10);
        float left = 0;
        float right = 0;
        if (CHECK(circle.span_at(10.0f, left, right))) {
            CHECK(near_enough(left, 0.0f));
            CHECK(near_enough(right, 20.0f));
        }
        // At a quarter of the way down, the half-width is sqrt(1 - 1/4) of
        // the radius: 10 - 8.660 = 1.34 in from each edge.
        if (CHECK(circle.span_at(5.0f, left, right))) {
            CHECK(near_enough(left, 10.0f - 8.6603f));
            CHECK(near_enough(right, 10.0f + 8.6603f));
        }
        CHECK(!circle.span_at(-1.0f, left, right));
        CHECK(!circle.span_at(20.0f, left, right));
        CHECK(circle.contains_point(10.0f, 10.0f));
        CHECK(!circle.contains_point(0.5f, 0.5f));
        CHECK(circle.contains_point(0.0f, 10.0f));

        // Coverage: the middle of the shape is whole, a pixel well outside
        // the corner is nothing, and the pixel the curve crosses is partial.
        CHECK_EQ(circle.coverage(10, 10), std::uint8_t(255));
        CHECK_EQ(circle.coverage(0, 0), std::uint8_t(0));
        std::uint8_t const edge = circle.coverage(3, 2);
        CHECK(edge > 0 && edge < 255);
    }

    // --- The curve a border leaves --------------------------------------------------
    {
        RoundedRect const outer = square_corners(0, 0, 100, 60, 20);
        RoundedRect const inner = outer.inset(5, 5, 5, 5);
        CHECK(near_enough(inner.x, 5.0f));
        CHECK(near_enough(inner.width, 90.0f));
        CHECK(near_enough(inner.top_left_x, 15.0f));
        CHECK(near_enough(inner.bottom_right_y, 15.0f));
        // A border wider than the radius squares the corner off.
        RoundedRect const thick = outer.inset(30, 30, 30, 30);
        CHECK(thick.is_rectangular());
        // Uneven borders shrink each radius by the edge behind it.
        RoundedRect const uneven = outer.inset(2, 8, 0, 0);
        CHECK(near_enough(uneven.top_left_x, 18.0f));
        CHECK(near_enough(uneven.top_left_y, 12.0f));
        CHECK(near_enough(uneven.top_right_x, 20.0f));
    }

    // --- Filling ---------------------------------------------------------------------
    {
        Bitmap canvas(30, 30, Color::rgb(255, 255, 255));
        canvas.fill_rounded(square_corners(0, 0, 30, 30, 15), Color::rgb(0, 0, 0));
        // A disc: the middle is black, the corners are left alone, and the
        // pixel the curve crosses is a blend of the two.
        CHECK(same(canvas.pixel(15, 15), Color::rgb(0, 0, 0)));
        CHECK(same(canvas.pixel(0, 0), Color::rgb(255, 255, 255)));
        CHECK(same(canvas.pixel(29, 29), Color::rgb(255, 255, 255)));
        Color const corner = canvas.pixel(6, 2);
        CHECK(corner.r > 0 && corner.r < 255);
        // Straight edges stay crisp: the middle of every side is filled.
        CHECK(same(canvas.pixel(15, 0), Color::rgb(0, 0, 0)));
        CHECK(same(canvas.pixel(1, 15), Color::rgb(0, 0, 0)));

        // A rounded clip fades what is drawn through it the same way.
        Bitmap clipped(30, 30, Color::rgb(255, 255, 255));
        clipped.push_round_clip(square_corners(0, 0, 30, 30, 15));
        clipped.fill_rect(Rect { 0, 0, 30, 30 }, Color::rgb(0, 0, 0));
        CHECK(same(clipped.pixel(15, 15), Color::rgb(0, 0, 0)));
        CHECK(same(clipped.pixel(0, 0), Color::rgb(255, 255, 255)));
        CHECK(same(clipped.pixel(6, 2), corner));
        clipped.truncate_round_clips(0);
        clipped.fill_rect(Rect { 0, 0, 2, 2 }, Color::rgb(0, 0, 0));
        CHECK(same(clipped.pixel(0, 0), Color::rgb(0, 0, 0)));
    }

    // --- The values through the cascade ----------------------------------------------
    {
        Page const page = lay_out(page_with(R"HTML(<style>
  #one { border-radius: 8px }
  #two { border-radius: 1px 2px 3px 4px }
  #three { border-radius: 10px 20px / 30px 40px }
  #four { border-top-left-radius: 5px 9px; border-bottom-right-radius: 50% }
  #five { border-radius: 25% }
  #six { border-radius: -4px }
  #seven { border-radius: 6px; border-radius: 3px 4px 5px 6px 7px }
  #eight { border-radius: 12px; border-radius: inherit }
</style>
<div id="one"></div><div id="two"></div><div id="three"></div><div id="four"></div>
<div id="five"></div><div id="six"></div><div id="seven"></div><div id="eight"></div>)HTML"));
        auto const px = [](css::LengthPercent const& length) { return length.value; };
        css::ComputedStyle const* one = style_of(page, "one");
        if (CHECK(one)) {
            CHECK(one->rounded());
            CHECK_EQ(px(one->border_top_left_radius.x), 8.0f);
            CHECK_EQ(px(one->border_bottom_right_radius.y), 8.0f);
        }
        // Four values run clockwise from the top left.
        css::ComputedStyle const* two = style_of(page, "two");
        if (CHECK(two)) {
            CHECK_EQ(px(two->border_top_left_radius.x), 1.0f);
            CHECK_EQ(px(two->border_top_right_radius.x), 2.0f);
            CHECK_EQ(px(two->border_bottom_right_radius.x), 3.0f);
            CHECK_EQ(px(two->border_bottom_left_radius.x), 4.0f);
        }
        // A slash gives the horizontal radii first, then the vertical: two
        // values each cover the diagonals.
        css::ComputedStyle const* three = style_of(page, "three");
        if (CHECK(three)) {
            CHECK_EQ(px(three->border_top_left_radius.x), 10.0f);
            CHECK_EQ(px(three->border_top_left_radius.y), 30.0f);
            CHECK_EQ(px(three->border_top_right_radius.x), 20.0f);
            CHECK_EQ(px(three->border_top_right_radius.y), 40.0f);
            CHECK_EQ(px(three->border_bottom_right_radius.x), 10.0f);
            CHECK_EQ(px(three->border_bottom_left_radius.y), 40.0f);
        }
        css::ComputedStyle const* four = style_of(page, "four");
        if (CHECK(four)) {
            CHECK_EQ(px(four->border_top_left_radius.x), 5.0f);
            CHECK_EQ(px(four->border_top_left_radius.y), 9.0f);
            CHECK(four->border_bottom_right_radius.x.kind == css::LengthPercent::Kind::Percent);
            CHECK_EQ(four->border_bottom_right_radius.y.value, 50.0f);
            CHECK(four->border_top_right_radius.is_zero());
        }
        css::ComputedStyle const* five = style_of(page, "five");
        if (CHECK(five)) {
            CHECK(five->border_top_left_radius.x.kind == css::LengthPercent::Kind::Percent);
            CHECK_EQ(five->border_bottom_left_radius.y.value, 25.0f);
            CHECK(five->rounded());
        }
        // A negative radius, and a fifth value, are junk: the declaration
        // falls on the floor and what came before it stands.
        css::ComputedStyle const* six = style_of(page, "six");
        if (CHECK(six))
            CHECK(!six->rounded());
        css::ComputedStyle const* seven = style_of(page, "seven");
        if (CHECK(seven))
            CHECK_EQ(px(seven->border_top_left_radius.x), 6.0f);
        // The corners are not inherited, so `inherit` takes the body's.
        css::ComputedStyle const* eight = style_of(page, "eight");
        if (CHECK(eight))
            CHECK(!eight->rounded());
    }

    // --- On the canvas ----------------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"HTML(<style>
  #disc { width: 40px; height: 40px; background: #000; border-radius: 50% }
  #ring { width: 40px; height: 40px; background: #fff;
          border: 10px solid #f00; border-radius: 20px }
  #cut { width: 40px; height: 40px; background: #00f; border-radius: 0 0 0 20px }
  #hides { width: 40px; height: 40px; border-radius: 20px; overflow: hidden }
  #hides div { width: 40px; height: 40px; background: #0f0 }
</style>
<div id="disc"></div><div id="ring"></div><div id="cut"></div><div id="hides"><div></div></div>)HTML"));
        // The boxes stand one under the other: the disc at 0 (40 tall),
        // the ring at 40 (60 tall, its borders outside its 40), the cut
        // box at 100 and the clipping one at 140.
        Bitmap const canvas = paint_canvas(page, 60, 180);
        // The disc: filled at its middle and at the top of its curve, the
        // page's white left in its corner.
        CHECK(same(canvas.pixel(20, 20), Color::rgb(0, 0, 0)));
        CHECK(same(canvas.pixel(1, 1), Color::rgb(255, 255, 255)));
        CHECK(same(canvas.pixel(20, 0), Color::rgb(0, 0, 0)));
        // The ring: red border, white padding box inside it, nothing outside
        // the curve.
        CHECK(same(canvas.pixel(30, 40 + 2), Color::rgb(255, 0, 0)));
        CHECK(same(canvas.pixel(30, 40 + 30), Color::rgb(255, 255, 255)));
        CHECK(same(canvas.pixel(1, 40 + 1), Color::rgb(255, 255, 255)));
        // Only the bottom-left corner is cut from the third box.
        CHECK(same(canvas.pixel(1, 100 + 1), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(38, 100 + 1), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(38, 100 + 38), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(1, 100 + 38), Color::rgb(255, 255, 255)));
        // The fourth box clips its child along the curve.
        CHECK(same(canvas.pixel(20, 140 + 20), Color::rgb(0, 255, 0)));
        CHECK(same(canvas.pixel(1, 140 + 1), Color::rgb(255, 255, 255)));
    }

    return sashfold::test::report("rounded corners");
}
