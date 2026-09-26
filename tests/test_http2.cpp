// HTTP/2 and HPACK: the examples of RFC 7541 Appendix C byte for byte, a
// few thousand random header lists through one encoder and decoder, every
// frame type written and read back with the ways a frame can be malformed,
// and then net::fetch against a loopback HTTP/2 server written here over
// the same framing and HPACK code: one request, six at once on one
// connection with their data interleaved, a body far past the receive
// window, a server that allows one stream at a time, one that goes away
// mid-flight, pings, a refused push, a header block across CONTINUATION
// frames, a server that answers the preface with something else, an https
// origin whose handshakes hang and fail (six fetches, one connection, one
// wait), a stream the server never answers holding the only slot, and a
// large POST to a server busy sending a large answer. Plain
// TCP speaks HTTP/2 here by prior knowledge, since ALPN needs TLS; the one
// check through TLS runs against node's own HTTP/2 server on loopback, with
// a throwaway certificate from openssl, and SKIPs without them.
#include "Test.h"

#include "net/Connections.h"
#include "net/Cookies.h"
#include "net/Hpack.h"
#include "net/Http.h"
#include "net/Http2.h"
#include "platform/Net.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace sashfold;
namespace hpack = sashfold::net::hpack;
namespace h2 = sashfold::net::h2;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes from_hex(std::string_view text)
{
    Bytes out;
    int high = -1;
    for (char const c : text) {
        int value = -1;
        if (c >= '0' && c <= '9')
            value = c - '0';
        else if (c >= 'a' && c <= 'f')
            value = c - 'a' + 10;
        if (value < 0)
            continue;
        if (high < 0) {
            high = value;
        } else {
            out.push_back(static_cast<std::uint8_t>(high * 16 + value));
            high = -1;
        }
    }
    return out;
}

std::string hex(std::span<std::uint8_t const> bytes)
{
    std::string out;
    char buffer[3];
    for (std::uint8_t const b : bytes) {
        std::snprintf(buffer, sizeof buffer, "%02x", b);
        out += buffer;
    }
    return out;
}

std::string text_of(Bytes const& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

using hpack::Field;

std::vector<Field> fields(std::initializer_list<std::pair<char const*, char const*>> list)
{
    std::vector<Field> out;
    for (auto const& [name, value] : list)
        out.push_back(Field { name, value, false });
    return out;
}

// The table's entries, newest first, as "name: value".
std::vector<std::string> entries(hpack::DynamicTable const& table)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < table.count(); ++i)
        out.push_back(table.at(i).name + ": " + table.at(i).value);
    return out;
}

bool same_fields(std::optional<std::vector<Field>> const& decoded, std::vector<Field> const& expected)
{
    if (!decoded || decoded->size() != expected.size())
        return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if ((*decoded)[i].name != expected[i].name || (*decoded)[i].value != expected[i].value)
            return false;
    }
    return true;
}

// ---- HPACK

void test_integers()
{
    // C.1.1 to C.1.3.
    Bytes out;
    hpack::encode_integer(out, 10, 5);
    CHECK_EQ(hex(out), std::string("0a"));
    out.clear();
    hpack::encode_integer(out, 1337, 5);
    CHECK_EQ(hex(out), std::string("1f9a0a"));
    out.clear();
    hpack::encode_integer(out, 42, 8);
    CHECK_EQ(hex(out), std::string("2a"));

    Bytes const coded = from_hex("1f9a0a");
    std::size_t at = 0;
    CHECK_EQ(hpack::decode_integer(coded, at, 5).value_or(0), 1337u);
    CHECK_EQ(at, 3u);
    // Cut short, and past 32 bits.
    Bytes const short_one = from_hex("1f9a");
    at = 0;
    CHECK(!hpack::decode_integer(short_one, at, 5));
    Bytes const huge = from_hex("1fffffffff7f");
    at = 0;
    CHECK(!hpack::decode_integer(huge, at, 5));
    // Every prefix, round trip, across the boundary values.
    for (int prefix = 1; prefix <= 8; ++prefix) {
        for (std::uint64_t const value : { 0ull, 1ull, 30ull, 31ull, 127ull, 128ull, 255ull, 256ull, 16383ull, 1000000ull, 0xffffffffull }) {
            Bytes encoded;
            hpack::encode_integer(encoded, value, prefix, 0);
            std::size_t read = 0;
            std::optional<std::uint64_t> const back = hpack::decode_integer(encoded, read, prefix);
            CHECK(back && *back == value && read == encoded.size());
        }
    }
}

void test_huffman()
{
    struct Case {
        char const* text;
        char const* coded;
    };
    // The strings of C.4 and C.6.
    Case const cases[] = {
        { "www.example.com", "f1e3c2e5f23a6ba0ab90f4ff" },
        { "no-cache", "a8eb10649cbf" },
        { "custom-key", "25a849e95ba97d7f" },
        { "custom-value", "25a849e95bb8e8b4bf" },
        { "302", "6402" },
        { "private", "aec3771a4b" },
        { "Mon, 21 Oct 2013 20:13:21 GMT", "d07abe941054d444a8200595040b8166e082a62d1bff" },
        { "https://www.example.com", "9d29ad171863c78f0b97c8e9ae82ae43d3" },
        { "307", "640eff" },
        { "gzip", "9bd9ab" },
        { "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1",
            "94e7821dd7f2e6c7b335dfdfcd5b3960d5af27087f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007" },
    };
    for (Case const& c : cases) {
        Bytes out;
        hpack::huffman_encode(out, c.text);
        CHECK_EQ(hex(out), std::string(c.coded));
        CHECK_EQ(hpack::huffman_encoded_length(c.text), out.size());
        CHECK_EQ(hpack::huffman_decode(from_hex(c.coded), 1024).value_or("<failed>"), std::string(c.text));
    }
    // All 256 octets, each alone and all together.
    std::string every;
    for (int i = 0; i < 256; ++i)
        every += static_cast<char>(i);
    Bytes coded;
    hpack::huffman_encode(coded, every);
    CHECK(hpack::huffman_decode(coded, 4096) == every);
    int singles_ok = 0;
    for (int i = 0; i < 256; ++i) {
        std::string const one(1, static_cast<char>(i));
        Bytes single;
        hpack::huffman_encode(single, one);
        if (hpack::huffman_decode(single, 16) == one)
            ++singles_ok;
    }
    CHECK_EQ(singles_ok, 256);

    // 'a' is 00011: three bits of padding, which must be ones.
    CHECK(hpack::huffman_decode(from_hex("1f"), 16) == std::string("a"));
    CHECK(!hpack::huffman_decode(from_hex("18"), 16)); // padding of zeros
    CHECK(!hpack::huffman_decode(from_hex("1fff"), 16)); // eleven bits of padding
    CHECK(!hpack::huffman_decode(from_hex("ffffffff"), 16)); // the end-of-string symbol itself
    CHECK(!hpack::huffman_decode(from_hex("fe"), 16)); // seven ones and a zero: no symbol, not padding
    CHECK(!hpack::huffman_decode(coded, 100)); // longer than allowed
}

void test_representations()
{
    // C.2.1 to C.2.4, each decoded and encoded, with the table after it.
    {
        hpack::Decoder decoder;
        Bytes const block = from_hex("400a637573746f6d2d6b65790d637573746f6d2d686561646572");
        CHECK(same_fields(decoder.decode(block), fields({ { "custom-key", "custom-header" } })));
        CHECK_EQ(decoder.table().size(), 55u);
        CHECK_EQ(decoder.table().count(), 1u);
        hpack::Encoder encoder;
        encoder.set_huffman(false);
        Bytes out;
        encoder.encode_field(Field { "custom-key", "custom-header", false }, hpack::Representation::IncrementalIndexing, out);
        CHECK_EQ(hex(out), hex(block));
        CHECK_EQ(encoder.table().size(), 55u);
    }
    {
        hpack::Decoder decoder;
        Bytes const block = from_hex("040c2f73616d706c652f70617468");
        CHECK(same_fields(decoder.decode(block), fields({ { ":path", "/sample/path" } })));
        CHECK_EQ(decoder.table().count(), 0u);
        hpack::Encoder encoder;
        encoder.set_huffman(false);
        Bytes out;
        encoder.encode_field(Field { ":path", "/sample/path", false }, hpack::Representation::WithoutIndexing, out);
        CHECK_EQ(hex(out), hex(block));
        CHECK_EQ(encoder.table().count(), 0u);
    }
    {
        hpack::Decoder decoder;
        Bytes const block = from_hex("100870617373776f726406736563726574");
        std::optional<std::vector<Field>> const decoded = decoder.decode(block);
        CHECK(same_fields(decoded, fields({ { "password", "secret" } })));
        CHECK(decoded && (*decoded)[0].never_indexed);
        CHECK_EQ(decoder.table().count(), 0u);
        hpack::Encoder encoder;
        encoder.set_huffman(false);
        Bytes out;
        encoder.encode_field(Field { "password", "secret", false }, hpack::Representation::NeverIndexed, out);
        CHECK_EQ(hex(out), hex(block));
    }
    {
        hpack::Decoder decoder;
        CHECK(same_fields(decoder.decode(from_hex("82")), fields({ { ":method", "GET" } })));
        CHECK_EQ(decoder.table().count(), 0u);
        hpack::Encoder encoder;
        Bytes out;
        encoder.encode_field(Field { ":method", "GET", false }, hpack::Representation::Indexed, out);
        CHECK_EQ(hex(out), std::string("82"));
    }
}

// Three blocks through one encoder and one decoder, as C.3 to C.6 run them.
void run_sequence(char const* name, std::size_t table_size, bool huffman, std::vector<std::vector<Field>> const& lists,
    std::vector<char const*> const& blocks, std::vector<std::vector<std::string>> const& tables,
    std::vector<std::size_t> const& sizes)
{
    hpack::Encoder encoder(table_size);
    encoder.set_huffman(huffman);
    hpack::Decoder decoder(table_size);
    if (table_size != 4096) {
        // Both sides settled on the size beforehand; the decoder hears of
        // it as the protocol has it, in a size update.
        Bytes update;
        hpack::encode_integer(update, table_size, 5, 0x20);
        CHECK(decoder.decode(update) && decoder.table().max_size() == table_size);
    }
    for (std::size_t i = 0; i < lists.size(); ++i) {
        Bytes const encoded = encoder.encode(lists[i]);
        if (!CHECK_EQ(hex(encoded), hex(from_hex(blocks[i]))))
            std::printf("  %s: block %zu\n", name, i + 1);
        CHECK(same_fields(decoder.decode(from_hex(blocks[i])), lists[i]));
        CHECK(entries(decoder.table()) == tables[i]);
        CHECK(entries(encoder.table()) == tables[i]);
        CHECK_EQ(decoder.table().size(), sizes[i]);
        CHECK_EQ(encoder.table().size(), sizes[i]);
    }
}

