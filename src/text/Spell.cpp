#include "text/Spell.h"

#include "core/Unicode.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace sashfold::text {

namespace {

using Flag = std::uint32_t;

// A rule's condition (the .aff's regular-expression subset): a sequence of
// one character, '.', or a bracket set (negated with ^), matched at the
// start of a stem for a prefix and at its end for a suffix.
struct ConditionPart {
    bool any = false;
    bool negated = false;
    std::u32string set;
    bool matches(char32_t c) const
    {
        if (any)
            return true;
        bool const in = set.find(c) != std::u32string::npos;
        return negated ? !in : in;
    }
};

struct Rule {
    Flag flag = 0;
    bool prefix = false;
    bool cross = false;
    std::u32string strip;
    std::u32string add;
    std::vector<Flag> continuation; // flags of the affix itself (two-level suffixes, NEEDAFFIX...)
    std::vector<ConditionPart> condition;

    bool condition_holds(std::u32string_view stem) const
    {
        if (condition.size() > stem.size())
            return false;
        if (prefix) {
            for (std::size_t i = 0; i < condition.size(); ++i) {
                if (!condition[i].matches(stem[i]))
                    return false;
            }
        } else {
            std::size_t const from = stem.size() - condition.size();
            for (std::size_t i = 0; i < condition.size(); ++i) {
                if (!condition[i].matches(stem[from + i]))
                    return false;
            }
        }
        return true;
    }
    bool continues_with(Flag other) const { return std::find(continuation.begin(), continuation.end(), other) != continuation.end(); }
};

struct Entry {
    std::vector<Flag> flags; // sorted
    bool has(Flag flag) const { return flag != 0 && std::binary_search(flags.begin(), flags.end(), flag); }
};

std::u32string lower(std::u32string_view text)
{
    std::u32string out(text);
    for (char32_t& c : out)
        c = to_lowercase(c);
    return out;
}

std::u32string upper(std::u32string_view text)
{
    std::u32string out(text);
    for (char32_t& c : out)
        c = to_uppercase(c);
    return out;
}

std::u32string capitalized(std::u32string_view text)
{
    std::u32string out = lower(text);
    if (!out.empty())
        out[0] = to_titlecase(out[0]);
    return out;
}

bool is_upper(char32_t c) { return to_lowercase(c) != c; }
bool is_lower(char32_t c) { return to_uppercase(c) != c; }

enum class Casing { Lower, Capitalized, Upper, Mixed };

Casing casing_of(std::u32string_view word)
{
    std::size_t uppers = 0;
    std::size_t lowers = 0;
    for (char32_t const c : word) {
        uppers += is_upper(c) ? 1 : 0;
        lowers += is_lower(c) ? 1 : 0;
    }
    if (uppers == 0)
        return Casing::Lower;
    if (lowers == 0)
        return Casing::Upper;
    if (uppers == 1 && is_upper(word[0]))
        return Casing::Capitalized;
    return Casing::Mixed;
}

// What the .aff says the encoding is, the bytes decoded by it: UTF-8, or one
// of the ISO 8859 sets the older dictionaries use (Latin-1 bytes are their
// code points; 8859-15's few changes are mapped).
std::u32string decode(std::string_view bytes, std::string const& encoding)
{
    if (encoding.empty() || encoding == "UTF-8" || encoding == "utf-8")
        return decode_utf8(bytes);
    std::u32string out;
    out.reserve(bytes.size());
    bool const latin9 = encoding == "ISO8859-15" || encoding == "ISO-8859-15";
    for (unsigned char const c : bytes) {
        char32_t code = c;
        if (latin9) {
            switch (c) {
            case 0xA4: code = 0x20AC; break;
            case 0xA6: code = 0x160; break;
            case 0xA8: code = 0x161; break;
            case 0xB4: code = 0x17D; break;
            case 0xB8: code = 0x17E; break;
            case 0xBC: code = 0x152; break;
            case 0xBD: code = 0x153; break;
            case 0xBE: code = 0x178; break;
            default: break;
            }
        }
        out += code;
    }
    return out;
}

std::vector<std::string_view> fields_of(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
            ++at;
        std::size_t const start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t')
            ++at;
        if (at > start)
            fields.push_back(line.substr(start, at - start));
    }
    return fields;
}

}

