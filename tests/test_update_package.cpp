#include "Test.h"

#include "core/Inflate.h"
#include "crypto/Ed25519.h"
#include "crypto/Sha2.h"
#include "ui/UpdatePackage.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytes_of(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

// --- A tar file and a gzip stream, written by the test ---------------------------

struct TarEntry {
    TarEntry(std::string its_name, std::string its_data, char its_type = '0', unsigned its_mode = 0644, std::string its_prefix = {},
        bool spoiled = false)
        : name(std::move(its_name))
        , data(std::move(its_data))
        , type(its_type)
        , mode(its_mode)
        , prefix(std::move(its_prefix))
        , spoil_checksum(spoiled)
    {
    }
    std::string name;
    std::string data;
    char type; // '0' a file, '5' a folder, '2' a link, 'L' a long name
    unsigned mode;
    std::string prefix; // the POSIX form's second half of a name
    bool spoil_checksum;
};

// A number as a header holds it: octal digits filling the field but for
// the NUL that ends it.
void put_octal(std::uint8_t* field, std::size_t width, std::uint64_t value)
{
    field[width - 1] = 0;
    for (std::size_t i = width - 1; i-- > 0;) {
        field[i] = static_cast<std::uint8_t>('0' + (value & 7));
        value >>= 3;
    }
}

Bytes tar_of(std::vector<TarEntry> const& entries, bool end_blocks = true)
{
    Bytes tar;
    for (TarEntry const& entry : entries) {
        std::uint8_t header[512] = {};
        std::memcpy(header, entry.name.data(), std::min<std::size_t>(entry.name.size(), 100));
        put_octal(header + 100, 8, entry.mode);
        put_octal(header + 108, 8, 1000);
        put_octal(header + 116, 8, 1000);
        put_octal(header + 124, 12, entry.data.size());
        put_octal(header + 136, 12, 0);
        header[156] = static_cast<std::uint8_t>(entry.type);
        std::memcpy(header + 257, "ustar", 6);
        std::memcpy(header + 263, "00", 2);
        std::memcpy(header + 345, entry.prefix.data(), std::min<std::size_t>(entry.prefix.size(), 155));
        std::memset(header + 148, ' ', 8);
        unsigned sum = 0;
        for (std::uint8_t const byte : header)
            sum += byte;
        put_octal(header + 148, 7, entry.spoil_checksum ? sum + 1 : sum);
        header[155] = ' ';
        tar.insert(tar.end(), header, header + 512);
        tar.insert(tar.end(), entry.data.begin(), entry.data.end());
        while (tar.size() % 512 != 0)
            tar.push_back(0);
    }
    if (end_blocks)
        tar.insert(tar.end(), 1024, 0);
    return tar;
}

// gzip with stored blocks: the header, the bytes as they are in blocks of
// at most 65,535, the CRC-32 and the length.
Bytes gzip_of(Bytes const& data)
{
    Bytes out = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
    std::size_t at = 0;
    do {
        std::size_t const length = std::min<std::size_t>(data.size() - at, 65535);
        bool const last = at + length == data.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length & 0xff));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        out.push_back(static_cast<std::uint8_t>(~length & 0xff));
        out.push_back(static_cast<std::uint8_t>((~length >> 8) & 0xff));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < data.size());
    std::uint32_t const crc = crc32_of(data);
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(crc >> shift));
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(data.size() >> shift));
    return out;
}

