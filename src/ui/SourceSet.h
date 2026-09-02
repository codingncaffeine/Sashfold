#pragma once

// Choosing an <img>'s picture the way the HTML standard says: the srcset
// attribute's candidates with their width and density descriptors, the
// sizes attribute's media-conditioned source size, and a <picture> parent's
// <source> elements taken in order — a source is passed over when its media
// condition fails or its type names a format this engine cannot decode.
// The chosen source carries the density its picture is drawn at, so a
// 400-pixel picture chosen for a 200 px slot is 200 px wide on the page.
// The device pixel ratio is 1 throughout.

#include "css/Stylesheets.h"
#include "net/Url.h"

#include <optional>
#include <string_view>
#include <vector>

namespace sashfold::dom {
class Element;
}

namespace sashfold::ui {

struct ImageCandidate {
    net::Url url;
    std::optional<float> width; // the w descriptor
    std::optional<float> density; // the x descriptor
};

// The candidates of a srcset attribute, resolved against `base`; a
// candidate with a malformed descriptor or an unparseable URL is dropped.
std::vector<ImageCandidate> parse_srcset(std::string_view srcset, net::Url const* base);

// The source size a sizes attribute yields in CSS px: the first entry whose
// media condition holds (or that has none); 100vw when no entry applies.
// An entry whose size is not a plain non-negative length — calc() and auto
// included — is passed over.
float parse_sizes(std::string_view sizes, css::MediaContext const& media);

struct ImageSource {
    net::Url url;
    float density = 1; // picture pixels per CSS px at this viewport
};

// The picture an <img> shows: from the first applicable <source> of its
// <picture> parent, else from its own srcset and src. nullopt when nothing
// names a picture.
std::optional<ImageSource> select_image_source(dom::Element const& img, net::Url const* base,
    css::MediaContext const& media);

// Whether a <source type> names a format this engine decodes (an empty
// type claims nothing and passes).
bool supports_image_type(std::string_view type);

}
