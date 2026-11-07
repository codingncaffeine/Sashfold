#include "pdf/Render.h"

#include "pdf/Filters.h"
#include "pdf/Function.h"
#include "pdf/Image.h"
#include "pdf/Parser.h"
#include "pdf/RenderInternal.h"
#include "pdf/Shading.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>

namespace sashfold::pdf {

namespace {

constexpr int k_max_form_depth = 24;
constexpr std::size_t k_max_operands = 128;

canvas::Transform matrix_of(Document const& doc, Object const& object, canvas::Transform fallback = {})
{
    Array const& a = doc.resolve(object).as_array();
    if (a.size() != 6)
        return fallback;
    canvas::Transform t { doc.resolve(a[0]).as_number(1), doc.resolve(a[1]).as_number(0), doc.resolve(a[2]).as_number(0),
        doc.resolve(a[3]).as_number(1), doc.resolve(a[4]).as_number(0), doc.resolve(a[5]).as_number(0) };
    return t.is_finite() ? t : fallback;
}

std::optional<canvas::Composite> blend_of(Document const& doc, Object const& object_in)
{
    Object const object = doc.resolve(object_in);
    if (object.is_array()) {
        for (Object const& item : object.as_array()) {
            if (auto b = blend_of(doc, item))
                return b;
        }
        return std::nullopt;
    }
    static std::unordered_map<std::string, canvas::Composite> const names = {
        { "Normal", canvas::Composite::SourceOver }, { "Compatible", canvas::Composite::SourceOver },
        { "Multiply", canvas::Composite::Multiply }, { "Screen", canvas::Composite::Screen },
        { "Overlay", canvas::Composite::Overlay }, { "Darken", canvas::Composite::Darken },
        { "Lighten", canvas::Composite::Lighten }, { "ColorDodge", canvas::Composite::ColorDodge },
        { "ColorBurn", canvas::Composite::ColorBurn }, { "HardLight", canvas::Composite::HardLight },
        { "SoftLight", canvas::Composite::SoftLight }, { "Difference", canvas::Composite::Difference },
        { "Exclusion", canvas::Composite::Exclusion }, { "Hue", canvas::Composite::Hue },
        { "Saturation", canvas::Composite::Saturation }, { "Color", canvas::Composite::Color },
        { "Luminosity", canvas::Composite::Luminosity },
    };
    auto const it = names.find(object.as_name());
    if (it == names.end())
        return std::nullopt;
    return it->second;
}

Color to_color(PaintColor const& color)
{
    float rgb[3] = { 0, 0, 0 };
    if (color.space)
        color.space->to_rgb(color.components.data(), rgb);
    return Color::rgb(static_cast<std::uint8_t>(std::lround(std::clamp(rgb[0], 0.0f, 1.0f) * 255)),
        static_cast<std::uint8_t>(std::lround(std::clamp(rgb[1], 0.0f, 1.0f) * 255)),
        static_cast<std::uint8_t>(std::lround(std::clamp(rgb[2], 0.0f, 1.0f) * 255)));
}

std::array<int, 4> mask_area(svg::Mask const& mask)
{
    return { mask.left, mask.top, mask.left + mask.width, mask.top + mask.height };
}

// Multiplies two masks (null = everything).
std::shared_ptr<canvas::ClipMask const> multiply(std::shared_ptr<canvas::ClipMask const> const& a,
    std::shared_ptr<canvas::ClipMask const> const& b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    auto out = std::make_shared<canvas::ClipMask>(a->size());
    for (std::size_t i = 0; i < a->size() && i < b->size(); ++i)
        (*out)[i] = static_cast<std::uint8_t>((static_cast<unsigned>((*a)[i]) * (*b)[i] + 127) / 255);
    return out;
}

} // namespace

PixelSize page_pixel_size(Page const& page, double scale, int extra_rotation)
{
    int const rotation = (page.rotate + extra_rotation) % 360;
    double const w = page.crop_box.width() * page.user_unit * scale;
    double const h = page.crop_box.height() * page.user_unit * scale;
    int const pw = std::max(1, static_cast<int>(std::ceil(w - 1e-6)));
    int const ph = std::max(1, static_cast<int>(std::ceil(h - 1e-6)));
    return rotation % 180 == 0 ? PixelSize { pw, ph } : PixelSize { ph, pw };
}

canvas::Transform page_to_device(Page const& page, double scale_in, int extra_rotation)
{
    double const s = scale_in * page.user_unit;
    Box const& b = page.crop_box;
    switch ((page.rotate + extra_rotation) % 360) {
    case 90:
        return canvas::Transform { 0, s, s, 0, -s * b.y0, -s * b.x0 };
    case 180:
        return canvas::Transform { -s, 0, 0, s, s * b.x1, -s * b.y0 };
    case 270:
        return canvas::Transform { 0, -s, -s, 0, s * b.y1, s * b.x1 };
    default:
        return canvas::Transform { s, 0, 0, -s, -s * b.x0, s * b.y1 };
    }
}

Renderer::Renderer(Document const& doc, canvas::Surface& surface, RenderOptions const& options)
    : m_doc(doc)
    , m_surface(&surface)
    , m_options(options)
{
    m_state.fill.space = ColorSpace::device_gray();
    m_state.stroke.space = ColorSpace::device_gray();
    // Optional content: the default configuration's OFF groups.
    Dict const& properties = doc.resolve(doc.catalog().get("OCProperties")).as_dict();
    if (!properties.empty()) {
        m_have_oc = true;
        Dict const& config = doc.resolve(properties.get("D")).as_dict();
        bool const base_off = doc.resolve(config.get("BaseState")).as_name() == "OFF";
        if (base_off) {
            for (Object const& group : doc.resolve(properties.get("OCGs")).as_array()) {
                if (group.is_ref())
                    m_off.insert(group.as_ref());
            }
            for (Object const& group : doc.resolve(config.get("ON")).as_array()) {
                if (group.is_ref())
                    m_off.erase(group.as_ref());
            }
        }
        for (Object const& group : doc.resolve(config.get("OFF")).as_array()) {
            if (group.is_ref())
                m_off.insert(group.as_ref());
        }
    }
}

void Renderer::set_initial_ctm(canvas::Transform const& ctm)
{
    m_state.ctm = ctm;
    m_base = ctm;
}

bool Renderer::cancelled() const
{
    return m_options.cancel && m_options.cancel->load(std::memory_order_relaxed);
}

bool Renderer::content_visible(Object const& oc_in) const
{
    if (!m_have_oc)
        return true;
    Object const oc = m_doc.resolve(oc_in);
    Dict const& dict = oc.as_dict();
    std::string const& type = dict.get("Type").as_name();
    auto const group_on = [&](Object const& g) { return !(g.is_ref() && m_off.count(g.as_ref())); };
    if (type == "OCG" || (type.empty() && !dict.has("OCGs") && !dict.has("VE")))
        return group_on(oc_in);
    // An OCMD: a visibility expression, else its groups by /P.
    std::function<bool(Object const&, int)> expression = [&](Object const& e_in, int depth) -> bool {
        if (depth > 16)
            return true;
        if (e_in.is_ref() && !m_doc.resolve(e_in).is_array())
            return group_on(e_in);
        Array const& e = m_doc.resolve(e_in).as_array();
        if (e.empty())
            return true;
        std::string const op = m_doc.resolve(e[0]).as_name();
        if (op == "Not")
            return e.size() < 2 || !expression(e[1], depth + 1);
        bool const is_and = op == "And";
        bool result = is_and;
        for (std::size_t i = 1; i < e.size(); ++i) {
            bool const v = expression(e[i], depth + 1);
            result = is_and ? (result && v) : (result || v);
        }
        return result;
    };
    if (dict.has("VE"))
        return expression(dict.get("VE"), 0);
    Object const groups = m_doc.resolve(dict.get("OCGs"));
    std::vector<Object> list;
    if (groups.is_array()) {
        for (Object const& g : groups.as_array())
            list.push_back(g);
    } else if (!dict.get("OCGs").is_null()) {
        list.push_back(dict.get("OCGs"));
    }
    if (list.empty())
        return true;
    std::string const policy = dict.has("P") ? m_doc.resolve(dict.get("P")).as_name() : "AnyOn";
    std::size_t on = 0;
    for (Object const& g : list)
        on += group_on(g) ? 1 : 0;
    if (policy == "AllOn")
        return on == list.size();
    if (policy == "AnyOff")
        return on < list.size();
    if (policy == "AllOff")
        return on == 0;
    return on > 0;
}

std::shared_ptr<canvas::ClipMask const> Renderer::effective_clip() const
{
    return multiply(m_state.clip, m_state.soft_mask);
}

canvas::DrawState Renderer::draw_state(double alpha) const
{
    canvas::DrawState state;
    state.transform = m_state.ctm;
    state.global_alpha = std::clamp(alpha, 0.0, 1.0);
    state.composite = m_state.blend;
    state.clip = effective_clip();
    return state;
}

void Renderer::run(std::vector<std::uint8_t> const& content, Dict const& resources)
{
    Lexer lexer(std::span<std::uint8_t const>(content.data(), content.size()));
    Parser parser(lexer, false);
    std::vector<Object> operands;
    std::size_t const stack_floor = m_stack.size();
    std::size_t const saved_floor = m_stack_floor;
    m_stack_floor = stack_floor;
    std::size_t const marked_floor = m_marked.size();
    for (;;) {
        if ((++m_operators & 255) == 0 && cancelled())
            break;
        Token token = lexer.next();
        if (token.kind == Token::Kind::Eof)
            break;
        if (token.kind != Token::Kind::Keyword) {
            Object value = parser.parse_from(std::move(token));
            if (operands.size() < k_max_operands)
                operands.push_back(std::move(value));
            continue;
        }
        std::string const& name = token.text;
        if (name == "true" || name == "false" || name == "null") {
            operands.push_back(name == "null" ? Object {} : Object::boolean(name == "true"));
            continue;
        }
        if (name == "BI") {
            inline_image(lexer, resources);
            operands.clear();
            continue;
        }
        op(name, operands, resources);
        operands.clear();
    }
    // What the stream left open is closed with it.
    while (m_stack.size() > stack_floor) {
        m_state = std::move(m_stack.back());
        m_stack.pop_back();
    }
    m_stack_floor = saved_floor;
    while (m_marked.size() > marked_floor) {
        if (m_marked.back())
            --m_hidden;
        m_marked.pop_back();
    }
}

void Renderer::op(std::string const& name, std::vector<Object>& operands, Dict const& resources)
{
    if (m_ignore_color && !name.empty() && name.size() <= 3
        && (name == "g" || name == "G" || name == "rg" || name == "RG" || name == "k" || name == "K" || name == "cs" || name == "CS"
            || name == "sc" || name == "SC" || name == "scn" || name == "SCN"))
        return;
    auto const num = [&](std::size_t i) { return i < operands.size() ? operands[i].as_number(0) : 0.0; };
    std::size_t const n = operands.size();
    char const c0 = name.empty() ? 0 : name[0];
    switch (name.size()) {
    case 1:
        switch (c0) {
        case 'q':
            if (m_stack.size() < 256)
                m_stack.push_back(m_state);
            return;
        case 'Q':
            // Never below where this content stream began: an unbalanced Q
            // in a form must not undo the state its caller saved.
            if (m_stack.size() > m_stack_floor) {
                m_state = std::move(m_stack.back());
                m_stack.pop_back();
            }
            return;
        case 'w':
            if (n >= 1)
                m_state.line_width = std::abs(num(0));
            return;
        case 'J':
            if (n >= 1) {
                int const cap = static_cast<int>(num(0));
                m_state.cap = cap == 1 ? svg::LineCap::Round : cap == 2 ? svg::LineCap::Square : svg::LineCap::Butt;
            }
            return;
        case 'j':
            if (n >= 1) {
                int const join = static_cast<int>(num(0));
                m_state.join = join == 1 ? svg::LineJoin::Round : join == 2 ? svg::LineJoin::Bevel : svg::LineJoin::Miter;
            }
            return;
        case 'M':
            if (n >= 1)
                m_state.miter_limit = std::max(1.0, num(0));
            return;
        case 'd':
            if (n >= 2) {
                m_state.dashes.clear();
                bool any = false;
                for (Object const& v : operands[0].as_array()) {
                    m_state.dashes.push_back(static_cast<float>(std::abs(v.as_number())));
                    any = any || v.as_number() != 0;
                }
                if (!any)
                    m_state.dashes.clear();
                m_state.dash_phase = num(1);
            }
            return;
        case 'i':
            return;
        case 'm':
            if (n >= 2) {
                m_current = { static_cast<float>(num(0)), static_cast<float>(num(1)) };
                m_subpath_start = m_current;
                m_path.move_to(m_state.ctm, num(0), num(1));
            }
            return;
        case 'l':
            if (n >= 2) {
                if (!m_path.has_subpath())
                    m_path.move_to(m_state.ctm, num(0), num(1));
                m_current = { static_cast<float>(num(0)), static_cast<float>(num(1)) };
                m_path.line_to(m_state.ctm, num(0), num(1));
            }
            return;
        case 'c':
            if (n >= 6) {
                if (!m_path.has_subpath())
                    m_path.move_to(m_state.ctm, num(0), num(1));
                m_path.bezier_to(m_state.ctm, num(0), num(1), num(2), num(3), num(4), num(5));
                m_current = { static_cast<float>(num(4)), static_cast<float>(num(5)) };
            }
            return;
        case 'v':
            if (n >= 4) {
                if (!m_path.has_subpath())
                    m_path.move_to(m_state.ctm, num(0), num(1));
                m_path.bezier_to(m_state.ctm, static_cast<double>(m_current.x), static_cast<double>(m_current.y), num(0), num(1), num(2), num(3));
                m_current = { static_cast<float>(num(2)), static_cast<float>(num(3)) };
            }
            return;
        case 'y':
            if (n >= 4) {
                if (!m_path.has_subpath())
                    m_path.move_to(m_state.ctm, num(0), num(1));
                m_path.bezier_to(m_state.ctm, num(0), num(1), num(2), num(3), num(2), num(3));
                m_current = { static_cast<float>(num(2)), static_cast<float>(num(3)) };
            }
            return;
        case 'h':
            m_path.close();
            m_current = m_subpath_start;
            return;
        case 'S':
            stroke_path();
            end_path();
            return;
        case 's':
            m_path.close();
            stroke_path();
            end_path();
            return;
        case 'f':
        case 'F':
            fill_path(svg::FillRule::NonZero);
            end_path();
            return;
        case 'B':
            fill_path(svg::FillRule::NonZero);
            stroke_path();
            end_path();
            return;
        case 'b':
            m_path.close();
            fill_path(svg::FillRule::NonZero);
            stroke_path();
            end_path();
            return;
        case 'n':
            end_path();
            return;
        case 'W':
            m_pending_clip = svg::FillRule::NonZero;
            return;
        case 'G':
            m_state.stroke = PaintColor { ColorSpace::device_gray(), {}, {} };
            m_state.stroke.components[0] = static_cast<float>(num(0));
            return;
        case 'g':
            m_state.fill = PaintColor { ColorSpace::device_gray(), {}, {} };
            m_state.fill.components[0] = static_cast<float>(num(0));
            return;
        case 'K':
            m_state.stroke = PaintColor { ColorSpace::device_cmyk(), {}, {} };
            for (std::size_t i = 0; i < 4; ++i)
                m_state.stroke.components[i] = static_cast<float>(num(i));
            return;
        case 'k':
            m_state.fill = PaintColor { ColorSpace::device_cmyk(), {}, {} };
            for (std::size_t i = 0; i < 4; ++i)
                m_state.fill.components[i] = static_cast<float>(num(i));
            return;
        case '\'':
        case '"':
            text_operator(name, operands, resources);
            return;
        default:
            return;
        }
    case 2:
        if (name == "cm") {
            if (n >= 6) {
                canvas::Transform const t { num(0), num(1), num(2), num(3), num(4), num(5) };
                if (t.is_finite())
                    m_state.ctm = m_state.ctm.multiply(t);
            }
            return;
        }
        if (name == "re") {
            if (n >= 4) {
                double const x = num(0);
                double const y = num(1);
                double const w = num(2);
                double const h = num(3);
                m_path.move_to(m_state.ctm, x, y);
                m_path.line_to(m_state.ctm, x + w, y);
                m_path.line_to(m_state.ctm, x + w, y + h);
                m_path.line_to(m_state.ctm, x, y + h);
                m_path.close();
                m_current = m_subpath_start = { static_cast<float>(x), static_cast<float>(y) };
            }
            return;
        }
        if (name == "f*") {
            fill_path(svg::FillRule::EvenOdd);
            end_path();
            return;
        }
        if (name == "B*") {
            fill_path(svg::FillRule::EvenOdd);
            stroke_path();
            end_path();
            return;
        }
        if (name == "b*") {
            m_path.close();
            fill_path(svg::FillRule::EvenOdd);
            stroke_path();
            end_path();
            return;
        }
        if (name == "W*") {
            m_pending_clip = svg::FillRule::EvenOdd;
            return;
        }
        if (name == "gs") {
            if (n >= 1) {
                Dict const& states = m_doc.resolve(resources.get("ExtGState")).as_dict();
                Object const gs = m_doc.resolve(states.get(operands[0].as_name()));
                set_extgstate(gs.as_dict(), resources);
            }
            return;
        }
        if (name == "ri")
            return;
        if (name == "CS") {
            if (n >= 1)
                set_color_space(m_state.stroke, operands[0], resources);
            return;
        }
        if (name == "cs") {
            if (n >= 1)
                set_color_space(m_state.fill, operands[0], resources);
            return;
        }
        if (name == "SC") {
            set_color(m_state.stroke, operands, resources);
            return;
        }
        if (name == "sc") {
            set_color(m_state.fill, operands, resources);
            return;
        }
        if (name == "RG" || name == "rg") {
            PaintColor& color = name == "RG" ? m_state.stroke : m_state.fill;
            color = PaintColor { ColorSpace::device_rgb(), {}, {} };
            for (std::size_t i = 0; i < 3; ++i)
                color.components[i] = static_cast<float>(num(i));
            return;
        }
        if (name == "sh") {
            if (n >= 1)
                shade(operands[0].as_name(), resources);
            return;
        }
        if (name == "Do") {
            if (n >= 1)
                draw_xobject(operands[0].as_name(), resources);
            return;
        }
        if (name == "BT") {
            m_in_text = true;
            m_text_matrix = canvas::Transform {};
            m_line_matrix = canvas::Transform {};
            m_text_clip = canvas::PathBuilder {};
            return;
        }
        if (name == "ET") {
            end_text();
            m_in_text = false;
            return;
        }
        if (name == "MP" || name == "DP" || name == "BX" || name == "EX")
            return;
        if (name == "d0" || name == "d1") {
            // A Type 3 glyph's metrics; d1 makes it a shape painted with the caller's color.
            if (name == "d1" && m_type3_depth > 0)
                m_ignore_color = true;
            return;
        }
        if (name == "ID" || name == "EI")
            return;
        if (name[0] == 'T') {
            text_operator(name, operands, resources);
            return;
        }
        return;
    case 3:
        if (name == "SCN") {
            set_color(m_state.stroke, operands, resources);
            return;
        }
        if (name == "scn") {
            set_color(m_state.fill, operands, resources);
            return;
        }
        if (name == "BMC") {
            begin_marked(n >= 1 ? operands[0] : Object {}, Object {}, resources);
            return;
        }
        if (name == "BDC") {
            begin_marked(n >= 1 ? operands[0] : Object {}, n >= 2 ? operands[1] : Object {}, resources);
            return;
        }
        if (name == "EMC") {
            if (!m_marked.empty()) {
                if (m_marked.back())
                    --m_hidden;
                m_marked.pop_back();
            }
            return;
        }
        return;
    default:
        return;
    }
}

void Renderer::begin_marked(Object const& tag, Object const& properties, Dict const& resources)
{
    bool hides = false;
    if (tag.is_name("OC")) {
        Object oc = properties;
        if (properties.is_name())
            oc = m_doc.resolve(resources.get("Properties")).as_dict().get(properties.as_name());
        hides = !content_visible(oc);
    }
    if (m_marked.size() < 1024) {
        m_marked.push_back(hides);
        if (hides)
            ++m_hidden;
    }
}

void Renderer::end_path()
{
    if (!m_options.draw) {
        // Text alone: clips make no difference to what the text is.
        m_pending_clip.reset();
        m_path = canvas::PathBuilder {};
        return;
    }
    if (m_pending_clip && !m_path.path.segments.empty()) {
        m_state.clip = canvas::intersect_clip(*m_surface, m_state.clip, m_path.path, *m_pending_clip);
    } else if (m_pending_clip) {
        // A clip by an empty path clips everything.
        m_state.clip = std::make_shared<canvas::ClipMask const>(
            static_cast<std::size_t>(m_surface->width) * static_cast<std::size_t>(m_surface->height), 0);
    }
    m_pending_clip.reset();
    m_path = canvas::PathBuilder {};
}

void Renderer::fill_path(svg::FillRule rule)
{
    if (m_hidden > 0 || m_path.path.segments.empty() || !m_options.draw)
        return;
    svg::Mask const coverage = canvas::fill_coverage(*m_surface, m_path.path, rule);
    paint_coverage(coverage, m_state.fill, m_state.fill_alpha);
}

void Renderer::stroke_path()
{
    if (m_hidden > 0 || m_path.path.segments.empty() || !m_options.draw)
        return;
    svg::StrokeStyle pen;
    double const scale = m_state.ctm.scale_factor();
    // No stroke is thinner than one device pixel — a zero width is the
    // thinnest line the device can draw, and a hairline stays a line at any
    // zoom, as pdf.js, PDFium and poppler draw them.
    double width = m_state.line_width;
    if (scale > 0 && width * scale < 1)
        width = 1 / scale;
    pen.width = static_cast<float>(width);
    pen.cap = m_state.cap;
    pen.join = m_state.join;
    pen.miter_limit = static_cast<float>(m_state.miter_limit);
    pen.dashes = m_state.dashes;
    pen.dash_offset = static_cast<float>(m_state.dash_phase);
    canvas::DrawState const state = draw_state(m_state.stroke_alpha);
    svg::Mask const coverage = canvas::stroke_coverage(*m_surface, m_path.path, pen, state);
    paint_coverage(coverage, m_state.stroke, m_state.stroke_alpha);
}

void Renderer::paint_coverage(svg::Mask const& coverage, PaintColor const& color, double alpha)
{
    if (coverage.empty() || !color.space || color.space->paints_nothing())
        return;
    if (color.space->family() == ColorSpace::Family::Pattern) {
        paint_pattern(coverage, color, alpha);
        return;
    }
    canvas::Style style;
    style.color = to_color(color);
    canvas::DrawState state = draw_state(alpha);
    state.transform = canvas::Transform {};
    canvas::paint_mask(*m_surface, coverage, style, state);
}

void Renderer::composite_layer(canvas::Surface const& layer, double alpha, canvas::Composite blend,
    std::shared_ptr<canvas::ClipMask const> const& clip)
{
    canvas::DrawState state;
    state.global_alpha = std::clamp(alpha, 0.0, 1.0);
    state.composite = blend;
    state.clip = clip;
    state.smoothing = false;
    canvas::draw_image(*m_surface, layer, 0, 0, layer.width, layer.height, 0, 0, layer.width, layer.height, state);
}

void Renderer::paint_pattern(svg::Mask const& coverage, PaintColor const& color, double alpha)
{
    Object const pattern = m_doc.resolve(color.pattern);
    Dict const& dict = pattern.as_dict();
    int const type = static_cast<int>(m_doc.resolve(dict.get("PatternType")).as_int(0));
    canvas::Transform const placement = m_base.multiply(matrix_of(m_doc, dict.get("Matrix")));
    // What the pattern paints, in a layer the size of the page, kept to the coverage.
    canvas::Surface layer(m_surface->width, m_surface->height);
    if (type == 2) {
        std::shared_ptr<Shading const> const shading = parse_shading(m_doc, dict.get("Shading"), nullptr);
        if (!shading)
            return;
        paint_shading(layer, *shading, placement, mask_area(coverage), true);
    } else if (type == 1) {
        Stream const* const stream = pattern.as_stream();
        if (!stream || m_depth >= k_max_form_depth)
            return;
        int const paint_type = static_cast<int>(m_doc.resolve(dict.get("PaintType")).as_int(1));
        Array const& bbox_array = m_doc.resolve(dict.get("BBox")).as_array();
        double const xstep = std::abs(m_doc.resolve(dict.get("XStep")).as_number(0));
        double const ystep = std::abs(m_doc.resolve(dict.get("YStep")).as_number(0));
        if (bbox_array.size() != 4 || xstep <= 0 || ystep <= 0)
            return;
        double const bx0 = m_doc.resolve(bbox_array[0]).as_number();
        double const by0 = m_doc.resolve(bbox_array[1]).as_number();
        double const bx1 = m_doc.resolve(bbox_array[2]).as_number();
        double const by1 = m_doc.resolve(bbox_array[3]).as_number();
        // One tile of xstep by ystep in pattern space, at device resolution.
        double const sx_dev = std::hypot(placement.a, placement.b);
        double const sy_dev = std::hypot(placement.c, placement.d);
        int tile_w = std::clamp(static_cast<int>(std::ceil(xstep * sx_dev)), 1, 4096);
        int tile_h = std::clamp(static_cast<int>(std::ceil(ystep * sy_dev)), 1, 4096);
        double const sx = tile_w / xstep;
        double const sy = tile_h / ystep;
        canvas::Surface tile(tile_w, tile_h);
        {
            Renderer cell(m_doc, tile, m_options);
            cell.m_depth = m_depth + 1;
            cell.m_forms = m_forms;
            cell.m_off = m_off;
            cell.m_have_oc = m_have_oc;
            canvas::Transform const tile_ctm { sx, 0, 0, -sy, 0, static_cast<double>(tile_h) };
            std::vector<std::uint8_t> const content = m_doc.decode(*stream);
            Dict const& pattern_resources = m_doc.resolve(dict.get("Resources")).as_dict();
            // The cell and its neighbours, so a cell that reaches past its step wraps.
            for (int j = -1; j <= 1; ++j) {
                for (int i = -1; i <= 1; ++i) {
                    canvas::Transform const shifted = tile_ctm.multiply(canvas::Transform { 1, 0, 0, 1, i * xstep, j * ystep });
                    cell.set_initial_ctm(shifted);
                    cell.m_state.clip.reset();
                    canvas::PathBuilder box;
                    box.rect(shifted, bx0, by0, bx1 - bx0, by1 - by0);
                    cell.m_state.clip = canvas::intersect_clip(tile, nullptr, box.path, svg::FillRule::NonZero);
                    if (paint_type == 2) {
                        // Uncolored: the cell's own colors are ignored, the pattern's color paints its shapes.
                        cell.m_state.fill.space = ColorSpace::device_gray();
                        cell.m_state.stroke.space = ColorSpace::device_gray();
                    }
                    cell.run(content, pattern_resources);
                }
            }
        }
        if (paint_type == 2) {
            PaintColor base;
            base.space = color.space->pattern_base();
            base.components = color.components;
            Color const c = base.space ? to_color(base) : Color::rgb(0, 0, 0);
            for (std::size_t i = 0; i + 3 < tile.pixels.size(); i += 4) {
                unsigned const a = tile.pixels[i + 3];
                tile.pixels[i] = static_cast<std::uint8_t>((c.r * a + 127) / 255);
                tile.pixels[i + 1] = static_cast<std::uint8_t>((c.g * a + 127) / 255);
                tile.pixels[i + 2] = static_cast<std::uint8_t>((c.b * a + 127) / 255);
            }
        }
        auto canvas_pattern = std::make_shared<canvas::Pattern>();
        canvas_pattern->image = std::make_shared<canvas::Surface const>(std::move(tile));
        canvas_pattern->repeat_x = true;
        canvas_pattern->repeat_y = true;
        canvas_pattern->transform = placement.multiply(canvas::Transform { 1 / sx, 0, 0, -1 / sy, 0, ystep });
        canvas::Style style;
        style.pattern = canvas_pattern;
        canvas::DrawState state;
        state.smoothing = false;
        canvas::paint_mask(layer, coverage, style, state);
        composite_layer(layer, alpha, m_state.blend, effective_clip());
        return;
    } else {
        return;
    }
    // The shading layer through the coverage.
    auto mask = std::make_shared<canvas::ClipMask>(static_cast<std::size_t>(m_surface->width) * static_cast<std::size_t>(m_surface->height), 0);
    for (int y = std::max(0, coverage.top); y < std::min(m_surface->height, coverage.top + coverage.height); ++y) {
        for (int x = std::max(0, coverage.left); x < std::min(m_surface->width, coverage.left + coverage.width); ++x)
            (*mask)[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_surface->width) + static_cast<std::size_t>(x)] = coverage.at(x, y);
    }
    composite_layer(layer, alpha, m_state.blend, multiply(mask, effective_clip()));
}