std::string hex_of(crypto::Sha256::Digest const& digest)
{
    std::string text;
    for (std::uint8_t const byte : digest) {
        static constexpr char digits[] = "0123456789abcdef";
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

// The test's own release key: no release is signed with it.
constexpr crypto::Ed25519Seed test_seed = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };

Bytes signature_of(Bytes const& message, crypto::Ed25519Seed const& seed = test_seed)
{
    crypto::Ed25519Signature const signature = crypto::ed25519_sign(seed, message);
    return Bytes(signature.begin(), signature.end());
}

void test_names_and_checksums()
{
    std::string const archive = ui::release_archive_name("0.2.1");
#if defined(__linux__) && defined(__x86_64__)
    CHECK_EQ(archive, std::string("sashfold-0.2.1-linux-x64.tar.gz"));
    CHECK_EQ(ui::release_folder_name("0.2.1"), std::string("sashfold-0.2.1-linux-x64"));
#endif
    CHECK_EQ(ui::release_file_address("0.2.1", "SHA256SUMS"),
        std::string("https://github.com/codingncaffeine/Sashfold/releases/download/v0.2.1/SHA256SUMS"));
    // Not a version: no name, so nothing can be fetched under it.
    CHECK(ui::release_archive_name("../../etc").empty());
    CHECK(ui::release_archive_name("0.2.1-rc1").empty());
    CHECK(ui::release_archive_name("").empty());

    std::string const a(64, 'a');
    std::string const b = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef";
    std::optional<std::vector<ui::ReleaseFile>> files = ui::parse_checksums(a + "  one.tar.gz\n" + b + " *two words.zip\r\n\n");
    CHECK(files && files->size() == 2);
    if (files && files->size() == 2) {
        CHECK_EQ((*files)[0].name, std::string("one.tar.gz"));
        CHECK_EQ(static_cast<int>((*files)[0].sha256[0]), 0xaa);
        CHECK_EQ((*files)[1].name, std::string("two words.zip"));
        CHECK_EQ(static_cast<int>((*files)[1].sha256[0]), 0x01);
        CHECK_EQ(static_cast<int>((*files)[1].sha256[15]), 0xef);
    }
    CHECK(ui::parse_checksums("") && ui::parse_checksums("")->empty());
    CHECK(!ui::parse_checksums(a.substr(1) + "  short.tar.gz\n"));
    CHECK(!ui::parse_checksums(std::string(64, 'g') + "  not-hex.tar.gz\n"));
    CHECK(!ui::parse_checksums(a + " one-space.tar.gz\n"));
    CHECK(!ui::parse_checksums(a + "\n"));
    CHECK(!ui::parse_checksums("<html>Not Found</html>\n"));
}

void test_the_check()
{
    std::string const name = ui::release_archive_name("0.2.1");
    if (name.empty()) {
        // No release replaces a build of this kind: whatever is offered is refused.
        CHECK(!ui::release_refusal("0.2.1", "0.2.0", {}, {}, {}).empty());
        return;
    }
    crypto::Ed25519PublicKey const key = crypto::ed25519_public_key(test_seed);
    Bytes const archive = bytes_of("the archive's bytes, whatever they are");
    std::string const other(64, '0');
    Bytes const checksums = bytes_of(other + "  sashfold-0.2.1-somewhere-else.zip\n" + hex_of(crypto::Sha256::hash(archive)) + "  " + name + "\n");
    Bytes const signature = signature_of(checksums);

    // The release itself.
    CHECK_EQ(ui::release_refusal("0.2.1", "0.2.0", checksums, signature, archive, key), std::string());
    CHECK_EQ(ui::release_refusal("0.2.1", "0.1.9", checksums, signature, archive, key), std::string());
    // Under the key that is built in, which did not sign it: refused. (What
    // the default is: the release key, not a key a caller forgot to give.)
    CHECK(ui::release_refusal("0.2.1", "0.2.0", checksums, signature, archive).find("not signed by the release key") != std::string::npos);
    // Signed by someone else.
    crypto::Ed25519Seed other_seed = test_seed;
    other_seed[0] ^= 1;
    CHECK(ui::release_refusal("0.2.1", "0.2.0", checksums, signature_of(checksums, other_seed), archive, key).find("not signed") != std::string::npos);
    // The list changed after it was signed — one byte of a hash, or a line added.
    Bytes changed = checksums;
    changed[3] = changed[3] == '0' ? '1' : '0';
    CHECK(ui::release_refusal("0.2.1", "0.2.0", changed, signature, archive, key).find("not signed") != std::string::npos);
    Bytes longer = checksums;
    longer.push_back('\n');
    CHECK(ui::release_refusal("0.2.1", "0.2.0", longer, signature, archive, key).find("not signed") != std::string::npos);
    // The signature cut short, or missing, or with a byte changed.
    CHECK(!ui::release_refusal("0.2.1", "0.2.0", checksums, Bytes(signature.begin(), signature.end() - 1), archive, key).empty());
    CHECK(!ui::release_refusal("0.2.1", "0.2.0", checksums, {}, archive, key).empty());
    Bytes bent = signature;
    bent[10] ^= 0x40;
    CHECK(!ui::release_refusal("0.2.1", "0.2.0", checksums, bent, archive, key).empty());
    // The download is not the file the list names: a byte changed, a byte missing.
    Bytes other_archive = archive;
    other_archive[0] ^= 1;
    CHECK(ui::release_refusal("0.2.1", "0.2.0", checksums, signature, other_archive, key).find("SHA-256 differs") != std::string::npos);
    CHECK(ui::release_refusal("0.2.1", "0.2.0", checksums, signature, Bytes(archive.begin(), archive.end() - 1), key).find("SHA-256 differs") != std::string::npos);
    // Not later than what is running: the same release, an older one.
    CHECK(ui::release_refusal("0.2.1", "0.2.1", checksums, signature, archive, key).find("not later") != std::string::npos);
    CHECK(ui::release_refusal("0.2.1", "0.3.0", checksums, signature, archive, key).find("not later") != std::string::npos);
    // An older release's list and archive, truly signed, offered as a newer
    // one: its list does not name the newer one's file.
    CHECK(ui::release_refusal("0.2.2", "0.2.0", checksums, signature, archive, key).find("does not list") != std::string::npos);
    // A signed list that cannot be read as one.
    Bytes const nonsense = bytes_of("not a list of checksums\n");
    CHECK(ui::release_refusal("0.2.1", "0.2.0", nonsense, signature_of(nonsense), archive, key).find("cannot be read") != std::string::npos);
    // Not a version at all.
    CHECK(!ui::release_refusal("latest", "0.2.0", checksums, signature, archive, key).empty());
}

void test_unpacking()
{
    std::string const folder = "sashfold-0.2.1-linux-x64";
    std::string why;
    // A release as it is packed: the folder, the program, a file in a folder.
    Bytes const good = gzip_of(tar_of({
        { folder + "/", "", '5', 0755 },
        { folder + "/sashfold", "the program", '0', 0755 },
        { folder + "/themes/", "", '5', 0755 },
        { folder + "/themes/default.json", "{\"name\": \"Default\"}", '0', 0644 },
        { folder + "/LICENSE", std::string(70000, 'x'), '0', 0644 }, // longer than a gzip block
    }));
    std::optional<std::vector<ui::ReleaseEntry>> entries = ui::unpack_release(good, folder, &why);
    CHECK(entries.has_value());
    CHECK_EQ(why, std::string());
    if (entries) {
        CHECK_EQ(entries->size(), 3u);
        if (entries->size() == 3) {
            CHECK_EQ((*entries)[0].path, std::string("sashfold"));
            CHECK((*entries)[0].bytes == bytes_of("the program"));
            CHECK((*entries)[0].executable);
            CHECK_EQ((*entries)[1].path, std::string("themes/default.json"));
            CHECK(!(*entries)[1].executable);
            CHECK_EQ((*entries)[2].bytes.size(), 70000u);
        }
    }
    // Names as other tar programs write them: a leading "./", a long name
    // in a GNU 'L' entry, a name in the POSIX form's two halves.
    std::string const deep = folder + "/assets/icons/hicolor/256x256/apps/a-name-long-enough-to-need-more-than-the-hundred-bytes-a-header-holds/sashfold.png";
    entries = ui::unpack_release(gzip_of(tar_of({
                                     { "./" + folder + "/", "", '5', 0755 },
                                     { "./" + folder + "/README.md", "read me", '0', 0644 },
                                     { "././@LongLink", deep + std::string(1, '\0'), 'L', 0 },
                                     { deep.substr(0, 100), "png", '0', 0644 },
                                     { "tools/install.sh", "#!/bin/sh", '0', 0755, folder },
                                 })),
        folder, &why);
    CHECK(entries.has_value());
    if (entries && entries->size() == 3) {
        CHECK_EQ((*entries)[0].path, std::string("README.md"));
        CHECK_EQ((*entries)[1].path, deep.substr(folder.size() + 1));
        CHECK_EQ((*entries)[2].path, std::string("tools/install.sh"));
        CHECK((*entries)[2].executable);
    } else {
        CHECK(entries && entries->size() == 3);
    }

    // What is refused, and that it is said why.
    struct Refused {
        Bytes archive;
        std::string_view because;
    };
    Bytes const tar = tar_of({ { folder + "/", "", '5', 0755 }, { folder + "/sashfold", "the program", '0', 0755 } });
    Bytes cut_gzip = gzip_of(tar);
    cut_gzip.resize(cut_gzip.size() - 9);
    Refused const refused[] = {
        { bytes_of("PK\x03\x04 a zip cut"), "cut short" }, // read as the zip it begins as
        { cut_gzip, "not a gzip stream" },
        { gzip_of(bytes_of("gzip, but of no tar file: " + std::string(600, 'z'))), "checksum is wrong" },
        { gzip_of(Bytes(tar.begin(), tar.begin() + 512 + 512)), "cut short" }, // no end blocks
        { gzip_of(Bytes(tar.begin(), tar.begin() + 512 + 512 + 512)), "cut short" }, // a header whose file is not there
        { gzip_of(tar_of({ { folder + "/sashfold", "x", '0', 0755, "", true } })), "checksum is wrong" },
        { gzip_of(tar_of({ { folder + "/../../.bashrc", "x" } })), "does not stay in its folder" },
        { gzip_of(tar_of({ { "/etc/passwd", "x" } })), "does not stay in its folder" },
        { gzip_of(tar_of({ { folder + "/a//b", "x" } })), "does not stay in its folder" },
        { gzip_of(tar_of({ { folder + "/a\\b", "x" } })), "does not stay in its folder" },
        { gzip_of(tar_of({ { "another-folder/sashfold", "x" } })), "outside" },
        { gzip_of(tar_of({ { folder + "-evil/sashfold", "x" } })), "outside" },
        { gzip_of(tar_of({ { folder + "/link", "", '2' } })), "neither a file nor a folder" },
        { gzip_of(tar_of({ { folder + "/hard", "", '1' } })), "neither a file nor a folder" },
        { gzip_of(tar_of({ { folder + "/device", "", '3' } })), "neither a file nor a folder" },
        { gzip_of(tar_of({ { folder, "a file where the folder should be" } })), "not a folder" },
        { gzip_of(tar_of({ { folder + "/", "", '5', 0755 } })), "holds no file" },
        { gzip_of(tar_of({})), "holds no file" },
    };
    for (Refused const& one : refused) {
        why.clear();
        CHECK(!ui::unpack_release(one.archive, folder, &why).has_value());
        if (why.find(one.because) == std::string::npos)
            std::printf("  refused because \"%s\", expected \"%.*s\"\n", why.c_str(), static_cast<int>(one.because.size()), one.because.data());
        CHECK(why.find(one.because) != std::string::npos);
    }
    // No folder named, or one that is a path: nothing is unpacked.
    CHECK(!ui::unpack_release(good, "", &why).has_value());
    CHECK(!ui::unpack_release(good, "..", &why).has_value());
    CHECK(!ui::unpack_release(good, "a/b", &why).has_value());
}

std::string read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return std::move(text).str();
}

