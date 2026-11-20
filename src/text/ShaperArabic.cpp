// The shaper of Arabic and the scripts that join like it (Syriac, N'Ko,
// Mandaic, Mongolian, Adlam, Hanifi Rohingya, ...): the cursive joining
// analysis of Unicode §9.2 (with the Syriac Alaph rules of §9.3) from the
// characters' joining types, stepping over transparent marks; the joining
// forms applied as the features 'isol' 'fina' 'fin2' 'fin3' 'medi' 'med2'
// 'init', each in a stage of its own, then 'rlig', 'calt' and 'mset' — the
// order of the OpenType script development document for Arabic; and the
// mark reordering of UTR #53 before shaping.

#include "text/ShaperInternal.h"

#include <algorithm>
#include <array>
#include <vector>

namespace sashfold::text::shaping {

namespace {

using otl::make_tag;

// The forms, in the order their masks are kept; None is no form.
enum Form : std::uint8_t {
    Isol,
    Fina,
    Fin2,
    Fin3,
    Medi,
    Med2,
    Init,
    None,
};

constexpr std::array<std::uint32_t, 7> form_tags {
    make_tag('i', 's', 'o', 'l'),
    make_tag('f', 'i', 'n', 'a'),
    make_tag('f', 'i', 'n', '2'),
    make_tag('f', 'i', 'n', '3'),
    make_tag('m', 'e', 'd', 'i'),
    make_tag('m', 'e', 'd', '2'),
    make_tag('i', 'n', 'i', 't'),
};

bool is_syriac_form(std::uint32_t tag)
{
    return tag == form_tags[Fin2] || tag == form_tags[Fin3] || tag == form_tags[Med2];
}

// The joining analysis as a walk over the non-transparent characters: what
// the previous one was decides how the current one joins and whether the
// previous one's form changes.
enum State : std::uint8_t {
    AfterNonJoining, // nothing before, or a character that joins nothing
    AfterRight, // a right-joining letter, or an Alaph that stands alone at the start
    AfterJoinable, // a dual- or left-joining letter not joined to the one before it
    AfterJoined, // a dual-joining letter joined to the one before it
    AfterAlaphJoined, // an Alaph joined to the letter before it
    AfterAlaphFinal, // an Alaph after a letter that does not join it
    AfterDalathRish, // Dalath or Rish: an Alaph after it takes 'fin3'
};

enum Column : std::uint8_t {
    ColNone, // U
    ColLeft, // L
    ColRight, // R
    ColDual, // D and C
    ColAlaph,
    ColDalathRish,
    ColTransparent,
};

Column column_of(char32_t c)
{
    switch (joining_type(c)) {
    case JoiningType::NonJoining: return ColNone;
    case JoiningType::LeftJoining: return ColLeft;
    case JoiningType::RightJoining: return ColRight;
    case JoiningType::DualJoining:
    case JoiningType::JoinCausing: return ColDual;
    case JoiningType::Alaph: return ColAlaph;
    case JoiningType::DalathRish: return ColDalathRish;
    case JoiningType::Transparent: return ColTransparent;
    }
    return ColNone;
}

struct Step {
    Form previous; // the previous letter's new form, None to leave it
    Form current;
    State next;
};

Step step(State state, Column column)
{
    // A letter that joins on its right (R, D, Alaph, Dalath-Rish) joins a
    // joinable letter before it: that one becomes initial or medial, this
    // one final. An Alaph's forms follow §9.3: joined, it is final, or
    // 'med2' when a joining letter follows; unjoined, it is 'fin2' ('fin3'
    // after Dalath or Rish), or isolated when a joining letter follows.
    bool const joinable = state == AfterJoinable || state == AfterJoined;
    Form const joined_previous = state == AfterJoined ? Medi : Init;
    switch (column) {
    case ColNone:
        return { None, None, AfterNonJoining };
    case ColLeft:
        return { None, Isol, AfterJoinable };
    case ColRight:
    case ColDual:
    case ColDalathRish: {
        State const after_isolated = column == ColDual ? AfterJoinable : column == ColRight ? AfterRight : AfterDalathRish;
        State const after_joined = column == ColDual ? AfterJoined : column == ColRight ? AfterRight : AfterDalathRish;
        if (joinable)
            return { joined_previous, Fina, after_joined };
        if (state == AfterAlaphJoined)
            return { Med2, Isol, after_isolated };
        if (state == AfterAlaphFinal)
            return { Isol, Isol, after_isolated };
        return { None, Isol, after_isolated };
    }
    case ColAlaph:
        if (joinable)
            return { joined_previous, Fina, AfterAlaphJoined };
        switch (state) {
        case AfterNonJoining: return { None, Isol, AfterRight };
        case AfterAlaphJoined: return { Med2, Fin2, AfterAlaphFinal };
        case AfterAlaphFinal: return { Isol, Fin2, AfterAlaphFinal };
        case AfterDalathRish: return { None, Fin3, AfterAlaphFinal };
        default: return { None, Fin2, AfterAlaphFinal };
        }
    case ColTransparent:
        break;
    }
    return { None, None, state };
}

bool is_mongolian_variation_selector(char32_t c)
{
    return (c >= 0x180B && c <= 0x180D) || c == 0x180F;
}

void setup_masks(Plan const& plan, ShapeFont const&, Buffer& b, std::u32string_view before, std::u32string_view after)
{
    std::size_t const count = b.len();
    std::size_t previous = static_cast<std::size_t>(-1);
    State state = AfterNonJoining;
    // The text before the run decides how its first letter joins.
    for (std::size_t k = before.size(); k > 0; --k) {
        Column const column = column_of(before[k - 1]);
        if (column == ColTransparent)
            continue;
        state = step(AfterNonJoining, column).next;
        break;
    }
    for (std::size_t i = 0; i < count; ++i) {
        Column const column = column_of(b.info[i].codepoint);
        if (column == ColTransparent) {
            b.info[i].shaper = None;
            continue;
        }
        Step const s = step(state, column);
        if (s.previous != None && previous != static_cast<std::size_t>(-1))
            b.info[previous].shaper = s.previous;
        b.info[i].shaper = s.current;
        previous = i;
        state = s.next;
    }
    // And the text after it how its last letter does.
    for (char32_t c : after) {
        Column const column = column_of(c);
        if (column == ColTransparent)
            continue;
        Step const s = step(state, column);
        if (s.previous != None && previous != static_cast<std::size_t>(-1))
            b.info[previous].shaper = s.previous;
        break;
    }
    // A Mongolian free variation selector takes its letter's form, so that
    // the font's lookups for the variant see it in the same feature.
    for (std::size_t i = 1; i < count; ++i) {
        if (is_mongolian_variation_selector(b.info[i].codepoint))
            b.info[i].shaper = b.info[i - 1].shaper;
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t const form = b.info[i].shaper;
        if (form < None)
            b.info[i].mask |= plan.shaper_masks[form];
    }
}

void collect_features(PlanBuilder& p)
{
    // 'stch' first, then the forms each in its own stage so that each sees
    // the result of the one before, as the Arabic document orders them.
    p.enable_feature(make_tag('s', 't', 'c', 'h'));
    p.add_gsub_pause(nullptr);
    p.enable_feature(make_tag('c', 'c', 'm', 'p'), feature_flag::manual_zwj);
    p.enable_feature(make_tag('l', 'o', 'c', 'l'), feature_flag::manual_zwj);
    p.add_gsub_pause(nullptr);
    for (std::uint32_t tag : form_tags) {
        bool const fallback = p.script == scripts::arabic && !is_syriac_form(tag);
        p.add_feature(tag, fallback ? feature_flag::has_fallback : 0);
        p.add_gsub_pause(nullptr);
    }
    p.add_gsub_pause(nullptr);
    p.enable_feature(make_tag('r', 'l', 'i', 'g'), feature_flag::manual_zwj | feature_flag::has_fallback);
    if (p.script == scripts::arabic)
        p.add_gsub_pause(arabic_fallback_shape);
    // 'rclt' and 'calt' together, without a pause between.
    p.enable_feature(make_tag('r', 'c', 'l', 't'), feature_flag::manual_zwj);
    p.enable_feature(make_tag('c', 'a', 'l', 't'), feature_flag::manual_zwj);
    p.add_gsub_pause(nullptr);
    p.enable_feature(make_tag('m', 's', 'e', 't'));
}

void init_plan(Plan& plan)
{
    bool fallback = plan.script == scripts::arabic;
    for (std::size_t k = 0; k < form_tags.size(); ++k) {
        plan.shaper_masks[k] = plan.one_mask(form_tags[k]);
        fallback = fallback && (is_syriac_form(form_tags[k]) || plan.needs_fallback(form_tags[k]));
    }
    plan.shaper_fallback = fallback;
}

// UTR #53 (Arabic Mark Rendering): within a sorted run of marks, the
// modifier combining marks of class 220 and then those of class 230 move
// to the front, where the font expects them right after their base. They
// are renumbered below every Arabic class so the run stays sorted.
bool is_modifier_combining_mark(char32_t c)
{
    switch (c) {
    case 0x0654: case 0x0655: case 0x0658: case 0x06DC: case 0x06E3: case 0x06E7: case 0x06E8: case 0x08CA:
    case 0x08CB: case 0x08CD: case 0x08CE: case 0x08CF: case 0x08D3: case 0x08F3:
        return true;
    default:
        return false;
    }
}

void reorder_marks(Plan const&, Buffer& b, std::size_t start, std::size_t end)
{
    std::size_t i = start;
    for (unsigned cc = 220; cc <= 230; cc += 10) {
        while (i < end && b.info[i].combining_class < cc)
            ++i;
        if (i == end)
            break;
        if (b.info[i].combining_class > cc)
            continue;
        std::size_t j = i;
        while (j < end && b.info[j].combining_class == cc && is_modifier_combining_mark(b.info[j].codepoint))
            ++j;
        if (i == j)
            continue;
        b.merge_clusters(start, j);
        std::vector<GlyphInfo> moved(b.info.begin() + static_cast<std::ptrdiff_t>(i), b.info.begin() + static_cast<std::ptrdiff_t>(j));
        std::move_backward(b.info.begin() + static_cast<std::ptrdiff_t>(start), b.info.begin() + static_cast<std::ptrdiff_t>(i),
            b.info.begin() + static_cast<std::ptrdiff_t>(j));
        std::copy(moved.begin(), moved.end(), b.info.begin() + static_cast<std::ptrdiff_t>(start));
        std::size_t const new_start = start + (j - i);
        std::uint8_t const renumbered = cc == 220 ? 1 : 2;
        for (; start < new_start; ++start)
            b.info[start].combining_class = renumbered;
        i = j;
    }
}

}

ScriptShaper const arabic_shaper {
    collect_features,
    nullptr,
    init_plan,
    nullptr,
    nullptr,
    Normalization::ComposedDiacritics,
    nullptr,
    nullptr,
    setup_masks,
    reorder_marks,
    0,
    ZeroMarks::ByGdefLate,
    true,
};

void arabic_fallback_shape(Plan const& plan, ShapeFont const& font, Buffer& b)
{
    using otl::make_tag;
    if (!plan.shaper_fallback)
        return;
    // The joining forms from the presentation forms the font maps (the
    // Unicode compatibility decompositions <isolated>, <final>, <initial>,
    // <medial> of single letters), for a font that has them in its cmap
    // but no GSUB for them.
    for (GlyphInfo& g : b.info) {
        std::uint8_t const form = g.shaper;
        if (form != Isol && form != Fina && form != Init && form != Medi)
            continue;
        if (!(g.mask & plan.shaper_masks[form]))
            continue;
        std::uint8_t const presentation = form == Isol ? 0 : form == Fina ? 1 : form == Init ? 2 : 3;
        char32_t const shaped = arabic_presentation_form(g.codepoint, presentation);
        if (!shaped)
            continue;
        if (std::uint32_t const glyph = font.nominal_glyph(shaped)) {
            g.glyph = glyph;
            g.props |= otl::glyph_props::substituted;
        }
    }
    // 'rlig': lam and alef as one, from the ligature presentation forms
    // (<isolated> or <final> of the pair) — isolated when the lam began
    // the pair, final when a letter joined it from before. Marks between
    // the two stay, numbered by the component they sat on.
    std::uint32_t const rlig = plan.one_mask(make_tag('r', 'l', 'i', 'g'));
    b.clear_output();
    std::size_t const count = b.len();
    b.idx = 0;
    while (b.idx < count && !b.failed) {
        GlyphInfo const& lam = b.cur();
        if (lam.codepoint != 0x0644 || !(lam.mask & rlig) || (lam.shaper != Init && lam.shaper != Medi)) {
            b.next_glyph();
            continue;
        }
        std::size_t j = b.idx + 1;
        while (j < count && is_unicode_mark(b.info[j]))
            ++j;
        if (j >= count || b.info[j].shaper != Fina) {
            b.next_glyph();
            continue;
        }
        char32_t const ligature = arabic_presentation_ligature(0x0644, b.info[j].codepoint, lam.shaper == Init ? 0 : 1);
        std::uint32_t const glyph = ligature ? font.nominal_glyph(ligature) : 0;
        if (!glyph) {
            b.next_glyph();
            continue;
        }
        unsigned const id = b.next_lig_id();
        b.merge_clusters(b.idx, j + 1);
        GlyphInfo& out = b.output_glyph(glyph);
        out.props = static_cast<std::uint16_t>(otl::glyph_props::ligature | otl::glyph_props::substituted | otl::glyph_props::ligated);
        otl::set_lig_props_for_ligature(out, id, 2);
        b.skip_glyph();
        while (b.idx < j) {
            otl::set_lig_props_for_mark(b.cur(), id, 1);
            b.next_glyph();
        }
        b.skip_glyph(); // the alef
        while (b.idx < count && is_unicode_mark(b.cur()) && otl::lig_id(b.cur()) == 0) {
            otl::set_lig_props_for_mark(b.cur(), id, 2);
            b.next_glyph();
        }
    }
    b.sync();
}

}
