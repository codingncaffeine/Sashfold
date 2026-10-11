#include "bindings/LayoutOracle.h"

#include "bindings/Internal.h"
#include "html/DocumentBase.h"
#include "paint/Painter.h"
#include "text/FontManager.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

layout::EmbeddedStates embedded_states(Realm& realm)
{
    using Represents = Realm::Internals::Represents;
    layout::EmbeddedStates states;
    for (auto const& [element, represents] : realm.internals().embedder_states) {
        switch (represents) {
        case Represents::Navigable: states.emplace(element, layout::Embedded::Document); break;
        case Represents::Image: states.emplace(element, layout::Embedded::Image); break;
        case Represents::Fallback: states.emplace(element, layout::Embedded::Fallback); break;
        case Represents::Nothing: states.emplace(element, layout::Embedded::Nothing); break;
        }
    }
    return states;
}

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

layout::Fragment const* find_element_fragment(layout::Fragment const& root, dom::Element const& element)
{
    return find_fragment(root, element);
}

namespace {

// A length as layout resolves it, a percentage of `base`.
float length_px(css::LengthPercent const& length, float base)
{
    switch (length.kind) {
    case css::LengthPercent::Kind::Px: return length.value;
    case css::LengthPercent::Kind::Percent: return base * length.value / 100.0f;
    case css::LengthPercent::Kind::Calc: return length.value + base * length.percent / 100.0f;
    default: return 0;
    }
}

// The furthest right and bottom page edges a box's descendants reach, those
// inside a box that clips them left out.
void reach_of(layout::Fragment const& box, float& right, float& bottom)
{
    for (layout::TextRun const& run : box.runs)
        right = std::max(right, run.x + run.width);
    for (layout::Fragment const& child : box.children) {
        right = std::max(right, child.x + child.width);
        bottom = std::max(bottom, child.y + child.height);
        bool const clips = child.style != nullptr && child.style->overflow_applies
            && (child.style->overflow_x != css::Overflow::Visible || child.style->overflow_y != css::Overflow::Visible);
        if (!clips)
            reach_of(child, right, bottom);
    }
}

// Where the margin boxes of a box's in-flow children end, which its end
// padding comes after, as layout settles a scroll range.
void in_flow_reach(layout::Fragment const& box, float base, float& right, float& bottom)
{
    for (layout::TextRun const& run : box.runs)
        right = std::max(right, run.x + run.width);
    for (layout::Fragment const& child : box.children) {
        if (child.style && (child.style->position == css::Position::Absolute || child.style->position == css::Position::Fixed))
            continue;
        float const margin_right = child.style ? length_px(child.style->margin_right, base) : 0.0f;
        float const margin_bottom = child.style ? length_px(child.style->margin_bottom, base) : 0.0f;
        right = std::max(right, child.x + child.width + margin_right);
        bottom = std::max(bottom, child.y + child.height + margin_bottom);
    }
}

}

ElementScroll scroll_of_box(layout::Fragment const& box, layout::ScrollOffsets const* offsets)
{
    ElementScroll at;
    if (layout::is_scroll_container(box)) {
        layout::ScrollOffset const now = layout::scroll_of(box, offsets);
        at.x = now.x;
        at.y = now.y;
        at.range_x = box.scroll_range_x;
        at.range_y = box.scroll_range_y;
        return at;
    }
    at.scrolls = false;
    css::ComputedStyle const* const style = box.style;
    float const left = box.x + (style ? style->border_left.width : 0);
    float const top = box.y + (style ? style->border_top.width : 0);
    float const width = box.width - (style ? style->border_left.width + style->border_right.width : 0);
    float const height = box.height - (style ? style->border_top.width + style->border_bottom.width : 0);
    float right = left + width;
    float bottom = top + height;
    reach_of(box, right, bottom);
    // The end padding after the in-flow content, as for a box that scrolls.
    float flow_right = 0;
    float flow_bottom = 0;
    in_flow_reach(box, width, flow_right, flow_bottom);
    if (style != nullptr && (!box.children.empty() || !box.runs.empty())) {
        right = std::max(right, flow_right + length_px(style->padding_right, width));
        // A block with nothing at its end edge lets its last child's bottom
        // margin collapse through it, out of its box: then that margin is not
        // content of its own.
        float const end_padding = length_px(style->padding_bottom, width);
        if (end_padding > 0 || style->border_bottom.width > 0 || style->display != css::Display::Block)
            bottom = std::max(bottom, flow_bottom + end_padding);
    }
    at.range_x = std::max(0.0f, right - (left + width));
    at.range_y = std::max(0.0f, bottom - (top + height));
    return at;
}

