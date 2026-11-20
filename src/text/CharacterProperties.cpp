#include "text/CharacterProperties.h"

#include "core/Bidi.h"
#include "text/ShapingData.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace sashfold::text {

namespace {

shaping_data::Record const& record_of(char32_t c)
{
    if (c > 0x10FFFF)
        c = 0x10FFFF; // a noncharacter: Cn, Zzzz
    std::uint16_t const block = shaping_data::record_blocks[c >> shaping_data::record_shift];
    std::uint32_t const at = static_cast<std::uint32_t>(block) << shaping_data::record_shift
        | (c & ((1u << shaping_data::record_shift) - 1));
    return shaping_data::records[shaping_data::record_indices[at]];
}

ScriptCode code_of_index(std::uint8_t index)
{
    char const* const code = shaping_data::script_codes[index < shaping_data::script_count ? index : 0];
    return script_code(code[0], code[1], code[2], code[3]);
}

// Hangul syllable arithmetic (Unicode §3.12).
constexpr char32_t hangul_s_base = 0xAC00;
constexpr char32_t hangul_l_base = 0x1100;
constexpr char32_t hangul_v_base = 0x1161;
constexpr char32_t hangul_t_base = 0x11A7;
constexpr char32_t hangul_l_count = 19;
constexpr char32_t hangul_v_count = 21;
constexpr char32_t hangul_t_count = 28;
constexpr char32_t hangul_n_count = hangul_v_count * hangul_t_count;
constexpr char32_t hangul_s_count = hangul_l_count * hangul_n_count;

}

CharacterProperties character_properties(char32_t c)
{
    shaping_data::Record const& r = record_of(c);
    CharacterProperties p;
    p.category = static_cast<GeneralCategory>(r.category);
    p.script = r.script;
    p.joining = static_cast<JoiningType>(r.joining & 7);
    p.combining_class = r.combining_class;
    p.indic_syllabic = r.indic_syllabic;
    p.indic_position = r.indic_position;
    p.extended_pictographic = (r.joining & 0x80) != 0;
    return p;
}

GeneralCategory general_category(char32_t c)
{
    return static_cast<GeneralCategory>(record_of(c).category);
}

std::uint8_t combining_class(char32_t c)
{
    return record_of(c).combining_class;
}

ScriptCode script_of(char32_t c)
{
    return code_of_index(record_of(c).script);
}

std::vector<ScriptCode> script_extensions(char32_t c)
{
    auto const* const begin = std::begin(shaping_data::script_extensions);
    auto const* const end = std::end(shaping_data::script_extensions);
    auto const* const it = std::upper_bound(begin, end, c,
        [](char32_t value, shaping_data::ScriptExtension const& e) { return value < e.first; });
    std::vector<ScriptCode> out;
    if (it != begin) {
        auto const& e = *(it - 1);
        if (c >= e.first && c <= e.last) {
            for (std::size_t k = 0; k < e.count; ++k)
                out.push_back(code_of_index(shaping_data::script_extension_lists[e.offset + k]));
            return out;
        }
    }
    out.push_back(script_of(c));
    return out;
}

JoiningType joining_type(char32_t c)
{
    return static_cast<JoiningType>(record_of(c).joining & 7);
}

bool canonical_decompose(char32_t c, char32_t& first, char32_t& second)
{
    if (c >= hangul_s_base && c < hangul_s_base + hangul_s_count) {
        char32_t const index = c - hangul_s_base;
        if (index % hangul_t_count != 0) {
            first = c - index % hangul_t_count; // LV
            second = hangul_t_base + index % hangul_t_count;
        } else {
            first = hangul_l_base + index / hangul_n_count;
            second = hangul_v_base + (index % hangul_n_count) / hangul_t_count;
        }
        return true;
    }
    auto const* const begin = std::begin(shaping_data::decompositions);
    auto const* const end = std::end(shaping_data::decompositions);
    auto const* const it = std::lower_bound(begin, end, c,
        [](shaping_data::Decomposition const& d, char32_t value) { return d.code_point < value; });
    if (it == end || it->code_point != c)
        return false;
    first = it->first;
    second = it->second;
    return true;
}

