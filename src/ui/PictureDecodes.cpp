#include "ui/PictureDecodes.h"

#include "core/Bmp.h"
#include "core/Gif.h"
#include "core/Jpeg.h"
#include "core/Png.h"
#include "core/Webp.h"
#include "ui/PageImages.h"

#include <chrono>
#include <cstring>

namespace sashfold::ui {

namespace {

// Tells one response's bytes from another's: eight bytes a step, mixed as
// a 64-bit multiply-xorshift. Not a defence against anything, only an
// answer to "are these the bytes that were offered" that costs a fraction
// of what decoding them does.
std::uint64_t hash_bytes(std::vector<std::uint8_t> const& bytes)
{
    std::uint64_t hash = 0x9e3779b97f4a7c15ull ^ bytes.size();
    std::size_t i = 0;
    for (; i + 8 <= bytes.size(); i += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes.data() + i, 8);
        hash = (hash ^ word) * 0xbf58476d1ce4e5b9ull;
        hash ^= hash >> 31;
    }
    for (; i < bytes.size(); ++i)
        hash = (hash ^ bytes[i]) * 0x94d049bb133111ebull;
    return hash ^ (hash >> 29);
}

}

bool is_raster_picture(std::vector<std::uint8_t> const& bytes)
{
    return looks_like_png(bytes) || looks_like_gif(bytes) || looks_like_jpeg(bytes) || looks_like_webp(bytes)
        || looks_like_bmp(bytes) || looks_like_ico(bytes);
}

void PictureDecodes::offer(std::string const& name, std::vector<std::uint8_t> const& bytes)
{
    if (!is_raster_picture(bytes))
        return;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        if (m_entries.contains(name))
            return;
    }
    Entry entry;
    entry.size = bytes.size();
    entry.hash = hash_bytes(bytes);
    auto const started = std::chrono::steady_clock::now();
    if (std::optional<Bitmap> decoded = decode_image_bytes(bytes))
        entry.decoded.image = std::make_shared<Bitmap const>(std::move(*decoded));
    entry.decoded.decode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::lock_guard<std::mutex> const lock(m_mutex);
    m_decode_ms += entry.decoded.decode_ms;
    m_entries.emplace(name, std::move(entry));
}

std::optional<PictureDecodes::Decoded> PictureDecodes::find(std::string const& name, std::vector<std::uint8_t> const& bytes) const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    auto const it = m_entries.find(name);
    if (it == m_entries.end() || it->second.size != bytes.size())
        return std::nullopt;
    Entry const& entry = it->second;
    if (entry.hash != hash_bytes(bytes))
        return std::nullopt;
    return entry.decoded;
}

std::size_t PictureDecodes::decoded() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_entries.size();
}

double PictureDecodes::decode_ms() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_decode_ms;
}

}
