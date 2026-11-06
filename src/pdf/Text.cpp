// The text operators (ISO 32000-2 §9.3, §9.4): the text state, text
// objects and their matrices, and strings shown glyph by glyph — each
// outline through the text rendering matrix, filled, stroked, both or
// neither and gathered for a clip as the render mode says, the pen moved
// by the glyph's width, the spacings and TJ's adjustments, across or
// down; a Type 3 glyph's own content run under the font matrix.

#include "pdf/Font.h"
#include "pdf/RenderInternal.h"

#include <cmath>

namespace sashfold::pdf {

namespace {

constexpr int k_max_type3_depth = 4;

svg::Matrix matrix(canvas::Transform const& t)
{
    return svg::Matrix { static_cast<float>(t.a), static_cast<float>(t.b), static_cast<float>(t.c), static_cast<float>(t.d),
        static_cast<float>(t.e), static_cast<float>(t.f) };
}

void append(svg::Path& into, svg::Path const& more)
{
    into.segments.insert(into.segments.end(), more.segments.begin(), more.segments.end());
}

} // namespace

void Renderer::text_operator(std::string const& name, std::vector<Object>& operands, Dict const& resources)
{
    auto const num = [&](std::size_t i) { return i < operands.size() ? operands[i].as_number(0) : 0.0; };
    TextState& ts = m_state.text;
    if (name == "Tc") {
        ts.char_spacing = num(0);
    } else if (name == "Tw") {
        ts.word_spacing = num(0);
    } else if (name == "Tz") {
        ts.horizontal_scale = num(0) / 100;
    } else if (name == "TL") {
        ts.leading = num(0);
    } else if (name == "Ts") {
        ts.rise = num(0);
    } else if (name == "Tr") {
        ts.render_mode = static_cast<int>(num(0));
    } else if (name == "Tf") {
        if (operands.size() >= 2)
            set_font(operands[0].as_name(), num(1), resources);
    } else if (name == "Td" || name == "TD") {
        if (name == "TD")
            ts.leading = -num(1);
        m_line_matrix = m_line_matrix.multiply(canvas::Transform { 1, 0, 0, 1, num(0), num(1) });
        m_text_matrix = m_line_matrix;
    } else if (name == "Tm") {
        if (operands.size() >= 6) {
            canvas::Transform const t { num(0), num(1), num(2), num(3), num(4), num(5) };
            if (t.is_finite()) {
                m_line_matrix = t;
                m_text_matrix = t;
            }
        }
    } else if (name == "T*") {
        m_line_matrix = m_line_matrix.multiply(canvas::Transform { 1, 0, 0, 1, 0, -ts.leading });
        m_text_matrix = m_line_matrix;
    } else if (name == "Tj") {
        if (!operands.empty())
            show_text(operands[0].as_string(), resources);
    } else if (name == "TJ") {
        if (!operands.empty())
            show_text_array(operands[0].as_array(), resources);
    } else if (name == "'") {
        m_line_matrix = m_line_matrix.multiply(canvas::Transform { 1, 0, 0, 1, 0, -ts.leading });
        m_text_matrix = m_line_matrix;
        if (!operands.empty())
            show_text(operands.back().as_string(), resources);
    } else if (name == "\"") {
        if (operands.size() >= 3) {
            ts.word_spacing = num(0);
            ts.char_spacing = num(1);
            m_line_matrix = m_line_matrix.multiply(canvas::Transform { 1, 0, 0, 1, 0, -ts.leading });
            m_text_matrix = m_line_matrix;
            show_text(operands[2].as_string(), resources);
        }
    }
}

void Renderer::set_font(std::string const& name, double size, Dict const& resources)
{
    Dict const& fonts = m_doc.resolve(resources.get("Font")).as_dict();
    Object const* font = fonts.find(name);
    m_state.text.font_size = size;
    m_state.text.font = font ? m_doc.font(*font) : nullptr;
}

void Renderer::set_font_object(Object const& font, double size)
{
    m_state.text.font_size = size;
    m_state.text.font = m_doc.font(font);
}

void Renderer::show_text(std::string const& bytes, Dict const& resources)
{
    TextState const& ts = m_state.text;
    std::shared_ptr<Font const> const font = ts.font;
    if (!font)
        return;
    int const mode = ts.render_mode & 7;
    bool const fills = mode == 0 || mode == 2 || mode == 4 || mode == 6;
    bool const strokes = mode == 1 || mode == 2 || mode == 5 || mode == 6;
    bool const clips = mode >= 4;
    bool const draws = m_hidden == 0 && (fills || strokes || clips);
    if (clips)
        m_text_clipping = true;
    double const size = ts.font_size;
    double const scale = ts.horizontal_scale;
    // Glyph space (per unit of font size) to text space.
    canvas::Transform const text_state { size * scale, 0, 0, size, 0, ts.rise };
    svg::Path shapes; // in device pixels
    std::size_t at = 0;
    ShownGlyph glyph;
    while (font->next(bytes, at, glyph)) {
        canvas::Transform const trm = m_state.ctm.multiply(m_text_matrix).multiply(text_state);
        if (draws) {
            if (font->kind() == Font::Kind::Type3) {
                if (Stream const* procedure = font->type3_procedure(glyph); procedure && m_type3_depth < k_max_type3_depth) {
                    // The glyph's content under the font matrix, its state its own.
                    m_stack.push_back(m_state);
                    canvas::Transform const saved_base = m_base;
                    canvas::Transform const saved_text = m_text_matrix;
                    canvas::Transform const saved_line = m_line_matrix;
                    bool const saved_in_text = m_in_text;
                    canvas::PathBuilder const saved_path = m_path;
                    m_path = canvas::PathBuilder {};
                    m_state.ctm = trm.multiply(font->type3_matrix());
                    m_base = m_state.ctm;
                    ++m_type3_depth;
                    bool const saved_ignore = m_ignore_color;
                    Dict const& glyph_resources = font->type3_resources().empty() ? resources : font->type3_resources();
                    run(m_doc.decode(*procedure), glyph_resources);
                    m_ignore_color = saved_ignore;
                    --m_type3_depth;
                    m_path = saved_path;
                    m_in_text = saved_in_text;
                    m_line_matrix = saved_line;
                    m_text_matrix = saved_text;
                    m_base = saved_base;
                    m_state = std::move(m_stack.back());
                    m_stack.pop_back();
                }
            } else if (std::shared_ptr<svg::Path const> const outline = font->outline(glyph)) {
                canvas::Transform placed = trm;
                if (font->vertical())
                    placed = trm.multiply(canvas::Transform { 1, 0, 0, 1, -glyph.origin_x, -glyph.origin_y });
                append(shapes, outline->transformed(matrix(placed)));
            }
        }
        // The pen moves on.
        double const spacing = ts.char_spacing + (glyph.word_space ? ts.word_spacing : 0);
        if (font->vertical()) {
            double const ty = glyph.vertical_advance * size + spacing;
            m_text_matrix = m_text_matrix.multiply(canvas::Transform { 1, 0, 0, 1, 0, ty });
        } else {
            double const tx = (glyph.width * size + spacing) * scale;
            m_text_matrix = m_text_matrix.multiply(canvas::Transform { 1, 0, 0, 1, tx, 0 });
        }
    }
    if (shapes.segments.empty())
        return;
    if (fills) {
        svg::Mask const coverage = canvas::fill_coverage(*m_surface, shapes, svg::FillRule::NonZero);
        paint_coverage(coverage, m_state.fill, m_state.fill_alpha);
    }
    if (strokes) {
        canvas::PathBuilder saved = m_path;
        m_path.path = shapes;
        stroke_path();
        m_path = saved;
    }
    if (clips)
        append(m_text_clip.path, shapes);
}

void Renderer::show_text_array(Array const& items, Dict const& resources)
{
    TextState const& ts = m_state.text;
    bool const vertical = ts.font && ts.font->vertical();
    for (Object const& item : items) {
        if (item.is_string()) {
            show_text(item.as_string(), resources);
        } else if (item.is_number()) {
            // A number moves the pen back by thousandths of the font size.
            double const adjust = -item.as_number() / 1000 * ts.font_size;
            if (vertical)
                m_text_matrix = m_text_matrix.multiply(canvas::Transform { 1, 0, 0, 1, 0, adjust });
            else
                m_text_matrix = m_text_matrix.multiply(canvas::Transform { 1, 0, 0, 1, adjust * ts.horizontal_scale, 0 });
        }
    }
}

void Renderer::end_text()
{
    // Clipping render modes: the glyphs shown in the object become the clip
    // at its end (nothing shown in a clipping mode clips everything away).
    if (m_text_clipping) {
        m_state.clip = m_text_clip.path.segments.empty()
            ? std::make_shared<canvas::ClipMask const>(static_cast<std::size_t>(m_surface->width) * static_cast<std::size_t>(m_surface->height), 0)
            : canvas::intersect_clip(*m_surface, m_state.clip, m_text_clip.path, svg::FillRule::NonZero);
    }
    m_text_clip = canvas::PathBuilder {};
    m_text_clipping = false;
}

}
