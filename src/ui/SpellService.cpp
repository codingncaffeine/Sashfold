#include "ui/SpellService.h"

#include "core/Unicode.h"
#include "core/WordBreak.h"

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

SpellService::SpellService(std::string language, std::vector<std::string> folders, std::string own_words)
    : m_language(language)
    , m_own_words_file(std::move(own_words))
{
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
        m_loader = std::thread([this, language = std::move(language), folders = std::move(folders)] { load(language, folders); });
}

SpellService::~SpellService()
{
    if (m_loader.joinable())
        m_loader.join();
}

void SpellService::load(std::string language, std::vector<std::string> folders)
{
    std::optional<text::DictionaryFiles> const files = text::find_dictionary(language, folders);
    if (!files)
        return;
    std::unique_ptr<text::SpellDictionary> dictionary = text::SpellDictionary::load(file_text(files->aff), file_text(files->dic));
    if (!dictionary)
        return;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_language = files->language;
    }
    use_dictionary(std::move(dictionary));
}

void SpellService::use_dictionary(std::unique_ptr<text::SpellDictionary> dictionary)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    m_owned = std::move(dictionary);
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
    if (!ready() || text.empty())
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
