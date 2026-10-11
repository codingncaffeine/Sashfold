#include "bindings/Internal.h"

// The SVG DOM (SVG 2 §4, §5, §8, §9, §10, §13, §14, §15, Filter Effects §9):
// an interface for every SVG element as the browsers have them —
// SVGGraphicsElement and SVGGeometryElement under SVGElement, the shapes,
// SVGSVGElement, text, the paint servers, clipping, masking, filters and
// animation — with the animated attributes a script reads geometry through
// (SVGAnimatedLength over SVGLength, SVGAnimatedNumber, SVGAnimatedString,
// SVGAnimatedEnumeration, SVGAnimatedRect, SVGAnimatedTransformList over
// SVGTransformList and SVGTransform, SVGAnimatedPreserveAspectRatio, the
// point list) and the geometry methods: getBBox, getCTM, getScreenCTM,
// getTotalLength, getPointAtLength, isPointInFill, isPointInStroke,
// getComputedTextLength. The answers come from svg::'s geometry, the walk
// the painter draws by, at the size layout gave the outermost <svg>.
//
// No animation runs (SMIL is not written): every animVal reads what its
// baseVal reads, and writing one is NoModificationAllowedError. SVGPoint,
// SVGMatrix and SVGRect are DOMPoint, DOMMatrix and DOMRect under their
// legacy window names (Geometry.cpp, Node.cpp), as Firefox has them.

#include "bindings/NodeSupport.h"
#include "core/Ascii.h"
#include "dom/Dom.h"
#include "js/Object.h"
#include "js/Strings.h"
#include "svg/Svg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// ---- which interface an element has

struct TagInterface {
    std::string_view tag;
    std::string_view interface;
};

constexpr TagInterface svg_tags[] = {
    { "a", "SVGAElement" },
    { "animate", "SVGAnimateElement" },
    { "animateMotion", "SVGAnimateMotionElement" },
    { "animateTransform", "SVGAnimateTransformElement" },
    { "circle", "SVGCircleElement" },
    { "clipPath", "SVGClipPathElement" },
    { "defs", "SVGDefsElement" },
    { "desc", "SVGDescElement" },
    { "ellipse", "SVGEllipseElement" },
    { "feBlend", "SVGFEBlendElement" },
    { "feColorMatrix", "SVGFEColorMatrixElement" },
    { "feComponentTransfer", "SVGFEComponentTransferElement" },
    { "feComposite", "SVGFECompositeElement" },
    { "feConvolveMatrix", "SVGFEConvolveMatrixElement" },
    { "feDiffuseLighting", "SVGFEDiffuseLightingElement" },
    { "feDisplacementMap", "SVGFEDisplacementMapElement" },
    { "feDistantLight", "SVGFEDistantLightElement" },
    { "feDropShadow", "SVGFEDropShadowElement" },
    { "feFlood", "SVGFEFloodElement" },
    { "feFuncA", "SVGFEFuncAElement" },
    { "feFuncB", "SVGFEFuncBElement" },
    { "feFuncG", "SVGFEFuncGElement" },
    { "feFuncR", "SVGFEFuncRElement" },
    { "feGaussianBlur", "SVGFEGaussianBlurElement" },
    { "feImage", "SVGFEImageElement" },
    { "feMerge", "SVGFEMergeElement" },
    { "feMergeNode", "SVGFEMergeNodeElement" },
    { "feMorphology", "SVGFEMorphologyElement" },
    { "feOffset", "SVGFEOffsetElement" },
    { "fePointLight", "SVGFEPointLightElement" },
    { "feSpecularLighting", "SVGFESpecularLightingElement" },
    { "feSpotLight", "SVGFESpotLightElement" },
    { "feTile", "SVGFETileElement" },
    { "feTurbulence", "SVGFETurbulenceElement" },
    { "filter", "SVGFilterElement" },
    { "foreignObject", "SVGForeignObjectElement" },
    { "g", "SVGGElement" },
    { "image", "SVGImageElement" },
    { "line", "SVGLineElement" },
    { "linearGradient", "SVGLinearGradientElement" },
    { "marker", "SVGMarkerElement" },
    { "mask", "SVGMaskElement" },
    { "metadata", "SVGMetadataElement" },
    { "mpath", "SVGMPathElement" },
    { "path", "SVGPathElement" },
    { "pattern", "SVGPatternElement" },
    { "polygon", "SVGPolygonElement" },
    { "polyline", "SVGPolylineElement" },
    { "radialGradient", "SVGRadialGradientElement" },
    { "rect", "SVGRectElement" },
    { "script", "SVGScriptElement" },
    { "set", "SVGSetElement" },
    { "stop", "SVGStopElement" },
    { "style", "SVGStyleElement" },
    { "svg", "SVGSVGElement" },
    { "switch", "SVGSwitchElement" },
    { "symbol", "SVGSymbolElement" },
    { "text", "SVGTextElement" },
    { "textPath", "SVGTextPathElement" },
    { "title", "SVGTitleElement" },
    { "tspan", "SVGTSpanElement" },
    { "use", "SVGUseElement" },
    { "view", "SVGViewElement" },
};

// ---- lengths (§4.6.2)

namespace unit {
constexpr int unknown = 0;
constexpr int number = 1;
constexpr int percentage = 2;
constexpr int ems = 3;
constexpr int exs = 4;
constexpr int px = 5;
constexpr int cm = 6;
constexpr int mm = 7;
constexpr int in = 8;
constexpr int pt = 9;
constexpr int pc = 10;
}

struct Length {
    double number = 0;
    int unit = unit::number;
};

constexpr std::pair<std::string_view, int> unit_suffixes[] = { { "%", unit::percentage }, { "em", unit::ems }, { "ex", unit::exs },
    { "px", unit::px }, { "cm", unit::cm }, { "mm", unit::mm }, { "in", unit::in }, { "pt", unit::pt }, { "pc", unit::pc } };

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r' || text.front() == '\f'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r' || text.back() == '\f'))
        text.remove_suffix(1);
    return text;
}

// A number as SVG writes one: sign, digits, a fraction, an exponent.
std::optional<double> parse_number_prefix(std::string_view text, std::size_t& used)
{
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
        ++i;
    std::size_t digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        ++i;
        ++digits;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
            ++digits;
        }
    }
    if (digits == 0)
        return std::nullopt;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-'))
            ++j;
        std::size_t exponent_digits = 0;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
            ++j;
            ++exponent_digits;
        }
        if (exponent_digits > 0)
            i = j;
    }
    used = i;
    std::string const copy(text.substr(0, i));
    return std::strtod(copy.c_str(), nullptr);
}

std::optional<Length> parse_length(std::string_view text)
{
    text = trim(text);
    std::size_t used = 0;
    std::optional<double> const number = parse_number_prefix(text, used);
    if (!number)
        return std::nullopt;
    std::string_view const rest = text.substr(used);
    if (rest.empty())
        return Length { *number, unit::number };
    for (auto const& [suffix, kind] : unit_suffixes) {
        if (rest.size() == suffix.size() && ascii_ci_equals(rest, suffix))
            return Length { *number, kind };
    }
    return std::nullopt;
}

std::string number_text(double value)
{
    std::u16string const text = js::number_to_string(value);
    return std::string(text.begin(), text.end());
}

std::string serialize(Length const& length)
{
    std::string out = number_text(length.number);
    for (auto const& [suffix, kind] : unit_suffixes) {
        if (kind == length.unit)
            out += suffix;
    }
    return out;
}

// Which viewport dimension a length attribute's percentage is of.
enum class Direction : std::uint8_t { Horizontal, Vertical, Diagonal };

Direction direction_of(std::string_view attribute)
{
    if (attribute == "y" || attribute == "cy" || attribute == "height" || attribute == "y1" || attribute == "y2" || attribute == "ry"
        || attribute == "fy" || attribute == "refY" || attribute == "markerHeight")
        return Direction::Vertical;
    if (attribute == "r" || attribute == "fr" || attribute == "textLength" || attribute == "startOffset")
        return Direction::Diagonal;
    return Direction::Horizontal;
}

// The context a length's relative units need: the font size and the
// nearest viewport, from layout when the element is laid out.
struct LengthContext {
    double em = 16;
    double viewport_width = 0;
    double viewport_height = 0;
};

svg::StyleOf style_lookup(Realm::Internals& in)
{
    return [&in](dom::Element const& element) -> css::ComputedStyle const* {
        return in.hooks.computed_style ? in.hooks.computed_style(element) : nullptr;
    };
}

// Where an element sits: its outermost <svg>, that box on the page, and the
// element's user space in it.
struct Placement {
    dom::Element const* root = nullptr;
    LayoutBox box;
    svg::UserSpace space;
};

std::optional<Placement> place(Realm::Internals& in, dom::Element const& element)
{
    dom::Element const* const root = svg::outermost_svg(element);
    if (!root)
        return std::nullopt;
    Placement placement;
    placement.root = root;
    if (in.hooks.layout_box) {
        if (std::optional<LayoutBox> const box = in.hooks.layout_box(*root))
            placement.box = *box;
    }
    std::optional<svg::UserSpace> const space = svg::user_space(*root, element, placement.box.width, placement.box.height, style_lookup(in));
    if (!space)
        return std::nullopt;
    placement.space = *space;
    return placement;
}

