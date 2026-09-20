#include "ui/BookmarkSources.h"

#include "core/Json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace sashfold::ui {

namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t max_file_bytes = 64u * 1024u * 1024u;
constexpr std::size_t max_depth = 64;

std::optional<std::string> read_file(fs::path const& path)
{
    std::error_code error;
    std::uintmax_t const size = fs::file_size(path, error);
    if (error || size > max_file_bytes)
        return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool looks_like_netscape_file(std::string_view text)
{
    std::string head(text.substr(0, 2048));
    for (char& c : head)
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return head.find("netscape-bookmark-file") != std::string::npos;
}

// Chrome's dates are microseconds since 1601, written as a string; Firefox's
// are microseconds since 1970, a number. Both to seconds since 1970.
std::int64_t chromium_seconds(JsonValue const* value)
{
    if (!value || !value->is_string())
        return 0;
    std::int64_t micros = 0;
    for (char const c : value->as_string()) {
        if (c < '0' || c > '9' || micros > 900000000000000000LL)
            return 0;
        micros = micros * 10 + (c - '0');
    }
    std::int64_t const seconds = micros / 1000000 - 11644473600LL;
    return seconds > 0 ? seconds : 0;
}

std::string string_of(JsonValue const& object, std::string_view key)
{
    JsonValue const* const value = object.get(key);
    return value && value->is_string() ? value->as_string() : std::string();
}

std::size_t add_chromium(JsonValue const& list, Bookmarks& into, std::uint64_t folder, std::size_t depth)
{
    if (!list.is_array() || depth > max_depth)
        return 0;
    std::size_t came = 0;
    for (JsonValue const& node : list.as_array()) {
        if (!node.is_object())
            continue;
        std::string const type = string_of(node, "type");
        if (type == "folder") {
            std::uint64_t const made = into.add_folder(folder, string_of(node, "name"));
            if (JsonValue const* const children = node.get("children"); made != 0 && children)
                came += add_chromium(*children, into, made, depth + 1);
        } else if (type == "url") {
            std::string const url = string_of(node, "url");
            if (!url.empty() && into.add_bookmark(folder, string_of(node, "name"), url, chromium_seconds(node.get("date_added"))) != 0)
                ++came;
        }
    }
    return came;
}

std::size_t add_firefox(JsonValue const& list, Bookmarks& into, std::uint64_t folder, std::size_t depth)
{
    if (!list.is_array() || depth > max_depth)
        return 0;
    std::size_t came = 0;
    for (JsonValue const& node : list.as_array()) {
        if (!node.is_object())
            continue;
        std::string const type = string_of(node, "type");
        if (type == "text/x-moz-place-container") {
            std::uint64_t const made = into.add_folder(folder, string_of(node, "title"));
            if (JsonValue const* const children = node.get("children"); made != 0 && children)
                came += add_firefox(*children, into, made, depth + 1);
        } else if (type == "text/x-moz-place") {
            std::string const uri = string_of(node, "uri");
            // A query that browser runs in place of a bookmark is not one here.
            if (uri.empty() || uri.starts_with("place:"))
                continue;
            JsonValue const* const added = node.get("dateAdded");
            std::int64_t const seconds = added && added->is_number() && added->as_number() > 0
                ? static_cast<std::int64_t>(added->as_number() / 1000000.0)
                : 0;
            if (into.add_bookmark(folder, string_of(node, "title"), uri, seconds) != 0)
                ++came;
        }
    }
    return came;
}

// How many bookmarks a source holds, by bringing it into a tree of its own.
std::optional<std::size_t> count_in(fs::path const& path)
{
    Bookmarks scratch;
    return import_bookmark_source(path.string(), scratch);
}

std::string file_name_date(std::string const& name)
{
    // bookmarks-2026-09-18_1204_….jsonlz4, bookmarks-2026-09-18.json
    for (std::size_t i = 0; i + 10 <= name.size(); ++i) {
        auto const digit = [&](std::size_t at) { return name[at] >= '0' && name[at] <= '9'; };
        if (digit(i) && digit(i + 1) && digit(i + 2) && digit(i + 3) && name[i + 4] == '-' && digit(i + 5) && digit(i + 6) && name[i + 7] == '-'
            && digit(i + 8) && digit(i + 9))
            return name.substr(i, 10);
    }
    return {};
}

std::vector<fs::path> entries_of(fs::path const& directory)
{
    std::vector<fs::path> out;
    std::error_code error;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error);
    if (error)
        return out;
    for (fs::directory_iterator const end; it != end && out.size() < 4096; it.increment(error)) {
        if (error)
            break;
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct Family {
    char const* browser;
    char const* folder; // under the home folder
    bool firefox; // profiles of backups, not of a Bookmarks file
};

constexpr Family families[] = {
    { "Firefox", ".mozilla/firefox", true },
    { "Firefox", "snap/firefox/common/.mozilla/firefox", true },
    { "Firefox", ".var/app/org.mozilla.firefox/.mozilla/firefox", true },
    { "LibreWolf", ".librewolf", true },
    { "Chrome", ".config/google-chrome", false },
    { "Chrome", ".var/app/com.google.Chrome/config/google-chrome", false },
    { "Chromium", ".config/chromium", false },
    { "Chromium", ".var/app/org.chromium.Chromium/config/chromium", false },
    { "Brave", ".config/BraveSoftware/Brave-Browser", false },
    { "Brave", ".var/app/com.brave.Browser/config/BraveSoftware/Brave-Browser", false },
    { "Edge", ".config/microsoft-edge", false },
    { "Vivaldi", ".config/vivaldi", false },
    { "Opera", ".config/opera", false },
    // Windows and macOS keep them elsewhere under the same home.
    { "Firefox", "AppData/Roaming/Mozilla/Firefox/Profiles", true },
    { "Chrome", "AppData/Local/Google/Chrome/User Data", false },
    { "Edge", "AppData/Local/Microsoft/Edge/User Data", false },
    { "Brave", "AppData/Local/BraveSoftware/Brave-Browser/User Data", false },
    { "Firefox", "Library/Application Support/Firefox/Profiles", true },
    { "Chrome", "Library/Application Support/Google/Chrome", false },
    { "Edge", "Library/Application Support/Microsoft Edge", false },
    { "Brave", "Library/Application Support/BraveSoftware/Brave-Browser", false },
};

}

std::optional<std::string> mozlz4_decompress(std::span<std::uint8_t const> bytes, std::size_t most)
{
    constexpr std::string_view magic("mozLz40\0", 8);
    if (bytes.size() < 12 || std::string_view(reinterpret_cast<char const*>(bytes.data()), 8) != magic)
        return std::nullopt;
    std::size_t const size = static_cast<std::size_t>(bytes[8]) | static_cast<std::size_t>(bytes[9]) << 8
        | static_cast<std::size_t>(bytes[10]) << 16 | static_cast<std::size_t>(bytes[11]) << 24;
    if (size > most)
        return std::nullopt;
    std::string out;
    out.reserve(size);
    std::size_t at = 12;
    // One LZ4 block: sequences of literals and a match copied from what is
    // already out; the last sequence is literals alone.
    while (at < bytes.size()) {
        std::uint8_t const token = bytes[at++];
        std::size_t literals = token >> 4;
        if (literals == 15) {
            std::uint8_t more = 255;
            while (more == 255) {
                if (at >= bytes.size())
                    return std::nullopt;
                more = bytes[at++];
                literals += more;
            }
        }
        if (literals > bytes.size() - at || out.size() + literals > size)
            return std::nullopt;
        out.append(reinterpret_cast<char const*>(bytes.data() + at), literals);
        at += literals;
        if (at >= bytes.size())
            break;
        if (at + 2 > bytes.size())
            return std::nullopt;
        std::size_t const offset = static_cast<std::size_t>(bytes[at]) | static_cast<std::size_t>(bytes[at + 1]) << 8;
        at += 2;
        std::size_t length = (token & 15u) + 4u;
        if ((token & 15u) == 15u) {
            std::uint8_t more = 255;
            while (more == 255) {
                if (at >= bytes.size())
                    return std::nullopt;
                more = bytes[at++];
                length += more;
            }
        }
        if (offset == 0 || offset > out.size() || out.size() + length > size)
            return std::nullopt;
        // Byte by byte: a match may run into what it is writing.
        std::size_t from = out.size() - offset;
        for (std::size_t i = 0; i < length; ++i)
            out.push_back(out[from++]);
    }
    if (out.size() != size)
        return std::nullopt;
    return out;
}

std::size_t import_chromium_bookmarks(std::string_view json, Bookmarks& into)
{
    std::optional<JsonValue> const parsed = JsonValue::parse(json);
    JsonValue const* const roots = parsed ? parsed->get("roots") : nullptr;
    if (!roots || !roots->is_object())
        return 0;
    std::size_t came = 0;
    for (auto const& [name, root] : roots->as_object()) {
        if (!root.is_object())
            continue;
        JsonValue const* const children = root.get("children");
        if (!children)
            continue;
        came += add_chromium(*children, into, name == "bookmark_bar" ? Bookmarks::bar_id : Bookmarks::other_id, 0);
    }
    return came;
}

std::size_t import_firefox_backup(std::string_view json, Bookmarks& into)
{
    std::optional<JsonValue> const parsed = JsonValue::parse(json);
    JsonValue const* const roots = parsed ? parsed->get("children") : nullptr;
    if (!roots || !roots->is_array())
        return 0;
    std::size_t came = 0;
    for (JsonValue const& root : roots->as_array()) {
        if (!root.is_object())
            continue;
        JsonValue const* const children = root.get("children");
        if (!children)
            continue;
        std::string const guid = string_of(root, "guid");
        std::string const name = string_of(root, "root");
        bool const toolbar = guid == "toolbar_____" || name == "toolbarFolder";
        came += add_firefox(*children, into, toolbar ? Bookmarks::bar_id : Bookmarks::other_id, 0);
    }
    return came;
}

std::optional<std::size_t> import_bookmark_source(std::string const& path, Bookmarks& into)
{
    std::optional<std::string> const text = read_file(path);
    if (!text)
        return std::nullopt;
    if (text->starts_with("mozLz40")) {
        std::optional<std::string> const json
            = mozlz4_decompress(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text->data()), text->size()));
        if (!json)
            return std::nullopt;
        return import_firefox_backup(*json, into);
    }
    if (looks_like_netscape_file(*text))
        return into.import_netscape_html(*text);
    std::optional<JsonValue> const parsed = JsonValue::parse(*text);
    if (!parsed || !parsed->is_object())
        return std::nullopt;
    if (parsed->get("roots"))
        return import_chromium_bookmarks(*text, into);
    if (parsed->get("children"))
        return import_firefox_backup(*text, into);
    return std::nullopt;
}

