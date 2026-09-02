#include "layout/GridAlgorithm.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>

namespace sashfold::layout::grid {

namespace {

constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr int track_limit = 10000; // the implicit grid never grows past this many tracks

std::size_t index(int i)
{
    return static_cast<std::size_t>(i);
}

} // namespace

bool AxisLines::has(int line, std::string const& name) const
{
    if (line < 0 || line >= static_cast<int>(names.size()))
        return false;
    for (std::string const& candidate : names[index(line)]) {
        if (candidate == name)
            return true;
    }
    return false;
}

// --- Placement (§8.3) -----------------------------------------------------------

AxisPlacement resolve_lines(css::GridLine const& start_in, css::GridLine const& end_in, AxisLines const& lines)
{
    using Kind = css::GridLine::Kind;
    css::GridLine const& start = start_in;
    css::GridLine end = end_in;
    // Two spans: the end's is dropped.
    if (start.kind == Kind::Span && end.kind == Kind::Span)
        end = css::GridLine {};
    int const count = lines.tracks() + 1; // the explicit lines: 0 .. count-1

    // A line by number, by the nth line with a name, or by a name alone.
    // Lines beyond the explicit grid all carry every name.
    auto const resolve_line = [&](css::GridLine const& line, bool start_side) -> std::optional<int> {
        if (line.kind == Kind::Line) {
            if (line.name.empty())
                return line.number > 0 ? line.number - 1 : count + line.number;
            if (line.number > 0) {
                int seen = 0;
                for (int i = 0; i < count; ++i) {
                    if (lines.has(i, line.name) && ++seen == line.number)
                        return i;
                }
                return count - 1 + (line.number - seen);
            }
            int const wanted = -line.number;
            int seen = 0;
            for (int i = count - 1; i >= 0; --i) {
                if (lines.has(i, line.name) && ++seen == wanted)
                    return i;
            }
            return -(wanted - seen);
        }
        if (line.kind == Kind::Name) {
            std::string const edge = line.name + (start_side ? "-start" : "-end");
            for (int i = 0; i < count; ++i) {
                if (lines.has(i, edge))
                    return i;
            }
            for (int i = 0; i < count; ++i) {
                if (lines.has(i, line.name))
                    return i;
            }
            return count; // the first implicit line past the explicit grid
        }
        return std::nullopt;
    };
    // The line a span reaches from a definite one: so many tracks, or the
    // nth line with a name in that direction (implicit lines all have it).
    auto const span_from = [&](css::GridLine const& span, int from, bool forward) -> int {
        int const steps = std::max(1, span.number);
        if (span.name.empty())
            return forward ? from + steps : from - steps;
        int seen = 0;
        if (forward) {
            for (int i = from + 1; i < from + 1 + track_limit; ++i) {
                if ((i >= count || lines.has(i, span.name)) && ++seen == steps)
                    return i;
            }
            return from + steps;
        }
        for (int i = from - 1; i > from - 1 - track_limit; --i) {
            if ((i < 0 || lines.has(i, span.name)) && ++seen == steps)
                return i;
        }
        return from - steps;
    };

    AxisPlacement result;
    std::optional<int> s = resolve_line(start, true);
    std::optional<int> e = resolve_line(end, false);
    if (s && e) {
        if (*e < *s)
            std::swap(*s, *e);
        if (*e == *s)
            *e = *s + 1;
        result.start = s;
        result.end = e;
        return result;
    }
    if (s) {
        result.start = s;
        result.end = end.kind == Kind::Span ? span_from(end, *s, true) : *s + 1;
        return result;
    }
    if (e) {
        result.end = e;
        result.start = start.kind == Kind::Span ? span_from(start, *e, false) : *e - 1;
        return result;
    }
    // Neither line is definite: a span, of one track when it names a line.
    if (start.kind == Kind::Span)
        result.span = start.name.empty() ? std::max(1, start.number) : 1;
    else if (end.kind == Kind::Span)
        result.span = end.name.empty() ? std::max(1, end.number) : 1;
    return result;
}

// --- Auto-placement (§8.5) -----------------------------------------------------

PlacedGrid place_items(std::vector<ItemPlacement> const& items, int explicit_rows, int explicit_columns,
    css::GridAutoFlow flow)
{
    bool const column_flow
        = flow == css::GridAutoFlow::Column || flow == css::GridAutoFlow::ColumnDense;
    bool const dense = flow == css::GridAutoFlow::RowDense || flow == css::GridAutoFlow::ColumnDense;
    // The major axis is the one the cursor moves along slowly: rows for a
    // row flow (items fill a row, then the next).
    auto const major_of = [&](ItemPlacement const& item) -> AxisPlacement const& {
        return column_flow ? item.columns : item.rows;
    };
    auto const minor_of = [&](ItemPlacement const& item) -> AxisPlacement const& {
        return column_flow ? item.rows : item.columns;
    };
    int const explicit_major = column_flow ? explicit_columns : explicit_rows;
    int const explicit_minor = column_flow ? explicit_rows : explicit_columns;

    // Lines before the explicit grid shift everything.
    int major_min = 0;
    int minor_min = 0;
    for (ItemPlacement const& item : items) {
        if (major_of(item).definite())
            major_min = std::min(major_min, *major_of(item).start);
        if (minor_of(item).definite())
            minor_min = std::min(minor_min, *minor_of(item).start);
    }
    int const major_offset = -major_min;
    int const minor_offset = -minor_min;
    int major_count = std::min(track_limit, explicit_major + major_offset);
    int minor_count = std::min(track_limit, explicit_minor + minor_offset);
    for (ItemPlacement const& item : items) {
        if (major_of(item).definite())
            major_count = std::max(major_count, *major_of(item).end + major_offset);
        if (minor_of(item).definite())
            minor_count = std::max(minor_count, *minor_of(item).end + minor_offset);
        else
            minor_count = std::max(minor_count, minor_of(item).span);
    }
    major_count = std::min(major_count, track_limit);
    minor_count = std::min(minor_count, track_limit);

    std::vector<std::vector<bool>> cells; // [major][minor]
    auto const ensure_major = [&](int count) {
        count = std::min(count, track_limit);
        while (static_cast<int>(cells.size()) < count)
            cells.emplace_back(index(minor_count), false);
    };
    auto const ensure_minor = [&](int count) {
        count = std::min(count, track_limit);
        if (count <= minor_count)
            return;
        minor_count = count;
        for (std::vector<bool>& row : cells)
            row.resize(index(minor_count), false);
    };
    ensure_major(std::max(major_count, 1));
    auto const occupied = [&](int ms, int me, int ns, int ne) {
        for (int m = ms; m < me; ++m) {
            if (m >= static_cast<int>(cells.size()))
                break;
            for (int n = ns; n < ne; ++n) {
                if (n < static_cast<int>(cells[index(m)].size()) && cells[index(m)][index(n)])
                    return true;
            }
        }
        return false;
    };
    auto const occupy = [&](int ms, int me, int ns, int ne) {
        ensure_major(me);
        ensure_minor(ne);
        for (int m = ms; m < std::min(me, static_cast<int>(cells.size())); ++m) {
            for (int n = ns; n < std::min(ne, minor_count); ++n)
                cells[index(m)][index(n)] = true;
        }
    };

    struct Slot {
        int major_start = 0;
        int major_end = 1;
        int minor_start = 0;
        int minor_end = 1;
    };
    std::vector<std::optional<Slot>> placed(items.size());

    // 1. Items with a definite position in both axes.
    for (std::size_t i = 0; i < items.size(); ++i) {
        AxisPlacement const& major = major_of(items[i]);
        AxisPlacement const& minor = minor_of(items[i]);
        if (!major.definite() || !minor.definite())
            continue;
        Slot const slot { *major.start + major_offset, *major.end + major_offset, *minor.start + minor_offset,
            *minor.end + minor_offset };
        occupy(slot.major_start, slot.major_end, slot.minor_start, slot.minor_end);
        placed[i] = slot;
    }

    // 2. Items locked to a major line: the first free run of cells in it —
    // past the items this step already put there, unless packing densely.
    std::map<int, int> cursor_by_major;
    for (std::size_t i = 0; i < items.size(); ++i) {
        AxisPlacement const& major = major_of(items[i]);
        AxisPlacement const& minor = minor_of(items[i]);
        if (!major.definite() || minor.definite())
            continue;
        int const ms = *major.start + major_offset;
        int const me = *major.end + major_offset;
        int const span = std::max(1, minor.span);
        int start = dense ? 0 : cursor_by_major[ms];
        while (occupied(ms, me, start, start + span) && start < track_limit)
            ++start;
        Slot const slot { ms, me, start, start + span };
        occupy(slot.major_start, slot.major_end, slot.minor_start, slot.minor_end);
        placed[i] = slot;
        if (!dense)
            cursor_by_major[ms] = start + span;
    }

    // 3. The minor axis is settled now. 4. The rest flow from a cursor.
    int cursor_major = 0;
    int cursor_minor = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (placed[i])
            continue;
        AxisPlacement const& major = major_of(items[i]);
        AxisPlacement const& minor = minor_of(items[i]);
        if (minor.definite()) {
            // Locked to a minor line: down the major axis to the first fit.
            int const ns = *minor.start + minor_offset;
            int const ne = *minor.end + minor_offset;
            int const span = std::max(1, major.span);
            if (dense) {
                cursor_major = 0;
            } else if (ns < cursor_minor) {
                ++cursor_major;
            }
            cursor_minor = ns;
            int m = cursor_major;
            while (occupied(m, m + span, ns, ne) && m < track_limit)
                ++m;
            Slot const slot { m, m + span, ns, ne };
            occupy(slot.major_start, slot.major_end, slot.minor_start, slot.minor_end);
            placed[i] = slot;
            cursor_major = m;
            continue;
        }
        int const span_major = std::max(1, major.span);
        int const span_minor = std::max(1, minor.span);
        if (dense) {
            cursor_major = 0;
            cursor_minor = 0;
        }
        for (int guard = 0; guard < track_limit * 4; ++guard) {
            if (cursor_minor + span_minor > minor_count) {
                ++cursor_major;
                cursor_minor = 0;
                continue;
            }
            if (!occupied(cursor_major, cursor_major + span_major, cursor_minor, cursor_minor + span_minor))
                break;
            ++cursor_minor;
        }
        Slot const slot { cursor_major, cursor_major + span_major, cursor_minor, cursor_minor + span_minor };
        occupy(slot.major_start, slot.major_end, slot.minor_start, slot.minor_end);
        placed[i] = slot;
    }

