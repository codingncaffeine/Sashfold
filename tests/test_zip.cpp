// The zip reader against two archives the `zip` tool wrote: an .xpi with a
// deflated entry and two stored ones, and a .crx — the same kind of archive
// behind a header of its own, which moves every offset in it.

#include "core/Png.h"
#include "core/Zip.h"
#include "Test.h"

#include <fstream>
#include <iterator>
#include <string>

using namespace sashfold;

static std::vector<std::uint8_t> file_bytes(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv)
{
    std::string const fixtures = argc > 1 ? argv[1] : "tests/fixtures/themes";

    // --- A plain archive --------------------------------------------------------
    std::vector<std::uint8_t> const xpi = file_bytes(fixtures + "/firefox-sample.xpi");
    CHECK(!xpi.empty());
    {
        std::optional<ZipArchive> const archive = ZipArchive::open(xpi);
        CHECK(archive.has_value());
        if (archive) {
            CHECK_EQ(archive->entries().size(), std::size_t { 3 });
            ZipEntry const* const manifest = archive->find("manifest.json");
            ZipEntry const* const header = archive->find("header.png");
            CHECK(manifest != nullptr);
            CHECK(header != nullptr);
            CHECK(archive->find("./manifest.json") == manifest); // a leading ./ or / aside
            CHECK(archive->find("/header.png") == header);
            CHECK(archive->find("Manifest.json") == nullptr); // names are exact
            CHECK(archive->find("missing.png") == nullptr);
            if (manifest && header) {
                CHECK_EQ(static_cast<int>(manifest->method), 8); // deflated
                CHECK_EQ(static_cast<int>(header->method), 0); // stored
                std::optional<std::vector<std::uint8_t>> const text = archive->read(*manifest, 1u << 20);
                CHECK(text.has_value());
                if (text) {
                    CHECK_EQ(text->size(), static_cast<std::size_t>(manifest->size));
                    std::string const json(text->begin(), text->end());
                    CHECK(json.find("\"Sample Fox\"") != std::string::npos);
                }
                std::optional<std::vector<std::uint8_t>> const picture = archive->read(*header, 1u << 20);
                CHECK(picture.has_value());
                if (picture) {
                    std::optional<Bitmap> const decoded = decode_png(*picture);
                    CHECK(decoded.has_value());
                    if (decoded) {
                        CHECK_EQ(decoded->width(), 40);
                        CHECK_EQ(decoded->height(), 20);
                        CHECK(decoded->pixel(0, 0) == Color::rgb(0x00, 0xff, 0x00));
                    }
                }
                // No more is unpacked than the caller allows.
                CHECK(!archive->read(*manifest, 100).has_value());
                CHECK(!archive->read(*header, 100).has_value());
            }
        }
    }

    // --- The same kind of archive behind a header: a .crx ------------------------
    {
        std::vector<std::uint8_t> const crx = file_bytes(fixtures + "/chrome-sample.crx");
        CHECK(crx.size() > 28);
        CHECK(crx[0] == 'C' && crx[1] == 'r' && crx[2] == '2' && crx[3] == '4'); // the control: a header is there
        std::optional<ZipArchive> const archive = ZipArchive::open(crx);
        CHECK(archive.has_value());
        if (archive) {
            ZipEntry const* const manifest = archive->find("manifest.json");
            ZipEntry const* const frame = archive->find("images/theme_frame.png");
            CHECK(manifest != nullptr);
            CHECK(frame != nullptr);
            if (manifest) {
                std::optional<std::vector<std::uint8_t>> const text = archive->read(*manifest, 1u << 20);
                CHECK(text.has_value());
                if (text)
                    CHECK(std::string(text->begin(), text->end()).find("\"Sample Chrome\"") != std::string::npos);
            }
            if (frame) {
                std::optional<std::vector<std::uint8_t>> const picture = archive->read(*frame, 1u << 20);
                CHECK(picture.has_value());
                if (picture)
                    CHECK(decode_png(*picture).has_value());
            }
            bool directory_seen = false;
            for (ZipEntry const& entry : archive->entries())
                directory_seen = directory_seen || (entry.name == "images/" && entry.is_directory());
            CHECK(directory_seen);
        }
    }

    // --- What is wrong is refused, never guessed at -----------------------------
    {
        CHECK(!ZipArchive::open({}).has_value());
        CHECK(!ZipArchive::open(std::vector<std::uint8_t>(64, 0x41)).has_value());
        // Cut short: the directory is at the end, and the end is gone.
        std::vector<std::uint8_t> cut(xpi.begin(), xpi.begin() + static_cast<std::ptrdiff_t>(xpi.size() / 2));
        CHECK(!ZipArchive::open(cut).has_value());
        // One byte of a stored entry changed: it no longer matches its CRC-32.
        std::optional<ZipArchive> const whole = ZipArchive::open(xpi);
        CHECK(whole.has_value());
        if (whole) {
            ZipEntry const* const header = whole->find("header.png");
            CHECK(header != nullptr);
            if (header) {
                std::vector<std::uint8_t> bent = xpi;
                // Past the local header (30 bytes), the name and no extra field.
                std::size_t const at = static_cast<std::size_t>(header->local_header) + 30 + header->name.size() + 20;
                bent[at] = static_cast<std::uint8_t>(bent[at] ^ 0xFF);
                std::optional<ZipArchive> const damaged = ZipArchive::open(bent);
                CHECK(damaged.has_value());
                if (damaged) {
                    ZipEntry const* const again = damaged->find("header.png");
                    CHECK(again != nullptr);
                    if (again)
                        CHECK(!damaged->read(*again, 1u << 20).has_value());
                    ZipEntry const* const other = damaged->find("weave.png");
                    CHECK(other != nullptr);
                    if (other)
                        CHECK(damaged->read(*other, 1u << 20).has_value()); // the rest still reads
                }
            }
        }
    }

    return sashfold::test::report("zip");
}
