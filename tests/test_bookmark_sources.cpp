#include "Test.h"

#include "ui/BookmarkSources.h"
#include "ui/Bookmarks.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Where bookmarks come in from on this machine: Firefox's compressed copies,
// the file Chrome and its kin keep, bookmarks files lying in the usual
// folders — found under a home folder made here — and the dated copies kept
// of the reader's own.

using namespace sashfold;
using namespace sashfold::ui;

namespace {

namespace fs = std::filesystem;

std::string titles_of(BookmarkNode const& folder)
{
    std::string out;
    for (BookmarkNode const& child : folder.children) {
        if (!out.empty())
            out += " ";
        out += child.folder ? "[" + child.title + ": " + titles_of(child) + "]" : child.title;
    }
    return out;
}

std::string le32(std::size_t value)
{
    std::string out;
    for (int i = 0; i < 4; ++i)
        out += static_cast<char>((value >> (8 * i)) & 0xff);
    return out;
}

// Firefox's container around one LZ4 block of literals alone.
std::string mozlz4_of(std::string const& text)
{
    std::string out("mozLz40\0", 8);
    out += le32(text.size());
    if (text.size() < 15) {
        out += static_cast<char>(text.size() << 4);
    } else {
        out += static_cast<char>(0xF0);
        std::size_t rest = text.size() - 15;
        while (rest >= 255) {
            out += static_cast<char>(0xFF);
            rest -= 255;
        }
        out += static_cast<char>(rest);
    }
    return out + text;
}

std::optional<std::string> decompressed(std::string const& bytes, std::size_t most = 1u << 20)
{
    return mozlz4_decompress(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(bytes.data()), bytes.size()), most);
}

void write(fs::path const& path, std::string const& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string const netscape = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<TITLE>Bookmarks</TITLE>\n<DL><p>\n"
                             "<DT><H3 PERSONAL_TOOLBAR_FOLDER=\"true\">Bar</H3>\n<DL><p>\n"
                             "<DT><A HREF=\"https://one.example/\">One</A>\n</DL><p>\n"
                             "<DT><A HREF=\"https://two.example/\">Two</A>\n</DL><p>\n";

std::string const chromium
    = R"({"checksum":"0","roots":{"bookmark_bar":{"children":[{"date_added":"13350000000000000","name":"Git","type":"url","url":"https://git.example/"},)"
      R"({"children":[{"name":"Inner","type":"url","url":"https://inner.example/"}],"name":"Tools","type":"folder"}],"name":"Bookmarks bar","type":"folder"},)"
      R"("other":{"children":[{"name":"Ref","type":"url","url":"https://ref.example/"}],"name":"Other bookmarks","type":"folder"},)"
      R"("synced":{"children":[],"name":"Mobile bookmarks","type":"folder"}},"version":1})";

std::string const firefox
    = R"({"guid":"root________","title":"","type":"text/x-moz-place-container","root":"placesRoot","children":[)"
      R"({"guid":"menu________","title":"menu","type":"text/x-moz-place-container","root":"bookmarksMenuFolder","children":[)"
      R"({"title":"Moz","type":"text/x-moz-place","uri":"https://moz.example/","dateAdded":1700000000000000},)"
      R"({"title":"Recent tags","type":"text/x-moz-place","uri":"place:type=6&sort=14"}]},)"
      R"({"guid":"toolbar_____","title":"toolbar","type":"text/x-moz-place-container","root":"toolbarFolder","children":[)"
      R"({"title":"Reading","type":"text/x-moz-place-container","children":[{"title":"Lwn","type":"text/x-moz-place","uri":"https://lwn.example/"}]},)"
      R"({"title":"Phx","type":"text/x-moz-place","uri":"https://phx.example/"}]}]})";

}

