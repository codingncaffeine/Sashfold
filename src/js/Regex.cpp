#include "js/Regex.h"

#include "core/CaseData.h"
#include "core/Unicode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Regular expressions (§22.2). A pattern is parsed into a small tree by
// the §22.2.1 grammar (with Annex B.1.2's relaxations outside `u` mode),
// compiled to a flat instruction list, and run by a backtracking machine
// whose backtrack stack is explicit: choice points interleaved with the
// undo records that put captures and loop counters back when a choice
// point is resumed. The machine never recurses, so a hostile pattern can
// only spend its step budget, never the process's stack.

namespace sashfold::js {

// The compiled form. Nested types keep external linkage without leaking
// names into the namespace the other modules share.
struct RegexProgram {
    struct Range {
        char32_t first = 0;
        char32_t last = 0;
    };

    // A class after canonicalization: sorted, disjoint, non-adjacent ranges.
    struct CharClass {
        std::vector<Range> ranges;
        bool negated = false;

        bool contains(char32_t c) const
        {
            auto const it = std::upper_bound(ranges.begin(), ranges.end(), c,
                [](char32_t value, Range const& range) { return value < range.first; });
            if (it == ranges.begin())
                return false;
            return c <= std::prev(it)->last;
        }
    };

    enum class Op : std::uint8_t {
        Char, // a: the (canonicalized) character
        Any, // . outside dotAll: anything but a LineTerminator
        AnyAll, // . under dotAll, and [^]
        Class, // a: class index
        Backref, // a: group number
        BackrefSet, // a: index into backref_sets — same-named groups, at most one of which participated
        LineStart,
        LineEnd,
        WordBoundary,
        NotWordBoundary,
        Save, // a: capture slot
        Split, // try a first, then b
        Jump, // a
        RepeatInit, // a: loop; count = 0
        RepeatDecide, // a: loop; exit, enter, or push the alternative
        RepeatEnter, // a: loop; record the start, reset the body's captures
        RepeatExit, // a: loop; the empty-iteration check, count + 1, back to the top
        SimpleGreedy, // a: loop; the single-character body follows at pc + 1, the exit is pc + 2
        SimpleLazy, // likewise
        LookStart, // a: continuation pc, b: 1 when negative
        LookEnd,
        Match,
    };

    struct Instruction {
        Op op = Op::Match;
        std::uint32_t a = 0;
        std::uint32_t b = 0;
    };

    struct Loop {
        std::uint32_t min = 0;
        std::uint32_t max = 0; // infinite = no bound
        bool greedy = true;
        std::uint32_t first_slot = 0; // capture slots the body owns, reset per iteration
        std::uint32_t last_slot = 0; // (first > last when the body has no groups)
        std::uint32_t top_pc = 0;
        std::uint32_t body_pc = 0;
        std::uint32_t exit_pc = 0;
        std::uint32_t count_register = 0;
        std::uint32_t start_register = 0;
    };

