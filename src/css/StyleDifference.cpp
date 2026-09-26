#include "css/StyleResolver.h"

#include "css/Parser.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Two computed styles compared field by field, deeply: what a shared list
// holds rather than which object holds it, since a style computed twice
// holds equal lists in different objects. Custom properties compare by the
// values an element sees, whatever base they are stored over.

namespace sashfold::css {

namespace {

bool same(float a, float b) { return a == b || (a != a && b != b); }
bool same(int a, int b) { return a == b; }
bool same(bool a, bool b) { return a == b; }
bool same(std::string const& a, std::string const& b) { return a == b; }
bool same(Color const& a, Color const& b) { return a == b; }

template<typename Enum>
    requires std::is_enum_v<Enum>
bool same(Enum a, Enum b)
{
    return a == b;
}

// Declared ahead of the templates over lists and pointers, which find
// their element's comparison when they are defined.
bool same(GridTrackList::Track const& a, GridTrackList::Track const& b);
bool same(GridAreas::Area const& a, GridAreas::Area const& b);
bool same(GradientStop const& a, GradientStop const& b);
bool same(BackgroundImage const& a, BackgroundImage const& b);
bool same(BackgroundRepeatPair const& a, BackgroundRepeatPair const& b);
bool same(BackgroundPosition const& a, BackgroundPosition const& b);
bool same(BackgroundSize const& a, BackgroundSize const& b);
bool same(CounterOp const& a, CounterOp const& b);
bool same(ContentItem const& a, ContentItem const& b);
bool same(ComponentValue const& a, ComponentValue const& b);
bool same(GeneratedBox const& a, GeneratedBox const& b);
bool same(GridTrackList const& a, GridTrackList const& b);
bool same(GridAreas const& a, GridAreas const& b);
bool same(Gradient const& a, Gradient const& b);
bool same(Content const& a, Content const& b);
bool same(CustomProperties const& a, CustomProperties const& b);
bool same(GeneratedContent const& a, GeneratedContent const& b);
bool same(ComputedStyle const& a, ComputedStyle const& b);
template<typename A, typename B>
bool same(std::pair<A, B> const& a, std::pair<A, B> const& b);

bool same(LengthPercent const& a, LengthPercent const& b)
{
    return a.kind == b.kind && same(a.value, b.value) && same(a.percent, b.percent);
}

bool same(AspectRatio const& a, AspectRatio const& b) { return a.with_auto == b.with_auto && same(a.ratio, b.ratio); }
bool same(LineHeight const& a, LineHeight const& b) { return a.kind == b.kind && same(a.value, b.value); }
bool same(VerticalAlign const& a, VerticalAlign const& b) { return a.kind == b.kind && same(a.offset, b.offset); }

bool same(BorderSide const& a, BorderSide const& b)
{
    return same(a.width, b.width) && a.style == b.style && a.color == b.color && a.current_color == b.current_color;
}

bool same(CornerRadius const& a, CornerRadius const& b) { return same(a.x, b.x) && same(a.y, b.y); }

bool same(SvgPaint const& a, SvgPaint const& b)
{
    return a.kind == b.kind && a.color == b.color && a.reference == b.reference && a.has_fallback == b.has_fallback;
}

bool same(GridLine const& a, GridLine const& b) { return a.kind == b.kind && a.number == b.number && a.name == b.name; }

bool same(TrackBreadth const& a, TrackBreadth const& b)
{
    return a.kind == b.kind && same(a.length, b.length) && same(a.fr, b.fr);
}

template<typename T>
bool same(std::optional<T> const& a, std::optional<T> const& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a || same(*a, *b);
}

bool same(TrackSize const& a, TrackSize const& b)
{
    return same(a.min, b.min) && same(a.max, b.max) && same(a.fit_content, b.fit_content);
}

template<typename T>
bool same(std::vector<T> const& a, std::vector<T> const& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same(a[i], b[i]))
            return false;
    }
    return true;
}

template<typename A, typename B>
bool same(std::pair<A, B> const& a, std::pair<A, B> const& b)
{
    return same(a.first, b.first) && same(a.second, b.second);
}

bool same(GridTrackList::Track const& a, GridTrackList::Track const& b)
{
    return same(a.names, b.names) && same(a.size, b.size);
}

bool same(GridTrackList const& a, GridTrackList const& b)
{
    return same(a.tracks, b.tracks) && same(a.trailing_names, b.trailing_names) && a.auto_repeat == b.auto_repeat
        && a.auto_repeat_at == b.auto_repeat_at && same(a.auto_repeat_leading_names, b.auto_repeat_leading_names)
        && same(a.auto_repeat_tracks, b.auto_repeat_tracks)
        && same(a.auto_repeat_trailing_names, b.auto_repeat_trailing_names);
}