std::vector<BookmarkSource> find_bookmark_sources(std::string const& home, std::string const& downloads)
{
    std::vector<BookmarkSource> sources;
    if (home.empty())
        return sources;
    std::error_code error;
    for (Family const& family : families) {
        fs::path const base = fs::path(home) / family.folder;
        if (!fs::is_directory(base, error))
            continue;
        for (fs::path const& profile : entries_of(base)) {
            if (!fs::is_directory(profile, error))
                continue;
            fs::path file;
            std::string when;
            if (family.firefox) {
                // The newest of the copies that browser keeps of its own bookmarks.
                std::vector<fs::path> const copies = entries_of(profile / "bookmarkbackups");
                for (auto it = copies.rbegin(); it != copies.rend() && file.empty(); ++it) {
                    if (it->extension() == ".jsonlz4" || it->extension() == ".json")
                        file = *it;
                }
                if (!file.empty())
                    when = file_name_date(file.filename().string());
            } else if (fs::is_regular_file(profile / "Bookmarks", error)) {
                file = profile / "Bookmarks";
            }
            if (file.empty())
                continue;
            std::optional<std::size_t> const count = count_in(file);
            if (!count || *count == 0)
                continue;
            sources.push_back(BookmarkSource { file.string(), family.browser, profile.filename().string(), when, *count });
        }
    }
    // Bookmarks files, where a reader would have put one.
    std::vector<fs::path> folders;
    if (!downloads.empty())
        folders.emplace_back(downloads);
    for (char const* name : { "Downloads", "Desktop", "Documents", "" })
        folders.push_back(*name ? fs::path(home) / name : fs::path(home));
    std::vector<std::string> seen;
    for (fs::path const& folder : folders) {
        for (fs::path const& entry : entries_of(folder)) {
            std::string extension = entry.extension().string();
            for (char& c : extension)
                c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            if ((extension != ".html" && extension != ".htm") || !fs::is_regular_file(entry, error))
                continue;
            std::string const key = entry.string();
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                continue;
            seen.push_back(key);
            std::ifstream file(entry, std::ios::binary);
            char head[2048] = {};
            file.read(head, sizeof head);
            if (!looks_like_netscape_file(std::string_view(head, static_cast<std::size_t>(file.gcount()))))
                continue;
            std::optional<std::size_t> const count = count_in(entry);
            if (!count || *count == 0)
                continue;
            sources.push_back(BookmarkSource { key, "", entry.filename().string() + " in " + entry.parent_path().filename().string(), "", *count });
        }
    }
    return sources;
}

