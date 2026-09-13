#pragma once

// The documents a page's frames show (the iframe element), drawn into the
// page: each <iframe>'s document — its srcdoc, or what its src names, a
// data: URL included — is parsed with scripting off, styled against the
// frame's own size, laid out and painted onto a picture exactly the size of
// the frame's content box, which the frame's fragment then carries as its
// image. A frame's own frames are drawn the same way.
//
// What a browser holds a frame to is held here. The page's policy says
// whether a frame's document is fetched at all (frame-src); the document's
// response says whether it may be shown in a frame (frame-ancestors, or
// X-Frame-Options when no enforced policy says frame-ancestors), and its
// own policy governs what it fetches, while an srcdoc or data: document
// keeps the policy of the document it is in. A page may show itself in a
// frame once, but not inside that again, as Blink and Gecko allow; frames go
// ten deep (Gecko's limit), and a page's whole tree of frames is drawn
// within a budget of frames and of pixels.
//
// A frame whose document has a realm of its own — opened by the page's realm
// with its scripts running — is drawn from that live document instead.
//
// Not drawn yet: input into a frame, scrolling inside one, a frame document
// that is not markup.

#include "bindings/Realm.h"
#include "core/Bitmap.h"
#include "layout/Layout.h"
#include "net/Csp.h"
#include "net/Filters.h"
#include "net/Http.h"
#include "net/Url.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::ui {

// What a fetch for a frame brought back: the body, the Content-Type it came
// with (empty for none), the URL it came from in the end, the response's
// headers, which a document's policy and framing rules are read from, and its
// HTTP status (200 for a document that was never fetched).
struct FrameResponse {
    std::vector<std::uint8_t> bytes;
    std::string content_type;
    net::Url url;
    std::vector<net::Header> headers;
    int status = 200;
};

// Fetches `url` for the document at `from`: a frame's document (kind
// Subdocument for an iframe or frame, Object for an object or embed) for the
// document the frame is in, whatever its status, or a stylesheet, a font or
// a picture for a frame's document — under the guard of that document's
// policy. nullopt when it cannot be had.
using FrameFetcher = std::function<std::optional<FrameResponse>(net::Url const& url, net::Url const& from,
    net::ResourceKind kind, net::RequestGuard const& guard)>;

// A frame's picture as last drawn, by element, and what it was drawn from:
// a frame showing the same thing at the same size is not drawn again.
struct DrawnFrame {
    std::string source; // the srcdoc, or the src resolved; empty for nothing to show
    int width = 0;
    int height = 0;
    std::shared_ptr<Bitmap const> bitmap;
    std::uint64_t mutations = 0; // a live frame's documents' count when it was drawn
};
using DrawnFrames = std::unordered_map<dom::Element const*, DrawnFrame>;

// Draws every frame laid out in `page` into its fragment, for the page at
// `base` under its `policy` (null for a page with none), at the page's
// device px per CSS px. A frame's document sets the process's page fonts to
// its own, so a caller that measures or paints text afterwards sets its
// page's again. With `drawn`, a frame drawn before is taken from it, and it
// is left holding this page's frames alone. With `realm`, the realm of the
// page's document, a frame whose document has a realm there is drawn from
// that live document, and its own frames from theirs.
void draw_frames(net::Url const& base, layout::LayoutResult& page, FrameFetcher const& fetch, float device_scale,
    net::ContentSecurityPolicy* policy = nullptr, DrawnFrames* drawn = nullptr, bindings::Realm* realm = nullptr);

// An iframe's document as the framing rules let it through, for a realm of
// its own: what its srcdoc or src names for the document at `base` under
// `policy` — or `target`, when the frame navigates there on its own — inside
// `ancestors` (the page first). What bindings::HostHooks::frame_document
// answers, by the rules a frame drawn without a realm is held to.
std::optional<bindings::FrameDocument> frame_document_for(dom::Element const& iframe, net::Url const& base,
    net::ContentSecurityPolicy* policy, std::vector<bindings::FrameAncestor> const& ancestors,
    std::optional<net::Url> const& target, FrameFetcher const& fetch);

}
