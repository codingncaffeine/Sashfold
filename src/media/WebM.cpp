#include "media/WebM.h"

#include <algorithm>
#include <bit>
#include <type_traits>
#include <utility>

namespace sashfold::media {

namespace {

constexpr std::uint64_t unknown_size = ~std::uint64_t { 0 };
// The largest element kept whole before it is read: Info, Tracks, a block.
constexpr std::uint64_t max_held_element = 64u * 1024u * 1024u;
constexpr std::size_t max_tracks = 64;
constexpr std::size_t max_laced_frames = 256;

enum Id : std::uint32_t {
    Ebml = 0x1A45DFA3,
    DocType = 0x4282,
    Segment = 0x18538067,
    Info = 0x1549A966,
    TimecodeScale = 0x2AD7B1,
    Duration = 0x4489,
    Tracks = 0x1654AE6B,
    TrackEntry = 0xAE,
    TrackNumber = 0xD7,
    TrackType = 0x83,
    CodecId = 0x86,
    CodecPrivate = 0x63A2,
    DefaultDuration = 0x23E383,
    CodecDelay = 0x56AA,
    SeekPreRoll = 0x56BB,
    Video = 0xE0,
    PixelWidth = 0xB0,
    PixelHeight = 0xBA,
    Audio = 0xE1,
    SamplingFrequency = 0xB5,
    Channels = 0x9F,
    Cluster = 0x1F43B675,
    Timecode = 0xE7,
    SimpleBlock = 0xA3,
    BlockGroup = 0xA0,
    Block = 0xA1,
    BlockDuration = 0x9B,
    ReferenceBlock = 0xFB,
    // The other children of a Segment, which end a Cluster of unknown size.
    SeekHead = 0x114D9B74,
    Cues = 0x1C53BB6B,
    Tags = 0x1254C367,
    Chapters = 0x1043A770,
    Attachments = 0x1941A469,
};

enum class Read { Ok,
    More,
    Bad };

struct Header {
    std::uint32_t id = 0;
    std::uint64_t size = 0; // unknown_size when every size bit is set
    std::size_t length = 0; // of the header itself
};

Read read_header(std::span<std::uint8_t const> in, Header& out)
{
    if (in.empty())
        return Read::More;
    std::size_t const id_length = static_cast<std::size_t>(std::countl_zero(in[0])) + 1;
    if (id_length > 4)
        return Read::Bad;
    if (in.size() < id_length + 1)
        return Read::More;
    std::uint32_t id = 0;
    for (std::size_t i = 0; i < id_length; ++i)
        id = (id << 8) | in[i];
    std::uint8_t const first = in[id_length];
    std::size_t const size_length = static_cast<std::size_t>(std::countl_zero(first)) + 1;
    if (size_length > 8)
        return Read::Bad;
    if (in.size() < id_length + size_length)
        return Read::More;
    std::uint8_t const mask = static_cast<std::uint8_t>(0xFFu >> size_length);
    std::uint64_t value = first & mask;
    bool all_ones = value == mask;
    for (std::size_t i = 1; i < size_length; ++i) {
        std::uint8_t const byte = in[id_length + i];
        value = (value << 8) | byte;
        all_ones = all_ones && byte == 0xFF;
    }
    out.id = id;
    out.size = all_ones ? unknown_size : value;
    out.length = id_length + size_length;
    return Read::Ok;
}

// A variable-length number with its marker taken off, as a block's track
// number and EBML lace sizes are written. `length` is how many bytes it took.
bool read_vint(std::span<std::uint8_t const> in, std::uint64_t& value, std::size_t& length)
{
    if (in.empty())
        return false;
    length = static_cast<std::size_t>(std::countl_zero(in[0])) + 1;
    if (length > 8 || in.size() < length)
        return false;
    value = in[0] & static_cast<std::uint8_t>(0xFFu >> length);
    for (std::size_t i = 1; i < length; ++i)
        value = (value << 8) | in[i];
    return true;
}

std::optional<std::uint64_t> read_uint(std::span<std::uint8_t const> body)
{
    if (body.size() > 8)
        return std::nullopt;
    std::uint64_t value = 0;
    for (std::uint8_t const byte : body)
        value = (value << 8) | byte;
    return value;
}

std::optional<double> read_float(std::span<std::uint8_t const> body)
{
    std::optional<std::uint64_t> const bits = read_uint(body);
    if (!bits)
        return std::nullopt;
    if (body.size() == 4)
        return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(*bits)));
    if (body.size() == 8)
        return std::bit_cast<double>(*bits);
    if (body.empty())
        return 0.0;
    return std::nullopt;
}