struct SpellDictionary::Impl {
    std::string encoding = "UTF-8";
    enum class FlagType { Ascii, Long, Number, Utf8 } flag_type = FlagType::Ascii;
    std::unordered_map<std::u32string, std::vector<Entry>> words;
    std::vector<Rule> prefixes;
    std::vector<Rule> suffixes;
    std::u32string try_characters;
    std::vector<std::u32string> keyboard_rows;
    std::vector<std::pair<std::u32string, std::u32string>> replacements;
    std::u32string word_characters;
    std::u32string ignored;
    Flag no_suggest = 0;
    Flag forbidden = 0;
    Flag keep_case = 0;
    Flag need_affix = 0;
    Flag only_in_compound = 0;
    Flag compound = 0;
    Flag compound_begin = 0;
    Flag compound_middle = 0;
    Flag compound_end = 0;
    std::size_t compound_min = 3;
    std::unordered_set<std::u32string> own_words;

    std::vector<Flag> parse_flags(std::u32string_view text) const
    {
        std::vector<Flag> flags;
        switch (flag_type) {
        case FlagType::Ascii:
        case FlagType::Utf8:
            for (char32_t const c : text)
                flags.push_back(c);
            break;
        case FlagType::Long:
            for (std::size_t i = 0; i + 1 < text.size(); i += 2)
                flags.push_back(static_cast<Flag>(text[i]) << 16 | static_cast<Flag>(text[i + 1]));
            break;
        case FlagType::Number: {
            Flag value = 0;
            bool any = false;
            for (char32_t const c : text) {
                if (c >= U'0' && c <= U'9') {
                    value = value * 10 + static_cast<Flag>(c - U'0');
                    any = true;
                } else if (c == U',') {
                    if (any)
                        flags.push_back(value);
                    value = 0;
                    any = false;
                }
            }
            if (any)
                flags.push_back(value);
            break;
        }
        }
        std::sort(flags.begin(), flags.end());
        return flags;
    }
    Flag one_flag(std::u32string_view text) const
    {
        std::vector<Flag> const flags = parse_flags(text);
        return flags.empty() ? 0 : flags.front();
    }

    static std::vector<ConditionPart> parse_condition(std::u32string_view text)
    {
        std::vector<ConditionPart> parts;
        if (text == U".")
            return parts;
        for (std::size_t i = 0; i < text.size(); ++i) {
            ConditionPart part;
            if (text[i] == U'.') {
                part.any = true;
            } else if (text[i] == U'[') {
                std::size_t j = i + 1;
                if (j < text.size() && text[j] == U'^') {
                    part.negated = true;
                    ++j;
                }
                while (j < text.size() && text[j] != U']')
                    part.set += text[j++];
                i = j;
            } else {
                part.set = std::u32string(1, text[i]);
            }
            parts.push_back(std::move(part));
        }
        return parts;
    }

