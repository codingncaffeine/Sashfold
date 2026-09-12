#include "ui/PageImages.h"

#include "core/Bmp.h"
#include "core/Gif.h"
#include "core/Jpeg.h"
#include "core/Png.h"
#include "dom/Dom.h"
#include "svg/Svg.h"
#include "ui/SourceSet.h"

#include <map>
#include <string>

namespace sashfold::ui {

namespace {

constexpr std::size_t max_images_per_page = 64;
constexpr std::size_t max_image_bytes = 8u * 1024u * 1024u;

struct Collector {
    net::Url const* base;
    css::MediaContext const& media;
    ImageFetcher const& fetch;
    layout::ImageMap& out;
    std::map<std::string, std::shared_ptr<Bitmap const>> by_url; // one fetch per URL
    std::map<std::string, float> by_url_density; // the factor an SVG was drawn at
    std::size_t fetched = 0;

    void visit(dom::Node const& node)
    {
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            if (element.is_html("img"))
                consider(element);
        }
        for (dom::Node const* child : node.children())
            visit(*child);
    }

    void consider(dom::Element const& element)
    {
        // The source the standard's selection names for this viewport: a
        // <picture> parent's applicable <source>, else srcset and src.
        std::optional<ImageSource> const source = select_image_source(element, base, media);
        if (!source)
            return;
        net::Url const& url = source->url;
        std::string const key = url.serialize(true);
        // A picture's density to the layout is its pixels per device px:
        // the source's pixels per CSS px, over the device's scale.
        float const scale = media.device_scale > 0 ? media.device_scale : 1.0f;
        if (auto const it = by_url.find(key); it != by_url.end()) {
            if (it->second)
                out.emplace(&element, layout::PageImage { it->second, source->density * by_url_density[key] / scale });
            return;
        }
        std::shared_ptr<Bitmap const> image;
        float drawn_at = 1; // an SVG drawn larger than its size reports the factor
        if (fetched < max_images_per_page && fetch) {
            ++fetched;
            if (std::optional<std::vector<std::uint8_t>> bytes = fetch(url);
                bytes && bytes->size() <= max_image_bytes) {
                if (std::optional<Bitmap> decoded = decode_image_bytes(*bytes, 0, &drawn_at))
                    image = std::make_shared<Bitmap const>(std::move(*decoded));
            }
        }
        by_url.emplace(key, image);
        by_url_density[key] = drawn_at;
        if (image)
            out.emplace(&element, layout::PageImage { std::move(image), source->density * drawn_at / scale });
    }
};

} // namespace

std::optional<Bitmap> decode_image_bytes(std::vector<std::uint8_t> const& bytes, int icon_size, float* density)
{
    if (density)
        *density = 1;
    // The bytes say what they are; the transport's claim does not.
    if (looks_like_png(bytes))
        return decode_png(bytes);
    if (looks_like_gif(bytes))
        return decode_gif(bytes);
    if (looks_like_jpeg(bytes))
        return decode_jpeg(bytes);
    if (looks_like_bmp(bytes))
        return decode_bmp(bytes);
    if (looks_like_ico(bytes))
        return decode_ico(bytes, icon_size);
    // A vector picture: drawn at its own size — or, for a page's picture,
    // larger, with the factor reported as its density — and scaled like
    // any other from there.
    if (svg::looks_like_svg(bytes))
        return svg::decode_svg(bytes, 16u << 20, density);
    return std::nullopt;
}

layout::ImageMap collect_images(dom::Document const& document, net::Url const* base,
    ImageFetcher const& fetch, css::MediaContext const& media)
{
    layout::ImageMap images;
    Collector collector { base, media, fetch, images, {}, {}, 0 };
    collector.visit(document);
    return images;
}

layout::BackgroundImages collect_background_images(css::StyleMap const& styles, ImageFetcher const& fetch)
{
    layout::BackgroundImages out;
    std::size_t fetched = 0;
    auto const consider = [&](css::ComputedStyle const& style) {
        if (!style.background_images)
            return;
        for (css::BackgroundImage const& image : *style.background_images) {
            if (image.url.empty() || out.contains(image.url))
                continue;
            std::shared_ptr<Bitmap const> bitmap;
            std::optional<net::Url> const url = net::parse_url(image.url);
            if (url && fetched < max_images_per_page && fetch) {
                ++fetched;
                if (std::optional<std::vector<std::uint8_t>> bytes = fetch(*url);
                    bytes && bytes->size() <= max_image_bytes) {
                    if (std::optional<Bitmap> decoded = decode_image_bytes(*bytes))
                        bitmap = std::make_shared<Bitmap const>(std::move(*decoded));
                }
            }
            out.emplace(image.url, std::move(bitmap));
        }
    };
    for (auto const& [element, style] : styles) {
        consider(style);
        if (style.generated) {
            if (style.generated->before)
                consider(style.generated->before->style);
            if (style.generated->after)
                consider(style.generated->after->style);
        }
    }
    return out;
}

}