    RegexFlags flags;
    std::vector<Instruction> code;
    std::vector<CharClass> classes;
    std::vector<Loop> loops;
    std::vector<std::vector<std::uint32_t>> backref_sets;
    std::size_t group_count = 0;
    std::vector<std::pair<std::u16string, std::size_t>> group_names;
    std::uint32_t register_count = 2; // capture slots first, then two per loop
};

namespace {

using Range = RegexProgram::Range;
using CharClass = RegexProgram::CharClass;
using Op = RegexProgram::Op;
using Instruction = RegexProgram::Instruction;
using Loop = RegexProgram::Loop;

constexpr std::uint32_t unset = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t infinite = unset;
// A quantifier bound past this is refused outright rather than believed.
constexpr std::uint32_t max_quantifier_bound = 65535;
// Group nesting the parser will follow before calling the pattern hostile.
constexpr int max_nesting_depth = 100;
// The backtrack stack's ceiling in entries (12 bytes each): past it a match
// gives up the way an exhausted step budget does.
constexpr std::size_t max_stack_entries = 8u * 1024u * 1024u;
// Entries pack their kind into the top bits of a 32-bit word; the low bits
// hold a pc or a register, so a program must fit under this many of each.
constexpr std::uint32_t entry_kind_shift = 29;
constexpr std::uint32_t entry_low_mask = (1u << entry_kind_shift) - 1u;

std::size_t g_step_budget = 20'000'000;

bool is_high_surrogate(char32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool is_low_surrogate(char32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
char32_t combine_surrogates(char32_t high, char32_t low)
{
    return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
}

// LineTerminator (§12.3): LF, CR, LS, PS.
bool is_line_terminator(char32_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; }
bool is_decimal_digit(char32_t c) { return c >= '0' && c <= '9'; }
bool is_octal_digit(char32_t c) { return c >= '0' && c <= '7'; }
bool is_ascii_letter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_ascii_word(char32_t c) { return is_ascii_letter(c) || is_decimal_digit(c) || c == '_'; }
bool is_hex_digit(char32_t c)
{
    return is_decimal_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
std::uint32_t hex_value(char32_t c)
{
    if (is_decimal_digit(c))
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    return 10 + (c - 'A');
}
// SyntaxCharacter (§22.2.1): ^ $ \ . * + ? ( ) [ ] { } |
bool is_syntax_character(char32_t c)
{
    switch (c) {
    case '^':
    case '$':
    case '\\':
    case '.':
    case '*':
    case '+':
    case '?':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '|':
        return true;
    default:
        return false;
    }
}

// Canonicalize (§22.2.2.7.3) without `u`: the character's uppercase
// mapping, except that a non-ASCII character whose uppercase is ASCII keeps
// itself (so ſ and K never fold into s and k), and a character whose FULL
// uppercase is more than one code unit stays as it is. The core tables are
// the simple mappings, which already leave ß, ŉ, ǰ and the ligatures
// alone; the Greek letters with ypogegrammeni are the ones with a simple
// mapping and a two-unit full one, so they are pinned by hand here.
char32_t canonicalize_non_unicode(char32_t ch)
{
    if ((ch >= 0x1F80 && ch <= 0x1F87) || (ch >= 0x1F90 && ch <= 0x1F97) || (ch >= 0x1FA0 && ch <= 0x1FA7)
        || ch == 0x1FB3 || ch == 0x1FC3 || ch == 0x1FF3)
        return ch;
    char32_t const upper = to_uppercase(ch);
    if (upper > 0xFFFF)
        return ch;
    if (ch >= 128 && upper < 128)
        return ch;
    return upper;
}

// Canonicalize under `u`: simple case folding (CaseFolding.txt statuses C
// and S). The tables carry the simple upper and lower mappings, and the
// fold is lower(upper(ch)) for every cased letter but a handful:
// U+0130 and U+0131 have no C or S folding and stay themselves, and three
// letters with no simple case mapping at all were given a status-S folding
// because their full foldings coincide with another letter's.
char32_t canonicalize_unicode(char32_t ch)
{
    switch (ch) {
    case 0x130:
    case 0x131:
        return ch;
    case 0x1FD3: // GREEK SMALL LETTER IOTA WITH DIALYTIKA AND OXIA
        return 0x390;
    case 0x1FE3: // GREEK SMALL LETTER UPSILON WITH DIALYTIKA AND OXIA
        return 0x3B0;
    case 0xFB05: // LATIN SMALL LIGATURE LONG S T
        return 0xFB06;
    default:
        return to_lowercase(to_uppercase(ch));
    }
}

struct CaseFoldEntry {
    char32_t from = 0;
    char32_t non_unicode = 0;
    char32_t unicode = 0;
};

// Every code point that Canonicalize moves, in either mode, with where it
// goes. Only a code point with a simple mapping can move — those are the
// members of the case runs, plus the three status-S foldings pinned in
// canonicalize_unicode — so the table is built from the runs rather than
// by scanning all of Unicode.
std::vector<CaseFoldEntry> const& case_fold_table()
{
    static std::vector<CaseFoldEntry> const table = [] {
        std::vector<char32_t> candidates { 0x1FD3, 0x1FE3, 0xFB05 };
        auto const gather = [&](auto const& runs) {
            for (CaseRun const& run : runs)
                for (char32_t c = run.first; c <= run.last; c += run.stride)
                    candidates.push_back(c);
        };
        gather(simple_uppercase_runs);
        gather(simple_lowercase_runs);
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        std::vector<CaseFoldEntry> entries;
        for (char32_t const c : candidates) {
            CaseFoldEntry entry;
            entry.from = c;
            entry.non_unicode = c <= 0xFFFF ? canonicalize_non_unicode(c) : c;
            entry.unicode = canonicalize_unicode(c);
            if (entry.non_unicode != c || entry.unicode != c)
                entries.push_back(entry);
        }
        return entries;
    }();
    return table;
}

std::vector<Range> normalized(std::vector<Range> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](Range const& a, Range const& b) { return a.first < b.first; });
    std::vector<Range> merged;
    for (Range const& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().last + 1)
            merged.back().last = std::max(merged.back().last, range.last);
        else
            merged.push_back(range);
    }
    return merged;
}

// The complement within [0, max_char] of a normalized range list.
std::vector<Range> complemented(std::vector<Range> const& ranges, char32_t max_char)
{
    std::vector<Range> out;
    char32_t next = 0;
    bool exhausted = false;
    for (Range const& range : ranges) {
        if (range.first > next)
            out.push_back({ next, range.first - 1 });
        if (range.last >= max_char) {
            exhausted = true;
            break;
        }
        next = range.last + 1;
    }
    if (!exhausted && next <= max_char)
        out.push_back({ next, max_char });
    return out;
}

// CharacterSetMatcher (§22.2.2.7.1) under ignoreCase asks whether some
// member a of the set has Canonicalize(a) equal to the canonicalized input
// character — that is, whether the input's canonical form lies in the
// canonical image of the set. The image is the set with every member that
// moves replaced by where it goes; computed once here so the matcher does
// one range lookup per character.
std::vector<Range> canonicalized(std::vector<Range> const& ranges, bool unicode)
{
    auto const& table = case_fold_table();
    std::vector<char32_t> removed;
    std::vector<Range> added;
    for (Range const& range : ranges) {
        auto it = std::lower_bound(table.begin(), table.end(), range.first,
            [](CaseFoldEntry const& entry, char32_t value) { return entry.from < value; });
        for (; it != table.end() && it->from <= range.last; ++it) {
            char32_t const mapped = unicode ? it->unicode : it->non_unicode;
            if (mapped == it->from)
                continue;
            removed.push_back(it->from);
            added.push_back({ mapped, mapped });
        }
    }
    if (removed.empty())
        return ranges;
    std::vector<Range> out;
    std::size_t k = 0;
    for (Range const& range : ranges) {
        char32_t start = range.first;
        bool open = true;
        while (k < removed.size() && removed[k] <= range.last) {
            char32_t const point = removed[k++];
            if (point > start)
                out.push_back({ start, point - 1 });
            if (point == std::numeric_limits<char32_t>::max()) {
                open = false;
                break;
            }
            start = point + 1;
        }
        if (open && start <= range.last)
            out.push_back({ start, range.last });
    }
    out.insert(out.end(), added.begin(), added.end());
    return normalized(std::move(out));
}

std::vector<Range> digit_ranges() { return { { '0', '9' } }; }

// \s is WhiteSpace (§12.2: TAB VT FF SP NBSP ZWNBSP and Zs) plus
// LineTerminator (§22.2.2.9 CharacterClassEscape :: s).
std::vector<Range> space_ranges()
{
    return { { 0x09, 0x0D }, { 0x20, 0x20 }, { 0xA0, 0xA0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200A },
        { 0x2028, 0x2029 }, { 0x202F, 0x202F }, { 0x205F, 0x205F }, { 0x3000, 0x3000 }, { 0xFEFF, 0xFEFF } };
}

// WordCharacters (§22.2.2.9.3): the basic word characters and, under `u`
// with ignoreCase, every character whose canonical form is one of them
// (ſ and K), so that \w, \W and \b agree with the case folding.
std::vector<Range> word_ranges(bool unicode, bool ignore_case)
{
    std::vector<Range> ranges { { '0', '9' }, { 'A', 'Z' }, { '_', '_' }, { 'a', 'z' } };
    if (unicode && ignore_case) {
        for (CaseFoldEntry const& entry : case_fold_table())
            if (entry.unicode != entry.from && is_ascii_word(entry.unicode) && !is_ascii_word(entry.from))
                ranges.push_back({ entry.from, entry.from });
    }
    return normalized(std::move(ranges));
}

// The set a CharacterClassEscape letter (d D s S w W) stands for.
std::vector<Range> class_escape_ranges(char16_t letter, bool unicode, bool ignore_case)
{
    char32_t const max_char = unicode ? 0x10FFFF : 0xFFFF;
    switch (letter) {
    case u'd':
        return digit_ranges();
    case u'D':
        return complemented(digit_ranges(), max_char);
    case u's':
        return space_ranges();
    case u'S':
        return complemented(space_ranges(), max_char);
    case u'w':
        return word_ranges(unicode, ignore_case);
    default:
        return complemented(word_ranges(unicode, ignore_case), max_char);
    }
}

// RegExpIdentifierName (§22.2.1) is IdentifierStart (IdentifierPart)*.
// Stand-ins for the lexer's ID_Start / ID_Continue tests: ASCII exactly,
// and above it any code point that is not a separator, control, format
// character or lone surrogate — marks may continue a name but not start
// one. The names a page writes are ASCII in practice.
bool is_identifier_start(char32_t c)
{
    if (c < 0x80)
        return is_ascii_letter(c) || c == '$' || c == '_';
    return !is_first_letter_skipped(c) && !is_surrogate(c) && !is_combining_mark(c);
}
bool is_identifier_part(char32_t c)
{
    if (c < 0x80)
        return is_ascii_word(c) || c == '$';
    if (c == 0x200C || c == 0x200D)
        return true;
    return !is_first_letter_skipped(c) && !is_surrogate(c);
}

void append_utf16(std::u16string& out, char32_t c)
{
    if (c > 0xFFFF) {
        c -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
    } else {
        out.push_back(static_cast<char16_t>(c));
    }
}

// ---------------------------------------------------------------- parsing

enum class NodeKind : std::uint8_t {
    Empty,
    Char,
    Any,
    Class,
    Backref,
    NamedBackref, // resolved to Backref or BackrefSet once the whole pattern is parsed
    BackrefSet,
    LineStart,
    LineEnd,
    WordBoundary,
    NotWordBoundary,
    Group,
    NonCapturingGroup,
    Lookahead,
    Alternation,
    Sequence,
    Repeat,
};

struct Node {
    NodeKind kind = NodeKind::Empty;
    char32_t ch = 0; // Char
    std::uint32_t index = 0; // Class: class index; Group, Backref: group number
    bool negative = false; // Lookahead
    bool greedy = true; // Repeat
    std::uint32_t min = 0; // Repeat
    std::uint32_t max = 0;
    std::uint32_t first_group = 0; // Repeat: the capturing groups inside the body
    std::uint32_t last_group = 0;
    std::uint32_t child = 0; // Group, NonCapturingGroup, Lookahead, Repeat
    std::vector<std::uint32_t> children; // Alternation, Sequence
    std::vector<std::uint32_t> group_set; // BackrefSet: the groups sharing the name
};

// Where a named group sits: for each enclosing Disjunction, which of its
// Alternatives holds the group. Two groups may share a name only when
// some common Disjunction has them in different Alternatives, so that
// they can never both participate (§22.2.1.1 MightBothParticipate).
using AlternativePath = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

bool might_both_participate(AlternativePath const& a, AlternativePath const& b)
{
    std::size_t const common = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (a[i].first != b[i].first)
            break;
        if (a[i].second != b[i].second)
            return false;
    }
    return true;
}

struct ClassAtom {
    bool is_set = false;
    char32_t ch = 0;
    std::vector<Range> set;
};

struct Escape {
    enum class Kind : std::uint8_t { Character, ClassEscape, Backslash };
    Kind kind = Kind::Character;
    char32_t ch = 0;
    char16_t letter = 0; // ClassEscape: d D s S w W
};

struct PendingName {
    std::uint32_t node = 0;
    std::u16string name;
    std::size_t offset = 0;
};

class PatternParser {
public:
    PatternParser(std::u16string_view pattern, RegexFlags flags)
        : m_pattern(pattern)
        , m_unicode(flags.unicode)
        , m_ignore_case(flags.ignore_case)
    {
    }

    bool parse();

    Regex::CompileError const& error() const { return m_error; }
    std::vector<Node> const& nodes() const { return m_nodes; }
    std::uint32_t root() const { return m_root; }
    std::vector<CharClass>& classes() { return m_classes; }
    std::uint32_t group_count() const { return m_group_index; }
    std::vector<std::pair<std::u16string, std::size_t>>& names() { return m_names; }

private:
    bool at_end() const { return m_pos >= m_pattern.size(); }
    char16_t unit() const { return m_pattern[m_pos]; }
    char16_t unit_at(std::size_t offset) const
    {
        return m_pos + offset < m_pattern.size() ? m_pattern[m_pos + offset] : u'\0';
    }
    bool at(char16_t c) const { return !at_end() && unit() == c; }
    bool error(std::string message, std::size_t offset)
    {
        if (!m_failed) {
            m_failed = true;
            m_error.message = std::move(message);
            m_error.offset = offset;
        }
        return false;
    }
    std::uint32_t make(Node node)
    {
        m_nodes.push_back(std::move(node));
        return static_cast<std::uint32_t>(m_nodes.size() - 1);
    }
    std::uint32_t make_simple(NodeKind kind)
    {
        Node node;
        node.kind = kind;
        return make(std::move(node));
    }
    std::uint32_t make_char(char32_t ch)
    {
        Node node;
        node.kind = NodeKind::Char;
        node.ch = ch;
        return make(std::move(node));
    }
    std::uint32_t make_class(std::vector<Range> ranges, bool negated)
    {
        CharClass cls;
        cls.ranges = normalized(std::move(ranges));
        if (m_ignore_case)
            cls.ranges = canonicalized(cls.ranges, m_unicode);
        cls.negated = negated;
        m_classes.push_back(std::move(cls));
        Node node;
        node.kind = NodeKind::Class;
        node.index = static_cast<std::uint32_t>(m_classes.size() - 1);
        return make(std::move(node));
    }