    void read_aff(std::string_view aff)
    {
        // The encoding first, as everything else is read in it.
        {
            std::istringstream lines { std::string(aff) };
            for (std::string line; std::getline(lines, line);) {
                std::vector<std::string_view> const fields = fields_of(line);
                if (fields.size() >= 2 && fields[0] == "SET") {
                    encoding = std::string(fields[1]);
                    while (!encoding.empty() && (encoding.back() == '\r'))
                        encoding.pop_back();
                }
                if (fields.size() >= 2 && fields[0] == "FLAG") {
                    if (fields[1].starts_with("long"))
                        flag_type = FlagType::Long;
                    else if (fields[1].starts_with("num"))
                        flag_type = FlagType::Number;
                    else if (fields[1].starts_with("UTF-8"))
                        flag_type = FlagType::Utf8;
                }
            }
        }
        std::u32string const text = decode(aff, encoding);
        std::size_t at = 0;
        // Rules are read once their header says how many follow.
        while (at < text.size()) {
            std::size_t end = text.find(U'\n', at);
            if (end == std::u32string::npos)
                end = text.size();
            std::u32string line = text.substr(at, end - at);
            at = end + 1;
            while (!line.empty() && (line.back() == U'\r' || line.back() == U' '))
                line.pop_back();
            if (line.empty() || line[0] == U'#')
                continue;
            std::vector<std::u32string> fields;
            {
                std::size_t i = 0;
                while (i < line.size()) {
                    while (i < line.size() && (line[i] == U' ' || line[i] == U'\t'))
                        ++i;
                    std::size_t const start = i;
                    while (i < line.size() && line[i] != U' ' && line[i] != U'\t')
                        ++i;
                    if (i > start)
                        fields.push_back(line.substr(start, i - start));
                }
            }
            if (fields.empty())
                continue;
            std::u32string const& key = fields[0];
            auto const value = [&](std::size_t i) -> std::u32string { return i < fields.size() ? fields[i] : std::u32string(); };
            if (key == U"TRY") {
                try_characters = value(1);
            } else if (key == U"KEY") {
                std::u32string rows = value(1);
                std::size_t start = 0;
                while (start <= rows.size()) {
                    std::size_t const bar = std::min(rows.find(U'|', start), rows.size());
                    keyboard_rows.push_back(rows.substr(start, bar - start));
                    start = bar + 1;
                }
            } else if (key == U"WORDCHARS") {
                word_characters = value(1);
            } else if (key == U"IGNORE") {
                ignored = value(1);
            } else if (key == U"NOSUGGEST") {
                no_suggest = one_flag(value(1));
            } else if (key == U"FORBIDDENWORD") {
                forbidden = one_flag(value(1));
            } else if (key == U"KEEPCASE") {
                keep_case = one_flag(value(1));
            } else if (key == U"NEEDAFFIX" || key == U"PSEUDOROOT") {
                need_affix = one_flag(value(1));
            } else if (key == U"ONLYINCOMPOUND") {
                only_in_compound = one_flag(value(1));
            } else if (key == U"COMPOUNDFLAG") {
                compound = one_flag(value(1));
            } else if (key == U"COMPOUNDBEGIN") {
                compound_begin = one_flag(value(1));
            } else if (key == U"COMPOUNDMIDDLE") {
                compound_middle = one_flag(value(1));
            } else if (key == U"COMPOUNDEND" || key == U"COMPOUNDLAST") {
                compound_end = one_flag(value(1));
            } else if (key == U"COMPOUNDMIN") {
                compound_min = std::max<std::size_t>(1, std::strtoul(to_utf8(value(1)).c_str(), nullptr, 10));
            } else if (key == U"REP" && fields.size() >= 3) {
                std::u32string to = value(2);
                std::replace(to.begin(), to.end(), U'_', U' ');
                replacements.emplace_back(value(1), to);
            } else if ((key == U"PFX" || key == U"SFX") && fields.size() >= 5) {
                // A rule line: PFX flag strip add[/flags] condition.
                Rule rule;
                rule.prefix = key == U"PFX";
                rule.flag = one_flag(value(1));
                std::u32string const strip = value(2);
                rule.strip = strip == U"0" ? std::u32string() : strip;
                std::u32string add = value(3);
                if (std::size_t const slash = add.find(U'/'); slash != std::u32string::npos) {
                    rule.continuation = parse_flags(std::u32string_view(add).substr(slash + 1));
                    add.erase(slash);
                }
                rule.add = add == U"0" ? std::u32string() : add;
                rule.condition = parse_condition(value(4));
                // Whether the class crosses with the other kind comes from
                // its header, which came first.
                auto const& headers = rule.prefix ? prefix_cross : suffix_cross;
                if (auto const found = headers.find(rule.flag); found != headers.end())
                    rule.cross = found->second;
                (rule.prefix ? prefixes : suffixes).push_back(std::move(rule));
            } else if ((key == U"PFX" || key == U"SFX") && fields.size() >= 4) {
                // A header: PFX flag cross_product count.
                (key == U"PFX" ? prefix_cross : suffix_cross)[one_flag(value(1))] = value(2) == U"Y";
            }
        }
    }
    std::unordered_map<Flag, bool> prefix_cross;
    std::unordered_map<Flag, bool> suffix_cross;

