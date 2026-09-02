#pragma once

// Block and inline layout: styles + DOM in, an immutable fragment tree
// out, in absolute page coordinates. Paint consumes only fragments.
//
// The architecture rules hold: layout is a pure function of
// style and constraints; fragments are the only output; physical coordinates
// live behind these types until logical ones land.

#include "css/ComputedStyle.h"
#include "css/StyleResolver.h"
#include "layout/Controls.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::text {
class FontStack;
}

namespace sashfold::layout {

struct TextRun {
    float x = 0; // absolute
    float baseline_y = 0; // absolute
    std::u32string text;
    css::ComputedStyle const* style = nullptr;
    dom::Element const* element = nullptr; // nearest element: hit-testing walks up from here
    text::FontStack const* fonts = nullptr; // the faces the style resolved to; paint draws through them
    float width = 0; // the run's advance, measured glyph by glyph
};

struct Fragment {
    dom::Element const* element = nullptr; // null for anonymous boxes
    css::ComputedStyle const* style = nullptr; // null for anonymous boxes
    // Border box, absolute page coordinates.
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    std::vector<Fragment> children;
    std::vector<TextRun> runs; // line content held by this fragment

    // A replaced element's picture, drawn scaled into this box; a null
    // bitmap reserves the space of an image that has not arrived.
    struct ImageBox {
        std::shared_ptr<Bitmap const> bitmap;
        float x = 0;
        float y = 0;
        float width = 0;
        float height = 0;
    };
    std::optional<ImageBox> image;

    // A form control drawn by the painter: its kind, box and state. The
    // text inside is this fragment's runs.
    struct ControlBox {
        ControlKind kind = ControlKind::Text;
        float x = 0;
        float y = 0;
        float width = 0;
        float height = 0;
        bool checked = false;
        bool focused = false;
        bool disabled = false;
        std::optional<float> caret_x; // the caret's page x, when focused and editable
    };
    std::optional<ControlBox> control;

    // Out of flow, placed beside or below its siblings' lines; painted
    // after the in-flow boxes at its level.
    bool floating = false;

    // Positioned (relative, absolute, fixed or sticky): painted after the
    // in-flow boxes and floats at its level, in z-index order, negative
    // ones under them.
    bool positioned = false;
    int z_index = 0;
    // A positioned box with a z-index other than auto: a stacking context,
    // painted as one unit at its level in the parent context; a positioned
    // box with z-index auto paints at level zero but its own positioned
    // descendants belong to the parent context.
    bool stacking_context = false;

    // The margin that reaches through this box's bottom edge from its last
    // in-flow child (CSS 2.1 §8.3.1): the caller joins it with the box's
    // own bottom margin. Zero when the edge lets nothing through.
    float collapsed_bottom = 0;
};

struct LayoutResult {
    Fragment root; // the html element's fragment
    float page_height = 0; // content height of the whole page
    Color canvas_background; // html/body background propagation
};

// A decoded picture and the density its source was chosen at: a 400-pixel
// picture picked for a 200 px slot has density 2, and its intrinsic size in
// CSS px is its pixel size over the density.
struct PageImage {
    std::shared_ptr<Bitmap const> bitmap;
    float density = 1;
};

// Decoded images by element, supplied by whoever fetched them.
using ImageMap = std::unordered_map<dom::Element const*, PageImage>;

// `controls` is the live state of the page's form controls (values typed,
// boxes checked, the focused one); without it the markup's defaults show.
// `viewport_height` sizes the initial containing block for absolutely and
// fixed positioned boxes; zero means the page's own height.
LayoutResult layout_document(dom::Document const& document, css::StyleMap const& styles,
    float viewport_width, ImageMap const* images = nullptr,
    ControlStates const* controls = nullptr, float viewport_height = 0);

}