void test_appendix_c()
{
    std::vector<std::vector<Field>> const requests = {
        fields({ { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" } }),
        fields({ { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" },
            { "cache-control", "no-cache" } }),
        fields({ { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/index.html" }, { ":authority", "www.example.com" },
            { "custom-key", "custom-value" } }),
    };
    std::vector<std::vector<std::string>> const request_tables = {
        { ":authority: www.example.com" },
        { "cache-control: no-cache", ":authority: www.example.com" },
        { "custom-key: custom-value", "cache-control: no-cache", ":authority: www.example.com" },
    };
    run_sequence("C.3", 4096, false, requests,
        { "828684410f7777772e6578616d706c652e636f6d", "828684be58086e6f2d6361636865",
            "828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565" },
        request_tables, { 57, 110, 164 });
    run_sequence("C.4", 4096, true, requests,
        { "828684418cf1e3c2e5f23a6ba0ab90f4ff", "828684be5886a8eb10649cbf",
            "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf" },
        request_tables, { 57, 110, 164 });

    char const* const cookie = "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1";
    std::vector<std::vector<Field>> const responses = {
        fields({ { ":status", "302" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" },
            { "location", "https://www.example.com" } }),
        fields({ { ":status", "307" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" },
            { "location", "https://www.example.com" } }),
        fields({ { ":status", "200" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:22 GMT" },
            { "location", "https://www.example.com" }, { "content-encoding", "gzip" }, { "set-cookie", cookie } }),
    };
    std::vector<std::vector<std::string>> const response_tables = {
        { "location: https://www.example.com", "date: Mon, 21 Oct 2013 20:13:21 GMT", "cache-control: private", ":status: 302" },
        { ":status: 307", "location: https://www.example.com", "date: Mon, 21 Oct 2013 20:13:21 GMT", "cache-control: private" },
        { std::string("set-cookie: ") + cookie, "content-encoding: gzip", "date: Mon, 21 Oct 2013 20:13:22 GMT" },
    };
    run_sequence("C.5", 256, false, responses,
        { "4803333032580770726976617465611d4d6f6e2c203231204f637420323031332032303a31333a323120474d546e17"
          "68747470733a2f2f7777772e6578616d706c652e636f6d",
            "4803333037c1c0bf",
            "88c1611d4d6f6e2c203231204f637420323031332032303a31333a323220474d54c05a04677a69707738666f6f3d41"
            "53444a4b48514b425a584f5157454f50495541585157454f49553b206d61782d6167653d333630303b207665727369"
            "6f6e3d31" },
        response_tables, { 222, 222, 215 });
    run_sequence("C.6", 256, true, responses,
        { "488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a62d1bff6e919d29ad171863c78f0b97"
          "c8e9ae82ae43d3",
            "4883640effc1c0bf",
            "88c16196d07abe941054d444a8200595040b8166e084a62d1bffc05a839bd9ab77ad94e7821dd7f2e6c7b335dfdfcd"
            "5b3960d5af27087f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007" },
        response_tables, { 222, 222, 215 });
}

void test_table_bounds()
{
    // Evictions by hand-computed sizes: each entry is name + value + 32.
    hpack::DynamicTable table(100);
    table.add("aaaa", "bbbb"); // 40
    table.add("cccc", "dddd"); // 40, 80 in all
    CHECK_EQ(table.size(), 80u);
    table.add("eeeee", "fffff"); // 42: the oldest goes, 82
    CHECK_EQ(table.size(), 82u);
    CHECK_EQ(table.count(), 2u);
    CHECK_EQ(table.at(1).name, std::string("cccc"));
    table.set_max_size(50); // only the newest fits
    CHECK_EQ(table.size(), 42u);
    CHECK_EQ(table.count(), 1u);
    table.add(std::string(60, 'x'), "y"); // 93 > 50: the table empties
    CHECK_EQ(table.count(), 0u);
    CHECK_EQ(table.size(), 0u);

    // Size updates: the encoder names the smallest size it passed through,
    // then the last; the decoder takes them only at the start of a block
    // and never above what it advertised.
    hpack::Encoder encoder;
    encoder.set_max_table_size(100);
    encoder.set_max_table_size(300);
    Bytes const block = encoder.encode(fields({ { ":method", "GET" } }));
    CHECK_EQ(hex(block), std::string("3f453f8d0282"));
    hpack::Decoder decoder(4096);
    CHECK(same_fields(decoder.decode(block), fields({ { ":method", "GET" } })));
    CHECK_EQ(decoder.table().max_size(), 300u);
    hpack::Decoder small(200);
    CHECK(!small.decode(block)); // 300 is above the 200 it allows
    hpack::Decoder late;
    CHECK(!late.decode(from_hex("823f45"))); // an update after a field

    // Broken blocks.
    hpack::Decoder broken;
    CHECK(!broken.decode(from_hex("80"))); // index 0
    hpack::Decoder beyond;
    CHECK(!beyond.decode(from_hex("be"))); // index 62 with an empty table
    hpack::Decoder cut;
    CHECK(!cut.decode(from_hex("400a6375"))); // a string cut short
    hpack::Decoder bad_code;
    CHECK(!bad_code.decode(from_hex("40811800"))); // a Huffman name padded with zeros
    // A list bound of 64: one 55-octet field fits, two do not.
    hpack::Decoder bounded(4096, 64);
    CHECK(bounded.decode(from_hex("400a637573746f6d2d6b65790d637573746f6d2d686561646572")).has_value());
    hpack::Decoder overfull(4096, 64);
    CHECK(!overfull.decode(from_hex("400a637573746f6d2d6b65790d637573746f6d2d686561646572be")));

    // Cookies and credentials go never indexed, and stay out of the table.
    hpack::Encoder sensitive;
    sensitive.set_huffman(false);
    Bytes const secret = sensitive.encode(fields({ { "cookie", "a=1" }, { "authorization", "Basic x" } }));
    CHECK_EQ(hex(secret).substr(0, 4), std::string("1f11")); // never indexed, name 32
    CHECK_EQ(sensitive.table().count(), 0u);
    hpack::Decoder reads_secret;
    std::optional<std::vector<Field>> const got = reads_secret.decode(secret);
    CHECK(got && got->size() == 2 && (*got)[0].never_indexed && (*got)[1].never_indexed);
    CHECK_EQ(reads_secret.table().count(), 0u);
}

void test_random_lists()
{
    std::mt19937 generator(20260926u);
    auto const pick = [&](std::size_t n) { return static_cast<std::size_t>(generator() % n); };
    std::vector<std::string> const names = { ":method", ":path", ":authority", "accept", "cookie", "user-agent",
        "x-custom", "referer", "cache-control", "authorization", "etag", "if-none-match", "content-type" };
    hpack::Encoder encoder;
    hpack::Decoder decoder(4096);
    int lists_ok = 0;
    int tables_in_step = 0;
    int const rounds = 3000;
    for (int round = 0; round < rounds; ++round) {
        if (pick(50) == 0) {
            std::size_t const size = pick(4097);
            encoder.set_max_table_size(size);
            if (pick(2) == 0)
                encoder.set_max_table_size(pick(4097));
        }
        encoder.set_huffman(pick(2) == 0);
        std::vector<Field> list;
        std::size_t const count = 1 + pick(12);
        for (std::size_t i = 0; i < count; ++i) {
            Field field;
            if (pick(3) == 0) {
                std::size_t const length = 1 + pick(12);
                for (std::size_t j = 0; j < length; ++j)
                    field.name += static_cast<char>('a' + pick(26));
            } else {
                field.name = names[pick(names.size())];
            }
            // Values repeat often, as real ones do, and are any octets.
            if (pick(2) == 0) {
                field.value = "value-" + std::to_string(pick(20));
            } else {
                std::size_t const length = pick(60);
                for (std::size_t j = 0; j < length; ++j)
                    field.value += static_cast<char>(pick(256));
            }
            field.never_indexed = pick(10) == 0;
            list.push_back(std::move(field));
        }
        Bytes const block = encoder.encode(list);
        std::optional<std::vector<Field>> const decoded = decoder.decode(block);
        bool ok = decoded && decoded->size() == list.size();
        for (std::size_t i = 0; ok && i < list.size(); ++i) {
            bool const sensitive = list[i].never_indexed || list[i].name == "cookie" || list[i].name == "authorization";
            ok = (*decoded)[i].name == list[i].name && (*decoded)[i].value == list[i].value
                && (*decoded)[i].never_indexed == sensitive;
        }
        if (ok)
            ++lists_ok;
        if (entries(encoder.table()) == entries(decoder.table()) && encoder.table().size() == decoder.table().size())
            ++tables_in_step;
    }
    CHECK_EQ(lists_ok, rounds);
    CHECK_EQ(tables_in_step, rounds);
}

// ---- framing

std::vector<h2::Frame> read_all(Bytes const& wire, std::uint32_t max_frame = h2::default_max_frame_size)
{
    // A byte at a time, as a slow socket might hand it over.
    h2::FrameReader reader(max_frame);
    std::vector<h2::Frame> frames;
    for (std::uint8_t const byte : wire) {
        reader.feed(std::span<std::uint8_t const>(&byte, 1));
        while (std::optional<h2::Frame> frame = reader.next())
            frames.push_back(std::move(*frame));
    }
    return frames;
}

std::optional<h2::ErrorCode> refusal(h2::Frame const& frame)
{
    std::optional<h2::FrameError> const error = h2::check_frame(frame);
    if (!error)
        return std::nullopt;
    return error->code;
}

h2::Frame frame_of(h2::FrameType type, std::uint8_t frame_flags, std::uint32_t stream, Bytes payload)
{
    h2::Frame frame;
    frame.type = static_cast<std::uint8_t>(type);
    frame.flags = frame_flags;
    frame.stream = stream;
    frame.payload = std::move(payload);
    return frame;
}

void test_framing()
{
    CHECK_EQ(h2::client_preface.size(), 24u);
    Bytes wire;
    Bytes const data = { 'h', 'e', 'l', 'l', 'o' };
    h2::write_data(wire, 1, data, true, std::uint8_t { 3 });
    h2::write_data(wire, 3, data, false);
    Bytes const block(40000, 0x82);
    h2::write_headers(wire, 5, block, true, 16384, std::uint8_t { 2 });
    std::pair<h2::Setting, std::uint32_t> const settings[] = {
        { h2::Setting::MaxConcurrentStreams, 7 }, { h2::Setting::InitialWindowSize, 1000 } };
    h2::write_settings(wire, settings);
    h2::write_settings_ack(wire);
    std::uint8_t const ping[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    h2::write_ping(wire, ping, false);
    h2::write_goaway(wire, 9, h2::ErrorCode::EnhanceYourCalm, "slow down");
    h2::write_rst_stream(wire, 7, h2::ErrorCode::Cancel);
    h2::write_window_update(wire, 0, 12345);
    h2::write_priority(wire, 11, 1, 200);

    std::vector<h2::Frame> const frames = read_all(wire);
    CHECK_EQ(frames.size(), 12u);
    if (frames.size() != 12)
        return;
    // DATA, padded and not.
    CHECK(frames[0].is(h2::FrameType::Data) && frames[0].has(h2::flags::padded) && frames[0].has(h2::flags::end_stream));
    CHECK_EQ(frames[0].payload.size(), 9u);
    CHECK(!h2::check_frame(frames[0]));
    CHECK_EQ(text_of(Bytes(h2::frame_content(frames[0]).begin(), h2::frame_content(frames[0]).end())), std::string("hello"));
    CHECK(frames[1].stream == 3 && !frames[1].has(h2::flags::end_stream) && h2::frame_content(frames[1]).size() == 5);
    // A 40000-octet block: HEADERS (padded) then two CONTINUATIONs, the last ending it.
    CHECK(frames[2].is(h2::FrameType::Headers) && frames[2].has(h2::flags::padded) && !frames[2].has(h2::flags::end_headers));
    CHECK_EQ(frames[2].payload.size(), 16384u);
    CHECK(frames[3].is(h2::FrameType::Continuation) && !frames[3].has(h2::flags::end_headers) && frames[3].payload.size() == 16384);
    CHECK(frames[4].is(h2::FrameType::Continuation) && frames[4].has(h2::flags::end_headers));
    CHECK_EQ(h2::frame_content(frames[2]).size() + frames[3].payload.size() + frames[4].payload.size(), 40000u);
    // SETTINGS and its acknowledgement.
    h2::FrameError error;
    auto const parsed = h2::parse_settings(frames[5], error);
    CHECK(parsed && parsed->size() == 2 && (*parsed)[0].second == 7 && (*parsed)[1].second == 1000);
    CHECK(frames[6].is(h2::FrameType::Settings) && frames[6].has(h2::flags::ack) && frames[6].payload.empty());
    CHECK(frames[7].is(h2::FrameType::Ping) && frames[7].payload == Bytes(std::begin(ping), std::end(ping)));
    h2::Goaway const goaway = h2::parse_goaway(frames[8]);
    CHECK(goaway.last_stream == 9 && goaway.code == h2::ErrorCode::EnhanceYourCalm && goaway.debug == "slow down");
    CHECK(frames[9].stream == 7 && h2::parse_rst_stream(frames[9]) == h2::ErrorCode::Cancel);
    CHECK(frames[10].stream == 0 && h2::parse_window_update(frames[10]) == 12345u);
    CHECK(frames[11].is(h2::FrameType::Priority) && !h2::check_frame(frames[11]));
    int well_formed = 0;
    for (h2::Frame const& frame : frames)
        well_formed += h2::check_frame(frame) ? 0 : 1;
    CHECK_EQ(well_formed, 12);

    // Malformed frames, each with the code Section 6 gives it.
    using h2::ErrorCode;
    using h2::FrameType;
    CHECK(refusal(frame_of(FrameType::Data, 0, 0, { 1 })) == ErrorCode::ProtocolError);
    CHECK(refusal(frame_of(FrameType::Data, h2::flags::padded, 1, { 4, 'a', 0, 0 })) == ErrorCode::ProtocolError);
    CHECK(refusal(frame_of(FrameType::Data, h2::flags::padded, 1, {})) == ErrorCode::FrameSizeError);
    CHECK(!refusal(frame_of(FrameType::Data, h2::flags::padded, 1, { 2, 0, 0 }))); // all padding is allowed
    CHECK(refusal(frame_of(FrameType::Headers, h2::flags::padded | h2::flags::priority, 1, { 0, 0, 0 })) == ErrorCode::FrameSizeError);
    CHECK(refusal(frame_of(FrameType::Headers, 0, 0, { 0x82 })) == ErrorCode::ProtocolError);
    std::optional<h2::FrameError> const priority = h2::check_frame(frame_of(FrameType::Priority, 0, 1, { 0, 0, 0, 0 }));
    CHECK(priority && priority->code == ErrorCode::FrameSizeError && !priority->connection);
    CHECK(refusal(frame_of(FrameType::RstStream, 0, 1, { 0, 0, 0 })) == ErrorCode::FrameSizeError);
    CHECK(refusal(frame_of(FrameType::RstStream, 0, 0, { 0, 0, 0, 8 })) == ErrorCode::ProtocolError);
    CHECK(refusal(frame_of(FrameType::Settings, h2::flags::ack, 0, { 0, 0, 0, 0, 0, 0 })) == ErrorCode::FrameSizeError);
    CHECK(refusal(frame_of(FrameType::Settings, 0, 0, { 0, 1, 0, 0, 0 })) == ErrorCode::FrameSizeError);
    CHECK(refusal(frame_of(FrameType::Settings, 0, 1, {})) == ErrorCode::ProtocolError);
    CHECK(refusal(frame_of(FrameType::PushPromise, h2::flags::end_headers, 1, { 0, 0, 0, 2, 0x82 })) == ErrorCode::ProtocolError);
    CHECK(refusal(frame_of(FrameType::Ping, 0, 0, { 1, 2, 3, 4, 5, 6, 7 })) == ErrorCode::FrameSizeError);
    CHECK(refusal(frame_of(FrameType::Ping, 0, 1, Bytes(8, 0))) == ErrorCode::ProtocolError);
    CHECK(refusal(frame_of(FrameType::Goaway, 0, 0, { 0, 0, 0, 0 })) == ErrorCode::FrameSizeError);
    CHECK(refusal(frame_of(FrameType::Goaway, 0, 3, Bytes(8, 0))) == ErrorCode::ProtocolError);
    CHECK(refusal(frame_of(FrameType::WindowUpdate, 0, 0, { 0, 0, 0 })) == ErrorCode::FrameSizeError);
    CHECK(refusal(frame_of(FrameType::WindowUpdate, 0, 0, { 0, 0, 0, 0 })) == ErrorCode::ProtocolError);
    std::optional<h2::FrameError> const zero_window = h2::check_frame(frame_of(FrameType::WindowUpdate, 0, 5, { 0x80, 0, 0, 0 }));
    CHECK(zero_window && zero_window->code == ErrorCode::ProtocolError && !zero_window->connection);
    CHECK(refusal(frame_of(FrameType::Continuation, h2::flags::end_headers, 0, { 0x82 })) == ErrorCode::ProtocolError);
    h2::Frame unknown;
    unknown.type = 0x42;
    unknown.payload = { 1, 2, 3 };
    CHECK(!h2::check_frame(unknown));

    // SETTINGS values out of bounds, and an identifier nobody knows.
    auto const settings_frame = [](std::uint16_t id, std::uint32_t value) {
        Bytes payload = { static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id),
            static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value) };
        return frame_of(FrameType::Settings, 0, 0, std::move(payload));
    };
    h2::FrameError bound;
    CHECK(!h2::parse_settings(settings_frame(4, 0x80000000u), bound) && bound.code == ErrorCode::FlowControlError);
    CHECK(!h2::parse_settings(settings_frame(5, 16383), bound) && bound.code == ErrorCode::ProtocolError);
    CHECK(!h2::parse_settings(settings_frame(5, 16777216), bound) && bound.code == ErrorCode::ProtocolError);
    CHECK(!h2::parse_settings(settings_frame(2, 2), bound) && bound.code == ErrorCode::ProtocolError);
    auto const skipped = h2::parse_settings(settings_frame(0x99, 5), bound);
    CHECK(skipped && skipped->empty());

    // A frame past the advertised size ends the connection.
    Bytes oversize;
    h2::write_data(oversize, 1, Bytes(16385, 0), false);
    h2::FrameReader reader;
    reader.feed(oversize);
    CHECK(!reader.next());
    CHECK(reader.error() && reader.error()->code == ErrorCode::FrameSizeError && reader.error()->connection);
    h2::FrameReader larger(32768);
    larger.feed(oversize);
    CHECK(larger.next().has_value());
}

// ---- a loopback HTTP/2 server

// The body a path is answered with: /size/N is N patterned octets, anything
// else says its path, so every stream's body is its own.
Bytes body_for(std::string const& path)
{
    if (path.starts_with("/size/")) {
        std::size_t const size = static_cast<std::size_t>(std::strtoull(path.c_str() + 6, nullptr, 10));
        Bytes out(size);
        for (std::size_t i = 0; i < size; ++i)
            out[i] = static_cast<std::uint8_t>((i * 31 + (i >> 11)) & 0xff);
        return out;
    }
    std::string text = "body of " + path;
    if (path.starts_with("/big")) {
        while (text.size() < 50000)
            text += " " + path;
    }
    return Bytes(text.begin(), text.end());
}

class H2Server {
public:
    struct Options {
        std::uint32_t max_concurrent = 0; // sent in SETTINGS when set
        bool interleave = false; // one DATA frame per stream in turn
        std::size_t chunk = 16384;
        std::size_t gather = 0; // answer nothing until this many streams are open (or 3 s pass)
        int delay_ms = 0; // before each response
        std::string slow_path; // this path waits slow_ms more
        int slow_ms = 0;
        bool ping = false; // a PING after SETTINGS
        bool push = false; // a PUSH_PROMISE on the first stream
        bool continuation = false; // response headers over three frames
        bool garbage = false; // the preface answered with an HTTP/1.1 error
        std::size_t goaway_after = 0; // on the first connection, GOAWAY once this many are open
        std::string stalled_path; // this path is never answered
        std::string hangup_path; // on the first connection, this path is met by hanging up
        std::string broken_path; // on the first connection, this path is met by a malformed frame
        bool broken_after_headers = false; // ... sent after the response headers and a little body
        std::string reset_path; // on the first connection, this path is met by RST_STREAM before any answer
        h2::ErrorCode reset_code = h2::ErrorCode::ProtocolError; // ... with this code
        bool open_windows = false; // grants the client the largest windows the protocol allows
        bool shut_windows = false; // gives every stream a send window of 0 and never opens it
    };

    struct Seen {
        std::string path;
        std::vector<Field> fields;
        int connection = 0;
        bool h2 = true;
    };

    struct Report {
        int h2_connections = 0;
        int h1_connections = 0;
        std::vector<Seen> requests;
        int stream_window_updates = 0;
        int connection_window_updates = 0;
        std::size_t max_open = 0;
        int refused = 0;
        std::vector<std::uint32_t> goaways_received;
        std::vector<std::uint32_t> resets_received; // the error code of each RST_STREAM
        bool ping_acknowledged = false;
        std::vector<std::uint32_t> data_order; // the stream of each DATA frame sent
    };

    explicit H2Server(Options options)
        : m_listener(*platform::TcpListener::listen_loopback())
        , m_options(std::move(options))
        , m_accepter([this] { accept_loop(); })
    {
    }
    ~H2Server() { finish(); }

    std::uint16_t port() const { return m_listener.port(); }
    net::Url url(std::string const& path) const
    {
        return *net::parse_url("http://127.0.0.1:" + std::to_string(port()) + path);
    }

    void finish()
    {
        if (!m_accepter.joinable())
            return;
        m_stop = true;
        if (auto poke = platform::TcpSocket::connect("127.0.0.1", port()))
            poke->close();
        m_accepter.join();
        for (std::thread& thread : m_connections)
            thread.join();
        m_connections.clear();
    }

    Report report() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_report;
    }

    // Shuts every connection being served, which ends a send blocked on
    // either side, and hangs up on every connection after: how a test that
    // found a hang gets its threads back, retries and fallbacks included.
    void sever()
    {
        m_severed = true;
        std::lock_guard<std::mutex> const lock(m_mutex);
        for (platform::TcpSocket* socket : m_serving)
            socket->shutdown();
    }

private:
    using Clock = std::chrono::steady_clock;

    void accept_loop()
    {
        int index = 0;
        while (true) {
            std::optional<platform::TcpSocket> client = m_listener.accept();
            if (m_stop || !client)
                return;
            if (m_severed)
                continue;
            m_connections.emplace_back([this, socket = std::move(*client), index]() mutable { serve(socket, index); });
            ++index;
        }
    }

    // Reads until `have` holds at least `want` octets; false once the peer is gone.
    bool fill(platform::TcpSocket& socket, Bytes& have, std::size_t want)
    {
        std::uint8_t buffer[4096];
        while (have.size() < want) {
            if (m_stop)
                return false;
            std::ptrdiff_t const got = socket.receive(buffer, sizeof buffer);
            if (got == 0)
                return false;
            if (got > 0)
                have.insert(have.end(), buffer, buffer + got);
        }
        return true;
    }

    void serve(platform::TcpSocket& socket, int index)
    {
        struct Serving {
            H2Server& server;
            platform::TcpSocket* socket;
            Serving(H2Server& the_server, platform::TcpSocket* the_socket)
                : server(the_server)
                , socket(the_socket)
            {
                std::lock_guard<std::mutex> const lock(server.m_mutex);
                server.m_serving.push_back(socket);
            }
            ~Serving()
            {
                std::lock_guard<std::mutex> const lock(server.m_mutex);
                std::erase(server.m_serving, socket);
            }
            Serving(Serving const&) = delete;
            Serving& operator=(Serving const&) = delete;
        } const serving(*this, &socket);
        socket.set_receive_timeout(2);
        Bytes have;
        if (!fill(socket, have, 4))
            return;
        if (std::memcmp(have.data(), "PRI ", 4) == 0)
            serve_h2(socket, std::move(have), index);
        else
            serve_h1(socket, std::move(have), index);
    }

    void serve_h1(platform::TcpSocket& socket, Bytes have, int index)
    {
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            ++m_report.h1_connections;
        }
        while (!m_stop) {
            std::string const text(have.begin(), have.end());
            std::size_t const end = text.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (!fill(socket, have, have.size() + 1))
                    return;
                continue;
            }
            std::size_t const space = text.find(' ');
            std::string const path = text.substr(space + 1, text.find(' ', space + 1) - space - 1);
            have.erase(have.begin(), have.begin() + static_cast<std::ptrdiff_t>(end + 4));
            {
                std::lock_guard<std::mutex> const lock(m_mutex);
                m_report.requests.push_back(Seen { path, {}, index, false });
            }
            Bytes const body = body_for(path);
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Protocol: http/1.1\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\n\r\n";
            response.append(body.begin(), body.end());
            if (!socket.send_all(reinterpret_cast<std::uint8_t const*>(response.data()), response.size()))
                return;
        }
    }

    struct Outgoing {
        std::uint32_t id = 0;
        std::string path;
        Bytes body;
        std::size_t at = 0;
        bool headers_sent = false;
        std::int64_t window = 0;
        Clock::time_point ready;
    };

    void serve_h2(platform::TcpSocket& socket, Bytes have, int index)
    {
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            ++m_report.h2_connections;
        }
        if (!fill(socket, have, h2::client_preface.size()))
            return;
        if (std::string_view(reinterpret_cast<char const*>(have.data()), h2::client_preface.size()) != h2::client_preface)
            return;
        have.erase(have.begin(), have.begin() + static_cast<std::ptrdiff_t>(h2::client_preface.size()));
        if (m_options.garbage) {
            std::string const refusal = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            socket.send_all(reinterpret_cast<std::uint8_t const*>(refusal.data()), refusal.size());
            return;
        }

        Bytes out;
        std::vector<std::pair<h2::Setting, std::uint32_t>> settings;
        if (m_options.max_concurrent != 0)
            settings.emplace_back(h2::Setting::MaxConcurrentStreams, m_options.max_concurrent);
        if (m_options.open_windows)
            settings.emplace_back(h2::Setting::InitialWindowSize, h2::largest_window);
        if (m_options.shut_windows)
            settings.emplace_back(h2::Setting::InitialWindowSize, 0u);
        h2::write_settings(out, settings);
        if (m_options.open_windows)
            h2::write_window_update(out, 0, h2::largest_window - h2::default_window);
        std::uint8_t const ping_data[8] = { 's', 'a', 's', 'h', 'f', 'o', 'l', 'd' };
        if (m_options.ping)
            h2::write_ping(out, ping_data, false);
        if (!socket.send_all(out.data(), out.size()))
            return;

        h2::FrameReader reader;
        reader.feed(have);
        hpack::Decoder decoder;
        hpack::Encoder encoder;
        std::int64_t connection_window = h2::default_window;
        std::int64_t initial_window = h2::default_window;
        std::map<std::uint32_t, Outgoing> streams;
        std::uint32_t block_stream = 0;
        Bytes block;
        bool gathered = m_options.gather == 0;
        bool goaway_sent = false;
        bool pushed = false;
        auto const started = Clock::now();

        while (!m_stop) {
            std::uint8_t buffer[16384];
            std::ptrdiff_t const got = socket.receive(buffer, sizeof buffer);
            if (got == 0)
                return;
            if (got > 0)
                reader.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(got)));
            out.clear();
            while (std::optional<h2::Frame> frame = reader.next()) {
                switch (static_cast<h2::FrameType>(frame->type)) {
                case h2::FrameType::Settings: {
                    if (frame->has(h2::flags::ack))
                        break;
                    h2::FrameError error;
                    auto const values = h2::parse_settings(*frame, error);
                    for (auto const& [setting, value] : values.value_or(std::vector<std::pair<h2::Setting, std::uint32_t>> {})) {
                        if (setting == h2::Setting::InitialWindowSize) {
                            for (auto& [id, stream] : streams)
                                stream.window += static_cast<std::int64_t>(value) - initial_window;
                            initial_window = value;
                        }
                    }
                    h2::write_settings_ack(out);
                    break;
                }
                case h2::FrameType::Headers:
                case h2::FrameType::Continuation: {
                    std::span<std::uint8_t const> const content = frame->is(h2::FrameType::Headers) ? h2::frame_content(*frame) : std::span<std::uint8_t const>(frame->payload);
                    if (frame->is(h2::FrameType::Headers))
                        block.clear();
                    block_stream = frame->stream;
                    block.insert(block.end(), content.begin(), content.end());
                    if (!frame->has(h2::flags::end_headers))
                        break;
                    std::optional<std::vector<Field>> decoded = decoder.decode(block);
                    if (!decoded)
                        return;
                    Seen seen;
                    seen.connection = index;
                    for (Field const& field : *decoded) {
                        if (field.name == ":path")
                            seen.path = field.value;
                    }
                    seen.fields = std::move(*decoded);
                    std::string const path = seen.path;
                    std::lock_guard<std::mutex> const lock(m_mutex);
                    m_report.requests.push_back(std::move(seen));
                    if (index == 0 && !m_options.hangup_path.empty() && path == m_options.hangup_path) {
                        socket.shutdown();
                        return;
                    }
                    if (index == 0 && !m_options.broken_path.empty() && path == m_options.broken_path) {
                        if (m_options.broken_after_headers) {
                            Bytes const head = encoder.encode(fields({ { ":status", "200" }, { "content-length", "100" } }));
                            h2::write_headers(out, block_stream, head, false, h2::default_max_frame_size);
                            Bytes const part = { 'p', 'a', 'r', 't' };
                            h2::write_data(out, block_stream, part, false);
                        }
                        // A WINDOW_UPDATE three octets long: FRAME_SIZE_ERROR
                        // for the whole connection.
                        Bytes const malformed = { 0, 0, 3, static_cast<std::uint8_t>(h2::FrameType::WindowUpdate), 0, 0, 0, 0, 0, 0, 0, 1 };
                        out.insert(out.end(), malformed.begin(), malformed.end());
                        break;
                    }
                    if (index == 0 && !m_options.reset_path.empty() && path == m_options.reset_path) {
                        h2::write_rst_stream(out, block_stream, m_options.reset_code);
                        break;
                    }
                    if (m_options.max_concurrent != 0 && streams.size() >= m_options.max_concurrent) {
                        ++m_report.refused;
                        h2::write_rst_stream(out, block_stream, h2::ErrorCode::RefusedStream);
                        break;
                    }
                    Outgoing stream;
                    stream.id = block_stream;
                    stream.path = path;
                    stream.body = body_for(path);
                    stream.window = initial_window;
                    int delay = m_options.delay_ms;
                    if (!m_options.slow_path.empty() && path == m_options.slow_path)
                        delay += m_options.slow_ms;
                    stream.ready = path == m_options.stalled_path ? Clock::time_point::max()
                                                                  : Clock::now() + std::chrono::milliseconds(delay);
                    streams.emplace(block_stream, std::move(stream));
                    m_report.max_open = std::max(m_report.max_open, streams.size());
                    if (m_options.push && !pushed) {
                        // A promise of stream 2, which this client refused in its SETTINGS.
                        Bytes promise = { 0, 0, 0, 2 };
                        Bytes const promised = encoder.encode(fields({ { ":method", "GET" }, { ":scheme", "http" },
                            { ":authority", "127.0.0.1" }, { ":path", "/pushed" } }));
                        promise.insert(promise.end(), promised.begin(), promised.end());
                        h2::write_frame(out, h2::FrameType::PushPromise, h2::flags::end_headers, block_stream, promise);
                        pushed = true;
                    }
                    break;
                }
                case h2::FrameType::WindowUpdate: {
                    std::uint32_t const increment = h2::parse_window_update(*frame);
                    std::lock_guard<std::mutex> const lock(m_mutex);
                    if (frame->stream == 0) {
                        connection_window += increment;
                        ++m_report.connection_window_updates;
                    } else {
                        ++m_report.stream_window_updates;
                        auto const found = streams.find(frame->stream);
                        if (found != streams.end())
                            found->second.window += increment;
                    }
                    break;
                }
                case h2::FrameType::Ping:
                    if (frame->has(h2::flags::ack)) {
                        std::lock_guard<std::mutex> const lock(m_mutex);
                        m_report.ping_acknowledged = frame->payload == Bytes(std::begin(ping_data), std::end(ping_data));
                    }
                    break;
                case h2::FrameType::Goaway: {
                    std::lock_guard<std::mutex> const lock(m_mutex);
                    m_report.goaways_received.push_back(static_cast<std::uint32_t>(h2::parse_goaway(*frame).code));
                    break;
                }
                case h2::FrameType::RstStream: {
                    streams.erase(frame->stream);
                    std::lock_guard<std::mutex> const lock(m_mutex);
                    m_report.resets_received.push_back(static_cast<std::uint32_t>(h2::parse_rst_stream(*frame)));
                    break;
                }
                case h2::FrameType::Data:
                case h2::FrameType::Priority:
                case h2::FrameType::PushPromise:
                    break;
                }
            }
            if (reader.error())
                return;

            if (!gathered && (streams.size() >= m_options.gather || Clock::now() - started > std::chrono::seconds(3)))
                gathered = true;
            if (gathered && m_options.goaway_after != 0 && index == 0 && !goaway_sent && !streams.empty()
                && streams.size() >= m_options.goaway_after) {
                // Only the first stream will be answered; the rest were never touched.
                std::uint32_t const last = streams.begin()->first;
                h2::write_goaway(out, last, h2::ErrorCode::NoError);
                std::erase_if(streams, [&](auto const& entry) { return entry.first > last; });
                goaway_sent = true;
            }
            if (gathered)
                respond(out, streams, encoder, connection_window, index);
            if (!out.empty() && !socket.send_all(out.data(), out.size()))
                return;
            if (goaway_sent && streams.empty())
                return;
        }
    }

    void respond(Bytes& out, std::map<std::uint32_t, Outgoing>& streams, hpack::Encoder& encoder,
        std::int64_t& connection_window, int index)
    {
        auto const now = Clock::now();
        bool progress = true;
        while (progress && out.size() < 256 * 1024) {
            progress = false;
            for (auto& [id, stream] : streams) {
                if (now < stream.ready)
                    continue;
                if (!stream.headers_sent) {
                    std::vector<Field> response = fields({ { ":status", "200" }, { "content-type", "text/plain" },
                        { "x-protocol", "h2" } });
                    response.push_back(Field { "content-length", std::to_string(stream.body.size()), false });
                    response.push_back(Field { "x-connection", std::to_string(index), false });
                    if (m_options.continuation)
                        response.push_back(Field { "x-long", std::string(3000, 'q'), false });
                    Bytes const block = encoder.encode(response);
                    if (m_options.continuation) {
                        // Three frames, whatever the size: HEADERS, then two CONTINUATIONs.
                        std::size_t const third = block.size() / 3;
                        std::span<std::uint8_t const> const all(block);
                        h2::write_frame(out, h2::FrameType::Headers, 0, id, all.first(third));
                        h2::write_frame(out, h2::FrameType::Continuation, 0, id, all.subspan(third, third));
                        h2::write_frame(out, h2::FrameType::Continuation, h2::flags::end_headers, id, all.subspan(2 * third));
                    } else {
                        h2::write_headers(out, id, block, stream.body.empty(), h2::default_max_frame_size);
                    }
                    stream.headers_sent = true;
                    progress = true;
                    if (m_options.interleave)
                        continue;
                }
                // DATA as the client's windows allow: one frame in turn when
                // interleaving, else the whole body (up to a send's worth).
                while (stream.at < stream.body.size()) {
                    std::size_t const chunk = std::min<std::size_t>({ stream.body.size() - stream.at, m_options.chunk,
                        static_cast<std::size_t>(std::max<std::int64_t>(0, connection_window)),
                        static_cast<std::size_t>(std::max<std::int64_t>(0, stream.window)), h2::default_max_frame_size });
                    if (chunk == 0)
                        break;
                    h2::write_data(out, id, std::span<std::uint8_t const>(stream.body).subspan(stream.at, chunk),
                        stream.at + chunk == stream.body.size());
                    stream.at += chunk;
                    connection_window -= static_cast<std::int64_t>(chunk);
                    stream.window -= static_cast<std::int64_t>(chunk);
                    progress = true;
                    {
                        std::lock_guard<std::mutex> const lock(m_mutex);
                        m_report.data_order.push_back(id);
                    }
                    if (m_options.interleave || out.size() >= 256 * 1024)
                        break;
                }
            }
            std::erase_if(streams, [](auto const& entry) { return entry.second.headers_sent && entry.second.at == entry.second.body.size(); });
        }
    }

    platform::TcpListener m_listener;
    Options m_options;
    std::atomic<bool> m_stop = false;
    std::atomic<bool> m_severed = false;
    mutable std::mutex m_mutex;
    Report m_report;
    std::vector<platform::TcpSocket*> m_serving; // under m_mutex
    std::vector<std::thread> m_connections; // touched by the accepter until it is joined
    std::thread m_accepter; // last: everything above is ready when it starts
};

net::FetchOptions h2_options(net::ConnectionPool& pool)
{
    net::FetchOptions options;
    options.pool = &pool;
    options.http2_prior_knowledge = true;
    return options;
}

std::string const* field_of(std::vector<Field> const& list, std::string_view name)
{
    for (Field const& field : list) {
        if (field.name == name)
            return &field.value;
    }
    return nullptr;
}

// Runs the fetches on threads of their own, started together.
std::vector<net::FetchResult> fetch_together(std::vector<net::Url> const& urls, net::FetchOptions const& options)
{
    std::vector<net::FetchResult> results(urls.size());
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool open = false;
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < urls.size(); ++i) {
        threads.emplace_back([&, i] {
            {
                std::unique_lock<std::mutex> lock(gate_mutex);
                gate.wait(lock, [&] { return open; });
            }
            results[i] = net::fetch(urls[i], options);
        });
    }
    {
        std::lock_guard<std::mutex> const lock(gate_mutex);
        open = true;
    }
    gate.notify_all();
    for (std::thread& thread : threads)
        thread.join();
    return results;
}