void Renderer::shade(std::string const& name, Dict const& resources)
{
    if (m_hidden > 0 || !m_options.draw)
        return;
    Dict const& shadings = m_doc.resolve(resources.get("Shading")).as_dict();
    std::shared_ptr<Shading const> const shading = parse_shading(m_doc, shadings.get(name), &resources);
    if (!shading)
        return;
    canvas::Surface layer(m_surface->width, m_surface->height);
    paint_shading(layer, *shading, m_state.ctm, { 0, 0, m_surface->width, m_surface->height }, false);
    composite_layer(layer, m_state.fill_alpha, m_state.blend, effective_clip());
}

void Renderer::set_color_space(PaintColor& color, Object const& space, Dict const& resources)
{
    std::shared_ptr<ColorSpace const> parsed = ColorSpace::parse(m_doc, space, &resources);
    if (!parsed)
        parsed = ColorSpace::device_gray();
    color.space = parsed;
    color.components = {};
    color.pattern = Object {};
    parsed->initial_color(color.components.data());
}

void Renderer::set_color(PaintColor& color, std::vector<Object> const& operands, Dict const& resources)
{
    if (!color.space)
        color.space = ColorSpace::device_gray();
    std::size_t count = operands.size();
    if (color.space->family() == ColorSpace::Family::Pattern && count >= 1 && operands.back().is_name()) {
        Dict const& patterns = m_doc.resolve(resources.get("Pattern")).as_dict();
        color.pattern = patterns.get(operands.back().as_name());
        --count;
    }
    for (std::size_t i = 0; i < count && i < color.components.size(); ++i)
        color.components[i] = static_cast<float>(operands[i].as_number(0));
}

