#pragma once

// The document's author stylesheets, gathered in cascade order: <style>
// elements in place, <link rel="stylesheet"> fetched through whatever the
// caller fetches with, and each sheet's @import rules fetched ahead of it.
// Bytes decode by the CSS rules: a BOM, then the transport's charset, then
// @charset, then UTF-8.

#include "net/Url.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::text {
struct PageFont;
struct PageFontLoad;
struct PageFontWait;
using PageFontLoader = std::function<PageFontLoad(PageFontWait const&)>;
}

namespace sashfold::css {

struct SheetSource {
    SheetSource() = default;
    SheetSource(std::string sheet_text, std::optional<net::Url> sheet_url)
        : text(std::move(sheet_text))
        , url(std::move(sheet_url))
    {
    }

    std::string text; // UTF-8
    std::optional<net::Url> url; // where it came from: the base for the URLs inside it
    // css-cascade-5 §6.4.2: the cascade layer an @import put the sheet in,
    // dotted (`a.b`), an anonymous layer as a segment no page can name;
    // nothing for a sheet in no layer.
    std::optional<std::string> layer;
    // The layers the importing sheet's `@layer a, b;` statements named
    // ahead of the @import, which take their places before this sheet's.
    std::vector<std::string> layers_first;
};

struct FetchedSheet {
    std::vector<std::uint8_t> bytes;
    std::string content_type; // the Content-Type header, for its charset; may be empty
    // The bytes already decoded, when the fetcher kept the text from an
    // earlier collection of the same sheet: the collector then decodes
    // nothing and the bytes may be left empty.
    std::shared_ptr<std::string const> text;
    // The resource was asked for and is still on its way: a font's
    // collector then takes no other source for its rule and leaves the
    // rule for a later collection, once it has come.
    bool pending = false;

