#include "css/Grid.h"

#include "core/Ascii.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace sashfold::css {

namespace {

using Values = std::vector<ComponentValue const*>;

bool is_ident(ComponentValue const& value, std::string_view name)
{
    return value.is_token(Token::Type::Ident) && ascii_ci_equals(value.token().value, name);
}

bool is_slash(ComponentValue const& value)
{
    return value.is_token(Token::Type::Delim) && value.token().delim == U'/';
}

bool is_function(ComponentValue const& value, std::string_view name)
{
    return value.is_function() && ascii_ci_equals(value.function().name, name);
}

bool is_line_names(ComponentValue const& value)
{
    return value.is_block() && value.block().open == Token::Type::OpenSquare;
}

// A <custom-ident> naming a line: an identifier that is not a CSS-wide
// keyword, nor span or auto.
std::optional<std::string> custom_ident(ComponentValue const& value)
{
    if (!value.is_token(Token::Type::Ident))
        return std::nullopt;
    std::string const& text = value.token().value;
    for (std::string_view const reserved :
        { "span", "auto", "inherit", "initial", "unset", "revert", "revert-layer", "default" }) {
        if (ascii_ci_equals(text, reserved))
            return std::nullopt;
    }
    return text;
}

// A function's arguments with whitespace dropped, split at its commas.
std::vector<Values> split_arguments(FunctionValue const& function)
{
    std::vector<Values> groups(1);
    for (ComponentValue const& value : function.values) {
        if (value.is_token(Token::Type::Whitespace))
            continue;
        if (value.is_token(Token::Type::Comma)) {
            groups.emplace_back();
            continue;
        }
        groups.back().push_back(&value);
    }
    return groups;
}

// A declaration's values split at its top-level slashes.
std::vector<Values> split_slashes(Values const& values)
{
    std::vector<Values> groups(1);
    for (ComponentValue const* value : values) {
        if (is_slash(*value)) {
            groups.emplace_back();
            continue;
        }
        groups.back().push_back(value);
    }
    return groups;
}

// [ name name ... ]
std::optional<std::vector<std::string>> parse_line_names(ComponentValue const& value)
{
    if (!is_line_names(value))
        return std::nullopt;
    std::vector<std::string> names;
    for (ComponentValue const& inner : value.block().values) {
        if (inner.is_token(Token::Type::Whitespace))
            continue;
        std::optional<std::string> name = custom_ident(inner);
        if (!name)
            return std::nullopt;
        names.push_back(std::move(*name));
    }
    return names;
}

bool negative(LengthPercent const& length)
{
    switch (length.kind) {
    case LengthPercent::Kind::Auto: return false;
    case LengthPercent::Kind::Px: return length.value < 0;
    case LengthPercent::Kind::Percent: return length.value < 0;
    case LengthPercent::Kind::Calc: return false; // its sign depends on the base
    // A track's own keywords are TrackBreadth kinds, not these.
    case LengthPercent::Kind::MinContent:
    case LengthPercent::Kind::MaxContent:
    case LengthPercent::Kind::FitContent: return false;
    }
    return false;
}

// <track-breadth>, or an <inflexible-breadth> when flex factors are not allowed.
std::optional<TrackBreadth> parse_breadth(ComponentValue const& value, LengthParser const& length,
    bool allow_flex)
{
    TrackBreadth breadth;
    if (value.is_token(Token::Type::Ident)) {
        std::string_view const keyword = value.token().value;
        if (ascii_ci_equals(keyword, "auto"))
            breadth.kind = TrackBreadth::Kind::Auto;
        else if (ascii_ci_equals(keyword, "min-content"))
            breadth.kind = TrackBreadth::Kind::MinContent;
        else if (ascii_ci_equals(keyword, "max-content"))
            breadth.kind = TrackBreadth::Kind::MaxContent;
        else
            return std::nullopt;
        return breadth;
    }
    if (value.is_token(Token::Type::Dimension) && ascii_ci_equals(value.token().unit, "fr")) {
        if (!allow_flex || value.token().numeric_value < 0)
            return std::nullopt;
        breadth.kind = TrackBreadth::Kind::Flex;
        breadth.fr = static_cast<float>(value.token().numeric_value);
        return breadth;
    }
    std::optional<LengthPercent> const parsed = length(value);
    if (!parsed || parsed->is_auto() || negative(*parsed))
        return std::nullopt;
    breadth.kind = TrackBreadth::Kind::Length;
    breadth.length = *parsed;
    return breadth;
}

// A <fixed-size>: a fixed breadth, or a minmax() with a fixed breadth on
// either side — what an auto-repeat may hold.
bool is_fixed_size(TrackSize const& size)
{
    return size.min.is_fixed() || size.max.is_fixed();
}

// The tracks inside a repeat(): sizes with their line names, no nesting.
struct RepeatBody {
    std::vector<GridTrackList::Track> tracks;
    std::vector<std::string> trailing_names;
};

std::optional<RepeatBody> parse_repeat_body(Values const& values, LengthParser const& length,
    bool fixed_only)
{
    RepeatBody body;
    std::vector<std::string> pending;
    for (ComponentValue const* value : values) {
        if (is_line_names(*value)) {
            std::optional<std::vector<std::string>> names = parse_line_names(*value);
            if (!names)
                return std::nullopt;
            pending.insert(pending.end(), names->begin(), names->end());
            continue;
        }
        if (is_function(*value, "repeat"))
            return std::nullopt;
        std::optional<TrackSize> const size = parse_track_size(*value, length);
        if (!size || (fixed_only && !is_fixed_size(*size)))
            return std::nullopt;
        body.tracks.push_back({ std::move(pending), *size });
        pending.clear();
    }
    if (body.tracks.empty())
        return std::nullopt;
    body.trailing_names = std::move(pending);
    return body;
}

// A name in a grid-template-areas string: letters, digits, hyphens,
// underscores and anything beyond ASCII.
bool is_area_name(std::string_view token)
{
    for (char const c : token) {
        auto const byte = static_cast<unsigned char>(c);
        bool const ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || byte >= 0x80;
        if (!ok)
            return false;
    }
    return true;
}

bool is_area_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// <grid-line> from the values of one longhand.
std::optional<GridLine> grid_line_of(Values const& values)
{
    if (values.empty() || values.size() > 3)
        return std::nullopt;
    if (values.size() == 1 && is_ident(*values[0], "auto"))
        return GridLine {};
    bool span = false;
    std::optional<int> number;
    std::optional<std::string> name;
    for (ComponentValue const* value : values) {
        if (is_ident(*value, "span")) {
            if (span)
                return std::nullopt;
            span = true;
        } else if (value->is_token(Token::Type::Number)) {
            Token const& token = value->token();
            if (number || token.numeric_type != Token::NumericType::Integer)
                return std::nullopt;
            double const n = token.numeric_value;
            if (n > 100000 || n < -100000)
                return std::nullopt;
            number = static_cast<int>(n);
        } else if (std::optional<std::string> ident = custom_ident(*value)) {
            if (name)
                return std::nullopt;
            name = std::move(*ident);
        } else {
            return std::nullopt;
        }
    }
    GridLine line;
    if (span) {
        if (!number && !name)
            return std::nullopt;
        if (number && *number < 1)
            return std::nullopt;
        line.kind = GridLine::Kind::Span;
        line.number = number.value_or(1);
        line.name = name.value_or("");
        return line;
    }
    if (number) {
        if (*number == 0)
            return std::nullopt;
        line.kind = GridLine::Kind::Line;
        line.number = *number;
        line.name = name.value_or("");
        return line;
    }
    if (!name)
        return std::nullopt;
    line.kind = GridLine::Kind::Name;
    line.name = *name;
    return line;
}

// The line a shorthand's omitted second value takes: the same name when
// the first is a name alone, else auto.
GridLine companion_of(GridLine const& line)
{
    return line.kind == GridLine::Kind::Name ? line : GridLine {};
}

} // namespace

