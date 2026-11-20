#include "Test.h"

#include "crypto/Aes.h"
#include "crypto/AesModes.h"
#include "crypto/Sha2.h"
#include "media/Cenc.h"
#include "media/ClearKey.h"
#include "media/Mp4.h"
#include "media/WebM.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

// Encrypted media below the script API: the AES modes Common Encryption
// runs on, against the known answers of NIST SP 800-38A; and the
// demuxers' protected samples taken back to the clear, against ffmpeg.
// The fixtures (tools/eme-make-tests.sh) are one encode copied into a
// clear file and into encrypted ones — 'cenc' by ffmpeg's own muxer,
// 'cbcs' by the openssl command — and every encrypted file was checked
// there to decrypt, by ffmpeg, to the clear file's packets. Here each
// sample our demuxer gives out, decrypted with the key, must hash to what
// ffmpeg read from the clear file, sample for sample.

using namespace sashfold;
using media::Mp4Frame;
using media::Mp4Parser;
using media::Mp4Track;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Bytes hex(std::string_view text)
{
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return out;
}

std::string hex_of(std::span<std::uint8_t const> bytes)
{
    static char const digits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

crypto::AesBlock block_of(std::string_view text)
{
    crypto::AesBlock block {};
    Bytes const bytes = hex(text);
    std::copy_n(bytes.begin(), 16, block.begin());
    return block;
}

// The fixtures' key and key ID (tools/eme-make-tests.sh).
constexpr std::string_view fixture_key = "3a2f5b4c6d7e8f901a2b3c4d5e6f7081";
constexpr std::string_view fixture_kid = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";

// --- The modes: SP 800-38A, Appendix F ----------------------------------------------------

constexpr std::string_view nist_key = "2b7e151628aed2a6abf7158809cf4f3c";
constexpr std::string_view nist_plain = "6bc1bee22e409f96e93d7e117393172a"
                                        "ae2d8a571e03ac9c9eb76fac45af8e51"
                                        "30c81c46a35ce411e5fbc1191a0a52ef"
                                        "f69f2445df4f9b17ad2b417be66c3710";

void test_cbc_known_answers()
{
    // F.2.1 CBC-AES128.Encrypt and F.2.2 CBC-AES128.Decrypt, the message
    // cut in two pieces that chain.
    std::optional<crypto::Aes> const aes = crypto::Aes::with_key(hex(nist_key));
    CHECK(aes.has_value());
    if (!aes)
        return;
    std::string const cipher = "7649abac8119b246cee98e9b12e9197d"
                               "5086cb9b507219ee95db113a917678b2"
                               "73bed6b8e3c1743b7116e69e22229516"
                               "3ff1caa1681fac09120eca307586e1a7";
    Bytes data = hex(nist_plain);
    crypto::AesBlock chain = block_of("000102030405060708090a0b0c0d0e0f");
    crypto::aes_cbc_encrypt_blocks(*aes, chain, std::span(data).first(16));
    crypto::aes_cbc_encrypt_blocks(*aes, chain, std::span(data).subspan(16));
    CHECK_EQ(hex_of(data), cipher);
    chain = block_of("000102030405060708090a0b0c0d0e0f");
    crypto::aes_cbc_decrypt_blocks(*aes, chain, std::span(data).first(48));
    crypto::aes_cbc_decrypt_blocks(*aes, chain, std::span(data).subspan(48));
    CHECK_EQ(hex_of(data), std::string(nist_plain));
}

void test_ctr_known_answers()
{
    // F.5.1 CTR-AES128.Encrypt and F.5.2 CTR-AES128.Decrypt, the message
    // cut in pieces that end inside blocks: the keystream runs on.
    std::optional<crypto::Aes> const aes = crypto::Aes::with_key(hex(nist_key));
    CHECK(aes.has_value());
    if (!aes)
        return;
    std::string const cipher = "874d6191b620e3261bef6864990db6ce"
                               "9806f66b7970fdff8617187bb9fffdff"
                               "5ae4df3edbd5d35e5b4f09020db03eab"
                               "1e031dda2fbe03d1792170a0f3009cee";
    crypto::AesBlock const counter = block_of("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    for (std::size_t const cut : { std::size_t(1), std::size_t(5), std::size_t(16), std::size_t(27) }) {
        Bytes data = hex(nist_plain);
        crypto::AesCtrStream stream(*aes, counter, 64);
        for (std::size_t at = 0; at < data.size(); at += cut)
            stream.apply(std::span(data).subspan(at, std::min(cut, data.size() - at)));
        CHECK_EQ(hex_of(data), cipher);
        crypto::AesCtrStream back(*aes, counter, 128);
        back.apply(data);
        CHECK_EQ(hex_of(data), std::string(nist_plain));
    }
    // The block counter is the low 64 bits (ISO/IEC 23001-7 §9.5.2.2): it
    // wraps there without carrying into the IV half.
    Bytes zeros(32, 0);
    crypto::AesCtrStream wrapping(*aes, block_of("0011223344556677ffffffffffffffff"), 64);
    wrapping.apply(zeros);
    crypto::AesBlock const first = aes->encrypt(block_of("0011223344556677ffffffffffffffff"));
    crypto::AesBlock const second = aes->encrypt(block_of("00112233445566770000000000000000"));
    CHECK(std::equal(first.begin(), first.end(), zeros.begin()));
    CHECK(std::equal(second.begin(), second.end(), zeros.begin() + 16));
}

void test_sample_layouts()
{
    // 'cenc' over subsamples is one keystream over the protected bytes
    // alone; 'cbcs' leaves a partial block and the skipped blocks clear.
    Bytes const key = hex(fixture_key);
    std::optional<crypto::Aes> const aes = crypto::Aes::with_key(key);
    Bytes plain(100);
    for (std::size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<std::uint8_t>(i * 7 + 3);

    media::SampleEncryption cenc;
    cenc.scheme = media::scheme_cenc;
    cenc.iv = hex("0102030405060708");
    cenc.subsamples = { { 5, 20 }, { 10, 65 } };
    Bytes sample = plain;
    CHECK(media::encrypt_sample(cenc, key, sample));
    Bytes protected_bytes;
    protected_bytes.insert(protected_bytes.end(), plain.begin() + 5, plain.begin() + 25);
    protected_bytes.insert(protected_bytes.end(), plain.begin() + 35, plain.end());
    crypto::AesBlock counter = block_of("01020304050607080000000000000000");
    std::optional<Bytes> const expected = crypto::aes_ctr(*aes, counter, 64, protected_bytes);
    CHECK(expected.has_value());
    CHECK(std::equal(sample.begin(), sample.begin() + 5, plain.begin()));
    CHECK(std::equal(sample.begin() + 5, sample.begin() + 25, expected->begin()));
    CHECK(std::equal(sample.begin() + 25, sample.begin() + 35, plain.begin() + 25));
    CHECK(std::equal(sample.begin() + 35, sample.end(), expected->begin() + 20));
    CHECK(media::decrypt_sample(cenc, key, sample));
    CHECK(sample == plain);

    media::SampleEncryption cbcs;
    cbcs.scheme = media::scheme_cbcs;
    cbcs.iv = hex("00112233445566778899aabbccddeeff");
    cbcs.crypt_byte_block = 1;
    cbcs.skip_byte_block = 2;
    cbcs.subsamples = { { 4, 96 } }; // six blocks: 0 and 3 encrypted
    sample = plain;
    CHECK(media::encrypt_sample(cbcs, key, sample));
    CHECK(std::equal(sample.begin(), sample.begin() + 4, plain.begin()));
    CHECK(!std::equal(sample.begin() + 4, sample.begin() + 20, plain.begin() + 4));
    CHECK(std::equal(sample.begin() + 20, sample.begin() + 52, plain.begin() + 20));
    CHECK(!std::equal(sample.begin() + 52, sample.begin() + 68, plain.begin() + 52));
    CHECK(std::equal(sample.begin() + 68, sample.end(), plain.begin() + 68));
    // Blocks 0 and 3 are one chain from the IV.
    Bytes chained;
    chained.insert(chained.end(), plain.begin() + 4, plain.begin() + 20);
    chained.insert(chained.end(), plain.begin() + 52, plain.begin() + 68);
    crypto::AesBlock iv = block_of("00112233445566778899aabbccddeeff");
    crypto::aes_cbc_encrypt_blocks(*aes, iv, chained);
    CHECK(std::equal(chained.begin(), chained.begin() + 16, sample.begin() + 4));
    CHECK(std::equal(chained.begin() + 16, chained.end(), sample.begin() + 52));
    CHECK(media::decrypt_sample(cbcs, key, sample));
    CHECK(sample == plain);

    // A map that does not cover the sample, or an IV of no size, is refused
    // and the sample left as it was.
    cenc.subsamples = { { 5, 20 } };
    sample = plain;
    CHECK(!media::decrypt_sample(cenc, key, sample));
    CHECK(sample == plain);
    cenc.subsamples.clear();
    cenc.iv.clear();
    CHECK(!media::decrypt_sample(cenc, key, sample));
}

// --- MP4: decrypted samples against ffmpeg's clear packets ---------------------------------

struct Packet {
    int stream = 0;
    std::size_t size = 0;
    std::string sha256;
};

std::vector<Packet> read_packets(std::string const& path)
{
    std::vector<Packet> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        Packet p;
        if (fields >> p.stream >> p.size >> p.sha256)
            out.push_back(p);
    }
    return out;
}

struct Demuxed {
    bool ok = true;
    media::Mp4Init init;
    std::vector<Mp4Frame> frames;
};

Demuxed demux(Bytes const& file, std::size_t piece)
{
    Demuxed out;
    Mp4Parser parser;
    for (std::size_t at = 0; at < file.size() && out.ok; at += piece) {
        out.ok = parser.append(std::span<std::uint8_t const>(file.data() + at, std::min(piece, file.size() - at)));
        for (Mp4Frame& frame : parser.take_frames())
            out.frames.push_back(std::move(frame));
    }
    out.init = parser.init();
    return out;
}

// Each sample of `name`, decrypted with `key`, against the clear file's
// packets: how many differ (a missing or extra sample counts).
std::size_t compare(std::string const& folder, std::string const& name, std::string const& clear, Bytes const& key,
    std::size_t piece, std::uint32_t scheme)
{
    Bytes const file = read_file(folder + "/" + name + ".mp4");
    std::vector<Packet> const packets = read_packets(folder + "/" + clear + ".packets");
    CHECK(!file.empty() && !packets.empty());
    Demuxed const demuxed = demux(file, piece);
    CHECK(demuxed.ok);
    std::vector<Mp4Track> const& tracks = demuxed.init.tracks;
    for (Mp4Track const& track : tracks) {
        // The protected entry read as its original format, with its scheme.
        CHECK(track.protection.has_value());
        if (!track.protection)
            continue;
        CHECK(track.protection->scheme == scheme);
        CHECK_EQ(hex_of(track.protection->key_id), std::string(fixture_kid));
        CHECK(track.codec_id == (track.kind == Mp4Track::Kind::Video ? "V_H264" : "A_AAC"));
    }
    std::vector<std::vector<Mp4Frame const*>> by_track(tracks.size());
    for (Mp4Frame const& frame : demuxed.frames) {
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].id == frame.track)
                by_track[i].push_back(&frame);
        }
    }
    std::vector<std::size_t> next(tracks.size(), 0);
    std::size_t differ = 0;
    std::size_t encrypted = 0;
    for (Packet const& p : packets) {
        auto const stream = static_cast<std::size_t>(p.stream);
        if (stream >= tracks.size() || next[stream] >= by_track[stream].size()) {
            ++differ;
            continue;
        }
        Mp4Frame const& frame = *by_track[stream][next[stream]++];
        Bytes data = frame.data;
        if (frame.encryption) {
            ++encrypted;
            CHECK_EQ(hex_of(frame.encryption->key_id), std::string(fixture_kid));
            if (!media::decrypt_sample(*frame.encryption, key, data))
                ++differ;
        }
        if (data.size() != p.size || hex_of(crypto::Sha256::hash(data)) != p.sha256)
            ++differ;
    }
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (next[i] != by_track[i].size())
            ++differ;
    }
    // Every sample of these files is protected.
    CHECK_EQ(encrypted, packets.size());
    return differ;
}