// Archives as the program that packs the releases writes them (GNU tar,
// its own format and the POSIX one, deflated): the same three files out
// of each, the long path whole, the program marked as one.
void test_archives_as_tar_writes_them(std::string const& fixtures)
{
    std::string const long_path = "assets/icons/hicolor/256x256/apps/"
                                  "a-folder-name-long-enough-that-the-whole-path-passes-the-hundred-bytes-a-tar-header-holds/sashfold.png";
    for (char const* const name : { "release-gnu.tar.gz", "release-ustar.tar.gz" }) {
        std::string const packed = read_file(std::filesystem::path(fixtures) / name);
        CHECK(!packed.empty());
        std::string why;
        std::optional<std::vector<ui::ReleaseEntry>> const entries = ui::unpack_release(bytes_of(packed), "sashfold-0.2.1-linux-x64", &why);
        if (!entries)
            std::printf("  %s: %s\n", name, why.c_str());
        CHECK(entries.has_value());
        if (!entries)
            continue;
        CHECK_EQ(entries->size(), 3u);
        std::string program;
        std::string theme;
        std::string picture;
        bool program_runs = false;
        bool theme_runs = true;
        for (ui::ReleaseEntry const& entry : *entries) {
            std::string const text(entry.bytes.begin(), entry.bytes.end());
            if (entry.path == "sashfold") {
                program = text;
                program_runs = entry.executable;
            } else if (entry.path == "themes/default.json") {
                theme = text;
                theme_runs = entry.executable;
            } else if (entry.path == long_path) {
                picture = text;
            }
        }
        CHECK_EQ(program, std::string("#!/bin/sh\necho the program\n"));
        CHECK(program_runs);
        CHECK_EQ(theme, std::string("{\"name\": \"Default\"}\n"));
        CHECK(!theme_runs);
        CHECK_EQ(picture, std::string("not really a picture\n"));
    }
}

