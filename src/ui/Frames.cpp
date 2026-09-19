#include "ui/Frames.h"

#include "bindings/LayoutOracle.h"
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
    layout::ControlStates const* controls = nullptr; // what the reader typed and toggled, for a live document's layout
    int frames_left = max_frames;
    long long pixels_left = max_pixels;
    std::vector<Ancestor> ancestors; // the page first
};

// What drawing one frame came to: its picture, null when there is nothing
// to show, whether that is settled — a frame the budget left out is looked
// at again the next time — and, for a live document, its view.
struct Drawn {
    std::shared_ptr<Bitmap const> bitmap;
    bool settled = true;
    std::shared_ptr<FrameView> view;
};

void draw_in(net::Url const& base, layout::LayoutResult& page, net::ContentSecurityPolicy* policy, Walk& walk,
    int depth, DrawnFrames* drawn, bindings::Realm* realm);

// A document rendered for a frame: its picture, and its view when it is a
// live document.
struct Rendered {
    std::shared_ptr<Bitmap const> bitmap;
    std::shared_ptr<FrameView> view;
};

Rendered render_document(dom::Document& document, net::Url const& document_url, net::ContentSecurityPolicy& policy,
    int width, int height, Walk& walk, int depth, bindings::Realm* realm, FrameView const* previous);

// What a frame shows, as the framing rules let it through: the response (an
// srcdoc's text, a data: URL's payload, or what was fetched), whether it is
// srcdoc text, and the policy the document keeps — the page's for srcdoc and
// data:, its response's own otherwise.
struct Opened {
    FrameResponse response;
    bool srcdoc = false;
    net::ContentSecurityPolicy policy;
};

std::optional<Opened> open_document(dom::Element const& element, bool from_srcdoc, std::optional<net::Url> const& url,
    net::Url const& base, net::ContentSecurityPolicy* policy, std::vector<Ancestor> const& ancestors, FrameFetcher const& fetch);

bool starts_with_ci(std::string_view text, std::string_view lowercase_prefix)
{
    return text.size() >= lowercase_prefix.size()
        && ascii_ci_equals(text.substr(0, lowercase_prefix.size()), lowercase_prefix);
}

// The layout's text runs in tree order, which a selection indexes.
void gather_runs(layout::Fragment const& fragment, std::vector<layout::TextRun const*>& out)
{
    for (layout::TextRun const& run : fragment.runs)
        out.push_back(&run);
    for (layout::Fragment const& child : fragment.children)
        gather_runs(child, out);
}

