#include "layout/TableStructure.h"

#include "dom/Dom.h"

#include <algorithm>
#include <optional>

namespace sashfold::layout::table {

namespace {

using css::ComputedStyle;
using css::Display;

bool is_blank_text(dom::Node const& node)
{
    if (!node.is_text())
        return false;
    for (char const c : static_cast<dom::Text const&>(node).data) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
            return false;
    }
    return true;
}

std::vector<dom::Node const*> children_of(dom::Element const& element)
{
    return std::vector<dom::Node const*>(element.children().begin(), element.children().end());
}

// A child element that takes part: its style and display. Nothing for
// display: none.
struct Part {
    dom::Element const* element = nullptr;
    ComputedStyle const* style = nullptr;
    Display display = Display::Inline;
};

struct Builder {
    StyleOf const& style_of;
    ComputedStyle const& table_style;
    Structure out;

    Builder(StyleOf const& the_style_of, ComputedStyle const& the_table_style)
        : style_of(the_style_of)
        , table_style(the_table_style)
    {
    }

    std::optional<Part> part_of(dom::Node const& node) const
    {
        if (!node.is_element())
            return std::nullopt;
        auto const& element = static_cast<dom::Element const&>(node);
        ComputedStyle const* const style = style_of(element);
        if (!style || style->display == Display::None)
            return std::nullopt;
        Display display = style->display;
        // An out-of-flow or floating box is not a table part: it is content
        // of an anonymous cell, which places it.
        if (style->out_of_flow() || style->floating != css::Float::None)
            display = Display::Block;
        return Part { &element, style, display };
    }

    // A row's cells: its children, or the run an anonymous row wraps. What
    // is not a cell gathers, with its neighbors, into an anonymous cell.
    void add_row(dom::Element const* element, ComputedStyle const* style,
        std::vector<dom::Node const*> const& children, std::size_t group)
    {
        Row row;
        row.element = element;
        row.style = style;
        row.group = group;
        std::vector<dom::Node const*> run;
        bool content = false;
        auto const flush = [&] {
            if (content) {
                Cell cell;
                cell.style = style;
                cell.nodes = run;
                out.cells.push_back(std::move(cell));
                row.cells.push_back(out.cells.size() - 1);
            }
            run.clear();
            content = false;
        };
        for (dom::Node const* child : children) {
            if (child->is_text()) {
                if (is_blank_text(*child)) {
                    if (!run.empty())
                        run.push_back(child);
                    continue;
                }
                run.push_back(child);
                content = true;
                continue;
            }
            std::optional<Part> const part = part_of(*child);
            if (!part)
                continue;
            if (part->display == Display::TableCell) {
                flush();
                Cell cell;
                cell.element = part->element;
                cell.style = part->style;
                cell.column_span = std::max(1, span_attribute(*part->element, "colspan", 1, 1000));
                cell.row_span = span_attribute(*part->element, "rowspan", 1, 65534);
                out.cells.push_back(std::move(cell));
                row.cells.push_back(out.cells.size() - 1);
                continue;
            }
            run.push_back(child);
            content = true;
        }
        flush();
        out.rows.push_back(std::move(row));
        out.groups[group].rows.push_back(out.rows.size() - 1);
    }

    // A row group's rows: its children, anything else wrapped in anonymous rows.
    void add_group(dom::Element const* element, ComputedStyle const* style, RowGroup::Kind kind,
        std::vector<dom::Node const*> const& children)
    {
        RowGroup group;
        group.element = element;
        group.style = style;
        group.kind = kind;
        out.groups.push_back(std::move(group));
        std::size_t const index = out.groups.size() - 1;
        std::vector<dom::Node const*> run;
        bool content = false;
        auto const flush = [&] {
            if (content)
                add_row(nullptr, style, run, index);
            run.clear();
            content = false;
        };
        for (dom::Node const* child : children) {
            if (child->is_text()) {
                if (is_blank_text(*child)) {
                    if (!run.empty())
                        run.push_back(child);
                    continue;
                }
                run.push_back(child);
                content = true;
                continue;
            }
            std::optional<Part> const part = part_of(*child);
            if (!part)
                continue;
            if (part->display == Display::TableRow) {
                flush();
                add_row(part->element, part->style, children_of(*part->element), index);
                continue;
            }
            run.push_back(child);
            content = true;
        }
        flush();
    }