LengthContext length_context(Realm::Internals& in, dom::Element const& element)
{
    LengthContext context;
    if (css::ComputedStyle const* const style = in.hooks.computed_style ? in.hooks.computed_style(element) : nullptr)
        context.em = style->font_size;
    // A percentage is of the viewport the element's own lengths are drawn
    // in: its parent's for an <svg>, which establishes the next one.
    dom::Element const* reference = &element;
    if (element.is_svg("svg") && element.parent() && element.parent()->is_element())
        reference = static_cast<dom::Element const*>(element.parent());
    if (std::optional<Placement> const placement = place(in, *reference)) {
        context.viewport_width = placement->space.viewport_width;
        context.viewport_height = placement->space.viewport_height;
    }
    return context;
}

double user_units(Length const& length, Direction direction, LengthContext const& context)
{
    switch (length.unit) {
    case unit::percentage: {
        double base = context.viewport_width;
        if (direction == Direction::Vertical)
            base = context.viewport_height;
        else if (direction == Direction::Diagonal)
            base = std::sqrt((context.viewport_width * context.viewport_width + context.viewport_height * context.viewport_height) / 2);
        return length.number * base / 100;
    }
    case unit::ems:
        return length.number * context.em;
    case unit::exs:
        return length.number * context.em / 2;
    case unit::cm:
        return length.number * 96 / 2.54;
    case unit::mm:
        return length.number * 96 / 25.4;
    case unit::in:
        return length.number * 96;
    case unit::pt:
        return length.number * 4 / 3;
    case unit::pc:
        return length.number * 16;
    default:
        return length.number;
    }
}

// ---- the value objects

// What an object over an attribute is.
enum class Kind : std::uint8_t {
    AnimatedLength,
    Length,
    AnimatedNumber,
    AnimatedString,
    AnimatedEnumeration,
    AnimatedRect,
    AnimatedTransformList,
    TransformList,
    Transform,
    AnimatedPreserveAspectRatio,
    PreserveAspectRatio,
    PointList,
    AnimatedBoolean,
    AnimatedInteger,
};

struct EnumTable {
    std::string_view attribute;
    std::span<std::string_view const> keywords; // index + 1 is the value; 0 is unknown
    int missing; // the value with the attribute absent
};

// An attribute of an element seen as one of the types above; or, with no
// element, a value of its own (createSVGLength, createSVGTransform).
class SvgValueObject final : public ElementBackedObject {
public:
    SvgValueObject(js::Object* prototype, NodeWrapper* the_wrapper, Kind the_kind, std::string the_attribute)
        : ElementBackedObject(prototype, the_wrapper)
        , kind(the_kind)
        , attribute(std::move(the_attribute))
    {
    }
    Kind kind;
    std::string attribute;
    bool read_only = false; // an animVal
    double fallback = 0; // a number's or a length's value with the attribute absent
    std::string fallback_text; // a length's, as written (an <svg>'s 100%)
    EnumTable const* table = nullptr;
    std::size_t index = 0; // a transform's place in its list
    // A detached value: a length, a number or a transform of its own.
    Length own_length;
    double own_number = 0;
    int own_type = 1; // a transform's
    svg::Matrix own_matrix;
    double own_angle = 0;
};

std::optional<SvgValueObject*> this_value_object(js::Interpreter& interp, js::Value const& this_value, Kind kind)
{
    if (this_value.is_object()) {
        if (auto* object = dynamic_cast<SvgValueObject*>(this_value.as_object()); object && object->kind == kind)
            return object;
    }
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

std::string attribute_text(dom::Element const& element, std::string_view name)
{
    dom::Attr const* const attr = element.find_attribute(name);
    return attr ? attr->value : std::string();
}

bool has_attribute(dom::Element const& element, std::string_view name)
{
    return element.find_attribute(name) != nullptr;
}

// The [SameObject] child of a value object or of an element: made once with
// the realm's interface, kept on the element's wrapper by key.
SvgValueObject* kept_value(Realm::Internals& in, NodeWrapper& wrapper, std::string const& key, std::string_view interface, Kind kind,
    std::string const& attribute)
{
    if (js::Object* const kept = wrapper.same_object(key))
        return static_cast<SvgValueObject*>(kept);
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&wrapper));
    auto* made = in.interpreter.heap().allocate<SvgValueObject>(wrapper.realm().internals().prototype(interface), &wrapper, kind, attribute);
    wrapper.keep_same_object(key, made);
    return made;
}

Native no_modification(js::Interpreter& interp)
{
    return internals_of(interp).throw_dom_exception("NoModificationAllowedError", "An animVal cannot be modified.");
}

// -- SVGLength

Length length_of(SvgValueObject const& object)
{
    if (dom::Element const* const element = object.element()) {
        if (std::optional<Length> const parsed = parse_length(attribute_text(*element, object.attribute)); parsed && has_attribute(*element, object.attribute))
            return *parsed;
        if (!object.fallback_text.empty())
            return parse_length(object.fallback_text).value_or(Length {});
        return Length { object.fallback, unit::number };
    }
    return object.own_length;
}

double length_value(Realm::Internals& in, SvgValueObject const& object)
{
    Length const length = length_of(object);
    dom::Element const* const element = object.element();
    if (!element)
        return user_units(length, Direction::Horizontal, LengthContext {});
    return user_units(length, direction_of(object.attribute), length_context(in, *element));
}

Native write_length(Realm::Internals& in, SvgValueObject& object, Length const& length)
{
    if (object.read_only)
        return no_modification(in.interpreter);
    if (dom::Element* const element = object.element())
        set_attribute(in, *element, object.attribute, serialize(length));
    else
        object.own_length = length;
    return js::Value::undefined();
}

// -- transforms (§8.5)

constexpr int transform_matrix = 1;
constexpr int transform_translate = 2;
constexpr int transform_scale = 3;
constexpr int transform_rotate = 4;
constexpr int transform_skew_x = 5;
constexpr int transform_skew_y = 6;

struct TransformItem {
    int type = transform_matrix;
    std::vector<double> values; // as written
};

constexpr double pi = 3.14159265358979323846;

svg::Matrix item_matrix(TransformItem const& item)
{
    auto const at = [&](std::size_t i, double fallback) { return i < item.values.size() ? item.values[i] : fallback; };
    switch (item.type) {
    case transform_translate:
        return svg::Matrix::translate(static_cast<float>(at(0, 0)), static_cast<float>(at(1, 0)));
    case transform_scale:
        return svg::Matrix::scale(static_cast<float>(at(0, 1)), static_cast<float>(at(1, at(0, 1))));
    case transform_rotate: {
        svg::Matrix const rotation = svg::Matrix::rotate(static_cast<float>(at(0, 0)));
        if (item.values.size() >= 3) {
            float const cx = static_cast<float>(item.values[1]);
            float const cy = static_cast<float>(item.values[2]);
            return svg::Matrix::translate(-cx, -cy).then(rotation).then(svg::Matrix::translate(cx, cy));
        }
        return rotation;
    }
    case transform_skew_x:
        return svg::Matrix::skew_x(static_cast<float>(at(0, 0)));
    case transform_skew_y:
        return svg::Matrix::skew_y(static_cast<float>(at(0, 0)));
    default:
        return svg::Matrix { static_cast<float>(at(0, 1)), static_cast<float>(at(1, 0)), static_cast<float>(at(2, 0)),
            static_cast<float>(at(3, 1)), static_cast<float>(at(4, 0)), static_cast<float>(at(5, 0)) };
    }
}

// The transform list microsyntax (§8.5): an empty list for anything wrong,
// as the browsers read an invalid attribute.
std::vector<TransformItem> parse_transform_items(std::string_view text)
{
    std::vector<TransformItem> items;
    std::size_t i = 0;
    auto const skip = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == ','))
            ++i;
    };
    constexpr std::pair<std::string_view, int> names[] = { { "matrix", transform_matrix }, { "translate", transform_translate },
        { "scale", transform_scale }, { "rotate", transform_rotate }, { "skewX", transform_skew_x }, { "skewY", transform_skew_y } };
    skip();
    while (i < text.size()) {
        int type = 0;
        for (auto const& [name, kind] : names) {
            if (text.substr(i, name.size()) == name) {
                type = kind;
                i += name.size();
                break;
            }
        }
        if (type == 0)
            return {};
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
            ++i;
        if (i >= text.size() || text[i] != '(')
            return {};
        ++i;
        TransformItem item;
        item.type = type;
        for (;;) {
            skip();
            if (i < text.size() && text[i] == ')') {
                ++i;
                break;
            }
            std::size_t used = 0;
            std::optional<double> const number = parse_number_prefix(text.substr(i), used);
            if (!number)
                return {};
            item.values.push_back(*number);
            i += used;
        }
        std::size_t const count = item.values.size();
        bool const valid = (type == transform_matrix && count == 6) || ((type == transform_translate || type == transform_scale) && (count == 1 || count == 2))
            || (type == transform_rotate && (count == 1 || count == 3)) || ((type == transform_skew_x || type == transform_skew_y) && count == 1);
        if (!valid)
            return {};
        items.push_back(std::move(item));
        skip();
    }
    return items;
}

std::string serialize_items(std::vector<TransformItem> const& items)
{
    constexpr std::string_view names[] = { "", "matrix", "translate", "scale", "rotate", "skewX", "skewY" };
    std::string out;
    for (TransformItem const& item : items) {
        if (!out.empty())
            out += ' ';
        out += names[item.type];
        out += '(';
        for (std::size_t k = 0; k < item.values.size(); ++k) {
            if (k > 0)
                out += ' ';
            out += number_text(item.values[k]);
        }
        out += ')';
    }
    return out;
}

