#include "media/StreamBuffer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace sashfold::media {

namespace {

// What the shipping engines hold before they refuse an append: a buffer
// with pictures in it, and one with sound alone.
constexpr std::size_t video_quota = 150u * 1024u * 1024u;
constexpr std::size_t audio_quota = 12u * 1024u * 1024u;

constexpr std::int64_t nanoseconds = 1'000'000'000;

std::int64_t to_ns(double seconds)
{
    if (!(seconds > -9.0e9))
        return std::numeric_limits<std::int64_t>::min();
    if (!(seconds < 9.0e9))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(nanoseconds)));
}

double to_seconds(std::int64_t ns)
{
    return static_cast<double>(ns) / static_cast<double>(nanoseconds);
}

using Frames = std::map<std::int64_t, CodedFrame>;

// Takes out [from, to) and, when pictures went, the pictures after them that
// leaned on them: everything up to the next key frame.
std::size_t erase_with_dependents(Frames& frames, Frames::iterator from, Frames::iterator to)
{
    std::size_t freed = 0;
    bool const any = from != to;
    for (auto it = from; it != to; ++it)
        freed += it->second.data.size();
    auto next = frames.erase(from, to);
    if (any) {
        while (next != frames.end() && !next->second.key) {
            freed += next->second.data.size();
            next = frames.erase(next);
        }
    }
    return freed;
}

}

TimeRanges intersect(TimeRanges const& a, TimeRanges const& b)
{
    TimeRanges out;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        double const start = std::max(a[i].start, b[j].start);
        double const end = std::min(a[i].end, b[j].end);
        if (start < end)
            out.push_back({ start, end });
        if (a[i].end < b[j].end)
            ++i;
        else
            ++j;
    }
    return out;
}

TimeRange const* range_at(TimeRanges const& ranges, double time, bool at_end_too)
{
    for (TimeRange const& range : ranges) {
        if (time >= range.start && (time < range.end || (at_end_too && time == range.end)))
            return &range;
    }
    return nullptr;
}

StreamBuffer::Appended StreamBuffer::append(std::span<std::uint8_t const> bytes)
{
    Appended result;
    result.ok = m_parser.append(bytes);
    if (m_parser.init_segments() > m_inits_seen) {
        m_inits_seen = m_parser.init_segments();
        std::vector<WebmTrack const*> playable;
        for (WebmTrack const& track : m_parser.init().tracks) {
            if (track.kind != WebmTrack::Kind::Other)
                playable.push_back(&track);
        }
        if (playable.empty())
            result.ok = false;
        if (m_tracks.empty()) {
            for (WebmTrack const* track : playable) {
                Track made;
                made.description = *track;
                m_tracks.push_back(std::move(made));
            }
        } else {
            // A later initialization segment describes the same tracks
            // again (MSE §3.5.7): as many, of the same kinds and codecs.
            bool same = playable.size() == m_tracks.size();
            for (std::size_t i = 0; same && i < playable.size(); ++i)
                same = playable[i]->kind == m_tracks[i].description.kind && playable[i]->codec_id == m_tracks[i].description.codec_id;
            if (!same)
                result.ok = false;
            for (std::size_t i = 0; same && i < playable.size(); ++i)
                m_tracks[i].description = *playable[i];
        }
        for (Track& track : m_tracks)
            track.need_key_frame = true;
        result.init_segment = result.ok;
    }
    // The frames read before a fault are still the stream's.
    for (WebmFrame& frame : m_parser.take_frames()) {
        for (Track& track : m_tracks) {
            if (track.description.number == frame.track) {
                file(track, std::move(frame));
                result.frames = true;
                break;
            }
        }
    }
    return result;
}

void StreamBuffer::file(Track& track, WebmFrame&& frame)
{
    double const raw_seconds = to_seconds(frame.time_ns);
    if (mode == Mode::Sequence && group_start) {
        timestamp_offset = *group_start - raw_seconds;
        m_group_end = *group_start;
        for (Track& each : m_tracks) {
            each.need_key_frame = true;
            each.last_time_ns.reset();
        }
        group_start.reset();
    }
    std::int64_t const time_ns = frame.time_ns + to_ns(timestamp_offset);

    // A step backwards, or a leap forwards, begins a new run of frames
    // (MSE §3.5.8 step 6): every track waits for a key frame again.
    if (track.last_time_ns) {
        std::int64_t const distance = time_ns - *track.last_time_ns;
        if (distance < 0 || (track.last_distance_ns > 0 && distance > 2 * track.last_distance_ns)) {
            if (mode == Mode::Sequence)
                group_start = m_group_end;
            for (Track& each : m_tracks) {
                each.need_key_frame = true;
                each.last_time_ns.reset();
                each.last_distance_ns = 0;
            }
            if (mode == Mode::Sequence) {
                file(track, std::move(frame));
                return;
            }
        }
    }

    CodedFrame coded;
    coded.time_ns = time_ns;
    coded.key = frame.key;
    if (frame.duration_ns) {
        coded.duration_ns = static_cast<std::int64_t>(std::min<std::uint64_t>(*frame.duration_ns, static_cast<std::uint64_t>(nanoseconds) * 3600));
    } else {
        // Until the next frame says how long this one lasted: as long as the
        // one before, or for the first the track's own hint.
        coded.duration_ns = track.last_distance_ns > 0 ? track.last_distance_ns : static_cast<std::int64_t>(std::min<std::uint64_t>(track.description.default_duration_ns, static_cast<std::uint64_t>(nanoseconds)));
        coded.duration_estimated = true;
    }
    // The frame before, when its length was a guess, lasted until this one.
    if (track.last_time_ns && time_ns > *track.last_time_ns) {
        auto const before = track.frames.find(*track.last_time_ns);
        if (before != track.frames.end() && before->second.duration_estimated)
            before->second.duration_ns = time_ns - *track.last_time_ns;
        track.last_distance_ns = time_ns - *track.last_time_ns;
        if (coded.duration_estimated)
            coded.duration_ns = track.last_distance_ns;
    }
    track.last_time_ns = time_ns;

    std::int64_t const end_ns = time_ns + coded.duration_ns;
    if (time_ns < to_ns(append_window_start) || end_ns > to_ns(append_window_end)) {
        track.need_key_frame = true;
        return;
    }
    if (track.need_key_frame) {
        if (!coded.key)
            return;
        track.need_key_frame = false;
    }

    // What the new frame lies over goes (MSE §3.5.8 steps 13 to 15).
    Frames& frames = track.frames;
    if (track.description.kind == WebmTrack::Kind::Video) {
        auto before = frames.lower_bound(time_ns);
        if (before != frames.begin()) {
            --before;
            if (before->second.time_ns + before->second.duration_ns > time_ns + 1000)
                track.bytes -= erase_with_dependents(frames, before, std::next(before));
        }
    }
    auto const from = frames.lower_bound(time_ns);
    auto to = from;
    while (to != frames.end() && (to->first < end_ns || to->first == time_ns))
        ++to;
    track.bytes -= erase_with_dependents(frames, from, to);

    coded.data = std::move(frame.data);
    track.bytes += coded.data.size();
    track.longest_frame_ns = std::max(track.longest_frame_ns, coded.duration_ns);
    m_group_end = std::max(m_group_end, to_seconds(end_ns));
    frames.insert_or_assign(time_ns, std::move(coded));
}