bool same(GridAreas::Area const& a, GridAreas::Area const& b)
{
    return a.name == b.name && a.row_start == b.row_start && a.row_end == b.row_end
        && a.column_start == b.column_start && a.column_end == b.column_end;
}

bool same(GridAreas const& a, GridAreas const& b)
{
    return same(a.areas, b.areas) && a.rows == b.rows && a.columns == b.columns;
}

bool same(GradientStop const& a, GradientStop const& b) { return a.color == b.color && same(a.position, b.position); }

bool same(Gradient const& a, Gradient const& b)
{
    return a.kind == b.kind && a.repeating == b.repeating && same(a.angle, b.angle) && a.corner == b.corner
        && a.shape == b.shape && a.extent == b.extent && same(a.center_x, b.center_x)
        && same(a.center_y, b.center_y) && same(a.stops, b.stops);
}

template<typename T>
bool same(std::shared_ptr<T const> const& a, std::shared_ptr<T const> const& b);

bool same(BackgroundImage const& a, BackgroundImage const& b) { return a.url == b.url && same(a.gradient, b.gradient); }
bool same(BackgroundRepeatPair const& a, BackgroundRepeatPair const& b) { return a.x == b.x && a.y == b.y; }
bool same(BackgroundPosition const& a, BackgroundPosition const& b) { return same(a.x, b.x) && same(a.y, b.y); }

bool same(BackgroundSize const& a, BackgroundSize const& b)
{
    return a.kind == b.kind && same(a.width, b.width) && same(a.height, b.height);
}

bool same(CounterOp const& a, CounterOp const& b) { return a == b; }

bool same(ContentItem const& a, ContentItem const& b)
{
    return a.kind == b.kind && a.text == b.text && a.fallback == b.fallback && a.style == b.style;
}

bool same(Content const& a, Content const& b) { return a.kind == b.kind && same(a.items, b.items); }

bool same(ComponentValue const& a, ComponentValue const& b);

bool same(Token const& a, Token const& b)
{
    return a.type == b.type && a.value == b.value && a.delim == b.delim && a.numeric_value == b.numeric_value
        && a.numeric_type == b.numeric_type && a.has_sign == b.has_sign && a.unit == b.unit
        && a.hash_type == b.hash_type && a.range_start == b.range_start && a.range_end == b.range_end;
}

bool same(ComponentValue const& a, ComponentValue const& b)
{
    if (a.value.index() != b.value.index())
        return false;
    if (a.is_token())
        return same(a.token(), b.token());
    if (a.is_function())
        return a.function().name == b.function().name && same(a.function().values, b.function().values);
    return a.block().open == b.block().open && same(a.block().values, b.block().values);
}

// The entries an element sees: the base's, each replaced by an entry of the
// same name over it, the invalid ones hiding theirs. Both lists are sorted.
std::vector<CustomProperty const*> visible_entries(CustomProperties const& set)
{
    std::vector<CustomProperty const*> seen;
    std::vector<CustomProperties::Entry> const empty;
    std::vector<CustomProperties::Entry> const& under = set.base ? set.base->entries : empty;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < set.entries.size() || j < under.size()) {
        CustomProperty const* pick = nullptr;
        if (j >= under.size() || (i < set.entries.size() && set.entries[i]->name <= under[j]->name)) {
            if (j < under.size() && set.entries[i]->name == under[j]->name)
                ++j;
            pick = set.entries[i++].get();
        } else {
            pick = under[j++].get();
        }
        if (pick->valid)
            seen.push_back(pick);
    }
    return seen;
}

bool same(CustomProperties const& a, CustomProperties const& b)
{
    std::vector<CustomProperty const*> const left = visible_entries(a);
    std::vector<CustomProperty const*> const right = visible_entries(b);
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i] == right[i])
            continue;
        if (left[i]->name != right[i]->name || !same(left[i]->value, right[i]->value))
            return false;
    }
    return true;
}

bool same(GeneratedBox const& a, GeneratedBox const& b)
{
    return a.text == b.text && !first_style_difference(a.style, b.style);
}

bool same(GeneratedContent const& a, GeneratedContent const& b)
{
    return same(a.before, b.before) && same(a.after, b.after);
}

bool same(ComputedStyle const& a, ComputedStyle const& b) { return !first_style_difference(a, b); }

template<typename T>
bool same(std::shared_ptr<T const> const& a, std::shared_ptr<T const> const& b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return same(*a, *b);
}

}

bool same_custom_properties(CustomProperties const& a, CustomProperties const& b) { return same(a, b); }

