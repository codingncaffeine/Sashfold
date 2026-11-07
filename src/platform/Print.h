#pragma once
// Printing a document through the system's own print dialog, asked on a
// thread of its own so the window never waits on it. Linux hands the PDF
// to the desktop portal (org.freedesktop.portal.Print), which shows the
// desktop's dialog and prints the file; Windows shows the print dialog
// and draws each page onto the printer; where there is neither,
// outcome() says Unavailable at once.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sashfold::platform {

struct PrintJob {
    std::string title; // the job's name, as the printer's queue shows it
    std::vector<std::uint8_t> pdf; // the document, as a PDF
    // The window the dialog belongs to, as the desktop names one
    // ("wayland:<exported handle>", "x11:<window id>", "win32:<hwnd>"), or nothing.
    std::string parent_window;
};

class PrintRequest {
public:
    enum class Outcome { Pending, Printed, Cancelled, Failed, Unavailable };
    virtual ~PrintRequest() = default;
    virtual Outcome outcome() const = 0;
};

// Opens the dialog; `wake` is called (on another thread) when the request
// has an outcome.
std::unique_ptr<PrintRequest> print_document(PrintJob job, std::function<void()> wake);

}
