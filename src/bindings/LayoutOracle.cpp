#include "bindings/LayoutOracle.h"

#include "text/FontManager.h"

#include <algorithm>
#include <utility>

namespace sashfold::bindings {

namespace {

layout::Fragment const* find_fragment(layout::Fragment const& fragment, dom::Element const& element)
{
    if (fragment.element == &element)
        return &fragment;
    for (layout::Fragment const& child : fragment.children) {
        if (layout::Fragment const* found = find_fragment(child, element))
            return found;
    }
    return nullptr;
}

// The runs whose element chain reaches `element`, joined into one box.
// The line's exact ascent lives in the font; the box is taken as the
// run's font-size line, which is what a script measuring an inline
// element gets to within a pixel or two.
void union_runs(layout::Fragment const& fragment, dom::Element const& element, LayoutBox& box, bool& found)
{
    for (layout::TextRun const& run : fragment.runs) {
        for (dom::Node const* node = run.element; node; node = node->parent()) {
            if (node != &element)
                continue;
            float const size = run.style ? run.style->font_size : 16.0f;
            float const top = run.baseline_y - size * 0.8f;
            float const height = size * 1.2f;
            if (!found) {
                box = LayoutBox { run.x, top, run.width, height };
                found = true;
            } else {
                float const right = std::max(box.x + box.width, run.x + run.width);
                float const bottom = std::max(box.y + box.height, top + height);
                box.x = std::min(box.x, run.x);
                box.y = std::min(box.y, top);
                box.width = right - box.x;
                box.height = bottom - box.y;
            }
            break;
        }
    }
    for (layout::Fragment const& child : fragment.children)
        union_runs(child, element, box, found);
}

} // namespace

std::optional<LayoutBox> find_element_box(layout::Fragment const& root, dom::Element const& element)
{
    if (layout::Fragment const* fragment = find_fragment(root, element))
        return LayoutBox { fragment->x, fragment->y, fragment->width, fragment->height };
    LayoutBox box;
    bool found = false;
    union_runs(root, element, box, found);
    if (found)
        return box;
    return std::nullopt;
}

LayoutOracle::LayoutOracle(dom::Document& document, net::Url const& base, css::SheetFetcher fetch, css::MediaContext media)
    : m_document(document)
    , m_base(base)
    , m_fetch(std::move(fetch))
    , m_media(media)
{
}

void LayoutOracle::install(HostHooks& hooks)
{
    hooks.layout_box = [this](dom::Element const& element) { return box(element); };
    hooks.computed_style = [this](dom::Element const& element) { return style(element); };
}

void LayoutOracle::ensure()
{
    std::uint64_t const mutations = m_realm ? m_realm->mutation_count() : 0;
    if (m_computed && mutations == m_mutations)
        return;
    std::vector<css::SheetSource> const sheets = css::collect_stylesheets(m_document, &m_base, m_fetch, m_media);
    // The page's own fonts answer its measurements, as they do the render:
    // a test that measures text in Ahem and then sets a width from it must
    // measure in Ahem.
    text::FontManager::instance().set_page_fonts(css::collect_page_fonts(sheets, m_fetch, m_media));
    m_styles = css::resolve_styles(m_document, sheets, m_media, &m_base);
    m_layout = layout::layout_document(m_document, m_styles, m_media.width, nullptr, nullptr, m_media.height);
    m_computed = true;
    m_mutations = mutations;
}

std::optional<LayoutBox> LayoutOracle::box(dom::Element const& element)
{
    ensure();
    return find_element_box(m_layout.root, element);
}

css::ComputedStyle const* LayoutOracle::style(dom::Element const& element)
{
    ensure();
    auto const it = m_styles.find(&element);
    return it == m_styles.end() ? nullptr : &it->second;
}

}
