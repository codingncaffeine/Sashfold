// tools/gen-unicode.cpp — the Unicode table generator (data in, our code
// out, nothing linked). Run at dev time; the emitted headers are
// committed. First consumers: UTS #46 domain mapping and NFC normalization
// for the URL parser's domain-to-ASCII. Later consumers (line-break classes,
// bidi classes, East Asian width, grapheme clusters) extend this tool
// alongside the feature that needs them.
//
// Usage:
//   g++ -std=c++23 -O2 tools/gen-unicode.cpp -o gen-unicode
//   ./gen-unicode <data-dir> <repo-root>
//
// <data-dir> holds the published Unicode data files (NOT vendored in the
// repo; download them when regenerating):
//   https://www.unicode.org/Public/idna/16.0.0/IdnaMappingTable.txt
//   https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt
//   https://www.unicode.org/Public/16.0.0/ucd/DerivedNormalizationProps.txt
//   https://www.unicode.org/Public/16.0.0/ucd/extracted/DerivedBidiClass.txt
//   https://www.unicode.org/Public/16.0.0/ucd/BidiBrackets.txt
//   https://www.unicode.org/Public/16.0.0/ucd/BidiMirroring.txt
//
// Emits:
//   src/net/IdnaData.h            UTS #46 statuses + mappings (UseSTD3ASCIIRules=false
//                                 baked in: disallowed_STD3_* collapse to their
//                                 permissive readings, deviation to valid — the
//                                 WHATWG domain-to-ASCII configuration)
//   src/core/NormalizationData.h  fully-expanded canonical decompositions,
//                                 nonzero combining classes, primary composition
//                                 pairs (Full_Composition_Exclusion applied),
//                                 combining-mark (Mn/Mc/Me) ranges, the
//                                 punctuation (Ps/Pe/Pi/Pf/Po) a ::first-letter
//                                 keeps with its letter, and the spaces and
//                                 formatting characters (Zs/Zl/Zp/Cc/Cf) it
//                                 steps over on the way to one
//   src/core/CaseData.h           the simple upper/lower/title case mappings,
//                                 packed into runs by distance and step, for
//                                 text-transform

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string const unicode_version = "16.0.0";

struct IdnaEntry {
    char32_t first = 0;
    char32_t last = 0;
    std::string status;
    std::vector<char32_t> mapping;
};

