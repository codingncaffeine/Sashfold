#include "a11y/PageSources.h"

#include "bindings/LayoutOracle.h"
#include "bindings/Realm.h"
#include "layout/Layout.h"
#include "paint/Painter.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sashfold::a11y {

namespace {

Rect rect_of(bindings::LayoutBox const& box)
{
    if (box.seen)
        return Rect { (*box.seen)[0], (*box.seen)[1], (*box.seen)[2], (*box.seen)[3] };
    return Rect { box.x, box.y, box.width, box.height };
}

}

struct StyleMapSource::Index {
    std::unordered_map<dom::Element const*, layout::Fragment const*> own; // an element's first fragment
    std::unordered_set<dom::Element const*> transformed; // drawn through a transform: asked of the whole tree
    std::unordered_map<dom::Element const*, Rect> runs; // an inline element's runs, united
    std::unordered_map<dom::Node const*, std::vector<layout::Fragment const*>> holders; // the fragments holding a text node's runs
};

css::ComputedStyle const* StyleMapSource::style(dom::Element const& element)
{
    auto const found = m_styles->find(&element);
    return found == m_styles->end() ? nullptr : &found->second;
}

StyleMapSource::Index const& StyleMapSource::index()
{
    if (m_index)
        return *m_index;
    m_index = std::make_shared<Index>();
    Index& index = *m_index;
    auto const unite = [](Rect& into, Rect const& add, bool first) {
        if (first) {
            into = add;
            return;
        }
        float const right = std::max(into.x + into.width, add.x + add.width);
        float const bottom = std::max(into.y + into.height, add.y + add.height);
        into.x = std::min(into.x, add.x);
        into.y = std::min(into.y, add.y);
        into.width = right - into.x;
        into.height = bottom - into.y;
    };
    auto const walk = [&](auto const& self, layout::Fragment const& fragment, bool transformed) -> void {
        transformed = transformed || fragment.transform != nullptr;
        if (fragment.element) {
            index.own.emplace(fragment.element, &fragment);
            if (transformed)
                index.transformed.insert(fragment.element);
        }
        for (layout::TextRun const& run : fragment.runs) {
            // The run's box as find_element_box measures one.
            float const size = run.style ? run.style->font_size : 16.0f;
            Rect const box { run.x, run.baseline_y - size * 0.8f, run.width, size * 1.2f };
            for (dom::Node const* node = run.element; node && node != fragment.element; node = dom::flat_parent(*node)) {
                if (!node->is_element())
                    continue;
                auto const* element = static_cast<dom::Element const*>(node);
                auto [entry, added] = index.runs.try_emplace(element, box);
                if (!added)
                    unite(entry->second, box, false);
                if (transformed)
                    index.transformed.insert(element);
            }
            if (run.source) {
                std::vector<layout::Fragment const*>& holders = index.holders[run.source];
                if (holders.empty() || holders.back() != &fragment)
                    holders.push_back(&fragment);
            }
        }
        for (layout::Fragment const& child : fragment.children)
            self(self, child, transformed);
    };
    if (m_root)
        walk(walk, *m_root, false);
    return index;
}

std::optional<Rect> StyleMapSource::box(dom::Element const& element)
{
    if (!m_root)
        return std::nullopt;
    if (css::ComputedStyle const* own = style(element); own && own->display == css::Display::Contents)
        return std::nullopt;
    Index const& found = index();
    if (found.transformed.contains(&element)) {
        std::optional<bindings::LayoutBox> const box = bindings::find_element_box(*m_root, element, *m_styles);
        return box ? std::optional<Rect>(rect_of(*box)) : std::nullopt;
    }
    if (auto const fragment = found.own.find(&element); fragment != found.own.end())
        return Rect { fragment->second->x, fragment->second->y, fragment->second->width, fragment->second->height };
    if (auto const runs = found.runs.find(&element); runs != found.runs.end())
        return runs->second;
    return std::nullopt;
}

std::vector<Rect> StyleMapSource::text_boxes(dom::Node const& node, std::uint32_t from, std::uint32_t to)
{
    std::vector<Rect> out;
    if (!m_root)
        return out;
    Index const& found = index();
    auto const holders = found.holders.find(&node);
    if (holders == found.holders.end())
        return out;
    for (layout::Fragment const* holder : holders->second) {
        for (bindings::LayoutBox const& box : bindings::find_text_boxes(*holder, node, from, to)) {
            Rect const rect = rect_of(box);
            if (std::find(out.begin(), out.end(), rect) == out.end())
                out.push_back(rect);
        }
    }
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
