#pragma once

// What the text shaper knows of a character on its own: the general
// category, the script (UAX #24) with its Script_Extensions, the Arabic
// joining type (ArabicShaping.txt), the canonical combining class, the
// Indic categories, the canonical decompositions and compositions — all
// from the tables tools/gen-unicode.cpp --shaping generates — and the
// helpers a caller needs before it shapes: splitting text into runs of one
// script (UAX #24 with the paired brackets), and naming a script and a
// language the way OpenType fonts do (the OpenType script and language
// system tag registries).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sashfold::text {

// A script as its ISO 15924 code packed big-endian into 32 bits: 'Arab'
// is 0x41726162. What runs, requests and itemization speak in.
using ScriptCode = std::uint32_t;

constexpr ScriptCode script_code(char a, char b, char c, char d)
{
    return static_cast<ScriptCode>(static_cast<unsigned char>(a)) << 24
        | static_cast<ScriptCode>(static_cast<unsigned char>(b)) << 16
        | static_cast<ScriptCode>(static_cast<unsigned char>(c)) << 8 | static_cast<ScriptCode>(static_cast<unsigned char>(d));
}

namespace scripts {
inline constexpr ScriptCode common = script_code('Z', 'y', 'y', 'y');
inline constexpr ScriptCode inherited = script_code('Z', 'i', 'n', 'h');
inline constexpr ScriptCode unknown = script_code('Z', 'z', 'z', 'z');
inline constexpr ScriptCode latin = script_code('L', 'a', 't', 'n');
inline constexpr ScriptCode greek = script_code('G', 'r', 'e', 'k');
inline constexpr ScriptCode cyrillic = script_code('C', 'y', 'r', 'l');
inline constexpr ScriptCode arabic = script_code('A', 'r', 'a', 'b');
inline constexpr ScriptCode syriac = script_code('S', 'y', 'r', 'c');
inline constexpr ScriptCode nko = script_code('N', 'k', 'o', 'o');
inline constexpr ScriptCode mandaic = script_code('M', 'a', 'n', 'd');
inline constexpr ScriptCode mongolian = script_code('M', 'o', 'n', 'g');
inline constexpr ScriptCode adlam = script_code('A', 'd', 'l', 'm');
inline constexpr ScriptCode hanifi_rohingya = script_code('R', 'o', 'h', 'g');
inline constexpr ScriptCode phags_pa = script_code('P', 'h', 'a', 'g');
inline constexpr ScriptCode manichaean = script_code('M', 'a', 'n', 'i');
inline constexpr ScriptCode psalter_pahlavi = script_code('P', 'h', 'l', 'p');
inline constexpr ScriptCode sogdian = script_code('S', 'o', 'g', 'd');
inline constexpr ScriptCode old_uyghur = script_code('O', 'u', 'g', 'r');
inline constexpr ScriptCode chorasmian = script_code('C', 'h', 'r', 's');
inline constexpr ScriptCode hebrew = script_code('H', 'e', 'b', 'r');
inline constexpr ScriptCode thai = script_code('T', 'h', 'a', 'i');
inline constexpr ScriptCode lao = script_code('L', 'a', 'o', 'o');
inline constexpr ScriptCode hangul = script_code('H', 'a', 'n', 'g');
}

// General_Category, in the order the generated tables number it.
enum class GeneralCategory : std::uint8_t {
    Cc, Cf, Cn, Co, Cs, Ll, Lm, Lo, Lt, Lu, Mc, Me, Mn, Nd, Nl, No, Pc, Pd, Pe, Pf, Pi, Po, Ps, Sc, Sk, Sm, So, Zl,
    Zp, Zs,
};

constexpr bool is_mark_category(GeneralCategory category)
{
    return category == GeneralCategory::Mn || category == GeneralCategory::Mc || category == GeneralCategory::Me;
}

// The Arabic joining types (Unicode §9.2), with the Syriac joining groups
// whose letters the Syriac rules treat apart (§9.3): Alaph, and Dalath and
// Rish.
enum class JoiningType : std::uint8_t {
    NonJoining, // U
    LeftJoining, // L
    RightJoining, // R
    DualJoining, // D
    JoinCausing, // C
    Transparent, // T
    Alaph,
    DalathRish,
};

struct CharacterProperties {
    GeneralCategory category = GeneralCategory::Cn;
    std::uint8_t script = 0; // index into the generated script table; see script_of
    JoiningType joining = JoiningType::NonJoining;
    std::uint8_t combining_class = 0; // Canonical_Combining_Class
    std::uint8_t indic_syllabic = 0; // shaping_data::IndicSyllabic
    std::uint8_t indic_position = 0; // shaping_data::IndicPosition
    bool extended_pictographic = false;
};

CharacterProperties character_properties(char32_t);
GeneralCategory general_category(char32_t);
std::uint8_t combining_class(char32_t);
// The Script property (UAX #24); Zzzz for a code point nothing assigns.
ScriptCode script_of(char32_t);
// Script_Extensions: the scripts a character is used with. For one that
// has no extensions listed, its own script alone.
std::vector<ScriptCode> script_extensions(char32_t);
JoiningType joining_type(char32_t);

// One level of a canonical decomposition (Hangul syllables included,
// algorithmically: LV to L V, LVT to LV T). False when the character is
// its own decomposition; `second` is 0 for a singleton.
bool canonical_decompose(char32_t, char32_t& first, char32_t& second);
// The primary composite of two characters (Hangul included), or 0.
char32_t canonical_compose(char32_t first, char32_t second);

// The Arabic presentation form of a letter (the character whose
// compatibility decomposition is <isolated>, <final>, <initial> or
// <medial> of it alone: form 0, 1, 2, 3), or 0.
char32_t arabic_presentation_form(char32_t, std::uint8_t form);

// The scripts the text's runs are in: a run of one script each, where
// Common and Inherited characters take the script of what is around them
// — marks the script of their base, a closing bracket the script its
// opening one took (UAX #24 §5.1 with BD16's pairing), and a character
// whose Script_Extensions name the neighbor's script stays with it.
// Characters that no script claims go with what precedes them, or with
// what follows at the start of the text; a text of Common characters only
// is one Common run.
struct ScriptRun {
    std::size_t start = 0;
    std::size_t end = 0;
    ScriptCode script = scripts::common;
};
std::vector<ScriptRun> itemize_scripts(std::u32string_view text);

// The direction a script is written in horizontally: true for the
// right-to-left scripts (Arabic, Hebrew, Syriac, Thaana, N'Ko, ...).
bool script_is_right_to_left(ScriptCode);

// The OpenType script tags for a script, the one fonts should prefer first
// (an Indic script's 'dev2' before its 'deva'): up to three, 0 past the
// last. Common and Inherited name none: a shaper then uses the font's
// default script.
struct OpenTypeScriptTags {
    std::uint32_t tags[3] = { 0, 0, 0 };
    std::size_t count = 0;
};
OpenTypeScriptTags opentype_script_tags(ScriptCode);

// The OpenType language system tag for a BCP 47 language tag ("fa" is
// 'FAR ', "sr-Latn" is 'SRB '), from the language system tag registry; 0
// when the registry has none for it. A tag written as four characters
// after "x-ot-" or as a bare four-character OpenType tag in capitals is
// taken as it is.
std::uint32_t opentype_language_tag(std::string_view bcp47);

}