std::string trim(std::string const& text)
{
    std::size_t const begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    std::size_t const end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::vector<std::string> split(std::string const& text, char separator)
{
    std::vector<std::string> fields;
    std::stringstream stream(text);
    std::string field;
    while (std::getline(stream, field, separator))
        fields.push_back(field);
    return fields;
}

char32_t parse_hex(std::string const& text)
{
    return static_cast<char32_t>(std::stoul(text, nullptr, 16));
}

// "0041" or "0041..005A" -> [first, last]
void parse_code_point_range(std::string const& text, char32_t& first, char32_t& last)
{
    std::size_t const dots = text.find("..");
    if (dots == std::string::npos) {
        first = last = parse_hex(text);
    } else {
        first = parse_hex(text.substr(0, dots));
        last = parse_hex(text.substr(dots + 2));
    }
}

std::vector<char32_t> parse_hex_sequence(std::string const& text)
{
    std::vector<char32_t> sequence;
    for (std::string const& piece : split(text, ' '))
        if (!trim(piece).empty())
            sequence.push_back(parse_hex(trim(piece)));
    return sequence;
}

std::ifstream open_or_die(std::string const& path)
{
    std::ifstream file(path);
    if (!file) {
        std::cerr << "gen-unicode: cannot open " << path << "\n";
        std::exit(1);
    }
    return file;
}

// --- IdnaMappingTable.txt ---------------------------------------------------

std::vector<IdnaEntry> load_idna(std::string const& path)
{
    std::vector<IdnaEntry> entries;
    std::ifstream file = open_or_die(path);
    std::string line;
    while (std::getline(file, line)) {
        std::size_t const hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;
        std::vector<std::string> const fields = split(line, ';');
        IdnaEntry entry;
        parse_code_point_range(trim(fields[0]), entry.first, entry.last);
        entry.status = trim(fields[1]);
        if (fields.size() > 2)
            entry.mapping = parse_hex_sequence(fields[2]);

        // Bake in the WHATWG configuration: UseSTD3ASCIIRules=false and
        // nontransitional processing.
        if (entry.status == "disallowed_STD3_valid")
            entry.status = "valid";
        else if (entry.status == "disallowed_STD3_mapped")
            entry.status = "mapped";
        else if (entry.status == "deviation") {
            // Nontransitional: deviation characters are valid as-is; their
            // transitional mappings are never applied.
            entry.status = "valid";
            entry.mapping.clear();
        }
        entries.push_back(std::move(entry));
    }

    // Merge adjacent runs that agree on status and mapping (mapped ranges in
    // the source share one mapping across the whole range, so this is safe).
    std::vector<IdnaEntry> merged;
    for (IdnaEntry& entry : entries) {
        if (!merged.empty() && merged.back().last + 1 == entry.first
            && merged.back().status == entry.status && merged.back().mapping == entry.mapping) {
            merged.back().last = entry.last;
        } else {
            merged.push_back(std::move(entry));
        }
    }
    return merged;
}

// --- UnicodeData.txt --------------------------------------------------------

// The punctuation classes CSS 2.1 §5.12.2 names: a ::first-letter takes the
// punctuation on either side of its letter, and these are the categories that
// counts as.
bool is_first_letter_punctuation_category(std::string const& category)
{
    return category == "Ps" || category == "Pe" || category == "Pi" || category == "Pf"
        || category == "Po";
}

// The categories a ::first-letter steps over on its way to the letter, and
// never selects: the spaces of every width, the line and paragraph
// separators, the control characters and the formatting ones.
bool is_first_letter_skipped_category(std::string const& category)
{
    return category == "Zs" || category == "Zl" || category == "Zp" || category == "Cc"
        || category == "Cf";
}

struct UnicodeData {
    std::map<char32_t, std::vector<char32_t>> canonical_decomposition; // direct, not expanded
    std::vector<std::pair<char32_t, std::uint8_t>> nonzero_ccc; // per code point
    std::vector<std::pair<char32_t, char32_t>> mark_ranges; // Mn/Mc/Me, merged
    std::vector<std::pair<char32_t, char32_t>> punctuation_ranges; // Ps/Pe/Pi/Pf/Po, merged
    std::vector<std::pair<char32_t, char32_t>> skipped_ranges; // Zs/Zl/Zp/Cc/Cf, merged
    // The simple case mappings — fields 12, 13 and 14: what one code point
    // becomes on its own, with no context and no change of length. The full
    // mappings (SpecialCasing.txt: ß to SS, the Turkish dotted i) are not
    // here; text-transform's `uppercase` and `lowercase` use the simple ones.
    std::vector<std::pair<char32_t, char32_t>> simple_uppercase;
    std::vector<std::pair<char32_t, char32_t>> simple_lowercase;
    std::vector<std::pair<char32_t, char32_t>> simple_titlecase;
};

// One run of code points that all move by the same distance at the same
// step: a whole alphabet (stride 1, as ASCII and Cyrillic are) or a block of
// alternating pairs (stride 2, as Latin Extended-A is).
struct CaseRun {
    char32_t first;
    char32_t last;
    unsigned int stride;
    long long delta;
};

// Packs (code point, mapping) pairs into those runs, greedily: the second
// entry settles the stride, and the run grows while both the step and the
// distance hold.
std::vector<CaseRun> pack_case_runs(std::vector<std::pair<char32_t, char32_t>> mappings)
{
    std::sort(mappings.begin(), mappings.end());
    std::vector<CaseRun> runs;
    for (std::size_t i = 0; i < mappings.size();) {
        long long const delta
            = static_cast<long long>(mappings[i].second) - static_cast<long long>(mappings[i].first);
        std::size_t j = i + 1;
        unsigned int stride = 1;
        if (j < mappings.size()
            && static_cast<long long>(mappings[j].second) - static_cast<long long>(mappings[j].first)
                == delta) {
            unsigned int const step = mappings[j].first - mappings[i].first;
            if (step == 1 || step == 2) {
                stride = step;
                while (j < mappings.size() && mappings[j].first == mappings[j - 1].first + stride
                    && static_cast<long long>(mappings[j].second)
                            - static_cast<long long>(mappings[j].first)
                        == delta)
                    ++j;
            } else {
                j = i + 1;
            }
        } else {
            j = i + 1;
        }
        runs.push_back(CaseRun { mappings[i].first, mappings[j - 1].first, stride, delta });
        i = j;
    }
    return runs;
}

// Sorts and merges [first, last] pairs that touch or overlap.
std::vector<std::pair<char32_t, char32_t>> merged(std::vector<std::pair<char32_t, char32_t>> ranges)
{
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<char32_t, char32_t>> out;
    for (auto const& [first, last] : ranges) {
        if (!out.empty() && out.back().second + 1 >= first)
            out.back().second = std::max(out.back().second, last);
        else
            out.push_back({ first, last });
    }
    return out;
}

UnicodeData load_unicode_data(std::string const& path)
{
    UnicodeData data;
    std::ifstream file = open_or_die(path);
    std::string line;
    std::vector<std::pair<char32_t, char32_t>> marks;
    std::vector<std::pair<char32_t, char32_t>> punctuation;
    std::vector<std::pair<char32_t, char32_t>> skipped;
    char32_t range_first = 0;
    bool in_range = false;
    std::string range_category;
    while (std::getline(file, line)) {
        if (line.empty())
            continue;
        std::vector<std::string> const fields = split(line, ';');
        char32_t const code_point = parse_hex(fields[0]);
        std::string const& name = fields[1];
        std::string const& category = fields[2];
        int const ccc = std::stoi(fields[3]);

        // First/Last block pairs (Hangul, CJK, planes of ideographs): no
        // decompositions or nonzero ccc inside, but categories apply.
        if (name.ends_with(", First>")) {
            in_range = true;
            range_first = code_point;
            range_category = category;
            continue;
        }
        if (name.ends_with(", Last>")) {
            in_range = false;
            if (range_category == "Mn" || range_category == "Mc" || range_category == "Me")
                marks.push_back({ range_first, code_point });
            if (is_first_letter_punctuation_category(range_category))
                punctuation.push_back({ range_first, code_point });
            if (is_first_letter_skipped_category(range_category))
                skipped.push_back({ range_first, code_point });
            continue;
        }
        (void)in_range;

        if (category == "Mn" || category == "Mc" || category == "Me")
            marks.push_back({ code_point, code_point });
        if (is_first_letter_punctuation_category(category))
            punctuation.push_back({ code_point, code_point });
        if (is_first_letter_skipped_category(category))
            skipped.push_back({ code_point, code_point });
        if (ccc != 0)
            data.nonzero_ccc.push_back({ code_point, static_cast<std::uint8_t>(ccc) });

        // Field 5: decomposition. Canonical ones carry no <tag>.
        std::string const decomposition = fields.size() > 5 ? trim(fields[5]) : "";
        if (!decomposition.empty() && decomposition[0] != '<')
            data.canonical_decomposition[code_point] = parse_hex_sequence(decomposition);

        // Fields 12, 13, 14: the simple upper, lower and title mappings. A
        // mapping to the code point itself says nothing and is left out.
        auto const mapping = [&](std::size_t field, std::vector<std::pair<char32_t, char32_t>>& into) {
            if (fields.size() <= field)
                return;
            std::string const text = trim(fields[field]);
            if (text.empty())
                return;
            char32_t const target = parse_hex(text);
            if (target != code_point)
                into.push_back({ code_point, target });
        };
        mapping(12, data.simple_uppercase);
        mapping(13, data.simple_lowercase);
        mapping(14, data.simple_titlecase);
    }

    data.mark_ranges = merged(std::move(marks));
    data.punctuation_ranges = merged(std::move(punctuation));
    data.skipped_ranges = merged(std::move(skipped));
    return data;
}

std::set<char32_t> load_full_composition_exclusions(std::string const& path)
{
    std::set<char32_t> excluded;
    std::ifstream file = open_or_die(path);
    std::string line;
    while (std::getline(file, line)) {
        std::size_t const hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;
        std::vector<std::string> const fields = split(line, ';');
        if (fields.size() < 2 || trim(fields[1]) != "Full_Composition_Exclusion")
            continue;
        char32_t first = 0;
        char32_t last = 0;
        parse_code_point_range(trim(fields[0]), first, last);
        for (char32_t c = first; c <= last; ++c)
            excluded.insert(c);
    }
    return excluded;
}

// Expands a decomposition to its fully-decomposed form (UAX #15 does this
// recursively at runtime; we do it once here so the runtime is one lookup).
// Hangul stays algorithmic in the runtime and never appears in these tables.
std::vector<char32_t> fully_decompose(
    std::map<char32_t, std::vector<char32_t>> const& direct, std::vector<char32_t> const& sequence)
{
    std::vector<char32_t> result;
    for (char32_t const code_point : sequence) {
        auto const it = direct.find(code_point);
        if (it == direct.end()) {
            result.push_back(code_point);
        } else {
            std::vector<char32_t> const expanded = fully_decompose(direct, it->second);
            result.insert(result.end(), expanded.begin(), expanded.end());
        }
    }
    return result;
}

// --- Emission ---------------------------------------------------------------

std::string hex(char32_t value)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%X", static_cast<unsigned>(value));
    return buffer;
}

