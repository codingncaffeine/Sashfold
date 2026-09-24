#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"

#include "bindings/Fetching.h"

// The canvas element's 2D context (HTML section4.12.5): HTMLCanvasElement's
// getContext, toDataURL and toBlob; CanvasRenderingContext2D over the
// drawing model of paint/Canvas2D, which rasterizes with the SVG renderer's
// path code; CanvasGradient, CanvasPattern, Path2D, TextMetrics, ImageData,
// ImageBitmap and createImageBitmap. A canvas keeps its state with its
// element's wrapper; the ones drawn on are listed for the host, which lays
// their pictures out and paints them as it does a video's frames, written
// again in place as the canvas changes.

#include "core/Base64.h"
#include "core/Bitmap.h"
#include "core/Bmp.h"
#include "core/Gif.h"
#include "core/Jpeg.h"
#include "core/Png.h"
#include "core/Unicode.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "js/Object.h"
#include "js/Runtime.h"
#include "js/Strings.h"
#include "net/DataUrl.h"
#include "paint/Canvas2D.h"
#include "svg/Svg.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

using canvas::Surface;

// The largest bitmap a canvas is given, in pixels: past it the canvas
// keeps a bitmap of no size, and draws nothing (a browser refuses such a
// canvas too).
constexpr std::size_t max_canvas_pixels = 64u * 1024u * 1024u;

class ContextObject;

// A canvas element's own state, kept by its wrapper: its bitmap, its
// context, whether anything of another origin has been drawn into it, and
// the picture handed to the host.
class CanvasStateObject final : public js::Object {
public:
    explicit CanvasStateObject(NodeWrapper& the_wrapper)
        : Object(nullptr, Class::Host)
        , wrapper(&the_wrapper)
    {
    }
    NodeWrapper* wrapper;
    std::shared_ptr<Surface> surface;
    ContextObject* context = nullptr;
    bool origin_clean = true;
    // Made by a context whose alpha setting is false: every pixel stays
    // opaque (HTML section4.12.5.1.1), transparent black reading as opaque black.
    bool opaque = false;
    bool listed = false;
    std::uint64_t painted = 0; // moves with every change to the bitmap
    std::uint64_t presented_at = ~std::uint64_t { 0 };
    std::shared_ptr<Bitmap> presented;
    std::uint64_t shape = 0;
    std::uint64_t frames = 0;

    void trace(js::Tracer& tracer) override;
};

// The drawing state (HTML section4.12.5.1.2) that save() pushes.
struct ContextState {
    canvas::DrawState draw;
    canvas::Style fill;
    canvas::Style stroke;
    js::Value fill_object; // the CanvasGradient or CanvasPattern fillStyle returns, else undefined
    js::Value stroke_object;
    svg::StrokeStyle pen { 1, svg::LineCap::Butt, svg::LineJoin::Miter, 10, {}, 0 };
    // The line settings as script set them, which the attributes and
    // getLineDash answer with: the pen holds them in floats for the
    // rasterizer, and 0.3 in a float reads back as 0.30000001192092896.
    double line_width = 1;
    double miter_limit = 10;
    double dash_offset = 0;
    std::vector<double> dashes;
    canvas::Font font;
    std::string font_text = "10px sans-serif";
    canvas::TextLayout text;
    std::string direction = "inherit";
    std::string letter_spacing = "0px";
    std::string word_spacing = "0px";
    std::string font_kerning = "auto";
    std::string font_stretch = "normal";
    std::string font_variant_caps = "normal";
    std::string text_rendering = "auto";
    std::string lang = "inherit";
    std::string filter = "none";
    std::string smoothing_quality = "low";
};

class ContextObject final : public js::Object {
public:
    ContextObject(js::Object* prototype, CanvasStateObject& the_owner)
        : Object(prototype, Class::Host)
        , owner(&the_owner)
    {
    }
    CanvasStateObject* owner;
    ContextState state;
    std::vector<ContextState> stack;
    canvas::PathBuilder path;
    bool alpha = true;
    bool will_read_frequently = false;
    bool desynchronized = false;

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(owner);
        tracer.visit(state.fill_object);
        tracer.visit(state.stroke_object);
        for (ContextState const& saved : stack) {
            tracer.visit(saved.fill_object);
            tracer.visit(saved.stroke_object);
        }
    }
};

void CanvasStateObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(wrapper);
    tracer.visit(context);
}

class GradientObject final : public js::Object {
public:
    GradientObject(js::Object* prototype, std::shared_ptr<canvas::Gradient> the_gradient)
        : Object(prototype, Class::Host)
        , gradient(std::move(the_gradient))
    {
    }
    std::shared_ptr<canvas::Gradient> gradient;
};

class PatternObject final : public js::Object {
public:
    PatternObject(js::Object* prototype, std::shared_ptr<canvas::Pattern> the_pattern, bool clean)
        : Object(prototype, Class::Host)
        , pattern(std::move(the_pattern))
        , origin_clean(clean)
    {
    }
    std::shared_ptr<canvas::Pattern> pattern;
    bool origin_clean;
};

class Path2DObject final : public js::Object {
public:
    explicit Path2DObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    canvas::PathBuilder path;
    std::size_t size_in_bytes() const override { return sizeof(*this) + path.path.segments.capacity() * sizeof(svg::Path::Segment); }
};

class TextMetricsObject final : public js::Object {
public:
    TextMetricsObject(js::Object* prototype, canvas::TextMeasure the_measure)
        : Object(prototype, Class::Host)
        , measure(the_measure)
    {
    }
    canvas::TextMeasure measure;
};

class ImageDataObject final : public js::Object {
public:
    explicit ImageDataObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    int width = 0;
    int height = 0;
    js::Value data; // a Uint8ClampedArray
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(data);
    }
};

class ImageBitmapObject final : public js::Object {
public:
    explicit ImageBitmapObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::shared_ptr<Surface const> surface; // null once closed
    int width = 0;
    int height = 0;
    bool origin_clean = true;
    std::size_t size_in_bytes() const override { return sizeof(*this) + (surface ? surface->pixels.size() : 0); }
};

// An <img>'s picture as the realm fetched and decoded it, kept by the
// element's wrapper for the source it was had for: for a canvas, and for an
// image the host does not fetch (one made by script), whose load or error
// event the realm fires itself.
class PictureObject final : public js::Object {
public:
    PictureObject()
        : Object(nullptr, Class::Host)
    {
    }
    std::string source;
    std::shared_ptr<Surface const> surface;
    bool origin_clean = true;
    bool failed = false; // the fetch failed: nothing came
    bool broken = false; // failed, or bytes that are no picture
    bool settled = false; // the realm fired this source's load or error event
    // The source the last load the realm queued is for: a queued task for
    // any other source, overtaken by a newer one, fires nothing.
    std::string pending;
    std::size_t size_in_bytes() const override { return sizeof(*this) + (surface ? surface->pixels.size() : 0); }
};

// --- Helpers ---------------------------------------------------------------------------

template<typename T>
T* object_as(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<T*>(value.as_object()) : nullptr;
}

Native not_enough(js::Interpreter& interp, std::string_view method, std::string_view interface, std::size_t required, std::size_t given)
{
    return interp.throw_type_error("Failed to execute '" + std::string(method) + "' on '" + std::string(interface) + "': "
        + std::to_string(required) + " argument" + (required == 1 ? "" : "s") + " required, but only " + std::to_string(given)
        + " present.");
}

// The arguments converted to numbers (unrestricted double), in order; the
// exception pending when one threw.
std::optional<std::vector<double>> numbers_of(js::Interpreter& interp, Args args, std::size_t from, std::size_t count)
{
    std::vector<double> out;
    out.reserve(count);
    for (std::size_t i = from; i < from + count; ++i) {
        std::optional<double> const value = interp.to_number(js::argument(args, i));
        if (!value)
            return std::nullopt;
        out.push_back(*value);
    }
    return out;
}

bool all_finite(std::vector<double> const& values)
{
    return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
}

// WebIDL [EnforceRange] long.
std::optional<int> enforce_long(js::Interpreter& interp, js::Value const& value)
{
    std::optional<double> const number = interp.to_number(value);
    if (!number)
        return std::nullopt;
    if (!std::isfinite(*number))
        return interp.throw_type_error("The value is not a finite number.");
    double const truncated = std::trunc(*number);
    if (truncated < -2147483648.0 || truncated > 2147483647.0)
        return interp.throw_type_error("The value is outside the range of a long.");
    return static_cast<int>(truncated);
}

std::string number_text(double value) { return encode_utf8(js::number_to_string(value)); }

// A color as the context serializes it (HTML section4.12.5.1.3): #rrggbb when
// opaque, else rgba() with the alpha as short as it can be written and still
// come back to the same byte.
std::string serialize_color(Color color)
{
    char buffer[64];
    if (color.a == 255) {
        std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x", color.r, color.g, color.b);
        return buffer;
    }
    std::string alpha;
    if (color.a == 0) {
        alpha = "0";
    } else {
        for (int digits = 1; digits <= 3; ++digits) {
            double const scale = digits == 1 ? 10 : digits == 2 ? 100 : 1000;
            double const rounded = std::round(color.a / 255.0 * scale) / scale;
            if (static_cast<int>(std::lround(rounded * 255)) == color.a || digits == 3) {
                std::snprintf(buffer, sizeof buffer, "%.*f", digits, rounded);
                alpha = buffer;
                while (!alpha.empty() && alpha.back() == '0')
                    alpha.pop_back();
                if (!alpha.empty() && alpha.back() == '.')
                    alpha.pop_back();
                break;
            }
        }
    }
    std::snprintf(buffer, sizeof buffer, "rgba(%d, %d, %d, %s)", color.r, color.g, color.b, alpha.c_str());
    return buffer;
}

dom::Element* canvas_element(CanvasStateObject const& state)
{
    if (!state.wrapper || state.wrapper->detached())
        return nullptr;
    dom::Node& node = state.wrapper->node();
    return node.is_element() ? &static_cast<dom::Element&>(node) : nullptr;
}

// A color string as fillStyle, strokeStyle, shadowColor and addColorStop
// take one; currentcolor is the canvas's own color, black without one.
std::optional<Color> parse_canvas_color(Realm::Internals& in, std::string_view text, dom::Element const* element)
{
    std::string trimmed(text);
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\n'))
        trimmed.pop_back();
    std::size_t start = 0;
    while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\t' || trimmed[start] == '\n'))
        ++start;
    if (ascii_lower(std::string_view(trimmed).substr(start)) == "currentcolor") {
        if (element && in.hooks.computed_style && &element->root() == in.document) {
            if (css::ComputedStyle const* style = in.hooks.computed_style(*element))
                return style->color;
        }
        return Color { 0, 0, 0, 255 };
    }
    return css::parse_color_text(text);
}

// --- The canvas's state and bitmap -------------------------------------------------------

int dimension_of(dom::Element const& element, std::string_view name, int fallback)
{
    dom::Attr const* const attribute = element.find_attribute(name);
    if (!attribute)
        return fallback;
    std::optional<double> const value = parse_html_non_negative_integer(attribute->value);
    if (!value || *value > 2147483647.0)
        return fallback;
    return static_cast<int>(*value);
}

std::pair<int, int> canvas_size(dom::Element const& element)
{
    return { dimension_of(element, "width", 300), dimension_of(element, "height", 150) };
}

CanvasStateObject& state_of(Realm::Internals& in, dom::Element& element)
{
    NodeWrapper& wrapper = wrapper_for(in, element);
    if (js::Object* const kept = wrapper.same_object("canvas state"))
        return *static_cast<CanvasStateObject*>(kept);
    CanvasStateObject* made = nullptr;
    {
        js::Heap::NoCollect const no_collect(in.interpreter.heap());
        made = in.interpreter.heap().allocate<CanvasStateObject>(wrapper);
        wrapper.keep_same_object("canvas state", made);
    }
    return *made;
}

std::shared_ptr<Surface> make_surface(std::pair<int, int> size)
{
    std::size_t const pixels = static_cast<std::size_t>(size.first) * static_cast<std::size_t>(size.second);
    if (pixels > max_canvas_pixels)
        return std::make_shared<Surface>();
    return std::make_shared<Surface>(size.first, size.second);
}

// An opaque canvas's alpha fixed at one. The colors are premultiplied, so
// what was drawn with less than full alpha reads as drawn over black.
void force_opaque(Surface& surface)
{
    for (std::size_t i = 3; i < surface.pixels.size(); i += 4)
        surface.pixels[i] = 255;
}

// The canvas's bitmap, made at its attributes' size the first time it is needed.
Surface& surface_of(CanvasStateObject& state)
{
    if (!state.surface) {
        dom::Element* element = canvas_element(state);
        state.surface = make_surface(element ? canvas_size(*element) : std::pair { 300, 150 });
        if (state.opaque)
            force_opaque(*state.surface);
    }
    return *state.surface;
}

// A change to the bitmap: the host's picture is out of date, and the canvas
// is listed for the host if it is not yet.
void touched(Realm::Internals& in, CanvasStateObject& state)
{
    if (state.opaque && state.surface)
        force_opaque(*state.surface);
    ++state.painted;
    if (!state.listed) {
        state.listed = true;
        in.canvases.push_back(&state);
    }
}

void reset_context(ContextObject& context)
{
    context.state = ContextState {};
    context.stack.clear();
    context.path = canvas::PathBuilder {};
}

// --- Picture sources ---------------------------------------------------------------------

// What a CanvasImageSource offers to draw (HTML section4.12.5.1.15, "check the
// usability of the image argument"): a picture; nothing yet (draw nothing);
// or an error thrown.
struct SourcePicture {
    std::shared_ptr<Surface const> surface;
    bool origin_clean = true;
    bool usable = false; // false: draw nothing and return
};

