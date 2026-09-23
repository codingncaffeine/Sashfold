#include "Test.h"

#include "media/Opus.h"
#include "media/RangeDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

// Opus: the packet framing (§3.2) over packets written byte by byte here,
// the range decoder's bookkeeping, and — when a checkout of the RFC 8251
// test vectors is given — the decoder against them. Each vector is a list of
// packets with the final range the reference encoder ended in; a decoder
// that reads every bit as it was written ends each packet in the same
// state, which is checked packet by packet. The output is then held to the
// specification's own quality metric (RFC 6716 §6.1, opus_compare) against
// the reference decode.

using namespace sashfold;
using media::OpusDecoder;
using media::OpusPacket;

namespace {

using Bytes = std::vector<std::uint8_t>;

void test_packet_framing()
{
    // Code 0: one frame, the rest of the packet.
    Bytes const one = { 0xF8, 1, 2, 3 }; // config 31: CELT, fullband, 20 ms
    auto packet = media::parse_opus_packet(one);
    CHECK(packet.has_value());
    CHECK_EQ(packet->config, 31);
    CHECK(packet->mode == OpusPacket::Mode::Celt);
    CHECK(packet->bandwidth == OpusPacket::Bandwidth::Full);
    CHECK_EQ(packet->frame_samples, 960);
    CHECK(!packet->stereo);
    CHECK_EQ(packet->frames.size(), 1u);
    CHECK_EQ(packet->frames[0].size(), 3u);

    // Code 1: two frames of equal size, so the length must be even.
    Bytes const two_equal = { 0xFD, 1, 2, 3, 4 }; // stereo, code 1
    packet = media::parse_opus_packet(two_equal);
    CHECK(packet.has_value() && packet->stereo && packet->frames.size() == 2 && packet->frames[1][0] == 3);
    CHECK(!media::parse_opus_packet(Bytes { 0xF9, 1, 2, 3 }).has_value());

    // Code 2: two frames, the first's length given.
    packet = media::parse_opus_packet(Bytes { 0xFA, 1, 9, 8, 7 });
    CHECK(packet.has_value() && packet->frames[0].size() == 1 && packet->frames[1].size() == 2);
    CHECK(!media::parse_opus_packet(Bytes { 0xFA, 5, 9 }).has_value()); // longer than the packet

    // A two-byte length: 252 + 4 * 1 = 256.
    Bytes big(1 + 2 + 256 + 10, 0);
    big[0] = 0xFA;
    big[1] = 252;
    big[2] = 1;
    packet = media::parse_opus_packet(big);
    CHECK(packet.has_value() && packet->frames[0].size() == 256 && packet->frames[1].size() == 10);

    // Code 3: a frame count, then equal frames. (The frames point into the
    // packet's bytes, which must outlive them.)
    Bytes const three_equal = { 0xFB, 0x03, 1, 1, 2, 2, 3, 3 };
    packet = media::parse_opus_packet(three_equal);
    CHECK(packet.has_value() && packet->frames.size() == 3 && packet->frames[2][1] == 3);
    // Sized frames, with padding: 255 means 254 bytes of padding and another
    // padding byte follows.
    Bytes padded = { 0xFB, 0xC2, 255, 1, 2, 7, 8, 9 };
    padded.insert(padded.end(), 255, 0);
    // 2 frames, VBR, padding 254 + 1: the first frame is 2 bytes, the second
    // the rest before the padding.
    packet = media::parse_opus_packet(padded);
    CHECK(packet.has_value());
    if (packet) {
        CHECK_EQ(packet->frames.size(), 2u);
        CHECK_EQ(packet->frames[0].size(), 2u);
        CHECK_EQ(packet->frames[1].size(), 1u);
    }
    // No frames, or more than 120 ms of them, is invalid.
    CHECK(!media::parse_opus_packet(Bytes { 0xFB, 0x00 }).has_value());
    CHECK(!media::parse_opus_packet(Bytes { 0xFB, 0x07, 1, 2, 3, 4, 5, 6, 7 }).has_value()); // 7 x 20 ms
    // Equal frames that do not divide the length.
    CHECK(!media::parse_opus_packet(Bytes { 0xFB, 0x02, 1, 2, 3 }).has_value());
    // A frame over 1275 bytes.
    Bytes huge(1 + 1276, 0);
    huge[0] = 0xF8;
    CHECK(!media::parse_opus_packet(huge).has_value());

    // The table of contents, a configuration of each kind.
    packet = media::parse_opus_packet(Bytes { 0x08, 0 }); // SILK narrowband 20 ms
    CHECK(packet && packet->mode == OpusPacket::Mode::Silk && packet->bandwidth == OpusPacket::Bandwidth::Narrow && packet->frame_samples == 960);
    packet = media::parse_opus_packet(Bytes { 0x78, 0 }); // hybrid fullband 20 ms (config 15)
    CHECK(packet && packet->mode == OpusPacket::Mode::Hybrid && packet->bandwidth == OpusPacket::Bandwidth::Full && packet->frame_samples == 960);
    packet = media::parse_opus_packet(Bytes { 0x80, 0 }); // CELT narrowband 2.5 ms (config 16)
    CHECK(packet && packet->mode == OpusPacket::Mode::Celt && packet->bandwidth == OpusPacket::Bandwidth::Narrow && packet->frame_samples == 120);
    packet = media::parse_opus_packet(Bytes { 0xA0, 0 }); // CELT wideband (config 20)
    CHECK(packet && packet->bandwidth == OpusPacket::Bandwidth::Wide);
}

void test_range_decoder()
{
    // A new decoder has spent one bit, whatever it reads (§4.1.6.1).
    Bytes const bytes = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
    media::RangeDecoder range(bytes);
    CHECK_EQ(range.tell(), 1);
    CHECK_EQ(range.tell_frac(), 8u);
    // Raw bits come from the end, least significant first.
    CHECK_EQ(range.bits(4), 0xCu);
    CHECK_EQ(range.bits(4), 0xBu);
    CHECK_EQ(range.bits(8), 0x9Au);
    CHECK_EQ(range.tell(), 17);
    CHECK_EQ(media::ilog(0), 0);
    CHECK_EQ(media::ilog(1), 1);
    CHECK_EQ(media::ilog(3), 2);
    CHECK_EQ(media::ilog(0x80000000u), 32);
}

// --- The test vectors ------------------------------------------------------------------------------

struct VectorPacket {
    Bytes bytes;
    std::uint32_t final_range = 0;
};

std::vector<VectorPacket> read_bitstream(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    Bytes const all { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    std::vector<VectorPacket> packets;
    std::size_t at = 0;
    auto const be32 = [&all](std::size_t i) {
        return static_cast<std::uint32_t>(all[i]) << 24 | static_cast<std::uint32_t>(all[i + 1]) << 16 | static_cast<std::uint32_t>(all[i + 2]) << 8
            | all[i + 3];
    };
    while (at + 8 <= all.size()) {
        std::uint32_t const length = be32(at);
        VectorPacket packet;
        packet.final_range = be32(at + 4);
        at += 8;
        if (at + length > all.size())
            break;
        packet.bytes.assign(all.begin() + static_cast<std::ptrdiff_t>(at), all.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        packets.push_back(std::move(packet));
    }
    return packets;
}

std::vector<float> read_pcm16(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    Bytes const all { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    std::vector<float> samples(all.size() / 2);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(static_cast<std::int16_t>(all[2 * i] | all[2 * i + 1] << 8));
    return samples;
}

// The specification's comparison (RFC 6716 §6.1): per-band spectral energy
// of both signals, masked in frequency and time, and the weighted error of
// their ratio mapped to a quality from 100 down; below zero fails.
double opus_quality(std::vector<float> const& reference, std::vector<float> const& decoded, int channels)
{
    constexpr int band_count = 21;
    constexpr int freq_count = 240;
    constexpr int window_size = 480;
    constexpr int step = 120;
    static constexpr int bands[band_count + 1] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 68, 80, 96, 120, 156, 200 };
    std::size_t const length = reference.size() / static_cast<std::size_t>(channels);
    if (decoded.size() != reference.size() || length < window_size)
        return -1000;
    std::size_t const frames = (length - window_size + step) / step;
    double const pi = 3.141592653589793;
    std::vector<float> window(window_size);
    std::vector<float> cosine(window_size);
    std::vector<float> sine(window_size);
    for (int j = 0; j < window_size; ++j) {
        window[static_cast<std::size_t>(j)] = 0.5F - 0.5F * static_cast<float>(std::cos((2 * pi / (window_size - 1)) * j));
        cosine[static_cast<std::size_t>(j)] = static_cast<float>(std::cos((2 * pi / window_size) * j));
        sine[static_cast<std::size_t>(j)] = static_cast<float>(std::sin((2 * pi / window_size) * j));
    }
    auto const band_energy = [&](std::vector<float> const& in, std::vector<float>* out, std::vector<float>& power) {
        std::vector<float> x(static_cast<std::size_t>(channels * window_size));
        for (std::size_t f = 0; f < frames; ++f) {
            for (int c = 0; c < channels; ++c)
                for (int k = 0; k < window_size; ++k)
                    x[static_cast<std::size_t>(c * window_size + k)] = window[static_cast<std::size_t>(k)] * in[(f * step + static_cast<std::size_t>(k)) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
            int j = 0;
            for (int b = 0; b < band_count; ++b) {
                float p[2] = { 0, 0 };
                for (; j < bands[b + 1]; ++j) {
                    for (int c = 0; c < channels; ++c) {
                        float re = 0;
                        float im = 0;
                        int t = 0;
                        for (int k = 0; k < window_size; ++k) {
                            re += cosine[static_cast<std::size_t>(t)] * x[static_cast<std::size_t>(c * window_size + k)];
                            im -= sine[static_cast<std::size_t>(t)] * x[static_cast<std::size_t>(c * window_size + k)];
                            t += j;
                            if (t >= window_size)
                                t -= window_size;
                        }
                        float const e = re * re + im * im + 100000;
                        power[(f * (window_size / 2) + static_cast<std::size_t>(j)) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = e;
                        p[c] += e;
                    }
                }
                if (out) {
                    for (int c = 0; c < channels; ++c)
                        (*out)[(f * band_count + static_cast<std::size_t>(b)) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = p[c] / static_cast<float>(bands[b + 1] - bands[b]);
                }
            }
        }
    };
    std::vector<float> xb(frames * band_count * static_cast<std::size_t>(channels));
    std::vector<float> X(frames * freq_count * static_cast<std::size_t>(channels));
    std::vector<float> Y(frames * freq_count * static_cast<std::size_t>(channels));
    band_energy(reference, &xb, X);
    band_energy(decoded, nullptr, Y);
    auto const at = [channels](std::size_t frame, std::size_t per, int index, int c) {
        return (frame * per + static_cast<std::size_t>(index)) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c);
    };
    for (std::size_t f = 0; f < frames; ++f) {
        for (int b = 1; b < band_count; ++b)
            for (int c = 0; c < channels; ++c)
                xb[at(f, band_count, b, c)] += 0.1F * xb[at(f, band_count, b - 1, c)];
        for (int b = band_count - 1; b-- > 0;)
            for (int c = 0; c < channels; ++c)
                xb[at(f, band_count, b, c)] += 0.03F * xb[at(f, band_count, b + 1, c)];
        if (f > 0) {
            for (int b = 0; b < band_count; ++b)
                for (int c = 0; c < channels; ++c)
                    xb[at(f, band_count, b, c)] += 0.5F * xb[at(f - 1, band_count, b, c)];
        }
        if (channels == 2) {
            for (int b = 0; b < band_count; ++b) {
                float const l = xb[at(f, band_count, b, 0)];
                float const r = xb[at(f, band_count, b, 1)];
                xb[at(f, band_count, b, 0)] += 0.01F * r;
                xb[at(f, band_count, b, 1)] += 0.01F * l;
            }
        }
        for (int b = 0; b < band_count; ++b)
            for (int j = bands[b]; j < bands[b + 1]; ++j)
                for (int c = 0; c < channels; ++c) {
                    X[at(f, freq_count, j, c)] += 0.1F * xb[at(f, band_count, b, c)];
                    Y[at(f, freq_count, j, c)] += 0.1F * xb[at(f, band_count, b, c)];
                }
    }
    for (int b = 0; b < band_count; ++b)
        for (int j = bands[b]; j < bands[b + 1]; ++j)
            for (int c = 0; c < channels; ++c) {
                float xtmp = X[at(0, freq_count, j, c)];
                float ytmp = Y[at(0, freq_count, j, c)];
                for (std::size_t f = 1; f < frames; ++f) {
                    float const x2 = X[at(f, freq_count, j, c)];
                    float const y2 = Y[at(f, freq_count, j, c)];
                    X[at(f, freq_count, j, c)] += xtmp;
                    Y[at(f, freq_count, j, c)] += ytmp;
                    xtmp = x2;
                    ytmp = y2;
                }
            }
    double err = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        double ef = 0;
        for (int b = 0; b < band_count; ++b) {
            double eb = 0;
            for (int j = bands[b]; j < bands[b + 1] && j < bands[band_count]; ++j)
                for (int c = 0; c < channels; ++c) {
                    float const re = Y[at(f, freq_count, j, c)] / X[at(f, freq_count, j, c)];
                    float im = re - std::log(re) - 1;
                    if (j >= 79 && j <= 81)
                        im *= 0.1F;
                    if (j == 80)
                        im *= 0.1F;
                    eb += static_cast<double>(im);
                }
            eb /= (bands[b + 1] - bands[b]) * channels;
            ef += eb * eb;
        }
        ef /= band_count;
        ef *= ef;
        err += ef * ef;
    }
    err = std::pow(err / static_cast<double>(frames), 1.0 / 16);
    return 100 * (1 - 0.5 * std::log(1 + err) / std::log(1.13));
}

struct VectorOutcome {
    std::size_t packets = 0;
    std::size_t celt_packets = 0;
    std::size_t ranges_matched = 0;
    std::size_t unsupported = 0;
    std::size_t first_mismatch = static_cast<std::size_t>(-1);
    double quality = -1000;
};

VectorOutcome run_vector(std::filesystem::path const& directory, std::string const& name, bool compare)
{
    VectorOutcome outcome;
    std::vector<VectorPacket> const packets = read_bitstream(directory / (name + ".bit"));
    OpusDecoder decoder(2);
    std::vector<float> pcm;
    for (VectorPacket const& packet : packets) {
        outcome.packets++;
        std::size_t const before = pcm.size();
        OpusDecoder::Result const result = decoder.decode(packet.bytes, pcm);
        if (result.outcome == OpusDecoder::Outcome::Unsupported) {
            outcome.unsupported++;
            continue;
        }
        if (result.outcome == OpusDecoder::Outcome::Invalid) {
            pcm.resize(before);
            if (outcome.first_mismatch == static_cast<std::size_t>(-1))
                outcome.first_mismatch = outcome.packets - 1;
            continue;
        }
        outcome.celt_packets++;
        if (decoder.final_range() == packet.final_range)
            outcome.ranges_matched++;
        else if (outcome.first_mismatch == static_cast<std::size_t>(-1))
            outcome.first_mismatch = outcome.packets - 1;
    }
    if (compare) {
        std::vector<float> reference = read_pcm16(directory / (name + ".dec"));
        std::vector<float> decoded(pcm.size());
        for (std::size_t i = 0; i < pcm.size(); ++i)
            decoded[i] = std::round(std::clamp(pcm[i] * 32768.0f, -32768.0f, 32767.0f));
        outcome.quality = opus_quality(reference, decoded, 2);
    }
    return outcome;
}

void test_vectors(std::filesystem::path const& directory)
{
    // The two vectors made of CELT alone must decode exactly: every final
    // range, and the output within the specification's tolerance.
    for (std::string const name : { "testvector01", "testvector07" }) {
        VectorOutcome const outcome = run_vector(directory, name, true);
        std::cerr << name << ": " << outcome.ranges_matched << " of " << outcome.celt_packets << " final ranges match";
        if (outcome.first_mismatch != static_cast<std::size_t>(-1))
            std::cerr << " (first miss at packet " << outcome.first_mismatch << ")";
        std::cerr << ", quality " << outcome.quality << "\n";
        CHECK_EQ(outcome.unsupported, 0u);
        CHECK_EQ(outcome.ranges_matched, outcome.packets);
        CHECK(outcome.quality >= 0);
    }
    // The rest mix in SILK and hybrid frames, which are not decoded yet: the
    // CELT frames among them are counted, not held to a result.
    for (std::string const name : { "testvector02", "testvector03", "testvector04", "testvector05", "testvector06", "testvector08",
             "testvector09", "testvector10", "testvector11", "testvector12" }) {
        VectorOutcome const outcome = run_vector(directory, name, false);
        std::cerr << name << ": " << outcome.ranges_matched << " of " << outcome.celt_packets << " CELT final ranges match, "
                  << outcome.unsupported << " packets not decoded\n";
    }
}

}

int main(int argc, char** argv)
{
    test_packet_framing();
    test_range_decoder();
    if (argc > 1 && std::filesystem::exists(std::filesystem::path(argv[1]) / "testvector01.bit"))
        test_vectors(argv[1]);
    else
        std::cerr << "test_opus: no test vectors (tools/opus-vectors-fetch.sh), framing only\n";
    return test::report("test_opus");
}