std::vector<BookmarkBackup> list_bookmark_backups(std::string const& directory)
{
    std::vector<BookmarkBackup> backups;
    if (directory.empty())
        return backups;
    std::vector<fs::path> const files = entries_of(directory);
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        std::string const name = it->filename().string();
        if (!name.starts_with("bookmarks-") || it->extension() != ".json")
            continue;
        std::optional<std::string> const text = read_file(*it);
        std::optional<Bookmarks> const held = text ? Bookmarks::from_json(*text) : std::nullopt;
        if (!held)
            continue;
        std::string when = file_name_date(name);
        if (name.find("before-restore") != std::string::npos)
            when += " (as they stood before a restore)";
        backups.push_back(BookmarkBackup { name, when, held->count() });
    }
    return backups;
}

bool keep_bookmark_backup(std::string const& directory, std::string_view json, std::string_view today)
{
    if (directory.empty() || today.size() != 10)
        return false;
    std::error_code error;
    fs::create_directories(directory, error);
    fs::path const file = fs::path(directory) / ("bookmarks-" + std::string(today) + ".json");
    if (fs::exists(file, error))
        return false; // today's is kept already
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
    }
    // The last ten days' copies stay; older ones go, by name, one at a time.
    std::vector<fs::path> dated;
    for (fs::path const& entry : entries_of(directory)) {
        std::string const name = entry.filename().string();
        if (name.starts_with("bookmarks-") && entry.extension() == ".json" && name.find("before-restore") == std::string::npos)
            dated.push_back(entry);
    }
    while (dated.size() > 10) {
        fs::remove(dated.front(), error);
        dated.erase(dated.begin());
    }
    return true;
}