std::optional<TrackSize> parse_track_size(ComponentValue const& value, LengthParser const& length)
{
    TrackSize size;
    if (is_function(value, "minmax")) {
        std::vector<Values> const arguments = split_arguments(value.function());
        if (arguments.size() != 2 || arguments[0].size() != 1 || arguments[1].size() != 1)
            return std::nullopt;
        std::optional<TrackBreadth> const min = parse_breadth(*arguments[0][0], length, false);
        std::optional<TrackBreadth> const max = parse_breadth(*arguments[1][0], length, true);
        if (!min || !max)
            return std::nullopt;
        size.min = *min;
        size.max = *max;
        return size;
    }
    if (is_function(value, "fit-content")) {
        std::vector<Values> const arguments = split_arguments(value.function());
        if (arguments.size() != 1 || arguments[0].size() != 1)
            return std::nullopt;
        std::optional<LengthPercent> const cap = length(*arguments[0][0]);
        if (!cap || cap->is_auto() || negative(*cap))
            return std::nullopt;
        size.min.kind = TrackBreadth::Kind::Auto;
        size.max.kind = TrackBreadth::Kind::MaxContent;
        size.fit_content = *cap;
        return size;
    }
    if (value.is_function() && !is_function(value, "calc") && !is_function(value, "min")
        && !is_function(value, "max") && !is_function(value, "clamp"))
        return std::nullopt;
    std::optional<TrackBreadth> const breadth = parse_breadth(value, length, true);
    if (!breadth)
        return std::nullopt;
    if (breadth->is_flexible()) {
        size.min.kind = TrackBreadth::Kind::Auto;
        size.max = *breadth;
    } else {
        size.min = *breadth;
        size.max = *breadth;
    }
    return size;
}