std::string generated_banner(std::string const& what)
{
    return "// " + what + "\n"
        + "// Generated by tools/gen-unicode.cpp from the Unicode " + unicode_version
        + " data files.\n// Do not edit by hand; regenerate per the tool's header comment.\n"
        + "#pragma once\n\n#include <cstdint>\n\n";
}

void emit_idna(std::string const& path, std::vector<IdnaEntry> const& entries)
{
    std::vector<char32_t> pool;
    std::ofstream out(path);
    out << generated_banner(
        "UTS #46 IDNA mapping table, WHATWG configuration (UseSTD3ASCIIRules=false,\n"
        "// nontransitional): deviation reads as valid, disallowed_STD3_* as their\n"
        "// permissive readings. Ranges are sorted for binary search.");
    out << "namespace sashfold::net {\n\n";
    out << "enum class IdnaStatus : std::uint8_t { Valid, Ignored, Mapped, Disallowed };\n\n";
    out << "struct IdnaRange {\n    char32_t first;\n    char32_t last;\n    IdnaStatus status;\n"
        << "    std::uint32_t mapping_offset;\n    std::uint16_t mapping_length;\n};\n\n";
    out << "inline constexpr IdnaRange idna_ranges[] = {\n";
    for (IdnaEntry const& entry : entries) {
        std::string status = "Disallowed";
        if (entry.status == "valid")
            status = "Valid";
        else if (entry.status == "ignored")
            status = "Ignored";
        else if (entry.status == "mapped")
            status = "Mapped";
        else if (entry.status != "disallowed") {
            std::cerr << "gen-unicode: unhandled IDNA status '" << entry.status << "'\n";
            std::exit(1);
        }
        std::size_t offset = 0;
        if (!entry.mapping.empty()) {
            offset = pool.size();
            pool.insert(pool.end(), entry.mapping.begin(), entry.mapping.end());
        }
        out << "    { " << hex(entry.first) << ", " << hex(entry.last) << ", IdnaStatus::"
            << status << ", " << offset << ", " << entry.mapping.size() << " },\n";
    }
    out << "};\n\ninline constexpr char32_t idna_mapping_pool[] = {";
    for (std::size_t i = 0; i < pool.size(); ++i)
        out << (i % 8 == 0 ? "\n    " : " ") << hex(pool[i]) << ",";
    out << "\n};\n\n}\n";
    std::cout << "wrote " << path << " (" << entries.size() << " ranges, pool " << pool.size()
              << ")\n";
}