std::optional<std::vector<std::uint8_t>> fetch_picture_bytes(Realm::Internals& in, net::Url const& url, bool cors, bool& clean)
{
    clean = true;
    if (url.scheme == "data") {
        std::optional<net::DataUrlPayload> payload = net::parse_data_url(url);
        if (!payload)
            return std::nullopt;
        return std::move(payload->bytes);
    }
    if (url.scheme == "blob") {
        auto const it = in.agent.blob_urls.find(url.serialize());
        if (it == in.agent.blob_urls.end())
            return std::nullopt;
        return it->second.bytes;
    }
    PageRequest request;
    request.url = url;
    request.mode = cors ? FetchMode::Cors : FetchMode::NoCors;
    request.destination = "image";
    FetchOutcome const outcome = perform_fetch(in, request);
    if (!outcome.ok || outcome.status < 200 || outcome.status > 299)
        return std::nullopt;
    clean = outcome.type != "opaque";
    return outcome.body;
}

std::optional<Bitmap> decode_picture(std::vector<std::uint8_t> const& bytes)
{
    if (looks_like_png(bytes))
        return decode_png(bytes);
    if (looks_like_gif(bytes))
        return decode_gif(bytes);
    if (looks_like_jpeg(bytes))
        return decode_jpeg(bytes);
    if (looks_like_bmp(bytes))
        return decode_bmp(bytes);
    // A vector picture is not drawn here: its decoder styles it through the
    // cascade, which is the host's to run, and a script may run on any of
    // the runners' threads.
    return std::nullopt;
}

bool same_origin_url(Realm::Internals& in, net::Url const& url)
{
    if (url.scheme == "data" || url.scheme == "blob" || url.scheme == "about")
        return true;
    return url.serialize_origin() == in.origin_url.serialize_origin();
}

// Whether a picture the host fetched is CORS-same-origin with the document
// (HTML section 2.5.4): its bytes came from the document's origin where its
// redirects ended, or, for an element with a crossorigin attribute, from an
// origin whose response passed the CORS check (Fetch section 4.9) for the
// attribute's credentials mode. A picture whose source the host cannot
// say is another origin's.
bool host_picture_clean(Realm::Internals& in, dom::Element const& image, HostPicture const& picture)
{
    if (!picture.from)
        return false;
    if (same_origin_url(in, *picture.from))
        return true;
    dom::Attr const* const crossorigin = image.find_attribute("crossorigin");
    if (!crossorigin)
        return false;
    bool const credentials = ascii_lower(crossorigin->value) == "use-credentials";
    if (picture.allow_origin == "*")
        return !credentials;
    if (picture.allow_origin != in.origin_url.serialize_origin())
        return false;
    return !credentials || picture.allow_credentials;
}

// The picture of an <img>'s source as fetched and decoded here, the first
// time it is asked for and then kept for that source.
PictureObject& fetched_picture(Realm::Internals& in, dom::Element& image, net::Url const& url)
{
    NodeWrapper& wrapper = wrapper_for(in, image);
    auto* kept = static_cast<PictureObject*>(wrapper.same_object("canvas picture"));
    std::string const key = url.serialize();
    if (kept && kept->source == key)
        return *kept;
    bool const cors = image.find_attribute("crossorigin") != nullptr;
    bool clean = true;
    std::optional<std::vector<std::uint8_t>> const bytes = fetch_picture_bytes(in, url, cors, clean);
    if (!kept) {
        js::Heap::NoCollect const no_collect(in.interpreter.heap());
        kept = in.interpreter.heap().allocate<PictureObject>();
        wrapper.keep_same_object("canvas picture", kept);
    }
    if (kept->source != key)
        kept->settled = false;
    kept->source = key;
    kept->surface.reset();
    kept->failed = !bytes;
    kept->broken = !bytes;
    if (bytes) {
        if (std::optional<Bitmap> decoded = decode_picture(*bytes))
            kept->surface = std::make_shared<Surface const>(Surface::from_bitmap(*decoded));
        else
            kept->broken = !svg::looks_like_svg(*bytes);
    }
    kept->origin_clean = clean && (cors || same_origin_url(in, url));
    return *kept;
}

// An <img>'s picture: from the host when it holds it, else fetched and
// decoded here the first time for its source.
Native image_picture(Realm::Internals& in, dom::Element& image, SourcePicture& out)
{
    dom::Attr const* const src = image.find_attribute("src");
    if (!src || src->value.empty()) {
        // No picture named: nothing to draw.
        return js::Value::undefined();
    }
    std::optional<net::Url> const url = net::parse_url(src->value, &in.base_url());
    if (!url)
        return in.throw_dom_exception("InvalidStateError", "The image is broken.");
    // The host's picture for an element it fetches (one in the document);
    // one it does not fetch is fetched here.
    ImageState const state = in.hooks.image_picture && in.hooks.image_state ? in.hooks.image_state(image) : ImageState::None;
    if (state != ImageState::None) {
        if (state == ImageState::Pending)
            return js::Value::undefined();
        if (state == ImageState::Broken)
            return in.throw_dom_exception("InvalidStateError", "The image is broken.");
        HostPicture const held = in.hooks.image_picture(image);
        if (!held.bitmap)
            return js::Value::undefined();
        out.surface = std::make_shared<Surface const>(Surface::from_bitmap(*held.bitmap));
        out.origin_clean = host_picture_clean(in, image, held);
        out.usable = true;
        return js::Value::undefined();
    }
    // Fetched now even while the load the realm queued for it waits for its
    // task, as a picture in the list of available images is had at once
    // (HTML section4.8.4.3.4's update the image data, step 6).
    PictureObject const& picture = fetched_picture(in, image, *url);
    // A picture that could not be had is broken once its failure is known:
    // its element is in the document, whose load it has had time to fail by,
    // or its error event has been fired. Before that one made by script is
    // taken as still on its way (draw nothing), as a browser's would be.
    // Bytes that are no picture are broken; a vector picture has nothing to
    // draw here.
    bool const broken = picture.broken && (!picture.failed || picture.settled || &image.root() == in.document);
    if (broken)
        return in.throw_dom_exception("InvalidStateError", "The image is broken.");
    out.surface = picture.surface;
    out.origin_clean = picture.origin_clean;
    out.usable = picture.surface && picture.surface->width > 0 && picture.surface->height > 0;
    return js::Value::undefined();
}

// The picture of any CanvasImageSource; the exception pending on nullopt.
Native source_picture(Realm::Internals& in, js::Value const& value, SourcePicture& out, std::string_view method)
{
    js::Interpreter& interp = in.interpreter;
    if (NodeWrapper* const wrapper = in.wrapper_of(value); wrapper && wrapper->node().is_element()) {
        auto& element = static_cast<dom::Element&>(wrapper->node());
        if (element.is_html("img") || element.is_svg("image")) {
            if (element.is_svg("image"))
                return js::Value::undefined();
            return image_picture(in, element, out);
        }
        if (element.is_html("canvas")) {
            CanvasStateObject& state = state_of(in, element);
            Surface const& surface = surface_of(state);
            if (surface.width == 0 || surface.height == 0)
                return in.throw_dom_exception("InvalidStateError", "The canvas has no pixels.");
            out.surface = std::make_shared<Surface const>(surface);
            out.origin_clean = state.origin_clean;
            out.usable = true;
            return js::Value::undefined();
        }
        if (element.is_html("video")) {
            std::shared_ptr<Bitmap const> const picture = media_current_picture(in, element);
            if (!picture)
                return js::Value::undefined();
            out.surface = std::make_shared<Surface const>(Surface::from_bitmap(*picture));
            out.usable = true;
            return js::Value::undefined();
        }
    }
    if (auto* bitmap = object_as<ImageBitmapObject>(value)) {
        if (!bitmap->surface)
            return in.throw_dom_exception("InvalidStateError", "The ImageBitmap has been closed.");
        out.surface = bitmap->surface;
        out.origin_clean = bitmap->origin_clean;
        out.usable = true;
        return js::Value::undefined();
    }
    return interp.throw_type_error("Failed to execute '" + std::string(method)
        + "' on 'CanvasRenderingContext2D': The provided value is not of type '(HTMLCanvasElement or HTMLImageElement or HTMLVideoElement or ImageBitmap or OffscreenCanvas or SVGImageElement or VideoFrame)'.");
}

// --- The context's this ------------------------------------------------------------------

std::optional<ContextObject*> this_context(js::Interpreter& interp, js::Value const& this_value)
{
    if (auto* context = object_as<ContextObject>(this_value))
        return context;
    return interp.throw_type_error("Illegal invocation");
}

using ContextBody = std::function<Native(Realm::Internals&, ContextObject&, Args)>;

void context_method(Realm::Internals& in, js::Object& prototype, std::string_view name, int length, ContextBody body)
{
    define_operation(in.interpreter, prototype, name, length,
        [body = std::move(body)](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<ContextObject*> const self = this_context(interp, this_value);
            if (!self)
                return std::nullopt;
            return body(internals_of(interp), **self, args);
        });
}

using ContextGetter = std::function<Native(Realm::Internals&, ContextObject&)>;
using ContextSetter = std::function<Native(Realm::Internals&, ContextObject&, js::Value const&)>;

void context_attribute(Realm::Internals& in, js::Object& prototype, std::string_view name, ContextGetter getter, ContextSetter setter = {})
{
    js::NativeFunction::Callback set;
    if (setter) {
        set = [setter = std::move(setter)](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<ContextObject*> const self = this_context(interp, this_value);
            if (!self)
                return std::nullopt;
            Native const done = setter(internals_of(interp), **self, js::argument(args, 0));
            if (!done)
                return std::nullopt;
            return js::Value::undefined();
        };
    }
    define_getter(
        in, prototype, name,
        [getter = std::move(getter)](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<ContextObject*> const self = this_context(interp, this_value);
            if (!self)
                return std::nullopt;
            return getter(internals_of(interp), **self);
        },
        std::move(set));
}

// An enumerated attribute: a value outside the list is ignored.
void enum_attribute(Realm::Internals& in, js::Object& prototype, std::string_view name, std::vector<std::string> values,
    std::function<std::string(ContextObject const&)> read, std::function<void(ContextObject&, std::string const&)> write)
{
    context_attribute(
        in, prototype, name, [read](Realm::Internals& internals, ContextObject& c) -> Native { return internals.string(read(c)); },
        [values = std::move(values), write](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<std::string> const text = internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            if (std::find(values.begin(), values.end(), *text) != values.end())
                write(c, *text);
            return js::Value::undefined();
        });
}

// --- Painting through the context ----------------------------------------------------

std::optional<svg::FillRule> fill_rule_of(Realm::Internals& in, js::Value const& value)
{
    if (value.is_undefined())
        return svg::FillRule::NonZero;
    std::optional<std::string> const text = in.to_utf8(value);
    if (!text)
        return std::nullopt;
    if (*text == "nonzero")
        return svg::FillRule::NonZero;
    if (*text == "evenodd")
        return svg::FillRule::EvenOdd;
    return in.interpreter.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type CanvasFillRule.");
}

canvas::Transform transform_of(Matrix2D const& m) { return canvas::Transform { m.a, m.b, m.c, m.d, m.e, m.f }; }

void paint(Realm::Internals& in, ContextObject& context, svg::Mask const& coverage, canvas::Style const& style)
{
    Surface& surface = surface_of(*context.owner);
    canvas::paint_mask(surface, coverage, style, context.state.draw);
    touched(in, *context.owner);
}

// The style's picture, if it has one, taints the canvas when it is of
// another origin (HTML section4.12.5.1.21).
void note_style_origin(ContextObject& context, js::Value const& object)
{
    if (auto* pattern = object_as<PatternObject>(object); pattern && !pattern->origin_clean)
        context.owner->origin_clean = false;
}

// fill()'s, stroke()'s and clip()'s arguments: an optional Path2D first.
struct PathArgument {
    svg::Path const* path = nullptr; // device space
    svg::Path transformed;
    std::size_t next = 0; // the argument after it
};

PathArgument path_argument(ContextObject& context, Args args)
{
    PathArgument out;
    if (auto* given = object_as<Path2DObject>(js::argument(args, 0))) {
        out.transformed = given->path.path.transformed(context.state.draw.transform.to_matrix());
        out.path = &out.transformed;
        out.next = 1;
    } else {
        out.path = &context.path.path;
    }
    return out;
}

bool is_path_object(js::Value const& value) { return object_as<Path2DObject>(value) != nullptr; }

// --- ImageData -----------------------------------------------------------------------

std::optional<ImageDataObject*> new_image_data(Realm::Internals& in, int width, int height, js::Value data = js::Value::undefined())
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    if (data.is_undefined()) {
        if (static_cast<double>(width) * static_cast<double>(height) > static_cast<double>(max_canvas_pixels)) {
            interp.throw_range_error("The ImageData is too large.");
            return std::nullopt;
        }
        std::size_t const bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        std::optional<js::TypedArrayObject*> const array = js::new_typed_array(interp, js::ElementType::Uint8Clamped, static_cast<double>(bytes));
        if (!array)
            return std::nullopt;
        data = js::Value::object(*array);
    }
    interp.root(data);
    auto* image = interp.heap().allocate<ImageDataObject>(in.prototype("ImageData"));
    image->width = width;
    image->height = height;
    image->data = data;
    return image;
}

std::uint8_t* image_data_bytes(ImageDataObject const& image, std::size_t& length)
{
    length = 0;
    auto* array = object_as<js::TypedArrayObject>(image.data);
    if (!array || array->is_out_of_bounds())
        return nullptr;
    length = array->byte_length();
    return array->buffer()->data() + array->byte_offset();
}