void test_mp4_against_ffmpeg(std::string const& folder)
{
    Bytes const key = hex(fixture_key);
    for (std::size_t const piece : { std::size_t(1) << 20, std::size_t(997) }) {
        CHECK_EQ(compare(folder, "av-cenc-frag", "av-clear-frag", key, piece, media::scheme_cenc), std::size_t(0));
        CHECK_EQ(compare(folder, "av-cenc-plain", "av-clear-plain", key, piece, media::scheme_cenc), std::size_t(0));
        CHECK_EQ(compare(folder, "a-cenc-frag", "a-clear-frag", key, piece, media::scheme_cenc), std::size_t(0));
        CHECK_EQ(compare(folder, "v-cenc-frag", "v-clear-frag", key, piece, media::scheme_cenc), std::size_t(0));
        CHECK_EQ(compare(folder, "v-cbcs-frag", "v-clear-frag", key, piece, media::scheme_cbcs), std::size_t(0));
    }
    // The control: a wrong key gives back none of the clear samples.
    Bytes wrong = key;
    wrong[0] ^= 1;
    CHECK_EQ(compare(folder, "av-cenc-frag", "av-clear-frag", wrong, 1 << 20, media::scheme_cenc),
        read_packets(folder + "/av-clear-frag.packets").size());
    CHECK_EQ(compare(folder, "v-cbcs-frag", "v-clear-frag", wrong, 1 << 20, media::scheme_cbcs),
        read_packets(folder + "/v-clear-frag.packets").size());
}