void test_one_request()
{
    H2Server server({});
    net::CookieJar jar;
    {
        net::ConnectionPool pool;
        net::FetchOptions options = h2_options(pool);
        net::Url const url = server.url("/hello?x=1");
        jar.store(url, nullptr, { { "Set-Cookie", "session=abc123" } }, std::time(nullptr));
        options.cookie_jar = &jar;
        options.referrer = "http://127.0.0.1/start";
        options.headers.push_back({ "X-Mixed-Case", "yes" });
        net::FetchResult const result = net::fetch(url, options);
        CHECK(result.response.has_value());
        if (!result.response) {
            std::printf("  one request: %s\n", result.error.c_str());
            return;
        }
        CHECK_EQ(result.response->status, 200);
        CHECK_EQ(text_of(result.response->body), std::string("body of /hello?x=1"));
        CHECK(net::find_header(result.response->headers, "x-protocol") && *net::find_header(result.response->headers, "x-protocol") == "h2");
        CHECK_EQ(result.timing.http2, 1);
        CHECK_EQ(result.timing.requests, 1);
        CHECK_EQ(result.timing.reused, 0);
        // The second request rides the same session.
        net::FetchResult const again = net::fetch(server.url("/again"), options);
        CHECK(again.response && text_of(again.response->body) == "body of /again");
        CHECK_EQ(again.timing.reused, 1);
        CHECK_EQ(pool.stats().sessions, 1u);
        CHECK_EQ(pool.stats().opened, 1u);
    }
    server.finish();
    H2Server::Report const report = server.report();
    CHECK_EQ(report.h2_connections, 1);
    CHECK_EQ(report.h1_connections, 0);
    CHECK_EQ(report.requests.size(), 2u);
    if (report.requests.empty())
        return;
    std::vector<Field> const& sent = report.requests[0].fields;
    CHECK(sent.size() >= 4 && sent[0].name == ":method" && sent[0].value == "GET");
    CHECK(sent.size() >= 4 && sent[1].name == ":scheme" && sent[1].value == "http");
    CHECK(sent.size() >= 4 && sent[2].name == ":authority" && sent[2].value == "127.0.0.1:" + std::to_string(server.port()));
    CHECK(sent.size() >= 4 && sent[3].name == ":path" && sent[3].value == "/hello?x=1");
    CHECK(field_of(sent, "user-agent") && field_of(sent, "user-agent")->starts_with("Mozilla/5.0"));
    CHECK(field_of(sent, "accept-encoding") && *field_of(sent, "accept-encoding") == "gzip, deflate, br");
    CHECK(field_of(sent, "referer") && *field_of(sent, "referer") == "http://127.0.0.1/start");
    CHECK(field_of(sent, "x-mixed-case") && *field_of(sent, "x-mixed-case") == "yes");
    CHECK(!field_of(sent, "connection") && !field_of(sent, "host"));
    bool lowercase = true;
    bool cookie_never_indexed = false;
    for (Field const& field : sent) {
        lowercase = lowercase && std::none_of(field.name.begin(), field.name.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
        if (field.name == "cookie")
            cookie_never_indexed = field.value == "session=abc123" && field.never_indexed;
    }
    CHECK(lowercase);
    CHECK(cookie_never_indexed);
}

void test_six_at_once()
{
    H2Server::Options server_options;
    server_options.gather = 6;
    server_options.interleave = true;
    server_options.chunk = 1000;
    server_options.slow_path = "/big/3";
    server_options.slow_ms = 150;
    H2Server server(server_options);
    std::vector<net::Url> urls;
    for (int i = 0; i < 6; ++i)
        urls.push_back(server.url("/big/" + std::to_string(i)));
    net::ConnectionPool::Stats stats;
    std::vector<net::FetchResult> results;
    {
        net::ConnectionPool pool;
        results = fetch_together(urls, h2_options(pool));
        stats = pool.stats();
    }
    server.finish();
    int correct = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].response && results[i].response->body == body_for("/big/" + std::to_string(i)))
            ++correct;
        else
            std::printf("  six at once: /big/%zu: %s\n", i, results[i].error.c_str());
    }
    CHECK_EQ(correct, 6);
    H2Server::Report const report = server.report();
    CHECK_EQ(report.h2_connections, 1);
    CHECK_EQ(report.h1_connections, 0);
    CHECK_EQ(report.max_open, 6u);
    CHECK_EQ(stats.opened, 1u);
    CHECK_EQ(stats.sessions, 1u);
    CHECK_EQ(stats.reused, 5u);
    // Interleaved: the stream changes from one DATA frame to the next far
    // more often than six streams sent one after another would (five).
    int switches = 0;
    for (std::size_t i = 1; i < report.data_order.size(); ++i)
        switches += report.data_order[i] != report.data_order[i - 1] ? 1 : 0;
    CHECK(switches > 100);
    std::printf("  six at once: one connection, %zu DATA frames, %d switches between streams\n", report.data_order.size(), switches);
}

