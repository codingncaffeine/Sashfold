// No desktop chooser written for this system: the window shows its own.
#include "platform/FileChooser.h"

namespace sashfold::platform {

namespace {

class NoChooser final : public FileChooser {
public:
    std::optional<std::vector<std::string>> take_result() override { return std::nullopt; }
    bool unavailable() const override { return true; }
};

}

std::unique_ptr<FileChooser> open_file_chooser(FileChoice, std::function<void()> wake)
{
    if (wake)
        wake();
    return std::make_unique<NoChooser>();
}

}
