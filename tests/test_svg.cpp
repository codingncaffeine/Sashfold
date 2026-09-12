#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "svg/Raster.h"
#include "svg/Svg.h"
#include "text/FontManager.h"
#include "ui/PageImages.h"

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// The SVG subset: the microsyntaxes, the rasterizer's rules, and whole
// documents decoded to pixels — each shape checked at a pixel it must
// cover and one it must not.

using namespace sashfold;

namespace {

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

bool near(float a, float b, float tolerance = 0.01f)
{
    return std::abs(a - b) <= tolerance;
}

bool same(Color a, Color b, int tolerance = 2)
{
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance
        && std::abs(a.a - b.a) <= tolerance;
}

Color at(Bitmap const& bitmap, int x, int y)
{
    return bitmap.pixel(x, y);
}

constexpr Color transparent = Color::rgba(0, 0, 0, 0);
constexpr Color red = Color::rgb(255, 0, 0);
constexpr Color blue = Color::rgb(0, 0, 255);
constexpr Color lime = Color::rgb(0, 255, 0);
constexpr Color black = Color::rgb(0, 0, 0);

} // namespace

int main()
{
    text::FontManager::instance().set_system_fonts(false);

    // --- Path data ------------------------------------------------------------
    {
        svg::Path const path = svg::parse_path_data("M10 10 h 20 v 20 H 10 Z");
        CHECK_EQ(path.segments.size(), std::size_t(5));
        CHECK(path.segments[0].verb == svg::Path::Verb::Move);
        CHECK(near(path.segments[1].p1.x, 30) && near(path.segments[1].p1.y, 10));
        CHECK(near(path.segments[2].p1.x, 30) && near(path.segments[2].p1.y, 30));
        CHECK(near(path.segments[3].p1.x, 10) && near(path.segments[3].p1.y, 30));
        CHECK(path.segments[4].verb == svg::Path::Verb::Close);

        // Numbers run together as SVG allows, and a repeated moveto is a lineto.
        svg::Path const dense = svg::parse_path_data("M1-2 3 4l.5.5");
        CHECK_EQ(dense.segments.size(), std::size_t(3));
        CHECK(near(dense.segments[0].p1.x, 1) && near(dense.segments[0].p1.y, -2));
        CHECK(dense.segments[1].verb == svg::Path::Verb::Line);
        CHECK(near(dense.segments[1].p1.x, 3) && near(dense.segments[1].p1.y, 4));
        CHECK(near(dense.segments[2].p1.x, 3.5f) && near(dense.segments[2].p1.y, 4.5f));

        // A curve, a smooth curve reflecting its control point, an arc as cubics.
        svg::Path const curves = svg::parse_path_data("M0 0 C 10 0 20 10 20 20 S 30 40 40 40 Q 50 50 60 40 T 80 40");
        CHECK_EQ(curves.segments.size(), std::size_t(5));
        CHECK(curves.segments[2].verb == svg::Path::Verb::Cubic);
        CHECK(near(curves.segments[2].p1.x, 20) && near(curves.segments[2].p1.y, 30)); // 2 * end - control
        svg::Path const arc = svg::parse_path_data("M0 0 A 10 10 0 0 1 20 0");
        CHECK(arc.segments.size() >= 3); // a half turn is two quarter-turn cubics
        CHECK(near(arc.current().x, 20) && near(arc.current().y, 0));
        // The arc flags may run into the next number.
        svg::Path const packed = svg::parse_path_data("M0 0a10 10 0 0120 0");
        CHECK(near(packed.current().x, 20) && near(packed.current().y, 0));

        // An error keeps what came before it.
        svg::Path const broken = svg::parse_path_data("M0 0 L 10 10 L nonsense 5");
        CHECK_EQ(broken.segments.size(), std::size_t(2));
        CHECK(svg::parse_path_data("").empty());
        CHECK(svg::parse_path_data("10 10").empty());
    }

    // --- Transforms -----------------------------------------------------------
    {
        svg::Matrix const t = svg::parse_transform("translate(10, 20)");
        svg::Point const p = t.apply(svg::Point { 1, 1 });
        CHECK(near(p.x, 11) && near(p.y, 21));
        // A list applies right to left to a point: translate(scale(p)).
        svg::Matrix const ts = svg::parse_transform("translate(10) scale(2)");
        svg::Point const q = ts.apply(svg::Point { 1, 1 });
        CHECK(near(q.x, 12) && near(q.y, 2));
        svg::Matrix const r = svg::parse_transform("rotate(90)");
        svg::Point const rp = r.apply(svg::Point { 1, 0 });
        CHECK(near(rp.x, 0) && near(rp.y, 1));
        svg::Matrix const rc = svg::parse_transform("rotate(180 5 5)");
        svg::Point const rcp = rc.apply(svg::Point { 0, 0 });
        CHECK(near(rcp.x, 10) && near(rcp.y, 10));
        svg::Matrix const m = svg::parse_transform("matrix(1 0 0 1 3 4)");
        svg::Point const mp = m.apply(svg::Point { 0, 0 });
        CHECK(near(mp.x, 3) && near(mp.y, 4));
        // The inverse undoes it.
        std::optional<svg::Matrix> const inverse = ts.inverse();
        if (CHECK(inverse.has_value())) {
            svg::Point const back = inverse->apply(q);
            CHECK(near(back.x, 1) && near(back.y, 1));
        }
        CHECK(near(svg::Matrix::scale(2, 8).scale_factor(), 4));
    }

    // --- The rasterizer -------------------------------------------------------
    {
        // A 10x10 square from (2, 2): full inside, nothing outside, and the
        // edge pixels whole since the edges are on pixel boundaries.
        svg::Polygon const square { { 2, 2 }, { 12, 2 }, { 12, 12 }, { 2, 12 } };
        svg::Mask const mask = svg::rasterize({ square }, svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK_EQ(mask.at(5, 5), std::uint8_t(255));
        CHECK_EQ(mask.at(2, 2), std::uint8_t(255));
        CHECK_EQ(mask.at(11, 11), std::uint8_t(255));
        CHECK_EQ(mask.at(12, 12), std::uint8_t(0));
        CHECK_EQ(mask.at(1, 5), std::uint8_t(0));
        // A half-pixel edge covers half.
        svg::Polygon const half { { 2.5f, 2 }, { 12, 2 }, { 12, 12 }, { 2.5f, 12 } };
        svg::Mask const half_mask = svg::rasterize({ half }, svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK(half_mask.at(2, 5) > 100 && half_mask.at(2, 5) < 156);
        // The clip holds the mask to the target.
        svg::Mask const clipped = svg::rasterize({ square }, svg::FillRule::NonZero, 0, 0, 8, 8);
        CHECK_EQ(clipped.at(5, 5), std::uint8_t(255));
        CHECK_EQ(clipped.at(9, 9), std::uint8_t(0));
        CHECK(clipped.left + clipped.width <= 8);

        // Two squares wound the same way, one inside the other: nonzero
        // fills the hole, even-odd leaves it.
        svg::Polygon const outer { { 0, 0 }, { 20, 0 }, { 20, 20 }, { 0, 20 } };
        svg::Polygon const inner { { 5, 5 }, { 15, 5 }, { 15, 15 }, { 5, 15 } };
        svg::Mask const nonzero = svg::rasterize({ outer, inner }, svg::FillRule::NonZero, 0, 0, 20, 20);
        svg::Mask const evenodd = svg::rasterize({ outer, inner }, svg::FillRule::EvenOdd, 0, 0, 20, 20);
        CHECK_EQ(nonzero.at(10, 10), std::uint8_t(255));
        CHECK_EQ(evenodd.at(10, 10), std::uint8_t(0));
        CHECK_EQ(evenodd.at(2, 2), std::uint8_t(255));
        // Wound the other way, nonzero leaves the hole too.
        svg::Polygon const inner_reversed { { 5, 5 }, { 5, 15 }, { 15, 15 }, { 15, 5 } };
        svg::Mask const cancelled = svg::rasterize({ outer, inner_reversed }, svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK_EQ(cancelled.at(10, 10), std::uint8_t(0));

        // A stroke: a horizontal line 4 wide covers two rows either side of it.
        svg::Path line;
        line.move_to(svg::Point { 0, 10 });
        line.line_to(svg::Point { 20, 10 });
        svg::StrokeStyle style;
        style.width = 4;
        std::vector<svg::Polygon> const outline = svg::stroke(svg::flatten(line), style);
        svg::Mask const stroke_mask = svg::rasterize(outline, svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK_EQ(stroke_mask.at(10, 8), std::uint8_t(255));
        CHECK_EQ(stroke_mask.at(10, 11), std::uint8_t(255));
        CHECK_EQ(stroke_mask.at(10, 7), std::uint8_t(0));
        CHECK_EQ(stroke_mask.at(10, 12), std::uint8_t(0));
        CHECK_EQ(stroke_mask.at(0, 10), std::uint8_t(255)); // a butt cap ends at the point
        CHECK_EQ(stroke_mask.at(19, 10), std::uint8_t(255));
        // Square caps reach half the width past the ends.
        style.cap = svg::LineCap::Square;
        svg::Path const short_line = [] {
            svg::Path p;
            p.move_to(svg::Point { 5, 10 });
            p.line_to(svg::Point { 15, 10 });
            return p;
        }();
        svg::Mask const capped = svg::rasterize(svg::stroke(svg::flatten(short_line), style), svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK_EQ(capped.at(3, 10), std::uint8_t(255));
        CHECK_EQ(capped.at(2, 10), std::uint8_t(0));
        // A corner with a miter join fills its outer point; a bevel cuts it.
        svg::Path corner;
        corner.move_to(svg::Point { 2, 18 });
        corner.line_to(svg::Point { 2, 2 });
        corner.line_to(svg::Point { 18, 2 });
        svg::StrokeStyle miter;
        miter.width = 4;
        svg::Mask const mitered = svg::rasterize(svg::stroke(svg::flatten(corner), miter), svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK_EQ(mitered.at(0, 0), std::uint8_t(255));
        miter.join = svg::LineJoin::Bevel;
        svg::Mask const beveled = svg::rasterize(svg::stroke(svg::flatten(corner), miter), svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK_EQ(beveled.at(0, 0), std::uint8_t(0));
        CHECK_EQ(beveled.at(1, 4), std::uint8_t(255));
        // Dashes: 4 on, 4 off along a 20 px line.
        svg::StrokeStyle dashed;
        dashed.width = 2;
        dashed.dashes = { 4, 4 };
        svg::Mask const dashes = svg::rasterize(svg::stroke(svg::flatten(line), dashed), svg::FillRule::NonZero, 0, 0, 20, 20);
        CHECK_EQ(dashes.at(2, 10), std::uint8_t(255));
        CHECK_EQ(dashes.at(6, 10), std::uint8_t(0));
        CHECK_EQ(dashes.at(10, 10), std::uint8_t(255));
        CHECK_EQ(dashes.at(14, 10), std::uint8_t(0));
    }

    // --- The intrinsic size ---------------------------------------------------
    {
        auto const size_of = [](std::string_view markup) {
            std::unique_ptr<dom::Document> const document = html::parse_document(markup);
            dom::Element const* svg_root = nullptr;
            std::function<void(dom::Node const&)> find = [&](dom::Node const& node) {
                if (svg_root)
                    return;
                if (node.is_element() && static_cast<dom::Element const&>(node).is_svg("svg")) {
                    svg_root = &static_cast<dom::Element const&>(node);
                    return;
                }
                for (dom::Node const* child : node.children())
                    find(*child);
            };
            find(*document);
            return svg_root ? svg::intrinsic_size(*svg_root) : svg::IntrinsicSize {};
        };
        svg::IntrinsicSize const both = size_of("<svg width=\"24\" height=\"12\"></svg>");
        CHECK(both.width && near(*both.width, 24));
        CHECK(both.height && near(*both.height, 12));
        CHECK(both.ratio && near(*both.ratio, 2));
        svg::IntrinsicSize const boxed = size_of("<svg viewBox=\"0 0 100 50\"></svg>");
        CHECK(!boxed.width && !boxed.height);
        CHECK(boxed.ratio && near(*boxed.ratio, 2));
        svg::IntrinsicSize const percent = size_of("<svg width=\"100%\" height=\"10mm\"></svg>");
        CHECK(!percent.width);
        CHECK(percent.height && near(*percent.height, 37.795f, 0.01f));
        CHECK(!size_of("<svg></svg>").ratio);
    }

    // --- Decoding whole documents ---------------------------------------------
    {
        CHECK(svg::looks_like_svg(bytes_of("<svg xmlns=\"http://www.w3.org/2000/svg\"/>")));
        CHECK(svg::looks_like_svg(bytes_of("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- c -->\n<!DOCTYPE svg>\n<svg/>")));
        CHECK(!svg::looks_like_svg(bytes_of("<!DOCTYPE html><html><svg/></html>")));
        CHECK(!svg::looks_like_svg(bytes_of("\x89PNG")));
        CHECK(!svg::looks_like_svg(bytes_of("")));

        // A red square in a blue field: the declared size, the fill from
        // a presentation attribute, the background from a rect.
        std::optional<Bitmap> const simple = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\">"
            "<rect width=\"40\" height=\"20\" fill=\"blue\"/>"
            "<rect x=\"10\" y=\"5\" width=\"10\" height=\"10\" fill=\"red\"/></svg>"));
        if (CHECK(simple.has_value())) {
            CHECK_EQ(simple->width(), 40);
            CHECK_EQ(simple->height(), 20);
            CHECK(same(at(*simple, 2, 2), blue));
            CHECK(same(at(*simple, 15, 10), red));
            CHECK(same(at(*simple, 25, 10), blue));
        }

        // The viewBox alone sizes it, and scales what is inside.
        std::optional<Bitmap> const scaled = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\" width=\"20\">"
            "<rect x=\"5\" y=\"0\" width=\"5\" height=\"10\" fill=\"#00f\"/></svg>"));
        if (CHECK(scaled.has_value())) {
            CHECK_EQ(scaled->width(), 20);
            CHECK_EQ(scaled->height(), 20);
            CHECK(same(at(*scaled, 15, 10), blue));
            CHECK(same(at(*scaled, 5, 10), transparent));
        }

        // Nothing declared: 300 by 150, and a circle with a stroke.
        std::optional<Bitmap> const plain = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\">"
            "<circle cx=\"50\" cy=\"50\" r=\"20\" fill=\"lime\" stroke=\"black\" stroke-width=\"4\"/></svg>"));
        if (CHECK(plain.has_value())) {
            CHECK_EQ(plain->width(), 300);
            CHECK_EQ(plain->height(), 150);
            CHECK(same(at(*plain, 50, 50), lime));
            CHECK(same(at(*plain, 50, 30), black)); // on the stroke, r = 20, width 4: 28..32
            CHECK(same(at(*plain, 50, 26), transparent));
            CHECK(same(at(*plain, 100, 100), transparent));
        }

        // A stylesheet rule beats a presentation attribute; the style
        // attribute beats both; fill: none draws nothing; currentColor
        // follows color; evenodd leaves the hole.
        std::optional<Bitmap> const styled = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"60\">"
            "<style>.a { fill: red } .b { fill: lime } g { color: blue }</style>"
            "<rect class=\"a\" width=\"20\" height=\"20\" fill=\"blue\"/>"
            "<rect class=\"b\" x=\"20\" width=\"20\" height=\"20\" style=\"fill: blue\"/>"
            "<rect x=\"40\" width=\"20\" height=\"20\" fill=\"none\" stroke=\"none\"/>"
            "<g><rect x=\"60\" width=\"20\" height=\"20\" fill=\"currentColor\"/></g>"
            "<path d=\"M0 30 h40 v30 h-40 z M10 40 h20 v10 h-20 z\" fill=\"red\" fill-rule=\"evenodd\"/>"
            "<path d=\"M50 30 h40 v30 h-40 z M60 40 h20 v10 h-20 z\" fill=\"red\"/>"
            "</svg>"));
        if (CHECK(styled.has_value())) {
            CHECK(same(at(*styled, 10, 10), red));
            CHECK(same(at(*styled, 30, 10), blue));
            CHECK(same(at(*styled, 50, 10), transparent));
            CHECK(same(at(*styled, 70, 10), blue));
            CHECK(same(at(*styled, 20, 45), transparent)); // evenodd: the hole
            CHECK(same(at(*styled, 5, 45), red));
            CHECK(same(at(*styled, 70, 45), red)); // nonzero: filled
        }

        // <use> places a defined shape, a transform moves a group, a
        // nested viewport clips, and a gradient runs from one color to
        // the other across its box.
        std::optional<Bitmap> const composed = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
            "<defs><rect id=\"r\" width=\"10\" height=\"10\" fill=\"red\"/>"
            "<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#000\"/><stop offset=\"1\" stop-color=\"#fff\"/></linearGradient>"
            "</defs>"
            "<use href=\"#r\" x=\"20\" y=\"20\"/>"
            "<g transform=\"translate(50 0)\"><use href=\"#r\"/></g>"
            "<svg x=\"0\" y=\"50\" width=\"20\" height=\"20\"><rect width=\"100\" height=\"100\" fill=\"blue\"/></svg>"
            "<rect x=\"40\" y=\"60\" width=\"40\" height=\"10\" fill=\"url(#g)\"/>"
            "</svg>"));
        if (CHECK(composed.has_value())) {
            CHECK(same(at(*composed, 25, 25), red));
            CHECK(same(at(*composed, 5, 5), transparent));
            CHECK(same(at(*composed, 55, 5), red));
            CHECK(same(at(*composed, 10, 60), blue));
            CHECK(same(at(*composed, 30, 60), transparent)); // past the nested viewport's edge
            Color const left = at(*composed, 42, 65);
            Color const right = at(*composed, 77, 65);
            CHECK(left.r < 40 && right.r > 215);
            CHECK(left.a == 255 && right.a == 255);
        }

        // A clip path keeps only what is inside it; opacity fades.
        std::optional<Bitmap> const clipped = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"60\" height=\"30\">"
            "<clipPath id=\"c\"><rect width=\"15\" height=\"30\"/></clipPath>"
            "<rect width=\"30\" height=\"30\" fill=\"red\" clip-path=\"url(#c)\"/>"
            "<rect x=\"30\" width=\"30\" height=\"30\" fill=\"blue\" opacity=\"0.5\"/>"
            "</svg>"));
        if (CHECK(clipped.has_value())) {
            CHECK(same(at(*clipped, 5, 15), red));
            CHECK(same(at(*clipped, 20, 15), transparent));
            Color const faded = at(*clipped, 45, 15);
            CHECK(faded.b == 255 && faded.a > 120 && faded.a < 136);
        }

        // Display none and visibility hidden draw nothing; an unknown
        // reference with no fallback draws nothing, with one draws that.
        std::optional<Bitmap> const hidden = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" height=\"20\">"
            "<rect width=\"20\" height=\"20\" fill=\"red\" display=\"none\"/>"
            "<rect x=\"20\" width=\"20\" height=\"20\" fill=\"red\" visibility=\"hidden\"/>"
            "<rect x=\"40\" width=\"20\" height=\"20\" fill=\"url(#missing)\"/>"
            "<rect x=\"60\" width=\"20\" height=\"20\" fill=\"url(#missing) blue\"/>"
            "</svg>"));
        if (CHECK(hidden.has_value())) {
            CHECK(same(at(*hidden, 10, 10), transparent));
            CHECK(same(at(*hidden, 30, 10), transparent));
            CHECK(same(at(*hidden, 50, 10), transparent));
            CHECK(same(at(*hidden, 70, 10), blue));
        }

        // Text: drawn in the built-in face at its size, from its anchor.
        std::optional<Bitmap> const text = svg::decode_svg(bytes_of(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"40\">"
            "<text x=\"10\" y=\"30\" font-size=\"20\" fill=\"red\">II</text></svg>"));
        if (CHECK(text.has_value())) {
            int inked = 0;
            for (int y = 0; y < 40; ++y) {
                for (int x = 0; x < 100; ++x) {
                    if (at(*text, x, y).a > 0)
                        ++inked;
                }
            }
            CHECK(inked > 20);
            CHECK(same(at(*text, 90, 5), transparent));
        }

        // Through the page's image decoder: sniffed as SVG, and a small
        // picture drawn larger with the factor reported as its density, so
        // a 10-unit badge shown at 40 px has four pixels to the unit.
        {
            std::vector<std::uint8_t> const badge = bytes_of(
                "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"red\"/></svg>");
            std::optional<Bitmap> const plain_decode = ui::decode_image_bytes(badge);
            if (CHECK(plain_decode.has_value())) {
                CHECK_EQ(plain_decode->width(), 10);
                CHECK(same(at(*plain_decode, 5, 5), red));
            }
            float density = 0;
            std::optional<Bitmap> const crisp = ui::decode_image_bytes(badge, 0, &density);
            if (CHECK(crisp.has_value())) {
                CHECK_EQ(density, 4.0f);
                CHECK_EQ(crisp->width(), 40);
                CHECK(same(at(*crisp, 20, 20), red));
            }
            // A large one is drawn as declared.
            std::optional<Bitmap> const big = ui::decode_image_bytes(
                bytes_of("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"300\"/>"), 0, &density);
            if (CHECK(big.has_value())) {
                CHECK_EQ(density, 1.0f);
                CHECK_EQ(big->width(), 400);
            }
            CHECK(!ui::decode_image_bytes(bytes_of("<html><body>no</body></html>")).has_value());
        }

        // Limits: a size past the budget is refused, a hostile path is
        // survived, and a use cycle ends.
        CHECK(!svg::decode_svg(bytes_of("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100000\" height=\"100000\"/>")));
        std::string hostile = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><path d=\"";
        for (int i = 0; i < 2000; ++i)
            hostile += "M0 0 L 1e30 -1e30 A 1e20 1e20 1e9 1 1 -3 4 ";
        hostile += "\"/><g id=\"a\"><use href=\"#a\"/></g><use href=\"#a\"/></svg>";
        CHECK(svg::decode_svg(bytes_of(hostile)).has_value());
    }

    // --- In a page: an inline <svg> laid out as a replaced box ------------------
    {
        std::unique_ptr<dom::Document> const document = html::parse_document(R"HTML(<!doctype html>
<html><head><style>body { margin: 0 } .icon { color: blue; width: 24px; height: 24px }</style></head><body>
<svg class="icon" viewBox="0 0 12 12"><rect width="12" height="12" fill="currentColor"/></svg>
<svg width="30" height="10"><rect width="30" height="10" fill="lime"/></svg>
</body></html>)HTML");
        css::StyleMap const styles = css::resolve_styles(*document);
        dom::Element const* svg_root = nullptr;
        std::function<void(dom::Node const&)> find = [&](dom::Node const& node) {
            if (svg_root)
                return;
            if (node.is_element() && static_cast<dom::Element const&>(node).is_svg("svg")) {
                svg_root = &static_cast<dom::Element const&>(node);
                return;
            }
            for (dom::Node const* child : node.children())
                find(*child);
        };
        find(*document);
        if (CHECK(svg_root != nullptr)) {
            // The page's stylesheet reaches the svg: color is blue, and
            // currentColor inside it follows.
            Bitmap const picture = svg::render(*svg_root, styles, 24, 24);
            CHECK(same(at(picture, 12, 12), blue));
            CHECK(same(at(picture, 0, 0), blue));
        }
    }

    return test::report("svg");
}
