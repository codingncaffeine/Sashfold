#include "Test.h"

#include "platform/Window.h"

#include <optional>
#include <string>

// The window's place as the profile's window.json keeps it between runs: what
// is written reads back the same, and a file that could not have come from a
// window — or no longer fits any screen — opens the window as a new one.

using namespace sashfold;
using platform::WindowPlacement;

int main()
{
    // A size and maximized, without a place (Wayland's), and back.
    {
        WindowPlacement placement;
        placement.width = 1432;
        placement.height = 911;
        placement.maximized = true;
        std::string const text = platform::placement_json(placement);
        CHECK(text.find("\"x\"") == std::string::npos);
        std::optional<WindowPlacement> const read = platform::placement_from_json(text);
        CHECK(read.has_value());
        if (read) {
            CHECK_EQ(read->width, 1432);
            CHECK_EQ(read->height, 911);
            CHECK(read->maximized);
            CHECK(!read->has_position);
        }
    }
    // With a place (Windows'), a negative one on a display left of the
    // primary included.
    {
        WindowPlacement placement;
        placement.width = 800;
        placement.height = 600;
        placement.has_position = true;
        placement.x = -1700;
        placement.y = 40;
        std::optional<WindowPlacement> const read = platform::placement_from_json(platform::placement_json(placement));
        CHECK(read.has_value());
        if (read) {
            CHECK(read->has_position);
            CHECK_EQ(read->x, -1700);
            CHECK_EQ(read->y, 40);
            CHECK(!read->maximized);
        }
    }
    // Not a placement: no window opens by any of these.
    CHECK(!platform::placement_from_json(""));
    CHECK(!platform::placement_from_json("[]"));
    CHECK(!platform::placement_from_json("{\"width\": 900}"));
    CHECK(!platform::placement_from_json("{\"width\": \"900\", \"height\": 700}"));
    CHECK(!platform::placement_from_json("{\"width\": 0, \"height\": 700}"));
    CHECK(!platform::placement_from_json("{\"width\": 900, \"height\": -5}"));
    CHECK(!platform::placement_from_json("{\"width\": 900.5, \"height\": 700}"));
    CHECK(!platform::placement_from_json("{\"width\": 99999, \"height\": 700}"));
    CHECK(!platform::placement_from_json("{\"width\": 1e400, \"height\": 700}"));
    // A place half given, or past any desktop, is no place: the size stands.
    {
        std::optional<WindowPlacement> const half = platform::placement_from_json("{\"width\": 900, \"height\": 700, \"x\": 10}");
        CHECK(half && !half->has_position);
        std::optional<WindowPlacement> const far = platform::placement_from_json("{\"width\": 900, \"height\": 700, \"x\": 10, \"y\": 90000}");
        CHECK(far && !far->has_position && far->width == 900);
    }
    // "maximized" that is not true or false leaves the window unmaximized.
    {
        std::optional<WindowPlacement> const read = platform::placement_from_json("{\"width\": 900, \"height\": 700, \"maximized\": 1}");
        CHECK(read && !read->maximized);
    }
    return test::report("window placement");
}
