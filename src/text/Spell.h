#pragma once
// Spelling: a dictionary in the Hunspell format every desktop and both
// browsers' Linux builds read (an .aff file of affix rules and settings,
// a .dic file of stems with the flags that say which rules apply), read
// here from the files themselves — the system's under /usr/share/hunspell,
// or one the browser ships. A word is checked by finding a stem it is made
// from by at most a prefix and two suffixes (or by compounding where the
// language does), in the cases a word may be written in; suggestions are
// what the .aff says to try (REP, KEY, TRY), splits, and the dictionary's
// nearest words.
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::text {

class SpellDictionary {
public:
    // From the two files' bytes; nothing when the .dic has no words.
    static std::unique_ptr<SpellDictionary> load(std::string_view aff, std::string_view dic);
    ~SpellDictionary();
    SpellDictionary(SpellDictionary const&) = delete;
    SpellDictionary& operator=(SpellDictionary const&) = delete;

    // Whether the word is spelled right (a word is a run the caller cut out
    // by the Unicode word boundaries; numbers and words with digits pass).
    bool check(std::u32string_view word) const;
    // The likeliest spellings of a word that is not, best first.
    std::vector<std::u32string> suggest(std::u32string_view word, std::size_t limit = 6) const;
    // A word of the reader's own ("Add to dictionary"), taken as it is.
    void add_word(std::u32string const& word);
    // Characters the language counts inside words besides letters
    // (WORDCHARS: the apostrophe, a hyphen), which a word may hold.
    std::u32string const& word_characters() const;

    struct Impl;

private:
    SpellDictionary();
    std::unique_ptr<Impl> m_impl;
};

// Where this system keeps its Hunspell dictionaries, and the files of the
// one for a language tag ("en-US" finds en_US.aff and en_US.dic, else any
// en_*); nothing when there is none.
struct DictionaryFiles {
    std::string aff;
    std::string dic;
    std::string language; // the file's own name for it ("en_US")
};
std::optional<DictionaryFiles> find_dictionary(std::string_view language, std::vector<std::string> const& folders);
std::vector<std::string> system_dictionary_folders();

}