// ImageDataSettings: its colorSpace, which must be srgb or display-p3.
std::optional<bool> read_image_data_settings(Realm::Internals& in, js::Value const& settings)
{
    if (settings.is_undefined() || settings.is_null())
        return true;
    if (!settings.is_object())
        return in.interpreter.throw_type_error("The settings are not a dictionary.");
    js::Interpreter& interp = in.interpreter;
    std::optional<js::Value> const space = interp.get(*settings.as_object(), interp.key("colorSpace"));
    if (!space)
        return std::nullopt;
    if (!space->is_undefined()) {
        std::optional<std::string> const text = in.to_utf8(*space);
        if (!text)
            return std::nullopt;
        if (*text != "srgb" && *text != "display-p3")
            return interp.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type PredefinedColorSpace.");
    }
    return true;
}

// --- Text ---------------------------------------------------------------------------

std::optional<std::u32string> text_argument(Realm::Internals& in, js::Value const& value)
{
    std::optional<std::string> const utf8 = in.to_utf8(value);
    if (!utf8)
        return std::nullopt;
    std::u32string text = decode_utf8(*utf8);
    // The text preparation algorithm (HTML section4.12.5.1.4): white space
    // becomes a space.
    for (char32_t& c : text) {
        if (c == U'\t' || c == U'\n' || c == U'\f' || c == U'\r')
            c = U' ';
    }
    return text;
}

// A CSS <length> as letterSpacing and wordSpacing take one, in px; em
// against the context's font.
struct Spacing {
    float px = 0;
    std::string text; // as the attribute serializes it
};

std::optional<Spacing> spacing_length(std::string_view text, float font_size)
{
    std::string const lower = ascii_lower(text);
    std::size_t end = 0;
    double value = 0;
    if (lower.empty() || !(std::isdigit(static_cast<unsigned char>(lower[0])) || lower[0] == '-' || lower[0] == '+' || lower[0] == '.'))
        return std::nullopt;
    try {
        value = std::stod(lower, &end);
    } catch (...) {
        return std::nullopt;
    }
    if (!std::isfinite(value))
        return std::nullopt;
    std::string const unit = lower.substr(end);
    double const em = static_cast<double>(font_size);
    struct Unit {
        std::string_view name;
        double px;
    };
    Unit const units[] = { { "px", 1 }, { "em", em }, { "rem", 16 }, { "ex", em / 2 }, { "ch", em / 2 }, { "ic", em }, { "pt", 4.0 / 3.0 },
        { "pc", 16 }, { "in", 96 }, { "cm", 96 / 2.54 }, { "mm", 96 / 25.4 }, { "q", 96 / 101.6 } };
    for (Unit const& u : units) {
        if (unit == u.name)
            return Spacing { static_cast<float>(value * u.px), number_text(value) + std::string(u.name) };
    }
    if (unit.empty() && value == 0)
        return Spacing { 0, "0px" };
    return std::nullopt;
}

bool is_generic_family(std::string_view family)
{
    std::string const lower = ascii_lower(family);
    for (std::string_view const generic : { "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "math",
             "emoji", "fangsong", "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded" }) {
        if (lower == generic)
            return true;
    }
    return false;
}

std::string font_serialization(css::FontShorthandValue const& font)
{
    std::string out;
    if (font.oblique)
        out += "oblique ";
    else if (font.italic)
        out += "italic ";
    if (font.small_caps)
        out += "small-caps ";
    if (font.weight != 400)
        out += (font.weight == 700 ? std::string("bold") : std::to_string(font.weight)) + " ";
    out += number_text(static_cast<double>(font.size)) + "px";
    for (std::size_t i = 0; i < font.families.size(); ++i) {
        std::string const& family = font.families[i];
        out += i == 0 ? " " : ", ";
        // A generic family is its keyword, lowercased; any other name is
        // written as it was unless something in it needs a string.
        if (is_generic_family(family)) {
            out += ascii_lower(family);
            continue;
        }
        bool plain = !family.empty() && !(family[0] >= '0' && family[0] <= '9');
        for (char const ch : family) {
            auto const u = static_cast<unsigned char>(ch);
            if (!(std::isalnum(u) || ch == '-' || ch == '_' || ch == ' ' || u >= 0x80))
                plain = false;
        }
        if (plain) {
            out += family;
            continue;
        }
        out += '"';
        for (char const ch : family) {
            if (ch == '"' || ch == '\\')
                out += '\\';
            out += ch;
        }
        out += '"';
    }
    return out;
}

// Text is shaped with the page's own fonts, through the host, which also
// keeps other pages' layouts out of the process's font manager meanwhile.
void with_page_fonts(Realm::Internals& in, std::function<void()> const& use)
{
    if (in.hooks.with_fonts)
        in.hooks.with_fonts(use);
    else
        use();
}

bool resolved_rtl(Realm::Internals& in, ContextObject& context)
{
    if (context.state.direction == "rtl")
        return true;
    if (context.state.direction == "ltr")
        return false;
    dom::Element* element = canvas_element(*context.owner);
    if (element && in.hooks.computed_style && &element->root() == in.document) {
        if (css::ComputedStyle const* style = in.hooks.computed_style(*element))
            return style->direction == css::Direction::Rtl;
    }
    for (dom::Node const* at = element; at; at = at->parent()) {
        if (!at->is_element())
            continue;
        if (dom::Attr const* dir = static_cast<dom::Element const*>(at)->find_attribute("dir"))
            return ascii_lower(dir->value) == "rtl";
    }
    return false;
}

Native draw_text(Realm::Internals& in, ContextObject& context, Args args, bool stroke, std::string_view method)
{
    js::Interpreter& interp = in.interpreter;
    if (args.size() < 3)
        return not_enough(interp, method, "CanvasRenderingContext2D", 3, args.size());
    std::optional<std::u32string> const text = text_argument(in, args[0]);
    if (!text)
        return std::nullopt;
    std::optional<std::vector<double>> const xy = numbers_of(interp, args, 1, 2);
    if (!xy)
        return std::nullopt;
    std::optional<double> max_width;
    if (args.size() > 3 && !args[3].is_undefined()) {
        std::optional<double> const given = interp.to_number(args[3]);
        if (!given)
            return std::nullopt;
        max_width = *given;
    }
    if (!all_finite(*xy) || (max_width && !std::isfinite(*max_width)))
        return js::Value::undefined();
    canvas::TextLayout layout = context.state.text;
    layout.rtl = resolved_rtl(in, context);
    Surface& surface = surface_of(*context.owner);
    std::optional<double> const pen = stroke ? std::optional<double>(context.state.pen.width) : std::nullopt;
    svg::Mask coverage;
    with_page_fonts(in, [&] {
        coverage = canvas::text_coverage(surface, *text, context.state.font, layout, (*xy)[0], (*xy)[1], max_width,
            context.state.draw.transform, pen);
    });
    paint(in, context, coverage, stroke ? context.state.stroke : context.state.fill);
    note_style_origin(context, stroke ? context.state.stroke_object : context.state.fill_object);
    return js::Value::undefined();
}

// --- Encoding ------------------------------------------------------------------------

std::vector<std::uint8_t> encode_canvas(Surface const& surface)
{
    return encode_png(surface.to_bitmap());
}

// --- Styles --------------------------------------------------------------------------

Native style_value(Realm::Internals& in, canvas::Style const& style, js::Value const& object)
{
    if (!object.is_undefined())
        return object;
    return in.string(serialize_color(style.color));
}

Native set_style(Realm::Internals& in, ContextObject& context, js::Value const& value, bool fill)
{
    canvas::Style& style = fill ? context.state.fill : context.state.stroke;
    js::Value& object = fill ? context.state.fill_object : context.state.stroke_object;
    if (auto* gradient = object_as<GradientObject>(value)) {
        style = canvas::Style {};
        style.gradient = gradient->gradient;
        object = value;
        return js::Value::undefined();
    }
    if (auto* pattern = object_as<PatternObject>(value)) {
        style = canvas::Style {};
        style.pattern = pattern->pattern;
        object = value;
        return js::Value::undefined();
    }
    std::optional<std::string> const text = in.to_utf8(value);
    if (!text)
        return std::nullopt;
    std::optional<Color> const color = parse_canvas_color(in, *text, canvas_element(*context.owner));
    if (!color)
        return js::Value::undefined();
    style = canvas::Style {};
    style.color = *color;
    object = js::Value::undefined();
    return js::Value::undefined();
}

// --- The installer's parts -------------------------------------------------------------

void install_path_methods(Realm::Internals& in, js::Object& prototype, bool on_context)
{
    // The builder behind `this` and the transform its points go through.
    struct Target {
        canvas::PathBuilder* path;
        canvas::Transform transform;
    };
    auto const target_of = [on_context](js::Interpreter& interp, js::Value const& this_value) -> std::optional<Target> {
        if (on_context) {
            if (auto* context = object_as<ContextObject>(this_value))
                return Target { &context->path, context->state.draw.transform };
        } else if (auto* path = object_as<Path2DObject>(this_value)) {
            return Target { &path->path, canvas::Transform {} };
        }
        return interp.throw_type_error("Illegal invocation");
    };
    std::string const interface = on_context ? "CanvasRenderingContext2D" : "Path2D";
    auto const method = [&](std::string_view name, std::size_t required,
                            std::function<Native(Realm::Internals&, Target&, std::vector<double> const&, Args)> body) {
        std::string const method_name(name);
        define_operation(in.interpreter, prototype, name, static_cast<int>(required),
            [target_of, required, body, method_name, interface](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<Target> target = target_of(interp, this_value);
                if (!target)
                    return std::nullopt;
                if (args.size() < required)
                    return not_enough(interp, method_name, interface, required, args.size());
                std::optional<std::vector<double>> const values = numbers_of(interp, args, 0, required);
                if (!values)
                    return std::nullopt;
                return body(internals_of(interp), *target, *values, args);
            });
    };
    method("closePath", 0, [](Realm::Internals&, Target& t, std::vector<double> const&, Args) -> Native {
        t.path->close();
        return js::Value::undefined();
    });
    method("moveTo", 2, [](Realm::Internals&, Target& t, std::vector<double> const& v, Args) -> Native {
        if (all_finite(v))
            t.path->move_to(t.transform, v[0], v[1]);
        return js::Value::undefined();
    });
    method("lineTo", 2, [](Realm::Internals&, Target& t, std::vector<double> const& v, Args) -> Native {
        if (all_finite(v))
            t.path->line_to(t.transform, v[0], v[1]);
        return js::Value::undefined();
    });
    method("quadraticCurveTo", 4, [](Realm::Internals&, Target& t, std::vector<double> const& v, Args) -> Native {
        if (all_finite(v))
            t.path->quadratic_to(t.transform, v[0], v[1], v[2], v[3]);
        return js::Value::undefined();
    });
    method("bezierCurveTo", 6, [](Realm::Internals&, Target& t, std::vector<double> const& v, Args) -> Native {
        if (all_finite(v))
            t.path->bezier_to(t.transform, v[0], v[1], v[2], v[3], v[4], v[5]);
        return js::Value::undefined();
    });
    method("arcTo", 5, [](Realm::Internals& internals, Target& t, std::vector<double> const& v, Args) -> Native {
        if (!all_finite(v))
            return js::Value::undefined();
        if (v[4] < 0)
            return internals.throw_dom_exception("IndexSizeError", "The radius provided is negative.");
        t.path->arc_to(t.transform, v[0], v[1], v[2], v[3], v[4]);
        return js::Value::undefined();
    });
    method("rect", 4, [](Realm::Internals&, Target& t, std::vector<double> const& v, Args) -> Native {
        if (all_finite(v))
            t.path->rect(t.transform, v[0], v[1], v[2], v[3]);
        return js::Value::undefined();
    });
    method("arc", 5, [](Realm::Internals& internals, Target& t, std::vector<double> const& v, Args args) -> Native {
        bool const anticlockwise = js::Interpreter::to_boolean(js::argument(args, 5));
        if (!all_finite(v))
            return js::Value::undefined();
        if (v[2] < 0)
            return internals.throw_dom_exception("IndexSizeError", "The radius provided is negative.");
        t.path->ellipse(t.transform, v[0], v[1], v[2], v[2], 0, v[3], v[4], anticlockwise);
        return js::Value::undefined();
    });
    method("ellipse", 7, [](Realm::Internals& internals, Target& t, std::vector<double> const& v, Args args) -> Native {
        bool const anticlockwise = js::Interpreter::to_boolean(js::argument(args, 7));
        if (!all_finite(v))
            return js::Value::undefined();
        if (v[2] < 0 || v[3] < 0)
            return internals.throw_dom_exception("IndexSizeError", "The radius provided is negative.");
        t.path->ellipse(t.transform, v[0], v[1], v[2], v[3], v[4], v[5], v[6], anticlockwise);
        return js::Value::undefined();
    });
    method("roundRect", 4, [](Realm::Internals& internals, Target& t, std::vector<double> const& v, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        // The radii: a number, a DOMPointInit, or a sequence of one to four
        // of them (HTML section4.12.5.1.14).
        js::Value const given = js::argument(args, 4);
        std::vector<js::Value> list;
        bool finite = true;
        if (given.is_undefined()) {
            list.push_back(js::Value::number(0));
        } else if (given.is_object() && !given.as_object()->is_callable()) {
            std::optional<js::Value> const iterator = interp.get(given, js::PropertyKey::symbol(interp.atoms().symbol_iterator));
            if (!iterator)
                return std::nullopt;
            if (iterator->is_undefined() || iterator->is_null()) {
                list.push_back(given);
            } else {
                std::optional<std::vector<js::Value>> items = interp.iterable_to_list(given);
                if (!items)
                    return std::nullopt;
                list = std::move(*items);
            }
        } else {
            list.push_back(given);
        }
        js::Interpreter::Roots const roots(interp);
        for (js::Value const& item : list)
            interp.root(item);
        if (list.empty() || list.size() > 4)
            return interp.throw_range_error("Failed to execute 'roundRect': " + std::to_string(list.size()) + " radii provided. Between one and four radii are necessary.");
        std::vector<svg::Point> radii;
        for (js::Value const& item : list) {
            double rx = 0;
            double ry = 0;
            if (item.is_object()) {
                std::optional<js::Value> const x = interp.get(*item.as_object(), interp.key("x"));
                if (!x)
                    return std::nullopt;
                std::optional<double> const nx = x->is_undefined() ? std::optional<double>(0) : interp.to_number(*x);
                if (!nx)
                    return std::nullopt;
                std::optional<js::Value> const y = interp.get(*item.as_object(), interp.key("y"));
                if (!y)
                    return std::nullopt;
                std::optional<double> const ny = y->is_undefined() ? std::optional<double>(0) : interp.to_number(*y);
                if (!ny)
                    return std::nullopt;
                rx = *nx;
                ry = *ny;
            } else {
                std::optional<double> const n = interp.to_number(item);
                if (!n)
                    return std::nullopt;
                rx = ry = *n;
            }
            if (!std::isfinite(rx) || !std::isfinite(ry)) {
                finite = false;
                continue;
            }
            if (rx < 0 || ry < 0)
                return interp.throw_range_error("Failed to execute 'roundRect': A radius provided is negative.");
            radii.push_back(svg::Point { static_cast<float>(rx), static_cast<float>(ry) });
        }
        if (!all_finite(v) || !finite)
            return js::Value::undefined();
        std::array<svg::Point, 4> corners;
        switch (radii.size()) {
        case 1:
            corners = { radii[0], radii[0], radii[0], radii[0] };
            break;
        case 2:
            corners = { radii[0], radii[1], radii[0], radii[1] };
            break;
        case 3:
            corners = { radii[0], radii[1], radii[2], radii[1] };
            break;
        default:
            corners = { radii[0], radii[1], radii[2], radii[3] };
            break;
        }
        t.path->round_rect(t.transform, v[0], v[1], v[2], v[3], corners);
        return js::Value::undefined();
    });
}