// The 'cenc' initialization data is the moov's 'pssh' boxes, each whole
// (version 1 carrying key IDs); a moof's own come out apart. ffmpeg writes
// none, so two are put into the fixture's moov here, and one into its moof.
void test_pssh(std::string const& folder)
{
    Bytes file = read_file(folder + "/av-cenc-frag.mp4");
    auto const be32_at = [&](std::size_t at) {
        return (std::uint32_t(file[at]) << 24) | (std::uint32_t(file[at + 1]) << 16) | (std::uint32_t(file[at + 2]) << 8) | file[at + 3];
    };
    auto const put32 = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            file[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    };
    // version 1, the common system ID, one key ID, no data.
    Bytes const v1 = hex("0000003470737368010000001077efecc0b24d02ace33c1e52e2fb4b0000000"
                         "1" + std::string(fixture_kid) + "00000000");
    // version 0 of another system, four bytes of data.
    Bytes const v0 = hex("00000024707373680000000000112233445566778899aabbccddeeff00000004deadbeef");
    std::size_t const moov = be32_at(0);
    CHECK(std::string(file.begin() + static_cast<std::ptrdiff_t>(moov + 4), file.begin() + static_cast<std::ptrdiff_t>(moov + 8)) == "moov");
    std::size_t const moov_end = moov + be32_at(moov);
    file.insert(file.begin() + static_cast<std::ptrdiff_t>(moov_end), v0.begin(), v0.end());
    file.insert(file.begin() + static_cast<std::ptrdiff_t>(moov_end), v1.begin(), v1.end());
    put32(moov, static_cast<std::uint32_t>(moov_end - moov + v1.size() + v0.size()));
    // The moof is next: a pssh after its children moves the mdat behind
    // it, so each run's data offset (from the moof's start) grows by the
    // box; the 'saio' offsets point before it and stay.
    std::size_t const moof = moov_end + v1.size() + v0.size();
    CHECK(std::string(file.begin() + static_cast<std::ptrdiff_t>(moof + 4), file.begin() + static_cast<std::ptrdiff_t>(moof + 8)) == "moof");
    std::size_t const moof_end = moof + be32_at(moof);
    for (std::size_t at = moof + 8; at + 16 < moof_end; ++at) {
        if (std::string(file.begin() + static_cast<std::ptrdiff_t>(at), file.begin() + static_cast<std::ptrdiff_t>(at + 4)) == "trun" && (file[at + 7] & 1))
            put32(at + 12, be32_at(at + 12) + static_cast<std::uint32_t>(v0.size()));
    }
    file.insert(file.begin() + static_cast<std::ptrdiff_t>(moof_end), v0.begin(), v0.end());
    put32(moof, static_cast<std::uint32_t>(moof_end - moof + v0.size()));

    Mp4Parser parser;
    CHECK(parser.append(file));
    CHECK_EQ(parser.init().pssh.size(), std::size_t(2));
    if (parser.init().pssh.size() == 2) {
        CHECK(parser.init().pssh[0] == v1);
        CHECK(parser.init().pssh[1] == v0);
    }
    auto const fragments = parser.take_fragment_pssh();
    CHECK_EQ(fragments.size(), std::size_t(1));
    if (fragments.size() == 1)
        CHECK(fragments[0] == std::vector<Bytes> { v0 });
    // The samples still read and decrypt.
    std::size_t decrypted = 0;
    for (Mp4Frame& frame : parser.take_frames()) {
        if (frame.encryption && media::decrypt_sample(*frame.encryption, hex(fixture_key), frame.data))
            ++decrypted;
    }
    CHECK_EQ(decrypted, read_packets(folder + "/av-clear-frag.packets").size());
}

