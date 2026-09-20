#pragma once

// What a document will ask for, read off its bytes before it is parsed: the
// stylesheets and the scripts its markup names. The parser waits for each of
// them where it stands, so found one at a time they arrive one at a time;
// found here, they can all be on their way before the parse begins (every
// browser's "preload scanner"). It is a light pass, not a parse: tags and
// their attributes, comments and the text of script and style passed over.
// What it misses is fetched later as it always was; what it finds that the
// page never asks for is one fetch wasted — so it names only what it is sure
// of, and leaves pictures, which srcset and <picture> choose between by the
// viewport, to be asked for once the tree stands.

#include <string>
#include <string_view>
#include <vector>

namespace sashfold::html {

struct Preload {
    enum class Kind { Stylesheet, Script };
    Kind kind = Kind::Stylesheet;
    std::string url; // as written, to be resolved against the document's base
};

struct PreloadScan {
    std::vector<Preload> resources;
    // The href of the first <base> that has one: what the URLs are relative to.
    std::string base_href;
    // The document states a Content Security Policy in a <meta>: what it
    // names must then wait to be judged by it, where the parse meets it.
    bool meta_policy = false;
};

PreloadScan scan_for_preloads(std::string_view html);

}