void install_context(Realm::Internals& in)
{
    js::Object* proto = define_interface(in, "CanvasRenderingContext2D", nullptr);
    js::Object& context = *proto;

    context_attribute(in, context, "canvas", [](Realm::Internals&, ContextObject& c) -> Native {
        return js::Value::object(c.owner->wrapper);
    });
    context_method(in, context, "getContextAttributes", 0, [](Realm::Internals& internals, ContextObject& c, Args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        js::Object* object = interp.new_object(interp.intrinsics().object_prototype);
        js::Interpreter::Roots const roots(interp);
        interp.root(js::Value::object(object));
        interp.create_data_property(*object, interp.key("alpha"), js::Value::boolean(c.alpha));
        // Each string rooted before the key is made: making a key may
        // collect.
        for (auto const& [name, text] : { std::pair<char const*, char const*> { "colorSpace", "srgb" }, std::pair<char const*, char const*> { "colorType", "unorm8" } }) {
            js::Value const value = internals.string(text);
            interp.root(value);
            interp.create_data_property(*object, interp.key(name), value);
        }
        interp.create_data_property(*object, interp.key("desynchronized"), js::Value::boolean(c.desynchronized));
        interp.create_data_property(*object, interp.key("willReadFrequently"), js::Value::boolean(c.will_read_frequently));
        return js::Value::object(object);
    });

    // --- State ---
    context_method(in, context, "save", 0, [](Realm::Internals&, ContextObject& c, Args) -> Native {
        if (c.stack.size() < 1024)
            c.stack.push_back(c.state);
        return js::Value::undefined();
    });
    context_method(in, context, "restore", 0, [](Realm::Internals&, ContextObject& c, Args) -> Native {
        if (!c.stack.empty()) {
            c.state = std::move(c.stack.back());
            c.stack.pop_back();
        }
        return js::Value::undefined();
    });
    context_method(in, context, "reset", 0, [](Realm::Internals& internals, ContextObject& c, Args) -> Native {
        reset_context(c);
        Surface& surface = surface_of(*c.owner);
        std::fill(surface.pixels.begin(), surface.pixels.end(), 0);
        touched(internals, *c.owner);
        return js::Value::undefined();
    });
    context_method(in, context, "isContextLost", 0, [](Realm::Internals&, ContextObject&, Args) -> Native { return js::Value::boolean(false); });

    // --- Transforms ---
    auto const transform_step = [&](std::string_view name, std::size_t count,
                                    std::function<canvas::Transform(std::vector<double> const&)> make, bool replace) {
        std::string const method(name);
        context_method(in, context, name, static_cast<int>(count),
            [count, make, replace, method](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
                if (args.size() < count)
                    return not_enough(internals.interpreter, method, "CanvasRenderingContext2D", count, args.size());
                std::optional<std::vector<double>> const v = numbers_of(internals.interpreter, args, 0, count);
                if (!v)
                    return std::nullopt;
                if (!all_finite(*v))
                    return js::Value::undefined();
                canvas::Transform const step = make(*v);
                c.state.draw.transform = replace ? step : c.state.draw.transform.multiply(step);
                return js::Value::undefined();
            });
    };
    transform_step("scale", 2, [](std::vector<double> const& v) { return canvas::Transform { v[0], 0, 0, v[1], 0, 0 }; }, false);
    transform_step("rotate", 1, [](std::vector<double> const& v) {
        canvas::SineCosine const turn = canvas::sine_cosine(v[0]);
        return canvas::Transform { turn.cosine, turn.sine, -turn.sine, turn.cosine, 0, 0 };
    }, false);
    transform_step("translate", 2, [](std::vector<double> const& v) { return canvas::Transform { 1, 0, 0, 1, v[0], v[1] }; }, false);
    transform_step("transform", 6, [](std::vector<double> const& v) { return canvas::Transform { v[0], v[1], v[2], v[3], v[4], v[5] }; }, false);
    context_method(in, context, "setTransform", 0, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        if (args.size() >= 6) {
            std::optional<std::vector<double>> const v = numbers_of(interp, args, 0, 6);
            if (!v)
                return std::nullopt;
            if (all_finite(*v))
                c.state.draw.transform = canvas::Transform { (*v)[0], (*v)[1], (*v)[2], (*v)[3], (*v)[4], (*v)[5] };
            return js::Value::undefined();
        }
        if (args.size() > 1)
            return not_enough(interp, "setTransform", "CanvasRenderingContext2D", 6, args.size());
        js::Value const init = js::argument(args, 0);
        if (!init.is_undefined() && !init.is_null() && !init.is_object())
            return interp.throw_type_error("Failed to execute 'setTransform' on 'CanvasRenderingContext2D': The provided value is not of type 'DOMMatrix2DInit'.");
        std::optional<Matrix2D> const m = matrix_2d_from_init(internals, init);
        if (!m)
            return std::nullopt;
        canvas::Transform const t = transform_of(*m);
        if (t.is_finite())
            c.state.draw.transform = t;
        return js::Value::undefined();
    });
    context_method(in, context, "resetTransform", 0, [](Realm::Internals&, ContextObject& c, Args) -> Native {
        c.state.draw.transform = canvas::Transform {};
        return js::Value::undefined();
    });
    context_method(in, context, "getTransform", 0, [](Realm::Internals& internals, ContextObject& c, Args) -> Native {
        canvas::Transform const& t = c.state.draw.transform;
        return new_dom_matrix_2d(internals, Matrix2D { t.a, t.b, t.c, t.d, t.e, t.f });
    });

    // --- Compositing and smoothing ---
    context_attribute(
        in, context, "globalAlpha", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::number(c.state.draw.global_alpha); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            if (std::isfinite(*number) && *number >= 0 && *number <= 1)
                c.state.draw.global_alpha = *number;
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "globalCompositeOperation",
        [](Realm::Internals& internals, ContextObject& c) -> Native { return internals.string(canvas::composite_name(c.state.draw.composite)); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<std::string> const text = internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            if (std::optional<canvas::Composite> const op = canvas::composite_by_name(*text))
                c.state.draw.composite = *op;
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "imageSmoothingEnabled", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::boolean(c.state.draw.smoothing); },
        [](Realm::Internals&, ContextObject& c, js::Value const& value) -> Native {
            c.state.draw.smoothing = js::Interpreter::to_boolean(value);
            return js::Value::undefined();
        });
    enum_attribute(in, context, "imageSmoothingQuality", { "low", "medium", "high" },
        [](ContextObject const& c) { return c.state.smoothing_quality; },
        [](ContextObject& c, std::string const& v) { c.state.smoothing_quality = v; });

    // --- Fill and stroke styles ---
    context_attribute(
        in, context, "fillStyle", [](Realm::Internals& internals, ContextObject& c) -> Native { return style_value(internals, c.state.fill, c.state.fill_object); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native { return set_style(internals, c, value, true); });
    context_attribute(
        in, context, "strokeStyle", [](Realm::Internals& internals, ContextObject& c) -> Native { return style_value(internals, c.state.stroke, c.state.stroke_object); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native { return set_style(internals, c, value, false); });
    auto const make_gradient = [](Realm::Internals& internals, canvas::Gradient gradient) -> Native {
        return js::Value::object(internals.interpreter.heap().allocate<GradientObject>(internals.prototype("CanvasGradient"),
            std::make_shared<canvas::Gradient>(std::move(gradient))));
    };
    auto const gradient_numbers = [](Realm::Internals& internals, Args args, std::size_t count, std::string const& method) -> std::optional<std::vector<double>> {
        js::Interpreter& interp = internals.interpreter;
        if (args.size() < count) {
            not_enough(interp, method, "CanvasRenderingContext2D", count, args.size());
            return std::nullopt;
        }
        std::optional<std::vector<double>> const v = numbers_of(interp, args, 0, count);
        if (!v)
            return std::nullopt;
        if (!all_finite(*v)) {
            interp.throw_type_error("Failed to execute '" + method + "' on 'CanvasRenderingContext2D': The provided double value is non-finite.");
            return std::nullopt;
        }
        return v;
    };
    context_method(in, context, "createLinearGradient", 4, [make_gradient, gradient_numbers](Realm::Internals& internals, ContextObject&, Args args) -> Native {
        std::optional<std::vector<double>> const v = gradient_numbers(internals, args, 4, "createLinearGradient");
        if (!v)
            return std::nullopt;
        canvas::Gradient g;
        g.kind = canvas::Gradient::Kind::Linear;
        g.x0 = (*v)[0];
        g.y0 = (*v)[1];
        g.x1 = (*v)[2];
        g.y1 = (*v)[3];
        return make_gradient(internals, std::move(g));
    });
    context_method(in, context, "createRadialGradient", 6, [make_gradient, gradient_numbers](Realm::Internals& internals, ContextObject&, Args args) -> Native {
        std::optional<std::vector<double>> const v = gradient_numbers(internals, args, 6, "createRadialGradient");
        if (!v)
            return std::nullopt;
        if ((*v)[2] < 0 || (*v)[5] < 0)
            return internals.throw_dom_exception("IndexSizeError", "The radius provided is negative.");
        canvas::Gradient g;
        g.kind = canvas::Gradient::Kind::Radial;
        g.x0 = (*v)[0];
        g.y0 = (*v)[1];
        g.r0 = (*v)[2];
        g.x1 = (*v)[3];
        g.y1 = (*v)[4];
        g.r1 = (*v)[5];
        return make_gradient(internals, std::move(g));
    });
    context_method(in, context, "createConicGradient", 3, [make_gradient, gradient_numbers](Realm::Internals& internals, ContextObject&, Args args) -> Native {
        std::optional<std::vector<double>> const v = gradient_numbers(internals, args, 3, "createConicGradient");
        if (!v)
            return std::nullopt;
        canvas::Gradient g;
        g.kind = canvas::Gradient::Kind::Conic;
        g.angle = (*v)[0];
        g.x0 = (*v)[1];
        g.y0 = (*v)[2];
        return make_gradient(internals, std::move(g));
    });
    context_method(in, context, "createPattern", 2, [](Realm::Internals& internals, ContextObject&, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        if (args.size() < 2)
            return not_enough(interp, "createPattern", "CanvasRenderingContext2D", 2, args.size());
        SourcePicture picture;
        if (!source_picture(internals, args[0], picture, "createPattern"))
            return std::nullopt;
        std::string repetition;
        if (!args[1].is_null()) {
            std::optional<std::string> const text = internals.to_utf8(args[1]);
            if (!text)
                return std::nullopt;
            repetition = *text;
        }
        auto pattern = std::make_shared<canvas::Pattern>();
        if (repetition.empty() || repetition == "repeat") {
        } else if (repetition == "repeat-x") {
            pattern->repeat_y = false;
        } else if (repetition == "repeat-y") {
            pattern->repeat_x = false;
        } else if (repetition == "no-repeat") {
            pattern->repeat_x = pattern->repeat_y = false;
        } else {
            return internals.throw_dom_exception("SyntaxError", "The provided repetition type is not one of 'repeat', 'repeat-x', 'repeat-y' and 'no-repeat'.");
        }
        if (!picture.usable)
            return js::Value::null();
        pattern->image = picture.surface;
        return js::Value::object(interp.heap().allocate<PatternObject>(internals.prototype("CanvasPattern"), std::move(pattern), picture.origin_clean));
    });

    // --- Shadows ---
    context_attribute(
        in, context, "shadowOffsetX", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::number(c.state.draw.shadow_offset_x); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            if (std::isfinite(*number))
                c.state.draw.shadow_offset_x = *number;
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "shadowOffsetY", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::number(c.state.draw.shadow_offset_y); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            if (std::isfinite(*number))
                c.state.draw.shadow_offset_y = *number;
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "shadowBlur", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::number(c.state.draw.shadow_blur); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            if (std::isfinite(*number) && *number >= 0)
                c.state.draw.shadow_blur = *number;
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "shadowColor", [](Realm::Internals& internals, ContextObject& c) -> Native { return internals.string(serialize_color(c.state.draw.shadow_color)); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<std::string> const text = internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            if (std::optional<Color> const color = parse_canvas_color(internals, *text, canvas_element(*c.owner)))
                c.state.draw.shadow_color = *color;
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "filter", [](Realm::Internals& internals, ContextObject& c) -> Native { return internals.string(c.state.filter); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<std::string> const text = internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            c.state.filter = *text;
            return js::Value::undefined();
        });

    // --- Rectangles ---
    auto const rect_method = [&](std::string_view name, std::function<void(Realm::Internals&, ContextObject&, double, double, double, double)> body) {
        std::string const method(name);
        context_method(in, context, name, 4, [body, method](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
            if (args.size() < 4)
                return not_enough(internals.interpreter, method, "CanvasRenderingContext2D", 4, args.size());
            std::optional<std::vector<double>> const v = numbers_of(internals.interpreter, args, 0, 4);
            if (!v)
                return std::nullopt;
            if (all_finite(*v))
                body(internals, c, (*v)[0], (*v)[1], (*v)[2], (*v)[3]);
            return js::Value::undefined();
        });
    };
    rect_method("clearRect", [](Realm::Internals& internals, ContextObject& c, double x, double y, double w, double h) {
        if (w == 0 || h == 0)
            return;
        canvas::clear_rect(surface_of(*c.owner), x, y, w, h, c.state.draw);
        touched(internals, *c.owner);
    });
    rect_method("fillRect", [](Realm::Internals& internals, ContextObject& c, double x, double y, double w, double h) {
        if (w == 0 || h == 0)
            return;
        canvas::PathBuilder rectangle;
        rectangle.rect(c.state.draw.transform, x, y, w, h);
        paint(internals, c, canvas::fill_coverage(surface_of(*c.owner), rectangle.path, svg::FillRule::NonZero, &c.state.draw), c.state.fill);
        note_style_origin(c, c.state.fill_object);
    });
    rect_method("strokeRect", [](Realm::Internals& internals, ContextObject& c, double x, double y, double w, double h) {
        if (w == 0 && h == 0)
            return;
        // One side zero is a closed rectangle all the same: a line traced
        // out and back, joined at both ends and never capped.
        canvas::PathBuilder rectangle;
        rectangle.rect(c.state.draw.transform, x, y, w, h);
        paint(internals, c, canvas::stroke_coverage(surface_of(*c.owner), rectangle.path, c.state.pen, c.state.draw), c.state.stroke);
        note_style_origin(c, c.state.stroke_object);
    });

    // --- Paths ---
    context_method(in, context, "beginPath", 0, [](Realm::Internals&, ContextObject& c, Args) -> Native {
        c.path = canvas::PathBuilder {};
        return js::Value::undefined();
    });
    context_method(in, context, "fill", 0, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        PathArgument const target = path_argument(c, args);
        std::optional<svg::FillRule> const rule = fill_rule_of(internals, js::argument(args, target.next));
        if (!rule)
            return std::nullopt;
        paint(internals, c, canvas::fill_coverage(surface_of(*c.owner), *target.path, *rule, &c.state.draw), c.state.fill);
        note_style_origin(c, c.state.fill_object);
        return js::Value::undefined();
    });
    context_method(in, context, "stroke", 0, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        PathArgument const target = path_argument(c, args);
        paint(internals, c, canvas::stroke_coverage(surface_of(*c.owner), *target.path, c.state.pen, c.state.draw), c.state.stroke);
        note_style_origin(c, c.state.stroke_object);
        return js::Value::undefined();
    });
    context_method(in, context, "clip", 0, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        PathArgument const target = path_argument(c, args);
        std::optional<svg::FillRule> const rule = fill_rule_of(internals, js::argument(args, target.next));
        if (!rule)
            return std::nullopt;
        c.state.draw.clip = canvas::intersect_clip(surface_of(*c.owner), c.state.draw.clip, *target.path, *rule);
        return js::Value::undefined();
    });
    context_method(in, context, "isPointInPath", 2, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        bool const with_path = is_path_object(js::argument(args, 0));
        std::size_t const first = with_path ? 1 : 0;
        if (args.size() < first + 2)
            return not_enough(interp, "isPointInPath", "CanvasRenderingContext2D", first + 2, args.size());
        std::optional<std::vector<double>> const v = numbers_of(interp, args, first, 2);
        if (!v)
            return std::nullopt;
        std::optional<svg::FillRule> const rule = fill_rule_of(internals, js::argument(args, first + 2));
        if (!rule)
            return std::nullopt;
        if (!all_finite(*v) || !c.state.draw.transform.inverse())
            return js::Value::boolean(false);
        PathArgument const target = path_argument(c, args);
        return js::Value::boolean(canvas::point_in_path(*target.path, *rule, (*v)[0], (*v)[1]));
    });
    context_method(in, context, "isPointInStroke", 2, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        bool const with_path = is_path_object(js::argument(args, 0));
        std::size_t const first = with_path ? 1 : 0;
        if (args.size() < first + 2)
            return not_enough(interp, "isPointInStroke", "CanvasRenderingContext2D", first + 2, args.size());
        std::optional<std::vector<double>> const v = numbers_of(interp, args, first, 2);
        if (!v)
            return std::nullopt;
        if (!all_finite(*v))
            return js::Value::boolean(false);
        PathArgument const target = path_argument(c, args);
        return js::Value::boolean(canvas::point_in_stroke(*target.path, c.state.pen, c.state.draw.transform, (*v)[0], (*v)[1]));
    });
    context_method(in, context, "drawFocusIfNeeded", 1, [](Realm::Internals& internals, ContextObject&, Args args) -> Native {
        js::Value const element = is_path_object(js::argument(args, 0)) ? js::argument(args, 1) : js::argument(args, 0);
        NodeWrapper* const wrapper = internals.wrapper_of(element);
        if (!wrapper || !wrapper->node().is_element())
            return internals.interpreter.throw_type_error("Failed to execute 'drawFocusIfNeeded': parameter is not of type 'Element'.");
        return js::Value::undefined();
    });
    install_path_methods(in, context, true);

    // --- Line styles ---
    context_attribute(
        in, context, "lineWidth", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::number(c.state.line_width); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            if (std::isfinite(*number) && *number > 0) {
                c.state.line_width = *number;
                c.state.pen.width = static_cast<float>(*number);
            }
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "miterLimit", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::number(c.state.miter_limit); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            if (std::isfinite(*number) && *number > 0) {
                c.state.miter_limit = *number;
                c.state.pen.miter_limit = static_cast<float>(*number);
            }
            return js::Value::undefined();
        });
    context_attribute(
        in, context, "lineDashOffset", [](Realm::Internals&, ContextObject& c) -> Native { return js::Value::number(c.state.dash_offset); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            if (std::isfinite(*number)) {
                c.state.dash_offset = *number;
                c.state.pen.dash_offset = static_cast<float>(*number);
            }
            return js::Value::undefined();
        });
    enum_attribute(in, context, "lineCap", { "butt", "round", "square" },
        [](ContextObject const& c) -> std::string {
            switch (c.state.pen.cap) {
            case svg::LineCap::Round: return "round";
            case svg::LineCap::Square: return "square";
            case svg::LineCap::Butt: return "butt";
            }
            return "butt";
        },
        [](ContextObject& c, std::string const& v) { c.state.pen.cap = v == "round" ? svg::LineCap::Round : v == "square" ? svg::LineCap::Square : svg::LineCap::Butt; });
    enum_attribute(in, context, "lineJoin", { "round", "bevel", "miter" },
        [](ContextObject const& c) -> std::string {
            switch (c.state.pen.join) {
            case svg::LineJoin::Round: return "round";
            case svg::LineJoin::Bevel: return "bevel";
            case svg::LineJoin::Miter: return "miter";
            }
            return "miter";
        },
        [](ContextObject& c, std::string const& v) { c.state.pen.join = v == "round" ? svg::LineJoin::Round : v == "bevel" ? svg::LineJoin::Bevel : svg::LineJoin::Miter; });
    context_method(in, context, "setLineDash", 1, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        if (args.empty())
            return not_enough(interp, "setLineDash", "CanvasRenderingContext2D", 1, 0);
        if (!args[0].is_object())
            return interp.throw_type_error("Failed to execute 'setLineDash' on 'CanvasRenderingContext2D': The provided value cannot be converted to a sequence.");
        std::optional<std::vector<js::Value>> const items = interp.iterable_to_list(args[0]);
        if (!items)
            return std::nullopt;
        std::vector<double> dashes;
        for (js::Value const& item : *items) {
            std::optional<double> const number = interp.to_number(item);
            if (!number)
                return std::nullopt;
            if (!std::isfinite(*number) || *number < 0)
                return js::Value::undefined();
            dashes.push_back(*number);
        }
        if (dashes.size() % 2 == 1) {
            std::size_t const n = dashes.size();
            for (std::size_t i = 0; i < n; ++i)
                dashes.push_back(dashes[i]);
        }
        c.state.pen.dashes.clear();
        for (double const dash : dashes)
            c.state.pen.dashes.push_back(static_cast<float>(dash));
        c.state.dashes = std::move(dashes);
        return js::Value::undefined();
    });
    context_method(in, context, "getLineDash", 0, [](Realm::Internals& internals, ContextObject& c, Args) -> Native {
        std::vector<js::Value> values;
        for (double const dash : c.state.dashes)
            values.push_back(js::Value::number(dash));
        return js::Value::object(internals.interpreter.new_array(values));
    });

    // --- Text styles ---
    context_attribute(
        in, context, "font", [](Realm::Internals& internals, ContextObject& c) -> Native { return internals.string(c.state.font_text); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<std::string> const text = internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            // Relative sizes are against the canvas's own font size while it
            // is in the document, else against 10px (HTML section4.12.5.1.4).
            float parent_size = 10;
            dom::Element* element = canvas_element(*c.owner);
            if (element && internals.hooks.computed_style && &element->root() == internals.document) {
                if (css::ComputedStyle const* style = internals.hooks.computed_style(*element))
                    parent_size = style->font_size;
            }
            std::optional<css::FontShorthandValue> parsed;
            with_page_fonts(internals, [&] { parsed = css::parse_font_shorthand_text(*text, parent_size); });
            if (!parsed)
                return js::Value::undefined();
            for (std::string const& family : parsed->families) {
                std::string const lower = ascii_lower(family);
                if (lower == "initial" || lower == "inherit" || lower == "unset" || lower == "default" || lower == "revert"
                    || lower == "revert-layer")
                    return js::Value::undefined();
            }
            c.state.font.size = parsed->size;
            c.state.font.weight = parsed->weight;
            c.state.font.italic = parsed->italic;
            c.state.font.stretch = parsed->stretch;
            c.state.font.small_caps = parsed->small_caps;
            c.state.font.families = parsed->families;
            c.state.font_text = font_serialization(*parsed);
            // A spacing written in the font's units follows the new font.
            if (std::optional<Spacing> const letters = spacing_length(c.state.letter_spacing, c.state.font.size))
                c.state.text.letter_spacing = letters->px;
            if (std::optional<Spacing> const words = spacing_length(c.state.word_spacing, c.state.font.size))
                c.state.text.word_spacing = words->px;
            return js::Value::undefined();
        });
    enum_attribute(in, context, "textAlign", { "start", "end", "left", "right", "center" },
        [](ContextObject const& c) -> std::string {
            switch (c.state.text.align) {
            case canvas::TextLayout::Align::Start: return "start";
            case canvas::TextLayout::Align::End: return "end";
            case canvas::TextLayout::Align::Left: return "left";
            case canvas::TextLayout::Align::Right: return "right";
            case canvas::TextLayout::Align::Center: return "center";
            }
            return "start";
        },
        [](ContextObject& c, std::string const& v) {
            using Align = canvas::TextLayout::Align;
            c.state.text.align = v == "end" ? Align::End : v == "left" ? Align::Left : v == "right" ? Align::Right : v == "center" ? Align::Center : Align::Start;
        });
    enum_attribute(in, context, "textBaseline", { "top", "hanging", "middle", "alphabetic", "ideographic", "bottom" },
        [](ContextObject const& c) -> std::string {
            switch (c.state.text.baseline) {
            case canvas::TextLayout::Baseline::Top: return "top";
            case canvas::TextLayout::Baseline::Hanging: return "hanging";
            case canvas::TextLayout::Baseline::Middle: return "middle";
            case canvas::TextLayout::Baseline::Alphabetic: return "alphabetic";
            case canvas::TextLayout::Baseline::Ideographic: return "ideographic";
            case canvas::TextLayout::Baseline::Bottom: return "bottom";
            }
            return "alphabetic";
        },
        [](ContextObject& c, std::string const& v) {
            using Baseline = canvas::TextLayout::Baseline;
            c.state.text.baseline = v == "top" ? Baseline::Top : v == "hanging" ? Baseline::Hanging : v == "middle" ? Baseline::Middle
                : v == "ideographic" ? Baseline::Ideographic : v == "bottom" ? Baseline::Bottom : Baseline::Alphabetic;
        });
    enum_attribute(in, context, "direction", { "ltr", "rtl", "inherit" },
        [](ContextObject const& c) { return c.state.direction; },
        [](ContextObject& c, std::string const& v) { c.state.direction = v; });
    enum_attribute(in, context, "fontKerning", { "auto", "normal", "none" },
        [](ContextObject const& c) { return c.state.font_kerning; },
        [](ContextObject& c, std::string const& v) {
            c.state.font_kerning = v;
            c.state.text.kerning = v != "none";
        });
    enum_attribute(in, context, "fontStretch",
        { "ultra-condensed", "extra-condensed", "condensed", "semi-condensed", "normal", "semi-expanded", "expanded", "extra-expanded", "ultra-expanded" },
        [](ContextObject const& c) { return c.state.font_stretch; },
        [](ContextObject& c, std::string const& v) {
            static constexpr std::pair<std::string_view, int> stretches[] = { { "ultra-condensed", 50 }, { "extra-condensed", 62 },
                { "condensed", 75 }, { "semi-condensed", 87 }, { "normal", 100 }, { "semi-expanded", 112 }, { "expanded", 125 },
                { "extra-expanded", 150 }, { "ultra-expanded", 200 } };
            c.state.font_stretch = v;
            for (auto const& [name, percent] : stretches) {
                if (name == v)
                    c.state.font.stretch = percent;
            }
        });
    enum_attribute(in, context, "fontVariantCaps",
        { "normal", "small-caps", "all-small-caps", "petite-caps", "all-petite-caps", "unicase", "titling-caps" },
        [](ContextObject const& c) { return c.state.font_variant_caps; },
        [](ContextObject& c, std::string const& v) { c.state.font_variant_caps = v; });
    enum_attribute(in, context, "textRendering", { "auto", "optimizeSpeed", "optimizeLegibility", "geometricPrecision" },
        [](ContextObject const& c) { return c.state.text_rendering; },
        [](ContextObject& c, std::string const& v) { c.state.text_rendering = v; });
    context_attribute(
        in, context, "lang", [](Realm::Internals& internals, ContextObject& c) -> Native { return internals.string(c.state.lang); },
        [](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
            std::optional<std::string> const text = internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            c.state.lang = *text;
            return js::Value::undefined();
        });
    for (bool const letters : { true, false }) {
        context_attribute(
            in, context, letters ? "letterSpacing" : "wordSpacing",
            [letters](Realm::Internals& internals, ContextObject& c) -> Native { return internals.string(letters ? c.state.letter_spacing : c.state.word_spacing); },
            [letters](Realm::Internals& internals, ContextObject& c, js::Value const& value) -> Native {
                std::optional<std::string> const text = internals.to_utf8(value);
                if (!text)
                    return std::nullopt;
                std::optional<Spacing> const length = spacing_length(*text, c.state.font.size);
                if (!length)
                    return js::Value::undefined();
                if (letters) {
                    c.state.letter_spacing = length->text;
                    c.state.text.letter_spacing = length->px;
                } else {
                    c.state.word_spacing = length->text;
                    c.state.text.word_spacing = length->px;
                }
                return js::Value::undefined();
            });
    }

    // --- Text ---
    context_method(in, context, "fillText", 3, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        return draw_text(internals, c, args, false, "fillText");
    });
    context_method(in, context, "strokeText", 3, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        return draw_text(internals, c, args, true, "strokeText");
    });
    context_method(in, context, "measureText", 1, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        if (args.empty())
            return not_enough(internals.interpreter, "measureText", "CanvasRenderingContext2D", 1, 0);
        std::optional<std::u32string> const text = text_argument(internals, args[0]);
        if (!text)
            return std::nullopt;
        canvas::TextLayout layout = c.state.text;
        layout.rtl = resolved_rtl(internals, c);
        canvas::TextMeasure measure;
        with_page_fonts(internals, [&] { measure = canvas::measure_text(*text, c.state.font, layout); });
        return js::Value::object(internals.interpreter.heap().allocate<TextMetricsObject>(internals.prototype("TextMetrics"), measure));
    });

    // --- Images ---
    context_method(in, context, "drawImage", 3, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        std::size_t const count = args.size() >= 9 ? 9 : args.size() >= 5 ? 5 : args.size() >= 3 ? 3 : 0;
        if (count == 0 || args.size() == 4 || (args.size() > 5 && args.size() < 9))
            return interp.throw_type_error("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': Valid arities are: [3, 5, 9], but "
                + std::to_string(args.size()) + " arguments provided.");
        SourcePicture picture;
        // The numbers are converted before the image is looked at, as the
        // overload's arguments are.
        std::optional<std::vector<double>> const v = numbers_of(interp, args, 1, count - 1);
        if (!v)
            return std::nullopt;
        if (!source_picture(internals, args[0], picture, "drawImage"))
            return std::nullopt;
        if (!all_finite(*v) || !picture.usable || !picture.surface)
            return js::Value::undefined();
        Surface const& source = *picture.surface;
        double sx = 0, sy = 0, sw = source.width, sh = source.height;
        double dx = 0, dy = 0, dw = sw, dh = sh;
        if (count == 3) {
            dx = (*v)[0];
            dy = (*v)[1];
        } else if (count == 5) {
            dx = (*v)[0];
            dy = (*v)[1];
            dw = (*v)[2];
            dh = (*v)[3];
        } else {
            sx = (*v)[0];
            sy = (*v)[1];
            sw = (*v)[2];
            sh = (*v)[3];
            dx = (*v)[4];
            dy = (*v)[5];
            dw = (*v)[6];
            dh = (*v)[7];
        }
        if (sw == 0 || sh == 0)
            return js::Value::undefined();
        // Negative sizes flip the rectangles to their positive forms.
        if (sw < 0) {
            sx += sw;
            sw = -sw;
        }
        if (sh < 0) {
            sy += sh;
            sh = -sh;
        }
        if (dw < 0) {
            dx += dw;
            dw = -dw;
        }
        if (dh < 0) {
            dy += dh;
            dh = -dh;
        }
        // The source rectangle clipped to the picture, the destination with it.
        double const scale_x = dw / sw;
        double const scale_y = dh / sh;
        if (sx < 0) {
            dx -= sx * scale_x;
            dw += sx * scale_x;
            sw += sx;
            sx = 0;
        }
        if (sy < 0) {
            dy -= sy * scale_y;
            dh += sy * scale_y;
            sh += sy;
            sy = 0;
        }
        if (sx + sw > source.width) {
            double const cut = sx + sw - source.width;
            sw -= cut;
            dw -= cut * scale_x;
        }
        if (sy + sh > source.height) {
            double const cut = sy + sh - source.height;
            sh -= cut;
            dh -= cut * scale_y;
        }
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
            return js::Value::undefined();
        canvas::draw_image(surface_of(*c.owner), source, sx, sy, sw, sh, dx, dy, dw, dh, c.state.draw);
        if (!picture.origin_clean)
            c.owner->origin_clean = false;
        touched(internals, *c.owner);
        return js::Value::undefined();
    });

    // --- Pixels ---
    context_method(in, context, "createImageData", 1, [](Realm::Internals& internals, ContextObject&, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        if (args.empty())
            return not_enough(interp, "createImageData", "CanvasRenderingContext2D", 1, 0);
        if (auto* other = object_as<ImageDataObject>(args[0])) {
            std::optional<ImageDataObject*> const made = new_image_data(internals, other->width, other->height);
            if (!made)
                return std::nullopt;
            return js::Value::object(*made);
        }
        if (args.size() < 2)
            return interp.throw_type_error("Failed to execute 'createImageData' on 'CanvasRenderingContext2D': parameter 1 is not of type 'ImageData'.");
        std::optional<int> const w = enforce_long(interp, args[0]);
        if (!w)
            return std::nullopt;
        std::optional<int> const h = enforce_long(interp, args[1]);
        if (!h)
            return std::nullopt;
        if (!read_image_data_settings(internals, js::argument(args, 2)))
            return std::nullopt;
        if (*w == 0 || *h == 0)
            return internals.throw_dom_exception("IndexSizeError", "The source width and height must not be zero.");
        std::optional<ImageDataObject*> const made = new_image_data(internals, std::abs(*w), std::abs(*h));
        if (!made)
            return std::nullopt;
        return js::Value::object(*made);
    });
    context_method(in, context, "getImageData", 4, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        if (args.size() < 4)
            return not_enough(interp, "getImageData", "CanvasRenderingContext2D", 4, args.size());
        int v[4];
        for (std::size_t i = 0; i < 4; ++i) {
            std::optional<int> const n = enforce_long(interp, args[i]);
            if (!n)
                return std::nullopt;
            v[i] = *n;
        }
        if (!read_image_data_settings(internals, js::argument(args, 4)))
            return std::nullopt;
        if (v[2] == 0 || v[3] == 0)
            return internals.throw_dom_exception("IndexSizeError", "The source width and height must not be zero.");
        if (!c.owner->origin_clean)
            return internals.throw_dom_exception("SecurityError", "The canvas has been tainted by cross-origin data.");
        long long sx = v[0], sy = v[1], sw = v[2], sh = v[3];
        if (sw < 0) {
            sx += sw;
            sw = -sw;
        }
        if (sh < 0) {
            sy += sh;
            sh = -sh;
        }
        std::optional<ImageDataObject*> const made = new_image_data(internals, static_cast<int>(sw), static_cast<int>(sh));
        if (!made)
            return std::nullopt;
        std::size_t length = 0;
        std::uint8_t* out = image_data_bytes(**made, length);
        Surface const& surface = surface_of(*c.owner);
        for (long long row = 0; row < sh; ++row) {
            long long const y = sy + row;
            if (y < 0 || y >= surface.height)
                continue;
            for (long long col = 0; col < sw; ++col) {
                long long const x = sx + col;
                if (x < 0 || x >= surface.width)
                    continue;
                std::uint8_t const* p = surface.pixels.data() + surface.offset(static_cast<int>(x), static_cast<int>(y));
                std::uint8_t* q = out + static_cast<std::size_t>((row * sw + col) * 4);
                unsigned const a = p[3];
                if (a == 0) {
                    q[0] = q[1] = q[2] = q[3] = 0;
                    continue;
                }
                q[0] = static_cast<std::uint8_t>(std::min(255u, (p[0] * 255u + a / 2) / a));
                q[1] = static_cast<std::uint8_t>(std::min(255u, (p[1] * 255u + a / 2) / a));
                q[2] = static_cast<std::uint8_t>(std::min(255u, (p[2] * 255u + a / 2) / a));
                q[3] = static_cast<std::uint8_t>(a);
            }
        }
        return js::Value::object(*made);
    });
    context_method(in, context, "putImageData", 3, [](Realm::Internals& internals, ContextObject& c, Args args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        if (args.size() < 3)
            return not_enough(interp, "putImageData", "CanvasRenderingContext2D", 3, args.size());
        auto* image = object_as<ImageDataObject>(args[0]);
        if (!image)
            return interp.throw_type_error("Failed to execute 'putImageData' on 'CanvasRenderingContext2D': parameter 1 is not of type 'ImageData'.");
        if (args.size() != 3 && args.size() != 7)
            return interp.throw_type_error("Failed to execute 'putImageData' on 'CanvasRenderingContext2D': Valid arities are: [3, 7].");
        int v[7] = { 0, 0, 0, 0, image->width, image->height, 0 };
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::optional<int> const n = enforce_long(interp, args[i]);
            if (!n)
                return std::nullopt;
            v[i - 1] = *n;
        }
        std::size_t length = 0;
        std::uint8_t const* data = image_data_bytes(*image, length);
        if (!data && image->width * image->height > 0)
            return internals.throw_dom_exception("InvalidStateError", "The ImageData's buffer has been detached.");
        long long dx = v[0], dy = v[1];
        long long dirty_x = 0, dirty_y = 0, dirty_w = image->width, dirty_h = image->height;
        if (args.size() == 7) {
            dirty_x = v[2];
            dirty_y = v[3];
            dirty_w = v[4];
            dirty_h = v[5];
        }
        if (dirty_w < 0) {
            dirty_x += dirty_w;
            dirty_w = -dirty_w;
        }
        if (dirty_h < 0) {
            dirty_y += dirty_h;
            dirty_h = -dirty_h;
        }
        if (dirty_x < 0) {
            dirty_w += dirty_x;
            dirty_x = 0;
        }
        if (dirty_y < 0) {
            dirty_h += dirty_y;
            dirty_y = 0;
        }
        dirty_w = std::min<long long>(dirty_w, image->width - dirty_x);
        dirty_h = std::min<long long>(dirty_h, image->height - dirty_y);
        if (dirty_w <= 0 || dirty_h <= 0)
            return js::Value::undefined();
        Surface& surface = surface_of(*c.owner);
        for (long long row = dirty_y; row < dirty_y + dirty_h; ++row) {
            long long const y = dy + row;
            if (y < 0 || y >= surface.height)
                continue;
            for (long long col = dirty_x; col < dirty_x + dirty_w; ++col) {
                long long const x = dx + col;
                if (x < 0 || x >= surface.width)
                    continue;
                std::size_t const from = static_cast<std::size_t>((row * image->width + col) * 4);
                if (from + 3 >= length)
                    continue;
                std::uint8_t const* p = data + from;
                std::uint8_t* q = surface.pixels.data() + surface.offset(static_cast<int>(x), static_cast<int>(y));
                unsigned const a = p[3];
                q[0] = static_cast<std::uint8_t>((p[0] * a + 127) / 255);
                q[1] = static_cast<std::uint8_t>((p[1] * a + 127) / 255);
                q[2] = static_cast<std::uint8_t>((p[2] * a + 127) / 255);
                q[3] = static_cast<std::uint8_t>(a);
            }
        }
        touched(internals, *c.owner);
        return js::Value::undefined();
    });
}

