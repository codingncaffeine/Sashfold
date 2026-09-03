#include "layout/TableBorders.h"

#include <algorithm>

namespace sashfold::layout::table {

namespace {

using css::BorderSide;
using css::BorderStyle;
using css::ComputedStyle;

// CSS 2.1 §17.6.2.1's last rule: when two borders are equally wide, the
// one on the box that comes first here wins.
enum Source {
    FromCell = 0,
    FromRow = 1,
    FromRowGroup = 2,
    FromColumn = 3,
    FromColumnGroup = 4,
    FromTable = 5,
};

struct Candidate {
    BorderSide const* side = nullptr;
    Source source = FromCell;
};

// §17.6.2.1: a `hidden` border takes the edge away outright; `none`
// loses to every other; else the widest wins, and equal widths go to the
// box that comes first in the list above. (Style priority — double over
// solid over dashed and the rest — decides nothing here: every visible
// style draws solid until the painter knows the others.)
CollapsedEdge resolve(std::vector<Candidate> const& candidates)
{
    for (Candidate const& candidate : candidates) {
        if (candidate.side && candidate.side->style == BorderStyle::Hidden)
            return CollapsedEdge {};
    }
    CollapsedEdge edge;
    Source winner = FromTable;
    for (Candidate const& candidate : candidates) {
        if (!candidate.side || candidate.side->style != BorderStyle::Solid || candidate.side->width <= 0)
            continue;
        bool const wider = candidate.side->width > edge.width;
        bool const sooner = candidate.side->width == edge.width && candidate.source < winner;
        if (!edge.drawn || wider || sooner) {
            edge.width = candidate.side->width;
            edge.color = candidate.side->color;
            edge.drawn = true;
            winner = candidate.source;
        }
    }
    return edge;
}

// A box's style, or nothing when the box is anonymous — an anonymous row
// or cell carries the style it inherits from, which is somebody else's
// and whose borders are not its own to offer.
ComputedStyle const* borders_of(dom::Element const* element, ComputedStyle const* style)
{
    return element ? style : nullptr;
}

}

CollapsedEdge const& CollapsedBorders::above(int line, int column) const
{
    static CollapsedEdge const nothing;
    if (line < 0 || line > rows || column < 0 || column >= columns)
        return nothing;
    return horizontal[static_cast<std::size_t>(line) * static_cast<std::size_t>(columns)
        + static_cast<std::size_t>(column)];
}

CollapsedEdge const& CollapsedBorders::left_of(int row, int line) const
{
    static CollapsedEdge const nothing;
    if (row < 0 || row >= rows || line < 0 || line > columns)
        return nothing;
    return vertical[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns + 1)
        + static_cast<std::size_t>(line)];
}

float CollapsedBorders::widest_horizontal(int line, int from_column, int to_column) const
{
    float widest = 0;
    for (int column = from_column; column < to_column; ++column)
        widest = std::max(widest, above(line, column).width);
    return widest;
}

float CollapsedBorders::widest_vertical(int line, int from_row, int to_row) const
{
    float widest = 0;
    for (int row = from_row; row < to_row; ++row)
        widest = std::max(widest, left_of(row, line).width);
    return widest;
}

Color CollapsedBorders::color_horizontal(int line, int from_column, int to_column) const
{
    Color color;
    float widest = -1;
    for (int column = from_column; column < to_column; ++column) {
        CollapsedEdge const& edge = above(line, column);
        if (edge.drawn && edge.width > widest) {
            widest = edge.width;
            color = edge.color;
        }
    }
    return color;
}

Color CollapsedBorders::color_vertical(int line, int from_row, int to_row) const
{
    Color color;
    float widest = -1;
    for (int row = from_row; row < to_row; ++row) {
        CollapsedEdge const& edge = left_of(row, line);
        if (edge.drawn && edge.width > widest) {
            widest = edge.width;
            color = edge.color;
        }
    }
    return color;
}