void Renderer::set_extgstate(Dict const& gs, Dict const& resources)
{
    for (auto const& [key, value_in] : gs.entries()) {
        Object const value = m_doc.resolve(value_in);
        if (key == "LW") {
            m_state.line_width = std::abs(value.as_number(m_state.line_width));
        } else if (key == "LC") {
            int const cap = static_cast<int>(value.as_int(0));
            m_state.cap = cap == 1 ? svg::LineCap::Round : cap == 2 ? svg::LineCap::Square : svg::LineCap::Butt;
        } else if (key == "LJ") {
            int const join = static_cast<int>(value.as_int(0));
            m_state.join = join == 1 ? svg::LineJoin::Round : join == 2 ? svg::LineJoin::Bevel : svg::LineJoin::Miter;
        } else if (key == "ML") {
            m_state.miter_limit = std::max(1.0, value.as_number(10));
        } else if (key == "D") {
            Array const& d = value.as_array();
            if (d.size() == 2) {
                m_state.dashes.clear();
                bool any = false;
                for (Object const& v : m_doc.resolve(d[0]).as_array()) {
                    m_state.dashes.push_back(static_cast<float>(std::abs(m_doc.resolve(v).as_number())));
                    any = any || m_doc.resolve(v).as_number() != 0;
                }
                if (!any)
                    m_state.dashes.clear();
                m_state.dash_phase = m_doc.resolve(d[1]).as_number();
            }
        } else if (key == "CA") {
            m_state.stroke_alpha = std::clamp(value.as_number(1), 0.0, 1.0);
        } else if (key == "ca") {
            m_state.fill_alpha = std::clamp(value.as_number(1), 0.0, 1.0);
        } else if (key == "BM") {
            if (auto const blend = blend_of(m_doc, value))
                m_state.blend = *blend;
        } else if (key == "SMask") {
            if (!m_options.draw)
                continue;
            if (value.is_name("None") || !value.is_dict())
                m_state.soft_mask.reset();
            else
                m_state.soft_mask = soft_mask_from(value.as_dict(), resources);
        } else if (key == "Font") {
            Array const& f = value.as_array();
            if (f.size() == 2)
                set_font_object(f[0], m_doc.resolve(f[1]).as_number(m_state.text.font_size));
        }
    }
}

