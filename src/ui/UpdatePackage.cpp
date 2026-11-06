#include "ui/UpdatePackage.h"

#include "core/Inflate.h"
#include "crypto/Sha2.h"
#include "ui/UpdateCheck.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace sashfold::ui {

namespace {

// The systems and machines releases are made for, as their downloads are
// named.
#if defined(__linux__) && defined(__x86_64__)
constexpr std::string_view platform_suffix = "linux-x64";
constexpr std::string_view archive_extension = ".tar.gz";
#elif defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
constexpr std::string_view platform_suffix = "windows-x64";
constexpr std::string_view archive_extension = ".zip";
#else
constexpr std::string_view platform_suffix = "";
constexpr std::string_view archive_extension = "";
#endif

bool said(std::string* why_not, std::string reason)
{
    if (why_not != nullptr)
        *why_not = std::move(reason);
    return false;
}

int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A number as a tar header writes it: octal digits, ended by a NUL or a
// space. Nothing for anything else (the base-256 form is for sizes no
// release reaches).
std::optional<std::uint64_t> octal(std::span<std::uint8_t const> field)
{
    std::uint64_t value = 0;
    bool any = false;
    for (std::uint8_t const c : field) {
        if (c == 0 || c == ' ') {
            if (any)
                break;
            continue;
        }
        if (c < '0' || c > '7' || value > (std::uint64_t { 1 } << 40))
            return std::nullopt;
        value = value * 8 + static_cast<std::uint64_t>(c - '0');
        any = true;
    }
    return any ? std::optional<std::uint64_t>(value) : std::nullopt;
}

std::string text_of(std::span<std::uint8_t const> field)
{
    std::size_t length = 0;
    while (length < field.size() && field[length] != 0)
        ++length;
    return std::string(reinterpret_cast<char const*>(field.data()), length);
}

// A path that stays where it is put: relative, no empty, `.` or `..`
// part, no backslash, no control character.
bool stays_inside(std::string_view path)
{
    if (path.empty() || path.front() == '/')
        return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t const end = std::min(path.find('/', start), path.size());
        std::string_view const part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        start = end + 1;
    }
    for (char const c : path) {
        if (c == '\\' || static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

}

std::string release_folder_name(std::string_view version)
{
    if (platform_suffix.empty() || !parse_release_version(version))
        return {};
    return "sashfold-" + std::string(version) + "-" + std::string(platform_suffix);
}

std::string release_archive_name(std::string_view version)
{
    std::string const folder = release_folder_name(version);
    return folder.empty() ? folder : folder + std::string(archive_extension);
}

std::string release_file_address(std::string_view version, std::string_view name)
{
    return "https://github.com/codingncaffeine/Sashfold/releases/download/v" + std::string(version) + "/" + std::string(name);
}

std::optional<std::vector<ReleaseFile>> parse_checksums(std::string_view text)
{
    std::vector<ReleaseFile> files;
    while (!text.empty()) {
        std::size_t const end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;
        if (line.size() < 67 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
            return std::nullopt;
        ReleaseFile file;
        for (std::size_t i = 0; i < 32; ++i) {
            int const high = hex_digit(line[2 * i]);
            int const low = hex_digit(line[2 * i + 1]);
            if (high < 0 || low < 0)
                return std::nullopt;
            file.sha256[i] = static_cast<std::uint8_t>(high * 16 + low);
        }
        file.name = std::string(line.substr(66));
        files.push_back(std::move(file));
    }
    return files;
}

std::string release_refusal(std::string_view version, std::string_view running, std::span<std::uint8_t const> checksums,
    std::span<std::uint8_t const> signature, std::span<std::uint8_t const> archive, crypto::Ed25519PublicKey const& key)
{
    std::string const name = release_archive_name(version);
    if (name.empty())
        return "this build has no download that replaces it";
    if (!is_newer_release(version, running))
        return "release " + std::string(version) + " is not later than the " + std::string(running) + " that is running";
    // The signature first: nothing of the list is read before it is known
    // to be the project's.
    if (signature.size() != 64 || !crypto::ed25519_verify(key, checksums, signature))
        return "the list of checksums is not signed by the release key";
    std::optional<std::vector<ReleaseFile>> const files
        = parse_checksums(std::string_view(reinterpret_cast<char const*>(checksums.data()), checksums.size()));
    if (!files)
        return "the list of checksums cannot be read";
    // The name carries the version: a signed list of an older release names
    // other files, and cannot stand for this one.
    for (ReleaseFile const& file : *files) {
        if (file.name != name)
            continue;
        if (crypto::Sha256::hash(archive) != file.sha256)
            return name + " is not the file the release lists: its SHA-256 differs";
        return {};
    }
    return "the release does not list " + name;
}

std::optional<std::vector<ReleaseEntry>> unpack_release(std::span<std::uint8_t const> archive, std::string_view folder, std::string* why_not)
{
    auto const refuse = [why_not](std::string reason) {
        said(why_not, std::move(reason));
        return std::nullopt;
    };
    if (folder.empty() || !stays_inside(folder) || folder.find('/') != std::string_view::npos)
        return refuse("no folder is named for the release");
    std::optional<std::vector<std::uint8_t>> const tar
        = gzip_decompress(std::vector<std::uint8_t>(archive.begin(), archive.end()), 512u * 1024u * 1024u);
    if (!tar)
        return refuse("the archive is not a gzip stream that decodes");
    std::span<std::uint8_t const> const bytes(*tar);
    std::vector<ReleaseEntry> entries;
    std::string long_name;
    std::size_t at = 0;
    bool ended = false;
    while (at + 512 <= bytes.size()) {
        std::span<std::uint8_t const> const header = bytes.subspan(at, 512);
        at += 512;
        // Two blocks of zeros end an archive; one is enough to stop on.
        bool blank = true;
        for (std::uint8_t const byte : header)
            blank = blank && byte == 0;
        if (blank) {
            ended = true;
            break;
        }
        // The header's own sum, taken with its checksum field as spaces.
        std::optional<std::uint64_t> const stated = octal(header.subspan(148, 8));
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < 512; ++i)
            sum += i >= 148 && i < 156 ? std::uint64_t { ' ' } : header[i];
        if (!stated || *stated != sum)
            return refuse("the archive is not a tar file: a header's checksum is wrong");
        std::optional<std::uint64_t> const size = octal(header.subspan(124, 12));
        if (!size || *size > bytes.size() - at)
            return refuse("the archive is cut short");
        std::size_t const data_at = at;
        at += static_cast<std::size_t>((*size + 511) / 512 * 512);
        char const type = static_cast<char>(header[156]);
        std::span<std::uint8_t const> const data = bytes.subspan(data_at, static_cast<std::size_t>(*size));
        if (type == 'L') {
            // GNU tar's long name: this entry's data names the next one.
            long_name = text_of(data);
            continue;
        }
        std::string name = std::exchange(long_name, {});
        if (name.empty()) {
            name = text_of(header.subspan(0, 100));
            // The POSIX form splits a long name in two.
            if (std::memcmp(header.data() + 257, "ustar", 5) == 0) {
                if (std::string const prefix = text_of(header.subspan(345, 155)); !prefix.empty())
                    name = prefix + "/" + name;
            }
        }
        while (name.size() > 1 && name.back() == '/')
            name.pop_back();
        if (name.starts_with("./"))
            name.erase(0, 2);
        if (!stays_inside(name))
            return refuse("the archive holds a path that does not stay in its folder: " + name);
        bool const is_folder = type == '5';
        if (!is_folder && type != '0' && type != '\0')
            return refuse("the archive holds something that is neither a file nor a folder: " + name);
        if (name == folder) {
            if (!is_folder)
                return refuse("the archive's top entry is not a folder");
            continue;
        }
        if (!name.starts_with(std::string(folder) + "/"))
            return refuse("the archive holds a path outside " + std::string(folder) + ": " + name);
        if (is_folder)
            continue;
        ReleaseEntry entry;
        entry.path = name.substr(folder.size() + 1);
        entry.bytes.assign(data.begin(), data.end());
        std::optional<std::uint64_t> const mode = octal(header.subspan(100, 8));
        entry.executable = mode && (*mode & 0100) != 0;
        entries.push_back(std::move(entry));
    }
    if (!ended)
        return refuse("the archive is cut short");
    if (entries.empty())
        return refuse("the archive holds no file under " + std::string(folder));
    return entries;
}

namespace {

std::optional<std::vector<std::uint8_t>> read_whole(std::filesystem::path const& path, std::size_t limit)
{
    std::error_code error;
    std::uintmax_t const size = std::filesystem::file_size(path, error);
    if (error || size > limit)
        return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file && !bytes.empty())
        return std::nullopt;
    return bytes;
}

bool write_whole(std::filesystem::path const& path, std::span<std::uint8_t const> bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

// The names of what waits. The archive's is the same for every version:
// the version is the line in `version`, and is checked against the list.
constexpr char const* staged_archive = "release.archive";
constexpr char const* staged_checksums = "SHA256SUMS";
constexpr char const* staged_signature = "SHA256SUMS.sig";
constexpr char const* staged_version_file = "version";
// A release is some megabytes; nothing larger is read at a start.
constexpr std::size_t largest_archive = 256u * 1024u * 1024u;

}

std::string staging_folder(std::string const& profile)
{
    return profile.empty() ? std::string() : (std::filesystem::path(profile) / "update").string();
}

bool stage_release(std::string const& folder, std::string_view version, std::span<std::uint8_t const> checksums,
    std::span<std::uint8_t const> signature, std::span<std::uint8_t const> archive, std::string* why_not)
{
    namespace fs = std::filesystem;
    if (folder.empty() || !parse_release_version(version))
        return said(why_not, "no folder, or no version, to keep the release under");
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder, error);
    // The version's line last: a folder without it holds nothing that waits.
    std::string const line = std::string(version) + "\n";
    if (!write_whole(fs::path(folder) / staged_archive, archive) || !write_whole(fs::path(folder) / staged_checksums, checksums)
        || !write_whole(fs::path(folder) / staged_signature, signature)
        || !write_whole(fs::path(folder) / staged_version_file, std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(line.data()), line.size()))) {
        fs::remove_all(folder, error);
        return said(why_not, "the release could not be written to " + folder);
    }
    return true;
}

