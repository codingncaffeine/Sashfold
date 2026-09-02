#include "Test.h"

#include "core/Bitmap.h"
#include "core/Png.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "net/Url.h"
#include "ui/PageImages.h"
#include "ui/SourceSet.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Image source selection: the srcset parser on the standard's edge cases,
// the sizes attribute against a viewport, the type gate, the selection an
// <img> and a <picture> arrive at, and the page images end to end — the
// chosen source is what gets fetched, and layout draws it at its density.

using namespace sashfold;

namespace {

net::Url page_url()
{
    return *net::parse_url("https://example.org/dir/page.html");
}

std::vector<ui::ImageCandidate> candidates(std::string_view srcset)
{
    net::Url const base = page_url();
    return ui::parse_srcset(srcset, &base);
}

std::string path_of(net::Url const& url)
{
    return url.serialize_path();
}

dom::Element const* find_img(dom::Node const& node, std::string_view id)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("img")) {
            if (dom::Attr const* attribute = element.find_attribute("id");
                attribute && attribute->value == id)
                return &element;
        }
    }
    for (dom::Node const* child : node.children()) {
        if (dom::Element const* found = find_img(*child, id))
            return found;
    }
    return nullptr;
}

} // namespace

int main()
{
    // --- srcset candidates ----------------------------------------------------
    {
        auto const one = candidates("a.png");
        CHECK(one.size() == 1 && path_of(one[0].url) == "/dir/a.png" && !one[0].width
            && !one[0].density);
        auto const two = candidates("a.png, b.png 2x");
        CHECK(two.size() == 2 && two[1].density && *two[1].density == 2.0f && !two[1].width);
        auto const widths = candidates("a.png 320w,b.png 640w");
        CHECK(widths.size() == 2 && widths[0].width && *widths[0].width == 320.0f
            && widths[1].width && *widths[1].width == 640.0f && !widths[1].density);
        CHECK(candidates("a.png 320w 2x").empty()); // w and x together
        CHECK(candidates("a.png 0w").empty());
        CHECK(candidates("a.png -1x").empty());
        CHECK(candidates("a.png .5x").empty());
        CHECK(candidates("a.png 1.x").empty());
        auto const fractional = candidates("a.png 1.5x");
        CHECK(fractional.size() == 1 && fractional[0].density && *fractional[0].density == 1.5f);
        auto const exponent = candidates("a.png 2e0x");
        CHECK(exponent.size() == 1 && exponent[0].density && *exponent[0].density == 2.0f);
        CHECK_EQ(candidates("a.png 1x,b.png 2x").size(), std::size_t { 2 });
        auto const data = candidates("data:image/png;base64,AAAA 1x, b.png 2x");
        CHECK(data.size() == 2 && data[0].url.scheme == "data");
        auto const commas = candidates("a.png,, b.png");
        CHECK(commas.size() == 2 && path_of(commas[0].url) == "/dir/a.png"
            && path_of(commas[1].url) == "/dir/b.png");
        auto const height = candidates("a.png 100h");
        CHECK(height.size() == 1 && !height[0].width && !height[0].density);
        CHECK(candidates("a.png 100h 2x").empty());
        CHECK(candidates("a.png (2 x)").empty()); // one parenthesized descriptor, invalid
        auto const mixed = candidates("bad.png 2y, good.png 2x");
        CHECK(mixed.size() == 1 && path_of(mixed[0].url) == "/dir/good.png");
        auto const absolute = candidates("https://cdn.example/x.png 400w");
        CHECK(absolute.size() == 1 && absolute[0].url.host == "cdn.example");
        CHECK(ui::parse_srcset("relative.png", nullptr).empty()); // no base to resolve against
        CHECK(candidates("  \n\t ").empty());
        auto const spaced = candidates("  a.png   2x  ,  b.png  ");
        CHECK(spaced.size() == 2 && spaced[0].density && *spaced[0].density == 2.0f
            && !spaced[1].density);
    }

    // --- sizes ----------------------------------------------------------------
    {
        css::MediaContext const wide { 800, 600 };
        css::MediaContext const narrow { 500, 900 };
        CHECK_EQ(ui::parse_sizes("", wide), 800.0f);
        CHECK_EQ(ui::parse_sizes("300px", wide), 300.0f);
        CHECK_EQ(ui::parse_sizes("50vw", wide), 400.0f);
        CHECK_EQ(ui::parse_sizes("10em", wide), 160.0f);
        CHECK_EQ(ui::parse_sizes("(max-width: 600px) 100vw, 300px", wide), 300.0f);
        CHECK_EQ(ui::parse_sizes("(max-width: 600px) 100vw, 300px", narrow), 500.0f);
        CHECK_EQ(ui::parse_sizes("(min-width: 30em) 40vw, 90vw", wide), 320.0f);
        CHECK_EQ(ui::parse_sizes("calc(100vw - 20px)", wide), 800.0f); // calc(): passed over
        CHECK_EQ(ui::parse_sizes("calc(100vw - 20px), 250px", wide), 250.0f);
        CHECK_EQ(ui::parse_sizes("-5px", wide), 800.0f);
        CHECK_EQ(ui::parse_sizes("auto", wide), 800.0f);
        CHECK_EQ(ui::parse_sizes("0", wide), 0.0f);
        CHECK_EQ(ui::parse_sizes("(max-width: 600px) 100vw", wide), 800.0f); // no entry applies
        CHECK_EQ(ui::parse_sizes("  (max-width: 600px)   100vw  ,  20vh  ", wide), 120.0f);
        CHECK_EQ(ui::parse_sizes("12pt", wide), 16.0f);
        CHECK_EQ(ui::parse_sizes("1in", wide), 96.0f);
    }

    // --- The type gate --------------------------------------------------------
    {
        CHECK(ui::supports_image_type("image/png"));
        CHECK(ui::supports_image_type("IMAGE/JPEG"));
        CHECK(ui::supports_image_type(" image/gif ; foo=bar"));
        CHECK(ui::supports_image_type(""));
        CHECK(!ui::supports_image_type("image/webp"));
        CHECK(!ui::supports_image_type("image/avif"));
        CHECK(!ui::supports_image_type("image/svg+xml"));
    }

    // --- Selection through a document ----------------------------------------
    {
        constexpr std::string_view html = R"HTML(<!doctype html><body>
<picture id="p1">
  <source type="image/webp" srcset="w.webp">
  <source media="(max-width: 500px)" srcset="small.png">
  <source srcset="s1.png 400w, s2.png 800w" sizes="200px">
  <img id="i1" src="fallback.png">
</picture>
<img id="i2" srcset="a.png 1x, b.png 2x" src="c.png">
<img id="i3" srcset="b.png 2x" src="c.png">
<img id="i4" srcset="a.png 400w" src="c.png" sizes="800px">
<img id="i5" srcset="" src="">
<img id="i6" srcset="x.png 300w, y.png 600w, z.png 1200w" sizes="(max-width: 600px) 100vw, 600px">
<img id="i7" src="plain.png">
<picture><source srcset=""><img id="i8" src="own.png"></picture>
<img id="i9" srcset="dup1.png 2x, dup2.png 2x, one.png 1x">
<picture><source media="(min-width: 700px)" srcset="wide.png 2x"><img id="i10" src="any.png"></picture>
</body>)HTML";
        auto const document = html::parse_document(html);
        net::Url const base = page_url();
        css::MediaContext const wide { 800, 600 };
        css::MediaContext const narrow { 400, 600 };
        auto const select = [&](std::string_view id, css::MediaContext const& media) {
            dom::Element const* img = find_img(*document, id);
            CHECK(img != nullptr);
            return img ? ui::select_image_source(*img, &base, media)
                       : std::optional<ui::ImageSource>();
        };
        auto const s1 = select("i1", wide);
        CHECK(s1 && path_of(s1->url) == "/dir/s1.png" && s1->density == 2.0f);
        auto const s1_narrow = select("i1", narrow);
        CHECK(s1_narrow && path_of(s1_narrow->url) == "/dir/small.png" && s1_narrow->density == 1.0f);
        auto const s2 = select("i2", wide);
        CHECK(s2 && path_of(s2->url) == "/dir/a.png" && s2->density == 1.0f);
        auto const s3 = select("i3", wide); // src joins as the 1x candidate
        CHECK(s3 && path_of(s3->url) == "/dir/c.png" && s3->density == 1.0f);
        auto const s4 = select("i4", wide); // a width descriptor keeps src out
        CHECK(s4 && path_of(s4->url) == "/dir/a.png" && s4->density == 0.5f);
        CHECK(!select("i5", wide).has_value());
        auto const s6 = select("i6", wide); // 600 px slot: densities .5, 1, 2
        CHECK(s6 && path_of(s6->url) == "/dir/y.png" && s6->density == 1.0f);
        auto const s6_narrow = select("i6", narrow); // 400 px slot: .75, 1.5, 3
        CHECK(s6_narrow && path_of(s6_narrow->url) == "/dir/y.png" && s6_narrow->density == 1.5f);
        auto const s7 = select("i7", wide);
        CHECK(s7 && path_of(s7->url) == "/dir/plain.png" && s7->density == 1.0f);
        auto const s8 = select("i8", wide); // a source with no candidates is passed over
        CHECK(s8 && path_of(s8->url) == "/dir/own.png");
        auto const s9 = select("i9", wide); // a repeated density is dropped
        CHECK(s9 && path_of(s9->url) == "/dir/one.png" && s9->density == 1.0f);
        auto const s10 = select("i10", wide); // no 1x on offer: the largest is taken
        CHECK(s10 && path_of(s10->url) == "/dir/wide.png" && s10->density == 2.0f);
        auto const s10_narrow = select("i10", narrow);
        CHECK(s10_narrow && path_of(s10_narrow->url) == "/dir/any.png");
    }

    // --- Page images end to end: fetched by choice, laid out by density ------
    {
        constexpr std::string_view html = R"HTML(<!doctype html>
<html><head><style>body { margin: 0 } p { margin: 0 }</style></head><body>
<p><img id="big" srcset="big.png 400w" sizes="200px"></p>
<p><img id="two" srcset="two.png 2x"></p>
<p><picture><source type="image/webp" srcset="never.webp"><img id="fallback" src="plain.png"></picture></p>
</body></html>)HTML";
        auto const document = html::parse_document(html);
        css::StyleMap const styles = css::resolve_styles(*document);
        net::Url const base = page_url();
        std::vector<std::string> requested;
        ui::ImageFetcher const fetcher = [&](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
            requested.push_back(path_of(url));
            if (path_of(url) == "/dir/big.png")
                return encode_png(Bitmap(400, 100, Color::rgb(0, 0, 255)));
            if (path_of(url) == "/dir/two.png")
                return encode_png(Bitmap(60, 40, Color::rgb(0, 255, 0)));
            if (path_of(url) == "/dir/plain.png")
                return encode_png(Bitmap(10, 10, Color::rgb(255, 0, 0)));
            return std::nullopt;
        };
        layout::ImageMap const images
            = ui::collect_images(*document, &base, fetcher, css::MediaContext { 800, 600 });
        CHECK((requested == std::vector<std::string> { "/dir/big.png", "/dir/two.png", "/dir/plain.png" }));
        CHECK_EQ(images.size(), std::size_t { 3 });
        dom::Element const* big = find_img(*document, "big");
        dom::Element const* two = find_img(*document, "two");
        if (big && images.contains(big)) {
            layout::PageImage const& image = images.at(big);
            CHECK(image.bitmap && image.bitmap->width() == 400 && image.density == 2.0f);
        } else {
            CHECK(false);
        }
        if (two && images.contains(two)) {
            layout::PageImage const& image = images.at(two);
            CHECK(image.bitmap && image.bitmap->width() == 60 && image.density == 2.0f);
        } else {
            CHECK(false);
        }

        layout::LayoutResult const page = layout::layout_document(*document, styles, 800, &images);
        std::vector<layout::Fragment const*> boxes;
        std::function<void(layout::Fragment const&)> const collect = [&](layout::Fragment const& f) {
            if (f.image)
                boxes.push_back(&f);
            for (layout::Fragment const& child : f.children)
                collect(child);
        };
        collect(page.root);
        auto const box_of = [&](std::string_view id) -> layout::Fragment const* {
            for (layout::Fragment const* box : boxes) {
                dom::Attr const* attribute = box->element ? box->element->find_attribute("id") : nullptr;
                if (attribute && attribute->value == id)
                    return box;
            }
            return nullptr;
        };
        CHECK_EQ(boxes.size(), std::size_t { 3 });
        if (layout::Fragment const* box = box_of("big"); CHECK(box != nullptr)) {
            CHECK_EQ(box->width, 200.0f); // 400 pixels at density 2
            CHECK_EQ(box->height, 50.0f);
        }
        if (layout::Fragment const* box = box_of("two"); CHECK(box != nullptr)) {
            CHECK_EQ(box->width, 30.0f);
            CHECK_EQ(box->height, 20.0f);
        }
        if (layout::Fragment const* box = box_of("fallback"); CHECK(box != nullptr)) {
            CHECK_EQ(box->width, 10.0f);
            CHECK_EQ(box->height, 10.0f);
        }
    }

    return sashfold::test::report("source_set");
}