std::optional<GridTrackList> parse_track_list(Values const& values, LengthParser const& length)
{
    GridTrackList list;
    if (values.empty())
        return std::nullopt;
    if (values.size() == 1 && is_ident(*values[0], "none"))
        return list;
    constexpr std::size_t track_limit = 10000;
    std::vector<std::string> pending; // line names waiting for the next track
    auto const append = [&](std::vector<std::string> const& names, TrackSize const& size) {
        std::vector<std::string> merged = pending;
        merged.insert(merged.end(), names.begin(), names.end());
        pending.clear();
        list.tracks.push_back({ std::move(merged), size });
    };
    for (ComponentValue const* value : values) {
        if (is_line_names(*value)) {
            std::optional<std::vector<std::string>> names = parse_line_names(*value);
            if (!names)
                return std::nullopt;
            pending.insert(pending.end(), names->begin(), names->end());
            continue;
        }
        if (is_function(*value, "repeat")) {
            std::vector<Values> const arguments = split_arguments(value->function());
            if (arguments.size() != 2 || arguments[0].size() != 1)
                return std::nullopt;
            ComponentValue const& count = *arguments[0][0];
            if (count.is_token(Token::Type::Number)) {
                Token const& token = count.token();
                if (token.numeric_type != Token::NumericType::Integer || token.numeric_value < 1)
                    return std::nullopt;
                std::optional<RepeatBody> const body = parse_repeat_body(arguments[1], length, false);
                if (!body)
                    return std::nullopt;
                auto const repetitions
                    = static_cast<std::size_t>(std::min(token.numeric_value, 100000.0));
                for (std::size_t r = 0; r < repetitions && list.tracks.size() < track_limit; ++r) {
                    for (GridTrackList::Track const& track : body->tracks)
                        append(track.names, track.size);
                    pending = body->trailing_names;
                }
                continue;
            }
            bool const fill = is_ident(count, "auto-fill");
            if (!fill && !is_ident(count, "auto-fit"))
                return std::nullopt;
            if (list.auto_repeat != GridTrackList::AutoRepeat::None)
                return std::nullopt;
            std::optional<RepeatBody> body = parse_repeat_body(arguments[1], length, true);
            if (!body)
                return std::nullopt;
            list.auto_repeat = fill ? GridTrackList::AutoRepeat::Fill : GridTrackList::AutoRepeat::Fit;
            list.auto_repeat_at = list.tracks.size();
            list.auto_repeat_leading_names = std::move(pending);
            list.auto_repeat_tracks = std::move(body->tracks);
            list.auto_repeat_trailing_names = body->trailing_names;
            // The names after the last repetition sit on the same line as
            // the next track's own.
            pending = std::move(body->trailing_names);
            continue;
        }
        std::optional<TrackSize> const size = parse_track_size(*value, length);
        if (!size)
            return std::nullopt;
        append({}, *size);
    }
    list.trailing_names = std::move(pending);
    if (list.tracks.empty() && list.auto_repeat == GridTrackList::AutoRepeat::None)
        return std::nullopt; // names alone
    if (list.auto_repeat != GridTrackList::AutoRepeat::None) {
        // An auto-repeat shares its list only with fixed sizes.
        for (GridTrackList::Track const& track : list.tracks) {
            if (!is_fixed_size(track.size))
                return std::nullopt;
        }
    }
    return list;
}

