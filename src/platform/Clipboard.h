#pragma once

// The clipboard seam: what a copy holds, in, out, through the OS interface
// where one exists. A clipboard private to this process stands in for
// --script runs (a test never touches the user's clipboard) and on OSes
// without a backend yet.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::platform {

// What a copy puts on the clipboard: its text always, and its markup
// (text/html) when it came from a page, so a paste into a rich editor
// keeps the links, the bold and the lists as every browser does; and what a
// paste may find there besides, a picture (a screenshot), as PNG.
struct ClipboardContent {
    std::string text;
    std::string html = {}; // empty when the copy had none
    std::vector<std::uint8_t> png = {}; // empty when there is no picture
};

// The largest picture a paste takes from another program.
inline constexpr std::size_t largest_clipboard_picture = 64u * 1024u * 1024u;

// Routes every read and write to the process-private clipboard.
void use_process_clipboard(bool enabled);

bool write_clipboard(ClipboardContent const&);
// The clipboard read on the spot: what another program holds is waited for
// (up to some seconds). Tools and scripts only: the window reads with
// start_clipboard_read.
std::optional<ClipboardContent> read_clipboard();

// A read of the clipboard that never makes its caller wait for the program
// that holds it (which may be slow, or hung): started on the window's
// thread, it is answered later, and `wake` is called — from another thread —
// once it has been.
class ClipboardRead {
public:
    bool done() const;
    // What was there (nothing at all: no answer, or nothing to paste); once.
    std::optional<ClipboardContent> take();
    // A backend's answer, from any thread.
    void finish(std::optional<ClipboardContent> content);

    // What stands in when the system's clipboard has nothing (the
    // process-private one, as read_clipboard falls back to it).
    std::optional<ClipboardContent> fallback;

private:
    mutable std::mutex m_mutex;
    bool m_done = false;
    std::optional<ClipboardContent> m_content;
};
std::shared_ptr<ClipboardRead> start_clipboard_read(std::function<void()> wake);

// Whether there is anything to paste — text, markup or a picture — asked
// without reading it (for a menu's Paste row): at once on every system.
bool clipboard_offers_content();

// The text alone (the address bar, a form field).
bool write_clipboard_text(std::string const& utf8);
std::optional<std::string> read_clipboard_text();

// The OS backends (one per platform).
bool os_write_clipboard(ClipboardContent const&);
std::optional<ClipboardContent> os_read_clipboard();
// Starts a read that answers `read` later, or false where the system has no
// clipboard to read (then the process-private one is the answer).
bool os_start_clipboard_read(std::shared_ptr<ClipboardRead> const& read, std::function<void()> wake);
// Whether the system's clipboard holds something to paste; nothing where it
// cannot say.
std::optional<bool> os_clipboard_offers_content();

}
