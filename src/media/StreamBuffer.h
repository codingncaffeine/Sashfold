#pragma once

// The media side of one Media Source Extensions SourceBuffer, and of a file
// an element plays by itself: bytes go in as the page appends them, the
// container's parser (WebM or MP4, told by the first bytes) cuts them into coded frames, and
// the coded frame processing algorithm (MSE §3.5.8) files each frame in its
// track's buffer — the timestamp offset added, the append window applied,
// frames until the first key frame dropped, whatever a new frame overlaps
// taken out. What is buffered is answered from the frames held, and a
// decoder reads them from here in order.

#include "media/Mp4.h"
#include "media/WebM.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
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
    std::int64_t decode_time_ns = 0; // the same as time_ns but for a stream whose pictures are reordered
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
        // The frames in the order a decoder takes them: decode time to
        // presentation time. The same order as `frames` but for a stream
        // whose pictures lean on later ones (H.264's B pictures).
        std::map<std::int64_t, std::int64_t> decode_order;
        std::size_t bytes = 0;
        bool need_key_frame = true;
        std::optional<std::int64_t> last_time_ns; // of the frame before, in this run of appends
        std::optional<std::int64_t> last_decode_ns; // its decode time: what a step backwards is judged by (MSE §3.5.8 step 6)
        std::int64_t last_distance_ns = 0;
        std::int64_t longest_frame_ns = 0;
        // The times of the earliest and the latest frame taken out or
        // replaced since whoever reads the frames in order last asked: a
        // reader that has taken frames in that stretch has taken what the
        // track no longer holds. Nothing changed while the first is past
        // the second. (One reader a track; asking does not change what the
        // track holds.)
        mutable std::int64_t changed_from_ns = std::numeric_limits<std::int64_t>::max();
        mutable std::int64_t changed_to_ns = std::numeric_limits<std::int64_t>::min();
        std::pair<std::int64_t, std::int64_t> take_changed() const
        {
            return { std::exchange(changed_from_ns, std::numeric_limits<std::int64_t>::max()),
                std::exchange(changed_to_ns, std::numeric_limits<std::int64_t>::min()) };
        }
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
    bool parsing_media_segment() const;

    // Takes out the frames that begin in [start, end), and after them the
    // frames that leaned on them, up to the next key frame (MSE §3.5.9).
    void remove(double start, double end);

    // Makes room for `incoming` more bytes by giving up what lies before the
    // group of pictures holding `current_time`. False when that is not enough.
    bool evict(double current_time, std::size_t incoming);

    bool has_init() const { return m_inits_seen > 0; }
    // The stream's duration as its header says, if it does.
    std::optional<double> duration_seconds() const { return m_duration_seconds; }
    enum class Container { Unknown,
        WebM,
        Mp4 };
    Container container() const { return m_container; }
    // What the bytes will be, from the type the page named; without it the
    // first bytes say.
    void expect(Container container) { m_container = container; }
    std::vector<Track> const& tracks() const { return m_tracks; }
    Track const* track_of(WebmTrack::Kind) const;

    // Where every track has frames. With `ended`, each track counts as
    // reaching the furthest end of any (MSE §3.1, the buffered attribute).
    TimeRanges buffered(bool ended = false) const;
    double highest_end() const; // seconds; 0 when empty
    double highest_start() const; // seconds; where the latest frame begins, 0 when empty
    // How far apart two of a track's frames may be and still count as one
    // stretch: twice the longest frame, the room the shipping engines give
    // a stream's own jitter. The buffered ranges and whoever plays the
    // frames use the same allowance, so a hole one crosses the other does.
    static std::int64_t gap_slack_ns(Track const& track) { return std::max<std::int64_t>(2 * track.longest_frame_ns, 1'000'000); }
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
    void file(Track&, WebmFrame&&, std::int64_t decode_time_ns);
    bool take_init(std::vector<WebmTrack> const& tracks, std::optional<double> duration);
    static TimeRanges ranges_of(Track const&);

    Container m_container = Container::Unknown;
    std::vector<std::uint8_t> m_sniff; // the first bytes, until the container is known
    WebmParser m_webm;
    Mp4Parser m_mp4;
    std::uint64_t m_inits_seen = 0;
    std::optional<double> m_duration_seconds;
    std::vector<Track> m_tracks;
    double m_group_end = 0;
};

}
