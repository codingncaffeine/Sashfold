#include "Test.h"

#include "core/Brotli.h"
#include "net/Http.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// Brotli against streams an INDEPENDENT compressor wrote (the reference
// `brotli` tool, at qualities from 0 to 11 and windows from 10 to 24 bits),
// the tables RFC 7932 gives check values for, hostile input, and the output
// cap.

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

// CRC-32 as RFC 7932 Appendix C computes it.
std::uint32_t crc32(std::uint8_t const* data, std::size_t size)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void check_stream(char const* name, std::vector<std::uint8_t> const& expected)
{
    std::optional<std::vector<std::uint8_t>> const stream = read_file(g_fixtures / name);
    CHECK(stream.has_value());
    if (!stream)
        return;
    std::optional<std::vector<std::uint8_t>> const out = brotli_decompress(*stream);
    CHECK(out.has_value());
    if (out)
        CHECK(*out == expected);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: test_brotli <fixtures-dir>\n";
        return 2;
    }
    g_fixtures = argv[1];

    // The fixed tables, against the CRC-32s the RFC gives for them — after
    // the CRC itself, against its standard check value.
    CHECK_EQ(crc32(reinterpret_cast<std::uint8_t const*>("123456789"), 9), 0xCBF43926u);
    CHECK_EQ(brotli_dictionary_size(), std::size_t { 122784 });
    CHECK_EQ(crc32(brotli_dictionary_bytes(), brotli_dictionary_size()), 0x5136cb04u);
    std::vector<std::uint8_t> const transforms = brotli_transform_bytes();
    CHECK_EQ(transforms.size(), std::size_t { 648 });
    CHECK_EQ(crc32(transforms.data(), transforms.size()), 0x3d965f81u);
    CHECK_EQ(crc32(brotli_context_lookup(0), 256), 0x8e91efb7u);
    CHECK_EQ(crc32(brotli_context_lookup(1), 256), 0xd01a32f4u);
    CHECK_EQ(crc32(brotli_context_lookup(2), 256), 0x0dd7a0d6u);

    // Streams the reference compressor wrote, back to their originals: text
    // the dictionary and context modeling work on, a random block that goes
    // over uncompressed, a long run of zeros, and nothing at all.
    std::vector<std::uint8_t> const words = read_file(g_fixtures / "words.txt").value_or(std::vector<std::uint8_t>());
    std::vector<std::uint8_t> const words6 = read_file(g_fixtures / "words6.txt").value_or(std::vector<std::uint8_t>());
    std::vector<std::uint8_t> const mixed = read_file(g_fixtures / "mixed.txt").value_or(std::vector<std::uint8_t>());
    std::vector<std::uint8_t> const noise = read_file(g_fixtures / "random.bin").value_or(std::vector<std::uint8_t>());
    CHECK(!words.empty() && !words6.empty() && !mixed.empty());
    CHECK_EQ(noise.size(), std::size_t { 4096 });
    check_stream("words.txt.q11.br", words);
    check_stream("words6.txt.q1.br", words6);
    check_stream("words6.txt.q11.br", words6);
    check_stream("words6.txt.w24.br", words6);
    check_stream("mixed.txt.q0.br", mixed);
    check_stream("mixed.txt.q2.br", mixed);
    check_stream("mixed.txt.q5.br", mixed);
    check_stream("mixed.txt.q11.br", mixed);
    check_stream("mixed.txt.w10.br", mixed);
    check_stream("random.bin.q11.br", noise);
    check_stream("zeros.bin.q11.br", std::vector<std::uint8_t>(100000, 0));
    check_stream("empty.bin.q11.br", std::vector<std::uint8_t>());

    // A stream cut anywhere before its end is refused, and the cap holds to
    // the byte: one short of the output refuses, the exact size decodes.
    if (std::optional<std::vector<std::uint8_t>> const stream = read_file(g_fixtures / "mixed.txt.q11.br")) {
        bool every_cut_refused = true;
        for (std::size_t cut = 0; cut < stream->size(); ++cut) {
            if (brotli_decompress(stream->data(), cut).has_value())
                every_cut_refused = false;
        }
        CHECK(every_cut_refused);
        CHECK(!brotli_decompress(*stream, mixed.size() - 1).has_value());
        CHECK(brotli_decompress(*stream, mixed.size()).has_value());
    }
    if (std::optional<std::vector<std::uint8_t>> const stream = read_file(g_fixtures / "zeros.bin.q11.br")) {
        CHECK(!brotli_decompress(*stream, 99999).has_value());
        CHECK(brotli_decompress(*stream, 100000).has_value());
    }

    // The one window code the RFC forbids, and a fill bit that is not zero
    // after the last meta-block.
    CHECK(!brotli_decompress(std::vector<std::uint8_t> { 0x11 }).has_value());
    if (std::optional<std::vector<std::uint8_t>> const empty = read_file(g_fixtures / "empty.bin.q11.br")) {
        CHECK_EQ(empty->size(), std::size_t { 1 });
        std::vector<std::uint8_t> dirty = *empty;
        dirty[0] = static_cast<std::uint8_t>(dirty[0] | 0x80);
        CHECK(!brotli_decompress(dirty).has_value());
    }

    // Hostile bytes: every single-bit flip of a real stream and a pile of
    // random buffers decode to something or to nothing, and never past the
    // cap — what the sanitizer lane runs this for.
    if (std::optional<std::vector<std::uint8_t>> const stream = read_file(g_fixtures / "words6.txt.q11.br")) {
        bool capped = true;
        for (std::size_t at = 0; at < stream->size(); ++at) {
            for (int bit = 0; bit < 8; ++bit) {
                std::vector<std::uint8_t> flipped = *stream;
                flipped[at] = static_cast<std::uint8_t>(flipped[at] ^ (1u << bit));
                std::optional<std::vector<std::uint8_t>> const out = brotli_decompress(flipped, 1u << 20);
                if (out && out->size() > (1u << 20))
                    capped = false;
            }
        }
        CHECK(capped);
    }
    std::mt19937 generator(7);
    for (int round = 0; round < 2000; ++round) {
        std::vector<std::uint8_t> garbage(generator() % 64);
        for (std::uint8_t& byte : garbage)
            byte = static_cast<std::uint8_t>(generator());
        std::optional<std::vector<std::uint8_t>> const out = brotli_decompress(garbage, 1u << 16);
        CHECK(!out || out->size() <= (1u << 16));
    }

    // Content-Encoding: br, the way the loader decodes a response body.
    if (std::optional<std::vector<std::uint8_t>> const stream = read_file(g_fixtures / "words.txt.q11.br")) {
        std::optional<std::vector<std::uint8_t>> const decoded = net::decode_content("br", *stream, 1u << 20);
        CHECK(decoded && *decoded == words);
    }

    return sashfold::test::report("brotli");
}
