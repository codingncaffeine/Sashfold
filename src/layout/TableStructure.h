#pragma once

// The structure of a table (CSS 2.1 §17.2): the rows, cells, row groups,
// columns and captions its children make; the anonymous rows and cells the
// specification wraps around what is out of place; and the grid the cells
// occupy with their spans. No sizes here — the layouter reads the
// structure and measures.

#include "css/ComputedStyle.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace sashfold::dom {
class Element;
class Node;
}

namespace sashfold::layout::table {

using StyleOf = std::function<css::ComputedStyle const*(dom::Element const&)>;

// A cell: an element with display table-cell, or an anonymous cell around
// a run of a row's children that were not cells (`nodes`; their inherited
// values come from `style`, the box that would have held them).
struct Cell {
    dom::Element const* element = nullptr;
    css::ComputedStyle const* style = nullptr;
    std::vector<dom::Node const*> nodes;
    int row = 0;
    int column = 0;
    int row_span = 1;
    int column_span = 1;

    bool anonymous() const { return element == nullptr; }
};

struct Row {
    dom::Element const* element = nullptr; // null: anonymous
    css::ComputedStyle const* style = nullptr; // its own, or what it inherits from when anonymous
    std::vector<std::size_t> cells; // indexes into Structure::cells, in order
    std::size_t group = 0;
};

struct RowGroup {
    enum class Kind {
        Header,
        Body,
        Footer,
    };
    dom::Element const* element = nullptr; // null: anonymous
    css::ComputedStyle const* style = nullptr;
    Kind kind = Kind::Body;
    std::vector<std::size_t> rows; // indexes into Structure::rows
};

// A column of the grid, and the col (or the colgroup standing in for its
// columns) behind it, when one names it.
struct Column {
    dom::Element const* element = nullptr;
    css::ComputedStyle const* style = nullptr;
};

struct ColumnGroup {
    dom::Element const* element = nullptr;
    css::ComputedStyle const* style = nullptr;
    int first = 0;
    int count = 0;
};

struct Caption {
    dom::Element const* element = nullptr;
    css::ComputedStyle const* style = nullptr;
};

struct Structure {
    std::vector<Cell> cells;
    std::vector<Row> rows; // in display order: the header group's first, the footer group's last
    std::vector<RowGroup> groups; // likewise
    std::vector<Column> columns; // one per grid column
    std::vector<ColumnGroup> column_groups;
    std::vector<Caption> captions;
    int column_count = 0;
    // The grid: [row][column] holds the index of the cell there — a
    // spanned slot holds the spanning cell's — or -1 for an empty slot.
    std::vector<std::vector<int>> slots;

    int cell_at(int row, int column) const;
};

// Builds the structure from a table box's children: an element's, or the
// run of nodes an anonymous table wraps. `table_style` is the table box's
// own style, which its anonymous rows and cells inherit from.
Structure build_structure(std::vector<dom::Node const*> const& children, css::ComputedStyle const& table_style,
    StyleOf const& style_of);

// A span attribute (colspan, rowspan, span): a non-negative integer, else
// the fallback; clamped to the limit.
int span_attribute(dom::Element const& element, char const* name, int fallback, int limit);

}