CollapsedBorders collapse_borders(Structure const& structure, ComputedStyle const& table_style)
{
    CollapsedBorders borders;
    borders.rows = static_cast<int>(structure.rows.size());
    borders.columns = structure.column_count;
    int const rows = borders.rows;
    int const columns = borders.columns;
    if (rows <= 0 || columns <= 0)
        return borders;
    borders.horizontal.assign(
        static_cast<std::size_t>(rows + 1) * static_cast<std::size_t>(columns), CollapsedEdge {});
    borders.vertical.assign(
        static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns + 1), CollapsedEdge {});

    auto const at = [](int i) { return static_cast<std::size_t>(i); };
    auto const row_group = [&](int row) -> RowGroup const* {
        std::size_t const group = structure.rows[at(row)].group;
        return group < structure.groups.size() ? &structure.groups[group] : nullptr;
    };
    auto const first_of_group = [&](int row) {
        RowGroup const* group = row_group(row);
        return group && !group->rows.empty() && group->rows.front() == at(row);
    };
    auto const last_of_group = [&](int row) {
        RowGroup const* group = row_group(row);
        return group && !group->rows.empty() && group->rows.back() == at(row);
    };
    auto const column_group = [&](int column) -> ColumnGroup const* {
        for (ColumnGroup const& group : structure.column_groups) {
            if (column >= group.first && column < group.first + group.count)
                return &group;
        }
        return nullptr;
    };
    auto const column_style = [&](int column) -> ComputedStyle const* {
        if (column < 0 || at(column) >= structure.columns.size())
            return nullptr;
        Column const& own = structure.columns[at(column)];
        return borders_of(own.element, own.style);
    };
    auto const row_style = [&](int row) -> ComputedStyle const* {
        Row const& own = structure.rows[at(row)];
        return borders_of(own.element, own.style);
    };
    auto const group_style = [&](int row) -> ComputedStyle const* {
        RowGroup const* group = row_group(row);
        return group ? borders_of(group->element, group->style) : nullptr;
    };
    auto const column_group_style = [&](int column) -> ComputedStyle const* {
        ColumnGroup const* group = column_group(column);
        return group ? borders_of(group->element, group->style) : nullptr;
    };
    // The cell whose edge lies on this line, or a marker that a cell
    // spans straight across it — a line inside a spanning cell carries no
    // border at all.
    struct Reach {
        ComputedStyle const* style = nullptr;
        bool crossed = false;
    };
    auto const cell_style = [&](int row, int column) -> ComputedStyle const* {
        int const index = structure.cell_at(row, column);
        if (index < 0)
            return nullptr;
        Cell const& cell = structure.cells[at(index)];
        return cell.anonymous() ? nullptr : cell.style;
    };
    auto const above_line = [&](int line, int column) {
        Reach reach;
        if (line <= 0)
            return reach;
        int const index = structure.cell_at(line - 1, column);
        if (index < 0)
            return reach;
        Cell const& cell = structure.cells[at(index)];
        if (cell.row + cell.row_span != line) {
            reach.crossed = true;
            return reach;
        }
        reach.style = cell_style(line - 1, column);
        return reach;
    };
    auto const below_line = [&](int line, int column) {
        Reach reach;
        if (line >= rows)
            return reach;
        int const index = structure.cell_at(line, column);
        if (index < 0)
            return reach;
        Cell const& cell = structure.cells[at(index)];
        if (cell.row != line) {
            reach.crossed = true;
            return reach;
        }
        reach.style = cell_style(line, column);
        return reach;
    };
    auto const before_line = [&](int row, int line) {
        Reach reach;
        if (line <= 0)
            return reach;
        int const index = structure.cell_at(row, line - 1);
        if (index < 0)
            return reach;
        Cell const& cell = structure.cells[at(index)];
        if (cell.column + cell.column_span != line) {
            reach.crossed = true;
            return reach;
        }
        reach.style = cell_style(row, line - 1);
        return reach;
    };
    auto const after_line = [&](int row, int line) {
        Reach reach;
        if (line >= columns)
            return reach;
        int const index = structure.cell_at(row, line);
        if (index < 0)
            return reach;
        Cell const& cell = structure.cells[at(index)];
        if (cell.column != line) {
            reach.crossed = true;
            return reach;
        }
        reach.style = cell_style(row, line);
        return reach;
    };

    std::vector<Candidate> candidates;
    auto const offer = [&](ComputedStyle const* style, BorderSide const ComputedStyle::*side, Source source) {
        if (style)
            candidates.push_back({ &(style->*side), source });
    };

    for (int line = 0; line <= rows; ++line) {
        for (int column = 0; column < columns; ++column) {
            Reach const over = above_line(line, column);
            Reach const under = below_line(line, column);
            if (over.crossed || under.crossed)
                continue; // a cell spans across: the line is inside it
            candidates.clear();
            offer(over.style, &ComputedStyle::border_bottom, FromCell);
            offer(under.style, &ComputedStyle::border_top, FromCell);
            if (line > 0) {
                offer(row_style(line - 1), &ComputedStyle::border_bottom, FromRow);
                if (last_of_group(line - 1))
                    offer(group_style(line - 1), &ComputedStyle::border_bottom, FromRowGroup);
            }
            if (line < rows) {
                offer(row_style(line), &ComputedStyle::border_top, FromRow);
                if (first_of_group(line))
                    offer(group_style(line), &ComputedStyle::border_top, FromRowGroup);
            }
            if (line == 0) {
                offer(column_style(column), &ComputedStyle::border_top, FromColumn);
                offer(column_group_style(column), &ComputedStyle::border_top, FromColumnGroup);
                offer(&table_style, &ComputedStyle::border_top, FromTable);
            }
            if (line == rows) {
                offer(column_style(column), &ComputedStyle::border_bottom, FromColumn);
                offer(column_group_style(column), &ComputedStyle::border_bottom, FromColumnGroup);
                offer(&table_style, &ComputedStyle::border_bottom, FromTable);
            }
            borders.horizontal[at(line) * at(columns) + at(column)] = resolve(candidates);
        }
    }

    for (int row = 0; row < rows; ++row) {
        for (int line = 0; line <= columns; ++line) {
            Reach const before = before_line(row, line);
            Reach const after = after_line(row, line);
            if (before.crossed || after.crossed)
                continue;
            candidates.clear();
            offer(before.style, &ComputedStyle::border_right, FromCell);
            offer(after.style, &ComputedStyle::border_left, FromCell);
            if (line > 0) {
                offer(column_style(line - 1), &ComputedStyle::border_right, FromColumn);
                ColumnGroup const* group = column_group(line - 1);
                if (group && line - 1 == group->first + group->count - 1)
                    offer(column_group_style(line - 1), &ComputedStyle::border_right, FromColumnGroup);
            }
            if (line < columns) {
                offer(column_style(line), &ComputedStyle::border_left, FromColumn);
                ColumnGroup const* group = column_group(line);
                if (group && line == group->first)
                    offer(column_group_style(line), &ComputedStyle::border_left, FromColumnGroup);
            }
            if (line == 0) {
                offer(row_style(row), &ComputedStyle::border_left, FromRow);
                offer(group_style(row), &ComputedStyle::border_left, FromRowGroup);
                offer(&table_style, &ComputedStyle::border_left, FromTable);
            }
            if (line == columns) {
                offer(row_style(row), &ComputedStyle::border_right, FromRow);
                offer(group_style(row), &ComputedStyle::border_right, FromRowGroup);
                offer(&table_style, &ComputedStyle::border_right, FromTable);
            }
            borders.vertical[at(row) * at(columns + 1) + at(line)] = resolve(candidates);
        }
    }
    return borders;
}

}