    FetchedSheet(std::vector<std::uint8_t> fetched_bytes, std::string fetched_content_type,
        std::shared_ptr<std::string const> decoded = nullptr)
        : bytes(std::move(fetched_bytes))
        , content_type(std::move(fetched_content_type))
        , text(std::move(decoded))
    {
    }
};

// Fetches one stylesheet on the document's behalf; nullopt when it cannot
// be had. `nonce` is the <link>'s nonce attribute, for the page's
// Content Security Policy to judge the request by; empty for an @import,
// a font, or an element without one.
using SheetFetcher = std::function<std::optional<FetchedSheet>(net::Url const&, std::string_view nonce)>;

// Whether a <style> element's text may apply (the page's Content
// Security Policy says); one refused contributes nothing.
using InlineSheetCheck = std::function<bool(dom::Element const& style, std::string_view text)>;

// What media queries are answered against: a screen of this size, with a
// fine pointer that hovers, a light color scheme, and no scripting.
struct MediaContext {
    // The viewport in the engine's px, which are device px: a CSS px is
    // one of these over device_scale.
    float width = 1024;
    float height = 768;
    // Device px per CSS px. The engine lays out in device px, so every
    // CSS length is multiplied by this as it is resolved — a query's
    // lengths, a sizes attribute's and a style's alike — and a picture's
    // own pixels count for one over it. 1 is the classic screen.
    float device_scale = 1;
};

// A <link rel=stylesheet> the collection asked for its sheet, and whether
// the sheet came: what decides the element's load or error event (HTML
// §4.6.7). A second link to a sheet already asked for has the first's answer.
struct LinkSheetOutcome {
    dom::Element const* element = nullptr;
    std::string url;
    bool loaded = false;
};

// Relative references resolve against `base` (the document's URL); with no
// base or no fetcher, only <style> elements contribute. A sheet that cannot
// be fetched is simply absent; imports go a few levels deep and never twice.
// Sheets whose media condition the context fails are left out, and so is
// a <style> the check refuses. `links`, when given, receives the outcome of
// each stylesheet link asked for.
std::vector<SheetSource> collect_stylesheets(dom::Document const& document, net::Url const* base,
    SheetFetcher const& fetch, MediaContext const& media = {}, InlineSheetCheck const& check = {},
    std::vector<LinkSheetOutcome>* links = nullptr);

std::string decode_stylesheet(std::vector<std::uint8_t> const& bytes, std::string_view content_type);

// What the object model has done to the document's sheets, as a string
// that changes whenever a script changes, disables or adopts one: part of
// the signature that says the sheets must be collected again.
std::string scripted_sheets_signature(dom::Document const& document);

// The @import rules at the head of a sheet, minus those whose media or
// supports() condition fails: each one's URL as written, the cascade layer
// it imports into (`layer` alone gives an anonymous one, said here as an
// empty name), and the layers the sheet's `@layer` statements named before
// it, in order.
struct ImportRule {
    std::string url;
    std::optional<std::string> layer;
    std::vector<std::string> layers_before;
};
std::vector<ImportRule> import_rules(std::string_view sheet_text, MediaContext const& media = {});

// The same rules' URLs alone.
std::vector<std::string> import_urls(std::string_view sheet_text, MediaContext const& media = {});

// One source of an @font-face rule: a URL as written (the caller resolves
// it) or a local() family name, with the format() hint when one is given.
struct FontFaceSource {
    std::string url; // empty for local()
    std::string local; // the local() name, when it is one
    std::string format; // lowercased, unquoted; empty when not written
};

// An @font-face rule as declared: the family, the weight, stretch and
// slant its descriptors claim (the first value of a range), the code
// points its unicode-range keeps it to (none: every one), and its
// sources in order.
struct FontFaceRule {
    std::string family;
    int weight = 400; // the range's low end; a single value is a range of one
    int weight_max = 400;
    int stretch = 100; // percent, as font-stretch; the range's low end
    int stretch_max = 100;
    bool italic = false;
    std::vector<std::pair<char32_t, char32_t>> unicode_ranges;
    std::vector<FontFaceSource> sources;
};

// The @font-face rules of a sheet — at its top level and inside the @media
// blocks the context satisfies — in order; a rule without a family or a
// source is left out.
std::vector<FontFaceRule> font_face_rules(std::string_view sheet_text, MediaContext const& media = {});

// The fonts the sheets bring along, fetched: for each @font-face rule the
// first source in a format this engine reads (TrueType, OpenType, WOFF or
// WOFF2, or unsaid and not an .eot or .svg file) that the fetcher can supply, its
// reference resolved against the sheet. Each URL is fetched once; bounded
// per page. Hand the result to text::FontManager::set_page_fonts.
std::vector<text::PageFont> collect_page_fonts(std::vector<SheetSource> const& sheets,
    SheetFetcher const& fetch, MediaContext const& media = {});

// The same with nothing fetched until text needs it: every font the sheets
// declare is returned waiting, with its readable sources in order and with
// `on_demand`, which its page is asked through the first time the layout
// gets as far as it (text::PageFontWait). `asked`, when given, names the
// fonts — each by the first of its sources — that the page has asked for
// already: those are fetched here as the first form fetches every font,
// and one of them still on its way is returned waiting and marked coming.
// The bound is on what is fetched, which the text decides, and no longer on
// how many a page may declare.
std::vector<text::PageFont> collect_page_fonts(std::vector<SheetSource> const& sheets, SheetFetcher const& fetch,
    MediaContext const& media, std::shared_ptr<text::PageFontLoader> const& on_demand,
    std::function<bool(std::string const&)> const& asked = {});

// What a fetch for a waiting font comes to, for a loader to answer with:
// the bytes as the manager takes them, hashed as a collection hashes them;
// nothing for a fetch that failed or bytes past the bound on one font.
// `address` is the source the fetch was for.
std::optional<text::PageFontLoad> loaded_page_font(std::string const& address, std::optional<FetchedSheet> got);

// A page that fetches where it stands — a render to a file, a frame's
// document, a test — and so loads a font the moment it is asked for one:
// the sources tried in order with the fetcher, as the collection tries
// them. The fetcher is used for as long as this lives and never after; the
// fonts that came by then stay.
class FontsOnDemand {
public:
    explicit FontsOnDemand(SheetFetcher fetch);
    ~FontsOnDemand();
    FontsOnDemand(FontsOnDemand const&) = delete;
    FontsOnDemand& operator=(FontsOnDemand const&) = delete;

    // The page's fonts, waiting — or all fetched now, where fonts on demand
    // are switched off (text::page_fonts_on_demand).
    std::vector<text::PageFont> collect(std::vector<SheetSource> const& sheets, MediaContext const& media = {}) const;
    void set_fetcher(SheetFetcher fetch);

private:
    std::shared_ptr<SheetFetcher> m_fetch;
    std::shared_ptr<text::PageFontLoader> m_loader;
};

// Media Queries evaluated against the context: media types (screen and
// all apply), not/only/and/or, width and height features in both plain
// and range syntax, orientation, aspect ratio, resolution, the preference
// and pointer features. An unknown feature makes its query false, as the
// specification says; an empty list is true.
bool media_query_matches(std::string_view query_list, MediaContext const& media);
struct ComponentValue;
bool media_prelude_matches(std::vector<ComponentValue> const& prelude, MediaContext const& media);

// font-stretch as a percentage: a keyword from ultra-condensed (50) to
// ultra-expanded (200), or a percentage token; nullopt for anything else.
std::optional<int> font_stretch_percent_of(ComponentValue const& value);

}
