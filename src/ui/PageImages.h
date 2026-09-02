#pragma once

// The images a page shows: every <img>'s source selected for the viewport
// (srcset, sizes, a <picture>'s <source> elements — see SourceSet.h),
// resolved against the page, fetched through whatever the caller fetches
// with, decoded, and handed to layout by element with the density it was
// chosen at. Formats this engine cannot decode yet leave the element to its
// alt text. Bounded per image and per page.

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

// `media` is the viewport the sources are chosen for.
layout::ImageMap collect_images(dom::Document const& document, net::Url const* base,
    ImageFetcher const& fetch, css::MediaContext const& media = {});

}
