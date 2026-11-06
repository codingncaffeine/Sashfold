#include "ui/SpellService.h"

#include "core/Unicode.h"
#include "core/WordBreak.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace sashfold::ui {

namespace {

std::string file_text(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// A run between word boundaries that is a word to check: one with a letter
// in it (not a number, a space, punctuation).
bool has_letter(std::u32string_view part)
{
    for (char32_t const c : part) {
        WordBreakClass const k = word_break_class(c);
        if (k == WordBreakClass::ALetter || k == WordBreakClass::Hebrew_Letter || k == WordBreakClass::Katakana)
            return true;
    }
    return false;
}

}

SpellService::SpellService(std::string language, std::vector<std::string> folders, std::string own_words, std::string settings)
    : m_folders(folders)
    , m_settings_file(std::move(settings))
    , m_language(language)
    , m_own_words_file(std::move(own_words))
{
    // What the reader chose before: `language <name>`, `checking off`.
    if (!m_settings_file.empty()) {
        std::string const text = file_text(m_settings_file);
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.starts_with("language ") && line.size() > 9)
                language = line.substr(9);
            else if (line == "checking off")
                m_enabled = false;
            start = end + 1;
        }
        m_language = language;
    }
    if (!m_own_words_file.empty()) {
        std::u32string const words = decode_utf8(file_text(m_own_words_file));
        std::size_t start = 0;
        while (start < words.size()) {
            std::size_t end = words.find(U'\n', start);
            if (end == std::u32string::npos)
                end = words.size();
            std::u32string word = words.substr(start, end - start);
            while (!word.empty() && (word.back() == U'\r' || word.back() == U' '))
                word.pop_back();
            if (!word.empty())
                m_own_words.insert(std::move(word));
            start = end + 1;
        }
    }
    if (!language.empty() && !folders.empty())
        m_loader = std::thread([this, language = std::move(language), folders = std::move(folders)] { load(language, folders, 0); });
}

SpellService::~SpellService()
{
    if (m_loader.joinable())
        m_loader.join();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        workers.swap(m_workers);
    }
    for (std::thread& worker : workers) {
        if (worker.joinable())
            worker.join();
    }
}

void SpellService::load(std::string language, std::vector<std::string> folders, std::uint64_t generation)
{
    // The languages the folders have, found once (at the start's load).
    if (generation == 0) {
        std::vector<std::string> found;
        std::error_code error;
        for (std::string const& folder : folders) {
            for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(folder, error)) {
                std::filesystem::path const path = entry.path();
                if (path.extension() != ".dic" || path.stem().string().starts_with("hyph"))
                    continue;
                std::filesystem::path aff = path;
                aff.replace_extension(".aff");
                if (!std::filesystem::exists(aff, error))
                    continue;
                std::string name = path.stem().string();
                if (std::find(found.begin(), found.end(), name) == found.end())
                    found.push_back(std::move(name));
            }
        }
        std::sort(found.begin(), found.end());
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_languages = std::move(found);
    }
    std::optional<text::DictionaryFiles> const files = text::find_dictionary(language, folders);
    if (!files)
        return;
    std::unique_ptr<text::SpellDictionary> dictionary = text::SpellDictionary::load(file_text(files->aff), file_text(files->dic));
    if (!dictionary)
        return;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        if (generation != m_generation)
            return; // the reader has chosen again since
        m_language = files->language;
    }
    use_dictionary(std::move(dictionary));
}

std::vector<std::string> SpellService::languages() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_languages;
}

void SpellService::set_language(std::string language)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    std::uint64_t const generation = ++m_generation;
    m_workers.emplace_back([this, language = std::move(language), generation] {
        load(language, m_folders, generation);
        save_settings();
    });
}

void SpellService::set_enabled(bool enabled)
{
    m_enabled = enabled;
    std::lock_guard<std::mutex> const lock(m_mutex);
    m_workers.emplace_back([this] { save_settings(); });
}

// The reader's choices to the settings file, from a thread of the service's.
void SpellService::save_settings()
{
    if (m_settings_file.empty())
        return;
    std::string text;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        text = "language " + m_language + "\n" + (m_enabled ? "" : "checking off\n");
    }
    std::ofstream file(m_settings_file, std::ios::binary | std::ios::trunc);
    file << text;
}

void SpellService::use_dictionary(std::unique_ptr<text::SpellDictionary> dictionary)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    // The one before may still be in a check on another thread (they read
    // it without the lock): it is kept, not freed, for the service's life.
    if (m_owned)
        m_retired.push_back(std::move(m_owned));
    m_owned = std::move(dictionary);
    ++m_version;
    m_checked.clear();
    m_dictionary.store(m_owned.get());
}

std::string SpellService::language() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_language;
}

bool SpellService::check(std::u32string_view word)
{
    text::SpellDictionary const* const dictionary = m_dictionary.load();
    if (!dictionary)
        return true;
    std::u32string const key(word);
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        if (m_own_words.contains(key))
            return true;
        if (auto const it = m_checked.find(key); it != m_checked.end())
            return it->second;
    }
    bool const right = dictionary->check(word);
    std::lock_guard<std::mutex> const lock(m_mutex);
    if (m_checked.size() > 50000)
        m_checked.clear();
    m_checked.emplace(key, right);
    return right;
}

std::vector<std::u32string> SpellService::suggest(std::u32string_view word)
{
    text::SpellDictionary const* const dictionary = m_dictionary.load();
    return dictionary ? dictionary->suggest(word) : std::vector<std::u32string> {};
}

void SpellService::add_word(std::u32string const& word)
{
    if (word.empty())
        return;
    std::lock_guard<std::mutex> const lock(m_mutex);
    if (!m_own_words.insert(word).second)
        return;
    m_checked.erase(word);
    if (!m_own_words_file.empty()) {
        std::ofstream file(m_own_words_file, std::ios::binary | std::ios::app);
        file << to_utf8(word) << "\n";
    }
}

std::vector<std::pair<std::size_t, std::size_t>> SpellService::misspelled(std::u32string_view text)
{
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (!ready() || !enabled() || text.empty())
        return out;
    std::vector<bool> const breaks = word_boundaries(text);
    std::size_t start = 0;
    for (std::size_t i = 1; i <= text.size(); ++i) {
        if (!breaks[i])
            continue;
        std::u32string_view const part = text.substr(start, i - start);
        if (has_letter(part) && !check(part))
            out.emplace_back(start, i);
        start = i;
    }
    return out;
}

}