    void add_columns(dom::Element const* element, ComputedStyle const* style, int count)
    {
        for (int i = 0; i < count; ++i)
            out.columns.push_back({ element, style });
    }

    // A column group: its col children, or its own span when it has none.
    void add_column_group(Part const& part)
    {
        ColumnGroup group;
        group.element = part.element;
        group.style = part.style;
        group.first = static_cast<int>(out.columns.size());
        bool any = false;
        for (dom::Node const* child : part.element->children()) {
            std::optional<Part> const column = part_of(*child);
            if (!column || column->display != Display::TableColumn)
                continue;
            any = true;
            add_columns(column->element, column->style, span_attribute(*column->element, "span", 1, 1000));
        }
        if (!any)
            add_columns(part.element, part.style, span_attribute(*part.element, "span", 1, 1000));
        group.count = static_cast<int>(out.columns.size()) - group.first;
        out.column_groups.push_back(group);
    }

    void build(std::vector<dom::Node const*> const& children)
    {
        std::optional<std::size_t> open_group; // the anonymous group holding loose rows
        std::vector<dom::Node const*> run;
        bool content = false;
        auto const ensure_group = [&] {
            if (!open_group) {
                RowGroup group;
                group.style = &table_style;
                out.groups.push_back(std::move(group));
                open_group = out.groups.size() - 1;
            }
            return *open_group;
        };
        auto const flush = [&] {
            if (content)
                add_row(nullptr, &table_style, run, ensure_group());
            run.clear();
            content = false;
        };
        for (dom::Node const* child : children) {
            if (child->is_text()) {
                if (is_blank_text(*child)) {
                    if (!run.empty())
                        run.push_back(child);
                    continue;
                }
                run.push_back(child);
                content = true;
                continue;
            }
            std::optional<Part> const part = part_of(*child);
            if (!part)
                continue;
            switch (part->display) {
            case Display::TableCaption:
                flush();
                out.captions.push_back({ part->element, part->style });
                break;
            case Display::TableColumnGroup:
                flush();
                add_column_group(*part);
                break;
            case Display::TableColumn:
                flush();
                add_columns(part->element, part->style, span_attribute(*part->element, "span", 1, 1000));
                break;
            case Display::TableRowGroup:
            case Display::TableHeaderGroup:
            case Display::TableFooterGroup:
                flush();
                open_group.reset();
                add_group(part->element, part->style,
                    part->display == Display::TableHeaderGroup ? RowGroup::Kind::Header
                        : part->display == Display::TableFooterGroup ? RowGroup::Kind::Footer
                                                                     : RowGroup::Kind::Body,
                    children_of(*part->element));
                break;
            case Display::TableRow:
                flush();
                add_row(part->element, part->style, children_of(*part->element), ensure_group());
                break;
            default:
                run.push_back(child);
                content = true;
                break;
            }
        }
        flush();
        order_groups();
        assign_slots();
    }

