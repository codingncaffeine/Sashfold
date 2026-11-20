#include "text/ShapedText.h"

#include "core/Bidi.h"
#include "core/Unicode.h"
#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace sashfold::text {

bool is_word_separator(char32_t c)
{
    // css-text-3 §8.1: the space, the no-break space, the Ethiopic word
    // space, the Aegean word separators, the Ugaritic word divider and the
    // Phoenician word separator.
    switch (c) {
    case 0x0020:
    case 0x00A0:
    case 0x1361:
    case 0x10100:
    case 0x10101:
    case 0x1039F:
    case 0x1091F:
        return true;
    default:
        return false;
    }
}

bool ShapedText::starts_cluster(std::size_t index) const
{
    return std::binary_search(cluster_starts.begin(), cluster_starts.end(), static_cast<std::uint32_t>(index));
}

std::size_t ShapedText::cluster_start_of(std::size_t index) const
{
    auto const after = std::upper_bound(cluster_starts.begin(), cluster_starts.end(), static_cast<std::uint32_t>(index));
    if (after == cluster_starts.begin())
        return 0;
    return *(after - 1);
}

std::size_t ShapedText::cluster_end_of(std::size_t index) const
{
    auto const after = std::upper_bound(cluster_starts.begin(), cluster_starts.end(), static_cast<std::uint32_t>(index));
    return after == cluster_starts.end() ? length : *after;
}

void ShapedText::character_edges(std::vector<float>& left, std::vector<float>& right) const
{
    left.assign(length, 0.0f);
    right.assign(length, 0.0f);
    if (length == 0)
        return;
    // The room each cluster's glyphs take on the line: from the leftmost
    // pen to the farthest pen plus advance among them.
    std::vector<float> from(length, 0.0f);
    std::vector<float> to(length, 0.0f);
    std::vector<bool> seen(length, false);
    for (PlacedGlyph const& g : glyphs) {
        std::size_t const c = cluster_start_of(std::min<std::size_t>(g.cluster, length - 1));
        float const a = g.x;
        float const b = g.x + g.advance + g.word_space;
        if (!seen[c]) {
            from[c] = a;
            to[c] = b;
            seen[c] = true;
        } else {
            from[c] = std::min(from[c], a);
            to[c] = std::max(to[c], b);
        }
    }
    for (std::size_t i = 0; i < cluster_starts.size(); ++i) {
        std::size_t const start = cluster_starts[i];
        std::size_t const end = i + 1 < cluster_starts.size() ? cluster_starts[i + 1] : length;
        if (start >= length || end <= start)
            continue;
        // A cluster with no glyph of its own (characters a font drew
        // nothing for) stands where the cluster before it ends.
        if (!seen[start]) {
            float const at = start > 0 ? right[start - 1] : 0.0f;
            from[start] = at;
            to[start] = at;
        }
        bool const rtl = !right_to_left.empty() && right_to_left[start];
        std::size_t const count = end - start;
        float const share = (to[start] - from[start]) / static_cast<float>(count);
        for (std::size_t k = 0; k < count; ++k) {
            std::size_t const character = start + k;
            // Shared out in the direction the cluster reads.
            std::size_t const slot = rtl ? count - 1 - k : k;
            left[character] = from[start] + share * static_cast<float>(slot);
            right[character] = slot + 1 == count ? to[start] : from[start] + share * static_cast<float>(slot + 1);
        }
    }
}