// The web-platform-tests' own encrypted content (a packager's, not
// ffmpeg's: senc, saiz and saio in every moof, keys for sound and picture
// apart), when the checkout is there: each sample decrypted with the key
// its key ID names must be the clear file's — ffmpeg decrypts the video
// to exactly that file's packets. How many differ, or nothing without it.
std::optional<std::size_t> compare_wpt(std::filesystem::path const& folder, std::string const& encrypted, std::string const& clear)
{
    Bytes const enc = read_file((folder / encrypted).string());
    Bytes const plain = read_file((folder / clear).string());
    if (enc.empty() || plain.empty())
        return std::nullopt;
    std::vector<std::pair<std::string, Bytes>> const keys = {
        { "ad13f9ea2be698b875f504a8e3ccea64", hex("be7df8a3667a6a8fd564d0ed81339a95") },
        { "558ee541b90ab2f3950d00ade3760d45", hex("91039263016da635770d57db92f98bd0") },
    };
    Demuxed const a = demux(enc, 1 << 20);
    Demuxed const b = demux(plain, 1 << 20);
    std::size_t differ = a.frames.size() == b.frames.size() ? 0 : 1;
    for (std::size_t i = 0; i < a.frames.size() && i < b.frames.size(); ++i) {
        Bytes data = a.frames[i].data;
        if (a.frames[i].encryption) {
            Bytes key;
            for (auto const& [id, value] : keys) {
                if (hex_of(a.frames[i].encryption->key_id) == id)
                    key = value;
            }
            if (key.empty() || !media::decrypt_sample(*a.frames[i].encryption, key, data))
                ++differ;
        }
        if (data != b.frames[i].data)
            ++differ;
    }
    return differ;
}

