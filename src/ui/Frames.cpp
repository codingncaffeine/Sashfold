#include "ui/Frames.h"

#include "bindings/Realm.h"
#include "core/Ascii.h"
#include "core/Unicode.h"
#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "net/DataUrl.h"
#include "paint/Painter.h"
#include "text/FontManager.h"
#include "ui/PageImages.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace sashfold::ui {

namespace {

// Ten frames deep is as far as Gecko goes. A page's whole tree of frames —
// a page of frames whose frames are pages of frames — is drawn within a
// budget of frames looked for and of pixels painted, so it costs what one
// large page does; a frame bigger than a picture of 4096 by 4096 is not
// drawn at all.
constexpr int max_depth = 10;
constexpr int max_frames = 100;
constexpr long long max_frame_pixels = 4096LL * 4096LL;
constexpr long long max_pixels = 4 * max_frame_pixels;

// A document the frames being drawn are inside: its URL without the
// fragment (about:srcdoc for an srcdoc document), which a frame may name
// once but not twice, and a URL of its origin (an srcdoc document's is that
// of the document it is in).
struct Ancestor {
    std::string address;
    net::Url origin;
};

struct Walk {
    Walk(FrameFetcher const& fetcher, float scale)
        : fetch(fetcher)
        , device_scale(scale)
    {
    }