void Renderer::render_form_into(canvas::Surface& layer, Stream const& form, Dict const& resources, canvas::Transform const& ctm)
{
    Renderer sub(m_doc, layer, m_options);
    sub.m_depth = m_depth + 1;
    sub.m_forms = m_forms;
    sub.m_off = m_off;
    sub.m_have_oc = m_have_oc;
    sub.m_fonts = m_fonts;
    canvas::Transform const placed = ctm.multiply(matrix_of(m_doc, form.dict.get("Matrix")));
    sub.set_initial_ctm(placed);
    Array const& bbox = m_doc.resolve(form.dict.get("BBox")).as_array();
    if (bbox.size() == 4) {
        double const x0 = m_doc.resolve(bbox[0]).as_number();
        double const y0 = m_doc.resolve(bbox[1]).as_number();
        double const x1 = m_doc.resolve(bbox[2]).as_number();
        double const y1 = m_doc.resolve(bbox[3]).as_number();
        canvas::PathBuilder box;
        box.rect(placed, x0, y0, x1 - x0, y1 - y0);
        sub.m_state.clip = canvas::intersect_clip(layer, nullptr, box.path, svg::FillRule::NonZero);
    }
    Dict const& form_resources = form.dict.has("Resources") ? m_doc.resolve(form.dict.get("Resources")).as_dict() : resources;
    sub.run(m_doc.decode(form), form_resources);
}