    // One PatternCharacter: a code point under `u` (a surrogate pair is
    // one character there, §22.2.1.1), a code unit otherwise.
    char32_t read_pattern_char()
    {
        char16_t const first = m_pattern[m_pos++];
        if (m_unicode && is_high_surrogate(first) && !at_end() && is_low_surrogate(unit()))
            return combine_surrogates(first, m_pattern[m_pos++]);
        return first;
    }
    // Group names read code points in every mode (RegExpIdentifierName
    // joins a surrogate pair even without `u`).
    char32_t read_code_point()
    {
        char16_t const first = m_pattern[m_pos++];
        if (is_high_surrogate(first) && !at_end() && is_low_surrogate(unit()))
            return combine_surrogates(first, m_pattern[m_pos++]);
        return first;
    }

    void prescan();
    std::optional<std::uint32_t> parse_disjunction(int depth);
    std::optional<std::uint32_t> parse_alternative(int depth);
    std::optional<std::uint32_t> parse_term(int depth);
    std::optional<std::uint32_t> parse_group_body(int depth, std::size_t group_start);
    enum class Braced : std::uint8_t { NotQuantifier, Quantifier, Error };
    Braced parse_braced_quantifier(std::uint32_t& min, std::uint32_t& max);
    std::optional<std::uint32_t> parse_atom_escape();
    bool parse_escape(bool in_class, Escape& out);
    std::optional<char32_t> parse_unicode_escape(bool unicode_mode);
    char32_t parse_legacy_octal();
    std::optional<std::u16string> parse_group_name();
    std::optional<std::uint32_t> parse_class();
    bool parse_class_atom(ClassAtom& out);