std::optional<LayoutBox> find_element_box(layout::Fragment const& root, dom::Element const& element,
    css::StyleMap const& styles)
{
    // Its generated boxes and text are drawn as its parent's, but carry it
    // as their element: they are not its box.
    if (auto const it = styles.find(&element); it != styles.end() && it->second.display == css::Display::Contents)
        return std::nullopt;
    if (layout::Fragment const* fragment = find_fragment(root, element))
        return LayoutBox { fragment->x, fragment->y, fragment->width, fragment->height };
    LayoutBox box;
    bool found = false;
    union_runs(root, element, box, found);
    if (found)
        return box;
    return std::nullopt;
}

LayoutOracle::LayoutOracle(dom::Document& document, net::Url const& base, css::SheetFetcher fetch, css::MediaContext media,
    css::SheetFetcher font_fetch)
    : m_document(&document)
    , m_base(base)
    , m_fetch(std::move(fetch))
    , m_font_fetch(std::move(font_fetch))
    , m_media(media)
    , m_fonts_on_demand([this](net::Url const& url, std::string_view nonce) -> std::optional<css::FetchedSheet> {
        css::SheetFetcher const& fetch_font = m_font_fetch ? m_font_fetch : m_fetch;
        return fetch_font ? fetch_font(url, nonce) : std::nullopt;
    })
{
}

void LayoutOracle::retarget(dom::Document& document, net::Url const& base)
{
    m_document = &document;
    m_base = base;
    m_computed = false;
    m_style_set.reset();
    m_sheet_signature.clear();
    m_styles.clear();
    m_style_record = {};
    m_layout = {};
}

void LayoutOracle::set_viewport(float width, float height)
{
    if (width == m_media.width && height == m_media.height)
        return;
    m_media.width = width;
    m_media.height = height;
    // Media queries may answer otherwise now: the sheets are compiled again.
    m_computed = false;
    m_style_set.reset();
}

void LayoutOracle::install(HostHooks& hooks)
{
    hooks.layout_box = [this](dom::Element const& element) { return box(element); };
    hooks.computed_style = [this](dom::Element const& element) { return style(element); };
    hooks.element_from_point = [this](float x, float y) { return element_at(x, y); };
    hooks.with_fonts = [this](std::function<void()> const& use) { with_fonts(use); };
    hooks.element_scroll = [this](dom::Element const& element) { return element_scroll(element); };
    hooks.scroll_element = [this](dom::Element const& element, float x, float y) { scroll_element(element, x, y); };
}

namespace {

// What the stylesheets are: the elements that carry them and what they
// say, so that a script change elsewhere in the tree does not recompile
// every sheet (the shell keeps the same signature for its tabs).
std::string sheet_signature(dom::Node const& node)
{
    std::string signature;
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("style") || element.is_svg("style")) {
            signature += "s" + std::to_string(reinterpret_cast<std::uintptr_t>(&element)) + ":";
            for (dom::Node const* child : element.children()) {
                if (child->is_text())
                    signature += std::to_string(static_cast<dom::Text const*>(child)->data.size()) + ",";
            }
        } else if (element.is_html("link")) {
            signature += "l" + std::to_string(reinterpret_cast<std::uintptr_t>(&element)) + ":";
            if (dom::Attr const* href = element.find_attribute("href"))
                signature += href->value;
            if (dom::Attr const* rel = element.find_attribute("rel"))
                signature += rel->value;
            if (dom::Attr const* media = element.find_attribute("media"))
                signature += media->value;
            signature += ";";
        }
    }
    for (dom::Node const* child : node.children())
        signature += sheet_signature(*child);
    return signature;
}

} // namespace

