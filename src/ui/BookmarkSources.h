#pragma once

// Where bookmarks can be brought in from on this machine, and the reading of
// them: another browser's own store — the JSON file Chrome and the browsers
// built on it keep, the compressed JSON copies Firefox keeps of its own — or
// a Netscape bookmarks file lying in one of the usual folders. Read only, on
// the reader's asking, and nothing of it leaves the machine.

#include "ui/Bookmarks.h"
#include "ui/BookmarksPage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

// What is found under a home folder, other browsers first; `downloads` is
// looked in for bookmarks files as the desktop, documents and home are.
std::vector<BookmarkSource> find_bookmark_sources(std::string const& home, std::string const& downloads);

// Adds what a source holds to the tree: a browser's toolbar onto the bar, the
// rest into the other bookmarks, replacing nothing. How many bookmarks came;
// nullopt when the file cannot be read as any of the three kinds.
std::optional<std::size_t> import_bookmark_source(std::string const& path, Bookmarks& into);

// The pieces, for whoever has the bytes already.
std::size_t import_chromium_bookmarks(std::string_view json, Bookmarks& into);
std::size_t import_firefox_backup(std::string_view json, Bookmarks& into);
// Firefox's "mozLz40" container: a size and one LZ4 block. nullopt for
// anything malformed or larger than `most`.
std::optional<std::string> mozlz4_decompress(std::span<std::uint8_t const> bytes, std::size_t most = 256u * 1024u * 1024u);

// The dated copies kept of the reader's own bookmarks in a folder of the
// profile, newest first; and the keeping of one: today's copy of `json`
// unless there is one already, the oldest dropped past ten. `today` is
// YYYY-MM-DD.
std::vector<BookmarkBackup> list_bookmark_backups(std::string const& directory);
bool keep_bookmark_backup(std::string const& directory, std::string_view json, std::string_view today);
// The copy kept of the bookmarks as they stand just before a dated copy is
// put back in their place, so that a restore can itself be undone: numbered
// within its day, the oldest dropped past ten. Its key; nullopt when it
// could not be written — and then nothing should be restored.
std::optional<std::string> keep_bookmarks_before_restore(std::string const& directory, std::string_view json, std::string_view today);
// A copy's text, by the key list_bookmark_backups gave it; nullopt for a key
// that names no copy there.
std::optional<std::string> read_bookmark_backup(std::string const& directory, std::string const& key);

}