void emit_normalization(std::string const& path, UnicodeData const& data,
    std::set<char32_t> const& excluded)
{
    std::ofstream out(path);
    out << generated_banner(
        "Canonical normalization data for NFC: fully-expanded canonical\n"
        "// decompositions, nonzero canonical combining classes, primary composition\n"
        "// pairs (Full_Composition_Exclusion applied), and combining-mark (Mn/Mc/Me)\n"
        "// ranges. Hangul is algorithmic in the runtime and absent here. The two\n"
        "// tables at the end serve ::first-letter: the punctuation (Ps/Pe/Pi/Pf/Po)\n"
        "// it keeps on either side of its letter, and the spaces and formatting\n"
        "// characters (Zs/Zl/Zp/Cc/Cf) it steps over on the way to one.");
    out << "namespace sashfold {\n\n";

    // Fully-expanded decompositions.
    std::vector<char32_t> pool;
    out << "struct DecompositionEntry {\n    char32_t code_point;\n    std::uint32_t offset;\n"
        << "    std::uint8_t length;\n};\n\n";
    out << "inline constexpr DecompositionEntry canonical_decompositions[] = {\n";
    for (auto const& [code_point, direct] : data.canonical_decomposition) {
        std::vector<char32_t> const expanded
            = fully_decompose(data.canonical_decomposition, direct);
        out << "    { " << hex(code_point) << ", " << pool.size() << ", " << expanded.size()
            << " },\n";
        pool.insert(pool.end(), expanded.begin(), expanded.end());
    }
    out << "};\n\ninline constexpr char32_t decomposition_pool[] = {";
    for (std::size_t i = 0; i < pool.size(); ++i)
        out << (i % 8 == 0 ? "\n    " : " ") << hex(pool[i]) << ",";
    out << "\n};\n\n";

    // Nonzero combining classes, merged into equal-value runs.
    out << "struct CombiningClassRange {\n    char32_t first;\n    char32_t last;\n"
        << "    std::uint8_t combining_class;\n};\n\n";
    out << "inline constexpr CombiningClassRange combining_class_ranges[] = {\n";
    std::size_t ccc_count = 0;
    for (std::size_t i = 0; i < data.nonzero_ccc.size();) {
        std::size_t j = i;
        while (j + 1 < data.nonzero_ccc.size()
            && data.nonzero_ccc[j + 1].first == data.nonzero_ccc[j].first + 1
            && data.nonzero_ccc[j + 1].second == data.nonzero_ccc[i].second)
            ++j;
        out << "    { " << hex(data.nonzero_ccc[i].first) << ", " << hex(data.nonzero_ccc[j].first)
            << ", " << static_cast<int>(data.nonzero_ccc[i].second) << " },\n";
        ++ccc_count;
        i = j + 1;
    }
    out << "};\n\n";

    // Primary composition pairs come from the DIRECT two-element canonical
    // decompositions (not the expanded ones), minus the excluded set.
    struct Pair {
        char32_t starter;
        char32_t combining;
        char32_t composed;
    };
    std::vector<Pair> pairs;
    for (auto const& [code_point, direct] : data.canonical_decomposition) {
        if (direct.size() != 2 || excluded.contains(code_point))
            continue;
        pairs.push_back({ direct[0], direct[1], code_point });
    }
    std::sort(pairs.begin(), pairs.end(), [](Pair const& a, Pair const& b) {
        return a.starter != b.starter ? a.starter < b.starter : a.combining < b.combining;
    });
    out << "struct CompositionPair {\n    char32_t starter;\n    char32_t combining;\n"
        << "    char32_t composed;\n};\n\n";
    out << "inline constexpr CompositionPair composition_pairs[] = {\n";
    for (Pair const& pair : pairs)
        out << "    { " << hex(pair.starter) << ", " << hex(pair.combining) << ", "
            << hex(pair.composed) << " },\n";
    out << "};\n\n";

    // Combining marks (general categories Mn, Mc, Me).
    out << "struct CombiningMarkRange {\n    char32_t first;\n    char32_t last;\n};\n\n";
    out << "inline constexpr CombiningMarkRange combining_mark_ranges[] = {\n";
    for (auto const& [first, last] : data.mark_ranges)
        out << "    { " << hex(first) << ", " << hex(last) << " },\n";
    out << "};\n\n";

    // The punctuation a ::first-letter takes with its letter (Ps, Pe, Pi, Pf, Po).
    out << "struct CodePointRange {\n    char32_t first;\n    char32_t last;\n};\n\n";
    out << "inline constexpr CodePointRange first_letter_punctuation_ranges[] = {\n";
    for (auto const& [first, last] : data.punctuation_ranges)
        out << "    { " << hex(first) << ", " << hex(last) << " },\n";
    out << "};\n\n";

    // What it steps over on the way to the letter (Zs, Zl, Zp, Cc, Cf).
    out << "inline constexpr CodePointRange first_letter_skipped_ranges[] = {\n";
    for (auto const& [first, last] : data.skipped_ranges)
        out << "    { " << hex(first) << ", " << hex(last) << " },\n";
    out << "};\n\n}\n";

    std::cout << "wrote " << path << " (" << data.canonical_decomposition.size()
              << " decompositions, " << ccc_count << " ccc ranges, " << pairs.size()
              << " composition pairs, " << data.mark_ranges.size() << " mark ranges, "
              << data.punctuation_ranges.size() << " punctuation ranges, "
              << data.skipped_ranges.size() << " skipped ranges)\n";
}

