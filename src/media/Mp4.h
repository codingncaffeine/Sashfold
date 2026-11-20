#pragma once

// MP4, the ISO base media file format (ISO/IEC 14496-12) as the web's video
// travels in it: a plain file whose sample tables (14496-12 §8.6, §8.7)
// say where every frame is and when it plays, with the tables before or
// after the data, and the fragmented form Media Source Extensions and
// DASH send (the ISO BMFF byte stream format: an initialization segment of
// ftyp and moov, then media segments of moof and mdat). Bytes arrive in
// pieces cut anywhere; the parser keeps what it cannot finish and gives out
// every frame the moment its bytes are there. The codecs' configuration
// records come out as the track's codec_private: H.264's avcC (14496-15),
// AAC's AudioSpecificConfig from the esds (14496-14), VP9's vpcC.

#include "media/Cenc.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media {

// A protected track's scheme (ISO/IEC 23001-7 §8, 14496-12 §8.12): the
// sample entry is an 'encv' or 'enca' whose 'sinf' names the original
// format ('frma'), the scheme ('schm') and, in the 'tenc', what every
// sample is encrypted with unless a 'seig' sample group says otherwise.
struct Mp4Protection {
    std::uint32_t scheme = 0; // 'cenc', 'cens', 'cbc1', 'cbcs'
    bool is_protected = true;
    std::uint8_t per_sample_iv_size = 0; // 0, 8 or 16; 0: the constant IV
    KeyId key_id {};
    std::vector<std::uint8_t> constant_iv;
    std::uint8_t crypt_byte_block = 0;
    std::uint8_t skip_byte_block = 0;
};

// A 'seig' sample group's description (23001-7 §6): the same fields, for
// the samples the group holds (key rotation).
struct Mp4SeigEntry {
    bool is_protected = false;
    std::uint8_t per_sample_iv_size = 0;
    KeyId key_id {};
    std::vector<std::uint8_t> constant_iv;
    std::uint8_t crypt_byte_block = 0;
    std::uint8_t skip_byte_block = 0;
};

struct Mp4Track {
    enum class Kind { Video,
        Audio,
        Other };
    std::uint32_t id = 0;
    Kind kind = Kind::Other;
    // As WebM names them, so one player reads both: "V_H264", "V_H265",
    // "V_VP9", "V_AV1", "A_AAC", "A_MP3", "A_OPUS"; empty when not known.
    std::string codec_id;
    std::string sample_entry; // the box's own name: "avc1", "mp4a", ...
    std::vector<std::uint8_t> codec_private;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t timescale = 1;
    std::optional<double> duration_seconds;
    std::string language; // ISO 639-2, "und" when not said
    // Defaults a fragment's runs fall back on (trex).
    std::uint32_t default_sample_duration = 0;
    std::uint32_t default_sample_size = 0;
    std::uint32_t default_sample_flags = 0;
    // The edit list's shift of presentation: media time at which the
    // presentation starts, in the track's timescale (a negative value is an
    // empty edit, a delay before the first sample).
    std::int64_t edit_media_time = 0;
    // For an encrypted track: its scheme and defaults, and the 'seig'
    // groups its sample table describes (a fragment may add its own).
    std::optional<Mp4Protection> protection;
    std::vector<Mp4SeigEntry> seig;
};

struct Mp4Init {
    std::uint32_t timescale = 1000;
    std::optional<double> duration_seconds;
    std::vector<Mp4Track> tracks;
    bool fragmented = false; // an mvex says fragments follow
    // The moov's 'pssh' boxes, each whole: the "cenc" initialization data
    // Encrypted Media Extensions hands the page (the concatenation).
    std::vector<std::vector<std::uint8_t>> pssh;

    Mp4Track const* track(std::uint32_t id) const;
};

struct Mp4Frame {
    std::uint32_t track = 0;
    std::int64_t time_ns = 0; // presentation (the edit list applied)
    std::int64_t decode_time_ns = 0;
    std::uint64_t duration_ns = 0;
    bool key = false;
    std::vector<std::uint8_t> data; // H.264: length-prefixed NAL units as the file has them
    // How the sample is encrypted, for a protected one; `data` is then the
    // ciphertext as the file has it.
    std::optional<SampleEncryption> encryption;
};

class Mp4Parser {
public:
    // Takes the next bytes of the stream. False once the stream is
    // malformed; the parser then stays failed until reset().
    bool append(std::span<std::uint8_t const> bytes);

    // How many initialization segments (moov boxes) have been read, and the last.
    std::uint64_t init_segments() const { return m_init_segments; }
    Mp4Init const& init() const { return m_init; }

    // The frames read since the last call, in decode order per track, as
    // the file holds them.
    std::vector<Mp4Frame> take_frames();

    // The 'pssh' boxes of each moof read since the last call (a fragment
    // carrying initialization data of its own), each list in box order.
    std::vector<std::vector<std::vector<std::uint8_t>>> take_fragment_pssh();

    // True between boxes at the top level: nothing held back.
    bool at_segment_boundary() const;

    // Forgets a segment cut short: the next bytes begin a new box. The
    // initialization segment already read stays; data offsets start over.
    void reset();

    bool failed() const { return m_failed; }

private:
    struct Sample {
        std::uint64_t offset = 0; // in the stream
        std::uint32_t size = 0;
        std::int64_t decode_time = 0; // track timescale
        std::int32_t composition_offset = 0;
        std::uint32_t duration = 0;
        bool key = true;
        std::uint32_t track = 0;
        // How the sample is encrypted; null for a clear one. Held apart: a
        // file may name sixteen million samples, and each carried the whole
        // description inline (2.3 GB for a table of clear samples).
        std::shared_ptr<SampleEncryption const> encryption;
    };
    struct Mdat {
        std::uint64_t offset = 0; // of the payload's first byte in the stream
        std::vector<std::uint8_t> bytes;
    };

    bool parse_available();
    bool parse_moov(std::span<std::uint8_t const> body, std::uint64_t body_offset);
    bool parse_trak(std::span<std::uint8_t const> body, std::uint64_t body_offset);
    bool parse_sample_entry(std::span<std::uint8_t const> box, Mp4Track& track);
    bool parse_moof(std::span<std::uint8_t const> body, std::uint64_t moof_offset, std::size_t header_size);
    void emit_ready();
    std::int64_t to_ns(std::int64_t ticks, std::uint32_t timescale) const;

    std::vector<std::uint8_t> m_pending;
    std::uint64_t m_stream_offset = 0; // of m_pending's first byte
    std::uint64_t m_skip = 0; // bytes of a box being passed over, still to come
    // An mdat being received: its payload's start and how much is still to come.
    std::optional<std::uint64_t> m_mdat_left;
    std::vector<Mdat> m_mdats;
    Mp4Init m_init;
    std::uint64_t m_init_segments = 0;
    std::vector<Sample> m_samples; // known, not yet given out, by offset
    // emit_ready's last look: how many samples waited after it, and the
    // lowest end among those whose bytes are still to come. Until the
    // stream reaches that, or more samples are named, a look finds nothing.
    std::size_t m_samples_looked_at = 0;
    std::uint64_t m_next_sample_end = 0;
    std::vector<Mp4Frame> m_frames;
    std::vector<std::int64_t> m_fragment_decode_time; // per track index: where the next run begins
    std::vector<std::vector<std::vector<std::uint8_t>>> m_fragment_pssh;
    bool m_failed = false;
};

}
