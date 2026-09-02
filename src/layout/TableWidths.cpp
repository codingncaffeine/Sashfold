#include "layout/TableWidths.h"

#include <algorithm>
#include <cstddef>

namespace sashfold::layout::table {

namespace {

std::size_t index(int i)
{
    return static_cast<std::size_t>(i);
}

// Shares `amount` among the columns in [first, last) that `eligible`, in
// proportion to `weight` — equally when the weights are all zero.
template<typename Eligible, typename Weight, typename Apply>
void share(std::size_t first, std::size_t last, float amount, Eligible const& eligible, Weight const& weight,
    Apply const& apply)
{
    float weight_sum = 0;
    std::size_t count = 0;
    for (std::size_t i = first; i < last; ++i) {
        if (eligible(i)) {
            weight_sum += weight(i);
            ++count;
        }
    }
    if (count == 0)
        return;
    for (std::size_t i = first; i < last; ++i) {
        if (!eligible(i))
            continue;
        float const part = weight_sum > 0 ? amount * weight(i) / weight_sum : amount / static_cast<float>(count);
        apply(i, part);
    }
}

} // namespace

WidthResult compute_widths(WidthInput const& input)
{
    std::size_t const n = input.columns.size();
    WidthResult result;
    result.columns.assign(n, 0.0f);
    float const gutters = input.spacing * static_cast<float>(n + 1);
    if (n == 0) {
        result.min = input.edges + gutters;
        result.max = result.min;
        result.width = std::max(input.width.value_or(0.0f), result.min);
        return result;
    }

    // A written width is a floor for both of a column's measures.
    std::vector<ColumnInput> columns = input.columns;
    for (ColumnInput& column : columns) {
        if (column.fixed) {
            column.min = std::max(column.min, *column.fixed);
            column.max = std::max(column.max, *column.fixed);
        }
        column.max = std::max(column.max, column.min);
    }

    // A spanning cell grows the columns it spans when they fall short
    // together, in proportion to their maximums; narrow spans first.
    std::vector<SpanInput> spans = input.spans;
    std::stable_sort(spans.begin(), spans.end(),
        [](SpanInput const& a, SpanInput const& b) { return a.span < b.span; });
    for (SpanInput const& span : spans) {
        std::size_t const first = index(std::max(0, span.column));
        std::size_t const last = std::min(n, first + index(std::max(1, span.span)));
        if (last <= first)
            continue;
        float const inner_gutters = input.spacing * static_cast<float>(last - first - 1);
        auto const grow = [&](float ColumnInput::*member, float need) {
            float sum = 0;
            for (std::size_t i = first; i < last; ++i)
                sum += columns[i].*member;
            float const shortfall = need - inner_gutters - sum;
            if (shortfall <= 0)
                return;
            share(
                first, last, shortfall, [](std::size_t) { return true; },
                [&](std::size_t i) { return columns[i].max; },
                [&](std::size_t i, float part) { columns[i].*member += part; });
        };
        float const need_min = std::max(span.min, span.fixed.value_or(0.0f));
        grow(&ColumnInput::min, need_min);
        grow(&ColumnInput::max, std::max(span.max, need_min));
        for (std::size_t i = first; i < last; ++i)
            columns[i].max = std::max(columns[i].max, columns[i].min);
    }

    float sum_min = 0;
    float sum_max = 0;
    for (ColumnInput const& column : columns) {
        sum_min += column.min;
        sum_max += column.max;
    }
    // Percentage columns can push the maximum up: the table must be wide
    // enough for each of them to be its share.
    float percent_sum = 0;
    float other_max = 0;
    for (ColumnInput const& column : columns) {
        if (column.percent)
            percent_sum += *column.percent;
        else
            other_max += column.max;
    }
    float max_from_percent = 0;
    if (percent_sum > 0 && percent_sum < 100)
        max_from_percent = other_max / (1.0f - percent_sum / 100.0f);
    for (ColumnInput const& column : columns) {
        if (column.percent && *column.percent > 0)
            max_from_percent = std::max(max_from_percent, column.max * 100.0f / *column.percent);
    }
    result.min = sum_min + gutters + input.edges;
    result.max = std::max(sum_max, max_from_percent) + gutters + input.edges;

    // The table's width: as written, never under its minimum (a fixed
    // layout ignores the content: the written width holds); else it
    // shrinks to fit the room.
    bool const fixed_layout = input.fixed_layout && input.width;
    if (fixed_layout)
        result.width = *input.width;
    else if (input.width)
        result.width = std::max(*input.width, result.min);
    else
        result.width = std::max(result.min, std::min(result.max, input.available));
    float const content = std::max(0.0f, result.width - input.edges - gutters);
    std::vector<float>& widths = result.columns;

    if (fixed_layout) {
        // Fixed layout: the written widths hold and the other columns share
        // what is left equally; the table grows if they overflow it.
        float used = 0;
        std::size_t free_count = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (input.columns[i].fixed)
                widths[i] = *input.columns[i].fixed;
            else if (input.columns[i].percent)
                widths[i] = content * *input.columns[i].percent / 100.0f;
            else {
                ++free_count;
                continue;
            }
            used += widths[i];
        }
        float const remaining = content - used;
        if (free_count > 0) {
            float const each = std::max(0.0f, remaining) / static_cast<float>(free_count);
            for (std::size_t i = 0; i < n; ++i) {
                if (!input.columns[i].fixed && !input.columns[i].percent)
                    widths[i] = each;
            }
        } else if (remaining > 0) {
            share(
                0, n, remaining, [](std::size_t) { return true; }, [&](std::size_t i) { return widths[i]; },
                [&](std::size_t i, float part) { widths[i] += part; });
        }
        float total = 0;
        for (float const width : widths)
            total += width;
        if (total > content)
            result.width = total + input.edges + gutters;
        return result;
    }