// --- DerivedBidiClass.txt ---------------------------------------------------

// Every Bidi_Class of UAX #9, in the order the engine's own enum lists
// them; the generator writes the names out, so the two stay in step by
// construction rather than by a matching pair of numbers.
constexpr char const* bidi_class_names[] = {
    "L", "R", "AL", // strong
    "EN", "ES", "ET", "AN", "CS", "NSM", "BN", // weak
    "B", "S", "WS", "ON", // neutral
    "LRE", "RLE", "LRO", "RLO", "PDF", // the explicit embeddings and overrides
    "LRI", "RLI", "FSI", "PDI", // and the isolates
};

// The long names the @missing lines use, beside the short ones the entries
// use. Only the classes that appear as a block default need the long form,
// but all of them are here so a data file that writes any of them out is
// read the same way.
std::size_t bidi_class_index(std::string const& name)
{
    static std::map<std::string, std::string> const long_names = {
        { "Left_To_Right", "L" }, { "Right_To_Left", "R" }, { "Arabic_Letter", "AL" },
        { "European_Number", "EN" }, { "European_Separator", "ES" },
        { "European_Terminator", "ET" }, { "Arabic_Number", "AN" },
        { "Common_Separator", "CS" }, { "Nonspacing_Mark", "NSM" }, { "Boundary_Neutral", "BN" },
        { "Paragraph_Separator", "B" }, { "Segment_Separator", "S" }, { "White_Space", "WS" },
        { "Other_Neutral", "ON" }, { "Left_To_Right_Embedding", "LRE" },
        { "Right_To_Left_Embedding", "RLE" }, { "Left_To_Right_Override", "LRO" },
        { "Right_To_Left_Override", "RLO" }, { "Pop_Directional_Format", "PDF" },
        { "Left_To_Right_Isolate", "LRI" }, { "Right_To_Left_Isolate", "RLI" },
        { "First_Strong_Isolate", "FSI" }, { "Pop_Directional_Isolate", "PDI" },
    };
    auto const spelled = long_names.find(name);
    std::string const& sought = spelled == long_names.end() ? name : spelled->second;
    for (std::size_t i = 0; i < std::size(bidi_class_names); ++i) {
        if (sought == bidi_class_names[i])
            return i;
    }
    std::cerr << "gen-unicode: unknown Bidi_Class '" << name << "'\n";
    std::exit(1);
}

