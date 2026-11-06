// The text operators (ISO 32000-2 §9.3, §9.4): the text state, text
// objects and their matrices. Showing glyphs needs the fonts of step 5c;
// until then strings move nothing and draw nothing.

#include "pdf/RenderInternal.h"

namespace sashfold::pdf {

class Font {
};

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

void Renderer::set_font(std::string const&, double size, Dict const&)
{
    m_state.text.font_size = size;
}

void Renderer::set_font_object(Object const&, double size)
{
    m_state.text.font_size = size;
}

void Renderer::show_text(std::string const&, Dict const&)
{
}

void Renderer::show_text_array(Array const& items, Dict const& resources)
{
    for (Object const& item : items) {
        if (item.is_string())
            show_text(item.as_string(), resources);
    }
}

void Renderer::end_text()
{
}

}