    void read_dic(std::string_view dic)
    {
        std::u32string const text = decode(dic, encoding);
        std::size_t at = 0;
        bool first = true;
        while (at < text.size()) {
            std::size_t end = text.find(U'\n', at);
            if (end == std::u32string::npos)
                end = text.size();
            std::u32string line = text.substr(at, end - at);
            at = end + 1;
            while (!line.empty() && (line.back() == U'\r' || line.back() == U' ' || line.back() == U'\t'))
                line.pop_back();
            if (first) {
                first = false; // the count of words
                continue;
            }
            if (line.empty() || line[0] == U'\t')
                continue;
            // word[/flags] [morphology]; a slash in the word is written \/.
            std::u32string word;
            std::u32string flags;
            std::size_t i = 0;
            for (; i < line.size(); ++i) {
                if (line[i] == U'\\' && i + 1 < line.size() && line[i + 1] == U'/') {
                    word += U'/';
                    ++i;
                } else if (line[i] == U'/') {
                    std::size_t j = i + 1;
                    while (j < line.size() && line[j] != U' ' && line[j] != U'\t')
                        flags += line[j++];
                    break;
                } else if (line[i] == U' ' || line[i] == U'\t') {
                    break;
                } else {
                    word += line[i];
                }
            }
            if (word.empty())
                continue;
            for (char32_t const c : ignored)
                std::erase(word, c);
            Entry entry;
            entry.flags = parse_flags(flags);
            words[word].push_back(std::move(entry));
        }
    }

    // --- Checking -------------------------------------------------------------

    enum class Found { No, Yes, Forbidden };

    // The word as a stem of the dictionary, taken with the rule flag
    // `with` when given (an affix found on it), alone when not.
    Found stem_found(std::u32string const& stem, Flag with, Flag also = 0, bool in_compound = false) const
    {
        auto const it = words.find(stem);
        if (it == words.end())
            return Found::No;
        bool forbidden_seen = false;
        for (Entry const& entry : it->second) {
            if (entry.has(forbidden)) {
                forbidden_seen = true;
                continue;
            }
            if (with != 0 && !entry.has(with))
                continue;
            if (also != 0 && !entry.has(also))
                continue;
            if (with == 0 && entry.has(need_affix))
                continue;
            if (!in_compound && entry.has(only_in_compound))
                continue;
            return Found::Yes;
        }
        return forbidden_seen && with == 0 ? Found::Forbidden : Found::No;
    }

    // A suffix rule taken off a word: the stem it leaves.
    static std::optional<std::u32string> without_suffix(Rule const& rule, std::u32string_view word)
    {
        if (word.size() < rule.add.size() || (word.size() == rule.add.size() && rule.strip.empty()))
            return std::nullopt;
        if (word.substr(word.size() - rule.add.size()) != rule.add)
            return std::nullopt;
        std::u32string stem = std::u32string(word.substr(0, word.size() - rule.add.size())) + rule.strip;
        if (!rule.condition_holds(stem))
            return std::nullopt;
        return stem;
    }

    static std::optional<std::u32string> without_prefix(Rule const& rule, std::u32string_view word)
    {
        if (word.size() < rule.add.size() || (word.size() == rule.add.size() && rule.strip.empty()))
            return std::nullopt;
        if (word.substr(0, rule.add.size()) != rule.add)
            return std::nullopt;
        std::u32string stem = rule.strip + std::u32string(word.substr(rule.add.size()));
        if (!rule.condition_holds(stem))
            return std::nullopt;
        return stem;
    }

    // A word made from a stem by suffixes (one, or two where the outer one's
    // class follows the inner one's), and the prefix flag the stem must also
    // carry when a prefix came off first.
    bool suffixed(std::u32string_view word, Flag prefix_flag, bool in_compound) const
    {
        for (Rule const& outer : suffixes) {
            if (prefix_flag != 0 && !outer.cross)
                continue;
            std::optional<std::u32string> const stem = without_suffix(outer, word);
            if (!stem)
                continue;
            if (outer.continues_with(need_affix) ? false : stem_found(*stem, outer.flag, prefix_flag, in_compound) == Found::Yes)
                return true;
            for (Rule const& inner : suffixes) {
                if (!inner.continues_with(outer.flag))
                    continue;
                std::optional<std::u32string> const root = without_suffix(inner, *stem);
                if (root && stem_found(*root, inner.flag, prefix_flag, in_compound) == Found::Yes)
                    return true;
            }
        }
        return false;
    }

