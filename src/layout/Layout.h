#pragma once

// Block and inline layout: styles + DOM in, an immutable fragment tree
// out, in absolute page coordinates. Paint consumes only fragments.
//
// The architecture rules hold: layout is a pure function of
// style and constraints; fragments are the only output; physical coordinates
// live behind these types until logical ones land.

#include "css/ComputedStyle.h"
#include "css/StyleResolver.h"

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
};

struct LayoutResult {
    Fragment root; // the html element's fragment
    float page_height = 0; // content height of the whole page
    Color canvas_background; // html/body background propagation
};

// Decoded images by element, supplied by whoever fetched them.
using ImageMap = std::unordered_map<dom::Element const*, std::shared_ptr<Bitmap const>>;

LayoutResult layout_document(dom::Document const& document, css::StyleMap const& styles,
    float viewport_width, ImageMap const* images = nullptr);

}