void test_flow_control()
{
    H2Server server({});
    net::FetchResult result;
    {
        net::ConnectionPool pool;
        net::FetchOptions options = h2_options(pool);
        options.http2_stream_window = 65535;
        options.http2_connection_window = 65535;
        result = net::fetch(server.url("/size/2097152"), options);
    }
    server.finish();
    CHECK(result.response && result.response->body == body_for("/size/2097152"));
    H2Server::Report const report = server.report();
    // 2 MB through windows of 64 KB: at least 31 increments of each kind
    // (the most one can open is 65535 octets) before the body can end.
    CHECK(report.stream_window_updates >= 31);
    CHECK(report.connection_window_updates >= 31);
    std::printf("  2 MB under 64 KB windows: %d stream and %d connection WINDOW_UPDATEs\n",
        report.stream_window_updates, report.connection_window_updates);
}

void test_concurrency_limit()
{
    H2Server::Options server_options;
    server_options.max_concurrent = 1;
    server_options.delay_ms = 100;
    H2Server server(server_options);
    std::vector<net::FetchResult> results;
    double elapsed_ms = 0;
    {
        net::ConnectionPool pool;
        net::FetchOptions const options = h2_options(pool);
        // One first, so the server's SETTINGS are in hand.
        net::FetchResult const first = net::fetch(server.url("/first"), options);
        CHECK(first.response.has_value());
        auto const started = std::chrono::steady_clock::now();
        results = fetch_together({ server.url("/a"), server.url("/b"), server.url("/c") }, options);
        elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    server.finish();
    int correct = 0;
    for (net::FetchResult const& result : results)
        correct += result.response && result.response->status == 200 ? 1 : 0;
    CHECK_EQ(correct, 3);
    H2Server::Report const report = server.report();
    CHECK_EQ(report.max_open, 1u);
    CHECK_EQ(report.refused, 0);
    CHECK_EQ(report.h2_connections, 1);
    // One at a time: three delays in a row, and well short of a fourth
    // (in parallel they would take one).
    CHECK(elapsed_ms >= 300);
    CHECK(elapsed_ms < 700);
    std::printf("  one stream at a time: three requests in %.0f ms\n", elapsed_ms);
}

void test_goaway()
{
    H2Server::Options server_options;
    server_options.gather = 3;
    server_options.goaway_after = 3;
    H2Server server(server_options);
    std::vector<net::FetchResult> results;
    net::ConnectionPool::Stats stats;
    {
        net::ConnectionPool pool;
        results = fetch_together({ server.url("/g1"), server.url("/g2"), server.url("/g3") }, h2_options(pool));
        stats = pool.stats();
    }
    server.finish();
    int correct = 0;
    std::vector<std::string> connections;
    for (std::size_t i = 0; i < results.size(); ++i) {
        std::string const path = "/g" + std::to_string(i + 1);
        if (results[i].response && text_of(results[i].response->body) == "body of " + path) {
            ++correct;
            connections.push_back(*net::find_header(results[i].response->headers, "x-connection"));
        } else {
            std::printf("  goaway: %s: %s\n", path.c_str(), results[i].error.c_str());
        }
    }
    CHECK_EQ(correct, 3);
    // One answered on the first connection, the two it went away before on
    // a second one.
    CHECK_EQ(std::count(connections.begin(), connections.end(), "0"), 1);
    CHECK_EQ(std::count(connections.begin(), connections.end(), "1"), 2);
    H2Server::Report const report = server.report();
    CHECK_EQ(report.h2_connections, 2);
    CHECK_EQ(stats.retried, 2u);
    CHECK_EQ(stats.sessions, 2u);
}

void test_ping_push_and_continuation()
{
    {
        H2Server::Options server_options;
        server_options.ping = true;
        H2Server server(server_options);
        {
            net::ConnectionPool pool;
            net::FetchResult const result = net::fetch(server.url("/pinged"), h2_options(pool));
            CHECK(result.response && text_of(result.response->body) == "body of /pinged");
        }
        server.finish();
        CHECK(server.report().ping_acknowledged);
    }
    {
        // A promise after this side said no: GOAWAY with PROTOCOL_ERROR,
        // and the request goes again, over HTTP/1.1.
        H2Server::Options server_options;
        server_options.push = true;
        server_options.delay_ms = 50;
        H2Server server(server_options);
        net::FetchResult result;
        net::FetchResult after;
        {
            net::ConnectionPool pool;
            result = net::fetch(server.url("/promised"), h2_options(pool));
            after = net::fetch(server.url("/after"), h2_options(pool));
        }
        server.finish();
        CHECK(result.response && text_of(result.response->body) == "body of /promised");
        CHECK(result.response && net::find_header(result.response->headers, "x-protocol")
            && *net::find_header(result.response->headers, "x-protocol") == "http/1.1");
        CHECK(after.response && after.timing.http2 == 0);
        H2Server::Report const report = server.report();
        CHECK(report.goaways_received.size() == 1 && report.goaways_received[0] == static_cast<std::uint32_t>(h2::ErrorCode::ProtocolError));
        CHECK_EQ(report.h2_connections, 1);
        CHECK_EQ(report.h1_connections, 1);
    }
    {
        H2Server::Options server_options;
        server_options.continuation = true;
        H2Server server(server_options);
        net::FetchResult result;
        {
            net::ConnectionPool pool;
            result = net::fetch(server.url("/continued"), h2_options(pool));
        }
        server.finish();
        CHECK(result.response && text_of(result.response->body) == "body of /continued");
        CHECK(result.response && net::find_header(result.response->headers, "x-long")
            && *net::find_header(result.response->headers, "x-long") == std::string(3000, 'q'));
    }
}

void test_garbage_after_preface()
{
    H2Server::Options server_options;
    server_options.garbage = true;
    H2Server server(server_options);
    net::FetchResult result;
    net::FetchResult after;
    net::ConnectionPool::Stats stats;
    {
        net::ConnectionPool pool;
        result = net::fetch(server.url("/fallback"), h2_options(pool));
        after = net::fetch(server.url("/after"), h2_options(pool));
        stats = pool.stats();
    }
    server.finish();
    CHECK(result.response && text_of(result.response->body) == "body of /fallback");
    CHECK(result.response && *net::find_header(result.response->headers, "x-protocol") == "http/1.1");
    CHECK(after.response && text_of(after.response->body) == "body of /after");
    H2Server::Report const report = server.report();
    // HTTP/2 was tried once (the connection became a session, which failed
    // on the server's first bytes); everything after went over HTTP/1.1, on
    // one kept connection.
    CHECK_EQ(report.h2_connections, 1);
    CHECK_EQ(report.h1_connections, 1);
    CHECK_EQ(stats.retried, 1u);
    CHECK_EQ(stats.sessions, 1u);
    CHECK_EQ(stats.opened, 2u);
}

// Runs `work` on a thread of its own and waits for it up to `limit`; past
// that, `unstick` is called to end it, and the thread is joined either way.
// Whether the work ended in time.
bool within(std::chrono::milliseconds limit, std::function<void()> const& work, std::function<void()> const& unstick)
{
    std::mutex mutex;
    std::condition_variable ended;
    bool done = false;
    std::thread worker([&] {
        work();
        {
            std::lock_guard<std::mutex> const lock(mutex);
            done = true;
        }
        ended.notify_all();
    });
    bool in_time = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        in_time = ended.wait_for(lock, limit, [&] { return done; });
    }
    if (!in_time)
        unstick();
    worker.join();
    return in_time;
}