    // The first header group goes first and the first footer group last;
    // any others are bodies where they stand.
    void order_groups()
    {
        std::vector<std::size_t> order;
        std::optional<std::size_t> header;
        std::optional<std::size_t> footer;
        for (std::size_t g = 0; g < out.groups.size(); ++g) {
            if (out.groups[g].kind == RowGroup::Kind::Header && !header)
                header = g;
            else if (out.groups[g].kind == RowGroup::Kind::Footer && !footer)
                footer = g;
            else
                order.push_back(g);
        }
        if (header)
            order.insert(order.begin(), *header);
        if (footer)
            order.push_back(*footer);
        // Rebuild the rows in that order; the groups keep their identity.
        std::vector<Row> rows;
        std::vector<std::size_t> new_index(out.rows.size(), 0);
        for (std::size_t const g : order) {
            for (std::size_t const r : out.groups[g].rows) {
                new_index[r] = rows.size();
                rows.push_back(std::move(out.rows[r]));
            }
        }
        for (RowGroup& group : out.groups) {
            for (std::size_t& r : group.rows)
                r = new_index[r];
        }
        std::vector<RowGroup> groups;
        std::vector<std::size_t> new_group(out.groups.size(), 0);
        for (std::size_t const g : order) {
            new_group[g] = groups.size();
            groups.push_back(std::move(out.groups[g]));
        }
        for (Row& row : rows)
            row.group = new_group[row.group];
        out.rows = std::move(rows);
        out.groups = std::move(groups);
    }

    // Every cell takes the first free slot in its row past the previous
    // one, and holds the slots its spans cover in the rows below.
    void assign_slots()
    {
        auto const row_count = static_cast<int>(out.rows.size());
        std::vector<std::vector<int>> slots(static_cast<std::size_t>(row_count));
        auto const occupied = [&](int r, int c) {
            std::vector<int> const& row = slots[static_cast<std::size_t>(r)];
            return c < static_cast<int>(row.size()) && row[static_cast<std::size_t>(c)] >= 0;
        };
        auto const take = [&](int r, int c, int cell) {
            std::vector<int>& row = slots[static_cast<std::size_t>(r)];
            if (static_cast<int>(row.size()) <= c)
                row.resize(static_cast<std::size_t>(c) + 1, -1);
            row[static_cast<std::size_t>(c)] = cell;
        };
        for (int r = 0; r < row_count; ++r) {
            Row const& row = out.rows[static_cast<std::size_t>(r)];
            int column = 0;
            for (std::size_t const ci : row.cells) {
                Cell& cell = out.cells[ci];
                while (occupied(r, column) && column < 1000)
                    ++column;
                cell.row = r;
                cell.column = column;
                int span = cell.row_span;
                if (span == 0) {
                    // To the end of the row group.
                    int last = r;
                    for (std::size_t const other : out.groups[row.group].rows)
                        last = std::max(last, static_cast<int>(other));
                    span = last - r + 1;
                }
                span = std::max(1, std::min(span, row_count - r));
                cell.row_span = span;
                for (int rr = r; rr < r + span; ++rr) {
                    for (int cc = column; cc < column + cell.column_span; ++cc)
                        take(rr, cc, static_cast<int>(ci));
                }
                column += cell.column_span;
            }
        }
        int columns = static_cast<int>(out.columns.size());
        for (std::vector<int> const& row : slots)
            columns = std::max(columns, static_cast<int>(row.size()));
        for (std::vector<int>& row : slots)
            row.resize(static_cast<std::size_t>(columns), -1);
        out.column_count = columns;
        out.columns.resize(static_cast<std::size_t>(columns));
        out.slots = std::move(slots);
    }
};

} // namespace

int Structure::cell_at(int row, int column) const
{
    if (row < 0 || row >= static_cast<int>(slots.size()) || column < 0 || column >= column_count)
        return -1;
    return slots[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)];
}

int span_attribute(dom::Element const& element, char const* name, int fallback, int limit)
{
    dom::Attr const* const attribute = element.find_attribute(name);
    if (!attribute)
        return fallback;
    std::string_view text = attribute->value;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n'))
        text.remove_prefix(1);
    if (text.empty() || text.front() < '0' || text.front() > '9')
        return fallback;
    long value = 0;
    for (char const c : text) {
        if (c < '0' || c > '9')
            break;
        value = value * 10 + (c - '0');
        if (value > limit)
            return limit;
    }
    return static_cast<int>(value);
}

Structure build_structure(std::vector<dom::Node const*> const& children, css::ComputedStyle const& table_style,
    StyleOf const& style_of)
{
    Builder builder(style_of, table_style);
    builder.build(children);
    return std::move(builder.out);
}

}
