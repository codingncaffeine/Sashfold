#pragma once

// How the desktop blinks a text caret: the time it is shown and then
// hidden, and how long after the reader's last key or click it goes on
// blinking before it rests, shown. A desktop that has blinking turned off
// (an accessibility setting on every one of them) says nothing, and the
// caret is drawn steady.
//
//   Linux: KDE's kdeglobals [KDE] CursorBlinkRate in a KDE session; else
//          the desktop portal's org.gnome.desktop.interface cursor-blink,
//          cursor-blink-time and cursor-blink-timeout (GNOME and the
//          desktops whose portal reads the GNOME settings); else GTK's own
//          settings.ini; else GTK's defaults (1200 ms a cycle, 10 s).
//   Windows: GetCaretBlinkTime, and the caret timeout from
//          SystemParametersInfo (SPI_GETCARETTIMEOUT).
//   Elsewhere: 500 ms each way, never resting.

#include <optional>

namespace sashfold::platform {

struct CaretBlink {
    int half_ms = 600;    // shown for this long, then hidden for as long
    int timeout_ms = 0;   // blinking stops this long after the last action; 0 never
};

// Starts finding the desktop's setting, on a thread of its own where it
// asks a service; until it is known, caret_blink() answers the defaults.
// Only a real window asks: a scripted run never blinks.
void start_caret_blink_lookup();
std::optional<CaretBlink> caret_blink();

}