TransformItem matrix_item(svg::Matrix const& m)
{
    return TransformItem { transform_matrix, { m.a, m.b, m.c, m.d, m.e, m.f } };
}

// A transform's own data: its type, matrix and angle.
struct TransformData {
    int type = transform_matrix;
    svg::Matrix matrix;
    double angle = 0;
};

TransformData data_of(TransformItem const& item)
{
    TransformData data;
    data.type = item.type;
    data.matrix = item_matrix(item);
    if (item.type == transform_rotate || item.type == transform_skew_x || item.type == transform_skew_y)
        data.angle = item.values.empty() ? 0 : item.values[0];
    return data;
}

std::optional<TransformData> transform_data(SvgValueObject const& object)
{
    if (dom::Element const* const element = object.element()) {
        std::vector<TransformItem> const items = parse_transform_items(attribute_text(*element, object.attribute));
        if (object.index >= items.size())
            return std::nullopt;
        return data_of(items[object.index]);
    }
    return TransformData { object.own_type, object.own_matrix, object.own_angle };
}

// Writes one item of a transform, in its list or of its own.
Native write_transform(Realm::Internals& in, SvgValueObject& object, TransformItem const& item)
{
    if (object.read_only)
        return no_modification(in.interpreter);
    if (dom::Element* const element = object.element()) {
        std::vector<TransformItem> items = parse_transform_items(attribute_text(*element, object.attribute));
        if (object.index < items.size()) {
            items[object.index] = item;
            set_attribute(in, *element, object.attribute, serialize_items(items));
        }
        return js::Value::undefined();
    }
    TransformData const data = data_of(item);
    object.own_type = data.type;
    object.own_matrix = data.matrix;
    object.own_angle = data.angle;
    return js::Value::undefined();
}

Matrix2D to_2d(svg::Matrix const& m)
{
    return Matrix2D { m.a, m.b, m.c, m.d, m.e, m.f };
}

svg::Matrix from_2d(Matrix2D const& m)
{
    return svg::Matrix { static_cast<float>(m.a), static_cast<float>(m.b), static_cast<float>(m.c), static_cast<float>(m.d),
        static_cast<float>(m.e), static_cast<float>(m.f) };
}

SvgValueObject* new_transform(Realm::Internals& in, NodeWrapper* wrapper, std::string const& attribute, std::size_t index, bool read_only)
{
    auto* made = in.interpreter.heap().allocate<SvgValueObject>(in.prototype("SVGTransform"), wrapper, Kind::Transform, attribute);
    made->index = index;
    made->read_only = read_only;
    return made;
}

// A transform object passed in (appendItem, initialize): its data.
std::optional<TransformItem> item_from(js::Interpreter& interp, js::Value const& value)
{
    if (value.is_object()) {
        if (auto const* object = dynamic_cast<SvgValueObject const*>(value.as_object()); object && object->kind == Kind::Transform) {
            if (std::optional<TransformData> const data = transform_data(*object))
                return matrix_item(data->matrix);
        }
    }
    interp.throw_type_error("The value is not of type 'SVGTransform'.");
    return std::nullopt;
}

// ---- enumerations

constexpr std::string_view unit_keywords[] = { "userSpaceOnUse", "objectBoundingBox" };
constexpr std::string_view spread_keywords[] = { "pad", "reflect", "repeat" };
constexpr std::string_view length_adjust_keywords[] = { "spacing", "spacingAndGlyphs" };
constexpr std::string_view marker_unit_keywords[] = { "userSpaceOnUse", "strokeWidth" };
constexpr std::string_view method_keywords[] = { "align", "stretch" };
constexpr std::string_view spacing_keywords[] = { "auto", "exact" };

EnumTable const units_user_space { "", unit_keywords, 1 };
EnumTable const units_bounding_box { "", unit_keywords, 2 };
EnumTable const spread_method { "spreadMethod", spread_keywords, 1 };
EnumTable const length_adjust { "lengthAdjust", length_adjust_keywords, 1 };
EnumTable const marker_units { "markerUnits", marker_unit_keywords, 2 };
EnumTable const text_path_method { "method", method_keywords, 1 };
EnumTable const text_path_spacing { "spacing", spacing_keywords, 2 };

int enumeration_value(SvgValueObject const& object)
{
    dom::Element const* const element = object.element();
    if (!element || !has_attribute(*element, object.attribute))
        return object.table->missing;
    std::string const text = attribute_text(*element, object.attribute);
    for (std::size_t i = 0; i < object.table->keywords.size(); ++i) {
        if (text == object.table->keywords[i])
            return static_cast<int>(i) + 1;
    }
    return object.table->missing;
}

// ---- preserveAspectRatio (§8.6)

constexpr std::string_view align_keywords[] = { "none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid",
    "xMinYMax", "xMidYMax", "xMaxYMax" };

std::pair<int, int> aspect_ratio_of(dom::Element const* element, std::string const& attribute)
{
    std::string const text = element ? std::string(trim(attribute_text(*element, attribute))) : std::string();
    int align = 6; // xMidYMid
    int meet_or_slice = 1; // meet
    std::size_t const space = text.find(' ');
    std::string_view const first = space == std::string::npos ? std::string_view(text) : std::string_view(text).substr(0, space);
    std::string_view const second = space == std::string::npos ? std::string_view() : trim(std::string_view(text).substr(space + 1));
    for (std::size_t i = 0; i < std::size(align_keywords); ++i) {
        if (first == align_keywords[i])
            align = static_cast<int>(i) + 1;
    }
    if (second == "slice")
        meet_or_slice = 2;
    return { align, meet_or_slice };
}

// ---- the geometry methods

Native bbox_of(Realm::Internals& in, dom::Element& element)
{
    std::optional<Placement> const placement = place(in, element);
    if (!placement)
        return make_rect(in, 0, 0, 0, 0);
    svg::Box const box = svg::bounding_box(element, placement->space, style_lookup(in));
    return make_rect(in, box.left, box.top, box.right - box.left, box.bottom - box.top);
}

std::optional<svg::Path> path_of(Realm::Internals& in, dom::Element const& element)
{
    std::optional<Placement> const placement = place(in, element);
    svg::UserSpace space;
    if (placement)
        space = placement->space;
    return svg::geometry_of(element, space, style_lookup(in));
}

std::optional<std::pair<double, double>> scroll_of(Realm::Internals& in)
{
    if (!in.hooks.scroll_position)
        return std::pair { 0.0, 0.0 };
    std::pair<int, int> const scroll = in.hooks.scroll_position(*in.document);
    return std::pair { static_cast<double>(scroll.first), static_cast<double>(scroll.second) };
}

// getScreenCTM: the CTM, then the outermost <svg>'s place in the viewport.
std::optional<svg::Matrix> screen_ctm(Realm::Internals& in, dom::Element const& element)
{
    std::optional<Placement> const placement = place(in, element);
    if (!placement)
        return std::nullopt;
    std::pair<double, double> const scroll = *scroll_of(in);
    return placement->space.ctm.then(svg::Matrix::translate(static_cast<float>(static_cast<double>(placement->box.x) - scroll.first),
        static_cast<float>(static_cast<double>(placement->box.y) - scroll.second)));
}

// The text an element holds, its descendants' in order, and its length in
// UTF-16 code units, which is what a character count means here.
void gather_text(dom::Node const& node, std::string& out)
{
    for (dom::Node const* child : node.children()) {
        if (child->is_text())
            out += static_cast<dom::Text const&>(*child).data;
        else if (child->is_element())
            gather_text(*child, out);
    }
}

std::size_t utf16_length(std::string const& utf8)
{
    std::size_t units = 0;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        unsigned char const c = static_cast<unsigned char>(utf8[i]);
        if ((c & 0xC0) == 0x80)
            continue;
        units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

// ---- installing

js::Object* interface(Realm::Internals& in, std::string_view name, std::string_view parent)
{
    return define_interface(in, name, in.prototype(parent));
}

// An SVGAnimated* attribute of an element: the one object for it, kept.
template<Kind kind>
void animated_attribute(Realm::Internals& in, js::Object& prototype, std::string_view name, std::string_view attribute,
    std::string_view interface_name, double fallback = 0, std::string_view fallback_text = {}, EnumTable const* table = nullptr)
{
    std::string const key = "svg:" + std::string(name);
    std::string const attribute_name(attribute);
    std::string const interface_string(interface_name);
    std::string const fallback_string(fallback_text);
    element_getter(in, prototype, name, [key, attribute_name, interface_string, fallback, fallback_string, table](Realm::Internals& internals, dom::Element& e) -> Native {
        NodeWrapper& wrapper = wrapper_for(internals, e);
        SvgValueObject* const object = kept_value(internals, wrapper, key, interface_string, kind, attribute_name);
        object->fallback = fallback;
        object->fallback_text = fallback_string;
        object->table = table;
        return js::Value::object(object);
    });
}

void lengths(Realm::Internals& in, js::Object& prototype, std::initializer_list<std::string_view> names)
{
    for (std::string_view const name : names)
        animated_attribute<Kind::AnimatedLength>(in, prototype, name, name, "SVGAnimatedLength");
}

// The baseVal and animVal of an animated value: the child object kept on
// the element's wrapper, read-only for animVal.
void animated_pair(Realm::Internals& in, js::Object& prototype, Kind animated, Kind child, std::string_view child_interface)
{
    for (bool const animation : { false, true }) {
        std::string const child_name(child_interface);
        define_getter(in, prototype, animation ? "animVal" : "baseVal",
            [animated, child, child_name, animation](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, animated);
                if (!self)
                    return std::nullopt;
                if (!(*self)->wrapper)
                    return js::Value::null();
                Realm::Internals& internals = internals_of(interp);
                std::string const key = "svg:" + (*self)->attribute + (animation ? ":anim" : ":base") + (child == Kind::TransformList ? ":list" : "");
                SvgValueObject* const made = kept_value(internals, *(*self)->wrapper, key, child_name, child, (*self)->attribute);
                made->read_only = animation;
                made->fallback = (*self)->fallback;
                made->fallback_text = (*self)->fallback_text;
                return js::Value::object(made);
            });
    }
}