    std::u16string_view m_pattern;
    std::size_t m_pos = 0;
    bool m_unicode;
    bool m_ignore_case;
    std::uint32_t m_total_groups = 0; // from the prescan: what \N may refer to
    bool m_has_named_groups = false;
    std::uint32_t m_group_index = 0; // groups opened so far, in order
    std::vector<Node> m_nodes;
    std::vector<CharClass> m_classes;
    std::vector<std::pair<std::u16string, std::size_t>> m_names;
    std::vector<AlternativePath> m_name_paths; // parallel to m_names
    std::vector<PendingName> m_pending_names;
    AlternativePath m_path; // the alternatives enclosing the current position
    std::uint32_t m_next_disjunction = 0;
    std::uint32_t m_root = 0;
    Regex::CompileError m_error;
    bool m_failed = false;
};

// Whether \N is a backreference depends on how many groups the WHOLE
// pattern has (§22.2.1.1 counts CountLeftCapturingParensWithin the
// Pattern; Annex B.1.2 makes the rest a legacy octal escape), and whether
// \k is a reference depends on whether any group is named. Both are known
// before parsing by a pass that only tracks escapes and classes.
void PatternParser::prescan()
{
    bool in_class = false;
    std::size_t const size = m_pattern.size();
    for (std::size_t i = 0; i < size; ++i) {
        char16_t const c = m_pattern[i];
        if (c == u'\\') {
            ++i;
            continue;
        }
        if (in_class) {
            if (c == u']')
                in_class = false;
            continue;
        }
        if (c == u'[') {
            in_class = true;
            continue;
        }
        if (c != u'(')
            continue;
        if (i + 1 < size && m_pattern[i + 1] == u'?') {
            if (i + 3 < size && m_pattern[i + 2] == u'<' && m_pattern[i + 3] != u'=' && m_pattern[i + 3] != u'!') {
                ++m_total_groups;
                m_has_named_groups = true;
            }
            continue;
        }
        ++m_total_groups;
    }
}

bool PatternParser::parse()
{
    prescan();
    auto const root = parse_disjunction(0);
    if (!root)
        return false;
    if (!at_end())
        return error("Unmatched ')'", m_pos);
    // A named reference may precede its group (§22.2.1.1 checks the whole
    // pattern for the name), so they resolve after the parse. A name held
    // by several groups (in different alternatives) refers to whichever
    // of them participated.
    for (PendingName const& pending : m_pending_names) {
        std::vector<std::uint32_t> numbers;
        for (auto const& entry : m_names)
            if (entry.first == pending.name)
                numbers.push_back(static_cast<std::uint32_t>(entry.second));
        if (numbers.empty())
            return error("Invalid named capture referenced", pending.offset);
        Node& node = m_nodes[pending.node];
        if (numbers.size() == 1) {
            node.kind = NodeKind::Backref;
            node.index = numbers[0];
        } else {
            node.kind = NodeKind::BackrefSet;
            node.group_set = std::move(numbers);
        }
    }
    m_root = *root;
    return true;
}

std::optional<std::uint32_t> PatternParser::parse_disjunction(int depth)
{
    if (depth > max_nesting_depth) {
        error("Regular expression too deeply nested", m_pos);
        return std::nullopt;
    }
    std::uint32_t const disjunction = m_next_disjunction++;
    std::vector<std::uint32_t> alternatives;
    for (;;) {
        m_path.emplace_back(disjunction, static_cast<std::uint32_t>(alternatives.size()));
        auto const alternative = parse_alternative(depth);
        m_path.pop_back();
        if (!alternative)
            return std::nullopt;
        alternatives.push_back(*alternative);
        if (at(u'|')) {
            ++m_pos;
            continue;
        }
        break;
    }
    if (alternatives.size() == 1)
        return alternatives[0];
    Node node;
    node.kind = NodeKind::Alternation;
    node.children = std::move(alternatives);
    return make(std::move(node));
}

std::optional<std::uint32_t> PatternParser::parse_alternative(int depth)
{
    std::vector<std::uint32_t> terms;
    while (!at_end() && unit() != u'|' && unit() != u')') {
        auto const term = parse_term(depth);
        if (!term)
            return std::nullopt;
        terms.push_back(*term);
    }
    if (terms.empty())
        return make_simple(NodeKind::Empty);
    if (terms.size() == 1)
        return terms[0];
    Node node;
    node.kind = NodeKind::Sequence;
    node.children = std::move(terms);
    return make(std::move(node));
}

std::optional<std::uint32_t> PatternParser::parse_group_body(int depth, std::size_t group_start)
{
    auto const body = parse_disjunction(depth + 1);
    if (!body)
        return std::nullopt;
    if (!at(u')')) {
        error("Unterminated group", group_start);
        return std::nullopt;
    }
    ++m_pos;
    return body;
}

// QuantifierPrefix :: { DecimalDigits } | { DecimalDigits , } | { DecimalDigits , DecimalDigits }.
// Consumes the braces only when they form one of those; a `{` that does
// not is left for the caller (a literal under Annex B, an error under `u`).
PatternParser::Braced PatternParser::parse_braced_quantifier(std::uint32_t& min, std::uint32_t& max)
{
    std::size_t const start = m_pos;
    std::size_t i = m_pos + 1;
    auto const read_number = [&](std::uint32_t& out) {
        if (i >= m_pattern.size() || !is_decimal_digit(m_pattern[i]))
            return false;
        std::uint64_t value = 0;
        while (i < m_pattern.size() && is_decimal_digit(m_pattern[i])) {
            value = std::min<std::uint64_t>(value * 10 + (m_pattern[i] - u'0'), 1u << 30);
            ++i;
        }
        out = static_cast<std::uint32_t>(value);
        return true;
    };
    if (!read_number(min))
        return Braced::NotQuantifier;
    if (i < m_pattern.size() && m_pattern[i] == u'}') {
        max = min;
        ++i;
    } else {
        if (i >= m_pattern.size() || m_pattern[i] != u',')
            return Braced::NotQuantifier;
        ++i;
        if (i < m_pattern.size() && m_pattern[i] == u'}') {
            max = infinite;
            ++i;
        } else {
            if (!read_number(max))
                return Braced::NotQuantifier;
            if (i >= m_pattern.size() || m_pattern[i] != u'}')
                return Braced::NotQuantifier;
            ++i;
        }
    }
    m_pos = i;
    if (max != infinite && min > max) {
        error("numbers out of order in {} quantifier", start);
        return Braced::Error;
    }
    if (min > max_quantifier_bound || (max != infinite && max > max_quantifier_bound)) {
        error("Quantifier bound too large (above 65535)", start);
        return Braced::Error;
    }
    return Braced::Quantifier;
}

std::optional<std::uint32_t> PatternParser::parse_term(int depth)
{
    std::size_t const term_start = m_pos;
    std::uint32_t const groups_before = m_group_index;
    bool quantifiable = true;
    std::optional<std::uint32_t> atom;
    char16_t const c = unit();
    switch (c) {
    case u'^':
        ++m_pos;
        atom = make_simple(NodeKind::LineStart);
        quantifiable = false;
        break;
    case u'$':
        ++m_pos;
        atom = make_simple(NodeKind::LineEnd);
        quantifiable = false;
        break;
    case u'\\':
        if (unit_at(1) == u'b' || unit_at(1) == u'B') {
            atom = make_simple(unit_at(1) == u'b' ? NodeKind::WordBoundary : NodeKind::NotWordBoundary);
            m_pos += 2;
            quantifiable = false;
            break;
        }
        ++m_pos;
        atom = parse_atom_escape();
        break;
    case u'(': {
        if (unit_at(1) == u'?') {
            char16_t const kind = unit_at(2);
            if (kind == u'=' || kind == u'!') {
                m_pos += 3;
                auto const body = parse_group_body(depth, term_start);
                if (!body)
                    return std::nullopt;
                Node node;
                node.kind = NodeKind::Lookahead;
                node.negative = kind == u'!';
                node.child = *body;
                atom = make(std::move(node));
                // Annex B.1.2: a lookahead is a QuantifiableAssertion
                // outside `u` mode; under `u` a quantifier on it is an error.
                quantifiable = !m_unicode;
                break;
            }
            if (kind == u'<' && (unit_at(3) == u'=' || unit_at(3) == u'!')) {
                error("Lookbehind assertions are not supported", term_start);
                return std::nullopt;
            }
            if (kind == u':') {
                m_pos += 3;
                auto const body = parse_group_body(depth, term_start);
                if (!body)
                    return std::nullopt;
                Node node;
                node.kind = NodeKind::NonCapturingGroup;
                node.child = *body;
                atom = make(std::move(node));
                break;
            }
            if (kind == u'<') {
                m_pos += 3;
                auto const name = parse_group_name();
                if (!name)
                    return std::nullopt;
                for (std::size_t k = 0; k < m_names.size(); ++k) {
                    if (m_names[k].first == *name && might_both_participate(m_name_paths[k], m_path)) {
                        error("Duplicate capture group name", term_start);
                        return std::nullopt;
                    }
                }
                std::uint32_t const number = ++m_group_index;
                m_names.emplace_back(*name, number);
                m_name_paths.push_back(m_path);
                auto const body = parse_group_body(depth, term_start);
                if (!body)
                    return std::nullopt;
                Node node;
                node.kind = NodeKind::Group;
                node.index = number;
                node.child = *body;
                atom = make(std::move(node));
                break;
            }
            error("Invalid group", term_start);
            return std::nullopt;
        }
        ++m_pos;
        std::uint32_t const number = ++m_group_index;
        auto const body = parse_group_body(depth, term_start);
        if (!body)
            return std::nullopt;
        Node node;
        node.kind = NodeKind::Group;
        node.index = number;
        node.child = *body;
        atom = make(std::move(node));
        break;
    }
    case u'*':
    case u'+':
    case u'?':
        error("Nothing to repeat", term_start);
        return std::nullopt;
    case u'{': {
        // Annex B.1.2: a `{` that does not open a quantifier is a literal
        // (ExtendedPatternCharacter); one that does, with nothing before
        // it, is InvalidBracedQuantifier — an error in every mode.
        std::uint32_t min = 0;
        std::uint32_t max = 0;
        Braced const braced = parse_braced_quantifier(min, max);
        if (braced == Braced::Error)
            return std::nullopt;
        if (braced == Braced::Quantifier) {
            error("Nothing to repeat", term_start);
            return std::nullopt;
        }
        if (m_unicode) {
            error("Lone quantifier brackets", term_start);
            return std::nullopt;
        }
        ++m_pos;
        atom = make_char(u'{');
        break;
    }
    case u']':
    case u'}':
        if (m_unicode) {
            error("Lone quantifier brackets", term_start);
            return std::nullopt;
        }
        ++m_pos;
        atom = make_char(c);
        break;
    case u'.':
        ++m_pos;
        atom = make_simple(NodeKind::Any);
        break;
    case u'[':
        atom = parse_class();
        break;
    default:
        atom = make_char(read_pattern_char());
        break;
    }
    if (!atom)
        return std::nullopt;
    if (at_end())
        return atom;

    std::uint32_t min = 0;
    std::uint32_t max = 0;
    bool has_quantifier = false;
    std::size_t const quantifier_start = m_pos;
    switch (unit()) {
    case u'*':
        min = 0;
        max = infinite;
        has_quantifier = true;
        ++m_pos;
        break;
    case u'+':
        min = 1;
        max = infinite;
        has_quantifier = true;
        ++m_pos;
        break;
    case u'?':
        min = 0;
        max = 1;
        has_quantifier = true;
        ++m_pos;
        break;
    case u'{': {
        Braced const braced = parse_braced_quantifier(min, max);
        if (braced == Braced::Error)
            return std::nullopt;
        if (braced == Braced::Quantifier) {
            has_quantifier = true;
        } else if (m_unicode) {
            error("Incomplete quantifier", quantifier_start);
            return std::nullopt;
        }
        break;
    }
    default:
        break;
    }
    if (!has_quantifier)
        return atom;
    if (!quantifiable) {
        error("Nothing to repeat", term_start);
        return std::nullopt;
    }
    Node node;
    node.kind = NodeKind::Repeat;
    node.child = *atom;
    node.min = min;
    node.max = max;
    node.greedy = true;
    if (at(u'?')) {
        ++m_pos;
        node.greedy = false;
    }
    node.first_group = groups_before + 1;
    node.last_group = m_group_index;
    return make(std::move(node));
}

// AtomEscape (§22.2.1): after the backslash. Backreferences and \k are
// decided here; everything else is a CharacterEscape or class escape
// shared with character classes.
std::optional<std::uint32_t> PatternParser::parse_atom_escape()
{
    std::size_t const escape_start = m_pos - 1;
    if (at_end()) {
        error("\\ at end of pattern", escape_start);
        return std::nullopt;
    }
    char16_t const c = unit();
    if (c >= u'1' && c <= u'9') {
        std::size_t const digits_start = m_pos;
        std::uint64_t number = 0;
        while (!at_end() && is_decimal_digit(unit())) {
            number = std::min<std::uint64_t>(number * 10 + (unit() - u'0'), 1u << 30);
            ++m_pos;
        }
        if (number <= m_total_groups) {
            Node node;
            node.kind = NodeKind::Backref;
            node.index = static_cast<std::uint32_t>(number);
            return make(std::move(node));
        }
        if (m_unicode) {
            error("Invalid escape", escape_start);
            return std::nullopt;
        }
        // Annex B.1.2: not a group the pattern has, so it is a legacy
        // octal escape (or the identity escape of 8 or 9) instead.
        m_pos = digits_start;
    } else if (c == u'k' && (m_unicode || m_has_named_groups)) {
        ++m_pos;
        if (!at(u'<')) {
            error("Invalid named reference", escape_start);
            return std::nullopt;
        }
        ++m_pos;
        auto const name = parse_group_name();
        if (!name)
            return std::nullopt;
        Node node;
        node.kind = NodeKind::NamedBackref;
        std::uint32_t const index = make(std::move(node));
        m_pending_names.push_back({ index, *name, escape_start });
        return index;
    }
    Escape escape;
    if (!parse_escape(false, escape))
        return std::nullopt;
    switch (escape.kind) {
    case Escape::Kind::Character:
        return make_char(escape.ch);
    case Escape::Kind::ClassEscape:
        return make_class(class_escape_ranges(escape.letter, m_unicode, m_ignore_case), false);
    case Escape::Kind::Backslash:
        return make_char(u'\\');
    }
    return std::nullopt;
}

// CharacterEscape and CharacterClassEscape (§22.2.1), plus what Annex
// B.1.2 adds outside `u` mode: identity escapes for anything, legacy
// octal escapes, and `\c` with no letter after it, which leaves the
// backslash as a literal and the `c` to be read again as itself.
bool PatternParser::parse_escape(bool in_class, Escape& out)
{
    std::size_t const escape_start = m_pos - 1;
    if (at_end())
        return error("\\ at end of pattern", escape_start);
    char16_t const c = unit();
    out.kind = Escape::Kind::Character;
    switch (c) {
    case u'd':
    case u'D':
    case u's':
    case u'S':
    case u'w':
    case u'W':
        ++m_pos;
        out.kind = Escape::Kind::ClassEscape;
        out.letter = c;
        return true;
    case u'p':
    case u'P':
        if (m_unicode)
            return error("Unicode property escapes (\\p{...}) are not supported", escape_start);
        ++m_pos;
        out.ch = c;
        return true;
    case u'b': // only reached inside a class: ClassEscape :: b is backspace
        ++m_pos;
        out.ch = 0x08;
        return true;
    case u'-':
        // ClassEscape :: [+UnicodeMode] -; an identity escape otherwise.
        if (m_unicode && !in_class)
            return error("Invalid escape", escape_start);
        ++m_pos;
        out.ch = u'-';
        return true;
    case u'c': {
        char16_t const next = unit_at(1);
        if (is_ascii_letter(next)) {
            m_pos += 2;
            out.ch = next % 32;
            return true;
        }
        // Annex B.1.2 ClassControlLetter: a digit or _ after \c in a class.
        if (in_class && !m_unicode && (is_decimal_digit(next) || next == u'_')) {
            m_pos += 2;
            out.ch = next % 32;
            return true;
        }
        if (m_unicode)
            return error("Invalid unicode escape", escape_start);
        out.kind = Escape::Kind::Backslash;
        return true;
    }
    case u'0':
        if (is_decimal_digit(unit_at(1))) {
            // \0 followed by a digit: CharacterEscape :: 0 [lookahead ∉
            // DecimalDigit] fails, so under `u` it is an error and under
            // Annex B a LegacyOctalEscapeSequence.
            if (m_unicode)
                return error("Invalid decimal escape", escape_start);
            out.ch = parse_legacy_octal();
            return true;
        }
        ++m_pos;
        out.ch = 0;
        return true;
    case u'1':
    case u'2':
    case u'3':
    case u'4':
    case u'5':
    case u'6':
    case u'7':
        if (m_unicode)
            return error(in_class ? "Invalid class escape" : "Invalid escape", escape_start);
        out.ch = parse_legacy_octal();
        return true;
    case u'8':
    case u'9':
        if (m_unicode)
            return error(in_class ? "Invalid class escape" : "Invalid escape", escape_start);
        ++m_pos;
        out.ch = c;
        return true;
    case u'x':
        if (is_hex_digit(unit_at(1)) && is_hex_digit(unit_at(2))) {
            out.ch = hex_value(unit_at(1)) * 16 + hex_value(unit_at(2));
            m_pos += 3;
            return true;
        }
        if (m_unicode)
            return error("Invalid escape", escape_start);
        ++m_pos;
        out.ch = u'x';
        return true;
    case u'u': {
        ++m_pos;
        std::size_t const after_u = m_pos;
        auto const value = parse_unicode_escape(m_unicode);
        if (value) {
            out.ch = *value;
            return true;
        }
        if (m_unicode)
            return error("Invalid Unicode escape", escape_start);
        m_pos = after_u;
        out.ch = u'u';
        return true;
    }
    case u't':
        ++m_pos;
        out.ch = 0x09;
        return true;
    case u'n':
        ++m_pos;
        out.ch = 0x0A;
        return true;
    case u'v':
        ++m_pos;
        out.ch = 0x0B;
        return true;
    case u'f':
        ++m_pos;
        out.ch = 0x0C;
        return true;
    case u'r':
        ++m_pos;
        out.ch = 0x0D;
        return true;
    case u'k':
        // Inside a class, or outside one with no named groups: an identity
        // escape only when Annex B applies and no group is named
        // (SourceCharacterIdentityEscape[+NamedCaptureGroups] excludes k).
        if (m_unicode || m_has_named_groups)
            return error("Invalid escape", escape_start);
        ++m_pos;
        out.ch = u'k';
        return true;
    default:
        break;
    }
    if (is_syntax_character(c) || c == u'/') {
        ++m_pos;
        out.ch = c;
        return true;
    }
    if (m_unicode)
        return error("Invalid escape", escape_start);
    out.ch = read_pattern_char();
    return true;
}

// LegacyOctalEscapeSequence (Annex B.1.2): up to three digits, but a first
// digit of 4–7 takes at most one more so the value stays below 256.
char32_t PatternParser::parse_legacy_octal()
{
    char32_t value = unit() - u'0';
    ++m_pos;
    if (at_end() || !is_octal_digit(unit()))
        return value;
    value = value * 8 + (unit() - u'0');
    ++m_pos;
    if (value >= 32 || at_end() || !is_octal_digit(unit()))
        return value;
    value = value * 8 + (unit() - u'0');
    ++m_pos;
    return value;
}

// RegExpUnicodeEscapeSequence after the `u`. In Unicode mode: u{…} or
// uHHHH, with a lead-and-trail pair of the latter joined (§22.2.1). Without
// it: uHHHH only. Consumes nothing on failure.
std::optional<char32_t> PatternParser::parse_unicode_escape(bool unicode_mode)
{
    std::size_t const start = m_pos;
    if (unicode_mode && at(u'{')) {
        std::size_t i = m_pos + 1;
        char32_t value = 0;
        std::size_t digits = 0;
        while (i < m_pattern.size() && is_hex_digit(m_pattern[i])) {
            value = value * 16 + hex_value(m_pattern[i]);
            ++digits;
            ++i;
            if (value > 0x10FFFF)
                return std::nullopt;
        }
        if (digits == 0 || i >= m_pattern.size() || m_pattern[i] != u'}')
            return std::nullopt;
        m_pos = i + 1;
        return value;
    }
    auto const four_hex = [&](std::size_t from, char32_t& out) {
        if (from + 4 > m_pattern.size())
            return false;
        char32_t value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            if (!is_hex_digit(m_pattern[from + k]))
                return false;
            value = value * 16 + hex_value(m_pattern[from + k]);
        }
        out = value;
        return true;
    };
    char32_t value = 0;
    if (!four_hex(start, value))
        return std::nullopt;
    m_pos = start + 4;
    if (unicode_mode && is_high_surrogate(value) && unit_at(0) == u'\\' && unit_at(1) == u'u') {
        char32_t low = 0;
        if (four_hex(m_pos + 2, low) && is_low_surrogate(low)) {
            m_pos += 6;
            return combine_surrogates(value, low);
        }
    }
    return value;
}

