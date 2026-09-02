#pragma once

// The document's author stylesheets, gathered in cascade order: <style>
// elements in place, <link rel="stylesheet"> fetched through whatever the
// caller fetches with, and each sheet's @import rules fetched ahead of it.
// Bytes decode by the CSS rules: a BOM, then the transport's charset, then
// @charset, then UTF-8.

#include "net/Url.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::dom {
class Document;
}

namespace sashfold::text {
struct PageFont;
}

namespace sashfold::css {

struct SheetSource {
    std::string text; // UTF-8
    std::optional<net::Url> url; // where it came from: the base for the URLs inside it
};

struct FetchedSheet {
    std::vector<std::uint8_t> bytes;
    std::string content_type; // the Content-Type header, for its charset; may be empty
};

// Fetches one stylesheet on the document's behalf; nullopt when it cannot be had.
using SheetFetcher = std::function<std::optional<FetchedSheet>(net::Url const&)>;

// What media queries are answered against: a screen of this size, with a
// fine pointer that hovers, a light color scheme, and no scripting.
struct MediaContext {
    float width = 1024; // CSS px
    float height = 768;
};

// Relative references resolve against `base` (the document's URL); with no
// base or no fetcher, only <style> elements contribute. A sheet that cannot
// be fetched is simply absent; imports go a few levels deep and never twice.
// Sheets whose media condition the context fails are left out.
std::vector<SheetSource> collect_stylesheets(dom::Document const& document, net::Url const* base,
    SheetFetcher const& fetch, MediaContext const& media = {});

std::string decode_stylesheet(std::vector<std::uint8_t> const& bytes, std::string_view content_type);

// The URLs of the @import rules at the head of a sheet, as written, minus
// those whose media condition the context fails.
std::vector<std::string> import_urls(std::string_view sheet_text, MediaContext const& media = {});

// One source of an @font-face rule: a URL as written (the caller resolves
// it) or a local() family name, with the format() hint when one is given.
struct FontFaceSource {
    std::string url; // empty for local()
    std::string local; // the local() name, when it is one
    std::string format; // lowercased, unquoted; empty when not written
};

// An @font-face rule as declared: the family, the weight and slant its
// descriptors claim (the first value of a range), and its sources in order.
struct FontFaceRule {
    std::string family;
    int weight = 400;
    bool italic = false;
    std::vector<FontFaceSource> sources;
};

// The @font-face rules of a sheet — at its top level and inside the @media
// blocks the context satisfies — in order; a rule without a family or a
// source is left out.
std::vector<FontFaceRule> font_face_rules(std::string_view sheet_text, MediaContext const& media = {});

// The fonts the sheets bring along, fetched: for each @font-face rule the
// first source in a format this engine reads (TrueType or OpenType, or
// unsaid and not a web-font extension) that the fetcher can supply, its
// reference resolved against the sheet. Each URL is fetched once; bounded
// per page. Hand the result to text::FontManager::set_page_fonts.
std::vector<text::PageFont> collect_page_fonts(std::vector<SheetSource> const& sheets,
    SheetFetcher const& fetch, MediaContext const& media = {});

// Media Queries evaluated against the context: media types (screen and
// all apply), not/only/and/or, width and height features in both plain
// and range syntax, orientation, aspect ratio, resolution, the preference
// and pointer features. An unknown feature makes its query false, as the
// specification says; an empty list is true.
bool media_query_matches(std::string_view query_list, MediaContext const& media);
struct ComponentValue;
bool media_prelude_matches(std::vector<ComponentValue> const& prelude, MediaContext const& media);

}