// Calls `visit(id, body)` for each child of a master element held whole.
// False when a child runs past its parent, has no size, or `visit` says so.
template<typename Visit>
bool each_child(std::span<std::uint8_t const> body, Visit visit)
{
    while (!body.empty()) {
        Header header;
        if (read_header(body, header) != Read::Ok)
            return false;
        if (header.size == unknown_size || header.size > body.size() - header.length)
            return false;
        if (!visit(header.id, body.subspan(header.length, static_cast<std::size_t>(header.size))))
            return false;
        body = body.subspan(header.length + static_cast<std::size_t>(header.size));
    }
    return true;
}

bool is_segment_child(std::uint32_t id)
{
    return id == Cluster || id == Info || id == Tracks || id == SeekHead || id == Cues || id == Tags || id == Chapters
        || id == Attachments;
}

}

WebmTrack const* WebmInit::track(std::uint64_t number) const
{
    for (WebmTrack const& candidate : tracks) {
        if (candidate.number == number)
            return &candidate;
    }
    return nullptr;
}

bool WebmParser::append(std::span<std::uint8_t const> bytes)
{
    if (m_failed)
        return false;
    if (m_skip > 0) {
        std::size_t const passed = static_cast<std::size_t>(std::min<std::uint64_t>(m_skip, bytes.size()));
        m_skip -= passed;
        bytes = bytes.subspan(passed);
    }
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
    if (!parse_available())
        m_failed = true;
    return !m_failed;
}

std::vector<WebmFrame> WebmParser::take_frames()
{
    return std::exchange(m_frames, {});
}

bool WebmParser::at_segment_boundary() const
{
    return m_pending.empty() && m_skip == 0 && !m_init_open && (!m_in_cluster || !m_cluster_sized || m_cluster_left == 0);
}

void WebmParser::reset()
{
    m_pending.clear();
    m_skip = 0;
    m_in_cluster = false;
    m_cluster_sized = false;
    m_cluster_left = 0;
    m_cluster_time.reset();
    m_init_open = false;
    m_have_info = false;
    m_have_tracks = false;
    m_building = {};
    m_failed = false;
}

void WebmParser::finish_init()
{
    m_init = std::move(m_building);
    m_building = {};
    m_have_info = false;
    m_have_tracks = false;
    m_init_open = false;
    ++m_init_segments;
}

bool WebmParser::parse_available()
{
    std::size_t at = 0;
    bool ok = true;
    while (at < m_pending.size()) {
        std::span<std::uint8_t const> const rest(m_pending.data() + at, m_pending.size() - at);
        if (m_in_cluster && m_cluster_sized && m_cluster_left == 0)
            m_in_cluster = false;
        Header header;
        Read const read = read_header(rest, header);
        if (read == Read::More)
            break;
        if (read == Read::Bad) {
            ok = false;
            break;
        }
        // A Cluster of unknown size lasts until a sibling of it begins.
        if (m_in_cluster && !m_cluster_sized && (is_segment_child(header.id) || header.id == Ebml || header.id == Segment))
            m_in_cluster = false;
        if (m_in_cluster && m_cluster_sized) {
            if (header.size == unknown_size || header.length + header.size > m_cluster_left) {
                ok = false;
                break;
            }
        }

        // The masters gone down into: only their headers are consumed.
        if (header.id == Segment && !m_in_cluster) {
            at += header.length;
            continue;
        }
        if (header.id == Cluster && !m_in_cluster) {
            if (m_init_open) {
                // Tracks without Info: the defaults stand.
                if (!m_have_tracks) {
                    ok = false;
                    break;
                }
                finish_init();
            }
            if (m_init_segments == 0) {
                ok = false; // media before any initialization segment
                break;
            }
            m_in_cluster = true;
            m_cluster_sized = header.size != unknown_size;
            m_cluster_left = m_cluster_sized ? header.size : 0;
            m_cluster_time.reset();
            at += header.length;
            continue;
        }
        if (header.size == unknown_size) {
            ok = false;
            break;
        }

        bool const wanted = m_in_cluster
            ? (header.id == Timecode || header.id == SimpleBlock || header.id == BlockGroup)
            : (header.id == Ebml || header.id == Info || header.id == Tracks);
        if (!wanted) {
            // Passed over, without waiting for it to arrive.
            if (m_in_cluster && m_cluster_sized)
                m_cluster_left -= header.length + header.size;
            std::uint64_t const total = header.length + header.size;
            std::uint64_t const here = std::min<std::uint64_t>(total, rest.size());
            at += static_cast<std::size_t>(here);
            m_skip = total - here;
            continue;
        }
        if (header.size > max_held_element) {
            ok = false;
            break;
        }
        if (rest.size() - header.length < header.size)
            break; // wait for the whole of it
        std::span<std::uint8_t const> const body = rest.subspan(header.length, static_cast<std::size_t>(header.size));
        switch (header.id) {
        case Ebml: {
            std::string doc_type;
            ok = each_child(body, [&](std::uint32_t id, std::span<std::uint8_t const> child) {
                if (id == DocType)
                    doc_type.assign(child.begin(), child.end());
                return true;
            });
            while (!doc_type.empty() && doc_type.back() == '\0')
                doc_type.pop_back();
            ok = ok && (doc_type == "webm" || doc_type == "matroska");
            m_building = {};
            m_have_info = false;
            m_have_tracks = false;
            m_init_open = true;
            break;
        }
        case Info:
            ok = m_init_open && parse_info(body);
            m_have_info = true;
            break;
        case Tracks:
            ok = m_init_open && parse_tracks(body);
            m_have_tracks = true;
            break;
        case Timecode: {
            std::optional<std::uint64_t> const value = read_uint(body);
            ok = value.has_value();
            if (value)
                m_cluster_time = *value;
            break;
        }
        case SimpleBlock:
            ok = parse_block(body, true, std::nullopt, false);
            break;
        case BlockGroup:
            ok = parse_block_group(body);
            break;
        default:
            break;
        }
        if (!ok)
            break;
        if (m_in_cluster && m_cluster_sized)
            m_cluster_left -= header.length + header.size;
        at += header.length + static_cast<std::size_t>(header.size);
        if (m_init_open && m_have_info && m_have_tracks)
            finish_init();
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(std::min(at, m_pending.size())));
    if (m_in_cluster && m_cluster_sized && m_cluster_left == 0)
        m_in_cluster = false;
    return ok;
}