double ms_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void test_unreachable_origin()
{
    // An https origin that cannot be reached, slowly: each connection is
    // taken, its ClientHello read, and then nothing for a while before the
    // server hangs up, so every handshake fails after that long.
    static constexpr int hang_ms = 400;
    platform::TcpListener listener = *platform::TcpListener::listen_loopback();
    std::uint16_t const port = listener.port();
    std::atomic<int> accepted = 0;
    std::atomic<bool> stop = false;
    std::vector<std::thread> held;
    std::thread accepter([&] {
        while (true) {
            std::optional<platform::TcpSocket> client = listener.accept();
            if (stop || !client)
                return;
            ++accepted;
            held.emplace_back([socket = std::move(*client)]() mutable {
                socket.set_receive_timeout(hang_ms);
                std::uint8_t buffer[4096];
                static_cast<void>(socket.receive(buffer, sizeof buffer));
                std::this_thread::sleep_for(std::chrono::milliseconds(hang_ms));
                socket.close();
            });
        }
    });

    std::vector<net::Url> urls;
    for (int i = 0; i < 6; ++i)
        urls.push_back(*net::parse_url("https://127.0.0.1:" + std::to_string(port) + "/" + std::to_string(i)));
    std::vector<net::FetchResult> results;
    auto const started = std::chrono::steady_clock::now();
    {
        net::ConnectionPool pool;
        net::FetchOptions options;
        options.pool = &pool;
        results = fetch_together(urls, options);
    }
    double const elapsed_ms = ms_since(started);
    stop = true;
    if (auto poke = platform::TcpSocket::connect("127.0.0.1", port))
        poke->close();
    accepter.join();
    for (std::thread& thread : held)
        thread.join();

    // Six fetches wait on the first one's connection and fail with its
    // error when it fails: one hang in all, where one each in turn would
    // be six.
    int failed_alike = 0;
    for (net::FetchResult const& result : results) {
        if (!result.response && !result.error.empty() && result.error == results[0].error)
            ++failed_alike;
    }
    CHECK_EQ(failed_alike, 6);
    CHECK_EQ(accepted.load(), 1);
    CHECK(elapsed_ms >= hang_ms);
    CHECK(elapsed_ms < 2 * hang_ms);
    std::printf("  unreachable origin: six fetches failed in %.0f ms over %d connection(s)\n", elapsed_ms, accepted.load());
}