namespace {

// Whether a character stays in one cluster with the one before it, and so
// in one face: a mark, a default ignorable (a joiner, a variation
// selector), a skin tone, a pictograph joined on by a zero width joiner,
// and the vowel and final jamo of a Hangul syllable written out.
bool continues_cluster(char32_t c, char32_t previous)
{
    if (is_combining_mark(c) || is_default_ignorable(c))
        return true;
    if (c >= 0x1F3FB && c <= 0x1F3FF)
        return true;
    if (previous == 0x200D && character_properties(c).extended_pictographic)
        return true;
    bool const jamo_tail = (c >= 0x1160 && c <= 0x11FF) || (c >= 0xD7B0 && c <= 0xD7FF);
    bool const hangul_before = (previous >= 0x1100 && previous <= 0x11FF) || (previous >= 0xA960 && previous <= 0xA97F)
        || (previous >= 0xD7B0 && previous <= 0xD7FF) || (previous >= 0xAC00 && previous <= 0xD7A3);
    return jamo_tail && hangul_before;
}

// Whether a character's form depends on its neighbors': the letters of
// the scripts that join (Unicode §9.2), and the joiners themselves.
bool joins(char32_t c)
{
    if (c < 0x0600)
        return false;
    JoiningType const type = joining_type(c);
    return type != JoiningType::NonJoining && type != JoiningType::Transparent;
}

// The context a joining letter reads, cut to what it can read: up to and
// including the nearest character that is not transparent on each side.
std::u32string_view trimmed_before(std::u32string_view before)
{
    for (std::size_t i = before.size(); i > 0; --i) {
        if (joining_type(before[i - 1]) != JoiningType::Transparent)
            return before.substr(i - 1);
    }
    return {};
}

std::u32string_view trimmed_after(std::u32string_view after)
{
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (joining_type(after[i]) != JoiningType::Transparent)
            return after.substr(0, i + 1);
    }
    return {};
}

// One glyph as a piece's shaping gives it, before the pen places it.
struct Set {
    Face const* face = nullptr;
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0; // into the whole text
    float advance = 0; // px, no spacing
    float x_offset = 0;
    float y_offset = 0; // y down
};

struct Work {
    FontStack const& stack;
    FontManager* manager = nullptr;
    std::u32string_view text;
    std::u32string_view before;
    std::u32string_view after;
    TextShaping const& shaping;
    bool contextual = false;
    ShapedRun run; // reused between pieces
    std::u32string context_before;
    std::u32string context_after;
    std::vector<FeatureSetting> features;

    // The built-in face: the code point is the glyph, at the face's own
    // advance — measured as the fixed-pitch face it is.
    void builtin_piece(std::size_t start, std::size_t end, bool rtl, std::vector<Set>& out) const
    {
        Face const& face = builtin_face();
        std::size_t const from = out.size();
        for (std::size_t i = start; i < end; ++i) {
            char32_t const c = rtl ? bidi_mirrored(text[i]) : text[i];
            out.push_back(Set { &face, static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(i),
                face.advance(static_cast<std::uint32_t>(c), shaping.size), 0, 0 });
        }
        if (rtl)
            std::reverse(out.begin() + static_cast<std::ptrdiff_t>(from), out.end());
    }

    // The faces to try for a cluster, in order: the stack's own, then the
    // manager's fallback for its first character.
    Face const* candidate(std::size_t index, char32_t base) const
    {
        std::vector<Face const*> const& faces = stack.faces();
        if (index < faces.size())
            return faces[index];
        if (index == faces.size() && manager && manager->system_fonts())
            return manager->fallback_for(base);
        return nullptr;
    }

