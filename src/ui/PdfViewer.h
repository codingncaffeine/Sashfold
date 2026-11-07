#pragma once

// A PDF shown in the tab, as Chrome's and Firefox's viewers show one: the
// response becomes a viewer page of our own (HTML, CSS and script, drawn by
// the engine like any page), and what the viewer needs of the document it
// asks for at an address of its own scheme — sashfold-pdf://<id>/… —
// served on the loader's threads, never the page's: the document's facts
// and outline, each page drawn at a scale, each page's text runs and links,
// the file itself. The id is a random token, and only the viewer page of
// that document is answered (its own address is the PDF's).
//
// A document stays registered while a history entry holds its handle.

#include "net/Http.h"
#include "net/Url.h"
#include "pdf/Document.h"
#include "platform/Print.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::ui {

struct PdfHandle {
    std::string id; // 32 hex digits
    net::Url url; // the PDF's own address: the viewer page's
    // In a frame, an embed or an object: the top-level page, the first
    // party the frame's requests are made for.
    std::optional<net::Url> embedder;
    std::string file_name;
    std::vector<std::uint8_t> bytes;
    mutable std::mutex mutex; // over what follows: an unlock replaces the document
    std::shared_ptr<pdf::Document const> document;
    pdf::Document::Status status = pdf::Document::Status::NotPdf;
    std::unique_ptr<platform::PrintRequest> print; // the last print asked for, while it runs
};

// How a viewer's print button reaches the system's print dialog
// (platform/Print.h): the shell sets it; none set (the script runner)
// means printing is unavailable.
using PdfPrinter = std::function<std::unique_ptr<platform::PrintRequest>(platform::PrintJob)>;
void set_pdf_printer(PdfPrinter printer);

// Whether a response is a PDF to show: its type says so, or (as browsers
// sniff a file served with no type, or a generic one) its bytes begin
// like one.
bool is_pdf_response(std::string const& content_type, std::vector<std::uint8_t> const& bytes);

// Opens and registers a document; the handle keeps it registered.
// `embedder`: the top-level page, for a document shown in one of its frames.
std::shared_ptr<PdfHandle> open_pdf(std::vector<std::uint8_t> bytes, net::Url const& url, std::string file_name,
    std::optional<net::Url> embedder = std::nullopt);
std::shared_ptr<PdfHandle> find_pdf(std::string const& id);

// The viewer page for a document, and its title (the document's, else the file's name).
std::string pdf_viewer_page(PdfHandle const& handle);
std::string pdf_title(PdfHandle const& handle);

// Answers an address of the viewer's scheme for a page at `first_party`
// (the PDF's own, or the page embedding it); nullopt when the address is
// not of the scheme. Thread-safe.
std::optional<net::FetchResult> serve_pdf_resource(net::Url const& url, net::Url const& first_party, std::string const& method,
    std::vector<std::uint8_t> const& body);

inline constexpr char k_pdf_scheme[] = "sashfold-pdf";

}