void test_installing()
{
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path const root = fs::absolute("update-package-scratch", error);
    fs::remove_all(root, error);
    fs::create_directories(root / "blocklists", error);
    // What is there before: the old program, a file the release does not
    // hold (the reader's own list), a file it does.
    std::ofstream(root / "sashfold", std::ios::binary) << "the old program";
    std::ofstream(root / "blocklists" / "mine.txt", std::ios::binary) << "the reader's own";
    std::ofstream(root / "README.md", std::ios::binary) << "old words";

    std::vector<ui::ReleaseEntry> const entries = {
        { "sashfold", bytes_of("the new program"), true },
        { "README.md", bytes_of("new words"), false },
        { "themes/dark/night.json", bytes_of("{}"), false },
    };
    std::string why;
    CHECK(ui::install_release(root.string(), entries, &why));
    CHECK_EQ(why, std::string());
    CHECK_EQ(read_file(root / "sashfold"), std::string("the new program"));
    CHECK_EQ(read_file(root / "README.md"), std::string("new words"));
    CHECK_EQ(read_file(root / "themes" / "dark" / "night.json"), std::string("{}"));
    CHECK_EQ(read_file(root / "blocklists" / "mine.txt"), std::string("the reader's own"));
#ifndef _WIN32
    CHECK((fs::status(root / "sashfold", error).permissions() & fs::perms::owner_exec) != fs::perms::none);
    CHECK((fs::status(root / "README.md", error).permissions() & fs::perms::owner_exec) == fs::perms::none);
#endif
    // Nothing is left beside the files.
    std::size_t leftovers = 0;
    for (fs::directory_entry const& entry : fs::recursive_directory_iterator(root, error))
        leftovers += entry.path().extension() == ".update" ? 1 : 0;
    CHECK_EQ(leftovers, 0u);
    // A path that would leave the folder is not written, whoever made the list.
    CHECK(!ui::install_release((root / "themes").string(), { { "../escaped", bytes_of("x"), false } }, &why));
    CHECK(!fs::exists(root / "escaped"));
    CHECK(!ui::install_release((root / "themes").string(), { { "/tmp/sashfold-escaped", bytes_of("x"), false } }, &why));
    // A place that cannot be written says so.
    CHECK(!ui::install_release((root / "sashfold").string(), { { "inside-a-file", bytes_of("x"), false } }, &why));
    CHECK(!why.empty());
    fs::remove_all(root, error);
}

}

