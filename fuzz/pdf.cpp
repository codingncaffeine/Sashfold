// libFuzzer harness for the PDF reader's file layer: hostile bytes opened
// as a document (cross-reference tables and streams, the rebuild, object
// streams, the security handler), then every page's resources and content
// streams resolved and decoded and the page drawn small, and each filter run
// on the raw bytes alone.
// No crash, no hang, no sanitizer finding.

#include "pdf/Document.h"
#include "pdf/Filters.h"
#include "pdf/Render.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> const input(data, data + size);
    sashfold::pdf::Dict const none;
    for (char const* filter : { "FlateDecode", "LZWDecode", "ASCIIHexDecode", "ASCII85Decode", "RunLengthDecode" })
        (void)sashfold::pdf::apply_filter(filter, input, none, 1u << 20);
    sashfold::pdf::Document::Status status {};
    auto const doc = sashfold::pdf::Document::open(input, "", status);
    if (!doc)
        return 0;
    (void)doc->title();
    for (std::size_t i = 0; i < doc->page_count() && i < 16; ++i) {
        sashfold::pdf::Page const& page = doc->page(i);
        (void)doc->resolve(page.resources);
        sashfold::pdf::Object const contents = doc->resolve(page.dict->get("Contents"));
        if (sashfold::pdf::Stream const* stream = contents.as_stream())
            (void)doc->decode(*stream);
        for (sashfold::pdf::Object const& part : contents.as_array()) {
            if (sashfold::pdf::Stream const* stream = doc->resolve(part).as_stream())
                (void)doc->decode(*stream);
        }
        // Drawn small: a page box can claim any size.
        double const longest = std::max(page.crop_box.width(), page.crop_box.height());
        sashfold::pdf::RenderOptions options;
        options.scale = longest > 0 ? std::min(1.0, 64.0 / longest) : 1.0;
        (void)sashfold::pdf::render_page(*doc, i, options);
    }
    return 0;
}
