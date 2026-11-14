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

// filter, backdrop-filter and clip-path: their values through the cascade,
// what the painter makes of a box painted through them — the colors a
// filter leaves, how far a blur and a drop shadow reach, the shape a clip
// path cuts — and that what a clip path cuts away no longer answers the
// pointer.

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
    page.styles = css::resolve_styles(*page.document, css::collect_stylesheets(*page.document, nullptr, {}), {}, nullptr);
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

bool same(Color a, Color b, int tolerance = 3)
{
    auto const near = [tolerance](std::uint8_t x, std::uint8_t y) {
        return (x > y ? x - y : y - x) <= tolerance;
    };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

std::string page_with(std::string_view body)
{
    return "<!doctype html><html><head><style>body { margin: 0 } div { box-sizing: border-box }</style></head><body>"
        + std::string(body) + "</body></html>";
}

std::string id_at(Page const& page, float x, float y)
{
    std::optional<paint::PointHit> const hit = paint::hit_test(page.result.root, x, y);
    if (!hit || !hit->element)
        return "none";
    dom::Attr const* const id = hit->element->find_attribute("id");
    return id ? id->value : hit->element->local_name();
}

Color const white = Color::rgb(255, 255, 255);
Color const red = Color::rgb(255, 0, 0);

} // namespace

