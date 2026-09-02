#pragma once

// The column widths of a table (CSS 2.1 §17.5.2) over plain numbers: the
// automatic layout from what the cells hold and what they are written to
// be, and the fixed layout from the written widths alone. Tested on its own.

#include <optional>
#include <vector>

namespace sashfold::layout::table {

// What a column's own cells (spanning one column) ask of it.
struct ColumnInput {
    float min = 0; // the widest thing its cells cannot break, outer
    float max = 0; // its cells on one line, outer
    std::optional<float> fixed; // the largest written width, px, outer
    std::optional<float> percent; // the largest written percentage, 0-100
};

// A cell spanning several columns.
struct SpanInput {
    int column = 0;
    int span = 2;
    float min = 0;
    float max = 0;
    std::optional<float> fixed;
    std::optional<float> percent;
};

struct WidthInput {
    std::vector<ColumnInput> columns;
    std::vector<SpanInput> spans;
    float spacing = 0; // the gutter between columns, and at both edges
    float edges = 0; // the table's border and padding, both sides together
    std::optional<float> width; // the table's width property, resolved: a border-box size
    float available = 0; // the containing block's width, for a table without one
    bool fixed_layout = false; // table-layout: fixed, honored with a written width
};

struct WidthResult {
    std::vector<float> columns; // the used widths, outer
    float width = 0; // the table's border-box width
    float min = 0; // the border-box intrinsic widths
    float max = 0;
};

WidthResult compute_widths(WidthInput const& input);

}
