#pragma once

// The window's own accessible nodes — the chrome the shell draws without a
// widget tree — built on the window's thread from what the shell shows
// (ui::Browser and its ChromeLayout) as the model's NodeData, and the tools'
// actions on them carried out as the reader's own input would: a press is
// a click at the control's middle, a tab chosen is the tab selected.
//
// The tree: the frame (the window, its title) → the tab strip (each tab,
// with its close button; the new-tab button; the window's own buttons where
// the shell draws them) → the toolbar (back, forward, reload, the address
// bar, reader mode, bookmark, the main menu) → the bars that are open (a
// site's question, find, bookmarks) → the content area, under which the
// bridge hangs the documents → the open menus with their items, the
// command palette, the window's own file chooser, the status text.

#include "a11y/Tree.h"
#include "platform/Accessibility.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sashfold::ui {

class Browser;

class ChromeAccessibility {
public:
    // The window's thread, once a turn: when a tool is there, what it asked
    // of the chrome is done, then the window is handed over as it stands
    // (only when anything of it changed). Nothing at all when no tool is.
    void turn(Browser& browser, platform::Accessibility& bridge, bool window_active);

    // The window as the bridge would get it now, with the events since the
    // last call (for tests).
    platform::WindowState build(Browser& browser, bool window_active);
    // Carries out a tool's action on the chrome.
    void perform(Browser& browser, platform::WindowAction const& action);

private:
    a11y::NodeId id_for(std::string const& key);
    std::map<std::string, a11y::NodeId> m_ids;
    a11y::NodeId m_next = a11y::NodeId { 1 } << 62; // apart from every page's ids
    std::map<a11y::NodeId, std::function<void(Browser&, platform::WindowAction const&)>> m_actions;
    std::shared_ptr<a11y::Snapshot const> m_last;
    std::string m_signature;
};

// The window's end of the bridge, for whoever runs the window's loop (the
// window, a script): opens the system's bridge and makes it the process's,
// turns the chrome once a loop, and at the end takes it out of the
// process's reach before it goes.
class WindowAccessibility {
public:
    WindowAccessibility() = default;
    ~WindowAccessibility() { close(); }
    WindowAccessibility(WindowAccessibility const&) = delete;
    WindowAccessibility& operator=(WindowAccessibility const&) = delete;

    // Nothing where the system has no bridge, NO_AT_BRIDGE=1 says none, or
    // (found out on the bridge's own thread) no accessibility bus is there.
    // `wake_window` wakes the window's thread and the pages' threads, from
    // any thread, without blocking.
    void open(std::string const& application_name, std::function<void()> wake_window);
    void close();
    bool is_open() const { return m_bridge != nullptr; }

    // The window's thread, once a loop.
    void turn(Browser& browser, bool window_active);

private:
    std::unique_ptr<platform::Accessibility> m_bridge;
    ChromeAccessibility m_chrome;
};

}
