#include "Test.h"

#include "css/StyleResolver.h"
#include "css/Transform.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "paint/Painter.h"
#include "text/FontManager.h"

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Transforms (css-transforms-1 and -2): the matrices the functions come
// to, how two lists meet between keyframes, the values through the
// cascade, where layout leaves a transformed box, what the painter makes
// of one, and that the pointer reaches it where it is drawn.

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

layout::Fragment const* box_of(layout::Fragment const& fragment, std::string_view id)
{
    if (fragment.element) {
        if (dom::Attr const* attribute = fragment.element->find_attribute("id"); attribute && attribute->value == id)
            return &fragment;
    }
    for (layout::Fragment const& child : fragment.children) {
        if (layout::Fragment const* found = box_of(child, id))
            return found;
    }
    return nullptr;
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
    return "<!doctype html><html><head><style>body { margin: 0 } div { box-sizing: border-box; position: absolute }"
           " .red { background: rgb(255, 0, 0) } .blue { background: rgb(0, 0, 255) }</style></head><body>"
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

bool near(double a, double b, double by = 1e-6) { return std::fabs(a - b) <= by; }

std::string list_text(css::ComputedStyle const* style)
{
    if (!style || !style->transforms || !style->transforms->list)
        return "none";
    return css::serialize(*style->transforms->list);
}

std::shared_ptr<css::TransformList const> list_of(Page const& page, std::string_view id)
{
    css::ComputedStyle const* const style = style_of(page, id);
    return style && style->transforms ? style->transforms->list : nullptr;
}

Color const white = Color::rgb(255, 255, 255);
Color const red = Color::rgb(255, 0, 0);
Color const blue = Color::rgb(0, 0, 255);

} // namespace