void install_gradient_pattern_path(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* gradient = define_interface(in, "CanvasGradient", nullptr);
    define_operation(interpreter, *gradient, "addColorStop", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* self = object_as<GradientObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        Realm::Internals& internals = internals_of(interp);
        if (args.size() < 2)
            return not_enough(interp, "addColorStop", "CanvasGradient", 2, args.size());
        std::optional<double> const offset = interp.to_number(args[0]);
        if (!offset)
            return std::nullopt;
        std::optional<std::string> const text = internals.to_utf8(args[1]);
        if (!text)
            return std::nullopt;
        if (!std::isfinite(*offset))
            return interp.throw_type_error("Failed to execute 'addColorStop' on 'CanvasGradient': The provided double value is non-finite.");
        if (*offset < 0 || *offset > 1)
            return internals.throw_dom_exception("IndexSizeError", "The provided value (" + number_text(*offset) + ") is outside the range (0.0, 1.0).");
        std::optional<Color> const color = parse_canvas_color(internals, *text, nullptr);
        if (!color)
            return internals.throw_dom_exception("SyntaxError", "The value provided ('" + *text + "') could not be parsed as a color.");
        self->gradient->add_stop(*offset, *color);
        return js::Value::undefined();
    });

    js::Object* pattern = define_interface(in, "CanvasPattern", nullptr);
    define_operation(interpreter, *pattern, "setTransform", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* self = object_as<PatternObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        std::optional<Matrix2D> const m = matrix_2d_from_init(internals_of(interp), js::argument(args, 0));
        if (!m)
            return std::nullopt;
        canvas::Transform const t = transform_of(*m);
        if (t.is_finite())
            self->pattern->transform = t;
        return js::Value::undefined();
    });

    js::Object* path = define_interface(in, "Path2D", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Value const init = js::argument(args, 0);
            svg::Path initial;
            if (auto* other = object_as<Path2DObject>(init)) {
                initial = other->path.path;
            } else if (!init.is_undefined()) {
                std::optional<std::string> const text = internals.to_utf8(init);
                if (!text)
                    return std::nullopt;
                initial = svg::parse_path_data(*text);
            }
            auto* made = interp.heap().allocate<Path2DObject>(internals.prototype("Path2D"));
            made->path.path = std::move(initial);
            return js::Value::object(made);
        },
        0);
    define_operation(interpreter, *path, "addPath", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* self = object_as<Path2DObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        auto* other = object_as<Path2DObject>(js::argument(args, 0));
        if (!other)
            return interp.throw_type_error("Failed to execute 'addPath' on 'Path2D': parameter 1 is not of type 'Path2D'.");
        js::Value const init = js::argument(args, 1);
        if (!init.is_undefined() && !init.is_null() && !init.is_object())
            return interp.throw_type_error("Failed to execute 'addPath' on 'Path2D': parameter 2 is not of type 'DOMMatrix2DInit'.");
        std::optional<Matrix2D> const m = matrix_2d_from_init(internals_of(interp), init);
        if (!m)
            return std::nullopt;
        canvas::Transform const t = transform_of(*m);
        if (!t.is_finite())
            return js::Value::undefined();
        svg::Path const copy = other->path.path;
        self->path.add_path(copy, t);
        return js::Value::undefined();
    });
    install_path_methods(in, *path, false);

    js::Object* metrics = define_interface(in, "TextMetrics", nullptr);
    struct MetricMember {
        char const* name;
        double canvas::TextMeasure::*field;
    };
    for (MetricMember const member : {
             MetricMember { "width", &canvas::TextMeasure::width },
             MetricMember { "actualBoundingBoxLeft", &canvas::TextMeasure::actual_left },
             MetricMember { "actualBoundingBoxRight", &canvas::TextMeasure::actual_right },
             MetricMember { "fontBoundingBoxAscent", &canvas::TextMeasure::font_ascent },
             MetricMember { "fontBoundingBoxDescent", &canvas::TextMeasure::font_descent },
             MetricMember { "actualBoundingBoxAscent", &canvas::TextMeasure::actual_ascent },
             MetricMember { "actualBoundingBoxDescent", &canvas::TextMeasure::actual_descent },
             MetricMember { "emHeightAscent", &canvas::TextMeasure::em_ascent },
             MetricMember { "emHeightDescent", &canvas::TextMeasure::em_descent },
             MetricMember { "hangingBaseline", &canvas::TextMeasure::hanging_baseline },
             MetricMember { "alphabeticBaseline", &canvas::TextMeasure::alphabetic_baseline },
             MetricMember { "ideographicBaseline", &canvas::TextMeasure::ideographic_baseline },
         }) {
        auto const field = member.field;
        define_getter(in, *metrics, member.name, [field](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            auto* self = object_as<TextMetricsObject>(this_value);
            if (!self)
                return interp.throw_type_error("Illegal invocation");
            return js::Value::number(self->measure.*field);
        });
    }
}

