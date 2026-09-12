#include "platform/Touch.h"

#include <cstdlib>

namespace sashfold::platform {

namespace {

WindowEvent pointer_event(WindowEvent::Kind kind, int x, int y, int button)
{
    WindowEvent event;
    event.kind = kind;
    event.x = x;
    event.y = y;
    event.button = button;
    return event;
}

}

void TouchGestures::down(int id, int x, int y, std::vector<WindowEvent>& out)
{
    static_cast<void>(out);
    if (m_active)
        return; // a second finger: the first one's story goes on
    m_active = true;
    m_scrolling = false;
    m_id = id;
    m_start_x = m_last_x = x;
    m_start_y = m_last_y = y;
}

void TouchGestures::motion(int id, int x, int y, std::vector<WindowEvent>& out)
{
    if (!m_active || id != m_id)
        return;
    if (!m_scrolling) {
        if (std::abs(x - m_start_x) <= m_slop && std::abs(y - m_start_y) <= m_slop)
            return; // still a tap in the making
        m_scrolling = true;
        // The distance already covered counts from where the finger landed.
        m_last_x = m_start_x;
        m_last_y = m_start_y;
    }
    // The content follows the finger: a finger going up brings what is
    // below into view, which is a positive scroll.
    WindowEvent event;
    event.kind = WindowEvent::Kind::Scroll;
    event.x = m_start_x;
    event.y = m_start_y;
    event.scroll_x = m_last_x - x;
    event.scroll_y = m_last_y - y;
    m_last_x = x;
    m_last_y = y;
    if (event.scroll_x != 0 || event.scroll_y != 0)
        out.push_back(event);
}

void TouchGestures::up(int id, std::vector<WindowEvent>& out)
{
    if (!m_active || id != m_id)
        return;
    m_active = false;
    if (m_scrolling) {
        m_scrolling = false;
        return;
    }
    out.push_back(pointer_event(WindowEvent::Kind::MouseMove, m_start_x, m_start_y, 0));
    out.push_back(pointer_event(WindowEvent::Kind::MouseDown, m_start_x, m_start_y, 1));
    out.push_back(pointer_event(WindowEvent::Kind::MouseUp, m_start_x, m_start_y, 1));
}

void TouchGestures::cancel(std::vector<WindowEvent>& out)
{
    static_cast<void>(out);
    m_active = false;
    m_scrolling = false;
}

}