std::shared_ptr<canvas::ClipMask const> Renderer::soft_mask_from(Dict const& smask, Dict const& resources)
{
    Object const group = m_doc.resolve(smask.get("G"));
    Stream const* const form = group.as_stream();
    if (!form || m_depth >= k_max_form_depth)
        return nullptr;
    bool const luminosity = m_doc.resolve(smask.get("S")).as_name() != "Alpha";
    canvas::Surface layer(m_surface->width, m_surface->height);
    // A luminosity mask starts from its backdrop color (black when none is given).
    std::array<float, 3> backdrop { 0, 0, 0 };
    if (luminosity) {
        Array const& bc = m_doc.resolve(smask.get("BC")).as_array();
        Dict const& group_dict = m_doc.resolve(form->dict.get("Group")).as_dict();
        std::shared_ptr<ColorSpace const> const space = ColorSpace::parse(m_doc, group_dict.get("CS"), &resources);
        if (space && static_cast<int>(bc.size()) == space->components()) {
            std::array<float, 32> comps {};
            for (std::size_t i = 0; i < bc.size(); ++i)
                comps[i] = static_cast<float>(m_doc.resolve(bc[i]).as_number());
            space->to_rgb(comps.data(), backdrop.data());
        }
    }
    render_form_into(layer, *form, resources, m_state.ctm);
    std::shared_ptr<Function const> const transfer = smask.has("TR") && !m_doc.resolve(smask.get("TR")).is_name("Identity")
        ? Function::parse(m_doc, smask.get("TR"))
        : nullptr;
    auto mask = std::make_shared<canvas::ClipMask>(static_cast<std::size_t>(layer.width) * static_cast<std::size_t>(layer.height));
    for (std::size_t i = 0; i < mask->size(); ++i) {
        std::uint8_t const* p = layer.pixels.data() + i * 4;
        float value = 0;
        if (luminosity) {
            float const a = p[3] / 255.0f;
            // The group over its backdrop (premultiplied over opaque), then luminosity.
            float const r = p[0] / 255.0f + backdrop[0] * (1 - a);
            float const g = p[1] / 255.0f + backdrop[1] * (1 - a);
            float const b = p[2] / 255.0f + backdrop[2] * (1 - a);
            value = 0.3f * r + 0.59f * g + 0.11f * b;
        } else {
            value = p[3] / 255.0f;
        }
        if (transfer && transfer->inputs() == 1 && transfer->outputs() >= 1) {
            float out = value;
            transfer->evaluate(&value, &out);
            value = out;
        }
        (*mask)[i] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255));
    }
    return mask;
}

void Renderer::draw_xobject(std::string const& name, Dict const& resources)
{
    if (m_hidden > 0)
        return;
    Dict const& xobjects = m_doc.resolve(resources.get("XObject")).as_dict();
    Object const& entry = xobjects.get(name);
    Object const xobject = m_doc.resolve(entry);
    Stream const* const stream = xobject.as_stream();
    if (!stream)
        return;
    if (stream->dict.has("OC") && !content_visible(stream->dict.get("OC")))
        return;
    std::string const& subtype = m_doc.resolve(stream->dict.get("Subtype")).as_name();
    if (subtype == "Image") {
        draw_image(stream->dict, m_doc.decode_image(*stream), resources);
        return;
    }
    if (subtype == "Form" || subtype.empty())
        draw_form(*stream, resources, entry.as_ref());
}

void Renderer::draw_form(Stream const& form, Dict const& resources, Ref ref)
{
    if (m_depth >= k_max_form_depth)
        return;
    if (ref.num > 0 && std::find(m_forms.begin(), m_forms.end(), ref) != m_forms.end())
        return;
    Dict const& group = m_doc.resolve(form.dict.get("Group")).as_dict();
    bool const transparency = group.get("S").is_name("Transparency");
    bool const needs_layer = transparency && m_options.draw
        && (m_state.fill_alpha < 1 || m_state.blend != canvas::Composite::SourceOver || m_state.soft_mask);
    if (needs_layer) {
        // A transparency group: drawn apart, then composited as one with the
        // state's alpha, blend mode and soft mask.
        canvas::Surface layer(m_surface->width, m_surface->height);
        m_forms.push_back(ref);
        Renderer sub(m_doc, layer, m_options);
        sub.m_depth = m_depth + 1;
        sub.m_forms = m_forms;
        sub.m_off = m_off;
        sub.m_have_oc = m_have_oc;
        sub.m_fonts = m_fonts;
        canvas::Transform const placed = m_state.ctm.multiply(matrix_of(m_doc, form.dict.get("Matrix")));
        sub.set_initial_ctm(placed);
        sub.m_state.clip = m_state.clip;
        Array const& bbox = m_doc.resolve(form.dict.get("BBox")).as_array();
        if (bbox.size() == 4) {
            double const x0 = m_doc.resolve(bbox[0]).as_number();
            double const y0 = m_doc.resolve(bbox[1]).as_number();
            canvas::PathBuilder box;
            box.rect(placed, x0, y0, m_doc.resolve(bbox[2]).as_number() - x0, m_doc.resolve(bbox[3]).as_number() - y0);
            sub.m_state.clip = canvas::intersect_clip(layer, sub.m_state.clip, box.path, svg::FillRule::NonZero);
        }
        Dict const& form_resources = form.dict.has("Resources") ? m_doc.resolve(form.dict.get("Resources")).as_dict() : resources;
        sub.run(m_doc.decode(form), form_resources);
        m_forms.pop_back();
        composite_layer(layer, m_state.fill_alpha, m_state.blend, effective_clip());
        return;
    }
    // Drawn in place: the state saved, the form's matrix and box applied.
    m_forms.push_back(ref);
    m_stack.push_back(m_state);
    canvas::Transform const saved_base = m_base;
    m_state.ctm = m_state.ctm.multiply(matrix_of(m_doc, form.dict.get("Matrix")));
    m_base = m_state.ctm;
    Array const& bbox = m_doc.resolve(form.dict.get("BBox")).as_array();
    if (bbox.size() == 4 && m_options.draw) {
        double const x0 = m_doc.resolve(bbox[0]).as_number();
        double const y0 = m_doc.resolve(bbox[1]).as_number();
        canvas::PathBuilder box;
        box.rect(m_state.ctm, x0, y0, m_doc.resolve(bbox[2]).as_number() - x0, m_doc.resolve(bbox[3]).as_number() - y0);
        m_state.clip = canvas::intersect_clip(*m_surface, m_state.clip, box.path, svg::FillRule::NonZero);
    }
    Dict const& form_resources = form.dict.has("Resources") ? m_doc.resolve(form.dict.get("Resources")).as_dict() : resources;
    ++m_depth;
    canvas::PathBuilder const saved_path = m_path;
    m_path = canvas::PathBuilder {};
    run(m_doc.decode(form), form_resources);
    m_path = saved_path;
    --m_depth;
    m_state = std::move(m_stack.back());
    m_stack.pop_back();
    m_base = saved_base;
    m_forms.pop_back();
}