int main()
{
    text::FontManager::instance().set_system_fonts(false);

    // --- The values through the cascade ------------------------------------------
    {
        Page const page = lay_out(page_with(R"HTML(
<div id="none"></div>
<div id="list" style="color: rgb(1, 2, 3); filter: blur(4px) brightness(50%) contrast(2) grayscale() hue-rotate(0.25turn) invert(30%) opacity(.5) saturate(3) sepia(150%) drop-shadow(1px 2px 3px)"></div>
<div id="shadow" style="filter: drop-shadow(rgb(9, 8, 7) -4px 5px)"></div>
<div id="webkit" style="-webkit-filter: blur(2px); -webkit-backdrop-filter: invert(1); -webkit-clip-path: inset(1px)"></div>
<div id="off" style="filter: blur(2px); filter: none; clip-path: circle(); clip-path: none"></div>
<div id="bad1" style="filter: blur(-1px)"></div><div id="bad2" style="filter: blur(10%)"></div>
<div id="bad3" style="filter: brightness(-1)"></div><div id="bad4" style="filter: url(#f)"></div>
<div id="bad5" style="filter: blur(1px) nonsense(2)"></div><div id="bad6" style="clip-path: circle(-5px)"></div>
<div id="bad7" style="clip-path: polygon(1px)"></div><div id="bad8" style="clip-path: url(#c)"></div>
<div id="circle" style="clip-path: circle(40% at 10px 20%)"></div>
<div id="ellipse" style="clip-path: ellipse(closest-side farthest-side at right bottom) padding-box"></div>
<div id="inset" style="clip-path: inset(1px 2px 3px 4px round 5px 6px / 7px)"></div>
<div id="polygon" style="clip-path: polygon(evenodd, 0 0, 100% 0, 50% 10px)"></div>
<div id="path" style="clip-path: path('M0 0 L10 0 L0 10 Z')"></div>
<div id="box" style="clip-path: content-box"></div>
)HTML"));
        using Kind = css::FilterFunction::Kind;
        css::ComputedStyle const* const none = style_of(page, "none");
        css::ComputedStyle const* const list = style_of(page, "list");
        if (CHECK(none && list)) {
            CHECK(!none->filter && !none->backdrop_filter && !none->clip_path);
            CHECK(!none->paints_through_effects());
            if (CHECK(list->filter && list->filter->size() == 10)) {
                css::FilterList const& f = *list->filter;
                Kind const kinds[] = { Kind::Blur, Kind::Brightness, Kind::Contrast, Kind::Grayscale, Kind::HueRotate, Kind::Invert,
                    Kind::Opacity, Kind::Saturate, Kind::Sepia, Kind::DropShadow };
                float const amounts[] = { 4, 0.5f, 2, 1, 90, 0.3f, 0.5f, 3, 1, 3 }; // sepia's 150% is held to 1
                for (std::size_t i = 0; i < 10; ++i) {
                    CHECK(f[i].kind == kinds[i]);
                    CHECK(f[i].amount > amounts[i] - 0.001f && f[i].amount < amounts[i] + 0.001f);
                }
                CHECK_EQ(f[9].x, 1.0f);
                CHECK_EQ(f[9].y, 2.0f);
                CHECK(f[9].current_color); // no color written: the element's own
            }
            CHECK(list->paints_through_effects() && list->effects_stack() && list->contains_absolute());
        }
        if (css::ComputedStyle const* const shadow = style_of(page, "shadow"); CHECK(shadow && shadow->filter && shadow->filter->size() == 1)) {
            css::FilterFunction const& f = shadow->filter->front();
            CHECK(!f.current_color);
            CHECK(f.color == Color::rgb(9, 8, 7));
            CHECK_EQ(f.x, -4.0f);
            CHECK_EQ(f.y, 5.0f);
            CHECK_EQ(f.amount, 0.0f);
        }
        if (css::ComputedStyle const* const webkit = style_of(page, "webkit"); CHECK(webkit))
            CHECK(webkit->filter && webkit->backdrop_filter && webkit->clip_path);
        if (css::ComputedStyle const* const off = style_of(page, "off"); CHECK(off))
            CHECK(!off->filter && !off->clip_path);
        for (std::string_view const id : { "bad1", "bad2", "bad3", "bad4", "bad5", "bad6", "bad7", "bad8" }) {
            css::ComputedStyle const* const bad = style_of(page, id);
            CHECK(bad && !bad->filter && !bad->clip_path);
        }
        using Shape = css::ClipPath::Shape;
        if (css::ComputedStyle const* const circle = style_of(page, "circle"); CHECK(circle && circle->clip_path)) {
            css::ClipPath const& clip = *circle->clip_path;
            CHECK(clip.shape == Shape::Circle && clip.box == css::ClipPath::Box::Border);
            CHECK(clip.radius_x.kind == css::ClipPath::Radius::Kind::Length);
            CHECK_EQ(clip.radius_x.length.value, 40.0f);
            CHECK_EQ(clip.center_x.value, 10.0f);
            CHECK_EQ(clip.center_y.value, 20.0f);
        }
        if (css::ComputedStyle const* const ellipse = style_of(page, "ellipse"); CHECK(ellipse && ellipse->clip_path)) {
            css::ClipPath const& clip = *ellipse->clip_path;
            CHECK(clip.shape == Shape::Ellipse && clip.box == css::ClipPath::Box::Padding);
            CHECK(clip.radius_x.kind == css::ClipPath::Radius::Kind::ClosestSide);
            CHECK(clip.radius_y.kind == css::ClipPath::Radius::Kind::FarthestSide);
            CHECK_EQ(clip.center_x.value, 100.0f);
            CHECK_EQ(clip.center_y.value, 100.0f);
        }
        if (css::ComputedStyle const* const inset = style_of(page, "inset"); CHECK(inset && inset->clip_path)) {
            css::ClipPath const& clip = *inset->clip_path;
            CHECK(clip.shape == Shape::Inset);
            CHECK_EQ(clip.inset[0].value, 1.0f);
            CHECK_EQ(clip.inset[1].value, 2.0f);
            CHECK_EQ(clip.inset[2].value, 3.0f);
            CHECK_EQ(clip.inset[3].value, 4.0f);
            // 5px 6px / 7px: across 5, 6, 5, 6 and down 7 at every corner.
            CHECK_EQ(clip.radii[0].value, 5.0f);
            CHECK_EQ(clip.radii[2].value, 6.0f);
            CHECK_EQ(clip.radii[4].value, 5.0f);
            CHECK_EQ(clip.radii[6].value, 6.0f);
            CHECK_EQ(clip.radii[1].value, 7.0f);
            CHECK_EQ(clip.radii[7].value, 7.0f);
        }
        if (css::ComputedStyle const* const polygon = style_of(page, "polygon"); CHECK(polygon && polygon->clip_path)) {
            CHECK(polygon->clip_path->shape == Shape::Polygon && polygon->clip_path->even_odd);
            CHECK_EQ(polygon->clip_path->points.size(), std::size_t { 3 });
        }
        if (css::ComputedStyle const* const path = style_of(page, "path"); CHECK(path && path->clip_path)) {
            CHECK(path->clip_path->shape == Shape::Path);
            CHECK_EQ(path->clip_path->path, std::string("M0 0 L10 0 L0 10 Z"));
        }
        if (css::ComputedStyle const* const box = style_of(page, "box"); CHECK(box && box->clip_path))
            CHECK(box->clip_path->shape == Shape::Box && box->clip_path->box == css::ClipPath::Box::Content);
    }

    // --- What a filter leaves of a color ------------------------------------------
    // Six 20 px squares in a row, each through one function.
    {
        Page const page = lay_out(page_with(R"HTML(<div style="display: flex">
<div style="width: 20px; height: 20px; background: rgb(255, 0, 0); filter: opacity(0.5)"></div>
<div style="width: 20px; height: 20px; background: rgb(255, 0, 0); filter: invert(1)"></div>
<div style="width: 20px; height: 20px; background: rgb(255, 0, 0); filter: grayscale(1)"></div>
<div style="width: 20px; height: 20px; background: rgb(200, 100, 0); filter: brightness(0.5)"></div>
<div style="width: 20px; height: 20px; background: rgb(200, 100, 50); filter: contrast(0)"></div>
<div style="width: 20px; height: 20px; background: rgb(255, 0, 0); filter: invert(1) opacity(0.5)"></div>
<div style="width: 20px; height: 20px; background: rgb(255, 0, 0); filter: saturate(0)"></div>
<div style="width: 20px; height: 20px; background: rgb(255, 0, 0); filter: hue-rotate(180deg)"></div>
<div style="width: 20px; height: 20px; background: rgb(255, 0, 0); filter: blur(0)"></div>
</div>)HTML"));
        Bitmap const canvas = paint_canvas(page, 200, 40);
        CHECK(same(canvas.pixel(10, 10), Color::rgb(255, 128, 128)));
        CHECK(same(canvas.pixel(30, 10), Color::rgb(0, 255, 255)));
        CHECK(same(canvas.pixel(50, 10), Color::rgb(54, 54, 54)));
        CHECK(same(canvas.pixel(70, 10), Color::rgb(100, 50, 0)));
        CHECK(same(canvas.pixel(90, 10), Color::rgb(128, 128, 128)));
        CHECK(same(canvas.pixel(110, 10), Color::rgb(128, 255, 255))); // in the order written
        CHECK(same(canvas.pixel(130, 10), Color::rgb(54, 54, 54)));
        CHECK(same(canvas.pixel(150, 10), Color::rgb(0, 109, 109))); // half a turn of the hue, by the standard's matrix
        CHECK(same(canvas.pixel(170, 10), red));
        CHECK(same(canvas.pixel(10, 30), white)); // nothing below the row
    }

    // --- A group keeps its own transparency ---------------------------------------
    // The filter changes what the box painted and nothing of what is behind
    // it: the black child inverts to white, the yellow page under the rest
    // of the (transparent) box stays yellow.
    {
        Page const page = lay_out(page_with(R"HTML(<div style="background: rgb(255, 255, 0); width: 100px; height: 60px; padding: 10px">
<div style="width: 40px; height: 40px; filter: invert(1)"><div style="width: 10px; height: 10px; background: rgb(0, 0, 0)"></div></div></div>)HTML"));
        Bitmap const canvas = paint_canvas(page, 120, 80);
        CHECK(same(canvas.pixel(15, 15), white));
        CHECK(same(canvas.pixel(35, 35), Color::rgb(255, 255, 0)));
        CHECK(same(canvas.pixel(80, 30), Color::rgb(255, 255, 0)));
    }

    // --- A blur reaches past the box, a drop shadow lies under it -----------------
    {
        Page const page = lay_out(page_with(R"HTML(
<div style="position: absolute; left: 30px; top: 30px; width: 40px; height: 40px; background: rgb(0, 0, 0); filter: blur(4px)"></div>
<div style="position: absolute; left: 120px; top: 10px; width: 20px; height: 20px; background: rgb(255, 0, 0); filter: drop-shadow(10px 10px 0 rgb(0, 0, 255))"></div>
<div id="soft" style="position: absolute; left: 200px; top: 10px; width: 20px; height: 20px; color: rgb(0, 128, 0); background: rgb(255, 0, 0); filter: drop-shadow(0 30px 2px)"></div>
)HTML"));
        Bitmap const canvas = paint_canvas(page, 260, 100);
        // The middle of a box ten deviations wide stays black; two px
        // outside its edge is a gray; four deviations out is still white.
        CHECK(canvas.pixel(50, 50).r < 8);
        Color const edge = canvas.pixel(28, 50);
        CHECK(edge.r > 100 && edge.r < 215 && edge.r == edge.g && edge.g == edge.b);
        Color const inside_edge = canvas.pixel(32, 50);
        CHECK(inside_edge.r > 40 && inside_edge.r < 155);
        CHECK(same(canvas.pixel(12, 50), white));
        CHECK(same(canvas.pixel(50, 88), white));
        // The shadow is the box's shape moved by its offset, in its color,
        // under the box.
        CHECK(same(canvas.pixel(125, 15), red));
        CHECK(same(canvas.pixel(135, 25), red));
        CHECK(same(canvas.pixel(145, 35), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(145, 15), white));
        CHECK(same(canvas.pixel(125, 35), white));
        // Without a color the shadow takes the element's own; its blur
        // softens the edge.
        CHECK(same(canvas.pixel(210, 50), Color::rgb(0, 128, 0), 6));
        Color const soft = canvas.pixel(210, 40);
        CHECK(soft.r > 60 && soft.r < 200);
    }

    // --- A backdrop filter changes what is behind the box, inside its shape -------
    {
        Page const page = lay_out(page_with(R"HTML(<div style="background: rgb(255, 0, 0); width: 200px; height: 100px">
<div style="position: absolute; left: 20px; top: 20px; width: 60px; height: 60px; border-radius: 30px; backdrop-filter: invert(1)"></div>
<div style="position: absolute; left: 120px; top: 20px; width: 40px; height: 40px; background: rgba(0, 0, 0, 0.5); backdrop-filter: invert(1)"></div>
</div>)HTML"));
        Bitmap const canvas = paint_canvas(page, 200, 100);
        CHECK(same(canvas.pixel(50, 50), Color::rgb(0, 255, 255)));
        CHECK(same(canvas.pixel(22, 22), red)); // outside the rounded corner
        CHECK(same(canvas.pixel(10, 50), red));
        // The box's own half-black background lies over its filtered backdrop.
        CHECK(same(canvas.pixel(140, 40), Color::rgb(0, 128, 128)));
    }

    // --- A clip path cuts the box and what is in it to a shape --------------------
    {
        Page const page = lay_out(page_with(R"HTML(<div style="display: flex">
<div id="circle" style="width: 40px; height: 40px; background: rgb(255, 0, 0); clip-path: circle(50%)"></div>
<div id="inset" style="width: 40px; height: 40px; background: rgb(255, 0, 0); clip-path: inset(10px)"><div id="kid" style="width: 40px; height: 40px"></div></div>
<div id="triangle" style="width: 40px; height: 40px; background: rgb(255, 0, 0); clip-path: polygon(0 0, 100% 0, 0 100%)"></div>
<div id="path" style="width: 40px; height: 40px; background: rgb(255, 0, 0); clip-path: path('M0 0 L20 0 L20 20 L0 20 Z')"></div>
<div id="content" style="width: 40px; height: 40px; padding: 10px; background: rgb(255, 0, 0); clip-path: content-box"></div>
<div id="ring" style="width: 40px; height: 40px; background: rgb(255, 0, 0); clip-path: polygon(evenodd, 0 0, 40px 0, 40px 40px, 0 40px, 10px 10px, 10px 30px, 30px 30px, 30px 10px, 10px 10px, 0 40px)"></div>
<div id="gone" style="width: 40px; height: 40px; background: rgb(255, 0, 0); clip-path: polygon(0 0, 0 0, 0 0)"></div>
<div id="faded" style="width: 40px; height: 40px; background: rgb(255, 0, 0); clip-path: inset(0 20px 0 0); opacity: 0.5"></div>
</div>)HTML"));
        Bitmap const canvas = paint_canvas(page, 320, 60);
        CHECK(same(canvas.pixel(20, 20), red));
        CHECK(same(canvas.pixel(1, 1), white));
        CHECK(same(canvas.pixel(38, 38), white));
        CHECK(same(canvas.pixel(60, 20), red));
        CHECK(same(canvas.pixel(45, 20), white));
        CHECK(same(canvas.pixel(60, 5), white));
        CHECK(same(canvas.pixel(85, 5), red));
        CHECK(same(canvas.pixel(115, 35), white));
        CHECK(same(canvas.pixel(130, 10), red));
        CHECK(same(canvas.pixel(150, 10), white));
        CHECK(same(canvas.pixel(130, 30), white));
        CHECK(same(canvas.pixel(180, 20), red));
        CHECK(same(canvas.pixel(165, 20), white));
        CHECK(same(canvas.pixel(205, 5), red)); // the ring's band
        CHECK(same(canvas.pixel(220, 20), white)); // its hole, by the even-odd rule
        CHECK(same(canvas.pixel(260, 20), white));
        CHECK(same(canvas.pixel(290, 20), Color::rgb(255, 128, 128))); // the shape, then the opacity
        CHECK(same(canvas.pixel(310, 20), white));
        // What is cut away is not there to point at: the pointer goes to
        // what is behind it. Inside the shape the box (or its child) answers.
        CHECK_EQ(id_at(page, 20, 20), std::string("circle"));
        CHECK(id_at(page, 1, 1) != "circle");
        CHECK_EQ(id_at(page, 60, 20), std::string("kid"));
        CHECK(id_at(page, 45, 20) != "kid" && id_at(page, 45, 20) != "inset");
        CHECK_EQ(id_at(page, 85, 5), std::string("triangle"));
        CHECK(id_at(page, 115, 35) != "triangle");
        CHECK_EQ(id_at(page, 205, 5), std::string("ring"));
        CHECK(id_at(page, 220, 20) != "ring");
        CHECK(id_at(page, 260, 20) != "gone");
    }

    // --- Borders of different colors meet on the corner's diagonal ---------------
    // CSS 2.1 §8.5.4: each side is the trapezoid the diagonals leave it — a
    // triangle when the box inside has no size, which is how pages draw
    // arrows. A border the page shows through is not laid twice over its
    // corners.
    {
        Page const page = lay_out(page_with(R"HTML(
<div style="position: absolute; left: 10px; top: 10px; width: 0; height: 0; border-top: 40px solid rgb(255, 0, 0); border-left: 40px solid transparent"></div>
<div style="position: absolute; left: 100px; top: 10px; width: 0; height: 0; border: 20px solid; border-color: rgb(255, 0, 0) rgb(0, 128, 0) rgb(0, 0, 255) rgb(255, 255, 0)"></div>
<div style="position: absolute; left: 200px; top: 10px; width: 40px; height: 40px; border: 10px solid rgba(255, 0, 0, 0.5)"></div>
<div style="position: absolute; left: 300px; top: 10px; width: 40px; height: 40px; border: 10px solid rgb(255, 0, 0)"></div>
)HTML"));
        Bitmap const canvas = paint_canvas(page, 400, 100);
        CHECK(same(canvas.pixel(40, 15), red)); // above the diagonal: the top border
        CHECK(same(canvas.pixel(30, 20), red));
        CHECK(same(canvas.pixel(15, 40), white)); // below it: the left border, which is transparent
        CHECK(same(canvas.pixel(20, 30), white));
        CHECK(same(canvas.pixel(120, 15), red)); // four triangles about (120, 30)
        CHECK(same(canvas.pixel(135, 30), Color::rgb(0, 128, 0)));
        CHECK(same(canvas.pixel(120, 45), Color::rgb(0, 0, 255)));
        CHECK(same(canvas.pixel(105, 30), Color::rgb(255, 255, 0)));
        CHECK(same(canvas.pixel(220, 13), Color::rgb(255, 128, 128))); // a side
        CHECK(same(canvas.pixel(203, 13), Color::rgb(255, 128, 128))); // a corner, as light as the side
        CHECK(same(canvas.pixel(237, 47), Color::rgb(255, 128, 128)));
        CHECK(same(canvas.pixel(220, 30), white));
        CHECK(same(canvas.pixel(303, 13), red)); // one opaque color all round
        CHECK(same(canvas.pixel(320, 30), white));
    }

    return sashfold::test::report("effects");
}