    // Shapes [start, end) of the text — one script, one direction — in
    // `face`, the `tried`-th candidate, and appends its glyphs in visual
    // order. A cluster the face has no glyph for is shaped again in the
    // next candidate that has every one of its characters.
    void piece(std::size_t start, std::size_t end, ScriptCode script, bool rtl, Face const* face, std::size_t tried,
        std::vector<Set>& out)
    {
        if (!face || &face->layout_face() == &builtin_face() || !face->truetype_font()) {
            builtin_piece(start, end, rtl, out);
            return;
        }
        ShapeRequest request;
        request.text = text.substr(start, end - start);
        if (contextual) {
            if (before.empty()) {
                request.before = text.substr(0, start);
            } else {
                context_before.assign(before);
                context_before.append(text.substr(0, start));
                request.before = context_before;
            }
            if (after.empty()) {
                request.after = text.substr(end);
            } else {
                context_after.assign(text.substr(end));
                context_after.append(after);
                request.after = context_after;
            }
        }
        request.script = script;
        request.language = shaping.language;
        request.direction = rtl ? TextDirection::RightToLeft : TextDirection::LeftToRight;
        features.clear();
        for (FeatureSetting const& setting : shaping.features) {
            if (setting.global()) {
                features.push_back(setting);
                continue;
            }
            std::size_t const from = std::max(setting.start, start);
            std::size_t const to = std::min(setting.end, end);
            if (from < to)
                features.push_back(FeatureSetting { setting.tag, setting.value, from - start, to - start });
        }
        request.features = features;
        request.beginning_of_text = start == 0 && before.empty();
        request.end_of_text = end == text.size() && after.empty();
        shape(*face, request, run);
        // A run shaped against its script's direction (a bidi override:
        // Latin at an odd level, Hebrew at an even one) comes back in the
        // script's own order: its clusters are put in the order the run
        // reads, each cluster's glyphs kept as they are.
        if (run.glyphs.size() > 1) {
            std::uint32_t const first = run.glyphs.front().cluster;
            std::uint32_t const last = run.glyphs.back().cluster;
            if ((rtl && first < last) || (!rtl && first > last)) {
                std::reverse(run.glyphs.begin(), run.glyphs.end());
                for (std::size_t i = 0; i < run.glyphs.size();) {
                    std::size_t j = i + 1;
                    while (j < run.glyphs.size() && run.glyphs[j].cluster == run.glyphs[i].cluster)
                        ++j;
                    std::reverse(run.glyphs.begin() + static_cast<std::ptrdiff_t>(i),
                        run.glyphs.begin() + static_cast<std::ptrdiff_t>(j));
                    i = j;
                }
            }
        }
        float const scale = shaping.size / static_cast<float>(run.units_per_em == 0 ? 1000 : run.units_per_em);
        std::size_t const length = end - start;
        // Which characters' clusters came out with a missing glyph.
        std::vector<bool> missing;
        for (ShapedGlyph const& g : run.glyphs) {
            if (g.glyph != 0)
                continue;
            if (missing.empty())
                missing.assign(length, false);
            missing[std::min<std::size_t>(g.cluster, length - 1)] = true;
        }
        if (missing.empty()) {
            for (ShapedGlyph const& g : run.glyphs)
                out.push_back(Set { face, g.glyph, static_cast<std::uint32_t>(start + g.cluster),
                    static_cast<float>(g.x_advance) * scale, static_cast<float>(g.x_offset) * scale,
                    -static_cast<float>(g.y_offset) * scale });
            return;
        }
        // The clusters: a character is in the cluster of the nearest start
        // at or before it. A missing glyph marks its whole cluster.
        std::vector<bool> starts(length, false);
        for (ShapedGlyph const& g : run.glyphs)
            starts[std::min<std::size_t>(g.cluster, length - 1)] = true;
        starts[0] = true;
        for (std::size_t i = 1; i < length; ++i) {
            if (!starts[i])
                missing[i] = missing[i - 1];
        }
        // Spans of one kind, in logical order; the run's glyphs are kept
        // for the spans it had glyphs for, the rest are shaped again.
        std::vector<ShapedGlyph> const glyphs = run.glyphs;
        std::vector<std::vector<Set>> spans;
        std::size_t span_start = 0;
        for (std::size_t i = 1; i <= length; ++i) {
            if (i < length && missing[i] == missing[span_start])
                continue;
            std::vector<Set> span;
            if (!missing[span_start]) {
                for (ShapedGlyph const& g : glyphs) {
                    if (g.cluster >= span_start && g.cluster < i)
                        span.push_back(Set { face, g.glyph, static_cast<std::uint32_t>(start + g.cluster),
                            static_cast<float>(g.x_advance) * scale, static_cast<float>(g.x_offset) * scale,
                            -static_cast<float>(g.y_offset) * scale });
                }
            } else {
                fallback(start + span_start, start + i, script, rtl, tried + 1, span);
            }
            spans.push_back(std::move(span));
            span_start = i;
        }
        if (rtl)
            std::reverse(spans.begin(), spans.end());
        for (std::vector<Set>& span : spans)
            out.insert(out.end(), span.begin(), span.end());
    }

