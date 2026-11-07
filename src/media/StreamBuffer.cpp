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
// leaned on them: everything up to the next key frame. The track notes from
// where it changed, for whoever had read that far.
std::size_t erase_with_dependents(StreamBuffer::Track& track, Frames::iterator from, Frames::iterator to)
{
    Frames& frames = track.frames;
    std::size_t freed = 0;
    bool const any = from != to;
    if (any)
        track.changed_from_ns = std::min(track.changed_from_ns, from->first);
    for (auto it = from; it != to; ++it) {
        freed += it->second.data.size();
        track.changed_to_ns = std::max(track.changed_to_ns, it->first);
        track.decode_order.erase(it->second.decode_time_ns);
    }
    auto next = frames.erase(from, to);
    if (any) {
        while (next != frames.end() && !next->second.key) {
            freed += next->second.data.size();
            track.changed_to_ns = std::max(track.changed_to_ns, next->first);
            track.decode_order.erase(next->second.decode_time_ns);
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

namespace {

// An MP4 track's description in the form the buffer keeps for every
// container (WebM's, the first written): kind, codec, configuration, size.
WebmTrack describe(Mp4Track const& track)
{
    WebmTrack out;
    out.number = track.id;
    out.kind = track.kind == Mp4Track::Kind::Video ? WebmTrack::Kind::Video : track.kind == Mp4Track::Kind::Audio ? WebmTrack::Kind::Audio : WebmTrack::Kind::Other;
    out.codec_id = track.codec_id;
    out.codec_private = track.codec_private;
    out.width = track.width;
    out.height = track.height;
    out.sample_rate = track.sample_rate;
    out.channels = track.channels;
    if (track.default_sample_duration > 0 && track.timescale > 0)
        out.default_duration_ns = static_cast<std::uint64_t>(static_cast<double>(track.default_sample_duration) * 1e9 / track.timescale);
    return out;
}

} // namespace

bool StreamBuffer::take_init(std::vector<WebmTrack> const& tracks, std::optional<double> duration)
{
    bool ok = true;
    std::vector<WebmTrack const*> playable;
    for (WebmTrack const& track : tracks) {
        if (track.kind != WebmTrack::Kind::Other)
            playable.push_back(&track);
    }
    if (playable.empty())
        ok = false;
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
            ok = false;
        for (std::size_t i = 0; same && i < playable.size(); ++i)
            m_tracks[i].description = *playable[i];
    }
    for (Track& track : m_tracks)
        track.need_key_frame = true;
    if (duration)
        m_duration_seconds = duration;
    return ok;
}

StreamBuffer::Appended StreamBuffer::append(std::span<std::uint8_t const> bytes)
{
    Appended result;
    // The container, from the first bytes: an EBML header (WebM) or a box
    // whose name an MP4 begins with.
    if (m_container == Container::Unknown) {
        m_sniff.insert(m_sniff.end(), bytes.begin(), bytes.end());
        if (m_sniff.size() < 8)
            return result;
        static constexpr std::uint8_t ebml[4] = { 0x1A, 0x45, 0xDF, 0xA3 };
        std::string const box(reinterpret_cast<char const*>(m_sniff.data()) + 4, 4);
        if (std::equal(ebml, ebml + 4, m_sniff.begin()))
            m_container = Container::WebM;
        else if (box == "ftyp" || box == "moov" || box == "styp" || box == "moof" || box == "sidx" || box == "free" || box == "skip" || box == "mdat")
            m_container = Container::Mp4;
        else {
            result.ok = false;
            return result;
        }
        bytes = m_sniff;
    }
    if (m_container == Container::WebM) {
        result.ok = m_webm.append(bytes);
        if (m_webm.init_segments() > m_inits_seen) {
            m_inits_seen = m_webm.init_segments();
            result.ok = take_init(m_webm.init().tracks, m_webm.init().duration_seconds) && result.ok;
            result.init_segment = result.ok;
        }
        // The frames read before a fault are still the stream's.
        for (WebmFrame& frame : m_webm.take_frames()) {
            for (Track& track : m_tracks) {
                if (track.description.number == frame.track) {
                    std::int64_t const decode_time = frame.time_ns;
                    file(track, std::move(frame), decode_time);
                    result.frames = true;
                    break;
                }
            }
        }
    } else {
        result.ok = m_mp4.append(bytes);
        if (m_mp4.init_segments() > m_inits_seen) {
            m_inits_seen = m_mp4.init_segments();
            std::vector<WebmTrack> tracks;
            for (Mp4Track const& track : m_mp4.init().tracks)
                tracks.push_back(describe(track));
            result.ok = take_init(tracks, m_mp4.init().duration_seconds) && result.ok;
            result.init_segment = result.ok;
        }
        for (Mp4Frame& frame : m_mp4.take_frames()) {
            for (Track& track : m_tracks) {
                if (track.description.number == frame.track) {
                    WebmFrame as_webm;
                    as_webm.track = frame.track;
                    as_webm.time_ns = frame.time_ns;
                    if (frame.duration_ns > 0)
                        as_webm.duration_ns = frame.duration_ns;
                    as_webm.key = frame.key;
                    as_webm.data = std::move(frame.data);
                    file(track, std::move(as_webm), frame.decode_time_ns);
                    result.frames = true;
                    break;
                }
            }
        }
    }
    m_sniff.clear();
    m_sniff.shrink_to_fit();
    return result;
}

bool StreamBuffer::parsing_media_segment() const
{
    if (m_container == Container::WebM)
        return !m_webm.at_segment_boundary();
    if (m_container == Container::Mp4)
        return !m_mp4.at_segment_boundary();
    return false;
}

void StreamBuffer::file(Track& track, WebmFrame&& frame, std::int64_t decode_time_ns)
{
    double const raw_seconds = to_seconds(frame.time_ns);
    if (mode == Mode::Sequence && group_start) {
        timestamp_offset = *group_start - raw_seconds;
        m_group_end = *group_start;
        for (Track& each : m_tracks) {
            each.need_key_frame = true;
            each.last_time_ns.reset();
            each.last_decode_ns.reset();
        }
        group_start.reset();
    }
    std::int64_t const time_ns = frame.time_ns + to_ns(timestamp_offset);

    // A step backwards, or a leap forwards, in decode time begins a new run
    // of frames (MSE §3.5.8 step 6): every track waits for a key frame
    // again. (Decode time, not presentation: a picture that leans on a
    // later one is presented before it and decoded after.)
    std::int64_t const decode_ns = decode_time_ns + (time_ns - frame.time_ns);
    if (track.last_decode_ns) {
        std::int64_t const distance = decode_ns - *track.last_decode_ns;
        // The leap that counts is past twice the longest frame seen, not
        // twice the last distance alone: with millisecond timestamps a
        // 60 fps stream's frames are 16 and 17 ms apart by turns, and one
        // dropped frame would otherwise begin a new run at every append.
        // The second frame of a run sets the pace; it is never a leap.
        std::int64_t const allowance = track.last_distance_ns > 0 ? 2 * std::max(track.last_distance_ns, track.longest_frame_ns) : 0;
        if (distance < 0 || (allowance > 0 && distance > allowance)) {
            if (mode == Mode::Sequence)
                group_start = m_group_end;
            for (Track& each : m_tracks) {
                each.need_key_frame = true;
                each.last_time_ns.reset();
                each.last_decode_ns.reset();
                each.last_distance_ns = 0;
            }
            if (mode == Mode::Sequence) {
                file(track, std::move(frame), decode_time_ns);
                return;
            }
        }
    }

    CodedFrame coded;
    coded.time_ns = time_ns;
    coded.decode_time_ns = decode_ns;
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
        if (coded.duration_estimated)
            coded.duration_ns = time_ns - *track.last_time_ns;
    }
    if (track.last_decode_ns && decode_ns > *track.last_decode_ns)
        track.last_distance_ns = decode_ns - *track.last_decode_ns;
    track.last_time_ns = time_ns;
    track.last_decode_ns = decode_ns;

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
                track.bytes -= erase_with_dependents(track, before, std::next(before));
        }
    }
    auto const from = frames.lower_bound(time_ns);
    auto to = from;
    while (to != frames.end() && (to->first < end_ns || to->first == time_ns))
        ++to;
    track.bytes -= erase_with_dependents(track, from, to);

    coded.data = std::move(frame.data);
    track.bytes += coded.data.size();
    track.longest_frame_ns = std::max(track.longest_frame_ns, coded.duration_ns);
    m_group_end = std::max(m_group_end, to_seconds(end_ns));
    if (auto const old = frames.find(time_ns); old != frames.end())
        track.decode_order.erase(old->second.decode_time_ns);
    track.decode_order[coded.decode_time_ns] = time_ns;
    frames.insert_or_assign(time_ns, std::move(coded));
}

void StreamBuffer::reset_parser()
{
    m_webm.reset();
    m_mp4.reset();
    m_sniff.clear();
    for (Track& track : m_tracks) {
        track.need_key_frame = true;
        track.last_time_ns.reset();
        track.last_decode_ns.reset();
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
        track.bytes -= erase_with_dependents(track, from, to);
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
            track.decode_order.erase(it->second.decode_time_ns);
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
    std::int64_t const slack = gap_slack_ns(track);
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

double StreamBuffer::highest_start() const
{
    std::int64_t highest = 0;
    for (Track const& track : m_tracks) {
        if (!track.frames.empty())
            highest = std::max(highest, track.frames.rbegin()->first);
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
