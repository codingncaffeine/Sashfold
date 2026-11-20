#pragma once

// The two PageSources the engine's hosts have: one over a style map and a
// laid-out fragment tree (the renderer, the tests), and one over the
// bindings' LayoutOracle, which keeps styles and layout current with what
// the page's scripts did (the test runners, a headless realm).

#include "a11y/Semantics.h"
#include "css/StyleResolver.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace sashfold::layout {
struct Fragment;
}
namespace sashfold::bindings {
class LayoutOracle;
class Realm;
}

namespace sashfold::a11y {

class StyleMapSource final : public PageSource {
public:
    // Neither is copied: both must outlive the source.
    explicit StyleMapSource(css::StyleMap const& styles, layout::Fragment const* root = nullptr)
        : m_styles(&styles)
        , m_root(root)
    {
    }
    void set(css::StyleMap const& styles, layout::Fragment const* root)
    {
        m_styles = &styles;
        m_root = root;
        ++m_generation;
        m_index.reset();
    }
    css::ComputedStyle const* style(dom::Element const&) override;
    std::optional<Rect> box(dom::Element const&) override;
    std::vector<Rect> text_boxes(dom::Node const&, std::uint32_t, std::uint32_t) override;
    dom::Element const* element_at(float x, float y) override;
    std::uint64_t generation() const override { return m_generation; }

private:
    // Where each element's box and each text node's runs are, found in one
    // walk of the fragments the first time a box is asked for: a tree of
    // every node asks for every box, which a walk per question makes
    // quadratic.
    struct Index;
    Index const& index();
    css::StyleMap const* m_styles;
    layout::Fragment const* m_root;
    std::uint64_t m_generation = 1;
    std::shared_ptr<Index> m_index;
};

class OracleSource final : public PageSource {
public:
    // `scroll` is the page's scroll down, added to the viewport positions
    // the oracle answers with to give page positions.
    OracleSource(bindings::LayoutOracle& oracle, bindings::Realm* realm)
        : m_oracle(&oracle)
        , m_realm(realm)
    {
    }
    css::ComputedStyle const* style(dom::Element const&) override;
    std::optional<Rect> box(dom::Element const&) override;
    std::vector<Rect> text_boxes(dom::Node const&, std::uint32_t, std::uint32_t) override;
    dom::Element const* element_at(float x, float y) override;
    std::uint64_t generation() const override;

private:
    bindings::LayoutOracle* m_oracle;
    bindings::Realm* m_realm;
};

}
