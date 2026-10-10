#include "platform/Window.h"

#include "core/Json.h"

#include <cmath>
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

std::string placement_json(WindowPlacement const& placement)
{
    std::string out = "{\n  \"width\": " + std::to_string(placement.width) + ",\n  \"height\": " + std::to_string(placement.height)
        + ",\n  \"maximized\": " + (placement.maximized ? "true" : "false");
    if (placement.has_position)
        out += ",\n  \"x\": " + std::to_string(placement.x) + ",\n  \"y\": " + std::to_string(placement.y);
    return out + "\n}\n";
}

std::optional<WindowPlacement> placement_from_json(std::string_view text)
{
    std::optional<JsonValue> const parsed = JsonValue::parse(text);
    if (!parsed || !parsed->is_object())
        return std::nullopt;
    // A whole number in [least, most], or nothing.
    auto const number = [&](std::string_view key, double least, double most) -> std::optional<int> {
        JsonValue const* const value = parsed->get(key);
        if (!value || !value->is_number())
            return std::nullopt;
        double const n = value->as_number();
        if (!std::isfinite(n) || n != std::floor(n) || n < least || n > most)
            return std::nullopt;
        return static_cast<int>(n);
    };
    // No display is wider or taller than 16384 px; a desktop of several
    // reaches no further than Windows' 16-bit coordinates.
    std::optional<int> const width = number("width", 64, 16384);
    std::optional<int> const height = number("height", 64, 16384);
    if (!width || !height)
        return std::nullopt;
    WindowPlacement placement;
    placement.width = *width;
    placement.height = *height;
    if (JsonValue const* const maximized = parsed->get("maximized"); maximized && maximized->is_bool())
        placement.maximized = maximized->as_bool();
    std::optional<int> const x = number("x", -32768, 32767);
    std::optional<int> const y = number("y", -32768, 32767);
    if (x && y) {
        placement.has_position = true;
        placement.x = *x;
        placement.y = *y;
    }
    return placement;
}

}