void test_stalled_stream()
{
    // One stream at a time, and a path the server never answers: the fetch
    // behind it waits for the slot, which the stall limit frees.
    constexpr int stall_ms = 300;
    H2Server::Options server_options;
    server_options.max_concurrent = 1;
    server_options.stalled_path = "/stalled";
    H2Server server(server_options);
    net::FetchResult first;
    net::FetchResult stalled;
    net::FetchResult after;
    double stalled_ms = 0;
    double after_ms = 0;
    bool in_time = false;
    {
        net::ConnectionPool pool;
        net::FetchOptions options = h2_options(pool);
        // No receive timeout, as a page load sets none.
        options.http2_stall_ms = stall_ms;
        first = net::fetch(server.url("/first"), options);
        in_time = within(
            std::chrono::seconds(10),
            [&] {
                auto const started = std::chrono::steady_clock::now();
                std::thread stalling([&] {
                    stalled = net::fetch(server.url("/stalled"), options);
                    stalled_ms = ms_since(started);
                });
                // The second goes once the first holds the only stream.
                auto const seen = [&server] {
                    H2Server::Report const report = server.report();
                    return std::any_of(report.requests.begin(), report.requests.end(),
                        [](H2Server::Seen const& request) { return request.path == "/stalled"; });
                };
                while (!seen() && ms_since(started) < 2000)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                after = net::fetch(server.url("/after"), options);
                after_ms = ms_since(started);
                stalling.join();
            },
            [&server] { server.sever(); });
    }
    server.finish();
    CHECK(first.response.has_value());
    CHECK(in_time);
    CHECK(!stalled.response && !stalled.error.empty());
    CHECK(after.response && text_of(after.response->body) == "body of /after");
    CHECK(stalled_ms >= stall_ms);
    CHECK(stalled_ms < 2 * stall_ms);
    CHECK(after_ms >= stall_ms);
    CHECK(after_ms < 2 * stall_ms);
    H2Server::Report const report = server.report();
    CHECK_EQ(report.h2_connections, 1);
    CHECK_EQ(report.max_open, 1u);
    CHECK_EQ(report.refused, 0);
    std::printf("  stalled stream: reset after %.0f ms, the one behind it done at %.0f ms\n", stalled_ms, after_ms);
}

