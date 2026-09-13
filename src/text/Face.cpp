#include "text/Face.h"

#include "core/Png.h"
#include "text/Rasterizer.h"
#include "text/SashfoldMono.h"

#include <cmath>
#include <memory>
#include <unordered_map>
#include <utility>

namespace sashfold::text {

namespace {

class BuiltinFace final : public Face {
public:
    std::string const& family() const override
    {
        static std::string const name = "Sashfold Mono";
        return name;
    }
    bool is_bold() const override { return false; }
    bool is_italic() const override { return false; }
    bool is_monospace() const override { return true; }

    std::uint32_t glyph_index(char32_t code_point) const override
    {
        return SashfoldMono::instance().has_glyph(code_point) ? code_point : 0;
    }

    FaceMetrics metrics(float size) const override
    {
        FontMetrics const mono = SashfoldMono::metrics(size);
        return FaceMetrics { mono.ascent, mono.descent, mono.line_gap, mono.x_height };
    }

    float advance(std::uint32_t, float size) const override { return SashfoldMono::advance(size); }

    void draw_glyph(Bitmap& target, std::uint32_t glyph, float x, float baseline_y, float size,
        Color color, bool bold, bool italic) const override
    {
        SashfoldMono::instance().draw_glyph(target, static_cast<char32_t>(glyph), x, baseline_y,
            size, color, bold, italic);
    }
};

struct MaskKey {
    std::uint32_t glyph;
    int size_q;
    bool embolden;
    bool oblique;
    bool operator==(MaskKey const&) const = default;
};

struct MaskKeyHash {
    std::size_t operator()(MaskKey const& key) const
    {
        std::size_t hash = key.glyph;
        hash = hash * 1315423911u ^ static_cast<std::size_t>(key.size_q);
        hash = hash * 1315423911u
            ^ (static_cast<std::size_t>(key.embolden) << 1 | static_cast<std::size_t>(key.oblique));
        return hash;
    }
};

class TrueTypeFace final : public Face {
public:
    explicit TrueTypeFace(TrueTypeFont font)
        : m_font(std::move(font))
    {
        // Fixed pitch when every mapped glyph advances alike: the first few
        // hundred glyphs are evidence enough.
        std::uint16_t const first = m_font.advance_width(m_font.glyph_index(U'i'));
        m_monospace = first != 0;
        for (char32_t c = 0x21; c <= 0x7E && m_monospace; ++c) {
            std::uint16_t const glyph = m_font.glyph_index(c);
            if (glyph != 0 && m_font.advance_width(glyph) != first)
                m_monospace = false;
        }
    }

    std::string const& family() const override { return m_font.family_name(); }
    bool is_bold() const override { return m_font.is_bold(); }
    bool is_italic() const override { return m_font.is_italic(); }
    bool is_monospace() const override { return m_monospace; }

    std::uint32_t glyph_index(char32_t code_point) const override
    {
        return m_font.glyph_index(code_point);
    }

    FaceMetrics metrics(float size) const override
    {
        float const scale = size / static_cast<float>(m_font.units_per_em());
        return FaceMetrics { m_font.ascender() * scale, -m_font.descender() * scale,
            m_font.line_gap() * scale, m_font.x_height() * scale };
    }

    float advance(std::uint32_t glyph, float size) const override
    {
        if (glyph > 0xFFFF)
            return 0;
        return m_font.advance_width(static_cast<std::uint16_t>(glyph)) * size
            / static_cast<float>(m_font.units_per_em());
    }

    float kerning(std::uint32_t left, std::uint32_t right, float size) const override
    {
        if (left > 0xFFFF || right > 0xFFFF || !m_font.has_kerning())
            return 0;
        return m_font.kerning(static_cast<std::uint16_t>(left), static_cast<std::uint16_t>(right)) * size
            / static_cast<float>(m_font.units_per_em());
    }

