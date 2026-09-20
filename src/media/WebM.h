#pragma once

// WebM, the Matroska subset the web's royalty-free media travels in, read
// the way Media Source Extensions hands it over (the WebM byte stream
// format): an initialization segment — the EBML header, the Segment's own
// header, Info and Tracks — and then media segments, each a Cluster of
// blocks. Bytes arrive in pieces cut anywhere, so the parser keeps what it
// cannot finish yet and gives out every frame the moment its block is whole.
// A file read from its first byte to its last is the same stream.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media {

struct WebmTrack {
    enum class Kind { Video,
        Audio,
        Other };
    std::uint64_t number = 0;
    Kind kind = Kind::Other;
    std::string codec_id; // "V_VP9", "A_OPUS", ...
    std::vector<std::uint8_t> codec_private;
    std::uint32_t width = 0; // video, in pixels
    std::uint32_t height = 0;
    double sample_rate = 8000; // audio; Matroska's default
    std::uint32_t channels = 1;
    std::uint64_t default_duration_ns = 0; // 0: not given
    std::uint64_t codec_delay_ns = 0;
    std::uint64_t seek_preroll_ns = 0;
};

struct WebmInit {
    std::uint64_t timecode_scale_ns = 1'000'000;
    std::optional<double> duration_seconds;
    std::vector<WebmTrack> tracks;

    WebmTrack const* track(std::uint64_t number) const;
};

struct WebmFrame {
    std::uint64_t track = 0;
    std::int64_t time_ns = 0;
    // From the BlockDuration, or for laced frames the track's default. A lone
    // block without one has no length of its own: a track's DefaultDuration
    // is a hint that real streams get wrong (60 frames a second under a
    // default of 33 ms), so the distance to the next frame is what counts.
    std::optional<std::uint64_t> duration_ns;
    bool key = false;
    std::vector<std::uint8_t> data;
};

class WebmParser {
public:
    // Takes the next bytes of the stream. False once the stream is malformed
    // (or holds one element larger than a parser should keep whole); the
    // parser then stays failed until reset().
    bool append(std::span<std::uint8_t const> bytes);

    // How many initialization segments have been read whole, and the last.
    std::uint64_t init_segments() const { return m_init_segments; }
    WebmInit const& init() const { return m_init; }

    // The frames read since the last call, in stream order.
    std::vector<WebmFrame> take_frames();

    // True between segments: nothing held back, no Cluster open with bytes
    // still owed. What MSE's abort() and "parsing media segment" ask about.
    bool at_segment_boundary() const;

    // Forgets a segment cut short: the next bytes begin a new segment. The
    // initialization segment already read stays.
    void reset();

    bool failed() const { return m_failed; }

private:
    bool parse_available();
    bool parse_info(std::span<std::uint8_t const> body);
    bool parse_tracks(std::span<std::uint8_t const> body);
    bool parse_block(std::span<std::uint8_t const> body, bool simple, std::optional<std::uint64_t> duration, bool has_reference);
    bool parse_block_group(std::span<std::uint8_t const> body);
    void finish_init();

    std::vector<std::uint8_t> m_pending;
    std::uint64_t m_skip = 0; // bytes of an element being passed over, still to come
    bool m_in_cluster = false;
    bool m_cluster_sized = false;
    std::uint64_t m_cluster_left = 0; // when sized
    std::optional<std::uint64_t> m_cluster_time;
    bool m_have_info = false;
    bool m_have_tracks = false;
    bool m_init_open = false; // an initialization segment is being read
    WebmInit m_building;
    WebmInit m_init;
    std::uint64_t m_init_segments = 0;
    std::vector<WebmFrame> m_frames;
    bool m_failed = false;
};

}