void Renderer::draw_image(Dict const& dict, ImageBytes const& bytes, Dict const& resources)
{
    if (m_hidden > 0 || !m_options.draw)
        return;
    if (dict.has("OC") && !content_visible(dict.get("OC")))
        return;
    std::optional<DecodedImage> image = decode_image(m_doc, dict, bytes, &resources);
    if (!image || image->surface.width <= 0 || image->surface.height <= 0)
        return;
    int const w = image->surface.width;
    int const h = image->surface.height;
    canvas::DrawState state = draw_state(m_state.fill_alpha);
    state.transform = m_state.ctm.multiply(canvas::Transform { 1.0 / w, 0, 0, -1.0 / h, 0, 1 });
    // Smoothed when asked, or when each sample covers fewer than four device pixels.
    double const pixels_per_sample = std::max(std::hypot(state.transform.a, state.transform.b), std::hypot(state.transform.c, state.transform.d));
    state.smoothing = image->interpolate || pixels_per_sample < 4;
    if (image->stencil) {
        if (m_state.fill.space && m_state.fill.space->family() == ColorSpace::Family::Pattern) {
            // A stencil painted with a pattern: its coverage in device pixels, then the pattern through it.
            canvas::Surface coverage_layer(m_surface->width, m_surface->height);
            canvas::DrawState plain = state;
            plain.clip.reset();
            plain.global_alpha = 1;
            plain.composite = canvas::Composite::SourceOver;
            canvas::draw_image(coverage_layer, image->surface, 0, 0, w, h, 0, 0, w, h, plain);
            svg::Mask coverage;
            coverage.left = 0;
            coverage.top = 0;
            coverage.width = coverage_layer.width;
            coverage.height = coverage_layer.height;
            coverage.alpha.resize(static_cast<std::size_t>(coverage.width) * static_cast<std::size_t>(coverage.height));
            for (std::size_t i = 0; i < coverage.alpha.size(); ++i)
                coverage.alpha[i] = coverage_layer.pixels[i * 4 + 3];
            paint_pattern(coverage, m_state.fill, m_state.fill_alpha);
            return;
        }
        Color const c = to_color(m_state.fill);
        for (std::size_t i = 0; i + 3 < image->surface.pixels.size(); i += 4) {
            unsigned const a = image->surface.pixels[i + 3];
            image->surface.pixels[i] = static_cast<std::uint8_t>((c.r * a + 127) / 255);
            image->surface.pixels[i + 1] = static_cast<std::uint8_t>((c.g * a + 127) / 255);
            image->surface.pixels[i + 2] = static_cast<std::uint8_t>((c.b * a + 127) / 255);
        }
    }
    canvas::draw_image(*m_surface, image->surface, 0, 0, w, h, 0, 0, w, h, state);
}

void Renderer::inline_image(Lexer& lexer, Dict const& resources)
{
    // BI key value ... ID <data> EI
    Parser parser(lexer, false);
    Dict dict;
    for (;;) {
        Token key = lexer.next();
        if (key.kind == Token::Kind::Eof)
            return;
        if (key.kind == Token::Kind::Keyword && key.text == "ID")
            break;
        if (key.kind != Token::Kind::Name)
            continue;
        std::string const full = expand_inline_image_key(key.text);
        Object const value = parser.parse();
        dict.set(full, expand_inline_image_value(full, value));
    }
    // One whitespace byte after ID, then the data.
    std::span<std::uint8_t const> const data = lexer.data();
    std::size_t start = lexer.position();
    if (start < data.size() && is_pdf_whitespace(data[start]))
        ++start;
    // Its length: from /Length when given; for unfiltered data, from its
    // size; else up to an "EI" with whitespace on both sides.
    std::size_t end = data.size();
    Object const filter = dict.get("Filter");
    std::int64_t const length = dict.get("Length").as_int(-1);
    if (length >= 0 && start + static_cast<std::size_t>(length) <= data.size()) {
        end = start + static_cast<std::size_t>(length);
    } else if (filter.is_null()) {
        std::int64_t const w = dict.get("Width").as_int(0);
        std::int64_t const h = dict.get("Height").as_int(0);
        int bpc = static_cast<int>(dict.get("BitsPerComponent").as_int(1));
        int comps = 1;
        if (!dict.get("ImageMask").as_bool()) {
            std::shared_ptr<ColorSpace const> const space = ColorSpace::parse(m_doc, dict.get("ColorSpace"), &resources);
            comps = space ? space->components() : 1;
        } else {
            bpc = 1;
        }
        std::size_t const bytes = static_cast<std::size_t>((w * comps * bpc + 7) / 8 * h);
        if (w > 0 && h > 0 && start + bytes <= data.size())
            end = start + bytes;
    } else {
        for (std::size_t i = start; i + 1 < data.size(); ++i) {
            if (data[i] == 'E' && data[i + 1] == 'I' && i > start && is_pdf_whitespace(data[i - 1])
                && (i + 2 >= data.size() || is_pdf_whitespace(data[i + 2]) || is_pdf_delimiter(data[i + 2]))) {
                end = i - 1;
                break;
            }
        }
    }
    std::vector<std::uint8_t> raw(data.begin() + static_cast<std::ptrdiff_t>(start), data.begin() + static_cast<std::ptrdiff_t>(end));
    // Past the data to the EI.
    lexer.seek(end);
    for (;;) {
        std::size_t const before = lexer.position();
        Token t = lexer.next();
        if (t.kind == Token::Kind::Eof)
            break;
        if (t.kind == Token::Kind::Keyword && t.text == "EI")
            break;
        if (lexer.position() - before > 4096)
            break;
    }
    // Through the filters, as a stream would be.
    ImageBytes bytes;
    std::vector<std::string> filters;
    std::vector<Dict> params;
    if (filter.is_name()) {
        filters.push_back(filter.as_name());
        params.push_back(m_doc.resolve(dict.get("DecodeParms")).as_dict());
    } else {
        Array const& p = dict.get("DecodeParms").as_array();
        for (std::size_t i = 0; i < filter.as_array().size(); ++i) {
            filters.push_back(filter.as_array()[i].as_name());
            params.push_back(i < p.size() ? p[i].as_dict() : Dict {});
        }
    }
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (is_image_filter(filters[i])) {
            bytes.codec = expand_filter_name(filters[i]);
            bytes.codec_params = params[i];
            break;
        }
        raw = apply_filter(filters[i], raw, params[i]);
    }
    bytes.bytes = std::move(raw);
    draw_image(dict, bytes, resources);
}