void test_wpt_content(std::filesystem::path const& folder)
{
    std::optional<std::size_t> const video = compare_wpt(folder, "video_512x288_h264-360k_enc_dashinit.mp4", "video_512x288_h264-360k_clear_dashinit.mp4");
    std::optional<std::size_t> const audio = compare_wpt(folder, "audio_aac-lc_128k_enc_dashinit.mp4", "audio_aac-lc_128k_dashinit.mp4");
    if (!video || !audio) {
        std::cerr << "test_eme: no web-platform-tests checkout at " << folder.string() << "; its encrypted content is not compared\n";
        return;
    }
    CHECK_EQ(*video, std::size_t(0));
    CHECK_EQ(*audio, std::size_t(0));
}

// --- Clear Key's formats (EME §9.1, the Initialization Data Format Registry) ------------------

void test_clear_key_formats()
{
    // base64url without padding (RFC 4648 §5 and §10's vectors).
    CHECK_EQ(media::base64url_encode(Bytes { 'f', 'o', 'o', 'b', 'a' }), std::string("Zm9vYmE"));
    CHECK_EQ(media::base64url_encode(Bytes { 0xfb, 0xff }), std::string("-_8"));
    CHECK(media::base64url_decode("Zm9vYmFy") == (Bytes { 'f', 'o', 'o', 'b', 'a', 'r' }));
    CHECK(!media::base64url_decode("Zm9vYg==")); // padding
    CHECK(!media::base64url_decode("+/8")); // the other alphabet
    CHECK(!media::base64url_decode("Zm9vYh")); // bits left over that are not zero

    // "keyids": a JSON object of base64url key IDs, each 1 to 512 bytes.
    auto const ids = [](std::string_view type, std::string_view text) {
        return media::key_ids_from_init_data(type, std::span(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
    };
    std::optional<std::vector<Bytes>> const keyids = ids("keyids", R"({"kids":["Dx4tPEtaaXiHlqW0w9Lh8A","AAEC"]})");
    CHECK(keyids && keyids->size() == 2 && hex_of((*keyids)[0]) == fixture_kid && hex_of((*keyids)[1]) == "000102");
    CHECK(!ids("keyids", R"({"kids":[]})"));
    CHECK(!ids("keyids", R"({"kids":[""]})"));
    CHECK(!ids("keyids", R"({"kid":["AAEC"]})"));
    CHECK(!ids("keyids", "not json"));
    // "webm": the bytes are one key ID.
    CHECK(ids("webm", "0123456789012345") && ids("webm", "0123456789012345")->size() == 1);
    CHECK(!ids("webm", std::string(513, 'x')));
    // "cenc": whole pssh boxes; the common system ID's version 1 boxes carry
    // the key IDs, another system's box is passed over, a box that does not
    // add up is refused.
    Bytes const common = hex("0000003470737368010000001077efecc0b24d02ace33c1e52e2fb4b00000001" + std::string(fixture_kid) + "00000000");
    Bytes const other = hex("00000024707373680000000000112233445566778899aabbccddeeff00000004deadbeef");
    Bytes both = other;
    both.insert(both.end(), common.begin(), common.end());
    std::optional<std::vector<Bytes>> const cenc = media::key_ids_from_init_data("cenc", both);
    CHECK(cenc && cenc->size() == 1 && hex_of(cenc->front()) == fixture_kid);
    std::optional<std::vector<Bytes>> const none = media::key_ids_from_init_data("cenc", other);
    CHECK(none && none->empty());
    Bytes cut = common;
    cut.pop_back();
    CHECK(!media::key_ids_from_init_data("cenc", cut));
    CHECK(!media::key_ids_from_init_data("cenc", Bytes(70000)));
    CHECK(!media::key_ids_from_init_data("mp4", common));

    // The license request, and a license: a JSON Web Key Set of "oct" keys.
    CHECK_EQ(media::license_request({ hex(fixture_kid) }, "temporary"), std::string(R"({"kids":["Dx4tPEtaaXiHlqW0w9Lh8A"],"type":"temporary"})"));
    auto const license = [](std::string_view text) {
        return media::parse_license(std::span(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
    };
    std::optional<media::License> const good = license(R"({"keys":[{"kty":"oct","k":"Oi9bTG1-j5AaKzxNXm9wgQ","kid":"Dx4tPEtaaXiHlqW0w9Lh8A"}],"type":"temporary"})");
    CHECK(good && good->keys.size() == 1);
    if (good && good->keys.size() == 1) {
        CHECK_EQ(hex_of(good->keys[0].first), std::string(fixture_kid));
        CHECK_EQ(hex_of(good->keys[0].second), std::string(fixture_key));
    }
    CHECK(!license(R"({"keys":[{"kty":"RSA","k":"Oi9bTG1-j5AaKzxNXm9wgQ","kid":"Dx4tPEtaaXiHlqW0w9Lh8A"}]})"));
    CHECK(!license(R"({"keys":[{"kty":"oct","k":"AAEC","kid":"Dx4tPEtaaXiHlqW0w9Lh8A"}]})")); // not 16 bytes
    CHECK(!license(R"({"keys":[]})"));
    CHECK(!license("{\"keys\":[{\"kty\":\"oct\",\"k\":\"Oi9bTG1-j5AaKzxNXm9wgQ\",\"kid\":\"\xc3\xa9\"}]}")); // not ASCII
    CHECK(!license(std::string("\x00\x11\x22\x33", 4)));

    // The key table: a session's keys go with it; the newest stands.
    media::KeyStore store;
    media::ContentKey first {};
    first[0] = 1;
    media::ContentKey second {};
    second[0] = 2;
    store.add(1, hex(fixture_kid), first);
    store.add(2, hex(fixture_kid), second);
    CHECK(store.find(hex(fixture_kid)) == second);
    store.remove_session(2);
    CHECK(store.find(hex(fixture_kid)) == first);
    store.remove_session(1);
    CHECK(!store.find(hex(fixture_kid)));
}

// --- WebM: the signal byte, IV and partitions ---------------------------------------------

// Each frame of the encrypted WebM file, decrypted with `key`, against
// ffmpeg's packets of the clear one: how many differ. `kinds` counts the
// frames encrypted whole, partitioned and left clear.
std::size_t compare_webm(std::string const& folder, Bytes const& key, std::size_t piece, std::array<std::size_t, 3>& kinds)
{
    Bytes const file = read_file(folder + "/vp9-opus-enc.webm");
    std::vector<Packet> const packets = read_packets(folder + "/vp9-opus-clear.packets");
    CHECK(!file.empty() && !packets.empty());
    media::WebmParser parser;
    std::vector<media::WebmFrame> frames;
    for (std::size_t at = 0; at < file.size(); at += piece) {
        CHECK(parser.append(std::span<std::uint8_t const>(file.data() + at, std::min(piece, file.size() - at))));
        for (media::WebmFrame& frame : parser.take_frames())
            frames.push_back(std::move(frame));
    }
    auto const& tracks = parser.init().tracks;
    CHECK_EQ(tracks.size(), std::size_t(2));
    for (media::WebmTrack const& track : tracks) {
        CHECK(track.encrypted);
        CHECK_EQ(hex_of(track.key_id), std::string(fixture_kid));
    }
    std::vector<std::vector<media::WebmFrame const*>> by_track(tracks.size());
    for (media::WebmFrame const& frame : frames) {
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].number == frame.track)
                by_track[i].push_back(&frame);
        }
    }
    std::vector<std::size_t> next(tracks.size(), 0);
    std::size_t differ = 0;
    kinds = {};
    for (Packet const& p : packets) {
        auto const stream = static_cast<std::size_t>(p.stream);
        if (stream >= tracks.size() || next[stream] >= by_track[stream].size()) {
            ++differ;
            continue;
        }
        media::WebmFrame const& frame = *by_track[stream][next[stream]++];
        Bytes data = frame.data;
        if (frame.encryption) {
            ++kinds[frame.encryption->subsamples.empty() ? 0 : 1];
            CHECK_EQ(hex_of(frame.encryption->key_id), std::string(fixture_kid));
            if (!media::decrypt_sample(*frame.encryption, key, data))
                ++differ;
        } else {
            ++kinds[2];
        }
        if (data.size() != p.size || hex_of(crypto::Sha256::hash(data)) != p.sha256)
            ++differ;
    }
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (next[i] != by_track[i].size())
            ++differ;
    }
    return differ;
}

