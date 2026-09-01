#include "Test.h"

#include "core/Inflate.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// The production inflate against: fixture streams produced by an INDEPENDENT
// compressor (.NET System.IO.Compression, which emits dynamic-Huffman blocks
// our own encoder never writes), hostile-input rejection, and the output cap.

using namespace sashfold;

namespace {

std::filesystem::path g_fixtures;

std::optional<std::vector<std::uint8_t>> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    std::string const text = std::move(stream).str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

void check_sample(char const* name)
{
    auto const raw = read_file(g_fixtures / (std::string(name) + ".raw"));
    auto const deflated = read_file(g_fixtures / (std::string(name) + ".deflate"));
    auto const gzipped = read_file(g_fixtures / (std::string(name) + ".gzip"));
    auto const zlibbed = read_file(g_fixtures / (std::string(name) + ".zlib"));
    CHECK(raw && deflated && gzipped && zlibbed);
    if (!raw || !deflated || !gzipped || !zlibbed)
        return;

    auto const from_deflate = inflate(*deflated);
    CHECK(from_deflate && *from_deflate == *raw);
    auto const from_gzip = gzip_decompress(*gzipped);
    CHECK(from_gzip && *from_gzip == *raw);
    auto const from_zlib = zlib_decompress(*zlibbed);
    CHECK(from_zlib && *from_zlib == *raw);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: test_inflate <fixtures-dir>\n";
        return 2;
    }
    g_fixtures = argv[1];

    // --- Independent-compressor round trips ----------------------------------
    check_sample("hello");
    check_sample("spec");
    check_sample("binary");

    // --- Corrupted streams are rejected, not trusted -------------------------
    {
        auto gzipped = read_file(g_fixtures / "hello.gzip");
        CHECK(gzipped.has_value());
        if (gzipped) {
            auto bad = *gzipped;
            bad[bad.size() / 2] ^= 0x40; // flip a payload bit
            CHECK(!gzip_decompress(bad).has_value());
            auto bad_crc = *gzipped;
            bad_crc[bad_crc.size() - 5] ^= 0x01; // flip a CRC bit
            CHECK(!gzip_decompress(bad_crc).has_value());
        }
        auto zlibbed = read_file(g_fixtures / "hello.zlib");
        CHECK(zlibbed.has_value());
        if (zlibbed) {
            auto bad = *zlibbed;
            bad.back() ^= 0x01; // flip an Adler bit
            CHECK(!zlib_decompress(bad).has_value());
        }
    }

    // --- Truncation and garbage ----------------------------------------------
    {
        auto gzipped = read_file(g_fixtures / "spec.gzip");
        CHECK(gzipped.has_value());
        if (gzipped) {
            for (std::size_t keep : { std::size_t { 0 }, std::size_t { 5 }, gzipped->size() / 2,
                     gzipped->size() - 1 }) {
                std::vector<std::uint8_t> const truncated(gzipped->begin(),
                    gzipped->begin() + static_cast<std::ptrdiff_t>(keep));
                CHECK(!gzip_decompress(truncated).has_value());
            }
        }
        CHECK(!inflate({ 0x07 }).has_value()); // BTYPE 3 is reserved
        CHECK(!zlib_decompress({ 0x78, 0x02, 0x00 }).has_value()); // bad FCHECK
    }

    // --- The output cap holds against decompression bombs --------------------
    {
        auto gzipped = read_file(g_fixtures / "binary.gzip");
        CHECK(gzipped.has_value());
        if (gzipped)
            CHECK(!gzip_decompress(*gzipped, 100).has_value()); // 2048 raw > 100 cap
    }

    // --- Deterministic pseudo-random stored-block round trip -----------------
    {
        std::mt19937 rng(20260901);
        std::vector<std::uint8_t> noise(70000);
        for (std::uint8_t& byte : noise)
            byte = static_cast<std::uint8_t>(rng());
        // Hand-build a stored-block deflate stream (incompressible data path,
        // exercising multi-block stitching: 70000 > 65535).
        std::vector<std::uint8_t> stream;
        std::size_t at = 0;
        while (at < noise.size()) {
            std::size_t const block = std::min<std::size_t>(65535, noise.size() - at);
            bool const final_block = at + block == noise.size();
            stream.push_back(final_block ? 0x01 : 0x00);
            stream.push_back(static_cast<std::uint8_t>(block & 0xFF));
            stream.push_back(static_cast<std::uint8_t>(block >> 8));
            stream.push_back(static_cast<std::uint8_t>(~block & 0xFF));
            stream.push_back(static_cast<std::uint8_t>((~block >> 8) & 0xFF));
            stream.insert(stream.end(), noise.begin() + static_cast<std::ptrdiff_t>(at),
                noise.begin() + static_cast<std::ptrdiff_t>(at + block));
            at += block;
        }
        auto const out = inflate(stream);
        CHECK(out && *out == noise);
    }

    return sashfold::test::report("inflate");
}