// GroupName :: < RegExpIdentifierName >, after the `<`.
std::optional<std::u16string> PatternParser::parse_group_name()
{
    std::size_t const name_start = m_pos;
    std::u16string name;
    bool first = true;
    for (;;) {
        if (at_end()) {
            error("Invalid capture group name", name_start);
            return std::nullopt;
        }
        if (unit() == u'>') {
            ++m_pos;
            break;
        }
        char32_t code_point = 0;
        if (unit() == u'\\') {
            ++m_pos;
            std::optional<char32_t> escaped;
            if (at(u'u')) {
                ++m_pos;
                escaped = parse_unicode_escape(true);
            }
            if (!escaped) {
                error("Invalid capture group name", name_start);
                return std::nullopt;
            }
            code_point = *escaped;
        } else {
            code_point = read_code_point();
        }
        if (first ? !is_identifier_start(code_point) : !is_identifier_part(code_point)) {
            error("Invalid capture group name", name_start);
            return std::nullopt;
        }
        append_utf16(name, code_point);
        first = false;
    }
    if (name.empty()) {
        error("Invalid capture group name", name_start);
        return std::nullopt;
    }
    return name;
}

// CharacterClass :: [ ClassContents ] and [^ ClassContents ].
std::optional<std::uint32_t> PatternParser::parse_class()
{
    std::size_t const class_start = m_pos;
    ++m_pos;
    bool negated = false;
    if (at(u'^')) {
        ++m_pos;
        negated = true;
    }
    std::vector<Range> ranges;
    auto const add_atom = [&](ClassAtom const& atom) {
        if (atom.is_set)
            ranges.insert(ranges.end(), atom.set.begin(), atom.set.end());
        else
            ranges.push_back({ atom.ch, atom.ch });
    };
    for (;;) {
        if (at_end()) {
            error("Unterminated character class", class_start);
            return std::nullopt;
        }
        if (unit() == u']') {
            ++m_pos;
            break;
        }
        std::size_t const atom_start = m_pos;
        ClassAtom first;
        if (!parse_class_atom(first))
            return std::nullopt;
        // NonemptyClassRanges :: ClassAtom - ClassAtom ClassContents: a dash
        // opens a range when anything but the closing bracket follows it (a
        // pattern may hold a real U+0000, so this cannot lean on unit_at's
        // end-of-pattern stand-in).
        if (at(u'-') && m_pos + 1 < m_pattern.size() && m_pattern[m_pos + 1] != u']') {
            ++m_pos;
            ClassAtom second;
            if (!parse_class_atom(second))
                return std::nullopt;
            if (first.is_set || second.is_set) {
                // NonemptyClassRangesNoDash with a class escape on either
                // side is an error under `u`; Annex B.1.2 reads the three
                // pieces as themselves instead.
                if (m_unicode) {
                    error("Invalid character class", atom_start);
                    return std::nullopt;
                }
                add_atom(first);
                ranges.push_back({ u'-', u'-' });
                add_atom(second);
                continue;
            }
            if (first.ch > second.ch) {
                error("Range out of order in character class", atom_start);
                return std::nullopt;
            }
            ranges.push_back({ first.ch, second.ch });
            continue;
        }
        add_atom(first);
    }
    return make_class(std::move(ranges), negated);
}

bool PatternParser::parse_class_atom(ClassAtom& out)
{
    out.is_set = false;
    if (unit() != u'\\') {
        out.ch = read_pattern_char();
        return true;
    }
    ++m_pos;
    Escape escape;
    if (!parse_escape(true, escape))
        return false;
    switch (escape.kind) {
    case Escape::Kind::Character:
        out.ch = escape.ch;
        return true;
    case Escape::Kind::ClassEscape:
        out.is_set = true;
        out.set = class_escape_ranges(escape.letter, m_unicode, m_ignore_case);
        return true;
    case Escape::Kind::Backslash:
        out.ch = u'\\';
        return true;
    }
    return false;
}