std::vector<TextGlyph> page_glyphs(Document const& doc, std::size_t index, double scale)
{
    std::vector<TextGlyph> glyphs;
    if (index >= doc.page_count())
        return glyphs;
    RenderOptions options;
    options.scale = scale;
    options.draw = false;
    options.text_sink = [&](TextGlyph const& glyph) {
        if (glyphs.size() < 1'000'000)
            glyphs.push_back(glyph);
    };
    Page const& page = doc.page(index);
    canvas::Surface surface(1, 1); // nothing is painted
    Renderer renderer(doc, surface, options);
    renderer.set_initial_ctm(page_to_device(page, scale));
    Dict const& resources = doc.resolve(page.resources).as_dict();
    Object const contents = doc.resolve(page.dict->get("Contents"));
    std::vector<std::uint8_t> content;
    if (Stream const* stream = contents.as_stream()) {
        content = doc.decode(*stream);
    } else {
        for (Object const& part : contents.as_array()) {
            if (Stream const* part_stream = doc.resolve(part).as_stream()) {
                std::vector<std::uint8_t> const bytes = doc.decode(*part_stream);
                content.insert(content.end(), bytes.begin(), bytes.end());
                content.push_back('\n');
            }
        }
    }
    renderer.run(content, resources);
    return glyphs;
}

std::optional<Bitmap> render_page(Document const& doc, std::size_t index, RenderOptions const& options)
{
    if (index >= doc.page_count())
        return std::nullopt;
    Page const& page = doc.page(index);
    PixelSize const size = page_pixel_size(page, options.scale, options.extra_rotation);
    if (static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) > k_max_render_pixels)
        return std::nullopt;
    canvas::Surface surface(size.width, size.height);
    std::fill(surface.pixels.begin(), surface.pixels.end(), 255); // paper
    Renderer renderer(doc, surface, options);
    renderer.set_initial_ctm(page_to_device(page, options.scale, options.extra_rotation));
    Dict const& resources = doc.resolve(page.resources).as_dict();
    Object const contents = doc.resolve(page.dict->get("Contents"));
    // Several content streams are one stream, joined with whitespace.
    std::vector<std::uint8_t> content;
    if (Stream const* stream = contents.as_stream()) {
        content = doc.decode(*stream);
    } else {
        for (Object const& part : contents.as_array()) {
            if (Stream const* part_stream = doc.resolve(part).as_stream()) {
                std::vector<std::uint8_t> const bytes = doc.decode(*part_stream);
                content.insert(content.end(), bytes.begin(), bytes.end());
                content.push_back('\n');
            }
        }
    }
    renderer.run(content, resources);
    if (renderer.cancelled())
        return std::nullopt;
    if (options.annotations && options.draw) {
        // Annotations over the content, each from the page's transform; their
        // text is not the page's (a viewer's text layer leaves them out).
        RenderOptions quiet;
        quiet.scale = options.scale;
        quiet.cancel = options.cancel;
        quiet.extra_rotation = options.extra_rotation;
        canvas::Transform const page_ctm = page_to_device(page, options.scale, options.extra_rotation);
        for (Object const& annotation_ref : doc.resolve(page.dict->get("Annots")).as_array()) {
            Dict const& annotation = doc.resolve(annotation_ref).as_dict();
            if (annotation.empty())
                continue;
            Renderer annotation_renderer(doc, surface, quiet);
            annotation_renderer.draw_annotation(annotation, page_ctm, resources);
            if (annotation_renderer.cancelled())
                return std::nullopt;
        }
    }
    return surface.to_bitmap();
}

// --- Annotations (12.5) ----------------------------------------------------

namespace {

// A color operator for an annotation's /C or /IC: by its component count
// (0: transparent, nothing drawn).
std::string color_operator(Document const& doc, Object const& array_in, bool stroke)
{
    Array const& array = doc.resolve(array_in).as_array();
    std::string out;
    char buffer[64];
    for (Object const& c : array) {
        std::snprintf(buffer, sizeof buffer, "%.4f ", std::clamp(doc.resolve(c).as_number(0), 0.0, 1.0));
        out += buffer;
    }
    switch (array.size()) {
    case 1:
        return out + (stroke ? "G\n" : "g\n");
    case 3:
        return out + (stroke ? "RG\n" : "rg\n");
    case 4:
        return out + (stroke ? "K\n" : "k\n");
    default:
        return "";
    }
}

std::string number(double v)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.4f", v);
    return buffer;
}

// The border width and dash of /BS (12.5.4), as content stream operators.
std::string border_operators(Document const& doc, Dict const& annotation, double& width)
{
    Dict const& bs = doc.resolve(annotation.get("BS")).as_dict();
    width = bs.has("W") ? doc.resolve(bs.get("W")).as_number(1) : 1;
    if (width < 0)
        width = 1;
    std::string out = number(width) + " w\n";
    if (doc.resolve(bs.get("S")).as_name() == "D") {
        Array const& d = doc.resolve(bs.get("D")).as_array();
        std::string dash = "[";
        for (Object const& v : d)
            dash += number(doc.resolve(v).as_number(3)) + " ";
        out += (d.empty() ? std::string("[3] 0 d\n") : dash + "] 0 d\n");
    }
    return out;
}

