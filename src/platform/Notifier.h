#pragma once

// The desktop's notifications: what a page's `new Notification(...)` is
// shown through once the reader has allowed its site to. On Linux that is
// the desktop's own notification service, spoken to over the session bus
// (platform/linux/NotifierDBus.cpp, the Desktop Notifications
// Specification); on Windows it is the system's toasts
// (platform/win32/NotifierWin32.cpp, Windows.UI.Notifications). macOS has
// no window of ours yet, so no notifier either: there the seam opens
// nothing and a page is answered "denied".
//
// A notifier is asked from the window's thread and answers through
// events, taken at the window's own pace; `wake` is called (from another
// thread) when there is one to take.

#include "core/Bitmap.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::platform {

struct Notice {
    std::string title;
    std::string body;
    // Who is speaking, shown with the notice so that a page cannot pass
    // for the browser or for another site: its host.
    std::string site;
    // The page's picture for it (its icon), no larger than
    // `notice_picture_size` either way; null for none.
    std::shared_ptr<Bitmap const> picture;
    bool stays = false; // until the reader dismisses it (requireInteraction)
    bool silent = false;
};

// The largest a notice's picture is sent at: what the desktops show is
// smaller still (GNOME and Plasma draw it at 48 to 64 px, Windows at 48 by
// 48 scaled), and four times 64 covers a screen of any density.
inline constexpr int notice_picture_size = 256;

class Notifier {
public:
    virtual ~Notifier() = default;

    // False once it is known that no service answers — there is no bus,
    // or nothing on it shows notifications — and true until then, the
    // moments before the service has been reached included: a page that
    // loads in those moments must not read "denied" and give up for good.
    // What is shown meanwhile waits for the service's first answer.
    virtual bool available() const = 0;
    // Shows the notice and names it, for the events and for close(); the
    // one named `replaces` (0: none) is taken down in its place.
    virtual std::uint64_t show(Notice const& notice, std::uint64_t replaces = 0) = 0;
    virtual void close(std::uint64_t id) = 0;

    struct Event {
        enum class Kind {
            Shown, // the desktop has it
            Clicked, // the reader pressed it
            Closed, // gone: dismissed, timed out, or closed from here
            Failed, // the desktop refused it
        };
        Kind kind = Kind::Shown;
        std::uint64_t id = 0;
        // Of a press: the desktop's leave for the window to come to the
        // front (Wayland's xdg-activation token, which the notification
        // service hands over with the press); empty where it gave none and
        // where the system needs none.
        std::string activation_token = {};
    };
    virtual std::vector<Event> take_events() = 0;
};

// The desktop's notifier, reached in the background; null where this
// system has none written or the session has no bus. `application` is the
// name the desktop files the notices under.
std::unique_ptr<Notifier> open_notifier(std::string application, std::function<void()> wake);

// The service's text takes a little markup where it says it does
// ("body-markup"): what a page wrote is then escaped, so that it reads as
// it was written and can be no link.
std::string escape_notice_markup(std::string_view text);

// A notice as a Windows toast's XML (the ToastGeneric template): the title
// (the site where it has none), the body, the site on the attribution line
// as Chrome puts it, the picture by `picture_uri` (a file: URI; none when
// empty), long on screen when it stays, and no sound when silent. What a
// page wrote is escaped, so that it reads as written and is no markup.
std::string toast_xml(Notice const& notice, std::string_view picture_uri);

}
