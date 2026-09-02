#pragma once

// Grid layout's values (css-grid-1): track lists and their sizing
// functions, the named areas, the placement of an item by its lines, and
// the shorthands that set them together. Lengths are parsed by a function
// the caller supplies, which knows the element's font size and the viewport.

#include "css/ComputedStyle.h"
#include "css/Parser.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace sashfold::css {

// Parses one component value as a length or percentage (never auto);
// nullopt for anything else.
using LengthParser = std::function<std::optional<LengthPercent>(ComponentValue const&)>;

// <track-size>: <track-breadth> | minmax(<inflexible-breadth>, <track-breadth>)
// | fit-content(<length-percentage>).
std::optional<TrackSize> parse_track_size(ComponentValue const& value, LengthParser const& length);

// grid-template-columns / grid-template-rows: none | <track-list> |
// <auto-track-list>. An empty list is none; subgrid and masonry are declined.
std::optional<GridTrackList> parse_track_list(std::vector<ComponentValue const*> const& values,
    LengthParser const& length);

// grid-auto-columns / grid-auto-rows: <track-size>+.
std::optional<std::vector<TrackSize>> parse_track_sizes(std::vector<ComponentValue const*> const& values,
    LengthParser const& length);

// grid-template-areas: none | <string>+. Declined when the rows differ in
// length or an area is not a rectangle. A result without rows is none.
std::optional<GridAreas> parse_grid_template_areas(std::vector<ComponentValue const*> const& values);

// grid-row-start and its siblings: one <grid-line>.
std::optional<GridLine> parse_grid_line(std::vector<ComponentValue const*> const& values);

// grid-row / grid-column: <grid-line> [ / <grid-line> ]? — start, then end.
std::optional<std::pair<GridLine, GridLine>> parse_grid_line_pair(
    std::vector<ComponentValue const*> const& values);

// grid-area: <grid-line> [ / <grid-line> ]{0,3} — row-start, column-start,
// row-end, column-end.
std::optional<std::array<GridLine, 4>> parse_grid_area(std::vector<ComponentValue const*> const& values);

// grid-auto-flow: [ row | column ] || dense.
std::optional<GridAutoFlow> parse_grid_auto_flow(std::vector<ComponentValue const*> const& values);

// What the grid-template and grid shorthands set: null lists and areas are
// none. The auto-flow parts are grid's alone; grid-template leaves them.
struct GridShorthand {
    std::shared_ptr<GridTrackList const> rows;
    std::shared_ptr<GridTrackList const> columns;
    std::shared_ptr<GridAreas const> areas;
    GridAutoFlow auto_flow = GridAutoFlow::Row;
    std::shared_ptr<std::vector<TrackSize> const> auto_rows;
    std::shared_ptr<std::vector<TrackSize> const> auto_columns;
};

// grid-template: none | <rows> / <columns> | the areas form, a row track
// size beside each string and the columns after a slash.
std::optional<GridShorthand> parse_grid_template(std::vector<ComponentValue const*> const& values,
    LengthParser const& length);

// grid: <grid-template> | <rows> / [ auto-flow && dense? ] <auto-columns>?
// | [ auto-flow && dense? ] <auto-rows>? / <columns>.
std::optional<GridShorthand> parse_grid_shorthand(std::vector<ComponentValue const*> const& values,
    LengthParser const& length);

}