void LayoutOracle::ensure()
{
    std::uint64_t const mutations = m_realm ? m_realm->mutation_count() : 0;
    if (m_computed && mutations == m_mutations)
        return;
    sheets_up_to_date();
    // A frame's document is laid out in its own fonts, and the page's are put
    // back: the process has one set at a time, and it is the page's between
    // layouts.
    std::vector<text::FontManager::PageFace> page_faces;
    if (m_keep_page_fonts)
        page_faces = text::FontManager::instance().page_faces();
    text::FontManager::instance().set_page_fonts(m_fonts);
    struct FontsBack {
        bool wanted;
        std::vector<text::FontManager::PageFace>& faces;
        ~FontsBack()
        {
            if (wanted)
                text::FontManager::instance().restore_page_faces(std::move(faces));
        }
    } const fonts_back { m_keep_page_fonts, page_faces };
    // Only what changed since the last answer is computed again; layout
    // stays whole.
    css::update_styles(*m_document, *m_style_set, m_styles, m_style_record);
    // The objects and embeds as the realm has decided them, so that a script
    // measuring an object's fallback, or an embed that represents nothing,
    // measures what is drawn.
    layout::EmbeddedStates const embedded = m_realm ? embedded_states(*m_realm) : layout::EmbeddedStates {};
    m_layout = layout::layout_document(*m_document, m_styles, m_media.width, nullptr, nullptr, m_media.height, 1,
        m_realm ? &embedded : nullptr);
    m_computed = true;
    m_mutations = mutations;
}

void LayoutOracle::with_fonts(std::function<void()> const& use)
{
    sheets_up_to_date();
    std::vector<text::FontManager::PageFace> page_faces;
    if (m_keep_page_fonts)
        page_faces = text::FontManager::instance().page_faces();
    text::FontManager::instance().set_page_fonts(m_fonts);
    struct FontsBack {
        bool wanted;
        std::vector<text::FontManager::PageFace>& faces;
        ~FontsBack()
        {
            if (wanted)
                text::FontManager::instance().restore_page_faces(std::move(faces));
        }
    } const fonts_back { m_keep_page_fonts, page_faces };
    use();
}

void LayoutOracle::sheets_up_to_date()
{
    // The sheets are parsed and compiled again only when the elements
    // carrying them changed; every mutation still cascades and lays out.
    std::string signature = sheet_signature(*m_document) + css::scripted_sheets_signature(*m_document);
    if (!m_style_set || signature != m_sheet_signature) {
        css::InlineSheetCheck check;
        if (m_policy) {
            check = [this](dom::Element const& style, std::string_view text) {
                dom::Attr const* const nonce = style.find_attribute("nonce");
                return !m_policy->inline_refusal(net::InlineKind::Style, nonce ? nonce->value : std::string(), text);
            };
        }
        std::vector<css::SheetSource> const sheets = css::collect_stylesheets(*m_document, &m_base, m_fetch, m_media, check);
        // The page's own fonts answer its measurements, as they do the
        // render: a test that measures text in Ahem and then sets a width
        // from it must measure in Ahem.
        m_fonts = m_fonts_on_demand.collect(sheets, m_media);
        net::Url const named_against = html::document_base_url(*m_document, m_base);
        m_style_set.emplace(sheets, m_media, &named_against);
        if (m_policy) {
            m_style_set->set_style_attribute_check([this](dom::Element const&, std::string_view text) {
                return !m_policy->inline_refusal(net::InlineKind::StyleAttribute, {}, text);
            });
        }
        m_sheet_signature = std::move(signature);
    }
}

std::optional<LayoutBox> LayoutOracle::box(dom::Element const& element)
{
    ensure();
    return find_element_box(m_layout.root, element, m_styles);
}

css::ComputedStyle const* LayoutOracle::style(dom::Element const& element)
{
    ensure();
    auto const it = m_styles.find(&element);
    return it == m_styles.end() ? nullptr : &it->second;
}

std::optional<ElementScroll> LayoutOracle::element_scroll(dom::Element const& element)
{
    ensure();
    layout::Fragment const* const fragment = find_fragment(m_layout.root, element);
    if (fragment == nullptr)
        return std::nullopt;
    return scroll_of_box(*fragment, &m_scrolls);
}

void LayoutOracle::scroll_element(dom::Element const& element, float x, float y)
{
    std::optional<ElementScroll> const at = element_scroll(element);
    if (!at || !at->scrolls)
        return;
    layout::ScrollOffset const want { std::clamp(x, 0.0f, at->range_x), std::clamp(y, 0.0f, at->range_y) };
    if (want.x == at->x && want.y == at->y)
        return;
    m_scrolls[&element] = want;
    if (m_realm != nullptr)
        m_realm->scrolled(&element);
}

dom::Element const* LayoutOracle::element_at(float x, float y)
{
    ensure();
    std::optional<paint::PointHit> const hit = paint::hit_test(m_layout.root, x, y, [this](dom::Element const& element) -> css::ComputedStyle const* {
        auto const it = m_styles.find(&element);
        return it == m_styles.end() ? nullptr : &it->second;
    });
    return hit ? hit->element : nullptr;
}

}