int main()
{
    text::FontManager::instance().set_system_fonts(false);
    using css::Matrix4;
    using css::PlaneTransform;

    // --- Matrices ----------------------------------------------------------------
    {
        // A quarter turn is zeros and ones: x goes to y, y to minus x.
        Matrix4 const quarter = Matrix4::rotation(0, 0, 1, 90);
        CHECK_EQ(css::serialize(quarter), std::string("matrix(0, 1, -1, 0, 0, 0)"));
        CHECK_EQ(css::serialize(Matrix4::rotation(0, 0, 1, 180)), std::string("matrix(-1, 0, 0, -1, 0, 0)"));
        CHECK_EQ(css::serialize(Matrix4::scaling(2, 3)), std::string("matrix(2, 0, 0, 3, 0, 0)"));
        CHECK_EQ(css::serialize(Matrix4::skewing(45, 0)), std::string("matrix(1, 0, 1, 1, 0, 0)"));
        CHECK_EQ(css::serialize(Matrix4::translation(5, 6)), std::string("matrix(1, 0, 0, 1, 5, 6)"));
        CHECK_EQ(css::serialize(Matrix4::translation(5, 6, 7)), std::string("matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1)"));
        CHECK_EQ(css::serialize(Matrix4::perspective(100)), std::string("matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -0.01, 0, 0, 0, 1)"));
        // The second written is applied first.
        CHECK_EQ(css::serialize(Matrix4::translation(10, 0) * Matrix4::scaling(2, 2)), std::string("matrix(2, 0, 0, 2, 10, 0)"));
        CHECK_EQ(css::serialize(Matrix4::scaling(2, 2) * Matrix4::translation(10, 0)), std::string("matrix(2, 0, 0, 2, 20, 0)"));
        CHECK(Matrix4::translation(3, 4).is_translation_2d());
        CHECK(!Matrix4::scaling(2, 2).is_translation_2d());
        CHECK(!Matrix4::translation(3, 4, 5).is_2d());
        // An inverse undoes its matrix, perspective and all.
        Matrix4 const mixed = Matrix4::perspective(300) * Matrix4::rotation(1, 2, 3, 37) * Matrix4::translation(4, 5, 6)
            * Matrix4::scaling(2, 0.5, 3) * Matrix4::skewing(10, 20);
        std::optional<Matrix4> const back = mixed.inverse();
        if (CHECK(back.has_value())) {
            Matrix4 const whole = mixed * *back;
            bool identity = true;
            for (std::size_t column = 0; column < 4; ++column) {
                for (std::size_t row = 0; row < 4; ++row)
                    identity = identity && near(whole.m[column][row], column == row ? 1 : 0, 1e-9);
            }
            CHECK(identity);
        }
        CHECK(!Matrix4::scaling(0, 1).inverse().has_value());
        // A box turned half round about the y axis shows its back; one
        // mirrored along x does not.
        CHECK(Matrix4::rotation(0, 1, 0, 180).shows_back());
        CHECK(Matrix4::rotation(1, 0, 0, 120).shows_back());
        CHECK(!Matrix4::rotation(0, 1, 0, 60).shows_back());
        CHECK(!Matrix4::scaling(-1, 1).shows_back());
        CHECK(!Matrix4::rotation(0, 0, 1, 180).shows_back());

        // The plane: a point through a quarter turn about (10, 10).
        PlaneTransform const turn = PlaneTransform::of(quarter).about(10, 10);
        double x = 0;
        double y = 0;
        CHECK(turn.map(20, 10, x, y));
        CHECK(near(x, 10) && near(y, 20));
        double left = 0;
        double top = 0;
        double right = 0;
        double bottom = 0;
        CHECK(turn.map_rect(10, 10, 30, 10, left, top, right, bottom)); // 30 wide, 10 tall, from the origin
        CHECK(near(left, 0) && near(top, 10) && near(right, 10) && near(bottom, 40));
        std::optional<PlaneTransform> const unturn = turn.inverse();
        if (CHECK(unturn.has_value())) {
            CHECK(unturn->map(10, 20, x, y));
            CHECK(near(x, 20) && near(y, 10));
        }
        // Perspective: what is nearer is larger, and a point behind the
        // viewer is nowhere.
        PlaneTransform const tilted = PlaneTransform::of(Matrix4::perspective(100) * Matrix4::rotation(0, 1, 0, 60));
        CHECK(tilted.map(-10, 0, x, y));
        double const near_side = x;
        CHECK(tilted.map(10, 0, x, y));
        CHECK(-near_side > x); // the left edge came forward
        CHECK(!tilted.map(-1000, 0, x, y));
    }

    // --- Between two transforms ---------------------------------------------------
    {
        auto const list = [](std::string_view declaration) {
            Page const page = lay_out(page_with("<div id=\"a\" style=\"" + std::string(declaration) + "\"></div>"));
            return list_of(page, "a");
        };
        auto const between = [&](std::string_view from, std::string_view to, double p) -> std::string {
            std::shared_ptr<css::TransformList const> const met = css::interpolate(list(from), list(to), p);
            return met ? css::serialize(*met) : "none";
        };
        // none stands for the function that does nothing: a turn from none
        // goes all the way round, not the short way to where it started.
        CHECK_EQ(between("transform: none", "transform: rotate(360deg)", 0.25), std::string("rotate(90deg)"));
        CHECK_EQ(between("transform: rotate(0deg)", "transform: rotate(720deg)", 0.5), std::string("rotate(360deg)"));
        CHECK_EQ(between("transform: none", "transform: none", 0.5), std::string("none"));
        CHECK_EQ(between("transform: scale(1)", "transform: scale(3)", 0.5), std::string("scale(2)"));
        CHECK_EQ(between("transform: none", "transform: scale(3, 5)", 0.5), std::string("scale(2, 3)"));
        CHECK_EQ(between("transform: translate(10px, 20px)", "transform: translate(30px, 60px)", 0.5), std::string("translate(20px, 40px)"));
        CHECK_EQ(between("transform: translate(10px)", "transform: translate(50%)", 0.5), std::string("translate(calc(25% + 5px))"));
        CHECK_EQ(between("transform: translateX(10px)", "transform: translateY(20px)", 0.5), std::string("translate(5px, 10px)"));
        CHECK_EQ(between("transform: translateX(10px)", "transform: translateZ(20px)", 0.5), std::string("translate3d(5px, 0px, 10px)"));
        CHECK_EQ(between("transform: skewX(10deg)", "transform: skewX(30deg)", 0.5), std::string("skewX(20deg)"));
        CHECK_EQ(between("transform: rotateX(10deg)", "transform: rotateX(30deg)", 0.5), std::string("rotateX(20deg)"));
        CHECK_EQ(between("transform: rotate(10deg) scale(2)", "transform: rotate(30deg) scale(4)", 0.5),
            std::string("rotate(20deg) scale(3)"));
        // A shorter list goes on with functions that do nothing.
        CHECK_EQ(between("transform: rotate(10deg)", "transform: rotate(30deg) scale(4)", 0.5), std::string("rotate(20deg) scale(2.5)"));
        // Lists that do not pair up meet as matrices: a quarter turn and a
        // doubling, halfway, is an eighth turn at one and a half.
        {
            std::shared_ptr<css::TransformList const> const met
                = css::interpolate(list("transform: rotate(90deg)"), list("transform: scale(2)"), 0.5);
            if (CHECK(met && met->functions.size() == 1)) {
                Matrix4 const matrix = met->matrix(0, 0);
                double const expected = 1.5 * std::sqrt(0.5);
                CHECK(near(matrix.m[0][0], expected, 1e-9) && near(matrix.m[0][1], expected, 1e-9));
                CHECK(near(matrix.m[1][0], -expected, 1e-9) && near(matrix.m[1][1], expected, 1e-9));
            }
        }
        // Matrices meet through what they are made of: the turn turns, it
        // does not shrink through the middle as its numbers would.
        {
            Matrix4 const met = css::interpolate(Matrix4::rotation(0, 0, 1, 0), Matrix4::rotation(0, 0, 1, 90), 0.5);
            CHECK(near(met.m[0][0], std::sqrt(0.5), 1e-9) && near(met.m[0][1], std::sqrt(0.5), 1e-9));
            Matrix4 const moved = css::interpolate(Matrix4::translation(10, 0) * Matrix4::scaling(2, 2),
                Matrix4::translation(30, 20) * Matrix4::scaling(4, 4), 0.5);
            CHECK_EQ(css::serialize(moved), std::string("matrix(3, 0, 0, 3, 20, 10)"));
            // The ends are themselves.
            Matrix4 const skewed = Matrix4::translation(7, 8) * Matrix4::rotation(0, 0, 1, 30) * Matrix4::skewing(20, 0)
                * Matrix4::scaling(2, 3);
            Matrix4 const at_start = css::interpolate(skewed, Matrix4 {}, 0);
            Matrix4 const at_end = css::interpolate(Matrix4 {}, skewed, 1);
            bool ends = true;
            for (std::size_t column = 0; column < 4; ++column) {
                for (std::size_t row = 0; row < 4; ++row) {
                    ends = ends && near(at_start.m[column][row], skewed.m[column][row], 1e-9)
                        && near(at_end.m[column][row], skewed.m[column][row], 1e-9);
                }
            }
            CHECK(ends);
            // In three dimensions too.
            Matrix4 const turned = Matrix4::perspective(500) * Matrix4::translation(1, 2, 3) * Matrix4::rotation(1, 1, 0, 50)
                * Matrix4::scaling(2, 3, 4);
            // The same transform, written with its last number at one (a
            // matrix times a number moves every point as the matrix does).
            Matrix4 const whole = css::interpolate(Matrix4 {}, turned, 1);
            bool same_3d = true;
            for (std::size_t column = 0; column < 4; ++column) {
                for (std::size_t row = 0; row < 4; ++row)
                    same_3d = same_3d && near(whole.m[column][row], turned.m[column][row] / turned.m[3][3], 1e-6);
            }
            CHECK(same_3d);
            CHECK(near(whole.m[3][3], 1, 1e-12));
            Matrix4 const half = css::interpolate(Matrix4::rotation(0, 1, 0, 0), Matrix4::rotation(0, 1, 0, 80), 0.5);
            Matrix4 const expected = Matrix4::rotation(0, 1, 0, 40);
            CHECK(near(half.m[0][0], expected.m[0][0], 1e-9) && near(half.m[0][2], expected.m[0][2], 1e-9)
                && near(half.m[2][0], expected.m[2][0], 1e-9));
        }
        // The rotate property: about one axis the angle moves; from one
        // axis to another the turn goes the short way.
        {
            css::RotateValue const about_z = css::interpolate(css::RotateValue { 0, 0, 1, 0 }, css::RotateValue { 0, 0, 1, 90 }, 0.5);
            CHECK(near(about_z.degrees, 45) && near(about_z.z, 1));
            css::RotateValue const from_none = css::interpolate(css::RotateValue {}, css::RotateValue { 1, 0, 0, 60 }, 0.5);
            CHECK(near(from_none.degrees, 30) && near(from_none.x, 1));
            css::RotateValue const across = css::interpolate(css::RotateValue { 1, 0, 0, 90 }, css::RotateValue { 0, 1, 0, 90 }, 0.5);
            CHECK(across.degrees > 0 && near(across.x, across.y, 1e-9)); // halfway between the two axes
        }
    }

    // --- The values through the cascade ------------------------------------------
    {
        Page const page = lay_out(page_with(R"HTML(
<div id="none"></div>
<div id="list" style="font-size: 10px; transform: translate(1em, 50%) rotate(0.25turn) scale(2, 150%) skew(10deg, 200grad) matrix(1, 2, 3, 4, 5, 6) perspective(20px) translate3d(1px, 2%, 3px) rotate3d(1, 2, 3, 40deg) scaleZ(4) translateZ(5px) rotateX(1deg) rotateY(2deg) rotateZ(3deg) scaleX(6) scaleY(7) skewX(8deg) skewY(9deg) translateX(1px) translateY(2px) scale3d(1, 2, 3) perspective(none)"></div>
<div id="calc" style="transform: rotate(calc(15deg * 3)) scale(calc(1 + 1)) translate(calc(10px + 5%))"></div>
<div id="zero" style="transform: rotate(0) translate(0)"></div>
<div id="webkit" style="-webkit-transform: scale(2); -webkit-transform-origin: 10px 20px"></div>
<div id="off" style="transform: scale(2); transform: none"></div>
<div id="bad1" style="transform: scale(2); transform: rotate(45)"></div>
<div id="bad2" style="transform: scale(2); transform: translate(10px 20px)"></div>
<div id="bad3" style="transform: scale(2); transform: scale()"></div>
<div id="bad4" style="transform: scale(2); transform: rotate(45deg) nothing"></div>
<div id="bad5" style="transform: scale(2); transform: matrix(1, 2, 3)"></div>
<div id="bad6" style="transform: scale(2); transform: translateZ(10%)"></div>
<div id="bad7" style="transform: scale(2); transform: perspective(-1px)"></div>
<div id="props" style="translate: 10px 20% 30px; rotate: x 45deg; scale: 2 50%"></div>
<div id="props2" style="translate: 5px; rotate: 90deg; scale: 3"></div>
<div id="props3" style="rotate: 1 2 3 0.5turn; scale: 1 2 3"></div>
<div id="props4" style="rotate: 45deg z; translate: none; scale: none"></div>
<div id="props5" style="rotate: 0 0 -2 30deg"></div><div id="props6" style="rotate: 100grad 0 2 0"></div>
<div id="badp" style="rotate: 10deg; rotate: 100px; scale: 2; scale: 1 2 3 4; translate: 1px; translate: 1px 2px 3%"></div>
<div id="badr" style="rotate: 10deg; rotate: x y 45deg"></div><div id="badr2" style="rotate: 10deg; rotate: 1 2 45deg"></div>
<div id="badr3" style="rotate: 10deg; rotate: 1 45deg 2 3"></div>
<div id="origin1" style="transform-origin: left"></div><div id="origin2" style="transform-origin: bottom"></div>
<div id="origin3" style="transform-origin: 10px 20%"></div><div id="origin4" style="transform-origin: top right"></div>
<div id="origin5" style="transform-origin: right 5px 7px"></div><div id="origin6" style="transform-origin: center"></div>
<div id="origin7" style="transform-origin: 10px; transform-origin: 5px left"></div>
<div id="origin8" style="transform-origin: 10px; transform-origin: top top"></div>
<div id="rest" style="transform-box: content-box; transform-style: preserve-3d; backface-visibility: hidden; perspective-origin: left 25%"></div>
<div id="inherit" style="transform: scale(3); rotate: 5deg"><div id="kid" style="transform: inherit"></div><div id="kid2"></div></div>
)HTML"));
        css::ComputedStyle const* const none = style_of(page, "none");
        CHECK(none && !none->transforms && !none->transformed());
        CHECK(none && !none->effects_stack() && !none->contains_fixed());
        CHECK_EQ(list_text(style_of(page, "list")),
            std::string("translate(10px, 50%) rotate(90deg) scale(2, 1.5) skew(10deg, 180deg) matrix(1, 2, 3, 4, 5, 6) perspective(20px) "
                        "translate3d(1px, 2%, 3px) rotate3d(1, 2, 3, 40deg) scaleZ(4) translateZ(5px) rotateX(1deg) rotateY(2deg) "
                        "rotateZ(3deg) scaleX(6) scaleY(7) skewX(8deg) skewY(9deg) translateX(1px) translateY(2px) scale3d(1, 2, 3) "
                        "perspective(none)"));
        css::ComputedStyle const* const list = style_of(page, "list");
        CHECK(list && list->transformed() && list->effects_stack() && list->contains_fixed() && list->contains_absolute());
        CHECK_EQ(list_text(style_of(page, "calc")), std::string("rotate(45deg) scale(2) translate(calc(5% + 10px))"));
        CHECK_EQ(list_text(style_of(page, "zero")), std::string("rotate(0deg) translate(0px)"));
        CHECK_EQ(list_text(style_of(page, "webkit")), std::string("scale(2)"));
        css::ComputedStyle const* const webkit = style_of(page, "webkit");
        CHECK(webkit && webkit->transforms && near(webkit->transforms->origin_x.px, 10) && near(webkit->transforms->origin_y.px, 20));
        CHECK_EQ(list_text(style_of(page, "off")), std::string("none"));
        CHECK(style_of(page, "off") && !style_of(page, "off")->transformed());
        for (std::string_view const bad : { "bad1", "bad2", "bad3", "bad4", "bad5", "bad6", "bad7" })
            CHECK_EQ(list_text(style_of(page, bad)), std::string("scale(2)"));

        auto const props = [&](std::string_view id) {
            css::ComputedStyle const* const style = style_of(page, id);
            std::string out;
            if (!style || !style->transforms)
                return std::string("none none none");
            css::Transforms const& t = *style->transforms;
            out += t.translate ? css::serialize(*t.translate) : "none";
            out += " | " + (t.rotate ? css::serialize(*t.rotate) : std::string("none"));
            out += " | " + (t.scale ? css::serialize(*t.scale) : std::string("none"));
            return out;
        };
        CHECK_EQ(props("props"), std::string("10px 20% 30px | x 45deg | 2 0.5"));
        CHECK_EQ(props("props2"), std::string("5px | 90deg | 3"));
        CHECK_EQ(props("props3"), std::string("none | 1 2 3 180deg | 1 2 3"));
        CHECK_EQ(props("props4"), std::string("none | 45deg | none"));
        CHECK_EQ(props("props5"), std::string("none | -30deg | none"));
        CHECK_EQ(props("props6"), std::string("none | y 90deg | none")); // the angle first
        CHECK_EQ(props("badp"), std::string("1px | 10deg | 2"));
        CHECK_EQ(props("badr"), std::string("none | 10deg | none"));
        CHECK_EQ(props("badr2"), std::string("none | 10deg | none"));
        CHECK_EQ(props("badr3"), std::string("none | 10deg | none")); // the three numbers stand together
        CHECK(style_of(page, "props") && style_of(page, "props")->transformed());

        auto const origin = [&](std::string_view id) {
            css::ComputedStyle const* const style = style_of(page, id);
            css::Transforms const initial;
            css::Transforms const& t = style && style->transforms ? *style->transforms : initial;
            auto const part = [](css::TransformLength const& length) {
                return std::to_string(static_cast<int>(length.px)) + "px+" + std::to_string(static_cast<int>(length.percent)) + "%";
            };
            return part(t.origin_x) + " " + part(t.origin_y) + " " + std::to_string(static_cast<int>(t.origin_z));
        };
        CHECK_EQ(origin("none"), std::string("0px+50% 0px+50% 0"));
        CHECK_EQ(origin("origin1"), std::string("0px+0% 0px+50% 0"));
        CHECK_EQ(origin("origin2"), std::string("0px+50% 0px+100% 0"));
        CHECK_EQ(origin("origin3"), std::string("10px+0% 0px+20% 0"));
        CHECK_EQ(origin("origin4"), std::string("0px+100% 0px+0% 0"));
        CHECK_EQ(origin("origin5"), std::string("0px+100% 5px+0% 7"));
        CHECK_EQ(origin("origin6"), std::string("0px+50% 0px+50% 0"));
        CHECK_EQ(origin("origin7"), std::string("10px+0% 0px+50% 0")); // a length does not come before `left`
        CHECK_EQ(origin("origin8"), std::string("10px+0% 0px+50% 0"));
        // An origin alone transforms nothing.
        CHECK(style_of(page, "origin1") && !style_of(page, "origin1")->transformed());
        css::ComputedStyle const* const rest = style_of(page, "rest");
        if (CHECK(rest && rest->transforms)) {
            CHECK(rest->transforms->box == css::TransformBox::ContentBox);
            CHECK(rest->transforms->preserve_3d);
            CHECK(rest->transforms->backface_hidden);
            CHECK(near(rest->transforms->perspective_origin_x.percent, 0) && near(rest->transforms->perspective_origin_y.percent, 25));
            CHECK(!rest->transformed());
        }
        // Not inherited, unless asked.
        CHECK_EQ(list_text(style_of(page, "kid")), std::string("scale(3)"));
        CHECK_EQ(props("kid"), std::string("none | none | none"));
        CHECK(style_of(page, "kid2") && !style_of(page, "kid2")->transforms);
    }

    // --- Where layout leaves a transformed box -----------------------------------
    {
        Page const page = lay_out(page_with(R"HTML(
<div id="moved" style="left: 10px; top: 10px; width: 40px; height: 20px; transform: translate(50%, 5px)"><div id="in" style="left: 3px; top: 4px; width: 1px; height: 1px"></div></div>
<div id="moved2" style="left: 10px; top: 50px; width: 40px; height: 20px; translate: 7px 8px; transform: translateX(1px)"></div>
<div id="turned" style="left: 100px; top: 10px; width: 40px; height: 20px; transform: rotate(90deg)"></div>
<div id="origin" style="left: 200px; top: 10px; width: 40px; height: 20px; transform: scale(2); transform-origin: 0 0"></div>
<div id="all" style="left: 300px; top: 10px; width: 40px; height: 20px; translate: 10px; rotate: 90deg; scale: 2; transform: translateX(5px)"></div>
<div id="scroller" style="left: 0; top: 100px; width: 100px; height: 100px; overflow: auto"><div id="big" style="left: 0; top: 0; width: 50px; height: 50px; transform: scale(4); transform-origin: 0 0"></div></div>
<span id="inline" style="transform: translate(50px, 50px) rotate(45deg)">inline</span>
)HTML"));
        layout::Fragment const* const moved = box_of(page.result.root, "moved");
        layout::Fragment const* const inner = box_of(page.result.root, "in");
        layout::Fragment const* const moved2 = box_of(page.result.root, "moved2");
        layout::Fragment const* const turned = box_of(page.result.root, "turned");
        layout::Fragment const* const origin = box_of(page.result.root, "origin");
        layout::Fragment const* const all = box_of(page.result.root, "all");
        layout::Fragment const* const scroller = box_of(page.result.root, "scroller");
        if (CHECK(moved && inner && moved2 && turned && origin && all && scroller)) {
            // A move is a move: the box and what it holds are laid there.
            CHECK_EQ(moved->x, 30.0f);
            CHECK_EQ(moved->y, 15.0f);
            CHECK(!moved->transform && moved->stacking_context);
            CHECK_EQ(inner->x, 33.0f);
            CHECK_EQ(inner->y, 19.0f);
            CHECK_EQ(moved2->x, 18.0f); // translate, then transform
            CHECK_EQ(moved2->y, 58.0f);
            // Anything else stays where it was laid and carries its matrix.
            CHECK_EQ(turned->x, 100.0f);
            CHECK_EQ(turned->y, 10.0f);
            CHECK(turned->stacking_context);
            if (CHECK(turned->transform != nullptr)) {
                // About the box's middle (20, 10 from its corner): the
                // corner goes to the top right.
                double x = 0;
                double y = 0;
                CHECK(turned->transform->map(0, 0, x, y));
                CHECK(near(x, 30) && near(y, -10));
                CHECK(turned->transform->map(20, 10, x, y));
                CHECK(near(x, 20) && near(y, 10));
            }
            if (CHECK(origin->transform != nullptr)) {
                double x = 0;
                double y = 0;
                CHECK(origin->transform->map(40, 20, x, y));
                CHECK(near(x, 80) && near(y, 40));
            }
            if (CHECK(all->transform != nullptr)) {
                // translate, rotate, scale, then the list: the box's middle
                // moves by translate's 10 and by the list's 5 turned and
                // doubled — 10 down the page.
                double x = 0;
                double y = 0;
                CHECK(all->transform->map(20, 10, x, y));
                CHECK(near(x, 30) && near(y, 20));
            }
            // What a scroll container can be moved to show reaches as far
            // as the transform takes its content.
            CHECK_EQ(scroller->scroll_range_x, 100.0f);
            CHECK_EQ(scroller->scroll_range_y, 100.0f);
        }
        // An inline box that is not replaced is not transformable.
        std::vector<layout::TextRun const*> runs;
        std::function<void(layout::Fragment const&)> const collect = [&](layout::Fragment const& fragment) {
            for (layout::TextRun const& run : fragment.runs)
                runs.push_back(&run);
            for (layout::Fragment const& child : fragment.children)
                collect(child);
        };
        collect(page.result.root);
        bool found_inline = false;
        for (layout::TextRun const* run : runs) {
            if (run->text == U"inline") {
                found_inline = true;
                CHECK_EQ(run->x, 0.0f);
            }
        }
        CHECK(found_inline);
    }

    // --- What the painter makes of them, and the pointer --------------------------
    {
        Page const page = lay_out(page_with(R"HTML(
<div id="scaled" class="red" style="left: 40px; top: 40px; width: 20px; height: 20px; transform: scale(2)"></div>
<div id="turned" style="left: 100px; top: 40px; width: 40px; height: 20px; transform: rotate(90deg)"><div class="red" style="left: 0; top: 0; width: 20px; height: 20px"></div><div class="blue" style="left: 20px; top: 0; width: 20px; height: 20px"></div></div>
<div id="flipped" style="left: 200px; top: 40px; width: 40px; height: 20px; transform: scaleX(-1)"><div class="red" style="left: 0; top: 0; width: 20px; height: 20px"></div><div class="blue" style="left: 20px; top: 0; width: 20px; height: 20px"></div></div>
<div id="origin" class="red" style="left: 300px; top: 40px; width: 10px; height: 10px; transform: scale(2); transform-origin: 0 0"></div>
<div id="diamond" class="red" style="left: 40px; top: 120px; width: 40px; height: 40px; transform: rotate(45deg)"></div>
<div id="prop" style="left: 100px; top: 120px; width: 40px; height: 20px; rotate: 90deg"><div class="red" style="left: 0; top: 0; width: 20px; height: 20px"></div><div class="blue" style="left: 20px; top: 0; width: 20px; height: 20px"></div></div>
<div id="outer" style="left: 200px; top: 120px; width: 40px; height: 40px; transform: scale(0.5)"><div id="nested" class="red" style="left: 0; top: 0; width: 40px; height: 40px; transform: rotate(90deg) scale(0.5)"></div></div>
<div id="back" class="red" style="left: 300px; top: 120px; width: 40px; height: 40px; transform: rotateY(180deg); backface-visibility: hidden"></div>
<div id="front" class="blue" style="left: 300px; top: 180px; width: 40px; height: 40px; transform: rotateY(180deg)"></div>
<div id="small" style="left: 40px; top: 200px; width: 80px; height: 80px; transform: scale(0.25); transform-origin: 0 0"><div class="red" style="left: 0; top: 0; width: 40px; height: 80px"></div><div class="blue" style="left: 40px; top: 0; width: 40px; height: 80px"></div></div>
<div id="faded" class="red" style="left: 200px; top: 200px; width: 20px; height: 20px; transform: scale(2); opacity: 0.5"></div>
<div id="gone" class="red" style="left: 260px; top: 200px; width: 20px; height: 20px; transform: scale(0)"></div>
)HTML"));
        Bitmap const canvas = paint_canvas(page, 400, 300);
        // Twice the size about its middle: from 30 to 70.
        CHECK(same(canvas.pixel(50, 50), red));
        CHECK(same(canvas.pixel(32, 32), red));
        CHECK(same(canvas.pixel(67, 67), red));
        CHECK(same(canvas.pixel(27, 50), white));
        CHECK(same(canvas.pixel(72, 50), white));
        CHECK(same(canvas.pixel(50, 27), white));
        // A quarter turn clockwise about (120, 50): the left half on top,
        // the box now 20 wide and 40 tall.
        CHECK(same(canvas.pixel(120, 35), red));
        CHECK(same(canvas.pixel(120, 65), blue));
        CHECK(same(canvas.pixel(105, 50), white));
        CHECK(same(canvas.pixel(135, 50), white));
        CHECK(same(canvas.pixel(120, 27), white));
        CHECK(same(canvas.pixel(112, 32), red));
        CHECK(same(canvas.pixel(128, 68), blue));
        // Mirrored: the halves change sides.
        CHECK(same(canvas.pixel(210, 50), blue));
        CHECK(same(canvas.pixel(230, 50), red));
        // From its corner: 300 to 320.
        CHECK(same(canvas.pixel(315, 55), red));
        CHECK(same(canvas.pixel(302, 42), red));
        CHECK(same(canvas.pixel(297, 45), white));
        CHECK(same(canvas.pixel(323, 45), white));
        // An eighth turn: a diamond about (60, 140), its points 28 px out.
        CHECK(same(canvas.pixel(60, 140), red));
        CHECK(same(canvas.pixel(60, 116), red));
        CHECK(same(canvas.pixel(84, 140), red));
        CHECK(same(canvas.pixel(42, 122), white)); // a corner of the box as laid out
        CHECK(same(canvas.pixel(78, 158), white));
        // The rotate property turns as the function does.
        CHECK(same(canvas.pixel(120, 115), red));
        CHECK(same(canvas.pixel(120, 145), blue));
        CHECK(same(canvas.pixel(105, 130), white));
        // One inside another: half of a half, about the same middle.
        CHECK(same(canvas.pixel(220, 140), red));
        CHECK(same(canvas.pixel(216, 136), red));
        CHECK(same(canvas.pixel(224, 144), red));
        CHECK(same(canvas.pixel(212, 140), white));
        CHECK(same(canvas.pixel(220, 132), white));
        // A hidden back is not drawn; a shown one is.
        CHECK(same(canvas.pixel(320, 140), white));
        CHECK(same(canvas.pixel(320, 200), blue));
        // Drawn at a quarter of its size: 20 px square, each half its own color.
        CHECK(same(canvas.pixel(44, 210), red));
        CHECK(same(canvas.pixel(56, 210), blue));
        CHECK(same(canvas.pixel(64, 210), white));
        CHECK(same(canvas.pixel(50, 224), white));
        // The box's opacity, after its transform.
        CHECK(same(canvas.pixel(210, 210), Color::rgb(255, 128, 128)));
        CHECK(same(canvas.pixel(192, 192), Color::rgb(255, 128, 128)));
        CHECK(same(canvas.pixel(186, 210), white));
        // Scaled to nothing.
        CHECK(same(canvas.pixel(270, 210), white));

        // The pointer finds a box where it is drawn.
        CHECK_EQ(id_at(page, 50, 50), std::string("scaled"));
        CHECK_EQ(id_at(page, 32, 32), std::string("scaled"));
        CHECK(id_at(page, 27, 50) != "scaled");
        CHECK_EQ(id_at(page, 120, 35), std::string("div")); // the red half, which has no id
        CHECK(id_at(page, 105, 50) != "turned" && id_at(page, 105, 50) != "div");
        CHECK_EQ(id_at(page, 60, 116), std::string("diamond"));
        CHECK(id_at(page, 42, 122) != "diamond");
        CHECK_EQ(id_at(page, 220, 140), std::string("nested"));
        CHECK_EQ(id_at(page, 212, 140), std::string("outer"));
        CHECK(id_at(page, 320, 140) != "back");
        CHECK_EQ(id_at(page, 320, 200), std::string("front"));
        CHECK(id_at(page, 270, 210) != "gone");
    }

    return sashfold::test::report("transforms");
}