std::optional<std::string> keep_bookmarks_before_restore(std::string const& directory, std::string_view json, std::string_view today)
{
    if (directory.empty() || today.size() != 10)
        return std::nullopt;
    std::error_code error;
    fs::create_directories(directory, error);
    std::string const stem = "bookmarks-" + std::string(today) + "-before-restore-";
    std::vector<fs::path> kept;
    int todays = 0;
    for (fs::path const& entry : entries_of(directory)) {
        std::string const name = entry.filename().string();
        if (!name.starts_with("bookmarks-") || entry.extension() != ".json" || name.find("before-restore") == std::string::npos)
            continue;
        kept.push_back(entry);
        if (name.starts_with(stem))
            ++todays;
    }
    // Nine to a day: a tenth takes the ninth's place.
    std::string const name = stem + std::to_string(std::min(todays + 1, 9)) + ".json";
    {
        std::ofstream out(fs::path(directory) / name, std::ios::binary | std::ios::trunc);
        if (!out)
            return std::nullopt;
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out.flush())
            return std::nullopt;
    }
    if (todays < 9)
        kept.push_back(fs::path(directory) / name);
    std::sort(kept.begin(), kept.end());
    while (kept.size() > 10) {
        fs::remove(kept.front(), error);
        kept.erase(kept.begin());
    }
    return name;
}

std::optional<std::string> read_bookmark_backup(std::string const& directory, std::string const& key)
{
    // Only a name list_bookmark_backups gives: no path, nothing outside the folder.
    if (directory.empty() || key.find('/') != std::string::npos || key.find('\\') != std::string::npos || !key.starts_with("bookmarks-")
        || !key.ends_with(".json"))
        return std::nullopt;
    return read_file(fs::path(directory) / key);
}

}
