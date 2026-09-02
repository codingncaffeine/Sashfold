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

// Relative references resolve against `base` (the document's URL); with no
// base or no fetcher, only <style> elements contribute. A sheet that cannot
// be fetched is simply absent; imports go a few levels deep and never twice.
std::vector<SheetSource> collect_stylesheets(dom::Document const& document, net::Url const* base,
    SheetFetcher const& fetch);

std::string decode_stylesheet(std::vector<std::uint8_t> const& bytes, std::string_view content_type);

// The URLs of the @import rules at the head of a sheet, as written, minus
// those whose media list a screen does not satisfy.
std::vector<std::string> import_urls(std::string_view sheet_text);

// Whether a media list (a <link media> or @import condition) applies to a
// screen: an empty list, "all", "screen", and feature-only queries do; a
// list naming only other media types does not.
bool media_list_applies(std::string_view media);

}