void test_shut_send_window()
{
    // A server that gives every stream a send window of 0 and never opens
    // it: a POST's body cannot go, and the stall limit ends the wait for
    // room with a RST_STREAM instead of holding the fetch thread for as
    // long as the connection lives.
    constexpr int stall_ms = 300;
    H2Server::Options server_options;
    server_options.shut_windows = true;
    server_options.stalled_path = "/upload";
    H2Server server(server_options);
    net::FetchResult first;
    net::FetchResult posted;
    double posted_ms = 0;
    bool in_time = false;
    {
        net::ConnectionPool pool;
        net::FetchOptions options = h2_options(pool);
        options.http2_stall_ms = stall_ms;
        // One first, so the server's SETTINGS are in hand before the POST.
        first = net::fetch(server.url("/first"), options);
        options.method = "POST";
        options.body.assign(1000, 'p');
        auto const started = std::chrono::steady_clock::now();
        in_time = within(
            std::chrono::seconds(5), [&] { posted = net::fetch(server.url("/upload"), options); },
            [&server] { server.sever(); });
        posted_ms = ms_since(started);
    }
    server.finish();
    CHECK(first.response.has_value());
    CHECK(in_time);
    CHECK(!posted.response && !posted.error.empty());
    CHECK(posted_ms >= stall_ms);
    CHECK(posted_ms < 2 * stall_ms);
    H2Server::Report const report = server.report();
    CHECK_EQ(report.h2_connections, 1);
    CHECK(std::count(report.resets_received.begin(), report.resets_received.end(),
              static_cast<std::uint32_t>(h2::ErrorCode::Cancel))
        == 1);
    std::printf("  shut send window: the POST gave up after %.0f ms (%s)\n", posted_ms, posted.error.c_str());
}

void test_post_to_a_busy_server()
{
    // A large POST to a server that answers at once with a large body and
    // reads nothing while it is sending: the client's writer blocks on a
    // full socket, and its reader has to go on reading for the server's
    // sends, and then the client's, to finish.
    constexpr std::size_t size = 32u * 1024u * 1024u;
    std::string const path = "/size/" + std::to_string(size);
    H2Server::Options server_options;
    server_options.open_windows = true;
    H2Server server(server_options);
    net::FetchResult result;
    bool in_time = false;
    double elapsed_ms = 0;
    {
        net::ConnectionPool pool;
        net::FetchOptions options = h2_options(pool);
        options.method = "POST";
        options.body.assign(size, 'p');
        options.http2_stream_window = h2::largest_window;
        options.http2_connection_window = h2::largest_window;
        auto const started = std::chrono::steady_clock::now();
        in_time = within(
            std::chrono::seconds(10), [&] { result = net::fetch(server.url(path), options); },
            [&server] { server.sever(); });
        elapsed_ms = ms_since(started);
    }
    server.finish();
    CHECK(in_time);
    CHECK(result.response && result.response->status == 200);
    CHECK(result.response && result.response->body == body_for(path));
    std::printf("  POST to a busy server: a 32 MB POST against a 32 MB answer in %.0f ms%s\n", elapsed_ms,
        in_time ? "" : " (deadlocked)");
}

void test_lost_before_an_answer()
{
    // A connection lost after a request went out and before any answer:
    // the server may or may not have acted on it. A GET goes again on a
    // new connection; a POST does not, since running it twice is not
    // harmless. Each case fetches once first, so the server's SETTINGS
    // are in hand and the loss is not taken for a server without HTTP/2.
    H2Server::Options server_options;
    server_options.hangup_path = "/hangup";
    for (std::string const method : { "GET", "POST" }) {
        H2Server server(server_options);
        net::FetchResult first;
        net::FetchResult result;
        net::ConnectionPool::Stats stats;
        {
            net::ConnectionPool pool;
            net::FetchOptions options = h2_options(pool);
            first = net::fetch(server.url("/first"), options);
            options.method = method;
            if (method == "POST")
                options.body.assign(100, 'p');
            result = net::fetch(server.url("/hangup"), options);
            stats = pool.stats();
        }
        server.finish();
        H2Server::Report const report = server.report();
        CHECK(first.response.has_value());
        if (method == "GET") {
            CHECK(result.response && text_of(result.response->body) == "body of /hangup");
            std::string const* const connection = result.response ? net::find_header(result.response->headers, "x-connection") : nullptr;
            CHECK(connection && *connection == "1");
            CHECK_EQ(report.h2_connections, 2);
            CHECK_EQ(report.requests.size(), 3u);
            CHECK_EQ(stats.retried, 1u);
        } else {
            CHECK(!result.response && !result.error.empty());
            CHECK_EQ(report.h2_connections, 1);
            CHECK_EQ(report.h1_connections, 0);
            CHECK_EQ(report.requests.size(), 2u);
            CHECK_EQ(stats.retried, 0u);
        }
        std::printf("  lost before an answer: %s %s over %d connection(s)%s%s\n", method.c_str(),
            result.response ? "answered" : "failed", report.h2_connections, result.response ? "" : ": ",
            result.error.c_str());
    }
}

void test_protocol_error_after_a_request()
{
    // A server that breaks the protocol after a request went out: HTTP/1.1
    // is the way on, but only for a request the server cannot have acted
    // on in a way that matters. One whose answer had begun, or one whose
    // method is not harmless run twice, fails instead of going again.
    struct Case {
        char const* method;
        bool after_headers;
        bool answered;
    };
    for (Case const c : { Case { "GET", true, false }, Case { "POST", false, false }, Case { "GET", false, true } }) {
        H2Server::Options server_options;
        server_options.broken_path = "/broken";
        server_options.broken_after_headers = c.after_headers;
        H2Server server(server_options);
        net::FetchResult result;
        {
            net::ConnectionPool pool;
            net::FetchOptions options = h2_options(pool);
            options.method = c.method;
            if (options.method == "POST")
                options.body.assign(100, 'p');
            result = net::fetch(server.url("/broken"), options);
        }
        server.finish();
        H2Server::Report const report = server.report();
        CHECK_EQ(report.h2_connections, 1);
        // The GOAWAY this side sends is not asserted: it races the shutdown
        // behind it, and a writer still holding the socket skips it.
        if (c.answered) {
            CHECK(result.response && text_of(result.response->body) == "body of /broken");
            CHECK(result.response && net::find_header(result.response->headers, "x-protocol")
                && *net::find_header(result.response->headers, "x-protocol") == "http/1.1");
            CHECK_EQ(report.h1_connections, 1);
            CHECK_EQ(report.requests.size(), 2u);
        } else {
            CHECK(!result.response && result.error.find("FRAME_SIZE_ERROR") != std::string::npos);
            CHECK_EQ(report.h1_connections, 0);
            CHECK_EQ(report.requests.size(), 1u);
        }
        std::printf("  protocol error after a request: %s%s %s, %d HTTP/1.1 connection(s)%s%s\n", c.method,
            c.after_headers ? " (answer begun)" : "", result.response ? "answered" : "failed", report.h1_connections,
            result.response ? "" : ": ", result.error.c_str());
    }
}