void install_types(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* global = interpreter.global();

    // SVGLength (§4.6.2).
    js::Object* length = define_interface(in, "SVGLength", nullptr);
    js::Value const length_constructor = *interpreter.get(*global, interpreter.key("SVGLength"));
    constexpr std::pair<char const*, int> length_types[] = { { "SVG_LENGTHTYPE_UNKNOWN", 0 }, { "SVG_LENGTHTYPE_NUMBER", 1 },
        { "SVG_LENGTHTYPE_PERCENTAGE", 2 }, { "SVG_LENGTHTYPE_EMS", 3 }, { "SVG_LENGTHTYPE_EXS", 4 }, { "SVG_LENGTHTYPE_PX", 5 },
        { "SVG_LENGTHTYPE_CM", 6 }, { "SVG_LENGTHTYPE_MM", 7 }, { "SVG_LENGTHTYPE_IN", 8 }, { "SVG_LENGTHTYPE_PT", 9 }, { "SVG_LENGTHTYPE_PC", 10 } };
    for (auto const& [name, value] : length_types) {
        length->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        length_constructor.as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
    define_getter(in, *length, "unitType", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
        if (!self)
            return std::nullopt;
        return js::Value::number(length_of(**self).unit);
    });
    define_getter(
        in, *length, "value",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
            if (!self)
                return std::nullopt;
            return js::Value::number(length_value(internals_of(interp), **self));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
            if (!self)
                return std::nullopt;
            std::optional<double> const number = interp.to_number(js::argument(args, 0));
            if (!number)
                return std::nullopt;
            if (!std::isfinite(*number))
                return interp.throw_type_error("The provided value is non-finite.");
            // The value in user units, kept in the unit the length has.
            Realm::Internals& internals = internals_of(interp);
            Length current = length_of(**self);
            double const per_unit = current.number != 0 ? length_value(internals, **self) / current.number : 0;
            if (per_unit == 0 || current.unit == unit::number || current.unit == unit::px)
                current = Length { *number, current.unit == unit::px ? unit::px : unit::number };
            else
                current.number = *number / per_unit;
            return write_length(internals, **self, current);
        });
    define_getter(
        in, *length, "valueInSpecifiedUnits",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
            if (!self)
                return std::nullopt;
            return js::Value::number(length_of(**self).number);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
            if (!self)
                return std::nullopt;
            std::optional<double> const number = interp.to_number(js::argument(args, 0));
            if (!number)
                return std::nullopt;
            if (!std::isfinite(*number))
                return interp.throw_type_error("The provided value is non-finite.");
            Length current = length_of(**self);
            current.number = *number;
            return write_length(internals_of(interp), **self, current);
        });
    define_getter(
        in, *length, "valueAsString",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
            if (!self)
                return std::nullopt;
            return internals_of(interp).string(serialize(length_of(**self)));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
            if (!self)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            std::optional<Length> const parsed = parse_length(*text);
            if (!parsed)
                return internals.throw_dom_exception("SyntaxError", "The value provided ('" + *text + "') is invalid.");
            return write_length(internals, **self, *parsed);
        });
    define_operation(interpreter, *length, "newValueSpecifiedUnits", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
        if (!self)
            return std::nullopt;
        std::optional<double> const type = interp.to_number(js::argument(args, 0));
        std::optional<double> const number = type ? interp.to_number(js::argument(args, 1)) : std::nullopt;
        if (!number)
            return std::nullopt;
        if (!std::isfinite(*number))
            return interp.throw_type_error("The provided value is non-finite.");
        if (*type < 1 || *type > 10)
            return internals_of(interp).throw_dom_exception("NotSupportedError", "The unit type is not supported.");
        return write_length(internals_of(interp), **self, Length { *number, static_cast<int>(*type) });
    });
    define_operation(interpreter, *length, "convertToSpecifiedUnits", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Length);
        if (!self)
            return std::nullopt;
        std::optional<double> const type = interp.to_number(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        if (*type < 1 || *type > 10)
            return internals_of(interp).throw_dom_exception("NotSupportedError", "The unit type is not supported.");
        Realm::Internals& internals = internals_of(interp);
        double const value = length_value(internals, **self);
        Length const unit_length { 1, static_cast<int>(*type) };
        LengthContext context;
        Direction direction = Direction::Horizontal;
        if (dom::Element const* const element = (*self)->element()) {
            context = length_context(internals, *element);
            direction = direction_of((*self)->attribute);
        }
        double const per_unit = user_units(unit_length, direction, context);
        return write_length(internals, **self, Length { per_unit != 0 ? value / per_unit : 0, static_cast<int>(*type) });
    });

    // SVGAnimatedLength (§4.6.3).
    js::Object* animated_length = define_interface(in, "SVGAnimatedLength", nullptr);
    animated_pair(in, *animated_length, Kind::AnimatedLength, Kind::Length, "SVGLength");

    // SVGAnimatedNumber, SVGAnimatedInteger, SVGAnimatedBoolean: baseVal a
    // primitive over the attribute.
    for (Kind const kind : { Kind::AnimatedNumber, Kind::AnimatedInteger, Kind::AnimatedBoolean }) {
        std::string_view const name = kind == Kind::AnimatedNumber ? "SVGAnimatedNumber" : kind == Kind::AnimatedInteger ? "SVGAnimatedInteger" : "SVGAnimatedBoolean";
        js::Object* animated = define_interface(in, name, nullptr);
        auto const read = [kind](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, kind);
            if (!self)
                return std::nullopt;
            dom::Element const* const element = (*self)->element();
            std::string const text = element ? std::string(trim(attribute_text(*element, (*self)->attribute))) : std::string();
            if (kind == Kind::AnimatedBoolean)
                return js::Value::boolean(text == "true");
            std::size_t used = 0;
            std::optional<double> number = parse_number_prefix(text, used);
            if (number && kind == Kind::AnimatedNumber && used < text.size() && text[used] == '%')
                *number /= 100; // an offset written as a percentage
            if (!number || (used < text.size() && text[used] != '%'))
                number = (*self)->fallback;
            return js::Value::number(kind == Kind::AnimatedInteger ? std::trunc(*number) : *number);
        };
        define_getter(in, *animated, "baseVal", read, [kind](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, kind);
            if (!self)
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            std::string text;
            if (kind == Kind::AnimatedBoolean) {
                text = js::Interpreter::to_boolean(value) ? "true" : "false";
            } else {
                std::optional<double> const number = interp.to_number(value);
                if (!number)
                    return std::nullopt;
                if (!std::isfinite(*number))
                    return interp.throw_type_error("The provided value is non-finite.");
                text = number_text(kind == Kind::AnimatedInteger ? std::trunc(*number) : *number);
            }
            if (dom::Element* const element = (*self)->element())
                set_attribute(internals_of(interp), *element, (*self)->attribute, text);
            return js::Value::undefined();
        });
        define_getter(in, *animated, "animVal", read);
    }

    // SVGAnimatedString (Node.cpp made it for href; className and the rest
    // read their attribute here).
    js::Object* animated_string = in.prototype("SVGAnimatedString");
    (void)animated_string;

    // SVGAnimatedEnumeration (§4.6.10).
    js::Object* enumeration = define_interface(in, "SVGAnimatedEnumeration", nullptr);
    define_getter(
        in, *enumeration, "baseVal",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedEnumeration);
            if (!self)
                return std::nullopt;
            return js::Value::number(enumeration_value(**self));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedEnumeration);
            if (!self)
                return std::nullopt;
            std::optional<double> const number = interp.to_number(js::argument(args, 0));
            if (!number)
                return std::nullopt;
            std::size_t const index = static_cast<std::size_t>(*number);
            if (*number < 1 || index > (*self)->table->keywords.size())
                return interp.throw_type_error("The enumeration value provided is not a valid value.");
            if (dom::Element* const element = (*self)->element())
                set_attribute(internals_of(interp), *element, (*self)->attribute, std::string((*self)->table->keywords[index - 1]));
            return js::Value::undefined();
        });
    define_getter(in, *enumeration, "animVal", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedEnumeration);
        if (!self)
            return std::nullopt;
        return js::Value::number(enumeration_value(**self));
    });

    // SVGAnimatedRect (§4.6.12): the viewBox as a DOMRect, null without one.
    js::Object* animated_rect = define_interface(in, "SVGAnimatedRect", nullptr);
    for (std::string_view const name : { "baseVal", "animVal" }) {
        define_getter(in, *animated_rect, name, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedRect);
            if (!self)
                return std::nullopt;
            dom::Element const* const element = (*self)->element();
            if (!element || !has_attribute(*element, (*self)->attribute))
                return js::Value::null();
            std::string const text = attribute_text(*element, (*self)->attribute);
            std::vector<double> numbers;
            std::string_view rest = text;
            for (;;) {
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == ',' || rest.front() == '\t' || rest.front() == '\n'))
                    rest.remove_prefix(1);
                if (rest.empty())
                    break;
                std::size_t used = 0;
                std::optional<double> const number = parse_number_prefix(rest, used);
                if (!number)
                    break;
                numbers.push_back(*number);
                rest.remove_prefix(used);
            }
            if (numbers.size() != 4)
                return make_rect(internals_of(interp), 0, 0, 0, 0);
            return make_rect(internals_of(interp), numbers[0], numbers[1], numbers[2], numbers[3]);
        });
    }

    // SVGPreserveAspectRatio and its animated form (§8.6.4).
    js::Object* aspect = define_interface(in, "SVGPreserveAspectRatio", nullptr);
    js::Value const aspect_constructor = *interpreter.get(*global, interpreter.key("SVGPreserveAspectRatio"));
    constexpr std::pair<char const*, int> aspect_constants[] = { { "SVG_PRESERVEASPECTRATIO_UNKNOWN", 0 }, { "SVG_PRESERVEASPECTRATIO_NONE", 1 },
        { "SVG_PRESERVEASPECTRATIO_XMINYMIN", 2 }, { "SVG_PRESERVEASPECTRATIO_XMIDYMIN", 3 }, { "SVG_PRESERVEASPECTRATIO_XMAXYMIN", 4 },
        { "SVG_PRESERVEASPECTRATIO_XMINYMID", 5 }, { "SVG_PRESERVEASPECTRATIO_XMIDYMID", 6 }, { "SVG_PRESERVEASPECTRATIO_XMAXYMID", 7 },
        { "SVG_PRESERVEASPECTRATIO_XMINYMAX", 8 }, { "SVG_PRESERVEASPECTRATIO_XMIDYMAX", 9 }, { "SVG_PRESERVEASPECTRATIO_XMAXYMAX", 10 },
        { "SVG_MEETORSLICE_UNKNOWN", 0 }, { "SVG_MEETORSLICE_MEET", 1 }, { "SVG_MEETORSLICE_SLICE", 2 } };
    for (auto const& [name, value] : aspect_constants) {
        aspect->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        aspect_constructor.as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
    define_getter(in, *aspect, "align", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::PreserveAspectRatio);
        if (!self)
            return std::nullopt;
        return js::Value::number(aspect_ratio_of((*self)->element(), (*self)->attribute).first);
    });
    define_getter(in, *aspect, "meetOrSlice", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::PreserveAspectRatio);
        if (!self)
            return std::nullopt;
        return js::Value::number(aspect_ratio_of((*self)->element(), (*self)->attribute).second);
    });
    js::Object* animated_aspect = define_interface(in, "SVGAnimatedPreserveAspectRatio", nullptr);
    animated_pair(in, *animated_aspect, Kind::AnimatedPreserveAspectRatio, Kind::PreserveAspectRatio, "SVGPreserveAspectRatio");

    // SVGTransform (§8.7.1).
    js::Object* transform = define_interface(in, "SVGTransform", nullptr);
    js::Value const transform_constructor = *interpreter.get(*global, interpreter.key("SVGTransform"));
    constexpr std::pair<char const*, int> transform_types[] = { { "SVG_TRANSFORM_UNKNOWN", 0 }, { "SVG_TRANSFORM_MATRIX", 1 },
        { "SVG_TRANSFORM_TRANSLATE", 2 }, { "SVG_TRANSFORM_SCALE", 3 }, { "SVG_TRANSFORM_ROTATE", 4 }, { "SVG_TRANSFORM_SKEWX", 5 },
        { "SVG_TRANSFORM_SKEWY", 6 } };
    for (auto const& [name, value] : transform_types) {
        transform->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        transform_constructor.as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
    define_getter(in, *transform, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Transform);
        if (!self)
            return std::nullopt;
        std::optional<TransformData> const data = transform_data(**self);
        return js::Value::number(data ? data->type : 0);
    });
    define_getter(in, *transform, "angle", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Transform);
        if (!self)
            return std::nullopt;
        std::optional<TransformData> const data = transform_data(**self);
        return js::Value::number(data ? data->angle : 0);
    });
    define_getter(in, *transform, "matrix", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Transform);
        if (!self)
            return std::nullopt;
        std::optional<TransformData> const data = transform_data(**self);
        return new_dom_matrix_2d(internals_of(interp), to_2d(data ? data->matrix : svg::Matrix {}));
    });
    define_operation(interpreter, *transform, "setMatrix", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Transform);
        if (!self)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<Matrix2D> const matrix = matrix_2d_from_init(internals, js::argument(args, 0));
        if (!matrix)
            return std::nullopt;
        return write_transform(internals, **self, matrix_item(from_2d(*matrix)));
    });
    constexpr std::pair<char const*, int> setters[] = { { "setTranslate", transform_translate }, { "setScale", transform_scale },
        { "setRotate", transform_rotate }, { "setSkewX", transform_skew_x }, { "setSkewY", transform_skew_y } };
    for (auto const& [name, type] : setters) {
        int const arguments = type == transform_rotate ? 3 : type == transform_translate || type == transform_scale ? 2 : 1;
        define_operation(interpreter, *transform, name, arguments, [type, arguments](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::Transform);
            if (!self)
                return std::nullopt;
            TransformItem item;
            item.type = type;
            for (int k = 0; k < arguments; ++k) {
                std::optional<double> const number = interp.to_number(js::argument(args, static_cast<std::size_t>(k)));
                if (!number)
                    return std::nullopt;
                if (!std::isfinite(*number))
                    return interp.throw_type_error("The provided value is non-finite.");
                item.values.push_back(*number);
            }
            return write_transform(internals_of(interp), **self, item);
        });
    }

    // SVGTransformList (§8.7.2).
    js::Object* list = define_interface(in, "SVGTransformList", nullptr);
    auto const items_of = [](SvgValueObject const& self) {
        dom::Element const* const element = self.element();
        return element ? parse_transform_items(attribute_text(*element, self.attribute)) : std::vector<TransformItem> {};
    };
    for (std::string_view const name : { "numberOfItems", "length" }) {
        define_getter(in, *list, name, [items_of](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::TransformList);
            if (!self)
                return std::nullopt;
            return js::Value::number(static_cast<double>(items_of(**self).size()));
        });
    }
    define_operation(interpreter, *list, "getItem", 1, [items_of](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::TransformList);
        if (!self)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if (*index < 0 || *index >= static_cast<double>(items_of(**self).size()))
            return internals.throw_dom_exception("IndexSizeError", "The index provided is greater than or equal to the number of items.");
        return js::Value::object(new_transform(internals, (*self)->wrapper, (*self)->attribute, static_cast<std::size_t>(*index), (*self)->read_only));
    });
    define_operation(interpreter, *list, "consolidate", 0, [items_of](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::TransformList);
        if (!self)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::vector<TransformItem> const items = items_of(**self);
        if (items.empty())
            return js::Value::null();
        if ((*self)->read_only)
            return no_modification(interp);
        svg::Matrix combined;
        for (std::size_t k = items.size(); k-- > 0;)
            combined = item_matrix(items[k]).then(combined);
        if (dom::Element* const element = (*self)->element())
            set_attribute(internals, *element, (*self)->attribute, serialize_items({ matrix_item(combined) }));
        return js::Value::object(new_transform(internals, (*self)->wrapper, (*self)->attribute, 0, false));
    });
    define_operation(interpreter, *list, "clear", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::TransformList);
        if (!self)
            return std::nullopt;
        if ((*self)->read_only)
            return no_modification(interp);
        if (dom::Element* const element = (*self)->element())
            set_attribute(internals_of(interp), *element, (*self)->attribute, "");
        return js::Value::undefined();
    });
    // initialize, appendItem, insertItemBefore, replaceItem, removeItem: the
    // list rewritten and the transform at its new place handed back.
    enum class Edit : std::uint8_t { Initialize, Append, Insert, Replace, Remove };
    constexpr std::pair<char const*, Edit> edits[] = { { "initialize", Edit::Initialize }, { "appendItem", Edit::Append },
        { "insertItemBefore", Edit::Insert }, { "replaceItem", Edit::Replace }, { "removeItem", Edit::Remove } };
    for (auto const& [name, edit] : edits) {
        int const arguments = edit == Edit::Insert || edit == Edit::Replace ? 2 : 1;
        define_operation(interpreter, *list, name, arguments, [edit, items_of](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::TransformList);
            if (!self)
                return std::nullopt;
            if ((*self)->read_only)
                return no_modification(interp);
            Realm::Internals& internals = internals_of(interp);
            std::vector<TransformItem> items = items_of(**self);
            std::optional<TransformItem> item;
            if (edit != Edit::Remove) {
                item = item_from(interp, js::argument(args, 0));
                if (!item)
                    return std::nullopt;
            }
            std::size_t index = items.size();
            if (edit == Edit::Insert || edit == Edit::Replace || edit == Edit::Remove) {
                std::optional<double> const at = interp.to_number(js::argument(args, edit == Edit::Remove ? 0 : 1));
                if (!at)
                    return std::nullopt;
                index = *at < 0 ? 0 : static_cast<std::size_t>(*at);
                if ((edit != Edit::Insert && index >= items.size()))
                    return internals.throw_dom_exception("IndexSizeError", "The index provided is greater than or equal to the number of items.");
                index = std::min(index, items.size());
            }
            TransformItem removed;
            switch (edit) {
            case Edit::Initialize:
                items = { *item };
                index = 0;
                break;
            case Edit::Append:
                items.push_back(*item);
                index = items.size() - 1;
                break;
            case Edit::Insert:
                items.insert(items.begin() + static_cast<std::ptrdiff_t>(index), *item);
                break;
            case Edit::Replace:
                items[index] = *item;
                break;
            case Edit::Remove:
                removed = items[index];
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
                break;
            }
            if (dom::Element* const element = (*self)->element())
                set_attribute(internals, *element, (*self)->attribute, serialize_items(items));
            if (edit == Edit::Remove) {
                SvgValueObject* const detached = new_transform(internals, nullptr, std::string(), 0, false);
                TransformData const data = data_of(removed);
                detached->own_type = data.type;
                detached->own_matrix = data.matrix;
                detached->own_angle = data.angle;
                return js::Value::object(detached);
            }
            return js::Value::object(new_transform(internals, (*self)->wrapper, (*self)->attribute, index, false));
        });
    }
    define_operation(interpreter, *list, "createSVGTransformFromMatrix", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<Matrix2D> const matrix = matrix_2d_from_init(internals, js::argument(args, 0));
        if (!matrix)
            return std::nullopt;
        SvgValueObject* const made = new_transform(internals, nullptr, std::string(), 0, false);
        made->own_matrix = from_2d(*matrix);
        return js::Value::object(made);
    });
    js::Object* animated_list = define_interface(in, "SVGAnimatedTransformList", nullptr);
    animated_pair(in, *animated_list, Kind::AnimatedTransformList, Kind::TransformList, "SVGTransformList");

    // SVGPointList (§10.7.2): the points as DOMPoints, read only here.
    js::Object* points = define_interface(in, "SVGPointList", nullptr);
    auto const points_of = [](SvgValueObject const& self) {
        std::vector<double> numbers;
        if (dom::Element const* const element = self.element()) {
            std::string const text = attribute_text(*element, self.attribute);
            std::string_view rest = text;
            for (;;) {
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == ',' || rest.front() == '\t' || rest.front() == '\n' || rest.front() == '\r'))
                    rest.remove_prefix(1);
                if (rest.empty())
                    break;
                std::size_t used = 0;
                std::optional<double> const number = parse_number_prefix(rest, used);
                if (!number)
                    break;
                numbers.push_back(*number);
                rest.remove_prefix(used);
            }
        }
        if (numbers.size() % 2 == 1)
            numbers.pop_back();
        return numbers;
    };
    for (std::string_view const name : { "numberOfItems", "length" }) {
        define_getter(in, *points, name, [points_of](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::PointList);
            if (!self)
                return std::nullopt;
            return js::Value::number(static_cast<double>(points_of(**self).size() / 2));
        });
    }
    define_operation(interpreter, *points, "getItem", 1, [points_of](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::PointList);
        if (!self)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        std::vector<double> const numbers = points_of(**self);
        if (*index < 0 || *index >= static_cast<double>(numbers.size() / 2))
            return internals_of(interp).throw_dom_exception("IndexSizeError", "The index provided is greater than or equal to the number of items.");
        std::size_t const at = static_cast<std::size_t>(*index) * 2;
        return new_dom_point(internals_of(interp), numbers[at], numbers[at + 1]);
    });

    // SVGNumber (§4.6.1): createSVGNumber's object, a value of its own.
    js::Object* number = define_interface(in, "SVGNumber", nullptr);
    define_getter(
        in, *number, "value",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedNumber);
            if (!self)
                return std::nullopt;
            return js::Value::number((*self)->own_number);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedNumber);
            if (!self)
                return std::nullopt;
            std::optional<double> const value = interp.to_number(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            (*self)->own_number = *value;
            return js::Value::undefined();
        });

    // SVGUnitTypes (§4.7): constants only.
    js::Object* unit_types = define_interface(in, "SVGUnitTypes", nullptr);
    js::Value const unit_types_constructor = *interpreter.get(*global, interpreter.key("SVGUnitTypes"));
    for (auto const& [name, value] : { std::pair { "SVG_UNIT_TYPE_UNKNOWN", 0 }, std::pair { "SVG_UNIT_TYPE_USERSPACEONUSE", 1 },
             std::pair { "SVG_UNIT_TYPE_OBJECTBOUNDINGBOX", 2 } }) {
        unit_types->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        unit_types_constructor.as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
}

