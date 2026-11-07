// No print path written for this system: the viewer says so.
#include "platform/Print.h"

namespace sashfold::platform {

namespace {

class NoPrint final : public PrintRequest {
public:
    Outcome outcome() const override { return Outcome::Unavailable; }
};

}

std::unique_ptr<PrintRequest> print_document(PrintJob, std::function<void()> wake)
{
    if (wake)
        wake();
    return std::make_unique<NoPrint>();
}

}
