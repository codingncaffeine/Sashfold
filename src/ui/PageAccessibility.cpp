#include "ui/PageAccessibility.h"

#include "css/ComputedStyle.h"
#include "layout/Controls.h"
#include "ui/Frames.h"

#include <unordered_map>

namespace sashfold::ui {

// The page's styles and layout (StyleMapSource), with the fields' carets
// and selections as the shell keeps them, and the documents of its live
// frames: each frame's styles and layout, its boxes moved to where the
// frame's content box is in the page, so that the tree holds the frames'
// documents under their frames (PageSource::frame_document) as one tree.
class PageAccessibility::Source final : public a11y::PageSource {
public:
    Source(css::StyleMap const& styles, layout::Fragment const* root)
        : inner(styles, root)
    {
    }

    // A live frame's document as this source reads it.
    struct Frame {
        FrameView const* view = nullptr;
        std::unique_ptr<a11y::StyleMapSource> source;
        float dx = 0;
        float dy = 0;
    };

    // The page thread, each turn: the frames as drawn now (their views may
    // have been made again since the last turn). The generation moves when
    // any of them is not the one it was.
    void set_frames(DrawnFrames const* top)
    {
        std::unordered_map<dom::Document const*, Frame> next;
        std::uint64_t signature = 0;
        if (top)
            collect(*top, nullptr, 0, 0, next, signature);
        if (signature != m_frames_signature)
            ++m_frames_generation;
        m_frames_signature = signature;
        m_frames = std::move(next);
        m_top_frames = top;
    }

    css::ComputedStyle const* style(dom::Element const& element) override
    {
        Frame const* const frame = frame_of(element.document());
        return frame ? frame->source->style(element) : inner.style(element);
    }
    std::optional<a11y::Rect> box(dom::Element const& element) override
    {
        Frame const* const frame = frame_of(element.document());
        if (!frame)
            return inner.box(element);
        std::optional<a11y::Rect> rect = frame->source->box(element);
        if (rect) {
            rect->x += frame->dx;
            rect->y += frame->dy;
        }
        return rect;
    }
    std::vector<a11y::Rect> text_boxes(dom::Node const& node, std::uint32_t from, std::uint32_t to) override
    {
        Frame const* const frame = frame_of(node.document());
        if (!frame)
            return inner.text_boxes(node, from, to);
        std::vector<a11y::Rect> boxes = frame->source->text_boxes(node, from, to);
        for (a11y::Rect& rect : boxes) {
            rect.x += frame->dx;
            rect.y += frame->dy;
        }
        return boxes;
    }
    dom::Element const* element_at(float x, float y) override
    {
        // Into a frame's document where the point is on one.
        dom::Element const* found = inner.element_at(x, y);
        for (int depth = 0; found && depth < 16; ++depth) {
            dom::Document* const document = frame_document(*found);
            Frame const* const frame = document ? frame_of(*document) : nullptr;
            if (!frame)
                break;
            dom::Element const* const below = frame->source->element_at(x - frame->dx, y - frame->dy);
            if (!below)
                break;
            found = below;
        }
        return found;
    }
    dom::Document* frame_document(dom::Element const& element) override
    {
        DrawnFrames const* frames = m_top_frames;
        if (Frame const* const owner = frame_of(element.document()))
            frames = &owner->view->frames;
        if (!frames)
            return nullptr;
        auto const drawn = frames->find(&element);
        if (drawn == frames->end() || !drawn->second.view || !drawn->second.view->document)
            return nullptr;
        // Only one this source reads (collect found it), so that the tree
        // never holds a document it cannot read the boxes of.
        return m_frames.contains(drawn->second.view->document) ? drawn->second.view->document : nullptr;
    }
    std::optional<dom::forms::TextSelection> text_selection(dom::Element const& element) override
    {
        if (!controls)
            return std::nullopt;
        auto const found = controls->states.find(&element);
        if (found == controls->states.end() || !found->second.selection)
            return std::nullopt;
        return *found->second.selection;
    }
    std::uint64_t generation() const override { return inner.generation() + m_frames_generation; }

    a11y::StyleMapSource inner;
    layout::ControlStates const* controls = nullptr;

private:
    Frame const* frame_of(dom::Document const& document) const
    {
        if (m_frames.empty())
            return nullptr;
        auto const found = m_frames.find(&document);
        return found == m_frames.end() ? nullptr : &found->second;
    }

    // Each live frame under `frames` (the page's, or a frame's own), with
    // where its content box is: the frame element's border box in the
    // document holding it, inside its border and padding.
    void collect(DrawnFrames const& frames, Frame const* holder, float dx, float dy, std::unordered_map<dom::Document const*, Frame>& into,
        std::uint64_t& signature)
    {
        for (auto const& [element, drawn] : frames) {
            FrameView const* const view = drawn.view.get();
            if (!element || !view || !view->document || into.contains(view->document))
                continue;
            a11y::StyleMapSource& holder_source = holder ? *holder->source : inner;
            std::optional<a11y::Rect> const border_box = holder_source.box(*element);
            if (!border_box)
                continue;
            float x = border_box->x + dx;
            float y = border_box->y + dy;
            if (css::ComputedStyle const* const style = holder_source.style(*element)) {
                auto const px = [](css::LengthPercent const& length) { return length.kind == css::LengthPercent::Kind::Px ? length.value : 0.0f; };
                x += style->border_left.width + px(style->padding_left);
                y += style->border_top.width + px(style->padding_top);
            }
            Frame frame;
            frame.view = view;
            frame.source = std::make_unique<a11y::StyleMapSource>(view->styles, &view->layout.root);
            frame.dx = x;
            frame.dy = y;
            // What the frame is now: its view, its picture's count, its
            // place; any of them other than before reads the boxes afresh.
            signature = signature * 1000003u + reinterpret_cast<std::uintptr_t>(view) + drawn.mutations * 31u
                + static_cast<std::uint64_t>(x) * 7u + static_cast<std::uint64_t>(y) * 13u + static_cast<std::uint64_t>(view->scroll_y);
            auto const [at, added] = into.emplace(view->document, std::move(frame));
            if (added)
                collect(view->frames, &at->second, 0, 0, into, signature);
        }
    }

    DrawnFrames const* m_top_frames = nullptr;
    std::unordered_map<dom::Document const*, Frame> m_frames;
    std::uint64_t m_frames_signature = 0;
    std::uint64_t m_frames_generation = 0;
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
        m_source->set_frames(turn.frames);
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
    } else {
        if (turn.layout_generation != m_layout_generation || turn.styles != m_styles || turn.root != m_root) {
            // Styles or layout made again, or the tab that holds them
            // moved: the boxes are read afresh.
            m_source->inner.set(*turn.styles, turn.root);
            m_styles = turn.styles;
            m_root = turn.root;
            m_layout_generation = turn.layout_generation;
        }
        // The frames' views may have been made again since: taken afresh
        // every turn, never kept from the last.
        m_source->set_frames(turn.frames);
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
