#include "platform/Window.h"

#include <cstddef>
#include <utility>

namespace sashfold::platform {

void keep_last_pointer_moves(std::vector<WindowEvent>& events)
{
    using Kind = WindowEvent::Kind;
    // Whether an event leaves a run of moves running on past it.
    auto const passes = [](Kind kind) {
        return kind == Kind::None || kind == Kind::Resize || kind == Kind::Scale || kind == Kind::Active || kind == Kind::Visible;
    };
    std::vector<WindowEvent> kept;
    kept.reserve(events.size());
    // Where in `kept` the run's move so far stands, while a run is running.
    std::size_t const none = static_cast<std::size_t>(-1);
    std::size_t run = none;
    for (WindowEvent& event : events) {
        if (event.kind == Kind::MouseMove) {
            if (run != none) {
                // The later move takes the earlier one's place in the order.
                kept[run] = std::move(event);
            } else {
                run = kept.size();
                kept.push_back(std::move(event));
            }
            continue;
        }
        if (!passes(event.kind))
            run = none;
        kept.push_back(std::move(event));
    }
    events = std::move(kept);
}

}