char32_t canonical_compose(char32_t first, char32_t second)
{
    // Hangul: L + V makes LV; LV + T makes LVT.
    if (first >= hangul_l_base && first < hangul_l_base + hangul_l_count && second >= hangul_v_base
        && second < hangul_v_base + hangul_v_count)
        return hangul_s_base + ((first - hangul_l_base) * hangul_v_count + (second - hangul_v_base)) * hangul_t_count;
    if (first >= hangul_s_base && first < hangul_s_base + hangul_s_count && (first - hangul_s_base) % hangul_t_count == 0
        && second > hangul_t_base && second < hangul_t_base + hangul_t_count)
        return first + (second - hangul_t_base);
    auto const* const begin = std::begin(shaping_data::compositions);
    auto const* const end = std::end(shaping_data::compositions);
    auto const* const it = std::lower_bound(begin, end, std::pair { first, second },
        [](shaping_data::Composition const& c, std::pair<char32_t, char32_t> const& key) {
            return c.first < key.first || (c.first == key.first && c.second < key.second);
        });
    if (it == end || it->first != first || it->second != second)
        return 0;
    return it->composite;
}

char32_t arabic_presentation_form(char32_t c, std::uint8_t form)
{
    // Few enough to search; the first in code point order wins.
    for (shaping_data::PresentationForm const& p : shaping_data::presentation_forms) {
        if (p.first == c && p.second == 0 && p.form == form)
            return p.code_point;
    }
    return 0;
}

char32_t arabic_presentation_ligature(char32_t first, char32_t second, std::uint8_t form)
{
    for (shaping_data::PresentationForm const& p : shaping_data::presentation_forms) {
        if (p.first == first && p.second == second && p.form == form)
            return p.code_point;
    }
    return 0;
}

// --- Itemization ------------------------------------------------------------

namespace {

bool is_real_script(ScriptCode script)
{
    return script != scripts::common && script != scripts::inherited && script != scripts::unknown;
}

}

std::vector<ScriptRun> itemize_scripts(std::u32string_view text)
{
    std::vector<ScriptRun> runs;
    if (text.empty())
        return runs;
    // The brackets still open: the closing bracket each waits for and the
    // index of the run that was current when it opened. Bounded, as other
    // implementations bound theirs: past the bound an opening bracket is
    // taken as plain Common punctuation.
    struct Open {
        char32_t closing;
        std::size_t run;
    };
    constexpr std::size_t max_open = 64;
    std::vector<Open> open;
    runs.push_back(ScriptRun { 0, 0, scripts::common });
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t const c = text[i];
        CharacterProperties const p = character_properties(c);
        ScriptCode const script = code_of_index(p.script);
        ScriptRun& current = runs.back();
        if (!is_real_script(script)) {
            // A closing bracket goes with the run its opening bracket was in.
            if (p.category == GeneralCategory::Pe) {
                for (std::size_t k = open.size(); k > 0; --k) {
                    if (open[k - 1].closing != c)
                        continue;
                    std::size_t const run = open[k - 1].run;
                    open.resize(k - 1);
                    if (run + 1 != runs.size() && runs[run].script != current.script && is_real_script(current.script)
                        && is_real_script(runs[run].script)) {
                        current.end = i;
                        runs.push_back(ScriptRun { i, i, runs[run].script });
                    }
                    break;
                }
            } else if (p.category == GeneralCategory::Ps) {
                char32_t const closing = bidi_mirrored(c);
                if (closing != c && open.size() < max_open)
                    open.push_back(Open { closing, runs.size() - 1 });
            }
            runs.back().end = i + 1;
            continue;
        }
        if (current.script == script) {
            current.end = i + 1;
            continue;
        }
        if (!is_real_script(current.script)) {
            // Everything so far in this run was Common: it takes this script.
            current.script = script;
            current.end = i + 1;
            continue;
        }
        // A character used with the current script stays in its run.
        std::vector<ScriptCode> const extensions = script_extensions(c);
        if (std::find(extensions.begin(), extensions.end(), current.script) != extensions.end()) {
            current.end = i + 1;
            continue;
        }
        runs.push_back(ScriptRun { i, i + 1, script });
    }
    // Runs that met again after a bracket closed are merged back.
    std::vector<ScriptRun> merged;
    for (ScriptRun const& run : runs) {
        if (run.start == run.end)
            continue;
        if (!merged.empty() && merged.back().script == run.script)
            merged.back().end = run.end;
        else
            merged.push_back(run);
    }
    return merged;
}

bool script_is_right_to_left(ScriptCode script)
{
    // Data-driven: a script is right-to-left when its first letter is.
    static std::array<std::uint8_t, 256> const rtl = [] {
        std::array<std::uint8_t, 256> seen {};
        for (char32_t c = 0; c < 0x20000; ++c) {
            shaping_data::Record const& r = record_of(c);
            if (seen[r.script] != 0)
                continue;
            auto const category = static_cast<GeneralCategory>(r.category);
            if (category != GeneralCategory::Lo && category != GeneralCategory::Lu && category != GeneralCategory::Ll)
                continue;
            seen[r.script] = strong_direction(c) == StrongDirection::Rtl ? 2 : 1;
        }
        return seen;
    }();
    for (std::uint8_t k = 0; k < shaping_data::script_count; ++k) {
        if (code_of_index(k) == script)
            return rtl[k] == 2;
    }
    return false;
}