// The Bidi_Class of every code point, as the derived file gives it: the
// @missing lines are the defaults (L everywhere, then R or AL through the
// blocks reserved for right-to-left scripts, so an UNASSIGNED code point in
// the Hebrew block is R and not L), with the listed entries on top of them.
// Reading the derived file rather than UnicodeData's field 4 is what makes
// those block defaults come out right.
std::vector<std::uint8_t> load_bidi_classes(std::string const& path)
{
    constexpr std::size_t code_points = 0x110000;
    // @missing: 0000..10FFFF; Left_To_Right
    std::vector<std::uint8_t> classes(code_points, 0);
    std::ifstream file = open_or_die(path);
    std::string line;
    // Two passes: the block defaults first, then the entries that override
    // them. The file happens to write a block's @missing line before that
    // block's own entries, but nothing in the format promises it.
    std::vector<std::pair<std::string, std::string>> missing;
    std::vector<std::pair<std::string, std::string>> listed;
    while (std::getline(file, line)) {
        std::size_t const at = line.find("@missing:");
        if (at != std::string::npos) {
            std::vector<std::string> const fields = split(trim(line.substr(at + 9)), ';');
            if (fields.size() >= 2)
                missing.push_back({ trim(fields[0]), trim(fields[1]) });
            continue;
        }
        std::size_t const hash = line.find('#');
        std::string const body = trim(hash == std::string::npos ? line : line.substr(0, hash));
        if (body.empty())
            continue;
        std::vector<std::string> const fields = split(body, ';');
        if (fields.size() >= 2)
            listed.push_back({ trim(fields[0]), trim(fields[1]) });
    }
    auto const apply = [&](std::vector<std::pair<std::string, std::string>> const& entries) {
        for (auto const& [range, name] : entries) {
            char32_t first = 0;
            char32_t last = 0;
            parse_code_point_range(range, first, last);
            if (last >= code_points)
                continue;
            auto const value = static_cast<std::uint8_t>(bidi_class_index(name));
            for (char32_t c = first; c <= last; ++c)
                classes[c] = value;
        }
    };
    apply(missing);
    apply(listed);
    return classes;
}