std::optional<std::string_view> first_style_difference(ComputedStyle const& a, ComputedStyle const& b)
{
    std::optional<std::string_view> found;
    auto const field = [&found](std::string_view name, auto const& left, auto const& right) {
        if (!found && !same(left, right))
            found = name;
    };
    field("display", a.display, b.display);
    field("width", a.width, b.width);
    field("height", a.height, b.height);
    field("min_width", a.min_width, b.min_width);
    field("max_width", a.max_width, b.max_width);
    field("min_height", a.min_height, b.min_height);
    field("max_height", a.max_height, b.max_height);
    field("box_sizing", a.box_sizing, b.box_sizing);
    field("aspect_ratio", a.aspect_ratio, b.aspect_ratio);
    field("object_fit", a.object_fit, b.object_fit);
    field("object_position_x", a.object_position_x, b.object_position_x);
    field("object_position_y", a.object_position_y, b.object_position_y);
    field("margin_top", a.margin_top, b.margin_top);
    field("margin_right", a.margin_right, b.margin_right);
    field("margin_bottom", a.margin_bottom, b.margin_bottom);
    field("margin_left", a.margin_left, b.margin_left);
    field("padding_top", a.padding_top, b.padding_top);
    field("padding_right", a.padding_right, b.padding_right);
    field("padding_bottom", a.padding_bottom, b.padding_bottom);
    field("padding_left", a.padding_left, b.padding_left);
    field("border_top", a.border_top, b.border_top);
    field("border_right", a.border_right, b.border_right);
    field("border_bottom", a.border_bottom, b.border_bottom);
    field("border_left", a.border_left, b.border_left);
    field("border_top_left_radius", a.border_top_left_radius, b.border_top_left_radius);
    field("border_top_right_radius", a.border_top_right_radius, b.border_top_right_radius);
    field("border_bottom_right_radius", a.border_bottom_right_radius, b.border_bottom_right_radius);
    field("border_bottom_left_radius", a.border_bottom_left_radius, b.border_bottom_left_radius);
    field("floating", a.floating, b.floating);
    field("clear", a.clear, b.clear);
    field("overflow", a.overflow, b.overflow);
    field("overflow_x", a.overflow_x, b.overflow_x);
    field("overflow_y", a.overflow_y, b.overflow_y);
    field("overflow_applies", a.overflow_applies, b.overflow_applies);
    field("viewport_overflow_x", a.viewport_overflow_x, b.viewport_overflow_x);
    field("viewport_overflow_y", a.viewport_overflow_y, b.viewport_overflow_y);
    field("border_collapse", a.border_collapse, b.border_collapse);
    field("border_spacing_horizontal", a.border_spacing_horizontal, b.border_spacing_horizontal);
    field("border_spacing_vertical", a.border_spacing_vertical, b.border_spacing_vertical);
    field("caption_side", a.caption_side, b.caption_side);
    field("empty_cells", a.empty_cells, b.empty_cells);
    field("table_layout", a.table_layout, b.table_layout);
    field("position", a.position, b.position);
    field("top", a.top, b.top);
    field("right", a.right, b.right);
    field("bottom", a.bottom, b.bottom);
    field("left", a.left, b.left);
    field("z_index", a.z_index, b.z_index);
    field("blockified", a.blockified, b.blockified);
    field("appearance", a.appearance, b.appearance);
    field("author_decorated", a.author_decorated, b.author_decorated);
    field("visibility", a.visibility, b.visibility);
    field("opacity", a.opacity, b.opacity);
    field("pointer_events", a.pointer_events, b.pointer_events);
    field("translate_x", a.translate_x, b.translate_x);
    field("translate_y", a.translate_y, b.translate_y);
    field("transformed", a.transformed, b.transformed);
    field("flex_direction", a.flex_direction, b.flex_direction);
    field("flex_wrap", a.flex_wrap, b.flex_wrap);
    field("justify_content", a.justify_content, b.justify_content);
    field("align_items", a.align_items, b.align_items);
    field("align_self", a.align_self, b.align_self);
    field("align_content", a.align_content, b.align_content);
    field("justify_items", a.justify_items, b.justify_items);
    field("justify_self", a.justify_self, b.justify_self);
    field("align_self_safe", a.align_self_safe, b.align_self_safe);
    field("align_self_last", a.align_self_last, b.align_self_last);
    field("justify_self_safe", a.justify_self_safe, b.justify_self_safe);
    field("justify_self_last", a.justify_self_last, b.justify_self_last);
    field("flex_grow", a.flex_grow, b.flex_grow);
    field("flex_shrink", a.flex_shrink, b.flex_shrink);
    field("flex_basis", a.flex_basis, b.flex_basis);
    field("row_gap", a.row_gap, b.row_gap);
    field("column_gap", a.column_gap, b.column_gap);
    field("order", a.order, b.order);
    field("grid_template_columns", a.grid_template_columns, b.grid_template_columns);
    field("grid_template_rows", a.grid_template_rows, b.grid_template_rows);
    field("grid_template_areas", a.grid_template_areas, b.grid_template_areas);
    field("grid_auto_columns", a.grid_auto_columns, b.grid_auto_columns);
    field("grid_auto_rows", a.grid_auto_rows, b.grid_auto_rows);
    field("grid_auto_flow", a.grid_auto_flow, b.grid_auto_flow);
    field("grid_row_start", a.grid_row_start, b.grid_row_start);
    field("grid_row_end", a.grid_row_end, b.grid_row_end);
    field("grid_column_start", a.grid_column_start, b.grid_column_start);
    field("grid_column_end", a.grid_column_end, b.grid_column_end);
    field("color", a.color, b.color);
    field("background_color", a.background_color, b.background_color);
    field("background_images", a.background_images, b.background_images);
    field("background_repeats", a.background_repeats, b.background_repeats);
    field("background_positions", a.background_positions, b.background_positions);
    field("background_sizes", a.background_sizes, b.background_sizes);
    field("background_origins", a.background_origins, b.background_origins);
    field("background_clips", a.background_clips, b.background_clips);
    field("font_size", a.font_size, b.font_size);
    field("font_weight", a.font_weight, b.font_weight);
    field("font_stretch", a.font_stretch, b.font_stretch);
    field("font_style", a.font_style, b.font_style);
    field("font_family", a.font_family, b.font_family);
    field("line_height", a.line_height, b.line_height);
    field("vertical_align", a.vertical_align, b.vertical_align);
    field("direction", a.direction, b.direction);
    field("writing_mode", a.writing_mode, b.writing_mode);
    field("text_orientation", a.text_orientation, b.text_orientation);
    field("unicode_bidi", a.unicode_bidi, b.unicode_bidi);
    field("text_align", a.text_align, b.text_align);
    field("text_align_last", a.text_align_last, b.text_align_last);
    field("text_justify", a.text_justify, b.text_justify);
    field("white_space", a.white_space, b.white_space);
    field("word_break", a.word_break, b.word_break);
    field("hyphens", a.hyphens, b.hyphens);
    field("hyphenate_character", a.hyphenate_character, b.hyphenate_character);
    field("line_break", a.line_break, b.line_break);
    field("overflow_wrap", a.overflow_wrap, b.overflow_wrap);
    field("font_kerning", a.font_kerning, b.font_kerning);
    field("font_synthesis_weight", a.font_synthesis_weight, b.font_synthesis_weight);
    field("font_synthesis_style", a.font_synthesis_style, b.font_synthesis_style);
    field("font_synthesis_small_caps", a.font_synthesis_small_caps, b.font_synthesis_small_caps);
    field("font_synthesis_position", a.font_synthesis_position, b.font_synthesis_position);
    field("text_transform", a.text_transform, b.text_transform);
    field("list_style_type", a.list_style_type, b.list_style_type);
    field("list_style_position", a.list_style_position, b.list_style_position);
    field("list_item_value", a.list_item_value, b.list_item_value);
    field("text_decoration", a.text_decoration, b.text_decoration);
    field("letter_spacing", a.letter_spacing, b.letter_spacing);
    field("word_spacing", a.word_spacing, b.word_spacing);
    field("text_indent", a.text_indent, b.text_indent);
    field("content", a.content, b.content);
    field("quotes", a.quotes, b.quotes);
    field("generated", a.generated, b.generated);
    field("counter_reset", a.counter_reset, b.counter_reset);
    field("counter_increment", a.counter_increment, b.counter_increment);
    field("counter_set", a.counter_set, b.counter_set);
    field("first_letter", a.first_letter, b.first_letter);
    field("custom", a.custom, b.custom);
    field("inherits_explicitly", a.inherits_explicitly, b.inherits_explicitly);
    field("selector_features", a.selector_features, b.selector_features);
    field("fill", a.fill, b.fill);
    field("stroke", a.stroke, b.stroke);
    field("fill_opacity", a.fill_opacity, b.fill_opacity);
    field("stroke_opacity", a.stroke_opacity, b.stroke_opacity);
    field("stroke_width", a.stroke_width, b.stroke_width);
    field("fill_rule", a.fill_rule, b.fill_rule);
    field("stroke_linecap", a.stroke_linecap, b.stroke_linecap);
    field("stroke_linejoin", a.stroke_linejoin, b.stroke_linejoin);
    field("stroke_miterlimit", a.stroke_miterlimit, b.stroke_miterlimit);
    field("stroke_dasharray", a.stroke_dasharray, b.stroke_dasharray);
    field("stroke_dashoffset", a.stroke_dashoffset, b.stroke_dashoffset);
    field("stop_color", a.stop_color, b.stop_color);
    field("stop_opacity", a.stop_opacity, b.stop_opacity);
    return found;
}

}