    // Shapes clusters no earlier candidate had, cluster by cluster runs of
    // the same next face: the first candidate from `from` on with a glyph
    // for every character of the cluster that draws, else the built-in
    // face, which draws the box.
    void fallback(std::size_t start, std::size_t end, ScriptCode script, bool rtl, std::size_t from,
        std::vector<Set>& out)
    {
        std::vector<std::vector<Set>> spans;
        std::size_t i = start;
        Face const* span_face = nullptr;
        std::size_t span_index = 0;
        std::size_t span_start = start;
        auto const flush = [&](std::size_t to) {
            if (to <= span_start)
                return;
            std::vector<Set> span;
            piece(span_start, to, script, rtl, span_face, span_index, span);
            spans.push_back(std::move(span));
        };
        while (i < end) {
            std::size_t j = i + 1;
            while (j < end && continues_cluster(text[j], text[j - 1]))
                ++j;
            Face const* chosen = nullptr;
            std::size_t chosen_index = static_cast<std::size_t>(-1);
            for (std::size_t k = from;; ++k) {
                Face const* face = candidate(k, text[i]);
                if (!face) {
                    if (k > stack.faces().size())
                        break;
                    continue;
                }
                bool all = true;
                for (std::size_t c = i; c < j && all; ++c)
                    all = is_default_ignorable(text[c]) || face->glyph_index(text[c]) != 0;
                if (all) {
                    chosen = face;
                    chosen_index = k;
                    break;
                }
            }
            if (i == start || chosen != span_face) {
                flush(i);
                span_start = i;
                span_face = chosen;
                span_index = chosen_index;
            }
            i = j;
        }
        flush(end);
        if (rtl)
            std::reverse(spans.begin(), spans.end());
        for (std::vector<Set>& span : spans)
            out.insert(out.end(), span.begin(), span.end());
    }

    // One run of one level and one script: cut where the face changes, a
    // whole cluster at a time, each piece shaped, the pieces in visual
    // order.
    void script_run(std::size_t start, std::size_t end, ScriptCode script, bool rtl, std::vector<Set>& out)
    {
        std::vector<std::vector<Set>> pieces;
        std::size_t piece_start = start;
        Face const* piece_face = nullptr;
        std::size_t i = start;
        auto const flush = [&](std::size_t to) {
            if (to <= piece_start)
                return;
            std::vector<Set> glyphs;
            // The face's place among the candidates: what its fallback
            // tries after it.
            std::size_t index = 0;
            std::vector<Face const*> const& faces = stack.faces();
            while (index < faces.size() && faces[index] != piece_face)
                ++index;
            piece(piece_start, to, script, rtl, piece_face, index, glyphs);
            pieces.push_back(std::move(glyphs));
        };
        while (i < end) {
            std::size_t j = i + 1;
            while (j < end && continues_cluster(text[j], text[j - 1]))
                ++j;
            Face const* face = &stack.face_for(text[i]);
            if (i == start || face != piece_face) {
                flush(i);
                piece_start = i;
                piece_face = face;
            }
            i = j;
        }
        flush(end);
        if (rtl)
            std::reverse(pieces.begin(), pieces.end());
        for (std::vector<Set>& piece_glyphs : pieces)
            out.insert(out.end(), piece_glyphs.begin(), piece_glyphs.end());
    }
};