    PlacedGrid result;
    result.areas.reserve(items.size());
    int last_major = major_count;
    int last_minor = minor_count;
    for (std::optional<Slot> const& slot : placed) {
        Slot const s = slot.value_or(Slot {});
        last_major = std::max(last_major, s.major_end);
        last_minor = std::max(last_minor, s.minor_end);
        Area area;
        if (column_flow) {
            area.column_start = s.major_start;
            area.column_end = s.major_end;
            area.row_start = s.minor_start;
            area.row_end = s.minor_end;
        } else {
            area.row_start = s.major_start;
            area.row_end = s.major_end;
            area.column_start = s.minor_start;
            area.column_end = s.minor_end;
        }
        result.areas.push_back(area);
    }
    if (column_flow) {
        result.columns = last_major;
        result.rows = last_minor;
        result.column_offset = major_offset;
        result.row_offset = minor_offset;
    } else {
        result.rows = last_major;
        result.columns = last_minor;
        result.row_offset = major_offset;
        result.column_offset = minor_offset;
    }
    result.rows = std::max(result.rows, 1);
    result.columns = std::max(result.columns, 1);
    return result;
}

// --- Track sizing (§11) --------------------------------------------------------

std::vector<float> size_tracks(SizingInput const& input)
{
    std::vector<Track> const& tracks = input.tracks;
    std::size_t const n = tracks.size();
    if (n == 0)
        return {};
    std::vector<float> base(n, 0.0f);
    std::vector<float> limit(n, infinity);
    std::vector<bool> growable(n, false); // "infinitely growable" for the next step

    auto const flexible = [&](std::size_t t) { return tracks[t].max.kind == Breadth::Kind::Flex; };
    auto const intrinsic_min = [&](std::size_t t) { return tracks[t].min.intrinsic(); };
    auto const intrinsic_max = [&](std::size_t t) { return tracks[t].max.intrinsic(); };
    auto const max_content_max = [&](std::size_t t) {
        return tracks[t].max.kind == Breadth::Kind::MaxContent || tracks[t].max.kind == Breadth::Kind::Auto;
    };
    // A fit-content() track behaves as max-content until it reaches its
    // cap, then as fixed at the cap.
    auto const fit_cap = [&](std::size_t t) -> std::optional<float> {
        if (tracks[t].fit_content)
            return *tracks[t].fit_content;
        if (tracks[t].max.kind == Breadth::Kind::Fixed)
            return tracks[t].max.value;
        return std::nullopt;
    };
    auto const gaps_in = [&](int start, int end) {
        int count = 0;
        for (int t = start; t < end; ++t) {
            if (!tracks[index(t)].collapsed)
                ++count;
        }
        return count > 1 ? input.gap * static_cast<float>(count - 1) : 0.0f;
    };
    auto const spans_flexible = [&](Contribution const& item) {
        for (int t = item.start; t < item.end; ++t) {
            if (flexible(index(t)))
                return true;
        }
        return false;
    };
    // The limited min-/max-content contribution: held under the sum of
    // the fixed maximums when the item spans only such tracks, and never
    // under the minimum contribution.
    auto const limited = [&](Contribution const& item, float contribution) {
        float sum = 0;
        bool all_fixed = true;
        for (int t = item.start; t < item.end; ++t) {
            if (std::optional<float> const cap = fit_cap(index(t)))
                sum += *cap;
            else
                all_fixed = false;
        }
        if (all_fixed)
            contribution = std::min(contribution, sum + gaps_in(item.start, item.end));
        return std::max(contribution, item.minimum);
    };

    // 11.4: initial base sizes and growth limits.
    for (std::size_t t = 0; t < n; ++t) {
        if (tracks[t].collapsed) {
            base[t] = 0;
            limit[t] = 0;
            continue;
        }
        base[t] = tracks[t].min.kind == Breadth::Kind::Fixed ? std::max(0.0f, tracks[t].min.value) : 0.0f;
        limit[t] = tracks[t].max.kind == Breadth::Kind::Fixed ? std::max(0.0f, tracks[t].max.value) : infinity;
        if (limit[t] < base[t])
            limit[t] = base[t];
    }

    // 11.5.1: distribute extra space to tracks. Each item's need is met
    // by growing the affected tracks it spans equally, up to their limits
    // first, then beyond them where the sizing function allows; a track
    // ends up with the largest increase any one item asked of it.
    // Returns the tracks whose growth limit went from infinite to finite.
    auto const distribute = [&](std::vector<Contribution const*> const& group, auto const& affected,
                                auto const& contribution_of, bool to_limits, auto const& beyond,
                                bool flex_weighted) {
        std::vector<float> planned(n, 0.0f);
        std::vector<bool> became_finite(n, false);
        auto const current_of = [&](std::size_t t) {
            if (to_limits)
                return limit[t] == infinity ? base[t] : limit[t];
            return base[t];
        };
        auto const cap_of = [&](std::size_t t) {
            if (to_limits)
                return (growable[t] || limit[t] == infinity) ? infinity : limit[t];
            float cap = limit[t];
            if (tracks[t].fit_content)
                cap = std::min(cap, std::max(base[t], *tracks[t].fit_content));
            return cap;
        };
        for (Contribution const* item : group) {
            std::vector<std::size_t> affected_tracks;
            for (int t = item->start; t < item->end; ++t) {
                if (affected(index(t)) && !tracks[index(t)].collapsed)
                    affected_tracks.push_back(index(t));
            }
            if (affected_tracks.empty())
                continue;
            float space = contribution_of(*item) - gaps_in(item->start, item->end);
            for (int t = item->start; t < item->end; ++t)
                space -= current_of(index(t));
            if (space <= 0)
                continue;
            // The shares: equal, or by flex factor when the tracks are
            // flexible (factors summing under one leave the rest equal).
            std::vector<float> weight(n, 0.0f);
            {
                float factor_sum = 0;
                for (std::size_t const t : affected_tracks)
                    factor_sum += flex_weighted ? tracks[t].max.value : 0.0f;
                auto const count = static_cast<float>(affected_tracks.size());
                for (std::size_t const t : affected_tracks) {
                    if (!flex_weighted)
                        weight[t] = 1.0f;
                    else if (factor_sum >= 1)
                        weight[t] = tracks[t].max.value / factor_sum;
                    else
                        weight[t] = tracks[t].max.value + (1.0f - factor_sum) / count;
                }
            }
            std::vector<float> increase(n, 0.0f);
            std::vector<bool> frozen(n, false);
            float remaining = space;
            // Up to limits.
            for (int round = 0; round < 64 && remaining > 0.0001f; ++round) {
                float weight_sum = 0;
                std::size_t open = 0;
                for (std::size_t const t : affected_tracks) {
                    if (!frozen[t]) {
                        weight_sum += weight[t];
                        ++open;
                    }
                }
                if (open == 0 || weight_sum <= 0)
                    break;
                float given = 0;
                bool any_frozen = false;
                for (std::size_t const t : affected_tracks) {
                    if (frozen[t])
                        continue;
                    float share = remaining * weight[t] / weight_sum;
                    float const room = cap_of(t) - (current_of(t) + increase[t]);
                    if (share >= room) {
                        share = std::max(room, 0.0f);
                        frozen[t] = true;
                        any_frozen = true;
                    }
                    increase[t] += share;
                    given += share;
                }
                remaining -= given;
                if (!any_frozen)
                    break;
            }
            // Beyond limits.
            if (remaining > 0.0001f) {
                std::vector<std::size_t> targets;
                for (std::size_t const t : affected_tracks) {
                    bool ok = beyond(t);
                    // A fit-content() track past its cap is fixed there.
                    if (ok && tracks[t].fit_content && !to_limits
                        && current_of(t) + increase[t] >= *tracks[t].fit_content - 0.0001f)
                        ok = false;
                    if (ok)
                        targets.push_back(t);
                }
                if (targets.empty())
                    targets = affected_tracks;
                float weight_sum = 0;
                for (std::size_t const t : targets)
                    weight_sum += weight[t];
                if (weight_sum > 0) {
                    for (std::size_t const t : targets)
                        increase[t] += remaining * weight[t] / weight_sum;
                }
            }
            for (std::size_t const t : affected_tracks)
                planned[t] = std::max(planned[t], increase[t]);
        }
        for (std::size_t t = 0; t < n; ++t) {
            if (planned[t] <= 0)
                continue;
            if (to_limits) {
                if (limit[t] == infinity) {
                    limit[t] = base[t] + planned[t];
                    became_finite[t] = true;
                } else {
                    limit[t] += planned[t];
                }
            } else {
                base[t] += planned[t];
            }
        }
        return became_finite;
    };

    // 11.5 step 1: items spanning one track that is not flexible.
    for (Contribution const& item : input.items) {
        if (item.end - item.start != 1 || spans_flexible(item))
            continue;
        std::size_t const t = index(item.start);
        if (t >= n || tracks[t].collapsed)
            continue;
        switch (tracks[t].min.kind) {
        case Breadth::Kind::MinContent:
            base[t] = std::max(base[t], item.min_content);
            break;
        case Breadth::Kind::MaxContent:
            base[t] = std::max(base[t], item.max_content);
            break;
        case Breadth::Kind::Auto:
            base[t] = std::max(base[t],
                input.constraint == Constraint::Definite ? item.minimum : limited(item, item.min_content));
            break;
        case Breadth::Kind::Fixed:
        case Breadth::Kind::Flex:
            break;
        }
        auto const raise_limit = [&](float value) {
            limit[t] = limit[t] == infinity ? value : std::max(limit[t], value);
        };
        switch (tracks[t].max.kind) {
        case Breadth::Kind::MinContent:
            raise_limit(item.min_content);
            break;
        case Breadth::Kind::MaxContent:
        case Breadth::Kind::Auto:
            raise_limit(item.max_content);
            if (tracks[t].fit_content)
                limit[t] = std::min(limit[t], *tracks[t].fit_content);
            break;
        case Breadth::Kind::Fixed:
        case Breadth::Kind::Flex:
            break;
        }
        if (limit[t] < base[t])
            limit[t] = base[t];
    }

    // 11.5 step 2: items spanning more tracks, none flexible, by span.
    {
        int longest = 1;
        for (Contribution const& item : input.items)
            longest = std::max(longest, item.end - item.start);
        for (int span = 2; span <= longest; ++span) {
            std::vector<Contribution const*> group;
            for (Contribution const& item : input.items) {
                if (item.end - item.start == span && !spans_flexible(item))
                    group.push_back(&item);
            }
            if (group.empty())
                continue;
            auto const minimum_of = [&](Contribution const& item) {
                return input.constraint == Constraint::Definite ? item.minimum : limited(item, item.min_content);
            };
            auto const min_content_of = [](Contribution const& item) { return item.min_content; };
            auto const max_content_of = [](Contribution const& item) { return item.max_content; };
            auto const limited_max_of = [&](Contribution const& item) { return limited(item, item.max_content); };
            distribute(group, intrinsic_min, minimum_of, false, intrinsic_max, false);
            distribute(group, [&](std::size_t t) {
                return tracks[t].min.kind == Breadth::Kind::MinContent
                    || tracks[t].min.kind == Breadth::Kind::MaxContent;
            }, min_content_of, false, intrinsic_max, false);
            if (input.constraint == Constraint::MaxContent) {
                distribute(group, [&](std::size_t t) {
                    return tracks[t].min.kind == Breadth::Kind::Auto
                        || tracks[t].min.kind == Breadth::Kind::MaxContent;
                }, limited_max_of, false, max_content_max, false);
            }
            distribute(group, [&](std::size_t t) { return tracks[t].min.kind == Breadth::Kind::MaxContent; },
                max_content_of, false, max_content_max, false);
            for (std::size_t t = 0; t < n; ++t) {
                if (limit[t] < base[t])
                    limit[t] = base[t];
            }
            std::vector<bool> const finite
                = distribute(group, intrinsic_max, min_content_of, true, intrinsic_max, false);
            for (std::size_t t = 0; t < n; ++t)
                growable[t] = finite[t];
            distribute(group, max_content_max, max_content_of, true, max_content_max, false);
            for (std::size_t t = 0; t < n; ++t) {
                growable[t] = false;
                if (tracks[t].fit_content && limit[t] != infinity)
                    limit[t] = std::min(limit[t], std::max(base[t], *tracks[t].fit_content));
            }
        }
    }

    // 11.5 step 3: items crossing flexible tracks grow those tracks alone,
    // by their flex factors.
    {
        std::vector<Contribution const*> group;
        for (Contribution const& item : input.items) {
            if (spans_flexible(item))
                group.push_back(&item);
        }
        if (!group.empty()) {
            auto const flexible_intrinsic_min = [&](std::size_t t) { return flexible(t) && intrinsic_min(t); };
            auto const minimum_of = [&](Contribution const& item) {
                return input.constraint == Constraint::Definite ? item.minimum : limited(item, item.min_content);
            };
            distribute(group, flexible_intrinsic_min, minimum_of, false, flexible, true);
            if (input.constraint == Constraint::MaxContent) {
                distribute(group, [&](std::size_t t) {
                    return flexible(t)
                        && (tracks[t].min.kind == Breadth::Kind::Auto
                            || tracks[t].min.kind == Breadth::Kind::MaxContent);
                }, [&](Contribution const& item) { return limited(item, item.max_content); }, false, flexible, true);
            }
        }
    }

    // 11.5 step 4: a growth limit still infinite is the base size.
    for (std::size_t t = 0; t < n; ++t) {
        if (limit[t] == infinity)
            limit[t] = base[t];
    }

    auto const total_gaps = [&] {
        int count = 0;
        for (std::size_t t = 0; t < n; ++t) {
            if (!tracks[t].collapsed)
                ++count;
        }
        return count > 1 ? input.gap * static_cast<float>(count - 1) : 0.0f;
    }();
    auto const free_space = [&]() -> float {
        if (input.constraint == Constraint::MinContent)
            return 0;
        if (!input.available)
            return infinity;
        float used = total_gaps;
        for (std::size_t t = 0; t < n; ++t)
            used += base[t];
        return std::max(0.0f, *input.available - used);
    };

    // 11.6: maximize tracks — the free space goes to every track equally,
    // each up to its growth limit.
    {
        float free = free_space();
        if (free == infinity) {
            for (std::size_t t = 0; t < n; ++t)
                base[t] = std::max(base[t], limit[t]);
        } else if (free > 0) {
            std::vector<bool> frozen(n, false);
            for (std::size_t t = 0; t < n; ++t)
                frozen[t] = tracks[t].collapsed || base[t] >= limit[t];
            for (int round = 0; round < 64 && free > 0.0001f; ++round) {
                std::size_t open = 0;
                for (std::size_t t = 0; t < n; ++t)
                    open += frozen[t] ? 0 : 1;
                if (open == 0)
                    break;
                float const share = free / static_cast<float>(open);
                bool any_frozen = false;
                for (std::size_t t = 0; t < n; ++t) {
                    if (frozen[t])
                        continue;
                    float const room = limit[t] - base[t];
                    float const given = std::min(share, room);
                    base[t] += given;
                    free -= given;
                    if (room <= share) {
                        frozen[t] = true;
                        any_frozen = true;
                    }
                }
                if (!any_frozen)
                    break;
            }
        }
    }

    // 11.7: expand flexible tracks.
    {
        bool any_flexible = false;
        for (std::size_t t = 0; t < n; ++t)
            any_flexible = any_flexible || (flexible(t) && !tracks[t].collapsed);
        if (any_flexible) {
            // 11.7.1: the size of an fr over some tracks and a space to fill.
            auto const find_fr = [&](int start, int end, float space) {
                std::vector<bool> inflexible(n, false);
                for (int round = 0; round < 64; ++round) {
                    float leftover = space - gaps_in(start, end);
                    float factor_sum = 0;
                    std::size_t open = 0;
                    for (int t = start; t < end; ++t) {
                        std::size_t const i = index(t);
                        if (flexible(i) && !inflexible[i] && !tracks[i].collapsed) {
                            factor_sum += tracks[i].max.value;
                            ++open;
                        } else {
                            leftover -= base[i];
                        }
                    }
                    if (open == 0)
                        return 0.0f;
                    float const hypothetical = leftover / std::max(factor_sum, 1.0f);
                    bool restart = false;
                    for (int t = start; t < end; ++t) {
                        std::size_t const i = index(t);
                        if (flexible(i) && !inflexible[i] && !tracks[i].collapsed
                            && hypothetical * tracks[i].max.value < base[i]) {
                            inflexible[i] = true;
                            restart = true;
                        }
                    }
                    if (!restart)
                        return std::max(hypothetical, 0.0f);
                }
                return 0.0f;
            };
            float fr = 0;
            if (input.constraint == Constraint::MinContent) {
                fr = 0;
            } else if (input.available) {
                fr = find_fr(0, static_cast<int>(n), *input.available);
            } else {
                for (std::size_t t = 0; t < n; ++t) {
                    if (!flexible(t) || tracks[t].collapsed)
                        continue;
                    float const factor = tracks[t].max.value;
                    fr = std::max(fr, factor > 1 ? base[t] / factor : base[t]);
                }
                for (Contribution const& item : input.items) {
                    if (spans_flexible(item))
                        fr = std::max(fr, find_fr(item.start, item.end, item.max_content));
                }
            }
            for (std::size_t t = 0; t < n; ++t) {
                if (flexible(t) && !tracks[t].collapsed)
                    base[t] = std::max(base[t], fr * tracks[t].max.value);
            }
        }
    }

    // 11.8: stretch auto tracks with what is left.
    if (input.stretch && input.available && input.constraint == Constraint::Definite) {
        float const free = free_space();
        if (free > 0) {
            std::size_t count = 0;
            for (std::size_t t = 0; t < n; ++t) {
                if (tracks[t].max.kind == Breadth::Kind::Auto && !tracks[t].collapsed)
                    ++count;
            }
            if (count > 0) {
                float const share = free / static_cast<float>(count);
                for (std::size_t t = 0; t < n; ++t) {
                    if (tracks[t].max.kind == Breadth::Kind::Auto && !tracks[t].collapsed)
                        base[t] += share;
                }
            }
        }
    }
    return base;
}