void StreamBuffer::reset_parser()
{
    m_parser.reset();
    for (Track& track : m_tracks) {
        track.need_key_frame = true;
        track.last_time_ns.reset();
        track.last_distance_ns = 0;
    }
}

void StreamBuffer::remove(double start, double end)
{
    std::int64_t const start_ns = to_ns(start);
    std::int64_t const end_ns = to_ns(end);
    if (end_ns <= start_ns)
        return;
    for (Track& track : m_tracks) {
        auto const from = track.frames.lower_bound(start_ns);
        // The removal runs on to the first key frame at or after its end.
        auto to = track.frames.lower_bound(end_ns);
        while (to != track.frames.end() && !to->second.key)
            ++to;
        if (track.last_time_ns && *track.last_time_ns >= start_ns && (to == track.frames.end() || *track.last_time_ns < to->first))
            track.last_time_ns.reset();
        track.bytes -= erase_with_dependents(track.frames, from, to);
    }
}

bool StreamBuffer::evict(double current_time, std::size_t incoming)
{
    if (bytes() + incoming <= quota())
        return true;
    std::int64_t const now_ns = to_ns(current_time);
    for (Track& track : m_tracks) {
        // The key frame the picture at `current_time` hangs from.
        auto keep = track.frames.upper_bound(now_ns);
        while (keep != track.frames.begin()) {
            --keep;
            if (keep->second.key)
                break;
        }
        for (auto it = track.frames.begin(); it != keep;) {
            track.bytes -= it->second.data.size();
            it = track.frames.erase(it);
        }
    }
    return bytes() + incoming <= quota();
}

StreamBuffer::Track const* StreamBuffer::track_of(WebmTrack::Kind kind) const
{
    for (Track const& track : m_tracks) {
        if (track.description.kind == kind)
            return &track;
    }
    return nullptr;
}

TimeRanges StreamBuffer::ranges_of(Track const& track)
{
    // Frames a hair apart are one stretch: twice the longest frame, the
    // room the shipping engines give a stream's own jitter.
    std::int64_t const slack = std::max<std::int64_t>(2 * track.longest_frame_ns, 1'000'000);
    TimeRanges out;
    std::int64_t open_start = 0;
    std::int64_t open_end = 0;
    bool open = false;
    for (auto const& [time, frame] : track.frames) {
        std::int64_t const end = time + frame.duration_ns;
        if (open && time <= open_end + slack) {
            open_end = std::max(open_end, end);
            continue;
        }
        if (open && open_end > open_start)
            out.push_back({ to_seconds(open_start), to_seconds(open_end) });
        open = true;
        open_start = time;
        open_end = end;
    }
    if (open && open_end > open_start)
        out.push_back({ to_seconds(open_start), to_seconds(open_end) });
    return out;
}

TimeRanges StreamBuffer::buffered(bool ended) const
{
    if (m_tracks.empty())
        return {};
    double const highest = highest_end();
    std::optional<TimeRanges> all;
    for (Track const& track : m_tracks) {
        TimeRanges ranges = ranges_of(track);
        if (ended && !ranges.empty())
            ranges.back().end = std::max(ranges.back().end, highest);
        all = all ? intersect(*all, ranges) : std::move(ranges);
    }
    return all.value_or(TimeRanges {});
}

double StreamBuffer::highest_end() const
{
    std::int64_t highest = 0;
    for (Track const& track : m_tracks) {
        if (!track.frames.empty()) {
            CodedFrame const& last = track.frames.rbegin()->second;
            highest = std::max(highest, last.time_ns + last.duration_ns);
        }
    }
    return to_seconds(highest);
}

std::size_t StreamBuffer::bytes() const
{
    std::size_t total = 0;
    for (Track const& track : m_tracks)
        total += track.bytes;
    return total;
}

std::size_t StreamBuffer::quota() const
{
    return track_of(WebmTrack::Kind::Video) != nullptr ? video_quota : audio_quota;
}

}
