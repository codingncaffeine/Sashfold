#pragma once

// The images a page shows: every <img>'s source selected for the viewport
// (srcset, sizes, a <picture>'s <source> elements — see SourceSet.h),
// resolved against the page, fetched through whatever the caller fetches
// with, decoded, and handed to layout by element with the density it was
// chosen at. Formats this engine cannot decode yet leave the element to its
// alt text. Bounded per image, and per page in decoded bytes.

#include "css/Stylesheets.h"
#include "layout/Layout.h"
#include "net/Url.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace sashfold::dom {
class Document;
}

namespace sashfold::ui {

// Fetches one image's bytes on the page's behalf; nullopt when it cannot be had.
using ImageFetcher = std::function<std::optional<std::vector<std::uint8_t>>(net::Url const&)>;

// The picture the bytes hold, told by what they begin with (PNG, GIF, JPEG,
// BMP, ICO, SVG), never by what the transport claimed; an ICO gives the
// entry nearest `icon_size` wide, or its largest for zero. Nullopt when no
// decoder here reads them. With `density` given, a small SVG is drawn at
// several times its declared size and the factor is written there: its
// pixels per CSS px, which layout divides out, so a vector picture shown
// large keeps its edges.
std::optional<Bitmap> decode_image_bytes(std::vector<std::uint8_t> const& bytes, int icon_size = 0,
    float* density = nullptr);

// How one pass over a page's pictures is bounded, for a shell that shows
// the page before every picture is in: elements already in `known` are
// left as they are, at most `budget` new sources are fetched in the pass
// (zero is no limit), and `more` is set when elements were left for a
// later pass. An element whose picture could not be had — not fetched,
// too large, or in no format decoded here — is entered with no bitmap, so
// it is never asked for again.
struct ImagePass {
    layout::ImageMap const* known = nullptr;
    std::size_t budget = 0;
    bool* more = nullptr;
    // Whether a picture's source is here to be had without waiting. One that
    // is still on its way — asked for ahead, on another thread — is left
    // for a later pass like one past the budget, and costs none of it; with
    // no such question every source is fetched where it is met.
    std::function<bool(net::Url const&)> arrived;
};

// `media` is the viewport the sources are chosen for. The map returned
// holds this pass's entries only.
layout::ImageMap collect_images(dom::Document const& document, net::Url const* base,
    ImageFetcher const& fetch, css::MediaContext const& media = {}, layout::EmbeddedStates const* embedded = nullptr,
    ImagePass const& pass = {});

// The pictures the styles name as backgrounds (their URLs already
// resolved), fetched once each and decoded, for the painter; bounded per
// pass. Those in `known` are left as they are, and the map returned holds
// the new ones only.
layout::BackgroundImages collect_background_images(css::StyleMap const& styles, ImageFetcher const& fetch,
    layout::BackgroundImages const* known = nullptr);

}
