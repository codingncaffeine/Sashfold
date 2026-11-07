#pragma once

// The page renderer's own class, shared by the files that make it: the
// operators of paths, color, pictures and XObjects (Render.cpp) and those
// of text (Text.cpp). Not for use outside src/pdf.

#include "paint/Canvas2D.h"
#include "pdf/ColorSpace.h"
#include "pdf/Document.h"
#include "pdf/Parser.h"
#include "pdf/Render.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sashfold::pdf {

class Font; // Text.cpp's

struct TextState {
    double char_spacing = 0;
    double word_spacing = 0;
    double horizontal_scale = 1; // Tz / 100
    double leading = 0;
    double rise = 0;
    int render_mode = 0;
    double font_size = 0;
    std::shared_ptr<Font const> font;
};

struct PaintColor {
    std::shared_ptr<ColorSpace const> space;
    std::array<float, 32> components {};
    Object pattern; // the pattern object when the space is Pattern
};

struct GraphicsState {
    canvas::Transform ctm;
    std::shared_ptr<canvas::ClipMask const> clip; // null: nothing clipped
    std::shared_ptr<canvas::ClipMask const> soft_mask; // null: none
    PaintColor fill;
    PaintColor stroke;
    double line_width = 1;
    svg::LineCap cap = svg::LineCap::Butt;
    svg::LineJoin join = svg::LineJoin::Miter;
    double miter_limit = 10;
    std::vector<float> dashes;
    double dash_phase = 0;
    double fill_alpha = 1;
    double stroke_alpha = 1;
    canvas::Composite blend = canvas::Composite::SourceOver;
    TextState text;
};

class Renderer {
public:
    Renderer(Document const& doc, canvas::Surface& surface, RenderOptions const& options);

    // Runs a content stream's bytes against `resources`; `base` is the
    // transform patterns in it are placed by (the page's, or the form's at its start).
    void run(std::vector<std::uint8_t> const& content, Dict const& resources);
    void set_initial_ctm(canvas::Transform const& ctm);
    bool cancelled() const;

    // Optional content: whether an OCG or OCMD is on in the default configuration.
    bool content_visible(Object const& oc) const;

    // One annotation's normal appearance drawn where its rectangle is (12.5.5),
    // or an appearance made for one of the markup kinds that has none, as
    // viewers do (12.5.6). The renderer starts from the page's transform.
    void draw_annotation(Dict const& annotation, Ref annotation_ref, canvas::Transform const& page_ctm, Dict const& page_resources);

private:
    // Operators.
    void op(std::string const& name, std::vector<Object>& operands, Dict const& resources);
    void fill_path(svg::FillRule rule);
    void stroke_path();
    void end_path();
    void set_extgstate(Dict const& gs, Dict const& resources);
    void set_color(PaintColor& color, std::vector<Object> const& operands, Dict const& resources);
    void set_color_space(PaintColor& color, Object const& space, Dict const& resources);
    void draw_xobject(std::string const& name, Dict const& resources);
    void draw_form(Stream const& form, Dict const& resources, Ref ref);
    void draw_image(Dict const& dict, ImageBytes const& bytes, Dict const& resources);
    void inline_image(Lexer& lexer, Dict const& resources);
    void shade(std::string const& name, Dict const& resources);
    void begin_marked(Object const& tag, Object const& properties, Dict const& resources);

    // Painting helpers.
    std::shared_ptr<canvas::ClipMask const> effective_clip() const;
    canvas::DrawState draw_state(double alpha) const;
    // Paints `coverage` with a color or a pattern.
    void paint_coverage(svg::Mask const& coverage, PaintColor const& color, double alpha);
    void paint_pattern(svg::Mask const& coverage, PaintColor const& color, double alpha);
    // Composites a layer (premultiplied, page-sized) through an extra mask.
    void composite_layer(canvas::Surface const& layer, double alpha, canvas::Composite blend,
        std::shared_ptr<canvas::ClipMask const> const& clip);
    std::shared_ptr<canvas::ClipMask const> soft_mask_from(Dict const& smask, Dict const& resources);
    void render_form_into(canvas::Surface& layer, Stream const& form, Dict const& resources, canvas::Transform const& ctm);

    // Text (Text.cpp).
    void text_operator(std::string const& name, std::vector<Object>& operands, Dict const& resources);
    void show_text(std::string const& bytes, Dict const& resources);
    void show_text_array(Array const& items, Dict const& resources);
    void set_font(std::string const& name, double size, Dict const& resources);
    void set_font_object(Object const& font, double size);
    void end_text();

    Document const& m_doc;
    canvas::Surface* m_surface;
    RenderOptions const& m_options;
    std::vector<GraphicsState> m_stack;
    std::size_t m_stack_floor = 0; // the depth the running content stream began at
    GraphicsState m_state;
    canvas::Transform m_base; // what patterns are placed by
    canvas::PathBuilder m_path;
    svg::Point m_current {}; // the current point in user space
    svg::Point m_subpath_start {};
    std::optional<svg::FillRule> m_pending_clip;
    // Text objects.
    canvas::Transform m_text_matrix;
    canvas::Transform m_line_matrix;
    bool m_in_text = false;
    canvas::PathBuilder m_text_clip; // glyph outlines for clipping render modes
    bool m_text_clipping = false; // a clipping mode showed text in this object
    int m_type3_depth = 0; // Type 3 glyphs being run
    bool m_ignore_color = false; // a d1 glyph: its color operators are ignored
    // Marked content: a count of open sequences that hide what they hold.
    std::vector<bool> m_marked;
    int m_hidden = 0;
    // Forms being drawn, against loops.
    std::vector<Ref> m_forms;
    int m_depth = 0;
    std::size_t m_operators = 0;
    // Optional content in the default configuration.
    std::set<Ref> m_off;
    bool m_have_oc = false;
    // Fonts loaded in this render (Text.cpp).
    std::map<Ref, std::shared_ptr<Font const>> m_fonts;
};

}