std::optional<std::string> staged_version(std::string const& folder)
{
    if (folder.empty())
        return std::nullopt;
    std::optional<std::vector<std::uint8_t>> const line = read_whole(std::filesystem::path(folder) / staged_version_file, 64);
    if (!line)
        return std::nullopt;
    std::string version(line->begin(), line->end());
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
        version.pop_back();
    if (!parse_release_version(version))
        return std::nullopt;
    return version;
}

StagedOutcome apply_staged_release(std::string const& folder, std::string const& install, std::string_view running,
    crypto::Ed25519PublicKey const& key)
{
    namespace fs = std::filesystem;
    StagedOutcome outcome;
    std::error_code error;
    if (folder.empty() || !fs::exists(folder, error))
        return outcome;
    // Whatever comes of it, the folder is emptied: a release that failed
    // is fetched again by the next daily question, not tried at every start.
    struct Emptied {
        std::string const& folder;
        ~Emptied()
        {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    } const emptied { folder };
    std::optional<std::string> const version = staged_version(folder);
    if (!version) {
        // A folder with no version's line: a write that was cut short, or
        // nothing of ours. Not a release that was refused.
        return outcome;
    }
    outcome.version = *version;
    outcome.what = StagedOutcome::What::Refused;
    std::optional<std::vector<std::uint8_t>> const checksums = read_whole(fs::path(folder) / staged_checksums, 1024u * 1024u);
    std::optional<std::vector<std::uint8_t>> const signature = read_whole(fs::path(folder) / staged_signature, 1024);
    std::optional<std::vector<std::uint8_t>> const archive = read_whole(fs::path(folder) / staged_archive, largest_archive);
    if (!checksums || !signature || !archive) {
        outcome.why = "the release that waited is not all there";
        return outcome;
    }
    if (std::string refusal = release_refusal(*version, running, *checksums, *signature, *archive, key); !refusal.empty()) {
        outcome.why = std::move(refusal);
        return outcome;
    }
    std::optional<std::vector<ReleaseEntry>> const entries = unpack_release(*archive, release_folder_name(*version), &outcome.why);
    if (!entries)
        return outcome;
    // The program that is running is kept beside the new one, for a reader
    // who has to go back by hand.
    for (ReleaseEntry const& entry : *entries) {
        if (!entry.executable || entry.path.find('/') != std::string::npos)
            continue;
        fs::path const program = fs::path(install) / entry.path;
        if (fs::exists(program, error))
            fs::copy_file(program, program.string() + ".previous", fs::copy_options::overwrite_existing, error);
    }
    if (!install_release(install, *entries, &outcome.why))
        return outcome;
    outcome.what = StagedOutcome::What::Installed;
    return outcome;
}

bool can_replace_itself(std::string const& install)
{
#if defined(SASHFOLD_SELF_UPDATE) && defined(__linux__)
    if (install.empty() || release_archive_name("0.0.0").empty())
        return false;
    // Writable by this user: a package manager's copy is root's.
    std::error_code error;
    std::filesystem::path const probe = std::filesystem::path(install) / ".sashfold-update-probe";
    {
        std::ofstream file(probe, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
    }
    std::filesystem::remove(probe, error);
    return true;
#else
    static_cast<void>(install);
    return false;
#endif
}

bool install_release(std::string const& directory, std::vector<ReleaseEntry> const& entries, std::string* why_not)
{
    namespace fs = std::filesystem;
    std::error_code error;
    for (ReleaseEntry const& entry : entries) {
        if (!stays_inside(entry.path))
            return said(why_not, "a path that does not stay in the folder: " + entry.path);
        fs::path const target = fs::path(directory) / entry.path;
        fs::create_directories(target.parent_path(), error);
        fs::path const beside = target.string() + ".update";
        {
            std::ofstream file(beside, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<char const*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
            if (!file)
                return said(why_not, "cannot write " + beside.string());
        }
        fs::permissions(beside,
            entry.executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec
                             : fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
            error);
        fs::rename(beside, target, error);
        if (error) {
            fs::remove(beside, error);
            return said(why_not, "cannot put " + target.string() + " in place");
        }
    }
    return true;
}

}