    FrameFetcher const& fetch;
    float device_scale;
    int frames_left = max_frames;
    long long pixels_left = max_pixels;
    std::vector<Ancestor> ancestors; // the page first
};

// What drawing one frame came to: its picture, null when there is nothing
// to show, and whether that is settled — a frame the budget left out is
// looked at again the next time.
struct Drawn {
    std::shared_ptr<Bitmap const> bitmap;
    bool settled = true;
};

void draw_in(net::Url const& base, layout::LayoutResult& page, net::ContentSecurityPolicy* policy, Walk& walk,
    int depth, DrawnFrames* drawn);

bool starts_with_ci(std::string_view text, std::string_view lowercase_prefix)
{
    return text.size() >= lowercase_prefix.size()
        && ascii_ci_equals(text.substr(0, lowercase_prefix.size()), lowercase_prefix);
}

std::string_view text_of(std::vector<std::uint8_t> const& bytes)
{
    return { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
}

void find_frames(layout::Fragment& fragment, std::vector<layout::Fragment*>& out)
{
    if (fragment.element && fragment.element->is_html("iframe") && fragment.image)
        out.push_back(&fragment);
    for (layout::Fragment& child : fragment.children)
        find_frames(child, out);
}

// Whether a frame may show `url`: named by one of the documents the frame
// is inside, yes, since pages that frame themselves exist; by two, no.
bool may_frame(net::Url const& url, std::vector<Ancestor> const& ancestors)
{
    std::string const address = url.serialize(true);
    return std::count_if(ancestors.begin(), ancestors.end(),
               [&](Ancestor const& ancestor) { return ancestor.address == address; })
        < 2;
}

// X-Frame-Options, as HTML processes it: the values of every such header,
// trimmed and lowercased, each counted once; several refuse when one of them
// is deny, sameorigin or allowall, and allow otherwise; deny refuses; and
// sameorigin refuses unless every document up the chain has the framed
// document's origin.
bool x_frame_options_allow(std::vector<net::Header> const& headers, net::Url const& framed,
    std::vector<Ancestor> const& ancestors)
{
    std::vector<std::string> values;
    for (net::Header const& header : headers) {
        if (!ascii_ci_equals(header.name, "x-frame-options"))
            continue;
        std::string_view rest = header.value;
        while (true) {
            std::size_t const comma = rest.find(',');
            std::string_view item = rest.substr(0, comma);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
                item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
                item.remove_suffix(1);
            std::string value;
            for (char const c : item)
                value += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
            if (std::find(values.begin(), values.end(), value) == values.end())
                values.push_back(std::move(value));
            if (comma == std::string_view::npos)
                break;
            rest.remove_prefix(comma + 1);
        }
    }
    if (values.empty())
        return true;
    if (values.size() > 1) {
        return std::none_of(values.begin(), values.end(), [](std::string const& value) {
            return value == "deny" || value == "sameorigin" || value == "allowall";
        });
    }
    if (values[0] == "deny")
        return false;
    if (values[0] == "sameorigin") {
        std::string const origin = framed.serialize_origin();
        for (Ancestor const& ancestor : ancestors) {
            if (origin == "null" || ancestor.origin.serialize_origin() != origin)
                return false;
        }
    }
    return true;
}

// A fetched document's own say on being shown in a frame: its enforced
// policies' frame-ancestors, and X-Frame-Options only when they have none.
bool may_be_framed(FrameResponse const& response, net::ContentSecurityPolicy& policy,
    std::vector<Ancestor> const& ancestors)
{
    std::vector<net::Url> origins;
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
        origins.push_back(it->origin);
    bool const refused = policy.frame_ancestors_refusal(origins).has_value();
    if (policy.governs_framing())
        return !refused;
    return x_frame_options_allow(response.headers, response.url, ancestors);
}

// A frame's document drawn at the frame's size, under its policy: `depth`
// is how many frames deep the document is.
std::shared_ptr<Bitmap const> render_frame(FrameResponse const& frame, bool srcdoc, net::ContentSecurityPolicy& policy,
    int width, int height, Walk& walk, int depth)
{
    // Markup is drawn; a picture or plain text sent to a frame is not yet.
    std::string_view const type = frame.content_type;
    if (!type.empty() && !starts_with_ci(type, "text/html") && !starts_with_ci(type, "application/xhtml"))
        return nullptr;
    auto const document = std::make_unique<dom::Document>();
    if (srcdoc)
        html::parse_document_into(*document, decode_utf8(text_of(frame.bytes))); // text, not bytes to sniff
    else
        html::parse_document_bytes_into(*document, text_of(frame.bytes));
    bindings::adopt_meta_policies(policy, *document);
    // The frame is its document's viewport.
    css::MediaContext const media { static_cast<float>(width), static_cast<float>(height), walk.device_scale };
    auto const fetch_kind = [&](net::ResourceKind kind) {
        return [&, kind](net::Url const& url, std::string_view nonce) -> std::optional<css::FetchedSheet> {
            std::optional<FrameResponse> response
                = walk.fetch ? walk.fetch(url, frame.url, kind, policy.guard(kind, std::string(nonce))) : std::nullopt;
            if (!response)
                return std::nullopt;
            return css::FetchedSheet { std::move(response->bytes), std::move(response->content_type) };
        };
    };
    css::InlineSheetCheck const inline_check = [&policy](dom::Element const& style, std::string_view text) {
        dom::Attr const* const nonce = style.find_attribute("nonce");
        return !policy.inline_refusal(net::InlineKind::Style, nonce ? nonce->value : std::string(), text);
    };
    std::vector<css::SheetSource> const sheets = css::collect_stylesheets(*document, &frame.url,
        fetch_kind(net::ResourceKind::Stylesheet), media, inline_check);
    std::vector<text::PageFont> const fonts = css::collect_page_fonts(sheets, fetch_kind(net::ResourceKind::Font), media);
    text::FontManager::instance().set_page_fonts(fonts);
    css::StyleSet style_set(sheets, media, &frame.url);
    style_set.set_style_attribute_check([&policy](dom::Element const&, std::string_view text) {
        return !policy.inline_refusal(net::InlineKind::StyleAttribute, {}, text);
    });
    css::StyleMap const styles = css::resolve_styles(*document, style_set);
    ImageFetcher const fetch_image = [&](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
        std::optional<FrameResponse> response = walk.fetch
            ? walk.fetch(url, frame.url, net::ResourceKind::Image, policy.guard(net::ResourceKind::Image))
            : std::nullopt;
        if (!response)
            return std::nullopt;
        return std::move(response->bytes);
    };
    layout::ImageMap const images = collect_images(*document, &frame.url, fetch_image, media);
    layout::BackgroundImages const backgrounds = collect_background_images(styles, fetch_image);
    layout::LayoutResult page = layout::layout_document(*document, styles, static_cast<float>(width), &images, nullptr,
        static_cast<float>(height), walk.device_scale);
    if (depth < max_depth)
        draw_in(frame.url, page, &policy, walk, depth, nullptr);
    // Its frames set fonts of their own; its own are put back to paint by.
    text::FontManager::instance().set_page_fonts(fonts);
    // A frame's canvas is transparent unless its document gives it a color,
    // so what is behind the frame shows through: the painter fills the
    // canvas with the canvas color, and a fill with no alpha leaves it be.
    if (!page.canvas_background_given)
        page.canvas_background = Color::rgba(0, 0, 0, 0);
    Bitmap canvas(width, height, page.canvas_background);
    paint::paint_page(canvas, page, 0, 0, &backgrounds);
    return std::make_shared<Bitmap const>(std::move(canvas));
}

// One frame of a document at `base`: where its document comes from, whether
// it may be fetched and shown, and its picture.
Drawn draw_one(dom::Element const& element, std::optional<net::Url> const& url, net::Url const& base,
    net::ContentSecurityPolicy* policy, int width, int height, Walk& walk, int depth)
{
    long long const pixels = static_cast<long long>(width) * height;
    if (pixels > max_frame_pixels)
        return {};
    if (walk.frames_left <= 0 || pixels > walk.pixels_left)
        return { nullptr, false };
    --walk.frames_left;
    FrameResponse response;
    bool srcdoc = false;
    // An srcdoc or data: document keeps the policy of the document it is
    // in; a fetched one has its response's own.
    bool inherits = true;
    if (dom::Attr const* const text = element.find_attribute("srcdoc")) {
        response = FrameResponse { std::vector<std::uint8_t>(text->value.begin(), text->value.end()), "text/html", base, {} };
        srcdoc = true;
    } else {
        if (!url || !may_frame(*url, walk.ancestors))
            return {};
        net::RequestGuard const guard = policy ? policy->guard(net::ResourceKind::Subdocument) : net::RequestGuard {};
        if (url->scheme == "data") {
            // Never fetched, so the page's frame-src is asked here.
            if (guard.refusal && guard.refusal(*url, false))
                return {};
            std::optional<net::DataUrlPayload> payload = net::parse_data_url(*url);
            if (!payload)
                return {};
            response = FrameResponse { std::move(payload->bytes), std::move(payload->mime_type), *url, {} };
        } else {
            std::optional<FrameResponse> fetched
                = walk.fetch ? walk.fetch(*url, base, net::ResourceKind::Subdocument, guard) : std::nullopt;
            if (!fetched)
                return {};
            response = std::move(*fetched);
            inherits = false;
        }
    }
    net::ContentSecurityPolicy own(response.url);
    if (inherits) {
        if (policy)
            own = *policy;
    } else {
        for (net::Header const& header : response.headers) {
            if (ascii_ci_equals(header.name, "content-security-policy"))
                own.add_header(header.value, false);
            else if (ascii_ci_equals(header.name, "content-security-policy-report-only"))
                own.add_header(header.value, true);
        }
        if (!may_be_framed(response, own, walk.ancestors))
            return {};
    }
    walk.pixels_left -= pixels;
    walk.ancestors.push_back(
        Ancestor { srcdoc ? std::string("about:srcdoc") : response.url.serialize(true), srcdoc ? base : response.url });
    std::shared_ptr<Bitmap const> bitmap = render_frame(response, srcdoc, own, width, height, walk, depth + 1);
    walk.ancestors.pop_back();
    return { std::move(bitmap), true };
}

void draw_in(net::Url const& base, layout::LayoutResult& page, net::ContentSecurityPolicy* policy, Walk& walk,
    int depth, DrawnFrames* drawn)
{
    std::vector<layout::Fragment*> frames;
    find_frames(page.root, frames);
    DrawnFrames kept;
    for (layout::Fragment* const frame : frames) {
        layout::Fragment::ImageBox& box = *frame->image;
        dom::Element const& element = *frame->element;
        bool const sized = box.width >= 0.5f && box.height >= 0.5f && box.width < 65536 && box.height < 65536;
        int const width = sized ? static_cast<int>(std::lround(box.width)) : 0;
        int const height = sized ? static_cast<int>(std::lround(box.height)) : 0;
        // What the frame shows, as HTML processes its attributes: srcdoc
        // first; then src, where none, an empty one, about:blank or one that
        // does not parse shows nothing.
        std::optional<net::Url> url;
        std::string source;
        if (dom::Attr const* const srcdoc = element.find_attribute("srcdoc")) {
            source = "srcdoc:" + srcdoc->value;
        } else if (dom::Attr const* const src = element.find_attribute("src"); src && !src->value.empty()) {
            url = net::parse_url(src->value, &base);
            if (url && url->scheme != "about")
                source = "src:" + url->serialize();
        }
        if (drawn) {
            auto const it = drawn->find(&element);
            if (it != drawn->end() && it->second.source == source && it->second.width == width
                && it->second.height == height) {
                box.bitmap = it->second.bitmap;
                kept.emplace(&element, it->second);
                continue;
            }
        }
        Drawn result;
        if (!source.empty() && width > 0 && height > 0)
            result = draw_one(element, url, base, policy, width, height, walk, depth);
        box.bitmap = result.bitmap;
        if (drawn && result.settled)
            kept.emplace(&element, DrawnFrame { source, width, height, result.bitmap });
    }
    if (drawn)
        *drawn = std::move(kept);
}

}

void draw_frames(net::Url const& base, layout::LayoutResult& page, FrameFetcher const& fetch, float device_scale,
    net::ContentSecurityPolicy* policy, DrawnFrames* drawn)
{
    Walk walk(fetch, device_scale);
    walk.ancestors.push_back(Ancestor { base.serialize(true), base });
    draw_in(base, page, policy, walk, 0, drawn);
}

}