std::optional<std::vector<TrackSize>> parse_track_sizes(Values const& values, LengthParser const& length)
{
    std::vector<TrackSize> sizes;
    for (ComponentValue const* value : values) {
        std::optional<TrackSize> const size = parse_track_size(*value, length);
        if (!size)
            return std::nullopt;
        sizes.push_back(*size);
    }
    if (sizes.empty())
        return std::nullopt;
    return sizes;
}

std::optional<GridAreas> parse_grid_template_areas(Values const& values)
{
    GridAreas areas;
    if (values.empty())
        return std::nullopt;
    if (values.size() == 1 && is_ident(*values[0], "none"))
        return areas;
    std::vector<std::vector<std::string>> cells; // rows of names; "" is a null cell
    for (ComponentValue const* value : values) {
        if (!value->is_token(Token::Type::String))
            return std::nullopt;
        std::vector<std::string> row;
        std::string const& text = value->token().value;
        std::size_t i = 0;
        while (i < text.size()) {
            if (is_area_space(text[i])) {
                ++i;
                continue;
            }
            std::size_t const start = i;
            while (i < text.size() && !is_area_space(text[i]))
                ++i;
            std::string token = text.substr(start, i - start);
            if (token.find_first_not_of('.') == std::string::npos) {
                row.emplace_back();
                continue;
            }
            if (!is_area_name(token))
                return std::nullopt;
            row.push_back(std::move(token));
        }
        if (row.empty())
            return std::nullopt;
        if (!cells.empty() && row.size() != cells.front().size())
            return std::nullopt;
        cells.push_back(std::move(row));
    }
    areas.rows = static_cast<int>(cells.size());
    areas.columns = static_cast<int>(cells.front().size());
    // Each name's cells make one rectangle, or the whole value is bad.
    for (int r = 0; r < areas.rows; ++r) {
        for (int c = 0; c < areas.columns; ++c) {
            std::string const& name = cells[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (name.empty())
                continue;
            bool seen = false;
            for (GridAreas::Area const& area : areas.areas)
                seen = seen || area.name == name;
            if (seen)
                continue;
            int r0 = r, r1 = r, c0 = c, c1 = c;
            for (int rr = 0; rr < areas.rows; ++rr) {
                for (int cc = 0; cc < areas.columns; ++cc) {
                    if (cells[static_cast<std::size_t>(rr)][static_cast<std::size_t>(cc)] == name) {
                        r0 = std::min(r0, rr);
                        r1 = std::max(r1, rr);
                        c0 = std::min(c0, cc);
                        c1 = std::max(c1, cc);
                    }
                }
            }
            for (int rr = r0; rr <= r1; ++rr) {
                for (int cc = c0; cc <= c1; ++cc) {
                    if (cells[static_cast<std::size_t>(rr)][static_cast<std::size_t>(cc)] != name)
                        return std::nullopt;
                }
            }
            areas.areas.push_back({ name, r0 + 1, r1 + 2, c0 + 1, c1 + 2 });
        }
    }
    return areas;
}

std::optional<GridLine> parse_grid_line(Values const& values)
{
    return grid_line_of(values);
}

std::optional<std::pair<GridLine, GridLine>> parse_grid_line_pair(Values const& values)
{
    std::vector<Values> const groups = split_slashes(values);
    if (groups.empty() || groups.size() > 2)
        return std::nullopt;
    std::optional<GridLine> const start = grid_line_of(groups[0]);
    if (!start)
        return std::nullopt;
    if (groups.size() == 1)
        return std::make_pair(*start, companion_of(*start));
    std::optional<GridLine> const end = grid_line_of(groups[1]);
    if (!end)
        return std::nullopt;
    return std::make_pair(*start, *end);
}

std::optional<std::array<GridLine, 4>> parse_grid_area(Values const& values)
{
    std::vector<Values> const groups = split_slashes(values);
    if (groups.empty() || groups.size() > 4)
        return std::nullopt;
    std::vector<GridLine> lines;
    for (Values const& group : groups) {
        std::optional<GridLine> const line = grid_line_of(group);
        if (!line)
            return std::nullopt;
        lines.push_back(*line);
    }
    std::array<GridLine, 4> result; // row-start, column-start, row-end, column-end
    result[0] = lines[0];
    result[1] = lines.size() > 1 ? lines[1] : companion_of(lines[0]);
    result[2] = lines.size() > 2 ? lines[2] : companion_of(lines[0]);
    result[3] = lines.size() > 3 ? lines[3] : companion_of(result[1]);
    return result;
}

std::optional<GridAutoFlow> parse_grid_auto_flow(Values const& values)
{
    if (values.empty() || values.size() > 2)
        return std::nullopt;
    bool column = false;
    bool dense = false;
    bool direction = false;
    for (ComponentValue const* value : values) {
        if (is_ident(*value, "row") || is_ident(*value, "column")) {
            if (direction)
                return std::nullopt;
            direction = true;
            column = is_ident(*value, "column");
        } else if (is_ident(*value, "dense")) {
            if (dense)
                return std::nullopt;
            dense = true;
        } else {
            return std::nullopt;
        }
    }
    if (column)
        return dense ? GridAutoFlow::ColumnDense : GridAutoFlow::Column;
    return dense ? GridAutoFlow::RowDense : GridAutoFlow::Row;
}

std::optional<GridShorthand> parse_grid_template(Values const& values, LengthParser const& length)
{
    GridShorthand result;
    if (values.empty())
        return std::nullopt;
    if (values.size() == 1 && is_ident(*values[0], "none"))
        return result;
    bool strings = false;
    for (ComponentValue const* value : values)
        strings = strings || value->is_token(Token::Type::String);
    std::vector<Values> const groups = split_slashes(values);
    if (groups.size() > 2 || groups[0].empty())
        return std::nullopt;
    if (!strings) {
        if (groups.size() != 2 || groups[1].empty())
            return std::nullopt;
        std::optional<GridTrackList> rows = parse_track_list(groups[0], length);
        std::optional<GridTrackList> columns = parse_track_list(groups[1], length);
        if (!rows || !columns)
            return std::nullopt;
        if (!rows->empty())
            result.rows = std::make_shared<GridTrackList const>(std::move(*rows));
        if (!columns->empty())
            result.columns = std::make_shared<GridTrackList const>(std::move(*columns));
        return result;
    }
    // The areas form: [ <line-names>? <string> <track-size>? <line-names>? ]+
    GridTrackList rows;
    Values strings_only;
    std::vector<std::string> pending;
    bool have_row = false;
    bool row_sized = false;
    for (ComponentValue const* value : groups[0]) {
        if (is_line_names(*value)) {
            std::optional<std::vector<std::string>> names = parse_line_names(*value);
            if (!names)
                return std::nullopt;
            pending.insert(pending.end(), names->begin(), names->end());
            continue;
        }
        if (value->is_token(Token::Type::String)) {
            strings_only.push_back(value);
            rows.tracks.push_back({ std::move(pending), TrackSize {} });
            pending.clear();
            have_row = true;
            row_sized = false;
            continue;
        }
        if (!have_row || row_sized || is_function(*value, "repeat"))
            return std::nullopt;
        std::optional<TrackSize> const size = parse_track_size(*value, length);
        if (!size)
            return std::nullopt;
        rows.tracks.back().size = *size;
        row_sized = true;
    }
    rows.trailing_names = std::move(pending);
    std::optional<GridAreas> areas = parse_grid_template_areas(strings_only);
    if (!areas || areas->rows == 0)
        return std::nullopt;
    result.areas = std::make_shared<GridAreas const>(std::move(*areas));
    result.rows = std::make_shared<GridTrackList const>(std::move(rows));
    if (groups.size() == 2) {
        if (groups[1].empty())
            return std::nullopt;
        std::optional<GridTrackList> columns = parse_track_list(groups[1], length);
        if (!columns || columns->auto_repeat != GridTrackList::AutoRepeat::None)
            return std::nullopt;
        if (!columns->empty())
            result.columns = std::make_shared<GridTrackList const>(std::move(*columns));
    }
    return result;
}

std::optional<GridShorthand> parse_grid_shorthand(Values const& values, LengthParser const& length)
{
    std::vector<Values> const groups = split_slashes(values);
    auto const has_auto_flow = [](Values const& group) {
        for (ComponentValue const* value : group) {
            if (is_ident(*value, "auto-flow"))
                return true;
        }
        return false;
    };
    if (groups.size() != 2 || (!has_auto_flow(groups[0]) && !has_auto_flow(groups[1])))
        return parse_grid_template(values, length);
    if (has_auto_flow(groups[0]) && has_auto_flow(groups[1]))
        return std::nullopt;
    // [ auto-flow && dense? ] <track-size>* — the keywords first.
    auto const flow_part = [&](Values const& group, bool& dense,
                               std::shared_ptr<std::vector<TrackSize> const>& sizes) {
        dense = false;
        bool flow = false;
        Values rest;
        for (ComponentValue const* value : group) {
            bool const keyword = is_ident(*value, "auto-flow") || is_ident(*value, "dense");
            if (keyword && !rest.empty())
                return false;
            if (is_ident(*value, "auto-flow")) {
                if (flow)
                    return false;
                flow = true;
            } else if (is_ident(*value, "dense")) {
                if (dense)
                    return false;
                dense = true;
            } else {
                rest.push_back(value);
            }
        }
        if (!rest.empty()) {
            std::optional<std::vector<TrackSize>> parsed = parse_track_sizes(rest, length);
            if (!parsed)
                return false;
            sizes = std::make_shared<std::vector<TrackSize> const>(std::move(*parsed));
        }
        return true;
    };
    GridShorthand result;
    bool dense = false;
    if (has_auto_flow(groups[0])) {
        if (!flow_part(groups[0], dense, result.auto_rows) || groups[1].empty())
            return std::nullopt;
        std::optional<GridTrackList> columns = parse_track_list(groups[1], length);
        if (!columns)
            return std::nullopt;
        if (!columns->empty())
            result.columns = std::make_shared<GridTrackList const>(std::move(*columns));
        result.auto_flow = dense ? GridAutoFlow::RowDense : GridAutoFlow::Row;
        return result;
    }
    if (groups[0].empty())
        return std::nullopt;
    std::optional<GridTrackList> rows = parse_track_list(groups[0], length);
    if (!rows || !flow_part(groups[1], dense, result.auto_columns))
        return std::nullopt;
    if (!rows->empty())
        result.rows = std::make_shared<GridTrackList const>(std::move(*rows));
    result.auto_flow = dense ? GridAutoFlow::ColumnDense : GridAutoFlow::Column;
    return result;
}

}