void test_webm_against_ffmpeg(std::string const& folder)
{
    Bytes const key = hex(fixture_key);
    std::array<std::size_t, 3> kinds {};
    for (std::size_t const piece : { std::size_t(1) << 20, std::size_t(61) }) {
        CHECK_EQ(compare_webm(folder, key, piece, kinds), std::size_t(0));
        CHECK(kinds[0] > 0 && kinds[1] > 0 && kinds[2] > 0);
    }
    // The control: with a wrong key only the frames left clear come back.
    Bytes wrong = key;
    wrong[15] ^= 0x80;
    std::size_t const total = read_packets(folder + "/vp9-opus-clear.packets").size();
    CHECK_EQ(compare_webm(folder, wrong, 1 << 20, kinds), total - kinds[2]);
}

}

int main(int argc, char** argv)
{
    test_cbc_known_answers();
    test_ctr_known_answers();
    test_sample_layouts();
    test_clear_key_formats();
    if (argc < 2)
        CHECK(!"the fixtures folder is the first argument");
    else {
        test_mp4_against_ffmpeg(argv[1]);
        test_pssh(argv[1]);
        test_webm_against_ffmpeg(argv[1]);
    }
    if (argc >= 3)
        test_wpt_content(argv[2]);
    return test::report("test_eme");
}