// The pen over the glyphs: letter-spacing after the visually last glyph of
// every cluster, word-spacing after a word separator's (css-text-3 §8.1,
// §8.2), each glyph's advance taken with its letter-spacing as one step and
// the word's worth added after — the order the built-in face's runs have
// always been stepped in, which its reference pictures are made with.
void place(std::vector<Set> const& set, std::u32string_view text, TextShaping const& shaping, ShapedText& out)
{
    out.glyphs.clear();
    out.glyphs.reserve(set.size());
    float x = 0;
    for (std::size_t i = 0; i < set.size(); ++i) {
        Set const& g = set[i];
        bool const last_of_cluster = i + 1 == set.size() || set[i + 1].cluster != g.cluster;
        PlacedGlyph placed;
        placed.face = g.face;
        placed.glyph = g.glyph;
        placed.cluster = g.cluster;
        placed.x = x;
        placed.x_offset = g.x_offset;
        placed.y_offset = g.y_offset;
        placed.advance = last_of_cluster ? g.advance + shaping.letter_spacing : g.advance;
        x += placed.advance;
        if (last_of_cluster && shaping.word_spacing != 0 && g.cluster < text.size() && is_word_separator(text[g.cluster])) {
            placed.word_space = shaping.word_spacing;
            x += shaping.word_spacing;
        }
        out.glyphs.push_back(placed);
    }
    out.width = x;
    out.pen_end = x;
}

void mark_clusters(ShapedText& out)
{
    out.cluster_starts.clear();
    if (out.length == 0)
        return;
    std::vector<bool> starts(out.length, false);
    starts[0] = true;
    for (PlacedGlyph const& g : out.glyphs) {
        if (g.cluster < out.length)
            starts[g.cluster] = true;
    }
    for (std::uint32_t i = 0; i < out.length; ++i) {
        if (starts[i])
            out.cluster_starts.push_back(i);
    }
}

// What a stack keeps of what it shaped, by everything that decides it.
struct KeyHash {
    using is_transparent = void;
    std::size_t operator()(std::u32string_view key) const { return std::hash<std::u32string_view> {}(key); }
};

void append_float(std::u32string& key, float value)
{
    key.push_back(static_cast<char32_t>(std::bit_cast<std::uint32_t>(value)));
}

}

struct FontStack::ShapeCache {
    static constexpr std::size_t text_limit = 64; // code points: longer texts are shaped each time
    static constexpr std::size_t bound = 16384;
    std::mutex mutex;
    std::uint64_t filled = 0; // the stack's m_faces_filled the entries were shaped with
    std::unordered_map<std::u32string, std::shared_ptr<ShapedText const>, KeyHash, std::equal_to<>> entries;
};

FontStack::FontStack() = default;
FontStack::~FontStack() = default;