    void draw_glyph(Bitmap& target, std::uint32_t glyph, float x, float baseline_y, float size,
        Color color, bool bold, bool italic) const override
    {
        // A glyph placed or sized far beyond any bitmap draws nothing, and
        // the conversions to int below are undefined for such values.
        constexpr float limit = 1.0e7f;
        if (!(x > -limit && x < limit && baseline_y > -limit && baseline_y < limit && size > 0.0f && size < limit))
            return;
        int const size_q = static_cast<int>(size * 4.0f + 0.5f);
        if (glyph == 0 || glyph > 0xFFFF || size_q <= 0)
            return; // 0 is "no glyph": the caller falls back, this face draws nothing
        auto const id = static_cast<std::uint16_t>(glyph);
        // A colour glyph first: its layers in their palette colours, or its
        // picture from a bitmap strike. Neither is emboldened or slanted.
        if (m_font.has_color_layers() && draw_layers(target, id, x, baseline_y, size_q, color))
            return;
        if (m_font.has_bitmap_glyphs() && draw_picture(target, id, x, baseline_y, size))
            return;
        GlyphMask const& mask = mask_for(id, size_q, bold && !m_font.is_bold(), italic && !m_font.is_italic());
        blend_mask(target, mask, x, baseline_y, color);
    }

private:
    static void blend_mask(Bitmap& target, GlyphMask const& mask, float x, float baseline_y, Color color)
    {
        if (mask.empty())
            return;
        int const origin_x = static_cast<int>(x + 0.5f) + mask.left;
        int const origin_y = static_cast<int>(baseline_y + 0.5f) + mask.top;
        for (int py = 0; py < mask.height; ++py) {
            for (int px = 0; px < mask.width; ++px) {
                std::uint8_t const alpha = mask.alpha[static_cast<std::size_t>(py)
                        * static_cast<std::size_t>(mask.width)
                    + static_cast<std::size_t>(px)];
                if (alpha == 0)
                    continue;
                Color shaded = color;
                shaded.a = static_cast<std::uint8_t>(static_cast<int>(color.a) * alpha / 255);
                target.blend_pixel(origin_x + px, origin_y + py, shaded);
            }
        }
    }

    // COLR: the glyph's layers, first to last, each an outline in its
    // palette colour — or in the text's own where the layer asks for it.
    bool draw_layers(Bitmap& target, std::uint16_t glyph, float x, float baseline_y, int size_q, Color color) const
    {
        std::vector<TrueTypeFont::ColorLayer> const layers = m_font.color_layers(glyph);
        if (layers.empty())
            return false;
        for (TrueTypeFont::ColorLayer const& layer : layers) {
            Color const fill = layer.foreground ? color : Color::rgba(layer.red, layer.green, layer.blue, layer.alpha);
            blend_mask(target, mask_for(layer.glyph, size_q, false, false), x, baseline_y, fill);
        }
        return true;
    }

    // A bitmap strike's picture of the glyph, decoded once per size and
    // drawn scaled from the strike's ppem to the size, set where the
    // strike's metrics put it: `left` px from the pen, its bottom `bottom`
    // px above the baseline.
    struct Picture {
        Bitmap bitmap;
        std::uint16_t ppem = 0;
        int left = 0;
        int bottom = 0;
        int width = 0;
        int height = 0;
    };

    bool draw_picture(Bitmap& target, std::uint16_t glyph, float x, float baseline_y, float size) const
    {
        Picture const* const picture = picture_for(glyph, size);
        if (!picture || picture->width <= 0 || picture->height <= 0)
            return false;
        float const scale = size / static_cast<float>(picture->ppem);
        int const left = static_cast<int>(std::lround(x + static_cast<float>(picture->left) * scale));
        int const top = static_cast<int>(
            std::lround(baseline_y - static_cast<float>(picture->bottom + picture->height) * scale));
        int const width = std::max(1, static_cast<int>(std::lround(static_cast<float>(picture->width) * scale)));
        int const height = std::max(1, static_cast<int>(std::lround(static_cast<float>(picture->height) * scale)));
        target.draw_scaled(picture->bitmap, Rect { left, top, width, height });
        return true;
    }