std::string_view text_of(std::vector<std::uint8_t> const& bytes)
{
    return { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
}

void find_frames(layout::Fragment& fragment, std::vector<layout::Fragment*>& out)
{
    // Every navigable container laid out as a box of its own: an iframe, an
    // object or an embed, or a frame in the cell its frameset gave it. A
    // frame outside a frameset has a window and no box, and shows nothing.
    if (fragment.element && fragment.image && bindings::is_navigable_container(*fragment.element))
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
    return render_document(*document, frame.url, policy, width, height, walk, depth, nullptr, nullptr).bitmap;
}

// A frame's document, parsed or live, drawn at the frame's size under its
// policy, `depth` frames deep; `realm`, when the document has one, is where
// its own frames' live documents are found, and the document's view is
// kept — with the scrolling and the frames of `previous`, the view this
// one replaces, so a script's change inside the frame does not put the
// reader back at the top.
Rendered render_document(dom::Document& document, net::Url const& document_url, net::ContentSecurityPolicy& policy,
    int width, int height, Walk& walk, int depth, bindings::Realm* realm, FrameView const* previous)
{
    // The frame is its document's viewport.
    css::MediaContext const media { static_cast<float>(width), static_cast<float>(height), walk.device_scale };
    auto const fetch_kind = [&](net::ResourceKind kind) {
        return [&, kind](net::Url const& url, std::string_view nonce) -> std::optional<css::FetchedSheet> {
            std::optional<FrameResponse> response
                = walk.fetch ? walk.fetch(url, document_url, kind, policy.guard(kind, std::string(nonce))) : std::nullopt;
            if (!response)
                return std::nullopt;
            return css::FetchedSheet { std::move(response->bytes), std::move(response->content_type) };
        };
    };
    css::InlineSheetCheck const inline_check = [&policy](dom::Element const& style, std::string_view text) {
        dom::Attr const* const nonce = style.find_attribute("nonce");
        return !policy.inline_refusal(net::InlineKind::Style, nonce ? nonce->value : std::string(), text);
    };
    std::vector<css::SheetSource> const sheets = css::collect_stylesheets(document, &document_url,
        fetch_kind(net::ResourceKind::Stylesheet), media, inline_check);
    std::vector<text::PageFont> const fonts = css::collect_page_fonts(sheets, fetch_kind(net::ResourceKind::Font), media);
    text::FontManager::instance().set_page_fonts(fonts);
    css::StyleSet style_set(sheets, media, &document_url);
    style_set.set_style_attribute_check([&policy](dom::Element const&, std::string_view text) {
        return !policy.inline_refusal(net::InlineKind::StyleAttribute, {}, text);
    });
    // Not const: a live document's view takes the map over, and the
    // fragments point into it.
    css::StyleMap styles = css::resolve_styles(document, style_set);
    ImageFetcher const fetch_image = [&](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
        std::optional<FrameResponse> response = walk.fetch
            ? walk.fetch(url, document_url, net::ResourceKind::Image, policy.guard(net::ResourceKind::Image))
            : std::nullopt;
        if (!response)
            return std::nullopt;
        return std::move(response->bytes);
    };
    // A live document's objects and embeds as its realm decided them.
    layout::EmbeddedStates const embedded = realm ? bindings::embedded_states(*realm) : layout::EmbeddedStates {};
    layout::EmbeddedStates const* const decided = realm ? &embedded : nullptr;
    // A frame's pictures come in one pass of the page's size; the rest of a
    // long frame are left to its alt text until frames load in passes too.
    layout::ImageMap images = collect_images(document, &document_url, fetch_image, media, decided, ImagePass { nullptr, 64, nullptr });
    layout::BackgroundImages backgrounds = collect_background_images(styles, fetch_image);
    layout::LayoutResult page = layout::layout_document(document, styles, static_cast<float>(width), &images,
        realm ? walk.controls : nullptr, static_cast<float>(height), walk.device_scale, decided);
    if (!realm) {
        if (depth < max_depth)
            draw_in(document_url, page, &policy, walk, depth, nullptr, nullptr);
        // Its frames set fonts of their own; its own are put back to paint by.
        text::FontManager::instance().set_page_fonts(fonts);
        // A frame's canvas is transparent unless its document gives it a color,
        // so what is behind the frame shows through: the painter fills the
        // canvas with the canvas color, and a fill with no alpha leaves it be.
        if (!page.canvas_background_given)
            page.canvas_background = Color::rgba(0, 0, 0, 0);
        Bitmap canvas(width, height, page.canvas_background);
        paint::paint_page(canvas, page, 0, 0, &backgrounds);
        return { std::make_shared<Bitmap const>(std::move(canvas)), nullptr };
    }
    // A live document keeps everything its picture is painted from, so the
    // shell can paint it again as the reader scrolls it, and find what is
    // under a point in it.
    auto view = std::make_shared<FrameView>();
    view->realm = realm;
    view->document = &document;
    view->url = document_url;
    view->width = width;
    view->height = height;
    view->device_scale = walk.device_scale;
    view->fonts = fonts;
    view->styles = std::move(styles);
    view->images = std::move(images);
    view->backgrounds = std::move(backgrounds);
    view->layout = std::move(page);
    if (previous) {
        view->scrolls = previous->scrolls;
        view->scroll_y = previous->scroll_y;
        view->frames = previous->frames;
    }
    if (depth < max_depth)
        draw_in(document_url, view->layout, &policy, walk, depth, &view->frames, realm);
    text::FontManager::instance().set_page_fonts(view->fonts);
    if (!view->layout.canvas_background_given)
        view->layout.canvas_background = Color::rgba(0, 0, 0, 0);
    // A fresh layout carries none of the scrolling: it is put back on,
    // held to what the new shape can reach.
    view->applied.clear();
    view->applied_page = {};
    view->settle_scrolls();
    gather_runs(view->layout.root, view->runs);
    std::shared_ptr<Bitmap const> picture = view->paint();
    return { std::move(picture), std::move(view) };
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
        return { nullptr, false, nullptr };
    --walk.frames_left;
    std::optional<Opened> opened
        = open_document(element, bindings::container_srcdoc(element) != nullptr, url, base, policy, walk.ancestors, walk.fetch);
    if (!opened)
        return {};
    walk.pixels_left -= pixels;
    walk.ancestors.push_back(Ancestor { opened->srcdoc ? std::string("about:srcdoc") : opened->response.url.serialize(true),
        opened->srcdoc ? base : opened->response.url });
    std::shared_ptr<Bitmap const> bitmap
        = render_frame(opened->response, opened->srcdoc, opened->policy, width, height, walk, depth + 1);
    walk.ancestors.pop_back();
    return { std::move(bitmap), true, nullptr };
}

// What a frame's document is fetched as, which names the directive of the
// page's policy that guards it (CSP3 §6.8.1, the effective directive):
// object-src for an object's or an embed's, whose request destinations are
// "object" and "embed"; frame-src for an iframe's or a frame's.
net::ResourceKind document_kind(dom::Element const& element)
{
    return element.is_html("object") || element.is_html("embed") ? net::ResourceKind::Object : net::ResourceKind::Subdocument;
}

std::optional<Opened> open_document(dom::Element const& element, bool from_srcdoc, std::optional<net::Url> const& url,
    net::Url const& base, net::ContentSecurityPolicy* policy, std::vector<Ancestor> const& ancestors, FrameFetcher const& fetch)
{
    FrameResponse response;
    bool srcdoc = false;
    // An srcdoc or data: document keeps the policy of the document it is
    // in; a fetched one has its response's own.
    bool inherits = true;
    if (dom::Attr const* const text = from_srcdoc ? element.find_attribute("srcdoc") : nullptr) {
        response = FrameResponse { std::vector<std::uint8_t>(text->value.begin(), text->value.end()), "text/html", base, {}, 200 };
        srcdoc = true;
    } else {
        if (!url || !may_frame(*url, ancestors))
            return {};
        net::ResourceKind const kind = document_kind(element);
        net::RequestGuard const guard = policy ? policy->guard(kind) : net::RequestGuard {};
        if (url->scheme == "data") {
            // Never fetched, so the page's frame-src or object-src is asked here.
            if (guard.refusal && guard.refusal(*url, false))
                return {};
            std::optional<net::DataUrlPayload> payload = net::parse_data_url(*url);
            if (!payload)
                return {};
            response = FrameResponse { std::move(payload->bytes), std::move(payload->mime_type), *url, {}, 200 };
        } else {
            std::optional<FrameResponse> fetched = fetch ? fetch(*url, base, kind, guard) : std::nullopt;
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
        if (!may_be_framed(response, own, ancestors))
            return {};
    }
    return Opened { std::move(response), srcdoc, std::move(own) };
}

// A frame drawn from the live document its realm holds, under that
// document's policy and within the same budget; `previous` is the view its
// last picture was painted from, whose scrolling this one keeps.
Drawn draw_live(bindings::Realm& frame, int width, int height, Walk& walk, int depth, FrameView const* previous)
{
    long long const pixels = static_cast<long long>(width) * height;
    if (pixels > max_frame_pixels)
        return {};
    if (walk.frames_left <= 0 || pixels > walk.pixels_left)
        return { nullptr, false, nullptr };
    net::ContentSecurityPolicy* const frame_policy = frame.hooks().policy;
    if (!frame_policy)
        return {};
    --walk.frames_left;
    walk.pixels_left -= pixels;
    walk.ancestors.push_back(Ancestor { frame.url().serialize(true), frame.origin_url() });
    Rendered rendered = render_document(frame.document(), frame.url(), *frame_policy, width, height, walk, depth + 1, &frame, previous);
    walk.ancestors.pop_back();
    return { std::move(rendered.bitmap), true, std::move(rendered.view) };
}

void draw_in(net::Url const& base, layout::LayoutResult& page, net::ContentSecurityPolicy* policy, Walk& walk,
    int depth, DrawnFrames* drawn, bindings::Realm* realm)
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
        // What the frame shows, as HTML processes its attributes: an iframe's
        // srcdoc first; then src, where none, an empty one, about:blank or one
        // that does not parse shows nothing.
        std::optional<net::Url> url;
        if (!bindings::container_srcdoc(element)) {
            if (dom::Attr const* const src = element.find_attribute("src"); src && !src->value.empty())
                url = net::parse_url(src->value, &base);
        }
        std::string const source = bindings::frame_source(element, base);
        // A frame whose document has a realm here is drawn from that live
        // document; its picture stands only while those documents are unchanged.
        bindings::Realm* const live = realm ? realm->frame_realm(element) : nullptr;
        // An object's or an embed's window is drawn from its live document
        // alone: laid out without a realm, nothing has decided that it shows a
        // document, and one that shows a picture already has it in its box.
        if (bindings::ContainerKind const kind = bindings::container_kind(element);
            !live && (kind == bindings::ContainerKind::Object || kind == bindings::ContainerKind::Embed))
            continue;
        std::uint64_t const mutations = live ? live->tree_mutation_count() : 0;
        FrameView const* previous = nullptr;
        if (drawn) {
            auto const it = drawn->find(&element);
            if (it != drawn->end() && it->second.source == source && it->second.width == width
                && it->second.height == height && it->second.mutations == mutations && !it->second.stale) {
                box.bitmap = it->second.bitmap;
                kept.emplace(&element, it->second);
                continue;
            }
            // The view the frame was drawn from last, whatever changed since,
            // when it is still the same document: its scrolling carries over.
            if (it != drawn->end() && it->second.view && live && it->second.view->document == &live->document())
                previous = it->second.view.get();
        }
        Drawn result;
        if (live && width > 0 && height > 0)
            result = draw_live(*live, width, height, walk, depth, previous);
        else if (!source.empty() && width > 0 && height > 0)
            result = draw_one(element, url, base, policy, width, height, walk, depth);
        box.bitmap = result.bitmap;
        if (drawn && result.settled)
            kept.emplace(&element, DrawnFrame { source, width, height, result.bitmap, mutations, std::move(result.view), false });
    }
    if (drawn)
        *drawn = std::move(kept);
}

}

