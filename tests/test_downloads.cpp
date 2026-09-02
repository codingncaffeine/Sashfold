#include "Test.h"

#include "net/Url.h"
#include "ui/Downloads.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Downloads as fetch: file names from Content-Disposition and URLs, always a
// single safe component; bytes land under a fresh name; the mark of the web
// is written where the OS has one.

using namespace sashfold;
using namespace sashfold::ui;

namespace {

net::Url url_of(std::string const& text)
{
    auto const url = net::parse_url(text);
    CHECK(url.has_value());
    return url ? *url : net::Url {};
}

std::string name_for(char const* disposition, std::string const& url)
{
    std::string const header = disposition ? disposition : "";
    return download_file_name(disposition ? &header : nullptr, url_of(url));
}

std::string read_all(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream stream;
    stream << file.rdbuf();
    return std::move(stream).str();
}

} // namespace

int main()
{
    // --- Renderable or not --------------------------------------------------------
    CHECK(is_renderable_content_type("text/html; charset=utf-8"));
    CHECK(is_renderable_content_type("TEXT/PLAIN"));
    CHECK(is_renderable_content_type("application/json"));
    CHECK(is_renderable_content_type(""));
    CHECK(!is_renderable_content_type("application/octet-stream"));
    CHECK(!is_renderable_content_type("application/pdf"));
    CHECK(!is_renderable_content_type("image/png"));
    CHECK(!is_renderable_content_type("application/zip"));

    // --- File names ------------------------------------------------------------------
    CHECK_EQ(name_for("attachment; filename=\"report.pdf\"", "https://x.test/dl?id=9"), "report.pdf");
    CHECK_EQ(name_for("attachment; filename=plain.bin", "https://x.test/dl"), "plain.bin");
    CHECK_EQ(name_for("attachment; filename=\"a.txt\"; filename*=UTF-8''caf%C3%A9%20menu.txt",
                 "https://x.test/dl"),
        "caf\xC3\xA9 menu.txt"); // the extended form wins
    // Inside a quoted string a backslash is a quoted-pair escape (RFC 7230), so
    // an unescaped one vanishes and an escaped one becomes a separator to
    // neutralize.
    CHECK_EQ(name_for("inline; filename=\"..\\..\\evil.exe\"", "https://x.test/dl"), "....evil.exe");
    CHECK_EQ(name_for("inline; filename=\"..\\\\..\\\\evil.exe\"", "https://x.test/dl"), ".._.._evil.exe");
    CHECK_EQ(name_for("attachment; filename=\"/etc/passwd\"", "https://x.test/dl"), "_etc_passwd");
    CHECK_EQ(name_for("attachment; filename=\"con\"", "https://x.test/dl"), "con_");
    CHECK_EQ(name_for("attachment; filename=\"  \"", "https://x.test/dl"), "download");
    CHECK_EQ(name_for("attachment", "https://x.test/files/archive%20one.zip"), "archive one.zip");
    CHECK_EQ(name_for(nullptr, "https://x.test/files/photo.jpg?x=1"), "photo.jpg");
    CHECK_EQ(name_for(nullptr, "https://x.test/files/"), "download");
    CHECK_EQ(name_for(nullptr, "https://x.test/"), "download");
    CHECK_EQ(name_for(nullptr, "data:application/octet-stream;base64,AAAA"), "download");

    // --- Saving ----------------------------------------------------------------------
    std::error_code error;
    std::filesystem::path const folder = std::filesystem::temp_directory_path(error)
        / "sashfold-test-downloads";
    std::filesystem::remove_all(folder, error);
    std::vector<std::uint8_t> const bytes = { 'h', 'e', 'l', 'l', 'o' };
    net::Url const source = url_of("https://x.test/files/note.txt");

    DownloadResult const first = save_download(folder.string(), "note.txt", bytes, source,
        "https://x.test/");
    CHECK(first.error.empty());
    CHECK_EQ(first.file_name, "note.txt");
    CHECK_EQ(read_all(first.path), "hello");
#ifdef _WIN32
    CHECK(first.marked);
    std::string const zone = read_all(std::filesystem::path(first.path + ":Zone.Identifier"));
    CHECK(zone.find("ZoneId=3") != std::string::npos);
    CHECK(zone.find("HostUrl=https://x.test/files/note.txt") != std::string::npos);
    CHECK(zone.find("ReferrerUrl=https://x.test/") != std::string::npos);
#endif

    // A second download with the same name never overwrites the first.
    std::vector<std::uint8_t> const other = { 'b', 'y', 'e' };
    DownloadResult const second = save_download(folder.string(), "note.txt", other, source, "");
    CHECK(second.error.empty());
    CHECK_EQ(second.file_name, "note (1).txt");
    CHECK_EQ(read_all(second.path), "bye");
    CHECK_EQ(read_all(first.path), "hello");
    DownloadResult const third = save_download(folder.string(), "note.txt", other, source, "");
    CHECK_EQ(third.file_name, "note (2).txt");

    // An unwritable folder is an error, not a crash.
    DownloadResult const nowhere = save_download((folder / "note.txt").string(), "x.bin", bytes,
        source, "");
    CHECK(!nowhere.error.empty());
    CHECK(nowhere.path.empty());

    std::filesystem::remove_all(folder, error); // only this test's own folder
    return sashfold::test::report("downloads");
}