// className (SVG 2 §4.4.1): an SVGAnimatedString over class.
void install_class_name(Realm::Internals& in, js::Object& svg_element)
{
    animated_attribute<Kind::AnimatedString>(in, svg_element, "className", "class", "SVGAnimatedStringOfAttribute");
}

}

std::string_view svg_interface_for(std::string_view local_name)
{
    for (TagInterface const& entry : svg_tags) {
        if (entry.tag == local_name)
            return entry.interface;
    }
    return "SVGElement";
}

std::optional<LayoutBox> svg_client_box(Realm::Internals& in, dom::Element const& element)
{
    if (element.namespace_uri() != dom::ns::svg || svg::outermost_svg(element) == &element)
        return std::nullopt;
    std::optional<Placement> const placement = place(in, element);
    if (!placement || !in.hooks.layout_box || !in.hooks.layout_box(*placement->root))
        return std::nullopt;
    svg::Box const local = svg::bounding_box(element, placement->space, style_lookup(in));
    std::pair<double, double> const scroll = *scroll_of(in);
    svg::Matrix const to_client = placement->space.ctm.then(
        svg::Matrix::translate(static_cast<float>(static_cast<double>(placement->box.x) - scroll.first), static_cast<float>(static_cast<double>(placement->box.y) - scroll.second)));
    svg::Point const corners[4] = { to_client.apply(svg::Point { local.left, local.top }), to_client.apply(svg::Point { local.right, local.top }),
        to_client.apply(svg::Point { local.left, local.bottom }), to_client.apply(svg::Point { local.right, local.bottom }) };
    float left = corners[0].x;
    float top = corners[0].y;
    float right = corners[0].x;
    float bottom = corners[0].y;
    for (svg::Point const& p : corners) {
        left = std::min(left, p.x);
        top = std::min(top, p.y);
        right = std::max(right, p.x);
        bottom = std::max(bottom, p.y);
    }
    return LayoutBox { left, top, right - left, bottom - top };
}