int FrameView::max_scroll() const
{
    int const page_height = static_cast<int>(layout.page_height + 0.5f);
    return std::max(0, page_height - height);
}

void FrameView::settle_scrolls()
{
    scroll_y = std::clamp(scroll_y, 0, max_scroll());
    layout::apply_scroll(layout.root, scrolls, applied);
    layout::apply_page_scroll(layout, static_cast<float>(std::max(1, width)), static_cast<float>(std::max(1, height)),
        layout::ScrollOffset { 0, static_cast<float>(scroll_y) }, applied_page, &applied);
}

std::shared_ptr<Bitmap const> FrameView::paint() const
{
    text::FontManager::instance().set_page_fonts(fonts);
    Bitmap canvas(width, height, layout.canvas_background);
    paint::paint_page(canvas, layout, 0, -static_cast<float>(scroll_y), &backgrounds, &scrolls);
    // More below than the cell shows: a thumb down the right edge, as the
    // shell draws the page's.
    int const page_height = static_cast<int>(layout.page_height + 0.5f);
    if (page_height > height && height > 0 && layout.viewport_overflow_y != css::Overflow::Hidden
        && layout.viewport_overflow_y != css::Overflow::Clip) {
        int const thumb = std::max(20, static_cast<int>(static_cast<long long>(height) * height / page_height));
        int const travel = std::max(0, height - thumb);
        int const range = max_scroll();
        int const thumb_y = range > 0 ? static_cast<int>(static_cast<long long>(travel) * scroll_y / range) : 0;
        canvas.fill_round_rect(Rect { width - 8, thumb_y + 2, 5, std::max(1, thumb - 4) }, 2, Color::rgba(96, 96, 96, 140));
    }
    return std::make_shared<Bitmap const>(std::move(canvas));
}