int repetitions_that_fit(float available, bool is_minimum, float fixed, int fixed_count, float repeat,
    int repeat_count, float gap)
{
    float const per_repetition = repeat + gap * static_cast<float>(repeat_count);
    if (repeat_count <= 0 || per_repetition <= 0)
        return 1;
    float const room = available - fixed - gap * static_cast<float>(fixed_count - 1);
    float const count = room / per_repetition;
    int const result = is_minimum ? static_cast<int>(std::ceil(count - 0.0001f))
                                  : static_cast<int>(std::floor(count + 0.0001f));
    int const cap = std::max(1, (track_limit - fixed_count) / repeat_count);
    return std::clamp(result, 1, cap);
}

TrackPositions distribute_tracks(std::vector<float> const& sizes, std::vector<bool> const& collapsed, float gap,
    std::optional<float> available, Distribution distribution)
{
    TrackPositions result;
    std::size_t const n = sizes.size();
    result.offsets.assign(n, 0.0f);
    int count = 0;
    float total = 0;
    for (std::size_t t = 0; t < n; ++t) {
        if (t < collapsed.size() && collapsed[t])
            continue;
        ++count;
        total += sizes[t];
    }
    if (count > 1)
        total += gap * static_cast<float>(count - 1);
    float const free = available ? *available - total : 0.0f;
    float cursor = 0;
    float between = gap;
    auto const n_tracks = static_cast<float>(count);
    switch (distribution) {
    case Distribution::Start:
    case Distribution::Stretch:
        break;
    case Distribution::End:
        cursor = free;
        break;
    case Distribution::Center:
        cursor = free / 2;
        break;
    case Distribution::SpaceBetween:
        if (free > 0 && count > 1)
            between += free / (n_tracks - 1);
        break;
    case Distribution::SpaceAround:
        if (free > 0 && count > 0) {
            cursor = free / (2 * n_tracks);
            between += free / n_tracks;
        }
        break;
    case Distribution::SpaceEvenly:
        if (free > 0 && count > 0) {
            cursor = free / (n_tracks + 1);
            between += free / (n_tracks + 1);
        }
        break;
    }
    bool first = true;
    for (std::size_t t = 0; t < n; ++t) {
        bool const skip = t < collapsed.size() && collapsed[t];
        if (skip) {
            result.offsets[t] = cursor;
            continue;
        }
        if (!first)
            cursor += between;
        first = false;
        result.offsets[t] = cursor;
        cursor += sizes[t];
    }
    result.extent = cursor;
    return result;
}

}