// ------------------------------------------------------------- compiling

class CodeGenerator {
public:
    CodeGenerator(PatternParser const& parser, RegexProgram& program)
        : m_nodes(parser.nodes())
        , m_program(program)
        , m_ignore_case(program.flags.ignore_case)
        , m_unicode(program.flags.unicode)
        , m_dot_all(program.flags.dot_all)
    {
    }

    void generate(std::uint32_t root)
    {
        emit(root);
        add({ Op::Match, 0, 0 });
        m_program.register_count = static_cast<std::uint32_t>(2 * (m_program.group_count + 1) + 2 * m_program.loops.size());
    }

private:
    std::uint32_t add(Instruction instruction)
    {
        m_program.code.push_back(instruction);
        return static_cast<std::uint32_t>(m_program.code.size() - 1);
    }
    std::uint32_t here() const { return static_cast<std::uint32_t>(m_program.code.size()); }

    char32_t canonical(char32_t ch) const
    {
        if (!m_ignore_case)
            return ch;
        return m_unicode ? canonicalize_unicode(ch) : canonicalize_non_unicode(ch);
    }

    // A body that is one character wide and owns no capture, seen through
    // any (?:…) wrapping: the loop over it needs no per-iteration state.
    std::optional<std::uint32_t> single_character(std::uint32_t index) const
    {
        Node const* node = &m_nodes[index];
        while (node->kind == NodeKind::NonCapturingGroup)
            node = &m_nodes[node->child];
        if (node->kind == NodeKind::Char || node->kind == NodeKind::Any || node->kind == NodeKind::Class)
            return static_cast<std::uint32_t>(node - m_nodes.data());
        return std::nullopt;
    }

    void emit_single(Node const& node)
    {
        switch (node.kind) {
        case NodeKind::Char:
            add({ Op::Char, canonical(node.ch), 0 });
            break;
        case NodeKind::Any:
            add({ m_dot_all ? Op::AnyAll : Op::Any, 0, 0 });
            break;
        default:
            add({ Op::Class, node.index, 0 });
            break;
        }
    }

    // Recursion here follows the tree the parser built, whose depth the
    // parser capped; a level of grouping adds at most a few frames.
    void emit(std::uint32_t index)
    {
        Node const& node = m_nodes[index];
        switch (node.kind) {
        case NodeKind::Empty:
            break;
        case NodeKind::Char:
        case NodeKind::Any:
        case NodeKind::Class:
            emit_single(node);
            break;
        case NodeKind::Backref:
        case NodeKind::NamedBackref:
            add({ Op::Backref, node.index, 0 });
            break;
        case NodeKind::BackrefSet:
            m_program.backref_sets.push_back(node.group_set);
            add({ Op::BackrefSet, static_cast<std::uint32_t>(m_program.backref_sets.size() - 1), 0 });
            break;
        case NodeKind::LineStart:
            add({ Op::LineStart, 0, 0 });
            break;
        case NodeKind::LineEnd:
            add({ Op::LineEnd, 0, 0 });
            break;
        case NodeKind::WordBoundary:
            add({ Op::WordBoundary, 0, 0 });
            break;
        case NodeKind::NotWordBoundary:
            add({ Op::NotWordBoundary, 0, 0 });
            break;
        case NodeKind::Group:
            add({ Op::Save, 2 * node.index, 0 });
            emit(node.child);
            add({ Op::Save, 2 * node.index + 1, 0 });
            break;
        case NodeKind::NonCapturingGroup:
            emit(node.child);
            break;
        case NodeKind::Lookahead: {
            std::uint32_t const start = add({ Op::LookStart, 0, node.negative ? 1u : 0u });
            emit(node.child);
            add({ Op::LookEnd, 0, 0 });
            m_program.code[start].a = here();
            break;
        }
        case NodeKind::Alternation: {
            std::vector<std::uint32_t> jumps;
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                if (i + 1 < node.children.size()) {
                    std::uint32_t const split = add({ Op::Split, 0, 0 });
                    m_program.code[split].a = here();
                    emit(node.children[i]);
                    jumps.push_back(add({ Op::Jump, 0, 0 }));
                    m_program.code[split].b = here();
                } else {
                    emit(node.children[i]);
                }
            }
            for (std::uint32_t const jump : jumps)
                m_program.code[jump].a = here();
            break;
        }
        case NodeKind::Sequence:
            for (std::uint32_t const child : node.children)
                emit(child);
            break;
        case NodeKind::Repeat:
            emit_repeat(node);
            break;
        }
    }

    void emit_repeat(Node const& node)
    {
        // RepeatMatcher (§22.2.2.3.1) with max = 0 is the continuation
        // alone; the body's groups stay unset because it never runs.
        if (node.max == 0)
            return;
        Loop loop;
        loop.min = node.min;
        loop.max = node.max;
        loop.greedy = node.greedy;
        loop.first_slot = 2 * node.first_group;
        loop.last_slot = node.first_group > node.last_group ? loop.first_slot - 1 : 2 * node.last_group + 1;
        std::uint32_t const id = static_cast<std::uint32_t>(m_program.loops.size());
        loop.count_register = static_cast<std::uint32_t>(2 * (m_program.group_count + 1) + 2 * id);
        loop.start_register = loop.count_register + 1;
        if (auto const single = single_character(node.child)) {
            add({ node.greedy ? Op::SimpleGreedy : Op::SimpleLazy, id, 0 });
            emit_single(m_nodes[*single]);
            m_program.loops.push_back(loop);
            return;
        }
        add({ Op::RepeatInit, id, 0 });
        loop.top_pc = add({ Op::RepeatDecide, id, 0 });
        loop.body_pc = add({ Op::RepeatEnter, id, 0 });
        m_program.loops.push_back(loop);
        emit(node.child);
        add({ Op::RepeatExit, id, 0 });
        m_program.loops[id].exit_pc = here();
    }

    std::vector<Node> const& m_nodes;
    RegexProgram& m_program;
    bool m_ignore_case;
    bool m_unicode;
    bool m_dot_all;
};

// -------------------------------------------------------------- matching

enum class EntryKind : std::uint32_t {
    Undo = 0, // low: register; a: its previous value
    Choice = 1, // low: pc to resume; a: position
    Look = 2, // low: continuation pc; a: position; b: 1 when negative
    Backoff = 3, // low: pc of a SimpleGreedy; a: position; b: characters still giveable
    Advance = 4, // low: pc of a SimpleLazy; a: position; b: characters still takeable
};

struct Entry {
    std::uint32_t tag = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

Entry make_entry(EntryKind kind, std::uint32_t low, std::uint32_t a, std::uint32_t b)
{
    return { (static_cast<std::uint32_t>(kind) << entry_kind_shift) | low, a, b };
}
EntryKind kind_of(Entry const& entry) { return static_cast<EntryKind>(entry.tag >> entry_kind_shift); }
std::uint32_t low_of(Entry const& entry) { return entry.tag & entry_low_mask; }

class Matcher {
public:
    enum class Outcome : std::uint8_t { Matched, Failed, Exhausted };

    Matcher(RegexProgram const& program, std::u16string_view input, std::size_t budget)
        : m_program(program)
        , m_input(input)
        , m_length(static_cast<std::uint32_t>(input.size()))
        , m_budget(budget)
        , m_unicode(program.flags.unicode)
        , m_ignore_case(program.flags.ignore_case)
        , m_multiline(program.flags.multiline)
    {
    }

    Outcome run(std::uint32_t start);
    std::vector<std::uint32_t> const& registers() const { return m_registers; }

private:
    // The character at pos: a code point under `u` (a pair is one
    // character), a code unit otherwise. pos < m_length.
    char32_t read_char(std::uint32_t pos, std::uint32_t& width) const
    {
        char16_t const first = m_input[pos];
        width = 1;
        if (m_unicode && is_high_surrogate(first) && pos + 1 < m_length && is_low_surrogate(m_input[pos + 1])) {
            width = 2;
            return combine_surrogates(first, m_input[pos + 1]);
        }
        return first;
    }
    // The character that ends at pos (pos > 0).
    char32_t read_char_before(std::uint32_t pos) const
    {
        char16_t const last = m_input[pos - 1];
        if (m_unicode && is_low_surrogate(last) && pos >= 2 && is_high_surrogate(m_input[pos - 2]))
            return combine_surrogates(m_input[pos - 2], last);
        return last;
    }
    // One character back from pos, never before start: the inverse of the
    // forward walk, since a pair the walk crossed is never split.
    std::uint32_t step_back(std::uint32_t pos, std::uint32_t start) const
    {
        if (m_unicode && pos >= start + 2 && is_low_surrogate(m_input[pos - 1]) && is_high_surrogate(m_input[pos - 2]))
            return pos - 2;
        return pos - 1;
    }
    char32_t canonicalize(char32_t c) const
    {
        if (!m_ignore_case)
            return c;
        return m_unicode ? canonicalize_unicode(c) : canonicalize_non_unicode(c);
    }
    // IsWordChar (§22.2.2.9.3).
    bool is_word_char(char32_t c) const
    {
        if (is_ascii_word(c))
            return true;
        return m_unicode && m_ignore_case && is_ascii_word(canonicalize_unicode(c));
    }
    bool word_before(std::uint32_t pos) const { return pos > 0 && is_word_char(read_char_before(pos)); }
    bool word_at(std::uint32_t pos) const
    {
        std::uint32_t width = 0;
        return pos < m_length && is_word_char(read_char(pos, width));
    }
    bool matches_single(Instruction const& instruction, std::uint32_t pos, std::uint32_t& width) const
    {
        char32_t const c = read_char(pos, width);
        switch (instruction.op) {
        case Op::Char:
            return canonicalize(c) == instruction.a;
        case Op::Any:
            return !is_line_terminator(c);
        case Op::AnyAll:
            return true;
        case Op::Class: {
            CharClass const& cls = m_program.classes[instruction.a];
            return cls.contains(canonicalize(c)) != cls.negated;
        }
        default:
            return false;
        }
    }