void install_image_data(Realm::Internals& in)
{
    js::Object* image_data = define_interface(in, "ImageData", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.size() < 2)
                return not_enough(interp, "ImageData", "ImageData", 2, args.size());
            js::Value const first = args[0];
            // The overloads are told apart by the first argument: a
            // Uint8ClampedArray is the data; anything else, another typed
            // array included, is converted to the width (WebIDL's overload
            // resolution takes the numeric one).
            if (first.is_object() && first.as_object()->class_id() == js::Object::Class::TypedArray
                && static_cast<js::TypedArrayObject*>(first.as_object())->element_type() == js::ElementType::Uint8Clamped) {
                auto* array = static_cast<js::TypedArrayObject*>(first.as_object());
                // unsigned long, as WebIDL converts one: no range enforced.
                std::optional<double> const wn = interp.to_number(args[1]);
                if (!wn)
                    return std::nullopt;
                std::optional<std::int64_t> const w = to_unsigned_long(*wn);
                std::optional<std::int64_t> h;
                if (args.size() > 2 && !args[2].is_undefined()) {
                    std::optional<double> const hn = interp.to_number(args[2]);
                    if (!hn)
                        return std::nullopt;
                    h = to_unsigned_long(*hn);
                }
                if (!read_image_data_settings(internals, js::argument(args, 3)))
                    return std::nullopt;
                std::size_t const length = array->length();
                if (length == 0)
                    return internals.throw_dom_exception("InvalidStateError", "The input data has zero elements.");
                if (length % 4 != 0)
                    return internals.throw_dom_exception("InvalidStateError", "The input data length is not a multiple of 4.");
                if (*w <= 0 || *w > 2147483647)
                    return internals.throw_dom_exception("IndexSizeError", "The source width is zero or not a number.");
                std::size_t const pixels = length / 4;
                if (pixels % static_cast<std::size_t>(*w) != 0)
                    return internals.throw_dom_exception("IndexSizeError", "The input data length is not a multiple of (4 * width).");
                std::size_t const rows = pixels / static_cast<std::size_t>(*w);
                if (h && (*h <= 0 || static_cast<std::size_t>(*h) != rows))
                    return internals.throw_dom_exception("IndexSizeError", "The input data length is not equal to (4 * width * height).");
                std::optional<ImageDataObject*> const made = new_image_data(internals, static_cast<int>(*w), static_cast<int>(rows), first);
                if (!made)
                    return std::nullopt;
                return js::Value::object(*made);
            }
            // unsigned long, as WebIDL converts one: no range enforced.
            std::optional<double> const wn = interp.to_number(args[0]);
            if (!wn)
                return std::nullopt;
            std::optional<double> const hn = interp.to_number(args[1]);
            if (!hn)
                return std::nullopt;
            std::uint32_t const w = to_unsigned_long(*wn);
            std::uint32_t const h = to_unsigned_long(*hn);
            if (!read_image_data_settings(internals, js::argument(args, 2)))
                return std::nullopt;
            if (w == 0 || h == 0)
                return internals.throw_dom_exception("IndexSizeError", "The source width or height is zero.");
            if (w > 2147483647u || h > 2147483647u || static_cast<double>(w) * h > static_cast<double>(max_canvas_pixels))
                return internals.throw_dom_exception("IndexSizeError", "The ImageData is too large.");
            std::optional<ImageDataObject*> const made = new_image_data(internals, static_cast<int>(w), static_cast<int>(h));
            if (!made)
                return std::nullopt;
            return js::Value::object(*made);
        },
        2);
    define_getter(in, *image_data, "width", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* self = object_as<ImageDataObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        return js::Value::number(self->width);
    });
    define_getter(in, *image_data, "height", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* self = object_as<ImageDataObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        return js::Value::number(self->height);
    });
    define_getter(in, *image_data, "data", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* self = object_as<ImageDataObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        return self->data;
    });
    define_getter(in, *image_data, "colorSpace", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!object_as<ImageDataObject>(this_value))
            return interp.throw_type_error("Illegal invocation");
        return internals_of(interp).string("srgb");
    });
    define_getter(in, *image_data, "pixelFormat", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!object_as<ImageDataObject>(this_value))
            return interp.throw_type_error("Illegal invocation");
        return internals_of(interp).string("rgba-unorm8");
    });
}