void install_svg_elements(Realm::Internals& in, js::Object& svg_element)
{
    js::Interpreter& interpreter = in.interpreter;
    install_types(in);

    // SVGAnimatedString of an attribute other than href: its own interface
    // object is Node.cpp's SVGAnimatedString; these read and write their
    // attribute through the same prototype's members, which look for href.
    // So className gets a prototype of its own under the same name's
    // members: written here as a plain pair over the attribute.
    js::Object* string_of_attribute = interpreter.new_object(in.prototype("SVGAnimatedString"));
    in.prototypes.emplace("SVGAnimatedStringOfAttribute", string_of_attribute);
    for (bool const animation : { false, true }) {
        auto const read = [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedString);
            if (!self)
                return std::nullopt;
            dom::Element const* const element = (*self)->element();
            return internals_of(interp).string(element ? attribute_text(*element, (*self)->attribute) : std::string());
        };
        if (animation) {
            define_getter(in, *string_of_attribute, "animVal", read);
        } else {
            define_getter(in, *string_of_attribute, "baseVal", read, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<SvgValueObject*> const self = this_value_object(interp, this_value, Kind::AnimatedString);
                if (!self)
                    return std::nullopt;
                Realm::Internals& internals = internals_of(interp);
                std::optional<std::string> value = internals.to_utf8(js::argument(args, 0));
                if (!value)
                    return std::nullopt;
                if (dom::Element* const element = (*self)->element())
                    set_attribute(internals, *element, (*self)->attribute, std::move(*value));
                return js::Value::undefined();
            });
        }
    }
    install_class_name(in, svg_element);

    // The interfaces, parents first.
    js::Object* graphics = interface(in, "SVGGraphicsElement", "SVGElement");
    js::Object* geometry = interface(in, "SVGGeometryElement", "SVGGraphicsElement");
    animated_attribute<Kind::AnimatedTransformList>(in, *graphics, "transform", "transform", "SVGAnimatedTransformList");
    element_method(in, *graphics, "getBBox", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native { return bbox_of(internals, e); });
    element_method(in, *graphics, "getCTM", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        std::optional<Placement> const placement = place(internals, e);
        if (!placement)
            return js::Value::null();
        return new_dom_matrix_2d(internals, to_2d(placement->space.ctm));
    });
    element_method(in, *graphics, "getScreenCTM", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        std::optional<svg::Matrix> const matrix = screen_ctm(internals, e);
        if (!matrix)
            return js::Value::null();
        return new_dom_matrix_2d(internals, to_2d(*matrix));
    });
    // SVGTests: what a <switch> picks by — every feature here.
    for (std::string_view const name : { "requiredExtensions", "systemLanguage" }) {
        element_getter(in, *graphics, name, [](Realm::Internals& internals, dom::Element&) -> Native {
            return js::Value::object(internals.interpreter.new_array());
        });
    }

    animated_attribute<Kind::AnimatedNumber>(in, *geometry, "pathLength", "pathLength", "SVGAnimatedNumber");
    element_method(in, *geometry, "getTotalLength", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        std::optional<svg::Path> const path = path_of(internals, e);
        return js::Value::number(path ? static_cast<double>(svg::path_length(*path)) : 0.0);
    });
    element_method(in, *geometry, "getPointAtLength", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<double> const distance = internals.interpreter.to_number(js::argument(args, 0));
        if (!distance)
            return std::nullopt;
        if (!std::isfinite(*distance))
            return internals.interpreter.throw_type_error("The provided value is non-finite.");
        std::optional<svg::Path> const path = path_of(internals, e);
        if (!path || path->empty())
            return internals.throw_dom_exception("InvalidStateError", "The element's path is empty.");
        svg::Point const point = svg::point_at_length(*path, static_cast<float>(*distance));
        return new_dom_point(internals, point.x, point.y);
    });
    for (bool const stroke : { false, true }) {
        element_method(in, *geometry, stroke ? "isPointInStroke" : "isPointInFill", 0, [stroke](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            std::optional<std::array<double, 4>> const point = dom_point_from_init(internals, js::argument(args, 0));
            if (!point)
                return std::nullopt;
            std::optional<svg::Path> const path = path_of(internals, e);
            if (!path)
                return js::Value::boolean(false);
            css::ComputedStyle const* const style = internals.hooks.computed_style ? internals.hooks.computed_style(e) : nullptr;
            svg::Point const p { static_cast<float>((*point)[0]), static_cast<float>((*point)[1]) };
            if (stroke) {
                float width = 1;
                if (style && style->stroke_width.kind == css::LengthPercent::Kind::Px)
                    width = style->stroke_width.value;
                return js::Value::boolean(svg::point_in_stroke(*path, p, width));
            }
            bool const even_odd = style && style->fill_rule == css::FillRule::EvenOdd;
            return js::Value::boolean(svg::point_in_fill(*path, p, even_odd));
        });
    }

    // The shapes (§10).
    lengths(in, *interface(in, "SVGRectElement", "SVGGeometryElement"), { "x", "y", "width", "height", "rx", "ry" });
    lengths(in, *interface(in, "SVGCircleElement", "SVGGeometryElement"), { "cx", "cy", "r" });
    lengths(in, *interface(in, "SVGEllipseElement", "SVGGeometryElement"), { "cx", "cy", "rx", "ry" });
    lengths(in, *interface(in, "SVGLineElement", "SVGGeometryElement"), { "x1", "y1", "x2", "y2" });
    interface(in, "SVGPathElement", "SVGGeometryElement");
    for (std::string_view const name : { "SVGPolylineElement", "SVGPolygonElement" }) {
        js::Object* poly = interface(in, name, "SVGGeometryElement");
        for (std::string_view const member : { "points", "animatedPoints" })
            animated_attribute<Kind::PointList>(in, *poly, member, "points", "SVGPointList");
    }

    // The structure (§5).
    js::Object* svg = interface(in, "SVGSVGElement", "SVGGraphicsElement");
    js::Object* use = interface(in, "SVGUseElement", "SVGGraphicsElement");
    js::Object* image = interface(in, "SVGImageElement", "SVGGraphicsElement");
    js::Object* foreign = interface(in, "SVGForeignObjectElement", "SVGGraphicsElement");
    js::Object* symbol = interface(in, "SVGSymbolElement", "SVGGraphicsElement");
    interface(in, "SVGGElement", "SVGGraphicsElement");
    interface(in, "SVGDefsElement", "SVGGraphicsElement");
    interface(in, "SVGSwitchElement", "SVGGraphicsElement");
    js::Object* view = interface(in, "SVGViewElement", "SVGElement");
    for (std::string_view const name : { "x", "y", "width", "height" }) {
        // An <svg>'s own width and height default to 100%.
        std::string_view const fallback = (name == "width" || name == "height") ? "100%" : "";
        animated_attribute<Kind::AnimatedLength>(in, *svg, name, name, "SVGAnimatedLength", 0, fallback);
        animated_attribute<Kind::AnimatedLength>(in, *use, name, name, "SVGAnimatedLength");
        animated_attribute<Kind::AnimatedLength>(in, *image, name, name, "SVGAnimatedLength");
        animated_attribute<Kind::AnimatedLength>(in, *foreign, name, name, "SVGAnimatedLength");
    }
    for (js::Object* fits : { svg, symbol, view }) {
        animated_attribute<Kind::AnimatedRect>(in, *fits, "viewBox", "viewBox", "SVGAnimatedRect");
        animated_attribute<Kind::AnimatedPreserveAspectRatio>(in, *fits, "preserveAspectRatio", "preserveAspectRatio", "SVGAnimatedPreserveAspectRatio");
    }
    animated_attribute<Kind::AnimatedPreserveAspectRatio>(in, *image, "preserveAspectRatio", "preserveAspectRatio", "SVGAnimatedPreserveAspectRatio");
    for (js::Object* linked : { use, image })
        animated_attribute<Kind::AnimatedString>(in, *linked, "href", "href", "SVGAnimatedStringOfAttribute");

    // SVGSVGElement's own members (§5.2.2).
    element_method(in, *svg, "createSVGPoint", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native { return new_dom_point(internals, 0, 0); });
    element_method(in, *svg, "createSVGMatrix", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native {
        return new_dom_matrix_2d(internals, Matrix2D {});
    });
    element_method(in, *svg, "createSVGRect", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native { return make_rect(internals, 0, 0, 0, 0); });
    element_method(in, *svg, "createSVGNumber", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native {
        return js::Value::object(internals.interpreter.heap().allocate<SvgValueObject>(internals.prototype("SVGNumber"), nullptr, Kind::AnimatedNumber, std::string()));
    });
    element_method(in, *svg, "createSVGLength", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native {
        return js::Value::object(internals.interpreter.heap().allocate<SvgValueObject>(internals.prototype("SVGLength"), nullptr, Kind::Length, std::string()));
    });
    element_method(in, *svg, "createSVGTransform", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native {
        return js::Value::object(new_transform(internals, nullptr, std::string(), 0, false));
    });
    element_method(in, *svg, "createSVGTransformFromMatrix", 0, [](Realm::Internals& internals, dom::Element&, Args args) -> Native {
        std::optional<Matrix2D> const matrix = matrix_2d_from_init(internals, js::argument(args, 0));
        if (!matrix)
            return std::nullopt;
        SvgValueObject* const made = new_transform(internals, nullptr, std::string(), 0, false);
        made->own_matrix = from_2d(*matrix);
        return js::Value::object(made);
    });
    element_method(in, *svg, "getElementById", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const id = internals.to_utf8(js::argument(args, 0));
        if (!id)
            return std::nullopt;
        std::vector<dom::Node*> pending;
        for (dom::Node* child : e.children())
            pending.push_back(child);
        std::reverse(pending.begin(), pending.end());
        while (!pending.empty()) {
            dom::Node* const node = pending.back();
            pending.pop_back();
            if (node->is_element() && attribute_text(static_cast<dom::Element const&>(*node), "id") == *id)
                return js::Value::object(internals.wrap(*node));
            std::vector<dom::Node*> children(node->children().begin(), node->children().end());
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                pending.push_back(*it);
        }
        return js::Value::null();
    });
    // No animation timeline runs: paused never, the time ever zero.
    for (std::string_view const name : { "pauseAnimations", "unpauseAnimations", "setCurrentTime", "unsuspendRedraw", "unsuspendRedrawAll", "forceRedraw", "deselectAll" })
        element_method(in, *svg, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    element_method(in, *svg, "animationsPaused", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(false); });
    element_method(in, *svg, "getCurrentTime", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::number(0); });
    element_method(in, *svg, "suspendRedraw", 1, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::number(0); });
    element_method(in, *svg, "checkIntersection", 2, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(false); });
    element_method(in, *svg, "checkEnclosure", 2, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(false); });
    element_getter(in, *svg, "currentScale", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(1); });
    element_getter(in, *svg, "currentTranslate", [](Realm::Internals& internals, dom::Element&) -> Native { return new_dom_point(internals, 0, 0); });

    // Text (§11).
    js::Object* text_content = interface(in, "SVGTextContentElement", "SVGGraphicsElement");
    js::Object* text_content_constructor = interpreter.get(*interpreter.global(), interpreter.key("SVGTextContentElement"))->as_object();
    for (auto const& [name, value] : { std::pair { "LENGTHADJUST_UNKNOWN", 0 }, std::pair { "LENGTHADJUST_SPACING", 1 }, std::pair { "LENGTHADJUST_SPACINGANDGLYPHS", 2 } }) {
        text_content->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        text_content_constructor->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
    animated_attribute<Kind::AnimatedLength>(in, *text_content, "textLength", "textLength", "SVGAnimatedLength");
    animated_attribute<Kind::AnimatedEnumeration>(in, *text_content, "lengthAdjust", "lengthAdjust", "SVGAnimatedEnumeration", 0, {}, &length_adjust);
    element_method(in, *text_content, "getComputedTextLength", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        return js::Value::number(svg::text_length(e, style_lookup(internals)));
    });
    element_method(in, *text_content, "getNumberOfChars", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        (void)internals;
        std::string text;
        gather_text(e, text);
        return js::Value::number(static_cast<double>(utf16_length(text)));
    });
    element_method(in, *text_content, "getSubStringLength", 2, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<double> const count = internals.interpreter.to_number(js::argument(args, 1));
        if (!count)
            return std::nullopt;
        std::string text;
        gather_text(e, text);
        double const characters = static_cast<double>(utf16_length(text));
        double const total = svg::text_length(e, style_lookup(internals));
        return js::Value::number(characters == 0 ? 0 : total * std::clamp(*count, 0.0, characters) / characters);
    });
    js::Object* positioning = interface(in, "SVGTextPositioningElement", "SVGTextContentElement");
    (void)positioning;
    interface(in, "SVGTextElement", "SVGTextPositioningElement");
    interface(in, "SVGTSpanElement", "SVGTextPositioningElement");
    js::Object* text_path = interface(in, "SVGTextPathElement", "SVGTextContentElement");
    animated_attribute<Kind::AnimatedLength>(in, *text_path, "startOffset", "startOffset", "SVGAnimatedLength");
    animated_attribute<Kind::AnimatedEnumeration>(in, *text_path, "method", "method", "SVGAnimatedEnumeration", 0, {}, &text_path_method);
    animated_attribute<Kind::AnimatedEnumeration>(in, *text_path, "spacing", "spacing", "SVGAnimatedEnumeration", 0, {}, &text_path_spacing);
    animated_attribute<Kind::AnimatedString>(in, *text_path, "href", "href", "SVGAnimatedStringOfAttribute");

    // Descriptive and document elements (§5.9, §6, §15).
    for (std::string_view const name : { "SVGTitleElement", "SVGDescElement", "SVGMetadataElement", "SVGMPathElement" })
        interface(in, name, "SVGElement");
    js::Object* style = interface(in, "SVGStyleElement", "SVGElement");
    for (std::string_view const name : { "type", "media", "title" }) {
        element_accessor(
            in, *style, name,
            [name](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(attribute_text(e, name)); },
            [name](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> text = internals.to_utf8(value);
                if (!text)
                    return std::nullopt;
                set_attribute(internals, e, name, std::move(*text));
                return js::Value::undefined();
            });
    }
    js::Object* script = interface(in, "SVGScriptElement", "SVGElement");
    animated_attribute<Kind::AnimatedString>(in, *script, "href", "href", "SVGAnimatedStringOfAttribute");

    // Paint servers (§14).
    js::Object* gradient = interface(in, "SVGGradientElement", "SVGElement");
    js::Object* gradient_constructor = interpreter.get(*interpreter.global(), interpreter.key("SVGGradientElement"))->as_object();
    for (auto const& [name, value] : { std::pair { "SVG_SPREADMETHOD_UNKNOWN", 0 }, std::pair { "SVG_SPREADMETHOD_PAD", 1 },
             std::pair { "SVG_SPREADMETHOD_REFLECT", 2 }, std::pair { "SVG_SPREADMETHOD_REPEAT", 3 } }) {
        gradient->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        gradient_constructor->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
    animated_attribute<Kind::AnimatedEnumeration>(in, *gradient, "gradientUnits", "gradientUnits", "SVGAnimatedEnumeration", 0, {}, &units_bounding_box);
    animated_attribute<Kind::AnimatedTransformList>(in, *gradient, "gradientTransform", "gradientTransform", "SVGAnimatedTransformList");
    animated_attribute<Kind::AnimatedEnumeration>(in, *gradient, "spreadMethod", "spreadMethod", "SVGAnimatedEnumeration", 0, {}, &spread_method);
    animated_attribute<Kind::AnimatedString>(in, *gradient, "href", "href", "SVGAnimatedStringOfAttribute");
    lengths(in, *interface(in, "SVGLinearGradientElement", "SVGGradientElement"), { "x1", "y1", "x2", "y2" });
    lengths(in, *interface(in, "SVGRadialGradientElement", "SVGGradientElement"), { "cx", "cy", "r", "fx", "fy", "fr" });
    js::Object* stop = interface(in, "SVGStopElement", "SVGElement");
    animated_attribute<Kind::AnimatedNumber>(in, *stop, "offset", "offset", "SVGAnimatedNumber");
    js::Object* pattern = interface(in, "SVGPatternElement", "SVGElement");
    lengths(in, *pattern, { "x", "y", "width", "height" });
    animated_attribute<Kind::AnimatedEnumeration>(in, *pattern, "patternUnits", "patternUnits", "SVGAnimatedEnumeration", 0, {}, &units_bounding_box);
    animated_attribute<Kind::AnimatedEnumeration>(in, *pattern, "patternContentUnits", "patternContentUnits", "SVGAnimatedEnumeration", 0, {}, &units_user_space);
    animated_attribute<Kind::AnimatedTransformList>(in, *pattern, "patternTransform", "patternTransform", "SVGAnimatedTransformList");
    animated_attribute<Kind::AnimatedRect>(in, *pattern, "viewBox", "viewBox", "SVGAnimatedRect");
    animated_attribute<Kind::AnimatedString>(in, *pattern, "href", "href", "SVGAnimatedStringOfAttribute");

    // Clipping, masking and markers (CSS Masking §14, SVG 2 §11.6).
    js::Object* clip_path = interface(in, "SVGClipPathElement", "SVGElement");
    animated_attribute<Kind::AnimatedEnumeration>(in, *clip_path, "clipPathUnits", "clipPathUnits", "SVGAnimatedEnumeration", 0, {}, &units_user_space);
    animated_attribute<Kind::AnimatedTransformList>(in, *clip_path, "transform", "transform", "SVGAnimatedTransformList");
    js::Object* mask = interface(in, "SVGMaskElement", "SVGElement");
    lengths(in, *mask, { "x", "y", "width", "height" });
    animated_attribute<Kind::AnimatedEnumeration>(in, *mask, "maskUnits", "maskUnits", "SVGAnimatedEnumeration", 0, {}, &units_bounding_box);
    animated_attribute<Kind::AnimatedEnumeration>(in, *mask, "maskContentUnits", "maskContentUnits", "SVGAnimatedEnumeration", 0, {}, &units_user_space);
    js::Object* marker = interface(in, "SVGMarkerElement", "SVGElement");
    lengths(in, *marker, { "refX", "refY", "markerWidth", "markerHeight" });
    animated_attribute<Kind::AnimatedEnumeration>(in, *marker, "markerUnits", "markerUnits", "SVGAnimatedEnumeration", 0, {}, &marker_units);
    animated_attribute<Kind::AnimatedRect>(in, *marker, "viewBox", "viewBox", "SVGAnimatedRect");
    animated_attribute<Kind::AnimatedPreserveAspectRatio>(in, *marker, "preserveAspectRatio", "preserveAspectRatio", "SVGAnimatedPreserveAspectRatio");

    // Filters (Filter Effects §9): the interfaces and their region; the
    // primitives' own attributes wait for the filters to be drawn.
    js::Object* filter = interface(in, "SVGFilterElement", "SVGElement");
    lengths(in, *filter, { "x", "y", "width", "height" });
    animated_attribute<Kind::AnimatedEnumeration>(in, *filter, "filterUnits", "filterUnits", "SVGAnimatedEnumeration", 0, {}, &units_bounding_box);
    animated_attribute<Kind::AnimatedEnumeration>(in, *filter, "primitiveUnits", "primitiveUnits", "SVGAnimatedEnumeration", 0, {}, &units_user_space);
    js::Object* transfer_function = interface(in, "SVGComponentTransferFunctionElement", "SVGElement");
    (void)transfer_function;
    for (std::string_view const name : { "SVGFEFuncAElement", "SVGFEFuncBElement", "SVGFEFuncGElement", "SVGFEFuncRElement" })
        interface(in, name, "SVGComponentTransferFunctionElement");
    for (std::string_view const name : { "SVGFEBlendElement", "SVGFEColorMatrixElement", "SVGFEComponentTransferElement", "SVGFECompositeElement",
             "SVGFEConvolveMatrixElement", "SVGFEDiffuseLightingElement", "SVGFEDisplacementMapElement", "SVGFEDistantLightElement",
             "SVGFEDropShadowElement", "SVGFEFloodElement", "SVGFEGaussianBlurElement", "SVGFEImageElement", "SVGFEMergeElement",
             "SVGFEMergeNodeElement", "SVGFEMorphologyElement", "SVGFEOffsetElement", "SVGFEPointLightElement", "SVGFESpecularLightingElement",
             "SVGFESpotLightElement", "SVGFETileElement", "SVGFETurbulenceElement" }) {
        js::Object* primitive = interface(in, name, "SVGElement");
        if (name != "SVGFEDistantLightElement" && name != "SVGFEPointLightElement" && name != "SVGFESpotLightElement" && name != "SVGFEMergeNodeElement")
            lengths(in, *primitive, { "x", "y", "width", "height" });
    }

    // Animation (§16 of SVG 1.1, kept by SVG 2): the elements and their
    // timing methods, with no timeline behind them.
    js::Object* animation = interface(in, "SVGAnimationElement", "SVGElement");
    for (std::string_view const name : { "beginElement", "endElement" })
        element_method(in, *animation, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    for (std::string_view const name : { "beginElementAt", "endElementAt" })
        element_method(in, *animation, name, 1, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    for (std::string_view const name : { "getStartTime", "getCurrentTime", "getSimpleDuration" })
        element_method(in, *animation, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::number(0); });
    element_getter(in, *animation, "targetElement", [](Realm::Internals& internals, dom::Element& e) -> Native {
        dom::Node* const parent = e.parent();
        return parent && parent->is_element() ? js::Value::object(internals.wrap(*parent)) : js::Value::null();
    });
    for (std::string_view const name : { "SVGAnimateElement", "SVGSetElement", "SVGAnimateMotionElement", "SVGAnimateTransformElement" })
        interface(in, name, "SVGAnimationElement");
}

}