    Found exact(std::u32string const& word, bool in_compound = false) const
    {
        Found const alone = stem_found(word, 0, 0, in_compound);
        if (alone != Found::No)
            return alone;
        if (suffixed(word, 0, in_compound))
            return Found::Yes;
        for (Rule const& prefix : prefixes) {
            std::optional<std::u32string> const stem = without_prefix(prefix, word);
            if (!stem)
                continue;
            if (!prefix.continues_with(need_affix) && stem_found(*stem, prefix.flag, 0, in_compound) == Found::Yes)
                return Found::Yes;
            if (prefix.cross && suffixed(*stem, prefix.flag, in_compound))
                return Found::Yes;
        }
        return Found::No;
    }

    // A compound of dictionary words (COMPOUNDFLAG or BEGIN, MIDDLE, END):
    // each part at least COMPOUNDMIN long; up to four parts.
    bool compounded(std::u32string const& word, std::size_t depth = 0) const
    {
        if ((compound == 0 && compound_begin == 0) || depth > 3)
            return false;
        auto const part_ok = [&](std::u32string const& part, Flag position) {
            for (Flag const flag : { compound, position }) {
                if (flag == 0)
                    continue;
                if (stem_found(part, flag, 0, true) == Found::Yes)
                    return true;
            }
            return false;
        };
        for (std::size_t cut = compound_min; cut + compound_min <= word.size(); ++cut) {
            std::u32string const head = word.substr(0, cut);
            if (!part_ok(head, depth == 0 ? compound_begin : compound_middle))
                continue;
            std::u32string const rest = word.substr(cut);
            if (part_ok(rest, compound_end))
                return true;
            if (compounded(rest, depth + 1))
                return true;
        }
        return false;
    }

    bool check_cased(std::u32string const& word) const
    {
        if (own_words.contains(word) || own_words.contains(lower(word)))
            return true;
        Found const found = exact(word);
        if (found == Found::Forbidden)
            return false;
        if (found == Found::Yes || compounded(word))
            return true;
        Casing const casing = casing_of(word);
        // A word capitalized at a sentence's start, or written in capitals, is
        // spelled right when its lower-case (or capitalized) form is — but not
        // a word the dictionary keeps in its own case.
        auto const kept_case = [&](std::u32string const& form) {
            auto const it = words.find(form);
            if (it == words.end())
                return false;
            return std::any_of(it->second.begin(), it->second.end(), [&](Entry const& e) { return e.has(keep_case); });
        };
        if (casing == Casing::Capitalized || casing == Casing::Upper) {
            std::u32string const low = lower(word);
            if (!kept_case(low) && (exact(low) == Found::Yes || compounded(low)))
                return true;
        }
        if (casing == Casing::Upper) {
            std::u32string const cap = capitalized(word);
            if (!kept_case(cap) && exact(cap) == Found::Yes)
                return true;
        }
        return false;
    }

    // --- Suggesting -----------------------------------------------------------

    bool suggestible(std::u32string const& word) const
    {
        if (!check_cased(word))
            return false;
        if (no_suggest != 0) {
            auto const it = words.find(word);
            if (it != words.end() && std::all_of(it->second.begin(), it->second.end(), [&](Entry const& e) { return e.has(no_suggest); }))
                return false;
        }
        return true;
    }

