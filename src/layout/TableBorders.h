#pragma once

// The collapsing border model (CSS 2.1 §17.6.2). The table, its column
// groups, columns, row groups, rows and cells all offer borders at the
// grid lines they touch; at every segment of every line one of them wins
// by §17.6.2.1, and the winner is drawn centred on the line — half of it
// in the box on each side.

#include "css/ComputedStyle.h"
#include "layout/TableStructure.h"

#include <vector>

namespace sashfold::layout::table {

// What won one segment of a grid line. A segment nothing claims, and one
// a `hidden` border took away, are both zero wide and drawn by nobody.
struct CollapsedEdge {
    float width = 0;
    Color color;
    bool drawn = false;
};

// Every segment of a table's grid lines. Horizontal line `line` runs
// above row `line` (there are rows + 1 of them, the last below the last
// row) and is cut into one segment per column; vertical line `line` runs
// left of column `line` (columns + 1 of them) and is cut into one
// segment per row.
struct CollapsedBorders {
    int rows = 0;
    int columns = 0;
    std::vector<CollapsedEdge> horizontal;
    std::vector<CollapsedEdge> vertical;

    CollapsedEdge const& above(int line, int column) const;
    CollapsedEdge const& left_of(int row, int line) const;

    // The widest segment over a stretch of a line — what a box that
    // spans that stretch takes as its used border width there. The
    // stretch is [from, to) in the axis the line is cut along.
    float widest_horizontal(int line, int from_column, int to_column) const;
    float widest_vertical(int line, int from_row, int to_row) const;
    // The color to draw a stretch with: the widest segment's.
    Color color_horizontal(int line, int from_column, int to_column) const;
    Color color_vertical(int line, int from_row, int to_row) const;
};

CollapsedBorders collapse_borders(Structure const& structure, css::ComputedStyle const& table_style);

}