// 12.5.6: what a viewer draws for a markup annotation that carries no
// appearance stream, in page space. Empty when the kind has no such
// drawing (a Text note's icon, FreeText's text: owed).
std::string synthesized_appearance(Document const& doc, Dict const& annotation, std::string const& subtype,
    double x0, double y0, double x1, double y1)
{
    std::string const stroke = color_operator(doc, annotation.get("C"), true);
    std::string const fill = color_operator(doc, annotation.get("IC"), false);
    std::string const fill_c = color_operator(doc, annotation.get("C"), false);
    double width = 1;
    std::string const border = border_operators(doc, annotation, width);
    std::string out;
    auto const points = [&](char const* key) {
        std::vector<double> v;
        for (Object const& o : doc.resolve(annotation.get(key)).as_array())
            v.push_back(doc.resolve(o).as_number(0));
        return v;
    };
    if (subtype == "Square" || subtype == "Circle") {
        // The rectangle inset by half the border, so the stroke lies within it.
        double const h = width / 2;
        double const ax = x0 + h, ay = y0 + h, bx = x1 - h, by = y1 - h;
        if (bx <= ax || by <= ay)
            return "";
        out += border;
        if (subtype == "Square") {
            out += number(ax) + " " + number(ay) + " " + number(bx - ax) + " " + number(by - ay) + " re\n";
        } else {
            double const k = 0.5523;
            double const cx = (ax + bx) / 2, cy = (ay + by) / 2, rx = (bx - ax) / 2, ry = (by - ay) / 2;
            out += number(cx + rx) + " " + number(cy) + " m\n";
            out += number(cx + rx) + " " + number(cy + ry * k) + " " + number(cx + rx * k) + " " + number(cy + ry) + " " + number(cx) + " " + number(cy + ry) + " c\n";
            out += number(cx - rx * k) + " " + number(cy + ry) + " " + number(cx - rx) + " " + number(cy + ry * k) + " " + number(cx - rx) + " " + number(cy) + " c\n";
            out += number(cx - rx) + " " + number(cy - ry * k) + " " + number(cx - rx * k) + " " + number(cy - ry) + " " + number(cx) + " " + number(cy - ry) + " c\n";
            out += number(cx + rx * k) + " " + number(cy - ry) + " " + number(cx + rx) + " " + number(cy - ry * k) + " " + number(cx + rx) + " " + number(cy) + " c\n";
            out += "h\n";
        }
        bool const do_stroke = !stroke.empty() && width > 0;
        if (!fill.empty() && do_stroke)
            return fill + stroke + out + "B\n";
        if (!fill.empty())
            return fill + out + "f\n";
        if (do_stroke)
            return stroke + out + "S\n";
        return "";
    }
    if (subtype == "Line") {
        std::vector<double> const l = points("L");
        if (l.size() != 4 || stroke.empty() || width <= 0)
            return "";
        return stroke + border + number(l[0]) + " " + number(l[1]) + " m " + number(l[2]) + " " + number(l[3]) + " l S\n";
    }
    if (subtype == "Polygon" || subtype == "PolyLine") {
        std::vector<double> const v = points("Vertices");
        if (v.size() < 4 || stroke.empty() || width <= 0)
            return "";
        out = stroke + border;
        for (std::size_t i = 0; i + 1 < v.size(); i += 2)
            out += number(v[i]) + " " + number(v[i + 1]) + (i == 0 ? " m\n" : " l\n");
        return out + (subtype == "Polygon" ? "h S\n" : "S\n");
    }
    if (subtype == "Ink") {
        if (stroke.empty() || width <= 0)
            return "";
        out = stroke + border + "1 J 1 j\n";
        for (Object const& path : doc.resolve(annotation.get("InkList")).as_array()) {
            Array const& pts = doc.resolve(path).as_array();
            for (std::size_t i = 0; i + 1 < pts.size(); i += 2)
                out += number(doc.resolve(pts[i]).as_number(0)) + " " + number(doc.resolve(pts[i + 1]).as_number(0)) + (i == 0 ? " m\n" : " l\n");
            out += "S\n";
        }
        return out;
    }
    if (subtype == "Highlight" || subtype == "Underline" || subtype == "StrikeOut" || subtype == "Squiggly") {
        // QuadPoints: x1 y1 x2 y2 x3 y3 x4 y4 per quad, the upper-left and
        // upper-right first, then the lower pair (as writers write them).
        std::vector<double> const q = points("QuadPoints");
        if (q.size() < 8 || fill_c.empty())
            return "";
        for (std::size_t i = 0; i + 7 < q.size(); i += 8) {
            double const left = std::min({ q[i], q[i + 2], q[i + 4], q[i + 6] });
            double const right = std::max({ q[i], q[i + 2], q[i + 4], q[i + 6] });
            double const top = std::max({ q[i + 1], q[i + 3], q[i + 5], q[i + 7] });
            double const bottom = std::min({ q[i + 1], q[i + 3], q[i + 5], q[i + 7] });
            double const h = top - bottom;
            if (subtype == "Highlight") {
                // Multiplied onto the text beneath, as viewers show a marker.
                out += fill_c + number(left) + " " + number(bottom) + " " + number(right - left) + " " + number(h) + " re f\n";
            } else if (subtype == "Underline") {
                double const t = std::max(1.0, h / 14);
                out += fill_c + number(left) + " " + number(bottom + t) + " " + number(right - left) + " " + number(t) + " re f\n";
            } else if (subtype == "StrikeOut") {
                double const t = std::max(1.0, h / 14);
                out += fill_c + number(left) + " " + number(bottom + h / 2 - t / 2) + " " + number(right - left) + " " + number(t) + " re f\n";
            } else {
                // A zigzag along the bottom, a quarter of the height tall.
                double const step = std::max(2.0, h / 4);
                out += color_operator(doc, annotation.get("C"), true) + number(std::max(1.0, h / 14)) + " w 1 J 1 j\n";
                bool up = false;
                for (double x = left; x <= right; x += step / 2) {
                    out += number(x) + " " + number(bottom + (up ? step / 2 : 0)) + (x == left ? " m\n" : " l\n");
                    up = !up;
                }
                out += "S\n";
            }
        }
        return out;
    }
    return "";
}

} // namespace

void Renderer::draw_annotation(Dict const& annotation, canvas::Transform const& page_ctm, Dict const& page_resources)
{
    std::string const subtype = m_doc.resolve(annotation.get("Subtype")).as_name();
    if (subtype == "Popup")
        return; // shown by a viewer's own window, never drawn
    int const flags = static_cast<int>(m_doc.resolve(annotation.get("F")).as_int(0));
    if ((flags & 2) || (flags & 32)) // Hidden, NoView
        return;
    if (annotation.has("OC") && !content_visible(annotation.get("OC")))
        return;
    Array const& rect = m_doc.resolve(annotation.get("Rect")).as_array();
    if (rect.size() != 4)
        return;
    double const r0 = m_doc.resolve(rect[0]).as_number(0), r1 = m_doc.resolve(rect[1]).as_number(0);
    double const r2 = m_doc.resolve(rect[2]).as_number(0), r3 = m_doc.resolve(rect[3]).as_number(0);
    double const x0 = std::min(r0, r2), y0 = std::min(r1, r3), x1 = std::max(r0, r2), y1 = std::max(r1, r3);
    if (!std::isfinite(x0 + y0 + x1 + y1))
        return;

    // 12.5.5: the normal appearance, by /AS when it is a set of states (the
    // one state there is when /AS is missing, as pdf.js takes it).
    Dict const& ap = m_doc.resolve(annotation.get("AP")).as_dict();
    Object entry = ap.get("N");
    Object normal = m_doc.resolve(entry);
    if (!normal.as_stream() && normal.is_dict()) {
        Dict const& states = normal.as_dict();
        std::string const as = m_doc.resolve(annotation.get("AS")).as_name();
        Object chosen;
        if (!as.empty())
            chosen = states.get(as);
        else if (states.entries().size() == 1)
            chosen = states.entries().begin()->second;
        entry = chosen;
        normal = m_doc.resolve(chosen);
    }
    Stream const* const form = normal.as_stream();

    m_stack.clear();
    m_forms.clear();
    m_depth = 0;
    m_path = canvas::PathBuilder {};
    // /CA (12.5.2): the annotation's opacity.
    double const ca = annotation.has("CA") ? std::clamp(m_doc.resolve(annotation.get("CA")).as_number(1), 0.0, 1.0) : 1;
    m_state.fill_alpha = ca;
    m_state.stroke_alpha = ca;

    if (!form) {
        if (ap.has("N"))
            return; // an appearance that cannot be read: nothing, not a stand-in
        std::string const content = synthesized_appearance(m_doc, annotation, subtype, x0, y0, x1, y1);
        if (content.empty())
            return;
        if (subtype == "Highlight")
            m_state.blend = canvas::Composite::Multiply;
        set_initial_ctm(page_ctm);
        run(std::vector<std::uint8_t>(content.begin(), content.end()), page_resources);
        return;
    }
    // Algorithm 8.1: the form's box through its matrix, then fitted to the
    // rectangle; the form's own matrix and box apply as draw_form does them.
    canvas::Transform const matrix = matrix_of(m_doc, form->dict.get("Matrix"));
    Array const& bbox = m_doc.resolve(form->dict.get("BBox")).as_array();
    if (bbox.size() != 4)
        return;
    double const bx0 = m_doc.resolve(bbox[0]).as_number(0), by0 = m_doc.resolve(bbox[1]).as_number(0);
    double const bx1 = m_doc.resolve(bbox[2]).as_number(0), by1 = m_doc.resolve(bbox[3]).as_number(0);
    double tx0 = 1e300, ty0 = 1e300, tx1 = -1e300, ty1 = -1e300;
    for (auto const& corner : { std::pair { bx0, by0 }, std::pair { bx1, by0 }, std::pair { bx1, by1 }, std::pair { bx0, by1 } }) {
        // The transform in double: a point's floats would move a corner.
        double const px = matrix.a * corner.first + matrix.c * corner.second + matrix.e;
        double const py = matrix.b * corner.first + matrix.d * corner.second + matrix.f;
        tx0 = std::min(tx0, px);
        ty0 = std::min(ty0, py);
        tx1 = std::max(tx1, px);
        ty1 = std::max(ty1, py);
    }
    if (!std::isfinite(tx0 + ty0 + tx1 + ty1))
        return;
    double const sx = tx1 - tx0 > 1e-9 ? (x1 - x0) / (tx1 - tx0) : 1;
    double const sy = ty1 - ty0 > 1e-9 ? (y1 - y0) / (ty1 - ty0) : 1;
    canvas::Transform const fit { sx, 0, 0, sy, x0 - tx0 * sx, y0 - ty0 * sy };
    set_initial_ctm(page_ctm.multiply(fit));
    draw_form(*form, page_resources, entry.is_ref() ? entry.as_ref() : Ref {});
}

}
