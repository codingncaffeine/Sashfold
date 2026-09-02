#include "ui/PageImages.h"

#include "core/Gif.h"
#include "core/Jpeg.h"
#include "core/Png.h"
#include "dom/Dom.h"

#include <map>
#include <string>

namespace sashfold::ui {

namespace {

constexpr std::size_t max_images_per_page = 64;
constexpr std::size_t max_image_bytes = 8u * 1024u * 1024u;

struct Collector {
    net::Url const* base;
    ImageFetcher const& fetch;
    layout::ImageMap& out;
    std::map<std::string, std::shared_ptr<Bitmap const>> by_url; // one fetch per URL
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
        dom::Attr const* src = element.find_attribute("src");
        if (!src || src->value.empty())
            return;
        std::optional<net::Url> const url = net::parse_url(src->value, base);
        if (!url)
            return;
        std::string const key = url->serialize(true);
        if (auto const it = by_url.find(key); it != by_url.end()) {
            if (it->second)
                out.emplace(&element, it->second);
            return;
        }
        std::shared_ptr<Bitmap const> image;
        if (fetched < max_images_per_page && fetch) {
            ++fetched;
            if (std::optional<std::vector<std::uint8_t>> bytes = fetch(*url);
                bytes && bytes->size() <= max_image_bytes) {
                // The bytes say what they are; the transport's claim does not.
                std::optional<Bitmap> decoded;
                if (looks_like_png(*bytes))
                    decoded = decode_png(*bytes);
                else if (looks_like_gif(*bytes))
                    decoded = decode_gif(*bytes);
                else if (looks_like_jpeg(*bytes))
                    decoded = decode_jpeg(*bytes);
                if (decoded)
                    image = std::make_shared<Bitmap const>(std::move(*decoded));
            }
        }
        by_url.emplace(key, image);
        if (image)
            out.emplace(&element, std::move(image));
    }
};

} // namespace

layout::ImageMap collect_images(dom::Document const& document, net::Url const* base,
    ImageFetcher const& fetch)
{
    layout::ImageMap images;
    Collector collector { base, fetch, images, {}, 0 };
    collector.visit(document);
    return images;
}

}
