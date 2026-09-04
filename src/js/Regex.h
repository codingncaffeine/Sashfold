#pragma once

// Regular expressions (§22.2), matched by backtracking over UTF-16 code
// units — or code points under the `u` flag. Written to the ES5 grammar
// plus the additions modern pages lean on: named groups, `s`, `y`, `u`,
// lookahead. No lookbehind and no property escapes yet; a pattern using
// them is a SyntaxError, never a silent mismatch.
//
// The matcher is iterative with an explicit backtrack stack and a step
// budget, so a pathological pattern on hostile input exhausts the budget
// rather than the process (a page cannot recurse the engine to death).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

struct RegexFlags {
    bool global = false; // g
    bool ignore_case = false; // i
    bool multiline = false; // m
    bool dot_all = false; // s
    bool unicode = false; // u
    bool sticky = false; // y

    // Parses "gimsuy" in any order; nullopt on a repeat or an unknown letter.
    static std::optional<RegexFlags> parse(std::u16string_view);
    std::u16string to_string() const; // canonical order: d g i m s u v y
};

struct RegexProgram; // the compiled form, private to Regex.cpp

class Regex {
public:
    struct Match {
        // groups[0] is the whole match; a group that did not participate
        // is nullopt. Offsets are code-unit indices into the input.
        std::vector<std::optional<std::pair<std::size_t, std::size_t>>> groups;
    };

    struct CompileError {
        std::string message; // "Invalid regular expression: …" without the prefix
        std::size_t offset = 0;
    };

    Regex();
    ~Regex();
    Regex(Regex&&) noexcept;
    Regex& operator=(Regex&&) noexcept;
    Regex(Regex const&) = delete;
    Regex& operator=(Regex const&) = delete;

    static std::optional<Regex> compile(std::u16string_view pattern, RegexFlags, CompileError* error = nullptr);

    // Tries a match starting at `start` and, unless the flags are sticky,
    // at each later position in turn. `budget_exhausted` (when given) is
    // set if the step budget ran out, in which case the result is nullopt.
    std::optional<Match> exec(std::u16string_view input, std::size_t start, bool* budget_exhausted = nullptr) const;

    std::size_t group_count() const; // capturing groups, not counting group 0
    // Group names in group order: (name, group number).
    std::vector<std::pair<std::u16string, std::size_t>> const& group_names() const;
    RegexFlags flags() const;

    // Steps a single exec() may take before giving up; tests lower it.
    static void set_step_budget(std::size_t);
    static std::size_t step_budget();

private:
    std::unique_ptr<RegexProgram> m_program;
};

}
