#pragma once
// Spelling for the window: the dictionary of the reader's language, read
// on a thread of its own at start (the system's Hunspell dictionaries, else
// the one the browser ships), the words the reader added (kept in the
// profile), and which words of a text are misspelled. Shared by every
// page's engine; safe to ask from any thread.
#include "text/Spell.h"

#include <atomic>
#include <cstdint>
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
    // of the reader's words (none: kept for the session alone); `settings`
    // the profile's file of what the reader chose in the menu — a language,
    // and whether to check at all — which overrides `language` (none: the
    // choice is the session's).
    SpellService(std::string language, std::vector<std::string> folders, std::string own_words, std::string settings = {});
    ~SpellService();
    SpellService(SpellService const&) = delete;
    SpellService& operator=(SpellService const&) = delete;

    // Whether a dictionary has been read (until then, and without one,
    // nothing is marked).
    bool ready() const { return m_dictionary.load() != nullptr; }
    // Counts the dictionaries put to use: a new one (a language chosen)
    // means every text is to be checked again.
    std::uint64_t version() const { return m_version.load(); }
    std::string language() const;
    // The languages there are dictionaries for in the folders, by the names
    // their files give ("de_DE", "en_US"), sorted; found in the background
    // at the start (empty until then).
    std::vector<std::string> languages() const;
    // The reader's choices, kept in the settings file: another language
    // (read in the background; until it has come, the one before stays), and
    // whether words are checked at all.
    void set_language(std::string language);
    bool enabled() const { return m_enabled.load(); }
    void set_enabled(bool enabled);
    // The words of `text` that are misspelled, [start, end) in code points:
    // the runs the Unicode word boundaries cut that hold a letter.
    std::vector<std::pair<std::size_t, std::size_t>> misspelled(std::u32string_view text);
    bool check(std::u32string_view word);
    std::vector<std::u32string> suggest(std::u32string_view word);
    void add_word(std::u32string const& word);
    // For a test: a dictionary given whole, read at once.
    void use_dictionary(std::unique_ptr<text::SpellDictionary> dictionary);

private:
    void load(std::string language, std::vector<std::string> folders, std::uint64_t generation);
    void save_settings();
    std::atomic<text::SpellDictionary const*> m_dictionary { nullptr };
    std::atomic<bool> m_enabled { true };
    std::atomic<std::uint64_t> m_version { 0 };
    std::vector<std::string> m_folders;
    std::string m_settings_file;
    std::vector<std::string> m_languages; // under m_mutex
    std::uint64_t m_generation = 0; // under m_mutex: the load that counts
    std::vector<std::unique_ptr<text::SpellDictionary>> m_retired; // under m_mutex: kept while a check may hold one
    std::vector<std::thread> m_workers; // under m_mutex
    std::unique_ptr<text::SpellDictionary> m_owned;
    std::string m_language;
    std::string m_own_words_file;
    mutable std::mutex m_mutex;
    std::unordered_set<std::u32string> m_own_words;
    std::unordered_map<std::u32string, bool> m_checked;
    std::thread m_loader;
};

}