void test_reset_before_an_answer()
{
    // A stream reset with a broken-protocol code before any of its answer
    // arrived, on a connection that goes on: the server may have acted on
    // the request, so only a request that is harmless run twice goes again
    // over HTTP/1.1. A POST fails where a GET is answered.
    struct Case {
        char const* method;
        bool answered;
    };
    for (Case const c : { Case { "POST", false }, Case { "GET", true } }) {
        H2Server::Options server_options;
        server_options.reset_path = "/reset";
        server_options.reset_code = h2::ErrorCode::ProtocolError;
        H2Server server(server_options);
        net::FetchResult result;
        {
            net::ConnectionPool pool;
            net::FetchOptions options = h2_options(pool);
            options.method = c.method;
            if (options.method == "POST")
                options.body.assign(100, 'p');
            result = net::fetch(server.url("/reset"), options);
        }
        server.finish();
        H2Server::Report const report = server.report();
        CHECK_EQ(report.h2_connections, 1);
        if (c.answered) {
            CHECK(result.response && text_of(result.response->body) == "body of /reset");
            CHECK(result.response && net::find_header(result.response->headers, "x-protocol")
                && *net::find_header(result.response->headers, "x-protocol") == "http/1.1");
            CHECK_EQ(report.h1_connections, 1);
            CHECK_EQ(report.requests.size(), 2u);
        } else {
            CHECK(!result.response && result.error.find("PROTOCOL_ERROR") != std::string::npos);
            CHECK_EQ(report.h1_connections, 0);
            CHECK_EQ(report.requests.size(), 1u);
        }
        std::printf("  reset before an answer: %s %s, %d HTTP/1.1 connection(s)%s%s\n", c.method,
            result.response ? "answered" : "failed", report.h1_connections, result.response ? "" : ": ", result.error.c_str());
    }
}

// ---- through TLS, against node's HTTP/2 server

#ifndef _WIN32

std::string read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

pid_t spawn(std::vector<std::string> const& args, std::string const& log)
{
    pid_t const child = ::fork();
    if (child != 0)
        return child;
    int const handle = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (handle >= 0) {
        ::dup2(handle, 1);
        ::dup2(handle, 2);
        ::close(handle);
    }
    std::vector<char*> argv;
    for (std::string const& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
}

bool reap(pid_t child, int seconds, bool ask_first)
{
    if (child <= 0)
        return false;
    if (ask_first)
        ::kill(child, SIGTERM);
    for (int waited = 0; waited < seconds * 20; ++waited) {
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        ::usleep(50 * 1000);
    }
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    return false;
}

char const node_server[] = R"(const http2 = require('http2');
const fs = require('fs');
const [cert, key, portFile] = process.argv.slice(2);
let connections = 0, h2 = 0, h1 = 0;
const server = http2.createSecureServer({ cert: fs.readFileSync(cert), key: fs.readFileSync(key), allowHTTP1: true });
server.on('secureConnection', () => { connections++; });
server.on('request', (req, res) => {
  const protocol = req.httpVersion === '2.0' ? 'h2' : 'http/1.1';
  if (protocol === 'h2') h2++; else h1++;
  res.setHeader('content-type', 'text/plain');
  res.setHeader('x-protocol', protocol);
  if (req.url === '/stats') { res.end('connections=' + connections + ' h2=' + h2 + ' h1=' + h1); return; }
  res.end('node body of ' + req.url);
});
// Beside it, a server that speaks HTTP/1.1 only and answers no ALPN.
const https = require('https');
const plain = https.createServer({ cert: fs.readFileSync(cert), key: fs.readFileSync(key) }, (req, res) => {
  res.setHeader('content-type', 'text/plain');
  res.end('https body of ' + req.url + ' over ' + req.httpVersion);
});
server.listen(0, '127.0.0.1', () => {
  plain.listen(0, '127.0.0.1', () => {
    fs.writeFileSync(portFile + '.part', server.address().port + ' ' + plain.address().port);
    fs.renameSync(portFile + '.part', portFile);
  });
});
)";

void test_through_tls(std::string const& openssl, std::string const& node)
{
    char const* const base = std::getenv("TMPDIR");
    std::string pattern = (base != nullptr && base[0] != '\0' ? std::string(base) : std::string("/tmp")) + "/sashfold-h2-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
        std::printf("SKIP test_http2 through TLS: no temporary directory could be made\n");
        return;
    }
    std::string const directory = buffer.data();
    std::string const certificate = directory + "/server.pem";
    std::string const key = directory + "/server.key";
    std::string const script = directory + "/server.js";
    std::string const port_file = directory + "/port";
    std::string const log = directory + "/log";
    auto const clean_up = [&] {
        // By name, one at a time.
        for (std::string const& path : { certificate, key, script, port_file, port_file + ".part", log })
            std::remove(path.c_str());
        ::rmdir(directory.c_str());
    };
    if (!reap(spawn({ openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "2",
                          "-subj", "/CN=localhost", "-keyout", key, "-out", certificate },
                  log),
            60, false)) {
        std::printf("SKIP test_http2 through TLS: openssl could not make a certificate\n");
        clean_up();
        return;
    }
    {
        std::ofstream out(script, std::ios::binary);
        out << node_server;
    }
    pid_t const server = spawn({ node, script, certificate, key, port_file }, log);
    std::string port_text;
    for (int waited = 0; waited < 200 && port_text.empty(); ++waited) {
        port_text = read_file(port_file);
        if (port_text.empty())
            ::usleep(50 * 1000);
    }
    if (port_text.empty()) {
        std::printf("SKIP test_http2 through TLS: node's server did not start:\n%s\n", read_file(log).c_str());
        reap(server, 5, true);
        clean_up();
        return;
    }
    ::setenv("SASHFOLD_TLS_INSECURE", "1", 1);
    std::size_t const space = port_text.find(' ');
    std::string const origin = "https://127.0.0.1:" + port_text.substr(0, space);
    std::string const plain_origin = "https://127.0.0.1:" + port_text.substr(space + 1);
    std::vector<net::Url> urls;
    for (int i = 0; i < 6; ++i)
        urls.push_back(*net::parse_url(origin + "/tls/" + std::to_string(i)));
    std::vector<net::Url> plain_urls;
    for (int i = 0; i < 3; ++i)
        plain_urls.push_back(*net::parse_url(plain_origin + "/plain/" + std::to_string(i)));
    std::vector<net::FetchResult> results;
    std::vector<net::FetchResult> plain_results;
    net::FetchResult stats;
    net::ConnectionPool::Stats pool_stats;
    {
        net::ConnectionPool pool;
        net::FetchOptions options;
        options.pool = &pool;
        results = fetch_together(urls, options);
        stats = net::fetch(*net::parse_url(origin + "/stats"), options);
        plain_results = fetch_together(plain_urls, options);
        pool_stats = pool.stats();
    }
    ::unsetenv("SASHFOLD_TLS_INSECURE");
    // The server that chose nothing: everything as HTTP/1.1 has it.
    int over_http1 = 0;
    for (std::size_t i = 0; i < plain_results.size(); ++i) {
        net::FetchResult const& result = plain_results[i];
        if (result.response && text_of(result.response->body) == "https body of /plain/" + std::to_string(i) + " over 1.1"
            && result.timing.http2 == 0)
            ++over_http1;
        else
            std::printf("  through TLS, HTTP/1.1 only: /plain/%zu: %s\n", i, result.error.c_str());
    }
    CHECK_EQ(over_http1, 3);
    CHECK_EQ(pool_stats.sessions, 1u);
    int over_h2 = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        net::FetchResult const& result = results[i];
        std::string const* const protocol = result.response ? net::find_header(result.response->headers, "x-protocol") : nullptr;
        if (result.response && text_of(result.response->body) == "node body of /tls/" + std::to_string(i) && protocol
            && *protocol == "h2" && result.timing.http2 == 1)
            ++over_h2;
        else
            std::printf("  through TLS: /tls/%zu: %s %s\n", i, result.error.c_str(), protocol ? protocol->c_str() : "");
    }
    CHECK_EQ(over_h2, 6);
    std::string const counted = stats.response ? text_of(stats.response->body) : stats.error;
    CHECK_EQ(counted, std::string("connections=1 h2=7 h1=0"));
    std::printf("  through TLS: node's server counted %s\n", counted.c_str());
    reap(server, 5, true);
    clean_up();
}

#endif

}

int main(int argc, char** argv)
{
    // A test that hangs is a failure, and says so.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::fprintf(stderr, "FAIL test_http2: a wait passed its deadline\n");
        std::_Exit(2);
    }).detach();
#ifndef _WIN32
    ::signal(SIGPIPE, SIG_IGN);
#endif
    test_integers();
    test_huffman();
    test_representations();
    test_appendix_c();
    test_table_bounds();
    test_random_lists();
    test_framing();
    test_one_request();
    test_six_at_once();
    test_flow_control();
    test_concurrency_limit();
    test_goaway();
    test_ping_push_and_continuation();
    test_garbage_after_preface();
    test_unreachable_origin();
    test_stalled_stream();
    test_shut_send_window();
    test_post_to_a_busy_server();
    test_lost_before_an_answer();
    test_protocol_error_after_a_request();
    test_reset_before_an_answer();
#ifndef _WIN32
    std::string const openssl = argc > 1 ? argv[1] : "";
    std::string const node = argc > 2 ? argv[2] : "";
    if (openssl.empty() || node.empty() || ::access(openssl.c_str(), X_OK) != 0 || ::access(node.c_str(), X_OK) != 0)
        std::printf("SKIP test_http2 through TLS: openssl or node is not on PATH\n");
    else
        test_through_tls(openssl, node);
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
    std::printf("SKIP test_http2 through TLS: no posix process control\n");
#endif
    return sashfold::test::report("http2");
}