bool WebmParser::parse_info(std::span<std::uint8_t const> body)
{
    std::optional<double> ticks;
    bool const ok = each_child(body, [&](std::uint32_t id, std::span<std::uint8_t const> child) {
        if (id == TimecodeScale) {
            std::optional<std::uint64_t> const value = read_uint(child);
            if (!value || *value == 0)
                return false;
            m_building.timecode_scale_ns = *value;
        } else if (id == Duration) {
            ticks = read_float(child);
            if (!ticks)
                return false;
        }
        return true;
    });
    if (ok && ticks && *ticks >= 0)
        m_building.duration_seconds = *ticks * static_cast<double>(m_building.timecode_scale_ns) / 1e9;
    return ok;
}

bool WebmParser::parse_tracks(std::span<std::uint8_t const> body)
{
    return each_child(body, [&](std::uint32_t id, std::span<std::uint8_t const> entry) {
        if (id != TrackEntry)
            return true;
        if (m_building.tracks.size() >= max_tracks)
            return false;
        WebmTrack track;
        auto const number = [](std::span<std::uint8_t const> child, auto& into) {
            std::optional<std::uint64_t> const value = read_uint(child);
            if (!value)
                return false;
            into = static_cast<std::remove_reference_t<decltype(into)>>(*value);
            return true;
        };
        bool const ok = each_child(entry, [&](std::uint32_t field, std::span<std::uint8_t const> child) {
            switch (field) {
            case TrackNumber:
                return number(child, track.number);
            case TrackType: {
                std::uint64_t type = 0;
                if (!number(child, type))
                    return false;
                track.kind = type == 1 ? WebmTrack::Kind::Video : type == 2 ? WebmTrack::Kind::Audio : WebmTrack::Kind::Other;
                return true;
            }
            case CodecId:
                track.codec_id.assign(child.begin(), child.end());
                while (!track.codec_id.empty() && track.codec_id.back() == '\0')
                    track.codec_id.pop_back();
                return true;
            case CodecPrivate:
                track.codec_private.assign(child.begin(), child.end());
                return true;
            case DefaultDuration:
                return number(child, track.default_duration_ns);
            case CodecDelay:
                return number(child, track.codec_delay_ns);
            case SeekPreRoll:
                return number(child, track.seek_preroll_ns);
            case Video:
                return each_child(child, [&](std::uint32_t inner, std::span<std::uint8_t const> value) {
                    if (inner == PixelWidth)
                        return number(value, track.width);
                    if (inner == PixelHeight)
                        return number(value, track.height);
                    return true;
                });
            case Audio:
                return each_child(child, [&](std::uint32_t inner, std::span<std::uint8_t const> value) {
                    if (inner == SamplingFrequency) {
                        std::optional<double> const rate = read_float(value);
                        if (!rate || !(*rate > 0))
                            return false;
                        track.sample_rate = *rate;
                        return true;
                    }
                    if (inner == Channels)
                        return number(value, track.channels);
                    return true;
                });
            default:
                return true;
            }
        });
        if (!ok || track.number == 0)
            return false;
        m_building.tracks.push_back(std::move(track));
        return true;
    });
}

