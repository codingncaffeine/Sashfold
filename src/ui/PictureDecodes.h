#pragma once

// Pictures decoded off the page's thread, as every engine decodes them (a
// decode pool in Chromium, Gecko's DecodePool): the host offers a picture's
// bytes the moment they arrive, on the loader's thread that fetched them,
// and they are decoded there and then. When the page's pass over its
// pictures comes to that address it takes the bitmap instead of decoding —
// only when the bytes it fetched are the bytes that were offered, so a
// response the page's own request got differently is decoded afresh.
//
// Raster formats only. A vector picture is drawn, and its text with the
// fonts of the page that shows it, so it is drawn on that page's thread.
//
// Shared between the loader's threads (offer) and the page's thread (find);
// one per page, dropped with it.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::ui {

class PictureDecodes {
public:
    // Any thread: a picture's bytes under its address, decoded now when
    // they are a raster format this engine reads. The first offer of an
    // address stands.
    void offer(std::string const& name, std::vector<std::uint8_t> const& bytes);

    struct Decoded {
        // Null when the bytes are a raster format that did not decode.
        std::shared_ptr<Bitmap const> image;
        double decode_ms = 0; // on the loader's thread
    };
    // The page's thread: the picture decoded from exactly these bytes under
    // this name; nothing when none was offered, or the bytes differ.
    std::optional<Decoded> find(std::string const& name, std::vector<std::uint8_t> const& bytes) const;

    // Pictures decoded here, and what the page's thread was spared.
    std::size_t decoded() const;
    double decode_ms() const;

private:
    struct Entry {
        std::size_t size = 0;
        std::uint64_t hash = 0;
        Decoded decoded;
    };
    mutable std::mutex m_mutex;
    std::map<std::string, Entry> m_entries;
    double m_decode_ms = 0;
};

// Whether these bytes are a raster picture: what PictureDecodes decodes.
bool is_raster_picture(std::vector<std::uint8_t> const& bytes);

}
