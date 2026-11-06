#include "Test.h"

#include "ui/SpellService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace sashfold;

namespace {

template <typename Ready>
bool eventually(Ready&& ready)
{
    auto const give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < give_up) {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
}

void write(std::filesystem::path const& path, std::string const& text)
{
    std::ofstream(path, std::ios::binary) << text;
}

std::string read(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}

// The languages the folders have dictionaries for, one chosen in the menu
// read in the background and kept (with whether to check at all) in the
// profile's settings file, which the next start reads.
void test_languages_and_settings()
{
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path const root = fs::absolute("spell-service-scratch", error);
    fs::remove_all(root, error);
    fs::create_directories(root / "dicts", error);
    write(root / "dicts" / "en_TS.aff", "SET UTF-8\n");
    write(root / "dicts" / "en_TS.dic", "2\nhello\nworld\n");
    write(root / "dicts" / "fr_TS.aff", "SET UTF-8\n");
    write(root / "dicts" / "fr_TS.dic", "2\nbonjour\nmonde\n");
    write(root / "dicts" / "hyph_en_TS.dic", "not a dictionary\n");
    write(root / "dicts" / "lonely.dic", "1\nnone\n"); // no .aff beside it
    std::string const settings = (root / "spelling-settings.txt").string();
    {
        ui::SpellService spell("en_TS", { (root / "dicts").string() }, "", settings);
        CHECK(eventually([&] { return spell.ready(); }));
        CHECK(spell.languages() == (std::vector<std::string> { "en_TS", "fr_TS" }));
        CHECK_EQ(spell.language(), std::string("en_TS"));
        CHECK(spell.check(U"hello"));
        CHECK(!spell.check(U"bonjour"));
        std::uint64_t const first = spell.version();
        spell.set_language("fr_TS");
        CHECK(eventually([&] { return spell.version() != first; }));
        CHECK_EQ(spell.language(), std::string("fr_TS"));
        CHECK(spell.check(U"bonjour"));
        CHECK(!spell.check(U"hello"));
        CHECK_EQ(spell.misspelled(U"bonjour hello").size(), 1u);
        // Turned off: nothing is misspelled; the dictionary still suggests.
        spell.set_enabled(false);
        CHECK(spell.misspelled(U"bonjour hello").empty());
    }
    CHECK_EQ(read(settings), std::string("language fr_TS\nchecking off\n"));
    // The next start begins where the reader left it.
    {
        ui::SpellService spell("en_TS", { (root / "dicts").string() }, "", settings);
        CHECK(eventually([&] { return spell.ready(); }));
        CHECK_EQ(spell.language(), std::string("fr_TS"));
        CHECK(!spell.enabled());
        spell.set_enabled(true);
        CHECK_EQ(spell.misspelled(U"bonjour hello").size(), 1u);
    }
    CHECK_EQ(read(settings), std::string("language fr_TS\n"));
    // A language with no dictionary leaves the one there was.
    {
        ui::SpellService spell("en_TS", { (root / "dicts").string() }, "", "");
        CHECK(eventually([&] { return spell.ready(); }));
        std::uint64_t const before = spell.version();
        spell.set_language("xx_NONE");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_EQ(spell.version(), before);
        CHECK(spell.check(U"hello"));
        CHECK_EQ(spell.language(), std::string("en_TS"));
    }
    fs::remove_all(root, error);
}

int main()
{
    test_languages_and_settings();
    return sashfold::test::report("spell_service");
}