    // The n-gram likeness of two words (Hunspell's ngram score in spirit):
    // shared runs of one to three letters, less what their lengths differ.
    static int likeness(std::u32string_view a, std::u32string_view b)
    {
        int score = 0;
        for (std::size_t n = 1; n <= 3; ++n) {
            if (a.size() < n)
                break;
            for (std::size_t i = 0; i + n <= a.size(); ++i) {
                if (b.find(a.substr(i, n)) != std::u32string_view::npos)
                    score += static_cast<int>(n);
            }
        }
        int const difference = static_cast<int>(a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
        return score - 2 * difference;
    }
};

SpellDictionary::SpellDictionary()
    : m_impl(std::make_unique<Impl>())
{
}

SpellDictionary::~SpellDictionary() = default;

std::unique_ptr<SpellDictionary> SpellDictionary::load(std::string_view aff, std::string_view dic)
{
    std::unique_ptr<SpellDictionary> dictionary(new SpellDictionary());
    dictionary->m_impl->read_aff(aff);
    dictionary->m_impl->read_dic(dic);
    if (dictionary->m_impl->words.empty())
        return nullptr;
    return dictionary;
}

bool SpellDictionary::check(std::u32string_view given) const
{
    std::u32string word(given);
    for (char32_t const c : m_impl->ignored)
        std::erase(word, c);
    if (word.empty())
        return true;
    // Numbers, and words with a digit in them, are not the dictionary's.
    if (std::any_of(word.begin(), word.end(), [](char32_t c) { return c >= U'0' && c <= U'9'; }))
        return true;
    // A typographic apostrophe is the ASCII one to a dictionary.
    std::replace(word.begin(), word.end(), U'’', U'\'');
    if (m_impl->check_cased(word))
        return true;
    // A hyphenated word is spelled right when each of its parts is (BREAK,
    // whose default breaks at hyphens).
    if (word.find(U'-') != std::u32string::npos && word.front() != U'-' && word.back() != U'-') {
        std::size_t start = 0;
        bool all = true;
        while (start <= word.size() && all) {
            std::size_t const hyphen = std::min(word.find(U'-', start), word.size());
            std::u32string const part = word.substr(start, hyphen - start);
            all = !part.empty() && check(part);
            start = hyphen + 1;
        }
        if (all)
            return true;
    }
    // A word ending in an apostrophe or a period (an abbreviation) is
    // checked without it too.
    if (word.size() > 1 && (word.back() == U'.' || word.back() == U'\''))
        return m_impl->check_cased(word.substr(0, word.size() - 1));
    return false;
}

std::vector<std::u32string> SpellDictionary::suggest(std::u32string_view given, std::size_t limit) const
{
    Impl const& d = *m_impl;
    std::u32string word(given);
    std::replace(word.begin(), word.end(), U'’', U'\'');
    Casing const casing = casing_of(word);
    std::u32string const base = casing == Casing::Mixed ? word : lower(word);
    std::vector<std::u32string> out;
    std::set<std::u32string> seen;
    auto const offer = [&](std::u32string candidate) {
        if (out.size() >= limit || candidate.empty() || candidate == word || seen.contains(candidate))
            return;
        // Two words: each spelled right.
        bool ok = false;
        if (std::size_t const space = candidate.find(U' '); space != std::u32string::npos)
            ok = d.suggestible(candidate.substr(0, space)) && d.suggestible(candidate.substr(space + 1));
        else
            ok = d.suggestible(candidate);
        if (!ok)
            return;
        seen.insert(candidate);
        out.push_back(std::move(candidate));
    };
    // In the case it was written in.
    auto const recased = [&](std::u32string const& candidate) {
        if (casing == Casing::Upper)
            return upper(candidate);
        if (casing == Casing::Capitalized && !candidate.empty()) {
            std::u32string c = candidate;
            c[0] = to_titlecase(c[0]);
            return c;
        }
        return candidate;
    };
    auto const try_form = [&](std::u32string const& candidate) {
        std::u32string const form = recased(candidate);
        if (d.check_cased(form)) {
            offer(form);
            return;
        }
        // A name written lower-case: its capitalized form, when the lower-case
        // one is no word of its own.
        if (casing == Casing::Lower && casing_of(candidate) == Casing::Lower)
            offer(capitalized(candidate));
    };
    // The case alone (a name written lower-case, an acronym).
    if (casing == Casing::Lower) {
        offer(capitalized(word));
        offer(upper(word));
    }
    // What the language says people get wrong (REP).
    for (auto const& [from, to] : d.replacements) {
        for (std::size_t at = base.find(from); at != std::u32string::npos; at = base.find(from, at + 1))
            try_form(base.substr(0, at) + to + base.substr(at + from.size()));
    }
    // Two letters swapped.
    for (std::size_t i = 0; i + 1 < base.size(); ++i) {
        std::u32string c = base;
        std::swap(c[i], c[i + 1]);
        try_form(c);
    }
    // A key next to the one meant (KEY).
    for (std::size_t i = 0; i < base.size(); ++i) {
        for (std::u32string const& row : d.keyboard_rows) {
            std::size_t const at = row.find(base[i]);
            if (at == std::u32string::npos)
                continue;
            for (std::size_t const near : { at - 1, at + 1 }) {
                if (near >= row.size())
                    continue;
                std::u32string c = base;
                c[i] = row[near];
                try_form(c);
            }
        }
    }
    // A letter too many.
    for (std::size_t i = 0; i < base.size(); ++i)
        try_form(base.substr(0, i) + base.substr(i + 1));
    // A letter missing, or a wrong one (TRY, the language's letters by use).
    std::u32string const letters = d.try_characters.empty() ? std::u32string(U"esianrtolcdugmphbyfvkwzESIANRTOLCDUGMPHBYFVKWZ'") : d.try_characters;
    for (std::size_t i = 0; i <= base.size(); ++i) {
        for (char32_t const c : letters)
            try_form(base.substr(0, i) + c + base.substr(i));
    }
    for (std::size_t i = 0; i < base.size(); ++i) {
        for (char32_t const c : letters) {
            if (c == base[i])
                continue;
            std::u32string candidate = base;
            candidate[i] = c;
            try_form(candidate);
        }
    }
    // Two words run together, when little else came.
    if (out.size() < 3) {
        for (std::size_t i = 1; i < base.size(); ++i)
            offer(recased(base.substr(0, i) + U" " + base.substr(i)));
    }
    // Then the dictionary's nearest stems.
    if (out.size() < limit && base.size() >= 3) {
        std::vector<std::pair<int, std::u32string const*>> nearest;
        for (auto const& [stem, entries] : d.words) {
            if (stem.size() + 3 < base.size() || stem.size() > base.size() + 3)
                continue;
            int const score = Impl::likeness(base, lower(stem));
            if (score >= static_cast<int>(base.size()))
                nearest.emplace_back(score, &stem);
        }
        std::sort(nearest.begin(), nearest.end(), [](auto const& a, auto const& b) { return a.first > b.first || (a.first == b.first && *a.second < *b.second); });
        for (std::size_t i = 0; i < nearest.size() && i < 20 && out.size() < limit; ++i)
            try_form(*nearest[i].second);
    }
    return out;
}

void SpellDictionary::add_word(std::u32string const& word)
{
    m_impl->own_words.insert(word);
}

std::u32string const& SpellDictionary::word_characters() const
{
    return m_impl->word_characters;
}

std::vector<std::string> system_dictionary_folders()
{
    std::vector<std::string> folders;
    if (char const* const data = std::getenv("XDG_DATA_HOME"); data && *data)
        folders.push_back(std::string(data) + "/hunspell");
    else if (char const* const home = std::getenv("HOME"); home && *home)
        folders.push_back(std::string(home) + "/.local/share/hunspell");
    for (char const* folder : { "/usr/share/hunspell", "/usr/share/myspell", "/usr/share/myspell/dicts", "/usr/local/share/hunspell" })
        folders.push_back(folder);
    return folders;
}

std::optional<DictionaryFiles> find_dictionary(std::string_view language, std::vector<std::string> const& folders)
{
    // "en-US" as the files name it, "en_US"; the language alone after.
    std::string tag(language);
    std::replace(tag.begin(), tag.end(), '-', '_');
    std::string const primary = tag.substr(0, tag.find('_'));
    std::error_code error;
    auto const pair_at = [&](std::filesystem::path const& folder, std::string const& name) -> std::optional<DictionaryFiles> {
        std::filesystem::path const aff = folder / (name + ".aff");
        std::filesystem::path const dic = folder / (name + ".dic");
        if (std::filesystem::is_regular_file(aff, error) && std::filesystem::is_regular_file(dic, error))
            return DictionaryFiles { aff.string(), dic.string(), name };
        return std::nullopt;
    };
    for (std::string const& folder : folders) {
        if (std::optional<DictionaryFiles> found = pair_at(folder, tag))
            return found;
    }
    for (std::string const& folder : folders) {
        if (std::optional<DictionaryFiles> found = pair_at(folder, primary))
            return found;
        std::vector<std::string> names;
        for (auto const& item : std::filesystem::directory_iterator(folder, error)) {
            std::string const name = item.path().stem().string();
            if (item.path().extension() == ".dic" && (name.starts_with(primary + "_") || name.starts_with(primary + "-")))
                names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        for (std::string const& name : names) {
            if (std::optional<DictionaryFiles> found = pair_at(folder, name))
                return found;
        }
    }
    return std::nullopt;
}

}