OpenTypeScriptTags opentype_script_tags(ScriptCode script)
{
    OpenTypeScriptTags out;
    auto push = [&](std::uint32_t tag) {
        if (out.count < 3)
            out.tags[out.count++] = tag;
    };
    auto tag = [](char const* s) { return script_code(s[0], s[1], s[2], s[3]); };
    if (!is_real_script(script))
        return out;
    // The scripts whose second version of shaping has its own tag.
    struct Versioned {
        ScriptCode script;
        char const* v2;
        char const* v1;
    };
    static constexpr Versioned versioned[] = {
        { script_code('B', 'e', 'n', 'g'), "bng2", "beng" },
        { script_code('D', 'e', 'v', 'a'), "dev2", "deva" },
        { script_code('G', 'u', 'j', 'r'), "gjr2", "gujr" },
        { script_code('G', 'u', 'r', 'u'), "gur2", "guru" },
        { script_code('K', 'n', 'd', 'a'), "knd2", "knda" },
        { script_code('M', 'l', 'y', 'm'), "mlm2", "mlym" },
        { script_code('O', 'r', 'y', 'a'), "ory2", "orya" },
        { script_code('T', 'a', 'm', 'l'), "tml2", "taml" },
        { script_code('T', 'e', 'l', 'u'), "tel2", "telu" },
        { script_code('M', 'y', 'm', 'r'), "mym2", "mymr" },
    };
    for (Versioned const& v : versioned) {
        if (v.script == script) {
            // The Indic scripts' third version ('dev3'), whose fonts are made
            // for the Universal Shaping Engine, before the second.
            if (script != script_code('M', 'y', 'm', 'r')) {
                char v3[5] = { v.v2[0], v.v2[1], v.v2[2], '3', 0 };
                push(tag(v3));
            }
            push(tag(v.v2));
            push(tag(v.v1));
            return out;
        }
    }
    // The registry's tags that are not the ISO code in lower case.
    struct Special {
        ScriptCode script;
        char const* ot;
    };
    static constexpr Special specials[] = {
        { script_code('H', 'i', 'r', 'a'), "kana" },
        { script_code('H', 'r', 'k', 't'), "kana" },
        { script_code('L', 'a', 'o', 'o'), "lao " },
        { script_code('Y', 'i', 'i', 'i'), "yi  " },
        { script_code('N', 'k', 'o', 'o'), "nko " },
        { script_code('V', 'a', 'i', 'i'), "vai " },
        { script_code('Z', 'm', 't', 'h'), "math" },
        { script_code('H', 'a', 'n', 's'), "hani" },
        { script_code('H', 'a', 'n', 't'), "hani" },
        { script_code('S', 'y', 'r', 'e'), "syrc" },
        { script_code('S', 'y', 'r', 'j'), "syrc" },
        { script_code('S', 'y', 'r', 'n'), "syrc" },
        { script_code('L', 'a', 't', 'f'), "latn" },
        { script_code('L', 'a', 't', 'g'), "latn" },
        { script_code('C', 'y', 'r', 's'), "cyrl" },
    };
    for (Special const& s : specials) {
        if (s.script == script) {
            push(tag(s.ot));
            return out;
        }
    }
    push(script | 0x20000000u); // the code with its first letter lowered
    return out;
}