    void push(Entry entry)
    {
        if (m_stack.size() >= max_stack_entries) {
            m_overflow = true;
            return;
        }
        m_stack.push_back(entry);
    }
    void set_register(std::uint32_t slot, std::uint32_t value)
    {
        push(make_entry(EntryKind::Undo, slot, m_registers[slot], 0));
        m_registers[slot] = value;
    }

    bool match_backreference(std::uint32_t group, std::uint32_t& pos) const;
    bool finish_lookahead(std::uint32_t& pc, std::uint32_t& pos);
    bool backtrack(std::uint32_t& pc, std::uint32_t& pos);

    RegexProgram const& m_program;
    std::u16string_view m_input;
    std::uint32_t m_length;
    std::size_t m_budget;
    std::size_t m_steps = 0;
    bool m_unicode;
    bool m_ignore_case;
    bool m_multiline;
    bool m_overflow = false;
    std::vector<std::uint32_t> m_registers;
    std::vector<Entry> m_stack;
};

// BackreferenceMatcher (§22.2.2.7.2): a group that did not participate
// matches the empty string; otherwise the captured text must recur here,
// character by character under ignoreCase.
bool Matcher::match_backreference(std::uint32_t group, std::uint32_t& pos) const
{
    std::uint32_t const start = m_registers[2 * group];
    std::uint32_t const end = m_registers[2 * group + 1];
    if (start == unset || end == unset)
        return true;
    if (!m_ignore_case) {
        std::uint32_t const length = end - start;
        if (length > m_length - pos)
            return false;
        for (std::uint32_t i = 0; i < length; ++i)
            if (m_input[start + i] != m_input[pos + i])
                return false;
        pos += length;
        return true;
    }
    std::uint32_t i = start;
    std::uint32_t j = pos;
    while (i < end) {
        if (j >= m_length)
            return false;
        std::uint32_t width_i = 0;
        std::uint32_t width_j = 0;
        char32_t const a = read_char(i, width_i);
        char32_t const b = read_char(j, width_j);
        if (canonicalize(a) != canonicalize(b))
            return false;
        i += width_i;
        j += width_j;
    }
    pos = j;
    return true;
}

// The body of a lookahead has matched. Per §22.2.2.4, a positive one
// continues from where it began with the body's captures kept and its
// choice points discarded (the assertion is atomic): the undo records
// above the marker are kept, in order, so a later backtrack past the
// assertion still restores everything. A negative one has just failed:
// the body's effects are undone and matching backtracks past the marker.
bool Matcher::finish_lookahead(std::uint32_t& pc, std::uint32_t& pos)
{
    std::size_t index = m_stack.size();
    while (index > 0 && kind_of(m_stack[index - 1]) != EntryKind::Look)
        --index;
    Entry const marker = m_stack[index - 1];
    if (marker.b == 0) {
        std::size_t write = index - 1;
        for (std::size_t j = index; j < m_stack.size(); ++j)
            if (kind_of(m_stack[j]) == EntryKind::Undo)
                m_stack[write++] = m_stack[j];
        m_stack.resize(write);
        pos = marker.a;
        pc = low_of(marker);
        return true;
    }
    for (std::size_t j = m_stack.size(); j > index; --j) {
        Entry const& entry = m_stack[j - 1];
        if (kind_of(entry) == EntryKind::Undo)
            m_registers[low_of(entry)] = entry.a;
    }
    m_stack.resize(index - 1);
    return false;
}

// Pops the stack to the nearest thing that can be resumed, restoring
// registers on the way. False when nothing is left: the attempt failed.
bool Matcher::backtrack(std::uint32_t& pc, std::uint32_t& pos)
{
    for (;;) {
        if (m_stack.empty())
            return false;
        Entry const entry = m_stack.back();
        std::uint32_t const low = low_of(entry);
        switch (kind_of(entry)) {
        case EntryKind::Undo:
            m_registers[low] = entry.a;
            m_stack.pop_back();
            break;
        case EntryKind::Choice:
            m_stack.pop_back();
            pc = low;
            pos = entry.a;
            return true;
        case EntryKind::Look:
            // The body failed: a negative assertion holds and matching goes
            // on after it; a positive one fails through to what came before.
            m_stack.pop_back();
            if (entry.b != 0) {
                pc = low;
                pos = entry.a;
                return true;
            }
            break;
        case EntryKind::Backoff: {
            Loop const& loop = m_program.loops[m_program.code[low].a];
            std::uint32_t const previous = step_back(entry.a, m_registers[loop.start_register]);
            if (entry.b <= 1) {
                m_stack.pop_back();
            } else {
                m_stack.back().a = previous;
                m_stack.back().b = entry.b - 1;
            }
            pc = low + 2;
            pos = previous;
            return true;
        }
        case EntryKind::Advance: {
            std::uint32_t width = 0;
            if (entry.a < m_length && matches_single(m_program.code[low + 1], entry.a, width)) {
                std::uint32_t const next = entry.a + width;
                if (entry.b == 1) {
                    m_stack.pop_back();
                } else {
                    m_stack.back().a = next;
                    if (entry.b != infinite)
                        m_stack.back().b = entry.b - 1;
                }
                ++m_steps;
                pc = low + 2;
                pos = next;
                return true;
            }
            m_stack.pop_back();
            break;
        }
        }
    }
}

Matcher::Outcome Matcher::run(std::uint32_t start)
{
    m_registers.assign(m_program.register_count, unset);
    m_registers[0] = start;
    m_stack.clear();
    std::uint32_t pc = 0;
    std::uint32_t pos = start;
    for (;;) {
        if (++m_steps > m_budget || m_overflow)
            return Outcome::Exhausted;
        bool ok = true;
        Instruction const& instruction = m_program.code[pc];
        switch (instruction.op) {
        case Op::Char:
        case Op::Any:
        case Op::AnyAll:
        case Op::Class: {
            std::uint32_t width = 0;
            if (pos < m_length && matches_single(instruction, pos, width)) {
                pos += width;
                ++pc;
            } else {
                ok = false;
            }
            break;
        }
        case Op::Backref:
            ok = match_backreference(instruction.a, pos);
            if (ok)
                ++pc;
            break;
        case Op::BackrefSet: {
            // BackreferenceMatcher (§22.2.2.7.2) over the groups sharing a
            // name: they sit in different alternatives, so at most one has
            // a value; with none it matches the empty string.
            std::uint32_t chosen = 0;
            for (std::uint32_t const group : m_program.backref_sets[instruction.a]) {
                if (m_registers[2 * group] != unset && m_registers[2 * group + 1] != unset) {
                    chosen = group;
                    break;
                }
            }
            ok = chosen == 0 || match_backreference(chosen, pos);
            if (ok)
                ++pc;
            break;
        }
        case Op::LineStart:
            // Multiline ^ also holds after a LineTerminator (§22.2.2.4).
            ok = pos == 0 || (m_multiline && is_line_terminator(m_input[pos - 1]));
            if (ok)
                ++pc;
            break;
        case Op::LineEnd:
            ok = pos == m_length || (m_multiline && is_line_terminator(m_input[pos]));
            if (ok)
                ++pc;
            break;
        case Op::WordBoundary:
            ok = word_before(pos) != word_at(pos);
            if (ok)
                ++pc;
            break;
        case Op::NotWordBoundary:
            ok = word_before(pos) == word_at(pos);
            if (ok)
                ++pc;
            break;
        case Op::Save:
            set_register(instruction.a, pos);
            ++pc;
            break;
        case Op::Split:
            push(make_entry(EntryKind::Choice, instruction.b, pos, 0));
            pc = instruction.a;
            break;
        case Op::Jump:
            pc = instruction.a;
            break;
        case Op::RepeatInit:
            set_register(m_program.loops[instruction.a].count_register, 0);
            ++pc;
            break;
        case Op::RepeatDecide: {
            // RepeatMatcher (§22.2.2.3.1): max reached is the continuation
            // alone; below min the body is mandatory; between, a greedy
            // loop tries the body with the exit as its alternative and a
            // lazy one the reverse.
            Loop const& loop = m_program.loops[instruction.a];
            std::uint32_t const count = m_registers[loop.count_register];
            if (loop.max != infinite && count >= loop.max) {
                pc = loop.exit_pc;
            } else if (count < loop.min) {
                pc = loop.body_pc;
            } else if (loop.greedy) {
                push(make_entry(EntryKind::Choice, loop.exit_pc, pos, 0));
                pc = loop.body_pc;
            } else {
                push(make_entry(EntryKind::Choice, loop.body_pc, pos, 0));
                pc = loop.exit_pc;
            }
            break;
        }
        case Op::RepeatEnter: {
            // Each iteration starts with the body's captures undefined
            // (§22.2.2.3.1 step 4: cap[k] = undefined for the body's groups).
            Loop const& loop = m_program.loops[instruction.a];
            set_register(loop.start_register, pos);
            for (std::uint32_t slot = loop.first_slot; slot <= loop.last_slot; ++slot)
                if (m_registers[slot] != unset)
                    set_register(slot, unset);
            ++pc;
            break;
        }
        case Op::RepeatExit: {
            // An iteration past the minimum that consumed nothing fails
            // (§22.2.2.3.1 step 2.b), which is what stops (a*)* and (?:)*.
            Loop const& loop = m_program.loops[instruction.a];
            std::uint32_t const count = m_registers[loop.count_register];
            if (count >= loop.min && pos == m_registers[loop.start_register]) {
                ok = false;
                break;
            }
            set_register(loop.count_register, count + 1);
            pc = loop.top_pc;
            break;
        }
        case Op::SimpleGreedy: {
            // A loop whose body is one character and no capture: take as
            // many as allowed, and give them back one at a time from a
            // single backtrack entry rather than one choice point each.
            Loop const& loop = m_program.loops[instruction.a];
            Instruction const& body = m_program.code[pc + 1];
            set_register(loop.start_register, pos);
            std::uint32_t count = 0;
            std::uint32_t p = pos;
            while (count < loop.max && p < m_length) {
                std::uint32_t width = 0;
                if (!matches_single(body, p, width))
                    break;
                p += width;
                ++count;
                if (++m_steps > m_budget)
                    return Outcome::Exhausted;
            }
            if (count < loop.min) {
                ok = false;
                break;
            }
            if (count > loop.min)
                push(make_entry(EntryKind::Backoff, pc, p, count - loop.min));
            pos = p;
            pc += 2;
            break;
        }
        case Op::SimpleLazy: {
            Loop const& loop = m_program.loops[instruction.a];
            Instruction const& body = m_program.code[pc + 1];
            std::uint32_t count = 0;
            std::uint32_t p = pos;
            while (count < loop.min) {
                std::uint32_t width = 0;
                if (p >= m_length || !matches_single(body, p, width)) {
                    ok = false;
                    break;
                }
                p += width;
                ++count;
                ++m_steps;
            }
            if (!ok)
                break;
            if (loop.max == infinite || count < loop.max)
                push(make_entry(EntryKind::Advance, pc, p, loop.max == infinite ? infinite : loop.max - count));
            pos = p;
            pc += 2;
            break;
        }
        case Op::LookStart:
            push(make_entry(EntryKind::Look, instruction.a, pos, instruction.b));
            ++pc;
            break;
        case Op::LookEnd:
            ok = finish_lookahead(pc, pos);
            break;
        case Op::Match:
            m_registers[1] = pos;
            return Outcome::Matched;
        }
        if (m_overflow || m_steps > m_budget)
            return Outcome::Exhausted;
        if (ok)
            continue;
        if (!backtrack(pc, pos))
            return Outcome::Failed;
    }
}

}