bool WebmParser::parse_block_group(std::span<std::uint8_t const> body)
{
    std::span<std::uint8_t const> block;
    bool have_block = false;
    std::optional<std::uint64_t> duration;
    bool has_reference = false;
    bool const ok = each_child(body, [&](std::uint32_t id, std::span<std::uint8_t const> child) {
        if (id == Block) {
            block = child;
            have_block = true;
        } else if (id == BlockDuration) {
            duration = read_uint(child);
            if (!duration)
                return false;
        } else if (id == ReferenceBlock) {
            has_reference = true;
        }
        return true;
    });
    if (!ok || !have_block)
        return false;
    return parse_block(block, false, duration, has_reference);
}

bool WebmParser::parse_block(std::span<std::uint8_t const> body, bool simple, std::optional<std::uint64_t> duration, bool has_reference)
{
    if (!m_cluster_time)
        return false; // a block before its Cluster's Timecode
    std::uint64_t track_number = 0;
    std::size_t used = 0;
    if (!read_vint(body, track_number, used) || body.size() < used + 3)
        return false;
    std::int16_t const relative = static_cast<std::int16_t>(static_cast<std::uint16_t>((body[used] << 8) | body[used + 1]));
    std::uint8_t const flags = body[used + 2];
    std::span<std::uint8_t const> payload = body.subspan(used + 3);
    WebmTrack const* const track = m_init.track(track_number);
    if (track == nullptr)
        return true; // a track the initialization segment never named: ignored

    std::uint64_t const scale = m_init.timecode_scale_ns;
    // Cluster times are 64-bit on the wire; a time that cannot be held in
    // nanoseconds is a stream nothing could play.
    if (*m_cluster_time > static_cast<std::uint64_t>(INT64_MAX) / scale - 0x8000)
        return false;
    std::int64_t const time_ns = (static_cast<std::int64_t>(*m_cluster_time) + relative) * static_cast<std::int64_t>(scale);
    std::optional<std::uint64_t> duration_ns;
    bool const duration_from_block = duration && *duration <= static_cast<std::uint64_t>(INT64_MAX) / scale;
    if (duration_from_block)
        duration_ns = *duration * scale;
    bool const key = simple ? (flags & 0x80) != 0 : !has_reference;

    // The laces: how many frames the block carries and how long each is.
    std::vector<std::size_t> sizes;
    int const lacing = (flags >> 1) & 3;
    if (lacing == 0) {
        sizes.push_back(payload.size());
    } else {
        // Laced frames follow one another at the track's default length.
        if (!duration_ns && track->default_duration_ns != 0)
            duration_ns = track->default_duration_ns;
        if (payload.empty())
            return false;
        std::size_t const count = static_cast<std::size_t>(payload[0]) + 1;
        payload = payload.subspan(1);
        if (count > max_laced_frames)
            return false;
        std::size_t total = 0;
        if (lacing == 2) { // fixed-size
            if (payload.size() % count != 0)
                return false;
            sizes.assign(count, payload.size() / count);
        } else {
            std::int64_t previous = 0;
            for (std::size_t i = 0; i + 1 < count; ++i) {
                std::int64_t size = 0;
                if (lacing == 1) { // Xiph: bytes of 255 and then the rest
                    for (;;) {
                        if (payload.empty())
                            return false;
                        std::uint8_t const byte = payload[0];
                        payload = payload.subspan(1);
                        size += byte;
                        if (byte != 255)
                            break;
                    }
                } else { // EBML: the first a size, the others signed differences
                    std::uint64_t value = 0;
                    std::size_t length = 0;
                    if (!read_vint(payload, value, length))
                        return false;
                    payload = payload.subspan(length);
                    if (i == 0)
                        size = static_cast<std::int64_t>(value);
                    else
                        size = previous + static_cast<std::int64_t>(value) - ((std::int64_t { 1 } << (7 * length - 1)) - 1);
                }
                if (size < 0 || static_cast<std::uint64_t>(size) > payload.size())
                    return false;
                previous = size;
                sizes.push_back(static_cast<std::size_t>(size));
                total += static_cast<std::size_t>(size);
            }
            if (total > payload.size())
                return false;
            sizes.push_back(payload.size() - total);
        }
        if (duration_from_block)
            duration_ns = *duration_ns / count; // a BlockDuration covers the whole block
    }

    std::size_t offset = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] > payload.size() - offset)
            return false;
        WebmFrame frame;
        frame.track = track_number;
        frame.time_ns = time_ns + static_cast<std::int64_t>(i * duration_ns.value_or(0));
        frame.duration_ns = duration_ns;
        frame.key = key;
        frame.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.begin() + static_cast<std::ptrdiff_t>(offset + sizes[i]));
        m_frames.push_back(std::move(frame));
        offset += sizes[i];
    }
    return true;
}

}