std::shared_ptr<ShapedText const> FontStack::shape(std::u32string_view text, TextShaping const& shaping,
    std::u32string_view before, std::u32string_view after) const
{
    auto result = std::make_shared<ShapedText>();
    ShapedText& out = *result;
    out.length = static_cast<std::uint32_t>(text.size());
    if (text.empty())
        return result;
    bool const rtl_base = shaping.direction == TextDirection::RightToLeft;

    // The built-in face alone is fixed pitch and shapes nothing: a glyph
    // per character, the code point its id, and a width that is a count
    // times one advance, exact where a running sum would drift. A run of
    // one right-to-left level is drawn last character first, mirrored; a
    // paragraph keeps the order it is written in, as text in this face
    // always has been outside a line's own reordering.
    if (builtin_alone()) {
        bool const reversed = rtl_base && !shaping.paragraph;
        {
            Face const& face = *m_faces[0];
            float const advance = face.advance(0, shaping.size);
            out.glyphs.reserve(text.size());
            float x = 0;
            std::size_t separators = 0;
            for (std::size_t k = 0; k < text.size(); ++k) {
                std::size_t const i = reversed ? text.size() - 1 - k : k;
                char32_t const c = reversed ? bidi_mirrored(text[i]) : text[i];
                PlacedGlyph g;
                g.face = &face;
                g.glyph = static_cast<std::uint32_t>(c);
                g.cluster = static_cast<std::uint32_t>(i);
                g.x = x;
                g.advance = advance + shaping.letter_spacing;
                x += g.advance;
                if (shaping.word_spacing != 0 && is_word_separator(text[i])) {
                    g.word_space = shaping.word_spacing;
                    x += shaping.word_spacing;
                    ++separators;
                }
                out.glyphs.push_back(g);
            }
            out.pen_end = x;
            float extra = 0;
            if (shaping.letter_spacing != 0 || shaping.word_spacing != 0) {
                extra = shaping.letter_spacing * static_cast<float>(text.size());
                for (std::size_t s = 0; s < separators; ++s)
                    extra += shaping.word_spacing;
            }
            out.width = static_cast<float>(text.size()) * advance + extra;
            out.cluster_starts.resize(text.size());
            for (std::uint32_t i = 0; i < out.length; ++i)
                out.cluster_starts[i] = i;
            if (reversed)
                out.right_to_left.assign(text.size(), true);
            return result;
        }
    }

    // Context matters only to text whose letters join.
    bool contextual = false;
    for (char32_t const c : text)
        contextual = contextual || joins(c);
    std::u32string_view const context_before = contextual ? trimmed_before(before) : std::u32string_view {};
    std::u32string_view const context_after = contextual ? trimmed_after(after) : std::u32string_view {};

    if (!m_shapes)
        m_shapes = std::make_unique<ShapeCache>();
    bool const cacheable = text.size() <= ShapeCache::text_limit;
    thread_local std::u32string key;
    if (cacheable) {
        key.clear();
        append_float(key, shaping.size);
        append_float(key, shaping.letter_spacing);
        append_float(key, shaping.word_spacing);
        key.push_back(static_cast<char32_t>((rtl_base ? 1u : 0u) | (shaping.paragraph ? 2u : 0u)));
        for (char const c : shaping.language)
            key.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
        key.push_back(0);
        for (FeatureSetting const& setting : shaping.features) {
            key.push_back(static_cast<char32_t>(setting.tag));
            key.push_back(static_cast<char32_t>(setting.value));
            key.push_back(static_cast<char32_t>(setting.start));
            key.push_back(static_cast<char32_t>(setting.end));
        }
        key.push_back(0);
        key.push_back(static_cast<char32_t>(context_before.size()));
        key.append(context_before);
        key.push_back(static_cast<char32_t>(context_after.size()));
        key.append(context_after);
        key.push_back(static_cast<char32_t>((before.empty() ? 1u : 0u) | (after.empty() ? 2u : 0u)));
        key.append(text);
        std::lock_guard<std::mutex> const lock(m_shapes->mutex);
        if (m_shapes->filled != m_faces_filled) {
            m_shapes->entries.clear();
            m_shapes->filled = m_faces_filled;
        }
        if (auto const found = m_shapes->entries.find(std::u32string_view(key)); found != m_shapes->entries.end())
            return found->second;
    }
    // A face the page had at hand can come while the text is shaped (a
    // character asked for it): the stack is filled again then, and what
    // was shaped with both sets of faces is not kept.
    std::uint64_t const faces = m_faces_filled;

    // The levels: one throughout for a run of one level, resolved by
    // UAX #9 for a paragraph that has any right-to-left character in it.
    std::vector<std::uint8_t> levels(text.size(), rtl_base ? 1 : 0);
    if (shaping.paragraph) {
        bool any = rtl_base;
        for (char32_t const c : text)
            any = any || c >= 0x0590;
        if (any) {
            BidiParagraph const resolved = bidi_resolve(text, static_cast<std::uint8_t>(rtl_base ? 1 : 0));
            for (std::size_t i = 0; i < text.size() && i < resolved.levels.size(); ++i) {
                bool const removed = i < resolved.removed.size() && resolved.removed[i];
                levels[i] = removed ? (i > 0 ? levels[i - 1] : resolved.paragraph_level) : resolved.levels[i];
            }
        }
    }
    struct LevelRun {
        std::size_t start;
        std::size_t end;
        std::uint8_t level;
    };
    std::vector<LevelRun> level_runs;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t j = i + 1;
        while (j < text.size() && levels[j] == levels[i])
            ++j;
        level_runs.push_back(LevelRun { i, j, levels[i] });
        i = j;
    }
    // Rule L2 over the runs: from the highest level down to the lowest odd
    // one, every stretch of runs at that level or above is reversed.
    if (level_runs.size() > 1) {
        std::uint8_t highest = 0;
        std::uint8_t lowest_odd = 255;
        for (LevelRun const& run : level_runs) {
            highest = std::max(highest, run.level);
            if (run.level & 1)
                lowest_odd = std::min(lowest_odd, run.level);
        }
        for (int level = highest; level >= lowest_odd && level > 0; --level) {
            for (std::size_t i = 0; i < level_runs.size();) {
                if (level_runs[i].level < level) {
                    ++i;
                    continue;
                }
                std::size_t j = i;
                while (j < level_runs.size() && level_runs[j].level >= level)
                    ++j;
                std::reverse(level_runs.begin() + static_cast<std::ptrdiff_t>(i), level_runs.begin() + static_cast<std::ptrdiff_t>(j));
                i = j;
            }
        }
    }

    Work work { *this, m_manager, text, context_before, context_after, shaping, contextual, {}, {}, {}, {} };
    std::vector<Set> set;
    set.reserve(text.size());
    for (LevelRun const& level_run : level_runs) {
        bool const rtl = (level_run.level & 1) != 0;
        std::vector<ScriptRun> const script_runs = itemize_scripts(text.substr(level_run.start, level_run.end - level_run.start));
        std::vector<std::vector<Set>> pieces;
        for (ScriptRun const& script_run : script_runs) {
            std::vector<Set> glyphs;
            work.script_run(level_run.start + script_run.start, level_run.start + script_run.end, script_run.script, rtl, glyphs);
            pieces.push_back(std::move(glyphs));
        }
        if (rtl)
            std::reverse(pieces.begin(), pieces.end());
        for (std::vector<Set>& piece : pieces)
            set.insert(set.end(), piece.begin(), piece.end());
    }
    place(set, text, shaping, out);
    mark_clusters(out);
    bool any_rtl = false;
    for (std::uint8_t const level : levels)
        any_rtl = any_rtl || (level & 1);
    if (any_rtl) {
        out.right_to_left.resize(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            out.right_to_left[i] = (levels[i] & 1) != 0;
    }

    if (cacheable && faces == m_faces_filled) {
        std::lock_guard<std::mutex> const lock(m_shapes->mutex);
        if (m_shapes->filled != m_faces_filled) {
            m_shapes->entries.clear();
            m_shapes->filled = m_faces_filled;
        }
        if (m_shapes->entries.size() >= ShapeCache::bound)
            m_shapes->entries.clear();
        m_shapes->entries.emplace(key, result);
    }
    return result;
}

float FontStack::measure(std::u32string_view text, float size, bool kern) const
{
    // The built-in face alone is fixed pitch: count times advance.
    if (builtin_alone())
        return static_cast<float>(text.size()) * m_faces[0]->advance(0, size);
    TextShaping shaping;
    shaping.size = size;
    shaping.paragraph = true;
    if (!kern)
        shaping.features.push_back(FeatureSetting { feature_tag('k', 'e', 'r', 'n'), 0 });
    return shape(text, shaping)->width;
}

}