void draw_frames(net::Url const& base, layout::LayoutResult& page, FrameFetcher const& fetch, float device_scale,
    net::ContentSecurityPolicy* policy, DrawnFrames* drawn, bindings::Realm* realm, layout::ControlStates const* controls)
{
    Walk walk(fetch, device_scale);
    walk.controls = controls;
    walk.ancestors.push_back(Ancestor { base.serialize(true), base });
    draw_in(base, page, policy, walk, 0, drawn, realm);
}

std::optional<bindings::FrameDocument> frame_document_for(dom::Element const& iframe, net::Url const& base,
    net::ContentSecurityPolicy* policy, std::vector<bindings::FrameAncestor> const& ancestors,
    std::optional<net::Url> const& target, FrameFetcher const& fetch)
{
    // What the frame shows, as HTML processes its attributes and draw_in reads
    // them: an iframe's srcdoc first, then a src that parses and is not
    // about:; or the URL the frame navigates to on its own, whatever its
    // attributes say.
    std::optional<net::Url> url = target;
    bool const from_srcdoc = !target && bindings::container_srcdoc(iframe) != nullptr;
    if (!target && !from_srcdoc) {
        dom::Attr const* const src = iframe.find_attribute("src");
        if (!src || src->value.empty())
            return std::nullopt;
        url = net::parse_url(src->value, &base);
    }
    if (!from_srcdoc && (!url || url->scheme == "about"))
        return std::nullopt;
    std::vector<Ancestor> chain;
    for (bindings::FrameAncestor const& ancestor : ancestors)
        chain.push_back(Ancestor { ancestor.address, ancestor.origin });
    std::optional<Opened> opened = open_document(iframe, from_srcdoc, url, base, policy, chain, fetch);
    if (!opened)
        return std::nullopt;
    bindings::FrameDocument answer;
    answer.bytes = std::move(opened->response.bytes);
    answer.content_type = std::move(opened->response.content_type);
    answer.url = opened->srcdoc ? *net::parse_url("about:srcdoc") : opened->response.url;
    // An srcdoc document has its parent's origin; a data: URL's is opaque.
    answer.origin = opened->srcdoc ? (chain.empty() ? base : chain.back().origin) : opened->response.url;
    answer.srcdoc = opened->srcdoc;
    answer.policy = std::move(opened->policy);
    answer.status = opened->response.status;
    return answer;
}

}