js::Value new_image_bitmap(Realm::Internals& in, std::shared_ptr<Surface const> surface, bool clean)
{
    auto* bitmap = in.interpreter.heap().allocate<ImageBitmapObject>(in.prototype("ImageBitmap"));
    bitmap->width = surface->width;
    bitmap->height = surface->height;
    bitmap->surface = std::move(surface);
    bitmap->origin_clean = clean;
    return js::Value::object(bitmap);
}

// createImageBitmap's crop and resize (HTML section8.10, "cropped to the source
// rectangle with formatting").
std::optional<Surface> cropped(Realm::Internals& in, Surface const& source, Args args, std::size_t options_at, bool& ok)
{
    js::Interpreter& interp = in.interpreter;
    ok = false;
    int sx = 0, sy = 0, sw = source.width, sh = source.height;
    if (args.size() >= 5) {
        int v[4];
        for (std::size_t i = 0; i < 4; ++i) {
            std::optional<int> const n = enforce_long(interp, args[1 + i]);
            if (!n)
                return std::nullopt;
            v[i] = *n;
        }
        if (v[2] == 0 || v[3] == 0) {
            interp.throw_range_error("The crop rect width and height must not be zero.");
            return std::nullopt;
        }
        sx = v[0];
        sy = v[1];
        sw = v[2];
        sh = v[3];
        if (sw < 0) {
            sx += sw;
            sw = -sw;
        }
        if (sh < 0) {
            sy += sh;
            sh = -sh;
        }
    }
    int resize_w = 0;
    int resize_h = 0;
    bool flip = false;
    bool smooth = true;
    js::Value const options = js::argument(args, options_at);
    if (options.is_object()) {
        auto const read_size = [&](std::string_view name, int& out) -> bool {
            std::optional<js::Value> const got = interp.get(*options.as_object(), interp.key(name));
            if (!got)
                return false;
            if (got->is_undefined())
                return true;
            std::optional<double> const n = interp.to_number(*got);
            if (!n)
                return false;
            if (!std::isfinite(*n) || *n < 0 || *n > 4294967295.0) {
                interp.throw_type_error("The resize size is out of range.");
                return false;
            }
            out = static_cast<int>(std::min(*n, 1e6));
            if (out == 0) {
                in.throw_dom_exception("InvalidStateError", "The resize size must not be zero.");
                return false;
            }
            return true;
        };
        std::optional<js::Value> const orientation = interp.get(*options.as_object(), interp.key("imageOrientation"));
        if (!orientation)
            return std::nullopt;
        if (!orientation->is_undefined()) {
            std::optional<std::string> const text = in.to_utf8(*orientation);
            if (!text)
                return std::nullopt;
            flip = *text == "flipY";
        }
        std::optional<js::Value> const quality = interp.get(*options.as_object(), interp.key("resizeQuality"));
        if (!quality)
            return std::nullopt;
        if (!quality->is_undefined()) {
            std::optional<std::string> const text = in.to_utf8(*quality);
            if (!text)
                return std::nullopt;
            smooth = *text != "pixelated";
        }
        if (!read_size("resizeHeight", resize_h) || !read_size("resizeWidth", resize_w))
            return std::nullopt;
    }
    double const area_out = resize_w || resize_h ? static_cast<double>(std::max(resize_w, 1)) * std::max(resize_h, 1) : 0;
    if (static_cast<double>(sw) * sh > static_cast<double>(max_canvas_pixels) || area_out > static_cast<double>(max_canvas_pixels)) {
        in.throw_dom_exception("InvalidStateError", "The ImageBitmap would be too large.");
        return std::nullopt;
    }
    Surface out = source.crop(sx, sy, sw, sh);
    if (resize_w || resize_h) {
        int const w = resize_w ? resize_w : std::max(1, static_cast<int>(std::lround(static_cast<double>(sw) * resize_h / sh)));
        int const h = resize_h ? resize_h : std::max(1, static_cast<int>(std::lround(static_cast<double>(sh) * resize_w / sw)));
        Surface resized(w, h);
        canvas::DrawState state;
        state.smoothing = smooth;
        state.composite = canvas::Composite::Copy;
        canvas::draw_image(resized, out, 0, 0, out.width, out.height, 0, 0, w, h, state);
        out = std::move(resized);
    }
    if (flip) {
        for (int y = 0; y < out.height / 2; ++y) {
            for (int x = 0; x < out.width * 4; ++x)
                std::swap(out.pixels[out.offset(0, y) + static_cast<std::size_t>(x)], out.pixels[out.offset(0, out.height - 1 - y) + static_cast<std::size_t>(x)]);
        }
    }
    ok = true;
    return out;
}

