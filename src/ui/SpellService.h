#pragma once
// Spelling for the window: the dictionary of the reader's language, read
// on a thread of its own at start (the system's Hunspell dictionaries, else
// the one the browser ships), the words the reader added (kept in the
// profile), and which words of a text are misspelled. Shared by every
// page's engine; safe to ask from any thread.
#include "text/Spell.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::ui {

class SpellService {
public:
    // `language` as the system names it ("en_US", "de-DE"); `folders` where
    // dictionaries are looked for, in order; `own_words` the profile's file
    // of the reader's words (none: kept for the session alone).
    SpellService(std::string language, std::vector<std::string> folders, std::string own_words);
    ~SpellService();
    SpellService(SpellService const&) = delete;
    SpellService& operator=(SpellService const&) = delete;

    // Whether a dictionary has been read (until then, and without one,
    // nothing is marked).
    bool ready() const { return m_dictionary.load() != nullptr; }
    std::string language() const;
    // The words of `text` that are misspelled, [start, end) in code points:
    // the runs the Unicode word boundaries cut that hold a letter.
    std::vector<std::pair<std::size_t, std::size_t>> misspelled(std::u32string_view text);
    bool check(std::u32string_view word);
    std::vector<std::u32string> suggest(std::u32string_view word);
    void add_word(std::u32string const& word);
    // For a test: a dictionary given whole, read at once.
    void use_dictionary(std::unique_ptr<text::SpellDictionary> dictionary);

private:
    void load(std::string language, std::vector<std::string> folders);
    std::atomic<text::SpellDictionary const*> m_dictionary { nullptr };
    std::unique_ptr<text::SpellDictionary> m_owned;
    std::string m_language;
    std::string m_own_words_file;
    mutable std::mutex m_mutex;
    std::unordered_set<std::u32string> m_own_words;
    std::unordered_map<std::u32string, bool> m_checked;
    std::thread m_loader;
};

}