// BidiBrackets.txt: the paired brackets rule N0 matches on (BD16). One
// entry per bracket, giving the code point it pairs with and whether it
// opens or closes.
struct BracketPair {
    char32_t code_point = 0;
    char32_t paired = 0;
    bool opening = false;
};

// BidiMirroring.txt: the glyph rule L4 draws instead when a character
// resolved right-to-left — a left parenthesis paints as a right one.
std::vector<std::pair<char32_t, char32_t>> load_bidi_mirroring(std::string const& path)
{
    std::vector<std::pair<char32_t, char32_t>> pairs;
    std::ifstream file = open_or_die(path);
    std::string line;
    while (std::getline(file, line)) {
        std::size_t const hash = line.find('#');
        std::string const body = trim(hash == std::string::npos ? line : line.substr(0, hash));
        if (body.empty())
            continue;
        std::vector<std::string> const fields = split(body, ';');
        if (fields.size() < 2)
            continue;
        pairs.push_back({ parse_hex(trim(fields[0])), parse_hex(trim(fields[1])) });
    }
    std::sort(pairs.begin(), pairs.end());
    return pairs;
}

std::vector<BracketPair> load_bidi_brackets(std::string const& path)
{
    std::vector<BracketPair> brackets;
    std::ifstream file = open_or_die(path);
    std::string line;
    while (std::getline(file, line)) {
        std::size_t const hash = line.find('#');
        std::string const body = trim(hash == std::string::npos ? line : line.substr(0, hash));
        if (body.empty())
            continue;
        std::vector<std::string> const fields = split(body, ';');
        if (fields.size() < 3)
            continue;
        BracketPair entry;
        entry.code_point = parse_hex(trim(fields[0]));
        entry.paired = parse_hex(trim(fields[1]));
        entry.opening = trim(fields[2]) == "o";
        brackets.push_back(entry);
    }
    std::sort(brackets.begin(), brackets.end(),
        [](BracketPair const& a, BracketPair const& b) { return a.code_point < b.code_point; });
    return brackets;
}

void emit_bidi(std::string const& path, std::vector<std::uint8_t> const& classes,
    std::vector<std::pair<char32_t, char32_t>> const& mirrors,
    std::vector<BracketPair> const& brackets)
{
    // One sorted, disjoint table over the whole code space: a run of code
    // points sharing a class becomes one entry, and L — the default almost
    // everywhere — is left out, so the table holds only what differs from it.
    struct Run {
        char32_t first;
        char32_t last;
        std::uint8_t klass;
    };
    std::vector<Run> runs;
    for (char32_t c = 0; c < classes.size(); ++c) {
        if (classes[c] == 0) // L, the default
            continue;
        if (!runs.empty() && runs.back().last + 1 == c && runs.back().klass == classes[c])
            runs.back().last = c;
        else
            runs.push_back({ c, c, classes[c] });
    }

    std::ofstream out(path);
    out << generated_banner(
        "The Bidi_Class of every code point (UAX #9), read from\n"
        "// DerivedBidiClass.txt so that the block defaults come with it: an\n"
        "// unassigned code point in a block reserved for a right-to-left script\n"
        "// is R or AL, not the L everything else falls back to, and the @missing\n"
        "// lines are the only place that is written. L is the default and is left\n"
        "// out of the table, so what is here is every run that differs from it;\n"
        "// the runs are sorted and disjoint, for binary search.");
    out << "namespace sashfold {\n\n";
    out << "enum class BidiClass : std::uint8_t {\n";
    for (char const* const name : bidi_class_names)
        out << "    " << name << ",\n";
    out << "};\n\n";
    out << "struct BidiRange {\n    char32_t first;\n    char32_t last;\n    BidiClass klass;\n};\n\n";
    out << "inline constexpr BidiRange bidi_class_ranges[] = {\n";
    for (Run const& run : runs) {
        out << "    { " << hex(run.first) << ", " << hex(run.last) << ", BidiClass::"
            << bidi_class_names[run.klass] << " },\n";
    }
    out << "};\n\n";
    // The paired brackets rule N0 works on (BD16). Sorted by code point, so
    // the same binary search finds them.
    out << "struct BidiBracket {\n    char32_t code_point;\n    char32_t paired;\n"
        << "    bool opening;\n};\n\n";
    out << "inline constexpr BidiBracket bidi_brackets[] = {\n";
    for (BracketPair const& entry : brackets) {
        out << "    { " << hex(entry.code_point) << ", " << hex(entry.paired) << ", "
            << (entry.opening ? "true" : "false") << " },\n";
    }
    out << "};\n\n";
    // Rule L4's mirrored glyphs, sorted by the character they replace.
    out << "struct BidiMirror {\n    char32_t code_point;\n    char32_t mirrored;\n};\n\n";
    out << "inline constexpr BidiMirror bidi_mirrors[] = {\n";
    for (auto const& [code_point, mirrored] : mirrors)
        out << "    { " << hex(code_point) << ", " << hex(mirrored) << " },\n";
    out << "};\n\n";
    out << "}\n";
    std::cout << "wrote " << path << " (" << runs.size() << " ranges that are not the L default, "
              << brackets.size() << " brackets, " << mirrors.size() << " mirrored glyphs)\n";
}

