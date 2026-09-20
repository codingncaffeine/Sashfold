#pragma once

// The media side of one Media Source Extensions SourceBuffer: bytes go in as
// the page appends them, the WebM parser cuts them into coded frames, and
// the coded frame processing algorithm (MSE §3.5.8) files each frame in its
// track's buffer — the timestamp offset added, the append window applied,
// frames until the first key frame dropped, whatever a new frame overlaps
// taken out. What is buffered is answered from the frames held, and a
// decoder reads them from here in order.

#include "media/WebM.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::media {

// A stretch of the timeline in seconds, start included and end not. A list
// of them is kept in order, none touching another.
struct TimeRange {
    double start = 0;
    double end = 0;
    bool operator==(TimeRange const&) const = default;
};
using TimeRanges = std::vector<TimeRange>;

TimeRanges intersect(TimeRanges const&, TimeRanges const&);
// The range holding `time`, if any. A time at a range's very end is inside
// it for `at_end_too`: what an ended stream's last moment asks.
TimeRange const* range_at(TimeRanges const&, double time, bool at_end_too = false);

struct CodedFrame {
    std::int64_t time_ns = 0;
    std::int64_t duration_ns = 0;
    bool duration_estimated = false; // no duration on the wire: the distance between frames
    bool key = false;
    std::vector<std::uint8_t> data;
};

class StreamBuffer {
public:
    enum class Mode { Segments,
        Sequence };

    struct Track {
        WebmTrack description;
        std::map<std::int64_t, CodedFrame> frames; // by presentation time
        std::size_t bytes = 0;
        bool need_key_frame = true;
        std::optional<std::int64_t> last_time_ns; // of the frame before, in this run of appends
        std::int64_t last_distance_ns = 0;
        std::int64_t longest_frame_ns = 0;
    };

    struct Appended {
        bool ok = true;
        bool init_segment = false; // an initialization segment was read whole in this append
        bool frames = false; // and coded frames were filed
    };
    Appended append(std::span<std::uint8_t const> bytes);

    // abort() and changeType(): a segment cut short is forgotten, and every
    // track waits for a key frame again.
    void reset_parser();
    bool parsing_media_segment() const { return !m_parser.at_segment_boundary(); }

    // Takes out the frames that begin in [start, end), and after them the
    // frames that leaned on them, up to the next key frame (MSE §3.5.9).
    void remove(double start, double end);

    // Makes room for `incoming` more bytes by giving up what lies before the
    // group of pictures holding `current_time`. False when that is not enough.
    bool evict(double current_time, std::size_t incoming);

    bool has_init() const { return m_parser.init_segments() > 0; }
    WebmInit const& init() const { return m_parser.init(); }
    std::vector<Track> const& tracks() const { return m_tracks; }
    Track const* track_of(WebmTrack::Kind) const;

    // Where every track has frames. With `ended`, each track counts as
    // reaching the furthest end of any (MSE §3.1, the buffered attribute).
    TimeRanges buffered(bool ended = false) const;
    double highest_end() const; // seconds; 0 when empty
    double group_end() const { return m_group_end; } // where sequence mode would put the next run
    std::size_t bytes() const;
    std::size_t quota() const;

    Mode mode = Mode::Segments;
    double timestamp_offset = 0;
    double append_window_start = 0;
    double append_window_end = std::numeric_limits<double>::infinity();
    // Sequence mode's running position; set when the mode or the offset is.
    std::optional<double> group_start;

private:
    void file(Track&, WebmFrame&&);
    static TimeRanges ranges_of(Track const&);

    WebmParser m_parser;
    std::vector<Track> m_tracks;
    std::uint64_t m_inits_seen = 0;
    double m_group_end = 0;
};

}
