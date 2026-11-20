#pragma once

// A page's accessibility tree as its own thread keeps it for the
// platform's bridge (platform/Accessibility.h): made at the first turn
// after an assistive tool shows itself, brought up to date at every turn,
// its queued questions answered there, and let go of when the page goes.
// While no tool is there a turn costs one atomic read and nothing is made.
//
// LIFETIME. It reads the document it was made for, so it must go before
// that document does: its owner (the shell's tab) ends it before the
// document is replaced or freed. It never compares document pointers to
// find out.

#include "a11y/PageSources.h"
#include "a11y/Tree.h"
#include "platform/Accessibility.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace sashfold::layout {
struct Fragment;
struct ControlStates;
}

namespace sashfold::ui {

class PageAccessibility {
public:
    struct Turn {
        dom::Document* document = nullptr;
        css::StyleMap const* styles = nullptr;
        layout::Fragment const* root = nullptr;
        layout::ControlStates const* controls = nullptr; // the fields' carets and selections
        // A number that moves whenever the styles or the layout were made
        // again: the tree reads boxes afresh then.
        std::uint64_t layout_generation = 0;
        platform::AccessibleDocument::View view;
        bool loaded = false; // the document finished loading
    };

    PageAccessibility();
    ~PageAccessibility();
    PageAccessibility(PageAccessibility const&) = delete;
    PageAccessibility& operator=(PageAccessibility const&) = delete;

    // Whether a turn would do anything: a tool is there.
    static bool wanted() { return platform::accessibility_wanted(); }

    // The page's thread, every turn while wanted(): `sink` does what a tool
    // asks of the page; `wake` gets the page's thread to its next turn soon
    // (called from the bridge's thread; must not block).
    void turn(Turn const& turn, a11y::ActionSink* sink, std::function<void()> const& wake);
    a11y::Tree* tree() { return m_tree.get(); }

private:
    class Source;
    void drop();
    std::unique_ptr<Source> m_source;
    std::unique_ptr<a11y::Tree> m_tree;
    std::shared_ptr<platform::AccessibleDocument> m_link;
    std::uint64_t m_bridge_generation = 0;
    css::StyleMap const* m_styles = nullptr;
    layout::Fragment const* m_root = nullptr;
    std::uint64_t m_layout_generation = 0;
    bool m_load_said = false;
};

}