// ------------------------------------------------------------ RegexFlags

std::optional<RegexFlags> RegexFlags::parse(std::u16string_view text)
{
    RegexFlags flags;
    for (char16_t const c : text) {
        bool* field = nullptr;
        switch (c) {
        case u'd':
            field = &flags.has_indices;
            break;
        case u'g':
            field = &flags.global;
            break;
        case u'i':
            field = &flags.ignore_case;
            break;
        case u'm':
            field = &flags.multiline;
            break;
        case u's':
            field = &flags.dot_all;
            break;
        case u'u':
            field = &flags.unicode;
            break;
        case u'y':
            field = &flags.sticky;
            break;
        default:
            return std::nullopt;
        }
        if (*field)
            return std::nullopt;
        *field = true;
    }
    return flags;
}

std::u16string RegexFlags::to_string() const
{
    std::u16string out;
    if (has_indices)
        out += u'd';
    if (global)
        out += u'g';
    if (ignore_case)
        out += u'i';
    if (multiline)
        out += u'm';
    if (dot_all)
        out += u's';
    if (unicode)
        out += u'u';
    if (sticky)
        out += u'y';
    return out;
}

// ----------------------------------------------------------------- Regex

Regex::Regex() = default;
Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;

std::optional<Regex> Regex::compile(std::u16string_view pattern, RegexFlags flags, CompileError* error)
{
    PatternParser parser(pattern, flags);
    if (!parser.parse()) {
        if (error)
            *error = parser.error();
        return std::nullopt;
    }
    auto program = std::make_unique<RegexProgram>();
    program->flags = flags;
    program->group_count = parser.group_count();
    program->group_names = std::move(parser.names());
    program->classes = std::move(parser.classes());
    std::sort(program->group_names.begin(), program->group_names.end(),
        [](auto const& a, auto const& b) { return a.second < b.second; });
    CodeGenerator generator(parser, *program);
    generator.generate(parser.root());
    if (program->code.size() >= entry_low_mask || program->register_count >= entry_low_mask) {
        if (error)
            *error = { "Regular expression too large", 0 };
        return std::nullopt;
    }
    Regex regex;
    regex.m_program = std::move(program);
    return regex;
}

std::optional<Regex::Match> Regex::exec(std::u16string_view input, std::size_t start, bool* budget_exhausted) const
{
    if (budget_exhausted)
        *budget_exhausted = false;
    if (!m_program || start > input.size())
        return std::nullopt;
    if (input.size() >= static_cast<std::size_t>(unset) - 2) {
        // Positions are 32-bit inside the machine; a string this long is
        // beyond what a script can build, and refusing is not a wrong answer.
        if (budget_exhausted)
            *budget_exhausted = true;
        return std::nullopt;
    }
    RegexFlags const& program_flags = m_program->flags;
    Matcher matcher(*m_program, input, g_step_budget);
    std::uint32_t pos = static_cast<std::uint32_t>(start);
    // RegExpBuiltinExec (§22.2.7.2) step 12: under `u` the matcher starts
    // at the character that contains lastIndex, so a position inside a
    // surrogate pair backs up to the pair's lead.
    if (program_flags.unicode && pos > 0 && pos < input.size() && is_low_surrogate(input[pos])
        && is_high_surrogate(input[pos - 1]))
        --pos;
    // A pattern that opens with ^ outside multiline mode can only match at
    // the start of the input, so a failure there is the whole answer rather
    // than the first of one attempt per position.
    bool const anchored = !program_flags.multiline && m_program->code[0].op == Op::LineStart;
    if (anchored && pos > 0)
        return std::nullopt;
    for (;;) {
        Matcher::Outcome const outcome = matcher.run(pos);
        if (outcome == Matcher::Outcome::Matched) {
            Match match;
            std::vector<std::uint32_t> const& registers = matcher.registers();
            match.groups.reserve(m_program->group_count + 1);
            for (std::size_t group = 0; group <= m_program->group_count; ++group) {
                std::uint32_t const s = registers[2 * group];
                std::uint32_t const e = registers[2 * group + 1];
                if (s == unset || e == unset)
                    match.groups.emplace_back(std::nullopt);
                else
                    match.groups.emplace_back(std::make_pair(static_cast<std::size_t>(s), static_cast<std::size_t>(e)));
            }
            return match;
        }
        if (outcome == Matcher::Outcome::Exhausted) {
            if (budget_exhausted)
                *budget_exhausted = true;
            return std::nullopt;
        }
        if (program_flags.sticky || anchored || pos >= input.size())
            return std::nullopt;
        // AdvanceStringIndex (§22.2.7.3): by a code point under `u`.
        if (program_flags.unicode && pos + 1 < input.size() && is_high_surrogate(input[pos]) && is_low_surrogate(input[pos + 1]))
            pos += 2;
        else
            pos += 1;
    }
}

std::size_t Regex::group_count() const
{
    return m_program ? m_program->group_count : 0;
}

std::vector<std::pair<std::u16string, std::size_t>> const& Regex::group_names() const
{
    static std::vector<std::pair<std::u16string, std::size_t>> const none;
    return m_program ? m_program->group_names : none;
}

RegexFlags Regex::flags() const
{
    return m_program ? m_program->flags : RegexFlags {};
}

void Regex::set_step_budget(std::size_t budget)
{
    g_step_budget = budget;
}

std::size_t Regex::step_budget()
{
    return g_step_budget;
}

}
