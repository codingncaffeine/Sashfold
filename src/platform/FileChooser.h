#pragma once
// The desktop's own file chooser (what <input type=file> opens): asked on a
// thread of its own so the window never waits on it, its answer taken when
// it comes. Linux asks the desktop portal (org.freedesktop.portal.
// FileChooser), Windows the common item dialog; where there is neither,
// unavailable() says so and the window draws a chooser of its own.
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::platform {

// What the page's accept attribute allows, as a chooser filters by it: a
// name for the filter, and the patterns ("*.png") and types ("image/png",
// "image/*") it lets through.
struct FileFilter {
    std::string name;
    std::vector<std::string> patterns;
    std::vector<std::string> mime_types;
};

struct FileChoice {
    std::string title;
    bool multiple = false;
    bool directory = false; // webkitdirectory: a folder, its files sent
    std::vector<FileFilter> filters;
    // The window the chooser belongs to, as the portal names one
    // ("wayland:<exported handle>", "x11:<window id>"), or nothing.
    std::string parent_window;
};

class FileChooser {
public:
    virtual ~FileChooser() = default;
    // The chosen files' paths once the reader has answered: empty when they
    // cancelled; nothing while the chooser is still open.
    virtual std::optional<std::vector<std::string>> take_result() = 0;
    // No chooser could be shown (no portal, the service refused): the window
    // shows its own.
    virtual bool unavailable() const = 0;
};

// Opens the chooser; `wake` is called (on another thread) when it has
// answered or found itself unavailable.
std::unique_ptr<FileChooser> open_file_chooser(FileChoice choice, std::function<void()> wake);

// The type a File of this name has (what browsers read from its extension:
// "photo.JPG" is image/jpeg); empty for one not known, as in every engine.
std::string type_of_file(std::string const& name);
// The patterns ("*.png") of the files a MIME type or a wildcard ("image/*")
// takes, for a chooser that filters by extension.
std::vector<std::string> patterns_for_type(std::string const& type);

// A file: URI from a chooser as a path ("file:///home/a%20b" → "/home/a b");
// nothing for any other URI.
std::optional<std::string> path_of_file_uri(std::string const& uri);

}
