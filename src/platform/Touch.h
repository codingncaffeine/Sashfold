#pragma once

// A finger's story, told as the events the shell already speaks. The shell
// knows a pointer and a wheel; a touchscreen gives fingers with positions.
// The first finger down is followed: lifted where it landed (within the
// slop), it was a tap — a move, a press and a release of the left button at
// that point, so links follow, controls take focus and text is placed as
// under a mouse; carried across the screen, it scrolls the content under
// the point it started from by the distance it moved, and lifting it then
// clicks nothing. A second finger while the first is down is left alone,
// and a cancel from the system ends the story with nothing more said. The
// window feeds it in device pixels and pushes what comes out.

#include "platform/Window.h"

#include <vector>

namespace sashfold::platform {

class TouchGestures {
public:
    // How far a finger may wander before it counts as moving, in device
    // pixels; a window sets it from the display's scale.
    void set_slop(int pixels) { m_slop = pixels > 0 ? pixels : 0; }
    int slop() const { return m_slop; }

    void down(int id, int x, int y, std::vector<WindowEvent>& out);
    void motion(int id, int x, int y, std::vector<WindowEvent>& out);
    void up(int id, std::vector<WindowEvent>& out);
    void cancel(std::vector<WindowEvent>& out);

    bool active() const { return m_active; }
    bool scrolling() const { return m_active && m_scrolling; }

private:
    bool m_active = false;
    bool m_scrolling = false;
    int m_id = 0;
    int m_start_x = 0;
    int m_start_y = 0;
    int m_last_x = 0;
    int m_last_y = 0;
    int m_slop = 10;
};

}
