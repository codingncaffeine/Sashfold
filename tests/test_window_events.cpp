#include "Test.h"

#include "platform/Window.h"

#include <string>
#include <vector>

// A batch of window events with its runs of pointer moves reduced to the
// last of each: what the window's loop acts on.

using namespace sashfold;
using platform::WindowEvent;

namespace {

WindowEvent move_to(int x, int y)
{
    WindowEvent event;
    event.kind = WindowEvent::Kind::MouseMove;
    event.x = x;
    event.y = y;
    return event;
}

WindowEvent of(WindowEvent::Kind kind, int x = 0, int y = 0)
{
    WindowEvent event;
    event.kind = kind;
    event.x = x;
    event.y = y;
    return event;
}

// "M10,20 D30,40 …": a letter for the kind and where it happened.
std::string said(std::vector<WindowEvent> const& events)
{
    std::string out;
    for (WindowEvent const& event : events) {
        using Kind = WindowEvent::Kind;
        if (!out.empty())
            out += " ";
        switch (event.kind) {
        case Kind::MouseMove: out += "M"; break;
        case Kind::MouseDown: out += "D"; break;
        case Kind::MouseUp: out += "U"; break;
        case Kind::Wheel: out += "W"; break;
        case Kind::Scroll: out += "S"; break;
        case Kind::KeyDown: out += "K"; break;
        case Kind::Text: out += "T"; break;
        case Kind::Resize: out += "R"; break;
        case Kind::Scale: out += "X"; break;
        case Kind::Active: out += "A"; break;
        case Kind::Visible: out += "V"; break;
        case Kind::Preedit: out += "P"; break;
        case Kind::Close: out += "C"; break;
        case Kind::None: out += "-"; break;
        }
        out += std::to_string(event.x) + "," + std::to_string(event.y);
    }
    return out;
}

}

int main()
{
    using Kind = WindowEvent::Kind;
    // A thousand moves are one: the last.
    {
        std::vector<WindowEvent> events;
        for (int i = 0; i < 1000; ++i)
            events.push_back(move_to(i, 2 * i));
        platform::keep_last_pointer_moves(events);
        CHECK_EQ(said(events), std::string("M999,1998"));
    }
    // A press ends a run: it lands where the pointer then was, and the moves
    // after it are a run of their own.
    {
        std::vector<WindowEvent> events { move_to(1, 1), move_to(2, 2), of(Kind::MouseDown, 2, 2), move_to(3, 3), move_to(4, 4),
            of(Kind::MouseUp, 4, 4), move_to(5, 5) };
        platform::keep_last_pointer_moves(events);
        CHECK_EQ(said(events), std::string("M2,2 D2,2 M4,4 U4,4 M5,5"));
    }
    // So do a wheel, a finger's scroll, a key and typed text.
    for (Kind const kind : { Kind::Wheel, Kind::Scroll, Kind::KeyDown, Kind::Text, Kind::Preedit, Kind::Close }) {
        std::vector<WindowEvent> events { move_to(1, 1), move_to(2, 2), of(kind), move_to(3, 3), move_to(4, 4) };
        platform::keep_last_pointer_moves(events);
        CHECK_EQ(events.size(), std::size_t { 3 });
        if (events.size() == 3) {
            CHECK(events[0].kind == Kind::MouseMove && events[0].x == 2);
            CHECK(events[1].kind == kind);
            CHECK(events[2].kind == Kind::MouseMove && events[2].x == 4);
        }
    }
    // A size, a scale, and the window's coming to the front or leaving sight
    // do not: the run goes on past them, its move standing where the run's
    // first did, and they are all kept.
    {
        std::vector<WindowEvent> events { move_to(1, 1), of(Kind::Resize), move_to(2, 2), of(Kind::Scale), of(Kind::Active),
            move_to(3, 3), of(Kind::Visible), of(Kind::None), move_to(4, 4) };
        platform::keep_last_pointer_moves(events);
        CHECK_EQ(said(events), std::string("M4,4 R0,0 X0,0 A0,0 V0,0 -0,0"));
    }
    // Nothing to do leaves everything as it was.
    {
        std::vector<WindowEvent> none;
        platform::keep_last_pointer_moves(none);
        CHECK(none.empty());
        std::vector<WindowEvent> events { of(Kind::KeyDown), move_to(7, 8), of(Kind::MouseDown, 7, 8) };
        platform::keep_last_pointer_moves(events);
        CHECK_EQ(said(events), std::string("K0,0 M7,8 D7,8"));
    }
    return test::report("window events");
}
