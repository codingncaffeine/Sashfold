#pragma once

// The platform's accessibility bridge: what puts the window's accessibility
// tree — the chrome's own nodes and each page's document (a11y/Tree.h) —
// in front of the system's assistive technology. On Linux that is AT-SPI2
// over D-Bus (platform/linux/AccessibilityAtspi.cpp); elsewhere there is
// none yet (AccessibilityNone.cpp) and nothing of this costs anything.
//
// THREADS. The bridge has a thread of its own, which answers the tools'
// questions from snapshots and never waits on the window's thread or a
// page's: what must be done there (an action, a focus move, a hit test of
// text) is posted, and the thread it is for does it at its next turn. The
// window's thread hands the chrome's tree over as a snapshot (set_window)
// and takes the tools' actions on the chrome (take_window_actions). A page
// thread makes its document's tree only while wanted() says a tool is
// there, and hands it over through an AccessibleDocument.

#include "a11y/Tree.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sashfold::platform {

// A page's document, as its thread shows it to the bridge. Shared: the
// page's thread makes it and says when its tree is gone (Bridge::detach);
// the bridge's thread holds it until it has said the document is gone.
struct AccessibleDocument {
    // The tree's thread-safe end.
    std::shared_ptr<a11y::Bridge> bridge;
    // Where the page's coordinates are in the window: the content area's
    // origin is the chrome's (WindowState::content); this is the page's
    // own scroll, in the same px as its nodes' bounds.
    struct View {
        float scroll_x = 0;
        float scroll_y = 0;
        bool shown = false; // the tab in front: a background tab's document is hidden
        bool loading = false;
        std::string url;
        std::string mime_type = "text/html";
        std::string title;
        bool operator==(View const&) const = default;
    };
    void set_view(View view)
    {
        std::lock_guard<std::mutex> const hold(lock);
        m_view = std::move(view);
    }
    View view() const
    {
        std::lock_guard<std::mutex> const hold(lock);
        return m_view;
    }
    mutable std::mutex lock;

private:
    View m_view;
};

// The window as the bridge exposes it: the frame, its title and state, and
// the chrome's own nodes as a tree of the model's NodeData (the frame its
// root, window coordinates for bounds) — the shell builds it, the bridge
// puts the shown document under `content`.
struct WindowState {
    std::shared_ptr<a11y::Snapshot const> chrome;
    a11y::NodeId content = 0; // the chrome node the documents hang under
    std::string title;
    bool active = true;
    int width = 0;
    int height = 0;
    // The content area, window coordinates: where a document's (0, 0) is.
    float content_x = 0;
    float content_y = 0;
    float content_width = 0;
    float content_height = 0;
    // What happened in the chrome since the last state, in order (focus
    // moves, states, text typed into the address bar).
    std::vector<a11y::Event> events;
};

// What a tool asked of the chrome: a node's action by name, or text set
// into an editable node, carried out by the window's thread.
struct WindowAction {
    a11y::NodeId node = 0;
    std::string action; // one of the node's NodeData::actions, or "set-text", "set-caret", "set-selection"
    std::string text;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

class Accessibility {
public:
    virtual ~Accessibility() = default;
    // Whether an assistive tool is there: it has asked something of this
    // process or listens for its events. Until it is, no tree is made — an
    // atomic read, what a page's turn pays when nobody listens.
    virtual bool wanted() const = 0;
    // A page's document joins the window's tree (its thread made its tree
    // because wanted() said so), and leaves it (after Bridge::detach).
    virtual void add_document(std::shared_ptr<AccessibleDocument> document) = 0;
    virtual void remove_document(std::shared_ptr<AccessibleDocument> const& document) = 0;
    // The window's thread: the window as it stands now.
    virtual void set_window(WindowState state) = 0;
    // The window's thread: what tools asked of the chrome, to do now.
    virtual std::vector<WindowAction> take_window_actions() = 0;
    // Any thread, never blocking: something changed, the bridge looks.
    virtual void wake() = 0;
    // Any thread, never blocking: the window's thread is woken (the
    // function open_accessibility was given).
    virtual void wake_window() = 0;
};

// The bridge of this system: nothing where there is none, where the
// environment says NO_AT_BRIDGE=1, or where no accessibility bus is there
// to be reached (checked on the bridge's own thread: the caller is never
// held). `wake_window` is called, from the bridge's thread, when the window
// has actions to take.
std::unique_ptr<Accessibility> open_accessibility(std::string const& application_name, std::function<void()> wake_window);

// The process's bridge, for the page threads to reach: set by whoever
// opened it (the window), cleared (nullptr) before it goes. The page
// threads never hold the bridge itself: they reach it only through the
// calls below, which take the lock set_accessibility takes, so that once
// set_accessibility(nullptr) has returned nothing reaches the old bridge.
void set_accessibility(Accessibility* accessibility);
// Whether a tool is there: one atomic read where no bridge is set.
bool accessibility_wanted();
// Calls `use` with the bridge, under that lock (it must not block); false
// when there is none.
bool with_accessibility(std::function<void(Accessibility&)> const& use);
// A number that moves each time another bridge is set: a page whose tree
// was made for one bridge makes it again for the next.
std::uint64_t accessibility_generation();
// Wakes the bridge's thread (a page's tree has events), or the window's
// (what only it can do is waiting: a query to a page whose thread is the
// window's, or a chrome action). Never blocks; nothing without a bridge.
void wake_accessibility();
void wake_accessibility_window();

}
