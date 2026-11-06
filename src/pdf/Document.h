#pragma once

// A PDF file opened (ISO 32000-2 §7.5, §7.7): its cross-reference sections
// read newest first through every /Prev — tables, streams and the hybrid
// of the two — its objects parsed when first asked for (object streams
// included), and its page tree walked with the attributes pages inherit.
// A file whose cross-reference data is missing or wrong is rebuilt by
// scanning it for its objects, as every viewer does. An encrypted file is
// opened with the empty user password, or the password given.
//
// Once open a Document is read from any thread: the parsed objects are
// cached under a lock, and nothing else changes.

#include "pdf/Object.h"
#include "pdf/Security.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::pdf {

class Font;

struct Box {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
    friend bool operator==(Box const&, Box const&) = default;
};

struct Page {
    Ref ref;
    std::shared_ptr<Dict const> dict; // the page object itself
    Box media_box;
    Box crop_box; // within the media box: what is shown
    int rotate = 0; // 0, 90, 180 or 270, clockwise
    double user_unit = 1;
    Object resources; // inherited when the page has none of its own
};

// A picture stream's bytes: through every filter before the picture codec
// that ends its chain, and that codec's name and parameters (none when the
// bytes are already samples).
struct ImageBytes {
    std::vector<std::uint8_t> bytes;
    std::string codec;
    Dict codec_params;
};

class Document {
public:
    enum class Status { Ok, NotPdf, NeedsPassword, Unsupported, Damaged };

    static std::shared_ptr<Document> open(std::vector<std::uint8_t> bytes, std::string const& password, Status& status);

    // Whether the bytes are a PDF by their header (within the first 1 KB, as readers allow).
    static bool looks_like_pdf(std::vector<std::uint8_t> const& bytes);

    Object fetch(Ref ref) const;
    // The object with references followed (a chain of them too); null for a missing object.
    Object resolve(Object const& object) const;
    // The stream's data through all its filters (decrypted first); a picture
    // codec at the end is left undone — decode_image() hands it on.
    std::vector<std::uint8_t> decode(Stream const& stream) const;
    ImageBytes decode_image(Stream const& stream) const;

    Dict const& trailer() const { return m_trailer; }
    Dict const& catalog() const { return m_catalog; }
    std::size_t page_count() const { return m_pages.size(); }
    Page const& page(std::size_t index) const { return m_pages[index]; }
    std::vector<Page> const& pages() const { return m_pages; }
    // The page whose object this is; nullopt when none is.
    std::optional<std::size_t> page_index(Ref ref) const;

    // The /Info title as UTF-8, empty when there is none.
    std::string title() const;
    std::string info_text(char const* key) const;
    std::string version() const { return m_version; }
    bool repaired() const { return m_repaired; }
    bool encrypted() const { return m_security.has_value(); }
    SecurityHandler const* security() const { return m_security ? &*m_security : nullptr; }
    std::size_t object_count() const { return m_entries.size(); }

    // Strings in an object fetched from the file, decrypted with its number.
    std::string decrypt_string(Ref owner, std::string const& data) const;

    // A font dictionary (or a reference to one) loaded once per reference
    // and shared by every render (defined in Font.cpp).
    std::shared_ptr<Font const> font(Object const& font) const;

    struct Entry {
        enum class Type : std::uint8_t { Free, Offset, Compressed };
        Type type = Type::Free;
        std::uint64_t offset = 0; // Offset: the byte; Compressed: the object stream's number
        int gen = 0; // Offset: the generation; Compressed: the index in the stream
    };

private:
    Document() = default;

    bool load_xref(std::size_t start_offset);
    bool read_xref_table(std::size_t offset, Dict& trailer);
    bool read_xref_stream(std::size_t offset, Dict& trailer);
    void reconstruct();
    void set_entry(int num, Entry entry, bool replace);
    bool setup_security(std::string const& password, Status& status);
    bool load_catalog();
    void walk_pages(Object const& node, Object const& resources, Box media, std::optional<Box> crop, int rotate,
        double user_unit, int depth, std::vector<Ref>& seen);

    Object parse_at(std::size_t offset, Ref expected, bool* matched) const;
    Object fetch_uncached(Ref ref, int depth) const;
    Object fetch_depth(Ref ref, int depth) const;
    Object resolve_depth(Object const& object, int depth) const;
    Object from_object_stream(int stream_num, int index, int num, int depth) const;
    std::vector<std::uint8_t> decode_through(Stream const& stream, bool stop_at_image, std::string* codec, Dict* codec_params,
        int depth) const;

    std::shared_ptr<std::vector<std::uint8_t> const> m_bytes;
    std::size_t m_header = 0; // where %PDF- begins: offsets are counted from here in damaged files
    std::string m_version;
    std::vector<Entry> m_entries;
    Dict m_trailer;
    Dict m_catalog;
    std::vector<Page> m_pages;
    std::optional<SecurityHandler> m_security;
    Ref m_encrypt_ref { -1, 0 };
    bool m_repaired = false;
    std::map<Ref, std::size_t> m_scanned; // objects found by the rebuild's scan

    struct ObjectStream {
        std::vector<std::uint8_t> data;
        std::vector<std::pair<int, std::size_t>> offsets; // (number, offset)
    };
    mutable std::mutex m_mutex;
    mutable std::map<int, Object> m_cache;
    mutable std::map<int, std::shared_ptr<ObjectStream const>> m_object_streams;
    mutable std::mutex m_font_mutex;
    mutable std::map<Ref, std::shared_ptr<Font const>> m_fonts;
};

}