    Picture const* picture_for(std::uint16_t glyph, float size) const
    {
        // The strike a size gets is the strike its neighbours get, so the
        // key is the size in whole px: a page's few emoji sizes decode once.
        std::uint32_t const px = static_cast<std::uint32_t>(std::min(size, 65535.0f));
        std::uint32_t const key = static_cast<std::uint32_t>(glyph) << 16 | px;
        if (auto const it = m_pictures.find(key); it != m_pictures.end())
            return it->second.get();
        std::unique_ptr<Picture> picture;
        if (std::optional<TrueTypeFont::BitmapGlyph> const data = m_font.bitmap_glyph(glyph, size)) {
            if (std::optional<Bitmap> decoded = decode_png(data->png, 4u * 1024u * 1024u)) {
                picture = std::make_unique<Picture>(Picture { std::move(*decoded), data->ppem, data->left,
                    data->bottom, data->width, data->height });
                // A strike without metrics (sbix) sizes the picture by itself.
                if (picture->width <= 0 || picture->height <= 0) {
                    picture->width = picture->bitmap.width();
                    picture->height = picture->bitmap.height();
                }
            }
        }
        auto const [it, inserted] = m_pictures.emplace(key, std::move(picture));
        (void)inserted;
        return it->second.get();
    }

    GlyphMask const& mask_for(std::uint16_t glyph, int size_q, bool embolden, bool oblique) const
    {
        MaskKey const key { glyph, size_q, embolden, oblique };
        if (auto const it = m_masks.find(key); it != m_masks.end())
            return it->second;
        GlyphMask mask;
        if (std::optional<GlyphOutline> const outline = m_font.outline(glyph))
            mask = rasterize(*outline, m_font.units_per_em(), size_q, RasterOptions { embolden, oblique });
        auto const [it, inserted] = m_masks.emplace(key, std::move(mask));
        (void)inserted;
        return it->second;
    }

    TrueTypeFont m_font;
    bool m_monospace = false;
    mutable std::unordered_map<MaskKey, GlyphMask, MaskKeyHash> m_masks;
    mutable std::unordered_map<std::uint32_t, std::unique_ptr<Picture>> m_pictures; // null: no picture at that size
};

} // namespace

void Face::draw_glyph_turned(Bitmap& target, std::uint32_t glyph, float baseline_x, float y,
    float size, Color color, bool bold, bool italic, bool clockwise) const
{
    // The glyph is drawn upright into a scratch of its own and read back a
    // quarter turn over. Two ems each way from the pen holds anything a
    // face draws at this size, overhangs and a synthesized slant included.
    int const reach = static_cast<int>(size * 2.0f) + 4;
    int const side = reach * 2;
    if (side <= 0 || side > 4096)
        return;
    // ⛔ The scratch is cleared by the read below, one written pixel at a
    // time, and not with a fill: filling with a fully transparent colour
    // composites nothing and leaves every glyph's ink behind for the next
    // one — which drew a blank space as whatever came before it.
    static thread_local Bitmap scratch(1, 1, Color::rgba(0, 0, 0, 0));
    if (scratch.width() != side || scratch.height() != side)
        scratch = Bitmap(side, side, Color::rgba(0, 0, 0, 0));
    float const pen = static_cast<float>(reach);
    draw_glyph(scratch, glyph, pen, pen, size, color, bold, italic);
    int const pen_page_x = static_cast<int>(baseline_x + 0.5f);
    int const pen_page_y = static_cast<int>(y + 0.5f);
    for (int row = 0; row < side; ++row) {
        int const above = row - reach; // how far below the baseline it sits
        for (int column = 0; column < side; ++column) {
            Color const pixel = scratch.pixel(column, row);
            if (pixel.a == 0)
                continue;
            scratch.set_pixel(column, row, Color::rgba(0, 0, 0, 0));
            int const along = column - reach; // how far along the advance
            // The quarter turn: what ran across the glyph runs down the
            // page, and what stood above its baseline stands beside the line.
            int const page_x = clockwise ? pen_page_x - above - 1 : pen_page_x + above;
            int const page_y = clockwise ? pen_page_y + along : pen_page_y - along - 1;
            target.blend_pixel(page_x, page_y, pixel);
        }
    }
}

float Face::kerning(std::uint32_t, std::uint32_t, float) const
{
    return 0; // a face without kerning tables: every pair at its advances
}

Face const& builtin_face()
{
    static BuiltinFace const face;
    return face;
}

std::unique_ptr<Face> make_truetype_face(TrueTypeFont font)
{
    return std::make_unique<TrueTypeFace>(std::move(font));
}

}