// A release kept by one run and put in place by the next — and what the
// next run refuses, since the folder it waits in is one a page that took
// the browser over could have written.
void test_from_one_run_to_the_next()
{
    namespace fs = std::filesystem;
    std::string const version = "9.9.9";
    std::string const name = ui::release_archive_name(version);
    if (name.empty())
        return; // no release replaces a build of this kind
    std::string const top = ui::release_folder_name(version);
    crypto::Ed25519PublicKey const key = crypto::ed25519_public_key(test_seed);
    Bytes const archive = gzip_of(tar_of({
        { top + "/", "", '5', 0755 },
        { top + "/sashfold", "the new program", '0', 0755 },
        { top + "/themes/", "", '5', 0755 },
        { top + "/themes/default.json", "{\"new\": true}", '0', 0644 },
    }));
    Bytes const checksums = bytes_of(hex_of(crypto::Sha256::hash(archive)) + "  " + name + "\n");
    Bytes const signature = signature_of(checksums);

    std::error_code error;
    fs::path const root = fs::absolute("update-run-scratch", error);
    fs::remove_all(root, error);
    std::string const waiting = ui::staging_folder((root / "profile").string());
    std::string const install = (root / "install").string();
    CHECK_EQ(waiting, (root / "profile" / "update").string());
    CHECK(ui::staging_folder("").empty());
    auto const fresh_install = [&] {
        fs::remove_all(install, error);
        fs::create_directories(fs::path(install) / "blocklists", error);
        std::ofstream(fs::path(install) / "sashfold", std::ios::binary) << "the old program";
        std::ofstream(fs::path(install) / "blocklists" / "mine.txt", std::ios::binary) << "the reader's own";
    };
    auto const untouched = [&] {
        return read_file(fs::path(install) / "sashfold") == "the old program" && !fs::exists(fs::path(install) / "themes")
            && !fs::exists(fs::path(install) / "sashfold.previous");
    };

    // Nothing waits: nothing is done, and no folder is made.
    fresh_install();
    ui::StagedOutcome outcome = ui::apply_staged_release(waiting, install, "0.2.0", key);
    CHECK(outcome.what == ui::StagedOutcome::What::Nothing);
    CHECK(!fs::exists(waiting));
    CHECK(!ui::staged_version(waiting).has_value());

    // Kept, then put in place by the next start: the new files are there,
    // the old program is beside the new one, the reader's own file is as it
    // was, and nothing waits any more.
    std::string why;
    CHECK(ui::stage_release(waiting, version, checksums, signature, archive, &why));
    CHECK_EQ(ui::staged_version(waiting).value_or("none"), version);
    outcome = ui::apply_staged_release(waiting, install, "0.2.0", key);
    CHECK(outcome.what == ui::StagedOutcome::What::Installed);
    CHECK_EQ(outcome.version, version);
    CHECK_EQ(outcome.why, std::string());
    CHECK_EQ(read_file(fs::path(install) / "sashfold"), std::string("the new program"));
    CHECK_EQ(read_file(fs::path(install) / "sashfold.previous"), std::string("the old program"));
    CHECK_EQ(read_file(fs::path(install) / "themes" / "default.json"), std::string("{\"new\": true}"));
    CHECK_EQ(read_file(fs::path(install) / "blocklists" / "mine.txt"), std::string("the reader's own"));
    CHECK(!fs::exists(waiting));
    outcome = ui::apply_staged_release(waiting, install, "0.2.0", key);
    CHECK(outcome.what == ui::StagedOutcome::What::Nothing);

    // What a start refuses, each leaving the program as it was and the
    // folder empty.
    struct Tampering {
        char const* what;
        std::function<void()> change;
        std::string_view because;
        std::string_view running = "0.2.0";
    };
    crypto::Ed25519Seed other_seed = test_seed;
    other_seed[31] ^= 0x80;
    Bytes const forged_archive = gzip_of(tar_of({ { top + "/", "", '5', 0755 }, { top + "/sashfold", "an intruder's program", '0', 0755 } }));
    Bytes const forged_checksums = bytes_of(hex_of(crypto::Sha256::hash(forged_archive)) + "  " + name + "\n");
    auto const overwrite = [&](char const* file, Bytes const& bytes) {
        std::ofstream out(fs::path(waiting) / file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    Tampering const tamperings[] = {
        { "the archive changed where it waited", [&] { overwrite("release.archive", forged_archive); }, "SHA-256 differs" },
        { "the archive and its list changed, the signature left",
            [&] {
                overwrite("release.archive", forged_archive);
                overwrite("SHA256SUMS", forged_checksums);
            },
            "not signed by the release key" },
        { "all three replaced by a release someone else signed",
            [&] {
                overwrite("release.archive", forged_archive);
                overwrite("SHA256SUMS", forged_checksums);
                overwrite("SHA256SUMS.sig", signature_of(forged_checksums, other_seed));
            },
            "not signed by the release key" },
        { "the signature removed", [&] { fs::remove(fs::path(waiting) / "SHA256SUMS.sig", error); }, "not all there" },
        { "the archive removed", [&] { fs::remove(fs::path(waiting) / "release.archive", error); }, "not all there" },
        { "the version's line changed to a later one", [&] { overwrite("version", bytes_of("10.0.0\n")); }, "does not list" },
        { "the running build is already that release", [] {}, "not later", "9.9.9" },
        { "the running build is later", [] {}, "not later", "10.0.0" },
    };
    for (Tampering const& tampering : tamperings) {
        fresh_install();
        CHECK(ui::stage_release(waiting, version, checksums, signature, archive, &why));
        tampering.change();
        outcome = ui::apply_staged_release(waiting, install, tampering.running, key);
        if (outcome.what != ui::StagedOutcome::What::Refused || outcome.why.find(tampering.because) == std::string::npos)
            std::printf("  %s: %s\n", tampering.what, outcome.why.c_str());
        CHECK(outcome.what == ui::StagedOutcome::What::Refused);
        CHECK(outcome.why.find(tampering.because) != std::string::npos);
        CHECK(untouched());
        CHECK(!fs::exists(waiting));
    }
    // The key that is built in did not sign the test's release.
    fresh_install();
    CHECK(ui::stage_release(waiting, version, checksums, signature, archive, &why));
    outcome = ui::apply_staged_release(waiting, install, "0.2.0");
    CHECK(outcome.what == ui::StagedOutcome::What::Refused);
    CHECK(untouched());
    // A folder with no version's line, or one that names no version, holds
    // nothing that waits; it is emptied all the same.
    for (char const* const line : { "", "../../somewhere\n", "latest\n" }) {
        fresh_install();
        CHECK(ui::stage_release(waiting, version, checksums, signature, archive, &why));
        if (*line == 0)
            fs::remove(fs::path(waiting) / "version", error);
        else
            overwrite("version", bytes_of(line));
        CHECK(!ui::staged_version(waiting).has_value());
        outcome = ui::apply_staged_release(waiting, install, "0.2.0", key);
        CHECK(outcome.what == ui::StagedOutcome::What::Nothing);
        CHECK(untouched());
        CHECK(!fs::exists(waiting));
    }
    // Nothing is kept under a name that is no version.
    CHECK(!ui::stage_release(waiting, "../x", checksums, signature, archive, &why));
    CHECK(!fs::exists(waiting));
    // Only the build the downloads are made of replaces itself, and only
    // in a folder it can write: a build tree's program, and a test's, does not.
#if defined(SASHFOLD_SELF_UPDATE) && (defined(__linux__) || defined(_WIN32))
    CHECK(ui::can_replace_itself(install));
    CHECK(!ui::can_replace_itself((root / "no-such-folder").string()));
    CHECK(!fs::exists(fs::path(install) / ".sashfold-update-probe"));
#else
    CHECK(!ui::can_replace_itself(install));
#endif
    fs::remove_all(root, error);
}

namespace {

// --- A zip file, written by the test (APPNOTE.TXT) -------------------------------

struct ZipEntry {
    std::string name;
    std::string data;
    std::uint32_t unix_mode = 0100644; // 0: made on Windows, no mode
    bool deflate = false;
    bool spoil_crc = false;
    bool encrypted = false;
};

void put16(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void put32(Bytes& out, std::uint32_t value)
{
    put16(out, value & 0xffff);
    put16(out, value >> 16);
}

Bytes zip_of(std::vector<ZipEntry> const& entries)
{
    Bytes out;
    Bytes directory;
    for (ZipEntry const& entry : entries) {
        Bytes const raw = bytes_of(entry.data);
        Bytes packed = raw;
        if (entry.deflate) {
            // zlib's stream less its two-byte header and Adler-32: raw DEFLATE.
            Bytes const z = zlib_compress(raw);
            packed.assign(z.begin() + 2, z.end() - 4);
        }
        std::uint32_t const crc = crc32_of(raw) + (entry.spoil_crc ? 1 : 0);
        std::uint32_t const flags = entry.encrypted ? 1 : 0;
        std::uint32_t const method = entry.deflate ? 8 : 0;
        std::uint32_t const local_at = static_cast<std::uint32_t>(out.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, flags);
        put16(out, method);
        put32(out, 0); // time and date
        put32(out, crc);
        put32(out, static_cast<std::uint32_t>(packed.size()));
        put32(out, static_cast<std::uint32_t>(raw.size()));
        put16(out, static_cast<std::uint32_t>(entry.name.size()));
        put16(out, 0);
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.insert(out.end(), packed.begin(), packed.end());
        put32(directory, 0x02014b50);
        put16(directory, entry.unix_mode != 0 ? (3u << 8) | 20 : 20);
        put16(directory, 20);
        put16(directory, flags);
        put16(directory, method);
        put32(directory, 0);
        put32(directory, crc);
        put32(directory, static_cast<std::uint32_t>(packed.size()));
        put32(directory, static_cast<std::uint32_t>(raw.size()));
        put16(directory, static_cast<std::uint32_t>(entry.name.size()));
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, entry.unix_mode << 16);
        put32(directory, local_at);
        directory.insert(directory.end(), entry.name.begin(), entry.name.end());
    }
    std::uint32_t const directory_at = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(directory.size()));
    put32(out, directory_at);
    put16(out, 0);
    return out;
}

// Windows' download is a zip: unpacked as the tarball is, stored and
// deflated files alike, each checked against its CRC-32, with the same
// refusals — and one made on Windows, with no modes, has its programs by
// their names.
void test_zip_archives()
{
    std::string const folder = "sashfold-0.2.1-windows-x64";
    std::string why;
    std::string const big(70000, 'x');
    Bytes const good = zip_of({
        { folder + "/", "", 040755 },
        { folder + "/sashfold.exe", "MZ the program", 0100755, true },
        { folder + "/themes/default.json", big, 0100644, true },
        { folder + "/LICENSE", "words" },
    });
    std::optional<std::vector<ui::ReleaseEntry>> entries = ui::unpack_release(good, folder, &why);
    CHECK_EQ(why, std::string());
    CHECK(entries && entries->size() == 3);
    if (entries && entries->size() == 3) {
        CHECK_EQ((*entries)[0].path, std::string("sashfold.exe"));
        CHECK_EQ(std::string((*entries)[0].bytes.begin(), (*entries)[0].bytes.end()), std::string("MZ the program"));
        CHECK((*entries)[0].executable);
        CHECK_EQ((*entries)[1].path, std::string("themes/default.json"));
        CHECK((*entries)[1].bytes.size() == big.size());
        CHECK(!(*entries)[1].executable);
        CHECK_EQ((*entries)[2].path, std::string("LICENSE"));
    }
    // Made on Windows: no modes; the .exe is the program.
    entries = ui::unpack_release(zip_of({ { folder + "/sashfold.exe", "MZ", 0 }, { folder + "/README.md", "r", 0 } }), folder, &why);
    CHECK(entries && entries->size() == 2 && (*entries)[0].executable && !(*entries)[1].executable);
    // Refused, each with why.
    auto const refused = [&](std::vector<ZipEntry> const& list, std::string_view says) {
        std::string reason;
        bool const refused_it = !ui::unpack_release(zip_of(list), folder, &reason).has_value();
        CHECK(refused_it);
        if (reason.find(says) == std::string::npos)
            CHECK_EQ(reason, std::string(says));
    };
    refused({ { folder + "/sashfold.exe", "MZ", 0100755, false, true } }, "CRC-32");
    refused({ { folder + "/sashfold.exe", "MZ", 0100755, true, true } }, "CRC-32");
    refused({ { folder + "/../escaped", "x" } }, "does not stay");
    refused({ { "elsewhere/sashfold.exe", "x" } }, "outside");
    refused({ { folder + "/link", "target", 0120777 } }, "neither a file nor a folder");
    refused({ { folder + "/secret", "x", 0100644, false, false, true } }, "encrypted");
    refused({ { folder, "not a folder" } }, "top entry");
    refused({}, "no file under");
    // Cut short: the end record is gone.
    Bytes cut = good;
    cut.resize(cut.size() - 10);
    CHECK(!ui::unpack_release(cut, folder, &why).has_value());
    // A tarball still reads as one.
    CHECK(ui::unpack_release(gzip_of(tar_of({ { "sashfold-0.2.1-linux-x64/sashfold", "p", '0', 0755 } })), "sashfold-0.2.1-linux-x64", &why).has_value());
}

// A new build on trial, what failed here, and the program before it put back.
void test_trials_and_going_back()
{
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path const root = fs::absolute("update-trial-scratch", error);
    fs::remove_all(root, error);
    fs::create_directories(root / "install", error);
    fs::create_directories(root / "profile", error);
    std::string const profile = (root / "profile").string();

    std::string const file = ui::trial_file(profile);
    CHECK_EQ(file, (root / "profile" / "update-trial").string());
    CHECK(!ui::read_trial(file).has_value());
    CHECK(ui::begin_trial(file, { "9.9.9", "0.2.0", 4242 }));
    std::optional<ui::Trial> const trial = ui::read_trial(file);
    CHECK(trial.has_value());
    if (trial) {
        CHECK_EQ(trial->version, std::string("9.9.9"));
        CHECK_EQ(trial->previous, std::string("0.2.0"));
        CHECK_EQ(trial->watcher, 4242u);
    }
    CHECK(!ui::begin_trial(file, { "not a version", "0.2.0", 1 }));
    ui::end_trial(file);
    CHECK(!fs::exists(file));
    ui::end_trial(file); // twice is nothing
    // A trial file a page could have written to say anything is read for
    // its version alone, and a broken one is none.
    std::ofstream(file, std::ios::binary) << "garbage\n";
    CHECK(!ui::read_trial(file).has_value());
    ui::end_trial(file);

    CHECK(ui::refused_releases(profile).empty());
    ui::refuse_release(profile, "9.9.9");
    ui::refuse_release(profile, "9.9.9");
    ui::refuse_release(profile, "nonsense");
    ui::refuse_release(profile, "10.0.0");
    CHECK(ui::refused_releases(profile) == (std::vector<std::string> { "9.9.9", "10.0.0" }));

    std::string const program = (root / "install" / "sashfold").string();
    std::string why;
    CHECK(!ui::restore_previous(program, &why)); // nothing to go back to
    CHECK(!why.empty());
    std::ofstream(program, std::ios::binary) << "the new program that does not start";
    std::ofstream(program + ".previous", std::ios::binary) << "the program before it";
    CHECK(ui::restore_previous(program, &why));
    CHECK_EQ(read_file(program), std::string("the program before it"));
#ifndef _WIN32
    CHECK((fs::status(program, error).permissions() & fs::perms::owner_exec) != fs::perms::none);
#endif
    fs::remove_all(root, error);
}

}

int main(int argc, char** argv)
{
    test_names_and_checksums();
    test_the_check();
    test_unpacking();
    if (argc > 1)
        test_archives_as_tar_writes_them(argv[1]);
    test_installing();
    test_from_one_run_to_the_next();
    test_zip_archives();
    test_trials_and_going_back();
    return sashfold::test::report("update_package");
}