void install_image_bitmap(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* bitmap = define_interface(in, "ImageBitmap", nullptr);
    define_getter(in, *bitmap, "width", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* self = object_as<ImageBitmapObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        return js::Value::number(self->surface ? self->width : 0);
    });
    define_getter(in, *bitmap, "height", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* self = object_as<ImageBitmapObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        return js::Value::number(self->surface ? self->height : 0);
    });
    define_operation(interpreter, *bitmap, "close", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* self = object_as<ImageBitmapObject>(this_value);
        if (!self)
            return interp.throw_type_error("Illegal invocation");
        self->surface.reset();
        return js::Value::undefined();
    });

    define_operation(interpreter, *interpreter.global(), "createImageBitmap", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        if (args.empty())
            return not_enough(interp, "createImageBitmap", "Window", 1, 0);
        if (args.size() >= 2 && args.size() < 5 && args.size() != 2)
            return interp.throw_type_error("Failed to execute 'createImageBitmap' on 'Window': Valid arities are: [1, 2, 5, 6].");
        std::size_t const options_at = args.size() >= 5 ? 5 : 1;
        js::Value const source = args[0];
        std::shared_ptr<Surface const> picture;
        bool clean = true;
        auto const reject = [&](std::string_view name, std::string_view message) -> Native {
            return rejected_promise(interp, dom_exception_value(internals, name, message));
        };
        if (auto* blob = object_as<BlobObject>(source)) {
            std::optional<Bitmap> decoded = decode_picture(blob->bytes);
            if (!decoded)
                return reject("InvalidStateError", "The source image could not be decoded.");
            picture = std::make_shared<Surface const>(Surface::from_bitmap(*decoded));
        } else if (auto* image = object_as<ImageDataObject>(source)) {
            std::size_t length = 0;
            std::uint8_t const* data = image_data_bytes(*image, length);
            if (!data)
                return internals.throw_dom_exception("InvalidStateError", "The ImageData's buffer has been detached.");
            Bitmap straight(image->width, image->height, Color { 0, 0, 0, 0 });
            std::copy_n(data, std::min(length, straight.writable_pixels().size()), straight.writable_pixels().begin());
            picture = std::make_shared<Surface const>(Surface::from_bitmap(straight));
        } else {
            SourcePicture found;
            if (!source_picture(internals, source, found, "createImageBitmap"))
                return std::nullopt;
            if (!found.usable || !found.surface)
                return reject("InvalidStateError", "The source image is not decoded yet.");
            picture = found.surface;
            clean = found.origin_clean;
        }
        if (picture->width == 0 || picture->height == 0)
            return internals.throw_dom_exception("InvalidStateError", "The source image has no pixels.");
        bool ok = false;
        std::optional<Surface> result = cropped(internals, *picture, args, options_at, ok);
        if (!result)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        js::Value const made = new_image_bitmap(internals, std::make_shared<Surface const>(std::move(*result)), clean);
        interp.root(made);
        return resolved_promise(interp, made);
    });
}

// --- The canvas element ------------------------------------------------------------------

Native get_context(Realm::Internals& in, dom::Element& element, Args args)
{
    js::Interpreter& interp = in.interpreter;
    if (args.empty())
        return not_enough(interp, "getContext", "HTMLCanvasElement", 1, 0);
    std::optional<std::string> const id = in.to_utf8(args[0]);
    if (!id)
        return std::nullopt;
    CanvasStateObject& state = state_of(in, element);
    if (*id != "2d") {
        // Only the 2D context is made here: a canvas that has one answers
        // null for any other, and an id this engine makes no context for
        // leaves the canvas without one, free to make a 2D one later.
        return js::Value::null();
    }
    if (state.context)
        return js::Value::object(state.context);
    // The settings dictionary's members, read in order.
    bool alpha = true;
    bool will_read = false;
    bool desynchronized = false;
    js::Value const options = js::argument(args, 1);
    if (options.is_object()) {
        for (auto const& [name, out] : { std::pair<char const*, bool*> { "alpha", &alpha }, std::pair<char const*, bool*> { "desynchronized", &desynchronized },
                 std::pair<char const*, bool*> { "willReadFrequently", &will_read } }) {
            std::optional<js::Value> const got = interp.get(*options.as_object(), interp.key(name));
            if (!got)
                return std::nullopt;
            if (!got->is_undefined())
                *out = js::Interpreter::to_boolean(*got);
        }
        std::optional<js::Value> const space = interp.get(*options.as_object(), interp.key("colorSpace"));
        if (!space)
            return std::nullopt;
        if (!space->is_undefined()) {
            std::optional<std::string> const text = in.to_utf8(*space);
            if (!text)
                return std::nullopt;
            if (*text != "srgb" && *text != "display-p3")
                return interp.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type PredefinedColorSpace.");
        }
    }
    ContextObject* made = nullptr;
    {
        js::Heap::NoCollect const no_collect(interp.heap());
        made = interp.heap().allocate<ContextObject>(in.prototype("CanvasRenderingContext2D"), state);
        state.context = made;
    }
    made->alpha = alpha;
    made->will_read_frequently = will_read;
    made->desynchronized = desynchronized;
    state.opaque = !alpha;
    surface_of(state);
    touched(in, state);
    return js::Value::object(made);
}

Native to_data_url(Realm::Internals& in, dom::Element& element, Args)
{
    CanvasStateObject& state = state_of(in, element);
    if (!state.origin_clean)
        return in.throw_dom_exception("SecurityError", "Tainted canvases may not be exported.");
    Surface const& surface = surface_of(state);
    if (surface.width == 0 || surface.height == 0)
        return in.string("data:,");
    // PNG whatever was asked: the engine encodes no other type, which the
    // specification allows.
    std::vector<std::uint8_t> const png = encode_canvas(surface);
    return in.string("data:image/png;base64," + base64_encode(png));
}

Native to_blob(Realm::Internals& in, dom::Element& element, Args args)
{
    js::Interpreter& interp = in.interpreter;
    if (args.empty())
        return not_enough(interp, "toBlob", "HTMLCanvasElement", 1, 0);
    if (!js::Interpreter::is_callable(args[0]))
        return interp.throw_type_error("Failed to execute 'toBlob' on 'HTMLCanvasElement': parameter 1 is not of type 'BlobCallback'.");
    CanvasStateObject& state = state_of(in, element);
    if (!state.origin_clean)
        return in.throw_dom_exception("SecurityError", "Tainted canvases may not be exported.");
    Surface const& surface = surface_of(state);
    std::optional<std::vector<std::uint8_t>> png;
    if (surface.width > 0 && surface.height > 0)
        png = encode_canvas(surface);
    auto callback = std::make_shared<js::Persistent>(interp.heap(), args[0]);
    in.post_task([&in, callback, png = std::move(png)]() mutable {
        Realm::Internals::Entry const entry(in);
        js::Interpreter::Roots const roots(in.interpreter);
        js::Value blob = js::Value::null();
        if (png) {
            blob = js::Value::object(new_blob(in, std::move(*png), "image/png"));
            in.interpreter.root(blob);
        }
        js::Value const arguments[] = { blob };
        in.call_reporting(callback->value(), js::Value::undefined(), arguments, "toBlob callback");
    });
    return js::Value::undefined();
}

} // namespace

void install_canvas(Realm::Internals& in)
{
    js::Object* canvas_proto = in.prototype("HTMLCanvasElement");
    if (canvas_proto) {
        js::Object& canvas = *canvas_proto;
        element_method(in, canvas, "getContext", 1, get_context);
        element_method(in, canvas, "toDataURL", 0, to_data_url);
        element_method(in, canvas, "toBlob", 1, to_blob);
    }
    install_context(in);
    install_gradient_pattern_path(in);
    install_image_data(in);
    if (in.worker == nullptr)
        install_image_bitmap(in);
}

void canvas_size_changed(Realm::Internals& in, dom::Element& element)
{
    NodeWrapper* wrapper = element.wrapper ? dynamic_cast<NodeWrapper*>(element.wrapper) : nullptr;
    if (!wrapper)
        return;
    auto* state = static_cast<CanvasStateObject*>(wrapper->same_object("canvas state"));
    if (!state)
        return;
    state->surface = make_surface(canvas_size(element));
    if (state->context)
        reset_context(*state->context);
    touched(in, *state);
}

std::vector<VideoFrame> canvas_frames(Realm::Internals& in)
{
    // The tree changed since the list was made: the canvases in it now,
    // any of them drawn on while out of it included.
    if (in.canvases_found_at != in.mutations) {
        in.canvases_found_at = in.mutations;
        for (js::Object* object : in.canvases)
            static_cast<CanvasStateObject*>(object)->listed = false;
        in.canvases.clear();
        auto const visit = [&](auto const& self, dom::Node& node) -> void {
            if (node.is_element()) {
                auto& element = static_cast<dom::Element&>(node);
                if (element.is_html("canvas") && element.wrapper) {
                    if (auto* wrapper = dynamic_cast<NodeWrapper*>(element.wrapper)) {
                        if (auto* state = static_cast<CanvasStateObject*>(wrapper->same_object("canvas state")); state && state->surface) {
                            state->listed = true;
                            in.canvases.push_back(state);
                        }
                    }
                }
            }
            for (dom::Node* child : node.children())
                self(self, *child);
        };
        visit(visit, *in.document);
    }
    std::vector<VideoFrame> frames;
    for (js::Object* object : in.canvases) {
        auto& state = *static_cast<CanvasStateObject*>(object);
        dom::Element* element = canvas_element(state);
        if (!element || &element->root() != in.document || !state.surface)
            continue;
        Surface const& surface = *state.surface;
        if (state.presented_at != state.painted) {
            state.presented_at = state.painted;
            if (surface.width == 0 || surface.height == 0) {
                state.presented.reset();
            } else if (state.presented && state.presented->width() == surface.width && state.presented->height() == surface.height) {
                state.presented->writable_pixels() = surface.to_bitmap().pixels();
                ++state.frames;
            } else {
                state.presented = std::make_shared<Bitmap>(surface.to_bitmap());
                ++state.shape;
            }
        }
        if (state.presented)
            frames.push_back({ element, state.presented, state.shape, state.frames });
    }
    return frames;
}

void trace_canvases(Realm::Internals const& in, js::Tracer& tracer)
{
    for (js::Object* const state : in.canvases)
        tracer.visit(state);
}

void image_source_changed(Realm::Internals& in, dom::Element& image)
{
    // An image in the document of a host that fetches pictures is the
    // host's to load, and to tell the realm of (Realm::image_settled).
    if (in.hooks.image_state && &image.root() == in.document)
        return;
    // Only this realm's own document's images: one of a frame's document,
    // written to from here, is the frame's.
    if (&image.document() != in.document)
        return;
    dom::Attr const* const src = image.find_attribute("src");
    if (!src || src->value.empty())
        return;
    std::optional<net::Url> const url = net::parse_url(src->value, &in.base_url());
    std::string const key = url ? url->serialize() : std::string();
    NodeWrapper& wrapper = wrapper_for(in, image);
    auto* picture = static_cast<PictureObject*>(wrapper.same_object("canvas picture"));
    {
        js::Heap::NoCollect const no_collect(in.interpreter.heap());
        if (!picture) {
            picture = in.interpreter.heap().allocate<PictureObject>();
            wrapper.keep_same_object("canvas picture", picture);
        }
    }
    picture->pending = key;
    // HTML section4.8.4.3.4: the picture is fetched and its event fired in a task,
    // never inside the script that set the source; the element's wrapper is
    // held until then. A source set again meanwhile queues its own task, and
    // this one does nothing.
    auto held = std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(&wrapper));
    in.post_task([&in, held, key, aborts = in.document_aborts] {
        Realm::Internals::Entry const entry(in);
        js::Interpreter::Roots const roots(in.interpreter);
        NodeWrapper* const target = in.wrapper_of(held->value());
        if (!target || !target->node().is_element() || in.document_aborts != aborts)
            return;
        auto& element = static_cast<dom::Element&>(target->node());
        auto* kept = static_cast<PictureObject*>(target->same_object("canvas picture"));
        if (!kept || kept->pending != key)
            return;
        bool failed = true;
        if (std::optional<net::Url> const source = net::parse_url(key)) {
            kept->pending.clear();
            PictureObject& had = fetched_picture(in, element, *source);
            had.settled = true;
            failed = had.broken;
        }
        kept->pending.clear();
        kept->settled = true;
        EventObject* event = in.new_event("Event", failed ? "error" : "load", false, false);
        in.interpreter.root(js::Value::object(event));
        event->is_trusted = true;
        in.dispatch(*event, held->value().as_object());
    });
}

std::optional<ImageDataCopy> image_data_copy(js::Object const& object)
{
    auto const* image = dynamic_cast<ImageDataObject const*>(&object);
    if (!image)
        return std::nullopt;
    ImageDataCopy copy;
    copy.width = image->width;
    copy.height = image->height;
    std::size_t length = 0;
    if (std::uint8_t const* bytes = image_data_bytes(*image, length))
        copy.bytes.assign(bytes, bytes + length);
    return copy;
}

std::optional<js::Value> image_data_from_copy(Realm::Internals& in, ImageDataCopy const& copy)
{
    std::optional<ImageDataObject*> const made = new_image_data(in, copy.width, copy.height);
    if (!made)
        return std::nullopt;
    std::size_t length = 0;
    if (std::uint8_t* bytes = image_data_bytes(**made, length))
        std::copy_n(copy.bytes.begin(), std::min(length, copy.bytes.size()), bytes);
    return js::Value::object(*made);
}

}
