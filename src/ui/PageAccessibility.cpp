#include "ui/PageAccessibility.h"

#include "layout/Controls.h"

namespace sashfold::ui {

// The page's styles and layout (StyleMapSource), with the fields' carets
// and selections as the shell keeps them.
class PageAccessibility::Source final : public a11y::PageSource {
public:
    Source(css::StyleMap const& styles, layout::Fragment const* root)
        : inner(styles, root)
    {
    }
    css::ComputedStyle const* style(dom::Element const& element) override { return inner.style(element); }
    std::optional<a11y::Rect> box(dom::Element const& element) override { return inner.box(element); }
    std::vector<a11y::Rect> text_boxes(dom::Node const& node, std::uint32_t from, std::uint32_t to) override
    {
        return inner.text_boxes(node, from, to);
    }
    dom::Element const* element_at(float x, float y) override { return inner.element_at(x, y); }
    std::optional<dom::forms::TextSelection> text_selection(dom::Element const& element) override
    {
        if (!controls)
            return std::nullopt;
        auto const found = controls->states.find(&element);
        if (found == controls->states.end() || !found->second.selection)
            return std::nullopt;
        return *found->second.selection;
    }
    std::uint64_t generation() const override { return inner.generation(); }

    a11y::StyleMapSource inner;
    layout::ControlStates const* controls = nullptr;
};

PageAccessibility::PageAccessibility() = default;

PageAccessibility::~PageAccessibility() { drop(); }

void PageAccessibility::drop()
{
    if (m_link) {
        // The bridge's end lets go of the tree first (a query posted from
        // now on is dropped); then the bridge hears the document is gone,
        // and may hold the link a while longer.
        m_link->bridge->detach();
        std::shared_ptr<platform::AccessibleDocument> const link = m_link;
        platform::with_accessibility([&link](platform::Accessibility& bridge) { bridge.remove_document(link); });
    }
    m_link.reset();
    m_tree.reset();
    m_source.reset();
    m_styles = nullptr;
    m_root = nullptr;
    m_load_said = false;
}

void PageAccessibility::turn(Turn const& turn, a11y::ActionSink* sink, std::function<void()> const& wake)
{
    if (!platform::accessibility_wanted()) {
        if (m_tree)
            drop();
        return;
    }
    // Made for a bridge that is gone: made again for this one.
    if (m_tree && m_bridge_generation != platform::accessibility_generation())
        drop();
    if (!turn.document || !turn.styles)
        return;
    if (!m_tree) {
        m_source = std::make_unique<Source>(*turn.styles, turn.root);
        m_tree = std::make_unique<a11y::Tree>(*turn.document, m_source.get());
        m_tree->set_page_wake(wake);
        m_link = std::make_shared<platform::AccessibleDocument>();
        // The tree's events wake the bridge's thread, through the lock that
        // keeps a bridge that is going from being reached.
        m_link->bridge = std::make_shared<a11y::Bridge>(*m_tree, [] { platform::wake_accessibility(); });
        m_link->set_view(turn.view);
        m_bridge_generation = platform::accessibility_generation();
        m_styles = turn.styles;
        m_root = turn.root;
        m_layout_generation = turn.layout_generation;
        std::shared_ptr<platform::AccessibleDocument> const link = m_link;
        platform::with_accessibility([&link](platform::Accessibility& bridge) { bridge.add_document(link); });
    } else if (turn.layout_generation != m_layout_generation || turn.styles != m_styles || turn.root != m_root) {
        // Styles or layout made again, or the tab that holds them moved:
        // the boxes are read afresh.
        m_source->inner.set(*turn.styles, turn.root);
        m_styles = turn.styles;
        m_root = turn.root;
        m_layout_generation = turn.layout_generation;
    }
    m_source->controls = turn.controls;
    m_tree->set_action_sink(sink);
    m_tree->update();
    m_tree->serve();
    m_tree->set_action_sink(nullptr); // the sink is the turn's
    if (turn.loaded && !m_load_said) {
        m_load_said = true;
        m_tree->load_complete();
    }
    if (m_link->view() != turn.view) {
        m_link->set_view(turn.view);
        platform::wake_accessibility();
    }
}

}