int main()
{
    // Firefox's container: a size and one LZ4 block.
    {
        CHECK(decompressed(mozlz4_of("hello")) == std::optional<std::string>("hello"));
        // Fifteen literals and more spell their count in further bytes.
        std::string const long_text(700, 'q');
        CHECK(decompressed(mozlz4_of(long_text)) == std::optional<std::string>(long_text));
        // A match copies from what is already out — and may run into what it
        // is writing: "abc", then nine bytes from three back, then "X".
        std::string matched("mozLz40\0", 8);
        matched += le32(13);
        matched += std::string("\x35" "abc" "\x03\x00" "\x10" "X", 8);
        CHECK(decompressed(matched) == std::optional<std::string>("abcabcabcabcX"));
        // A match of nineteen bytes and more spells its length in further bytes: 4 + 15 + 3.
        std::string longer("mozLz40\0", 8);
        longer += le32(1 + 22 + 1);
        longer += std::string("\x1F" "z" "\x01\x00" "\x03" "\x10" "!", 7);
        CHECK(decompressed(longer) == std::optional<std::string>(std::string(23, 'z') + "!"));

        // What is not that container, or lies about itself, is nothing.
        std::string bad_magic = mozlz4_of("hello");
        bad_magic[6] = '1';
        CHECK(!decompressed(bad_magic));
        CHECK(!decompressed(std::string("moz", 3)));
        std::string short_of = mozlz4_of("hello");
        short_of[8] = 6; // says six bytes, holds five
        CHECK(!decompressed(short_of));
        std::string truncated = mozlz4_of("hello");
        truncated.pop_back();
        CHECK(!decompressed(truncated));
        std::string zero_offset("mozLz40\0", 8);
        zero_offset += le32(8);
        zero_offset += std::string("\x10" "a" "\x00\x00" "\x10" "b", 6);
        CHECK(!decompressed(zero_offset));
        std::string far_offset("mozLz40\0", 8);
        far_offset += le32(8);
        far_offset += std::string("\x10" "a" "\x09\x00" "\x10" "b", 6);
        CHECK(!decompressed(far_offset));
        // A match that would write past the size it gave.
        std::string overrun("mozLz40\0", 8);
        overrun += le32(4);
        overrun += std::string("\x1F" "a" "\x01\x00" "\xFF\x00", 6);
        CHECK(!decompressed(overrun));
        // Larger than the caller will take.
        CHECK(!decompressed(mozlz4_of(long_text), 100));
    }

    // Chrome's file: its bar onto the bar, everything else into the other
    // bookmarks, folders kept, its dates — microseconds since 1601 — read.
    {
        Bookmarks b;
        CHECK_EQ(import_chromium_bookmarks(chromium, b), std::size_t { 3 });
        CHECK_EQ(titles_of(b.bar()), std::string("Git [Tools: Inner]"));
        CHECK_EQ(titles_of(b.other()), std::string("Ref"));
        BookmarkNode const* const git = b.find_url("https://git.example/");
        CHECK(git != nullptr);
        if (git)
            CHECK_EQ(git->added, std::int64_t { 1705526400 });
        Bookmarks none;
        CHECK_EQ(import_chromium_bookmarks("{\"roots\": 5}", none), std::size_t { 0 });
        CHECK_EQ(import_chromium_bookmarks("not json", none), std::size_t { 0 });
        CHECK_EQ(none.count(), std::size_t { 0 });
    }

    // Firefox's copy: its toolbar onto the bar, its menu into the other
    // bookmarks; a query it runs in a bookmark's place is no bookmark.
    {
        Bookmarks b;
        CHECK_EQ(import_firefox_backup(firefox, b), std::size_t { 3 });
        CHECK_EQ(titles_of(b.bar()), std::string("[Reading: Lwn] Phx"));
        CHECK_EQ(titles_of(b.other()), std::string("Moz"));
        BookmarkNode const* const moz = b.find_url("https://moz.example/");
        CHECK(moz != nullptr);
        if (moz)
            CHECK_EQ(moz->added, std::int64_t { 1700000000 });
    }

    fs::path const root = fs::temp_directory_path() / "sashfold-test-bookmark-sources";
    std::error_code error;
    fs::remove_all(root, error);

    // A file of any of the three kinds is read for what it is; what is none
    // of them, or is not there, is said to be so.
    {
        write(root / "kinds" / "a.jsonlz4", mozlz4_of(firefox));
        write(root / "kinds" / "Bookmarks", chromium);
        write(root / "kinds" / "b.html", netscape);
        write(root / "kinds" / "plain.json", firefox);
        write(root / "kinds" / "other.txt", "{\"hello\": 1}");
        write(root / "kinds" / "broken.jsonlz4", std::string("mozLz40\0\xff\xff\xff\x7f", 12));
        Bookmarks b;
        CHECK(import_bookmark_source((root / "kinds" / "a.jsonlz4").string(), b) == std::optional<std::size_t>(3));
        CHECK(import_bookmark_source((root / "kinds" / "Bookmarks").string(), b) == std::optional<std::size_t>(3));
        CHECK(import_bookmark_source((root / "kinds" / "b.html").string(), b) == std::optional<std::size_t>(2));
        CHECK(import_bookmark_source((root / "kinds" / "plain.json").string(), b) == std::optional<std::size_t>(3));
        CHECK_EQ(b.count(), std::size_t { 11 });
        CHECK(!import_bookmark_source((root / "kinds" / "other.txt").string(), b));
        CHECK(!import_bookmark_source((root / "kinds" / "broken.jsonlz4").string(), b));
        CHECK(!import_bookmark_source((root / "kinds" / "not-there.html").string(), b));
        CHECK_EQ(b.count(), std::size_t { 11 });
    }

    // What is found under a home folder: the browsers first — of Firefox's
    // copies the newest — then the bookmarks files of the usual folders; a
    // page that is no bookmarks file, and a file with nothing in it, are not.
    {
        fs::path const home = root / "home";
        write(home / ".mozilla/firefox/x1.default/bookmarkbackups/bookmarks-2025-12-30_9_old.jsonlz4", mozlz4_of("{\"children\": []}"));
        write(home / ".mozilla/firefox/x1.default/bookmarkbackups/bookmarks-2026-01-02_3_new.jsonlz4", mozlz4_of(firefox));
        write(home / ".mozilla/firefox/profiles.ini", "[General]\n");
        write(home / ".config/chromium/Default/Bookmarks", chromium);
        write(home / ".config/chromium/Empty Profile/Preferences", "{}");
        write(home / "Downloads/saved.html", netscape);
        write(home / "Downloads/notes.html", "<!doctype html><title>Notes</title><p>words");
        write(home / "Desktop/nothing.html", "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<DL><p>\n</DL><p>\n");
        write(home / "Documents/UPPER.HTM", netscape);
        std::vector<BookmarkSource> const found = find_bookmark_sources(home.string(), (home / "Downloads").string());
        CHECK_EQ(found.size(), std::size_t { 4 });
        if (found.size() == 4) {
            CHECK_EQ(found[0].browser, std::string("Firefox"));
            CHECK_EQ(found[0].detail, std::string("x1.default"));
            CHECK_EQ(found[0].when, std::string("2026-01-02"));
            CHECK_EQ(found[0].count, std::size_t { 3 });
            CHECK(found[0].key.ends_with("bookmarks-2026-01-02_3_new.jsonlz4"));
            CHECK_EQ(found[1].browser, std::string("Chromium"));
            CHECK_EQ(found[1].detail, std::string("Default"));
            CHECK_EQ(found[1].count, std::size_t { 3 });
            CHECK_EQ(found[2].browser, std::string());
            CHECK_EQ(found[2].detail, std::string("saved.html in Downloads"));
            CHECK_EQ(found[2].count, std::size_t { 2 });
            CHECK_EQ(found[3].detail, std::string("UPPER.HTM in Documents"));
        }
        CHECK(find_bookmark_sources("", "").empty());
        CHECK(find_bookmark_sources((root / "no-such-home").string(), "").empty());
    }

    // The dated copies: one a day, the last ten kept, newest first; the copy
    // kept before a restore is numbered within its day and is not among the
    // ten; and a copy is read by its name only.
    {
        std::string const folder = (root / "copies").string();
        Bookmarks b;
        b.add_bookmark(Bookmarks::bar_id, "One", "https://one.example/", 0);
        std::string const one = b.to_json();
        b.add_bookmark(Bookmarks::bar_id, "Two", "https://two.example/", 0);
        std::string const two = b.to_json();

        CHECK(list_bookmark_backups(folder).empty());
        CHECK(keep_bookmark_backup(folder, one, "2026-01-01"));
        // The day's copy is kept already: the second is not written over it.
        CHECK(!keep_bookmark_backup(folder, two, "2026-01-01"));
        CHECK(read_bookmark_backup(folder, "bookmarks-2026-01-01.json") == std::optional<std::string>(one));
        CHECK(!keep_bookmark_backup(folder, one, "today"));
        CHECK(!keep_bookmark_backup("", one, "2026-01-01"));
        for (int day = 2; day <= 12; ++day)
            CHECK(keep_bookmark_backup(folder, two, "2026-01-" + std::string(day < 10 ? "0" : "") + std::to_string(day)));
        std::vector<BookmarkBackup> const kept = list_bookmark_backups(folder);
        CHECK_EQ(kept.size(), std::size_t { 10 });
        if (kept.size() == 10) {
            CHECK_EQ(kept.front().key, std::string("bookmarks-2026-01-12.json"));
            CHECK_EQ(kept.front().when, std::string("2026-01-12"));
            CHECK_EQ(kept.front().count, std::size_t { 2 });
            CHECK_EQ(kept.back().key, std::string("bookmarks-2026-01-03.json"));
        }

        CHECK(keep_bookmarks_before_restore(folder, one, "2026-01-12") == std::optional<std::string>("bookmarks-2026-01-12-before-restore-1.json"));
        CHECK(keep_bookmarks_before_restore(folder, two, "2026-01-12") == std::optional<std::string>("bookmarks-2026-01-12-before-restore-2.json"));
        CHECK(read_bookmark_backup(folder, "bookmarks-2026-01-12-before-restore-1.json") == std::optional<std::string>(one));
        std::vector<BookmarkBackup> const with_those = list_bookmark_backups(folder);
        CHECK_EQ(with_those.size(), std::size_t { 12 });
        bool said = false;
        for (BookmarkBackup const& backup : with_those)
            said = said || backup.when == "2026-01-12 (as they stood before a restore)";
        CHECK(said);
        // A day's copy more: the ten dated ones are still ten, and the two
        // kept before a restore are still there.
        CHECK(keep_bookmark_backup(folder, two, "2026-01-13"));
        CHECK_EQ(list_bookmark_backups(folder).size(), std::size_t { 12 });
        CHECK(!read_bookmark_backup(folder, "bookmarks-2026-01-03.json"));
        // Nine to a day, the ninth written over after that.
        for (int i = 3; i <= 11; ++i)
            CHECK(keep_bookmarks_before_restore(folder, i == 11 ? one : two, "2026-01-12").has_value());
        CHECK(read_bookmark_backup(folder, "bookmarks-2026-01-12-before-restore-9.json") == std::optional<std::string>(one));
        CHECK(!read_bookmark_backup(folder, "bookmarks-2026-01-12-before-restore-10.json"));
        // Ten of them in all: the eleventh drops the oldest.
        CHECK(keep_bookmarks_before_restore(folder, two, "2026-01-13").has_value());
        CHECK(read_bookmark_backup(folder, "bookmarks-2026-01-12-before-restore-1.json").has_value());
        CHECK(keep_bookmarks_before_restore(folder, two, "2026-01-13").has_value());
        CHECK(!read_bookmark_backup(folder, "bookmarks-2026-01-12-before-restore-1.json"));
        CHECK(read_bookmark_backup(folder, "bookmarks-2026-01-12-before-restore-2.json").has_value());
        CHECK_EQ(list_bookmark_backups(folder).size(), std::size_t { 20 });

        // Only a name of the folder's own.
        write(root / "outside.json", one);
        CHECK(!read_bookmark_backup(folder, "../outside.json"));
        CHECK(!read_bookmark_backup(folder, "bookmarks-/../../outside.json"));
        CHECK(!read_bookmark_backup(folder, "bookmarks-2026-01-13.txt"));
        CHECK(!read_bookmark_backup(folder, "other-2026-01-13.json"));
        CHECK(!read_bookmark_backup("", "bookmarks-2026-01-13.json"));
        // What is not a bookmarks file is not listed as a copy.
        write(fs::path(folder) / "bookmarks-2026-01-14.json", "not json");
        CHECK_EQ(list_bookmark_backups(folder).size(), std::size_t { 20 });
    }

    fs::remove_all(root, error);
    return test::report("bookmark sources");
}