// The simple case mappings, packed into runs.
void emit_case(std::string const& path, UnicodeData const& data)
{
    std::ofstream out(path);
    out << generated_banner(
        "The simple case mappings — UnicodeData.txt fields 12, 13 and 14 —\n"
        "// packed into runs of code points that move by one distance at one step:\n"
        "// a whole alphabet has stride 1, a block of alternating upper/lower pairs\n"
        "// has stride 2. These are what text-transform's `uppercase`, `lowercase`\n"
        "// and `capitalize` are written in terms of. The FULL mappings (ß to SS,\n"
        "// the Turkish dotted i, final sigma) are context- or length-changing and\n"
        "// are not here.");
    out << "namespace sashfold {\n\n";
    out << "struct CaseRun {\n    char32_t first;\n    char32_t last;\n"
        << "    std::uint32_t stride;\n    std::int32_t delta;\n};\n\n";
    auto const table = [&](char const* name, std::vector<std::pair<char32_t, char32_t>> const& pairs) {
        std::vector<CaseRun> const runs = pack_case_runs(pairs);
        out << "inline constexpr CaseRun " << name << "[] = {\n";
        for (CaseRun const& run : runs) {
            out << "    { " << hex(run.first) << ", " << hex(run.last) << ", " << run.stride << ", "
                << run.delta << " },\n";
        }
        out << "};\n\n";
        return runs.size();
    };
    std::size_t const upper = table("simple_uppercase_runs", data.simple_uppercase);
    std::size_t const lower = table("simple_lowercase_runs", data.simple_lowercase);
    std::size_t const title = table("simple_titlecase_runs", data.simple_titlecase);
    out << "}\n";
    std::cout << "wrote " << path << " (" << data.simple_uppercase.size() << " uppercase mappings in "
              << upper << " runs, " << data.simple_lowercase.size() << " lowercase in " << lower
              << ", " << data.simple_titlecase.size() << " titlecase in " << title << ")\n";
}

}

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: gen-unicode <data-dir> <repo-root>\n";
        return 2;
    }
    std::string const data_dir = argv[1];
    std::string const repo = argv[2];

    std::vector<IdnaEntry> const idna = load_idna(data_dir + "/IdnaMappingTable.txt");
    UnicodeData const unicode_data = load_unicode_data(data_dir + "/UnicodeData.txt");
    std::set<char32_t> const excluded
        = load_full_composition_exclusions(data_dir + "/DerivedNormalizationProps.txt");

    std::vector<std::uint8_t> const bidi = load_bidi_classes(data_dir + "/DerivedBidiClass.txt");
    std::vector<BracketPair> const brackets = load_bidi_brackets(data_dir + "/BidiBrackets.txt");
    std::vector<std::pair<char32_t, char32_t>> const mirrors
        = load_bidi_mirroring(data_dir + "/BidiMirroring.txt");

    emit_idna(repo + "/src/net/IdnaData.h", idna);
    emit_normalization(repo + "/src/core/NormalizationData.h", unicode_data, excluded);
    emit_case(repo + "/src/core/CaseData.h", unicode_data);
    emit_bidi(repo + "/src/core/BidiData.h", bidi, mirrors, brackets);
    return 0;
}
