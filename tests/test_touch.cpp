#include "Test.h"

#include "platform/Touch.h"

#include <vector>

// TouchGestures: a tap is a click at the point the finger landed, a drag is
// a scroll of what was under that point by the distance the finger moved,
// a second finger is ignored, and a cancel says nothing.

using namespace sashfold;
using platform::TouchGestures;
using platform::WindowEvent;
using Kind = WindowEvent::Kind;

int main()
{
    // --- A tap: move, press, release at the landing point ---------------------
    {
        TouchGestures fingers;
        std::vector<WindowEvent> out;
        fingers.down(1, 100, 200, out);
        CHECK(fingers.active());
        CHECK(out.empty()); // nothing until the finger lifts: a drag would make it a scroll
        fingers.up(1, out);
        CHECK(!fingers.active());
        CHECK_EQ(out.size(), std::size_t(3));
        CHECK(out.size() == 3 && out[0].kind == Kind::MouseMove && out[0].x == 100 && out[0].y == 200);
        CHECK(out.size() == 3 && out[1].kind == Kind::MouseDown && out[1].button == 1 && out[1].x == 100);
        CHECK(out.size() == 3 && out[2].kind == Kind::MouseUp && out[2].button == 1 && out[2].y == 200);
    }

    // --- A wobble within the slop is still a tap, at the landing point --------
    {
        TouchGestures fingers;
        fingers.set_slop(10);
        std::vector<WindowEvent> out;
        fingers.down(1, 100, 200, out);
        fingers.motion(1, 104, 197, out);
        fingers.motion(1, 110, 210, out); // exactly the slop: still within
        CHECK(out.empty());
        CHECK(!fingers.scrolling());
        fingers.up(1, out);
        CHECK_EQ(out.size(), std::size_t(3));
        CHECK(out.size() == 3 && out[1].x == 100 && out[1].y == 200);
    }

    // --- A drag scrolls the content under the landing point, and never clicks
    {
        TouchGestures fingers;
        fingers.set_slop(10);
        std::vector<WindowEvent> out;
        fingers.down(1, 300, 400, out);
        fingers.motion(1, 300, 395, out); // within the slop
        CHECK(out.empty());
        fingers.motion(1, 302, 370, out); // past it: the whole distance counts
        CHECK(fingers.scrolling());
        CHECK_EQ(out.size(), std::size_t(1));
        CHECK(out.size() == 1 && out[0].kind == Kind::Scroll && out[0].x == 300 && out[0].y == 400);
        CHECK(out.size() == 1 && out[0].scroll_x == -2 && out[0].scroll_y == 30); // the finger went up: the page goes down
        fingers.motion(1, 302, 350, out);
        CHECK_EQ(out.size(), std::size_t(2));
        CHECK(out.size() == 2 && out[1].scroll_y == 20 && out[1].scroll_x == 0);
        fingers.motion(1, 302, 350, out); // no movement: no event
        CHECK_EQ(out.size(), std::size_t(2));
        fingers.motion(1, 312, 360, out); // back down and right: negative
        CHECK(out.size() == 3 && out[2].scroll_x == -10 && out[2].scroll_y == -10);
        fingers.up(1, out);
        CHECK_EQ(out.size(), std::size_t(3)); // no click after a scroll
        CHECK(!fingers.active());
    }

    // --- A second finger is left alone; the first one's story goes on ---------
    {
        TouchGestures fingers;
        std::vector<WindowEvent> out;
        fingers.down(1, 10, 10, out);
        fingers.down(2, 500, 500, out);
        fingers.motion(2, 400, 400, out);
        CHECK(out.empty());
        fingers.up(2, out);
        CHECK(out.empty());
        CHECK(fingers.active());
        fingers.up(1, out);
        CHECK_EQ(out.size(), std::size_t(3));
        CHECK(out.size() == 3 && out[1].x == 10 && out[1].y == 10);
    }

    // --- A cancel ends the story with nothing said ------------------------------
    {
        TouchGestures fingers;
        std::vector<WindowEvent> out;
        fingers.down(1, 10, 10, out);
        fingers.cancel(out);
        CHECK(out.empty());
        CHECK(!fingers.active());
        fingers.up(1, out); // a lift after the cancel is nobody's
        CHECK(out.empty());
        // And the next finger starts afresh.
        fingers.down(3, 20, 30, out);
        fingers.up(3, out);
        CHECK_EQ(out.size(), std::size_t(3));
        CHECK(out.size() == 3 && out[0].x == 20 && out[0].y == 30);
    }

    // --- The slop scales with the display -------------------------------------
    {
        TouchGestures fingers;
        fingers.set_slop(20);
        std::vector<WindowEvent> out;
        fingers.down(1, 0, 0, out);
        fingers.motion(1, 15, 15, out);
        CHECK(out.empty());
        fingers.motion(1, 25, 0, out);
        CHECK(out.size() == 1 && out[0].scroll_x == -25);
        fingers.set_slop(-5);
        CHECK_EQ(fingers.slop(), 0);
    }

    return sashfold::test::report("touch");
}