    // Automatic layout: percentage columns take their share first, written
    // widths hold, and the rest is the other columns' — their maximums when
    // it is enough, a point between their minimums and maximums when not.
    std::vector<bool> settled(n, false);
    float remaining = content;
    for (std::size_t i = 0; i < n; ++i) {
        if (columns[i].percent) {
            widths[i] = std::max(columns[i].min, content * *columns[i].percent / 100.0f);
            settled[i] = true;
            remaining -= widths[i];
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!settled[i] && columns[i].fixed) {
            widths[i] = columns[i].min;
            settled[i] = true;
            remaining -= widths[i];
        }
    }
    float auto_min = 0;
    float auto_max = 0;
    std::size_t auto_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!settled[i]) {
            auto_min += columns[i].min;
            auto_max += columns[i].max;
            ++auto_count;
        }
    }
    auto const is_auto = [&](std::size_t i) { return !settled[i]; };
    if (auto_count > 0 && remaining >= auto_max) {
        for (std::size_t i = 0; i < n; ++i) {
            if (is_auto(i))
                widths[i] = columns[i].max;
        }
        share(
            0, n, remaining - auto_max, is_auto, [&](std::size_t i) { return columns[i].max; },
            [&](std::size_t i, float part) { widths[i] += part; });
        return result;
    }
    if (auto_count > 0 && remaining >= auto_min) {
        float const range = auto_max - auto_min;
        float const t = range > 0 ? (remaining - auto_min) / range : 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            if (is_auto(i))
                widths[i] = columns[i].min + (columns[i].max - columns[i].min) * t;
        }
        return result;
    }
    if (auto_count > 0) {
        // The settled columns took too much: they give back toward their
        // minimums, in proportion to what they have to spare.
        for (std::size_t i = 0; i < n; ++i) {
            if (is_auto(i))
                widths[i] = columns[i].min;
        }
        float const over = auto_min - remaining;
        float slack = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (settled[i])
                slack += widths[i] - columns[i].min;
        }
        if (slack > 0) {
            float const fraction = std::min(1.0f, over / slack);
            for (std::size_t i = 0; i < n; ++i) {
                if (settled[i])
                    widths[i] -= (widths[i] - columns[i].min) * fraction;
            }
        }
        return result;
    }
    // No other columns: the difference goes to the written-width columns,
    // else to the percentage ones, in proportion to their widths.
    if (remaining > 0) {
        bool any_fixed = false;
        for (std::size_t i = 0; i < n; ++i)
            any_fixed = any_fixed || (columns[i].fixed && !columns[i].percent);
        share(
            0, n, remaining, [&](std::size_t i) { return !any_fixed || (columns[i].fixed && !columns[i].percent); },
            [&](std::size_t i) { return widths[i]; }, [&](std::size_t i, float part) { widths[i] += part; });
    } else if (remaining < 0) {
        float slack = 0;
        for (std::size_t i = 0; i < n; ++i)
            slack += widths[i] - columns[i].min;
        if (slack > 0) {
            float const fraction = std::min(1.0f, -remaining / slack);
            for (std::size_t i = 0; i < n; ++i)
                widths[i] -= (widths[i] - columns[i].min) * fraction;
        }
    }
    return result;
}

}
