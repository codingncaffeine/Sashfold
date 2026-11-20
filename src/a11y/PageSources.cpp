#include "a11y/PageSources.h"

#include "bindings/LayoutOracle.h"
#include "bindings/Realm.h"
#include "layout/Layout.h"
#include "paint/Painter.h"

namespace sashfold::a11y {

namespace {

Rect rect_of(bindings::LayoutBox const& box)
{
    if (box.seen)
        return Rect { (*box.seen)[0], (*box.seen)[1], (*box.seen)[2], (*box.seen)[3] };
    return Rect { box.x, box.y, box.width, box.height };
}

}

css::ComputedStyle const* StyleMapSource::style(dom::Element const& element)
{
    auto const found = m_styles->find(&element);
    return found == m_styles->end() ? nullptr : &found->second;
}

std::optional<Rect> StyleMapSource::box(dom::Element const& element)
{
    if (!m_root)
        return std::nullopt;
    std::optional<bindings::LayoutBox> const found = bindings::find_element_box(*m_root, element, *m_styles);
    if (!found)
        return std::nullopt;
    return rect_of(*found);
}

std::vector<Rect> StyleMapSource::text_boxes(dom::Node const& node, std::uint32_t from, std::uint32_t to)
{
    std::vector<Rect> out;
    if (!m_root)
        return out;
    for (bindings::LayoutBox const& box : bindings::find_text_boxes(*m_root, node, from, to))
        out.push_back(rect_of(box));
    return out;
}

dom::Element const* StyleMapSource::element_at(float x, float y)
{
    if (!m_root)
        return nullptr;
    std::optional<paint::PointHit> const hit = paint::hit_test(*m_root, x, y, [this](dom::Element const& element) { return style(element); });
    return hit ? hit->element : nullptr;
}

css::ComputedStyle const* OracleSource::style(dom::Element const& element)
{
    return m_oracle->style(element);
}

std::optional<Rect> OracleSource::box(dom::Element const& element)
{
    std::optional<bindings::LayoutBox> const found = m_oracle->box(element);
    if (!found)
        return std::nullopt;
    return rect_of(*found);
}

std::vector<Rect> OracleSource::text_boxes(dom::Node const& node, std::uint32_t from, std::uint32_t to)
{
    std::vector<Rect> out;
    for (bindings::LayoutBox const& box : m_oracle->text_boxes(node, from, to))
        out.push_back(rect_of(box));
    return out;
}

dom::Element const* OracleSource::element_at(float x, float y)
{
    return m_oracle->element_at(x, y - m_oracle->page_scroll());
}

std::uint64_t OracleSource::generation() const
{
    return m_realm ? m_realm->tree_mutation_count() : 0;
}

}
