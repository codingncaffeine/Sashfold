#pragma once

// The grid placement and track sizing algorithms (css-grid-1 §8 and §11)
// over plain numbers — no DOM, no styles — so they are tested on their own
// and the layouter only gathers their inputs and places their results.
//
// Lines are 0-based here (the specification's line 1 is line 0); track n
// lies between lines n and n+1. A negative line is an implicit one before
// the explicit grid.

#include "css/ComputedStyle.h"

#include <optional>
#include <string>
#include <vector>

namespace sashfold::layout::grid {

// The explicit grid's lines in one axis, for resolving names: the names
// on each line, one entry per line (the tracks plus one).
struct AxisLines {
    std::vector<std::vector<std::string>> names;

    int tracks() const { return names.empty() ? 0 : static_cast<int>(names.size()) - 1; }
    bool has(int line, std::string const& name) const;
};

// Where an item's two placement properties put it in one axis before
// auto-placement: both lines definite, or a span still to be placed.
struct AxisPlacement {
    std::optional<int> start;
    std::optional<int> end; // exclusive
    int span = 1;

    bool definite() const { return start.has_value(); }
};

// §8.3: resolves a start/end pair of grid-line values against an axis.
AxisPlacement resolve_lines(css::GridLine const& start, css::GridLine const& end, AxisLines const& lines);

// §8.5: places every item — the definite areas hold, the rest flow into
// the free cells in order.
struct Area {
    int row_start = 0;
    int row_end = 1;
    int column_start = 0;
    int column_end = 1;
};

struct ItemPlacement {
    AxisPlacement rows;
    AxisPlacement columns;
};

struct PlacedGrid {
    std::vector<Area> areas; // one per item, in the order given, 0-based in the implicit grid
    int rows = 0; // the implicit grid's size
    int columns = 0;
    int row_offset = 0; // the explicit grid's first line is at this index
    int column_offset = 0;
};

PlacedGrid place_items(std::vector<ItemPlacement> const& items, int explicit_rows, int explicit_columns,
    css::GridAutoFlow flow);

// §11: the sizes of the tracks in one axis.

// A track sizing function's side with its lengths resolved.
struct Breadth {
    enum class Kind {
        Fixed, // value is px
        Flex, // value is the flex factor
        Auto,
        MinContent,
        MaxContent,
    };
    Kind kind = Kind::Auto;
    float value = 0;

    bool intrinsic() const { return kind == Kind::Auto || kind == Kind::MinContent || kind == Kind::MaxContent; }
};

struct Track {
    Breadth min;
    Breadth max;
    std::optional<float> fit_content; // the cap of a fit-content() track, px
    bool collapsed = false; // an empty auto-fit repetition: no size, no gutters
};

// An item's sizes in this axis, all outer (margins included), over the
// tracks it spans.
struct Contribution {
    int start = 0;
    int end = 1; // exclusive
    float minimum = 0; // the minimum contribution (§6.6)
    float min_content = 0;
    float max_content = 0;
};

// How the container's size in this axis is known: definite, or being
// found from the content under a min- or max-content constraint.
enum class Constraint {
    Definite,
    MinContent,
    MaxContent,
};

struct SizingInput {
    std::vector<Track> tracks;
    std::vector<Contribution> items;
    std::optional<float> available; // the container's content size in this axis, when definite
    float gap = 0;
    bool stretch = true; // the content-distribution is normal or stretch: auto tracks take the free space
    Constraint constraint = Constraint::Definite;
};

std::vector<float> size_tracks(SizingInput const& input);

// §7.2.3.2: how many repetitions of an auto-repeat fit. `fixed` sums the
// other tracks' sizes (their fixed maximum, else minimum) and `repeat` one
// repetition's; the counts are their track counts. With a definite size
// the most that fit, at least one; with only a minimum size, the fewest
// that reach it.
int repetitions_that_fit(float available, bool is_minimum, float fixed, int fixed_count, float repeat,
    int repeat_count, float gap);

// §10.5: the tracks' offsets once the free space is shared out by the
// content-distribution keyword — the start of each track from the content
// edge, and the end of the last one.
enum class Distribution {
    Start,
    End,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
    Stretch, // the auto tracks already took the space: start
};

struct TrackPositions {
    std::vector<float> offsets;
    float extent = 0;
};

TrackPositions distribute_tracks(std::vector<float> const& sizes, std::vector<bool> const& collapsed, float gap,
    std::optional<float> available, Distribution distribution);

}