std::uint32_t opentype_language_tag(std::string_view bcp47)
{
    auto tag = [](char const* s) { return script_code(s[0], s[1], s[2], s[3]); };
    if (bcp47.empty())
        return 0;
    // A private-use OpenType tag spelled out ("x-ot-URD" or "-x-ot-URD").
    if (std::size_t const at = bcp47.find("x-ot-"); at != std::string_view::npos) {
        std::string_view const raw = bcp47.substr(at + 5, 4);
        char s[5] = { ' ', ' ', ' ', ' ', 0 };
        for (std::size_t k = 0; k < raw.size() && raw[k] != '-'; ++k)
            s[k] = raw[k];
        return tag(s);
    }
    std::string lower;
    for (char c : bcp47)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string const primary = lower.substr(0, lower.find_first_of("-_"));
    std::string_view const rest = std::string_view(lower).substr(primary.size());
    if (primary == "zh") {
        if (rest.find("hant") != std::string_view::npos || rest.find("-tw") != std::string_view::npos)
            return tag("ZHT ");
        if (rest.find("-hk") != std::string_view::npos || rest.find("-mo") != std::string_view::npos)
            return tag("ZHH ");
        return tag("ZHS ");
    }
    // ISO 639 codes to language system tags, from the OpenType language
    // system tag registry.
    struct Entry {
        char const* iso;
        char const* ot;
    };
    static constexpr Entry entries[] = {
        { "af", "AFK " }, { "am", "AMH " }, { "ar", "ARA " }, { "as", "ASM " }, { "az", "AZE " },
        { "be", "BEL " }, { "bg", "BGR " }, { "bn", "BEN " }, { "bo", "TIB " }, { "br", "BRE " },
        { "bs", "BOS " }, { "ca", "CAT " }, { "chr", "CHR " }, { "cs", "CSY " }, { "cy", "WEL " },
        { "da", "DAN " }, { "de", "DEU " }, { "dv", "DIV " }, { "dz", "DZN " }, { "el", "ELL " },
        { "en", "ENG " }, { "eo", "NTO " }, { "es", "ESP " }, { "et", "ETI " }, { "eu", "EUQ " },
        { "fa", "FAR " }, { "ff", "FUL " }, { "fi", "FIN " }, { "fo", "FOS " }, { "fr", "FRA " },
        { "fy", "FRI " }, { "ga", "IRI " }, { "gd", "GAE " }, { "gl", "GAL " }, { "gu", "GUJ " },
        { "ha", "HAU " }, { "he", "IWR " }, { "iw", "IWR " }, { "hi", "HIN " }, { "hr", "HRV " },
        { "hu", "HUN " }, { "hy", "HYE " }, { "id", "IND " }, { "ig", "IBO " }, { "is", "ISL " },
        { "it", "ITA " }, { "iu", "INU " }, { "ja", "JAN " }, { "jv", "JAV " }, { "ka", "KAT " },
        { "kk", "KAZ " }, { "kl", "GRN " }, { "km", "KHM " }, { "kn", "KAN " }, { "ko", "KOR " },
        { "ks", "KSH " }, { "ku", "KUR " }, { "ky", "KIR " }, { "la", "LAT " }, { "lb", "LTZ " },
        { "lo", "LAO " }, { "lt", "LTH " }, { "lv", "LVI " }, { "mg", "MLG " }, { "mi", "MRI " },
        { "mk", "MKD " }, { "ml", "MAL " }, { "mn", "MNG " }, { "mo", "MOL " }, { "mr", "MAR " },
        { "ms", "MLY " }, { "mt", "MTS " }, { "my", "BRM " }, { "nb", "NOR " }, { "ne", "NEP " },
        { "nl", "NLD " }, { "nn", "NYN " }, { "no", "NOR " }, { "nqo", "NKO " }, { "or", "ORI " },
        { "pa", "PAN " }, { "pl", "PLK " }, { "ps", "PAS " }, { "pt", "PTG " }, { "ro", "ROM " },
        { "ru", "RUS " }, { "sa", "SAN " }, { "sd", "SND " }, { "si", "SNH " }, { "sk", "SKY " },
        { "sl", "SLV " }, { "so", "SML " }, { "sq", "SQI " }, { "sr", "SRB " }, { "sv", "SVE " },
        { "sw", "SWK " }, { "syr", "SYR " }, { "ta", "TAM " }, { "te", "TEL " }, { "tg", "TAJ " },
        { "th", "THA " }, { "ti", "TGY " }, { "tk", "TKM " }, { "tl", "TGL " }, { "tr", "TRK " },
        { "tt", "TAT " }, { "ug", "UYG " }, { "uk", "UKR " }, { "ur", "URD " }, { "uz", "UZB " },
        { "vi", "VIT " }, { "yi", "JII " }, { "yo", "YBA " },
        // ISO 639-2 and 639-3 codes of languages written in the scripts
        // with syllable shapers, whose fonts carry their own forms.
        { "ban", "BAN " }, { "bug", "BUG " }, { "ksw", "KSW " }, { "mni", "MNI " }, { "mnw", "MON " },
        { "new", "NEW " }, { "sat", "SAT " }, { "shn", "SHN " }, { "su", "SUN " },
    };
    for (Entry const& e : entries) {
        if (primary == e.iso)
            return tag(e.ot);
    }
    // A bare four-letter OpenType tag ("URD", "URD ") is taken as it is.
    if (bcp47.size() <= 4 && bcp47.size() >= 3 && std::all_of(bcp47.begin(), bcp47.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || c == ' ';
        })) {
        char s[5] = { ' ', ' ', ' ', ' ', 0 };
        for (std::size_t k = 0; k < bcp47.size(); ++k)
            s[k] = bcp47[k];
        return tag(s);
    }
    return 0;
}

}
