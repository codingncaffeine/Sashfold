#include "media/Mp4.h"

// The box structure (14496-12 §4): a size, a four-character type, and for
// full boxes a version and flags; a size of 1 means a 64-bit size follows,
// 0 that the box runs to the stream's end. Top-level boxes are read whole
// but for mdat, whose payload is kept by stream offset until the samples
// in it are known and given out — the tables may come after the data in
// a plain file, and a fragment's runs name their data by offset.

#include <algorithm>
#include <cstring>

namespace sashfold::media {

namespace {

constexpr std::size_t k_longest_box = std::size_t { 1 } << 28; // a moov or moof larger than this is not one
constexpr std::size_t k_most_samples = 1u << 24;
// Fragments name their samples before the bytes arrive, and a run with no
// per-sample fields names any number of them in sixteen bytes: what is named
// and not yet given out is held to this (a real fragment names hundreds).
constexpr std::size_t k_most_pending_fragment_samples = 1u << 20;

std::uint32_t be32(std::uint8_t const* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint16_t be16(std::uint8_t const* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint64_t be64(std::uint8_t const* p)
{
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}

struct Box {
    std::string type;
    std::span<std::uint8_t const> body; // after the header
    std::size_t header_size = 0;
    std::uint64_t size = 0; // whole, header included; 0: to the end
};

// The box at the start of `bytes`: nothing when the header is not whole.
// `whole` says whether all of its body is there.
std::optional<Box> read_box(std::span<std::uint8_t const> bytes, bool& whole)
{
    whole = false;
    if (bytes.size() < 8)
        return std::nullopt;
    Box box;
    box.size = be32(bytes.data());
    box.type.assign(reinterpret_cast<char const*>(bytes.data() + 4), 4);
    box.header_size = 8;
    if (box.size == 1) {
        if (bytes.size() < 16)
            return std::nullopt;
        box.size = be64(bytes.data() + 8);
        box.header_size = 16;
    }
    if (box.type == "uuid")
        box.header_size += 16;
    if (box.size != 0 && box.size < box.header_size)
        return std::nullopt;
    std::uint64_t const available = bytes.size();
    if (box.size == 0) {
        box.body = bytes.subspan(std::min<std::size_t>(box.header_size, bytes.size()));
        whole = false;
        return box;
    }
    whole = box.size <= available;
    std::size_t const end = static_cast<std::size_t>(std::min<std::uint64_t>(box.size, available));
    box.body = bytes.subspan(std::min<std::size_t>(box.header_size, end), end - std::min<std::size_t>(box.header_size, end));
    return box;
}

// Every child box of a body, whole ones only.
template <typename F>
bool for_each_child(std::span<std::uint8_t const> body, F&& f)
{
    std::size_t at = 0;
    while (at + 8 <= body.size()) {
        bool whole = false;
        std::optional<Box> const box = read_box(body.subspan(at), whole);
        if (!box)
            return false;
        std::uint64_t const size = box->size == 0 ? body.size() - at : box->size;
        if (size < box->header_size || at + size > body.size())
            return false;
        if (!f(*box))
            return false;
        at += static_cast<std::size_t>(size);
    }
    return true;
}

std::optional<Box> find_child(std::span<std::uint8_t const> body, char const* type)
{
    std::optional<Box> found;
    for_each_child(body, [&](Box const& box) {
        if (box.type == type && !found)
            found = box;
        return true;
    });
    return found;
}

// A full box's version and flags, the body after them.
bool full_box(std::span<std::uint8_t const> body, int& version, std::uint32_t& flags, std::span<std::uint8_t const>& rest)
{
    if (body.size() < 4)
        return false;
    version = body[0];
    flags = (std::uint32_t(body[1]) << 16) | (std::uint32_t(body[2]) << 8) | body[3];
    rest = body.subspan(4);
    return true;
}

// 14496-1 §8.3.3: an expandable size, up to four bytes.
std::size_t expandable(std::span<std::uint8_t const> bytes, std::size_t& at)
{
    std::size_t size = 0;
    for (int i = 0; i < 4 && at < bytes.size(); ++i) {
        std::uint8_t const b = bytes[at++];
        size = (size << 7) | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    return size;
}

// The AudioSpecificConfig in an esds (14496-14 §5.6), and the object type
// that says what it is for.
bool read_esds(std::span<std::uint8_t const> esds, std::uint8_t& object_type, std::vector<std::uint8_t>& specific)
{
    int version = 0;
    std::uint32_t flags = 0;
    std::span<std::uint8_t const> d;
    if (!full_box(esds, version, flags, d))
        return false;
    std::size_t at = 0;
    if (at >= d.size() || d[at++] != 0x03) // ES_Descriptor
        return false;
    (void)expandable(d, at);
    at += 2; // ES_ID
    if (at >= d.size())
        return false;
    std::uint8_t const es_flags = d[at++];
    if (es_flags & 0x80)
        at += 2; // dependsOn_ES_ID
    if (es_flags & 0x40) {
        if (at >= d.size())
            return false;
        at += 1 + d[at]; // URL
    }
    if (es_flags & 0x20)
        at += 2; // OCR_ES_ID
    if (at >= d.size() || d[at++] != 0x04) // DecoderConfigDescriptor
        return false;
    std::size_t const config_size = expandable(d, at);
    std::size_t const config_end = std::min(d.size(), at + config_size);
    if (at + 13 > d.size())
        return false;
    object_type = d[at];
    at += 13; // objectTypeIndication, streamType, bufferSizeDB, maxBitrate, avgBitrate
    if (at < config_end && d[at] == 0x05) { // DecoderSpecificInfo
        ++at;
        std::size_t const info_size = expandable(d, at);
        std::size_t const info_end = std::min(d.size(), at + info_size);
        specific.assign(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(info_end));
    }
    return true;
}

// --- Common Encryption (ISO/IEC 23001-7) ---------------------------------------------------

// A box with its header, as a 'pssh' is handed on whole.
std::span<std::uint8_t const> whole_box(Box const& box)
{
    return { box.body.data() - box.header_size, box.header_size + box.body.size() };
}

KeyId key_id_at(std::uint8_t const* p)
{
    KeyId id {};
    std::memcpy(id.data(), p, id.size());
    return id;
}

// The fields a 'tenc' (§8.2) and a 'seig' entry (§6) share, from the byte
// that holds the pattern: crypt and skip, isProtected, Per_Sample_IV_Size,
// KID and, for a protected entry without per-sample IVs, the constant IV.
// How many bytes it took, or nothing when it does not fit.
std::optional<std::size_t> read_encryption_fields(std::span<std::uint8_t const> d, Mp4SeigEntry& out)
{
    if (d.size() < 20)
        return std::nullopt;
    out.crypt_byte_block = d[1] >> 4;
    out.skip_byte_block = d[1] & 15;
    out.is_protected = d[2] != 0;
    out.per_sample_iv_size = d[3];
    if (out.per_sample_iv_size != 0 && out.per_sample_iv_size != 8 && out.per_sample_iv_size != 16)
        return std::nullopt;
    out.key_id = key_id_at(d.data() + 4);
    std::size_t at = 20;
    if (out.is_protected && out.per_sample_iv_size == 0) {
        if (at >= d.size())
            return std::nullopt;
        std::size_t const size = d[at++];
        if ((size != 8 && size != 16) || at + size > d.size())
            return std::nullopt;
        out.constant_iv.assign(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
    }
    return at;
}

// The 'sinf' of an 'encv' or 'enca' (14496-12 §8.12): the original format,
// and the scheme with the 'tenc' defaults when the scheme is Common
// Encryption's. Nothing when there is no usable one.
std::optional<std::string> read_sinf(std::span<std::uint8_t const> children, std::optional<Mp4Protection>& protection)
{
    std::optional<Box> const sinf = find_child(children, "sinf");
    if (!sinf)
        return std::nullopt;
    std::optional<Box> const frma = find_child(sinf->body, "frma");
    if (!frma || frma->body.size() < 4)
        return std::nullopt;
    std::string const original(reinterpret_cast<char const*>(frma->body.data()), 4);
    std::optional<Box> const schm = find_child(sinf->body, "schm");
    std::optional<Box> const schi = find_child(sinf->body, "schi");
    std::optional<Box> const tenc = schi ? find_child(schi->body, "tenc") : std::nullopt;
    int version = 0;
    std::uint32_t flags = 0;
    std::span<std::uint8_t const> d;
    if (!schm || !full_box(schm->body, version, flags, d) || d.size() < 8)
        return original;
    std::uint32_t const scheme = be32(d.data());
    if (scheme != scheme_cenc && scheme != scheme_cens && scheme != scheme_cbc1 && scheme != scheme_cbcs)
        return original;
    if (!tenc || !full_box(tenc->body, version, flags, d))
        return original;
    Mp4SeigEntry fields;
    if (!read_encryption_fields(d, fields))
        return original;
    Mp4Protection made;
    made.scheme = scheme;
    made.is_protected = fields.is_protected;
    made.per_sample_iv_size = fields.per_sample_iv_size;
    made.key_id = fields.key_id;
    made.constant_iv = std::move(fields.constant_iv);
    // A version 0 'tenc' has no pattern: its second byte is reserved.
    if (version >= 1) {
        made.crypt_byte_block = fields.crypt_byte_block;
        made.skip_byte_block = fields.skip_byte_block;
    }
    protection = std::move(made);
    return original;
}

// The 'seig' descriptions of an 'sgpd' (14496-12 §8.9.3), in order.
bool read_seig_descriptions(std::span<std::uint8_t const> parent, std::vector<Mp4SeigEntry>& out)
{
    return for_each_child(parent, [&](Box const& box) {
        if (box.type != "sgpd")
            return true;
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(box.body, version, flags, d) || d.size() < 8)
            return false;
        if (std::string(reinterpret_cast<char const*>(d.data()), 4) != "seig")
            return true;
        std::size_t at = 4;
        std::uint32_t default_length = 0;
        if (version == 1) {
            default_length = be32(d.data() + at);
            at += 4;
        } else if (version >= 2) {
            at += 4; // default_sample_description_index
        }
        if (at + 4 > d.size())
            return false;
        std::uint32_t const count = be32(d.data() + at);
        at += 4;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::size_t length = default_length;
            if (version == 1 && default_length == 0) {
                if (at + 4 > d.size())
                    return false;
                length = be32(d.data() + at);
                at += 4;
            }
            if (at > d.size())
                return false;
            Mp4SeigEntry entry;
            std::span<std::uint8_t const> const rest = d.subspan(at, length != 0 ? std::min(length, d.size() - at) : d.size() - at);
            std::optional<std::size_t> const used = read_encryption_fields(rest, entry);
            if (!used)
                return false;
            at += length != 0 ? length : *used;
            out.push_back(std::move(entry));
        }
        return true;
    });
}

// The 'seig' group description index of each of `count` samples, from the
// container's 'sbgp' (14496-12 §8.9.2); zero for a sample no group holds.
bool read_seig_groups(std::span<std::uint8_t const> parent, std::size_t count, std::vector<std::uint32_t>& out)
{
    out.assign(count, 0);
    return for_each_child(parent, [&](Box const& box) {
        if (box.type != "sbgp")
            return true;
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(box.body, version, flags, d) || d.size() < 8)
            return false;
        if (std::string(reinterpret_cast<char const*>(d.data()), 4) != "seig")
            return true;
        std::size_t at = version == 1 ? 8 : 4;
        if (at + 4 > d.size())
            return false;
        std::uint32_t const entries = be32(d.data() + at);
        at += 4;
        if (d.size() < at + std::size_t { entries } * 8)
            return false;
        std::size_t sample = 0;
        for (std::uint32_t e = 0; e < entries && sample < count; ++e) {
            std::uint32_t const n = be32(d.data() + at + e * 8);
            std::uint32_t const index = be32(d.data() + at + e * 8 + 4);
            for (std::uint32_t k = 0; k < n && sample < count; ++k)
                out[sample++] = index;
        }
        return true;
    });
}

// One sample's auxiliary information (§7.2.2): its IV, then, when there is
// more, its subsample map.
bool read_aux_entry(std::span<std::uint8_t const> d, std::size_t iv_size, bool has_subsamples, std::vector<std::uint8_t>& iv,
    std::vector<Subsample>& subsamples, std::size_t& used)
{
    if (iv_size > d.size())
        return false;
    iv.assign(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(iv_size));
    std::size_t at = iv_size;
    if (has_subsamples) {
        if (at + 2 > d.size())
            return false;
        std::size_t const count = be16(d.data() + at);
        at += 2;
        if (at + count * 6 > d.size())
            return false;
        subsamples.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            subsamples[i].clear_bytes = be16(d.data() + at);
            subsamples[i].protected_bytes = be32(d.data() + at + 2);
            at += 6;
        }
    }
    used = at;
    return true;
}

// How each of `count` samples of a sample table or a track fragment is
// encrypted, from the track's defaults, the 'seig' groups the samples are
// in, and their auxiliary information — the 'senc' box when there is one,
// else what 'saiz' and 'saio' point at, which `aux_at` reads by offset.
// Nothing for a sample in the clear. False when the boxes do not hold
// together.
template<typename AuxAt>
bool read_sample_encryption(std::span<std::uint8_t const> parent, Mp4Track const& track, std::vector<Mp4SeigEntry> const& own_seig,
    std::size_t count, AuxAt&& aux_at, std::vector<std::optional<SampleEncryption>>& out)
{
    out.assign(count, std::nullopt);
    if (!track.protection || count == 0)
        return true;
    Mp4Protection const& protection = *track.protection;
    std::vector<std::uint32_t> groups;
    if (!read_seig_groups(parent, count, groups))
        return false;
    // Each sample's fields: the track's, or its group's.
    std::vector<Mp4SeigEntry> fields(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t const group = groups[i];
        Mp4SeigEntry const* entry = nullptr;
        if (group > 0x10000 && group - 0x10001 < own_seig.size())
            entry = &own_seig[group - 0x10001];
        else if (group > 0 && group <= 0x10000 && group - 1 < track.seig.size())
            entry = &track.seig[group - 1];
        else if (group != 0)
            return false;
        if (entry) {
            fields[i] = *entry;
        } else {
            fields[i].is_protected = protection.is_protected;
            fields[i].per_sample_iv_size = protection.per_sample_iv_size;
            fields[i].key_id = protection.key_id;
            fields[i].constant_iv = protection.constant_iv;
            fields[i].crypt_byte_block = protection.crypt_byte_block;
            fields[i].skip_byte_block = protection.skip_byte_block;
        }
    }
    auto const iv_size_of = [&](std::size_t i) -> std::size_t { return fields[i].is_protected ? fields[i].per_sample_iv_size : 0; };

    std::vector<std::vector<std::uint8_t>> ivs(count);
    std::vector<std::vector<Subsample>> maps(count);
    if (std::optional<Box> const senc = find_child(parent, "senc")) {
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(senc->body, version, flags, d) || d.size() < 4)
            return false;
        std::size_t const samples = be32(d.data());
        if (samples < count)
            return false;
        std::size_t at = 4;
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t used = 0;
            if (!read_aux_entry(d.subspan(at), iv_size_of(i), flags & 0x2, ivs[i], maps[i], used))
                return false;
            at += used;
        }
    } else if (std::optional<Box> const saiz = find_child(parent, "saiz")) {
        std::optional<Box> const saio = find_child(parent, "saio");
        if (!saio)
            return false;
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(saiz->body, version, flags, d))
            return false;
        std::size_t at = flags & 1 ? 8 : 0;
        if (at + 5 > d.size())
            return false;
        std::uint8_t const default_size = d[at];
        std::size_t const samples = be32(d.data() + at + 1);
        at += 5;
        if (samples < count || (default_size == 0 && at + count > d.size()))
            return false;
        std::vector<std::size_t> sizes(count, default_size);
        if (default_size == 0) {
            for (std::size_t i = 0; i < count; ++i)
                sizes[i] = d[at + i];
        }
        int offset_version = 0;
        std::uint32_t offset_flags = 0;
        std::span<std::uint8_t const> o;
        if (!full_box(saio->body, offset_version, offset_flags, o))
            return false;
        std::size_t p = offset_flags & 1 ? 8 : 0;
        if (p + 4 > o.size())
            return false;
        std::uint32_t const entries = be32(o.data() + p);
        p += 4;
        // One offset: the information of every sample, one after another.
        if (entries != 1 || p + (offset_version == 1 ? 8 : 4) > o.size())
            return false;
        std::uint64_t offset = offset_version == 1 ? be64(o.data() + p) : be32(o.data() + p);
        for (std::size_t i = 0; i < count; ++i) {
            if (sizes[i] > 0) {
                std::optional<std::span<std::uint8_t const>> const bytes = aux_at(offset, sizes[i]);
                if (!bytes)
                    return false;
                std::size_t used = 0;
                if (!read_aux_entry(*bytes, iv_size_of(i), sizes[i] > iv_size_of(i), ivs[i], maps[i], used))
                    return false;
            }
            offset += sizes[i];
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!fields[i].is_protected)
            continue;
        SampleEncryption made;
        made.scheme = protection.scheme;
        made.key_id.assign(fields[i].key_id.begin(), fields[i].key_id.end());
        made.iv = fields[i].per_sample_iv_size > 0 ? std::move(ivs[i]) : fields[i].constant_iv;
        made.subsamples = std::move(maps[i]);
        // The pattern is the pattern schemes' alone ('cenc' and 'cbc1'
        // encrypt every protected byte or block).
        if (protection.scheme == scheme_cens || protection.scheme == scheme_cbcs) {
            made.crypt_byte_block = fields[i].crypt_byte_block;
            made.skip_byte_block = fields[i].skip_byte_block;
        }
        out[i] = std::move(made);
    }
    return true;
}

} // namespace

Mp4Track const* Mp4Init::track(std::uint32_t id) const
{
    for (Mp4Track const& t : tracks) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

std::int64_t Mp4Parser::to_ns(std::int64_t ticks, std::uint32_t timescale) const
{
    if (timescale == 0)
        return 0;
    // Whole seconds apart from the rest: the rest times a billion fits.
    // Past 290 years (a broken header) the time is held at the edge.
    std::int64_t const whole = ticks / timescale;
    std::int64_t const rest = ticks % timescale;
    constexpr std::int64_t most_seconds = 9'000'000'000;
    if (whole > most_seconds)
        return most_seconds * 1000000000;
    if (whole < -most_seconds)
        return -most_seconds * 1000000000;
    return whole * 1000000000 + rest * 1000000000 / timescale;
}

bool Mp4Parser::append(std::span<std::uint8_t const> bytes)
{
    if (m_failed)
        return false;
    std::size_t at = 0;
    // Bytes of a box being passed over, or of an mdat being received.
    while (at < bytes.size()) {
        if (m_skip > 0) {
            std::size_t const take = static_cast<std::size_t>(std::min<std::uint64_t>(m_skip, bytes.size() - at));
            m_skip -= take;
            m_stream_offset += take;
            at += take;
            continue;
        }
        if (m_mdat_left) {
            std::size_t const take = static_cast<std::size_t>(std::min<std::uint64_t>(*m_mdat_left, bytes.size() - at));
            Mdat& mdat = m_mdats.back();
            mdat.bytes.insert(mdat.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                bytes.begin() + static_cast<std::ptrdiff_t>(at + take));
            *m_mdat_left -= take;
            m_stream_offset += take;
            at += take;
            if (*m_mdat_left == 0)
                m_mdat_left.reset();
            emit_ready();
            continue;
        }
        break;
    }
    if (at < bytes.size())
        m_pending.insert(m_pending.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    if (!parse_available()) {
        m_failed = true;
        return false;
    }
    return true;
}

bool Mp4Parser::parse_available()
{
    while (true) {
        if (m_mdat_left || m_skip > 0) {
            // The rest of m_pending belongs to the box being passed over or
            // received: hand it back through append's paths.
            std::vector<std::uint8_t> rest = std::move(m_pending);
            m_pending.clear();
            if (!rest.empty())
                return append(rest);
            return true;
        }
        bool whole = false;
        std::optional<Box> const box = read_box(m_pending, whole);
        if (!box)
            return true; // the header is not whole yet
        if (box->type == "mdat") {
            // Received as it comes; its samples are given out as they are known.
            Mdat mdat;
            mdat.offset = m_stream_offset + box->header_size;
            std::uint64_t const payload = box->size == 0 ? std::uint64_t(-1) : box->size - box->header_size;
            m_mdats.push_back(std::move(mdat));
            m_mdat_left = payload;
            m_stream_offset += box->header_size;
            m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(box->header_size));
            continue;
        }
        if (box->size == 0)
            return true; // only an mdat may run to the end; anything else waits for more (and gets nothing)
        if (box->size > k_longest_box) {
            // Not worth keeping whole: passed over.
            m_skip = box->size - std::min<std::uint64_t>(box->size, m_pending.size());
            m_stream_offset += box->size;
            m_pending.clear();
            continue;
        }
        if (!whole)
            return true;
        std::uint64_t const box_offset = m_stream_offset;
        if (box->type == "moov") {
            if (!parse_moov(box->body, box_offset + box->header_size))
                return false;
        } else if (box->type == "moof") {
            if (!parse_moof(box->body, box_offset, box->header_size))
                return false;
        }
        // ftyp, styp, sidx, free, skip, uuid, meta: passed over.
        m_stream_offset += box->size;
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(box->size));
        emit_ready();
    }
}

bool Mp4Parser::parse_moov(std::span<std::uint8_t const> body, std::uint64_t body_offset)
{
    Mp4Init init;
    std::optional<Box> const mvhd = find_child(body, "mvhd");
    if (mvhd) {
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(mvhd->body, version, flags, d))
            return false;
        if (version == 1 && d.size() >= 28) {
            init.timescale = be32(d.data() + 16);
            std::uint64_t const duration = be64(d.data() + 20);
            if (init.timescale && duration != std::uint64_t(-1))
                init.duration_seconds = static_cast<double>(duration) / init.timescale;
        } else if (version == 0 && d.size() >= 16) {
            init.timescale = be32(d.data() + 8);
            std::uint32_t const duration = be32(d.data() + 12);
            if (init.timescale && duration != 0xFFFFFFFFu)
                init.duration_seconds = static_cast<double>(duration) / init.timescale;
        }
    }
    m_init = std::move(init);
    bool ok = for_each_child(body, [&](Box const& box) {
        if (box.type == "trak")
            return parse_trak(box.body, body_offset + static_cast<std::uint64_t>(box.body.data() - body.data()));
        if (box.type == "pssh") {
            std::span<std::uint8_t const> const whole = whole_box(box);
            m_init.pssh.emplace_back(whole.begin(), whole.end());
            return true;
        }
        if (box.type == "mvex") {
            m_init.fragmented = true;
            return for_each_child(box.body, [&](Box const& child) {
                if (child.type != "trex")
                    return true;
                int version = 0;
                std::uint32_t flags = 0;
                std::span<std::uint8_t const> d;
                if (!full_box(child.body, version, flags, d) || d.size() < 20)
                    return false;
                std::uint32_t const id = be32(d.data());
                for (Mp4Track& t : m_init.tracks) {
                    if (t.id == id) {
                        t.default_sample_duration = be32(d.data() + 8);
                        t.default_sample_size = be32(d.data() + 12);
                        t.default_sample_flags = be32(d.data() + 16);
                    }
                }
                return true;
            });
        }
        return true;
    });
    if (!ok)
        return false;
    // Tracks are named in the moov before the mvex: the trex pass above
    // relies on that order (the usual one); a second pass covers the other.
    m_fragment_decode_time.assign(m_init.tracks.size(), 0);
    ++m_init_segments;
    return true;
}

bool Mp4Parser::parse_sample_entry(std::span<std::uint8_t const> box_bytes, Mp4Track& track)
{
    bool whole = false;
    std::optional<Box> const entry = read_box(box_bytes, whole);
    if (!entry || !whole)
        return false;
    track.sample_entry = entry->type;
    std::span<std::uint8_t const> const d = entry->body;
    // A protected entry is the original one renamed, with a 'sinf' among
    // its children saying what it was (14496-12 §8.12).
    std::string type = entry->type;
    if (track.kind == Mp4Track::Kind::Video) {
        if (d.size() < 78)
            return false;
        track.width = be16(d.data() + 24);
        track.height = be16(d.data() + 26);
        std::span<std::uint8_t const> const children = d.subspan(78);
        if (type == "encv") {
            std::optional<std::string> const original = read_sinf(children, track.protection);
            if (!original)
                return true;
            type = *original;
        }
        if (type == "avc1" || type == "avc3") {
            track.codec_id = "V_H264";
            if (std::optional<Box> const avcc = find_child(children, "avcC"))
                track.codec_private.assign(avcc->body.begin(), avcc->body.end());
        } else if (type == "hvc1" || type == "hev1") {
            track.codec_id = "V_H265";
            if (std::optional<Box> const hvcc = find_child(children, "hvcC"))
                track.codec_private.assign(hvcc->body.begin(), hvcc->body.end());
        } else if (type == "vp09") {
            track.codec_id = "V_VP9";
            if (std::optional<Box> const vpcc = find_child(children, "vpcC"))
                track.codec_private.assign(vpcc->body.begin(), vpcc->body.end());
        } else if (type == "av01") {
            track.codec_id = "V_AV1";
            if (std::optional<Box> const av1c = find_child(children, "av1C"))
                track.codec_private.assign(av1c->body.begin(), av1c->body.end());
        }
        return true;
    }
    if (track.kind == Mp4Track::Kind::Audio) {
        if (d.size() < 28)
            return false;
        std::uint16_t const version = be16(d.data() + 8);
        track.channels = be16(d.data() + 16);
        track.sample_rate = be16(d.data() + 24); // 16.16: the whole part
        std::size_t children_at = 28;
        if (version == 1)
            children_at += 16;
        else if (version == 2)
            children_at += 36;
        if (children_at > d.size())
            return false;
        std::span<std::uint8_t const> const children = d.subspan(children_at);
        if (type == "enca") {
            std::optional<std::string> const original = read_sinf(children, track.protection);
            if (!original)
                return true;
            type = *original;
        }
        if (type == "mp4a") {
            std::uint8_t object_type = 0;
            std::vector<std::uint8_t> specific;
            if (std::optional<Box> const esds = find_child(children, "esds"); esds && read_esds(esds->body, object_type, specific)) {
                if (object_type == 0x40 || object_type == 0x66 || object_type == 0x67 || object_type == 0x68) {
                    track.codec_id = "A_AAC";
                    track.codec_private = std::move(specific);
                    // The configuration's own rate and channels stand over the entry's.
                    if (track.codec_private.size() >= 2) {
                        static constexpr std::uint32_t rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000,
                            11025, 8000, 7350, 0, 0, 0 };
                        std::uint8_t const index = static_cast<std::uint8_t>(((track.codec_private[0] & 7) << 1) | (track.codec_private[1] >> 7));
                        std::uint8_t const channel_config = (track.codec_private[1] >> 3) & 15;
                        if (index < 13 && rates[index])
                            track.sample_rate = rates[index];
                        if (channel_config >= 1 && channel_config <= 7)
                            track.channels = channel_config == 7 ? 8 : channel_config;
                    }
                } else if (object_type == 0x69 || object_type == 0x6B) {
                    track.codec_id = "A_MP3";
                }
            }
        } else if (type == "Opus") {
            track.codec_id = "A_OPUS";
            if (std::optional<Box> const dops = find_child(children, "dOps"))
                track.codec_private.assign(dops->body.begin(), dops->body.end());
        } else if (type == "fLaC") {
            track.codec_id = "A_FLAC";
        }
        return true;
    }
    return true;
}

bool Mp4Parser::parse_trak(std::span<std::uint8_t const> body, std::uint64_t body_offset)
{
    Mp4Track track;
    std::optional<Box> const tkhd = find_child(body, "tkhd");
    if (!tkhd)
        return false;
    {
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(tkhd->body, version, flags, d))
            return false;
        if (version == 1 && d.size() >= 20)
            track.id = be32(d.data() + 16);
        else if (version == 0 && d.size() >= 12)
            track.id = be32(d.data() + 8);
        else
            return false;
    }
    std::optional<Box> const mdia = find_child(body, "mdia");
    if (!mdia)
        return false;
    std::uint64_t media_duration = 0;
    if (std::optional<Box> const mdhd = find_child(mdia->body, "mdhd")) {
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(mdhd->body, version, flags, d))
            return false;
        std::size_t language_at = 0;
        if (version == 1 && d.size() >= 30) {
            track.timescale = be32(d.data() + 16);
            media_duration = be64(d.data() + 20);
            language_at = 28;
        } else if (version == 0 && d.size() >= 18) {
            track.timescale = be32(d.data() + 8);
            media_duration = be32(d.data() + 12);
            language_at = 16;
        }
        if (track.timescale == 0)
            return false;
        if (media_duration != 0 && media_duration != std::uint64_t(-1) && media_duration != 0xFFFFFFFFu)
            track.duration_seconds = static_cast<double>(media_duration) / track.timescale;
        if (language_at) {
            // Three five-bit letters, each 0x60 above its code.
            std::uint16_t const packed = be16(d.data() + language_at);
            track.language = { static_cast<char>(((packed >> 10) & 31) + 0x60), static_cast<char>(((packed >> 5) & 31) + 0x60),
                static_cast<char>((packed & 31) + 0x60) };
        }
    }
    if (std::optional<Box> const hdlr = find_child(mdia->body, "hdlr"); hdlr && hdlr->body.size() >= 12) {
        std::string const handler(reinterpret_cast<char const*>(hdlr->body.data() + 8), 4);
        track.kind = handler == "vide" ? Mp4Track::Kind::Video : handler == "soun" ? Mp4Track::Kind::Audio : Mp4Track::Kind::Other;
    }
    // The edit list: the one entry that starts the presentation inside the
    // media (a composition offset), or an empty edit before it.
    if (std::optional<Box> const edts = find_child(body, "edts")) {
        if (std::optional<Box> const elst = find_child(edts->body, "elst")) {
            int version = 0;
            std::uint32_t flags = 0;
            std::span<std::uint8_t const> d;
            if (full_box(elst->body, version, flags, d) && d.size() >= 4) {
                std::uint32_t const count = be32(d.data());
                std::size_t at = 4;
                std::int64_t empty_before = 0; // movie timescale
                for (std::uint32_t i = 0; i < count && i < 64; ++i) {
                    std::int64_t segment_duration, media_time;
                    if (version == 1) {
                        if (at + 20 > d.size())
                            break;
                        segment_duration = static_cast<std::int64_t>(be64(d.data() + at));
                        media_time = static_cast<std::int64_t>(be64(d.data() + at + 8));
                        at += 20;
                    } else {
                        if (at + 12 > d.size())
                            break;
                        segment_duration = be32(d.data() + at);
                        media_time = static_cast<std::int32_t>(be32(d.data() + at + 4));
                        at += 12;
                    }
                    if (media_time == -1) {
                        empty_before += segment_duration;
                        continue;
                    }
                    // The first real edit: where the media starts, less any
                    // empty edit before it (in the track's own ticks).
                    track.edit_media_time = media_time;
                    if (empty_before > 0 && m_init.timescale > 0)
                        track.edit_media_time -= static_cast<std::int64_t>(static_cast<long double>(empty_before) * track.timescale / m_init.timescale);
                    break;
                }
            }
        }
    }
    std::optional<Box> const minf = find_child(mdia->body, "minf");
    std::optional<Box> const stbl = minf ? find_child(minf->body, "stbl") : std::nullopt;
    if (!stbl)
        return false;
    if (std::optional<Box> const stsd = find_child(stbl->body, "stsd")) {
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(stsd->body, version, flags, d) || d.size() < 4)
            return false;
        if (be32(d.data()) >= 1 && !parse_sample_entry(d.subspan(4), track))
            return false;
    }

    // The sample tables, into samples by offset (14496-12 §8.7.4, §8.7.5):
    // only a plain file has them filled; a fragmented one's are empty.
    std::vector<std::uint32_t> sizes;
    if (std::optional<Box> const stsz = find_child(stbl->body, "stsz")) {
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(stsz->body, version, flags, d) || d.size() < 8)
            return false;
        std::uint32_t const constant = be32(d.data());
        std::uint32_t const count = be32(d.data() + 4);
        if (count > k_most_samples)
            return false;
        if (constant != 0) {
            sizes.assign(count, constant);
        } else {
            if (d.size() < 8 + std::size_t { count } * 4)
                return false;
            sizes.resize(count);
            for (std::uint32_t i = 0; i < count; ++i)
                sizes[i] = be32(d.data() + 8 + i * 4);
        }
    } else if (std::optional<Box> const stz2 = find_child(stbl->body, "stz2")) {
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(stz2->body, version, flags, d) || d.size() < 8)
            return false;
        int const field_size = d[3];
        std::uint32_t const count = be32(d.data() + 4);
        if (count > k_most_samples || (field_size != 4 && field_size != 8 && field_size != 16))
            return false;
        if (d.size() < 8 + (std::size_t { count } * field_size + 7) / 8)
            return false;
        sizes.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (field_size == 4)
                sizes[i] = (d[8 + i / 2] >> (i % 2 ? 0 : 4)) & 15;
            else if (field_size == 8)
                sizes[i] = d[8 + i];
            else
                sizes[i] = be16(d.data() + 8 + i * 2);
        }
    }
    std::size_t const count = sizes.size();
    if (count > 0) {
        std::vector<Sample> samples(count);
        for (std::size_t i = 0; i < count; ++i) {
            samples[i].size = sizes[i];
            samples[i].track = track.id;
        }
        // Decode times from the deltas.
        if (std::optional<Box> const stts = find_child(stbl->body, "stts")) {
            int version = 0;
            std::uint32_t flags = 0;
            std::span<std::uint8_t const> d;
            if (!full_box(stts->body, version, flags, d) || d.size() < 4)
                return false;
            std::uint32_t const entries = be32(d.data());
            if (d.size() < 4 + std::size_t { entries } * 8)
                return false;
            std::int64_t time = 0;
            std::size_t sample = 0;
            for (std::uint32_t e = 0; e < entries && sample < count; ++e) {
                std::uint32_t const n = be32(d.data() + 4 + e * 8);
                std::uint32_t const delta = be32(d.data() + 8 + e * 8);
                for (std::uint32_t k = 0; k < n && sample < count; ++k, ++sample) {
                    samples[sample].decode_time = time;
                    samples[sample].duration = delta;
                    time += delta;
                }
            }
        }
        if (std::optional<Box> const ctts = find_child(stbl->body, "ctts")) {
            int version = 0;
            std::uint32_t flags = 0;
            std::span<std::uint8_t const> d;
            if (!full_box(ctts->body, version, flags, d) || d.size() < 4)
                return false;
            std::uint32_t const entries = be32(d.data());
            if (d.size() < 4 + std::size_t { entries } * 8)
                return false;
            std::size_t sample = 0;
            for (std::uint32_t e = 0; e < entries && sample < count; ++e) {
                std::uint32_t const n = be32(d.data() + 4 + e * 8);
                std::int32_t const offset = static_cast<std::int32_t>(be32(d.data() + 8 + e * 8));
                for (std::uint32_t k = 0; k < n && sample < count; ++k, ++sample)
                    samples[sample].composition_offset = offset; // unsigned in version 0: never past 2^31 in a real file
            }
        }
        if (std::optional<Box> const stss = find_child(stbl->body, "stss")) {
            int version = 0;
            std::uint32_t flags = 0;
            std::span<std::uint8_t const> d;
            if (!full_box(stss->body, version, flags, d) || d.size() < 4)
                return false;
            for (Sample& s : samples)
                s.key = false;
            std::uint32_t const entries = be32(d.data());
            if (d.size() < 4 + std::size_t { entries } * 4)
                return false;
            for (std::uint32_t e = 0; e < entries; ++e) {
                std::uint32_t const number = be32(d.data() + 4 + e * 4);
                if (number >= 1 && number <= count)
                    samples[number - 1].key = true;
            }
        }
        // Chunks: which samples each holds, and where it starts.
        std::vector<std::uint64_t> chunk_offsets;
        if (std::optional<Box> const stco = find_child(stbl->body, "stco")) {
            int version = 0;
            std::uint32_t flags = 0;
            std::span<std::uint8_t const> d;
            if (!full_box(stco->body, version, flags, d) || d.size() < 4)
                return false;
            std::uint32_t const entries = be32(d.data());
            if (entries > k_most_samples || d.size() < 4 + std::size_t { entries } * 4)
                return false;
            for (std::uint32_t e = 0; e < entries; ++e)
                chunk_offsets.push_back(be32(d.data() + 4 + e * 4));
        } else if (std::optional<Box> const co64 = find_child(stbl->body, "co64")) {
            int version = 0;
            std::uint32_t flags = 0;
            std::span<std::uint8_t const> d;
            if (!full_box(co64->body, version, flags, d) || d.size() < 4)
                return false;
            std::uint32_t const entries = be32(d.data());
            if (entries > k_most_samples || d.size() < 4 + std::size_t { entries } * 8)
                return false;
            for (std::uint32_t e = 0; e < entries; ++e)
                chunk_offsets.push_back(be64(d.data() + 4 + e * 8));
        }
        std::optional<Box> const stsc = find_child(stbl->body, "stsc");
        if (!stsc || chunk_offsets.empty())
            return false;
        {
            int version = 0;
            std::uint32_t flags = 0;
            std::span<std::uint8_t const> d;
            if (!full_box(stsc->body, version, flags, d) || d.size() < 4)
                return false;
            std::uint32_t const entries = be32(d.data());
            if (d.size() < 4 + std::size_t { entries } * 12)
                return false;
            std::size_t sample = 0;
            for (std::uint32_t e = 0; e < entries && sample < count; ++e) {
                std::uint32_t const first_chunk = be32(d.data() + 4 + e * 12);
                std::uint32_t const per_chunk = be32(d.data() + 8 + e * 12);
                std::uint32_t const next_first = e + 1 < entries ? be32(d.data() + 4 + (e + 1) * 12) : static_cast<std::uint32_t>(chunk_offsets.size()) + 1;
                if (first_chunk < 1 || next_first < first_chunk)
                    return false;
                for (std::uint32_t chunk = first_chunk; chunk < next_first && sample < count; ++chunk) {
                    if (chunk > chunk_offsets.size())
                        return false;
                    std::uint64_t offset = chunk_offsets[chunk - 1];
                    for (std::uint32_t k = 0; k < per_chunk && sample < count; ++k, ++sample) {
                        samples[sample].offset = offset;
                        offset += samples[sample].size;
                    }
                }
            }
            if (sample < count)
                return false; // samples no chunk holds
        }
        // A protected track's samples, each with how it is encrypted: the
        // table's 'senc', or its 'saiz' and 'saio' pointing (by offset in
        // the file) at information in this track's boxes.
        if (track.protection) {
            if (!read_seig_descriptions(stbl->body, track.seig))
                return false;
            std::vector<std::optional<SampleEncryption>> encryptions;
            auto const aux_at = [&](std::uint64_t offset, std::size_t size) -> std::optional<std::span<std::uint8_t const>> {
                if (offset < body_offset || offset - body_offset > body.size() || size > body.size() - (offset - body_offset))
                    return std::nullopt;
                return body.subspan(static_cast<std::size_t>(offset - body_offset), size);
            };
            if (!read_sample_encryption(stbl->body, track, {}, count, aux_at, encryptions))
                return false;
            for (std::size_t i = 0; i < count; ++i)
                samples[i].encryption = std::move(encryptions[i]);
        }
        m_samples.insert(m_samples.end(), samples.begin(), samples.end());
    } else if (track.protection) {
        if (!read_seig_descriptions(stbl->body, track.seig))
            return false;
    }
    m_init.tracks.push_back(std::move(track));
    return true;
}

bool Mp4Parser::parse_moof(std::span<std::uint8_t const> body, std::uint64_t moof_offset, std::size_t header_size)
{
    if (m_init.tracks.empty())
        return false; // no initialization segment: nothing to read the runs by
    if (m_fragment_decode_time.size() != m_init.tracks.size())
        m_fragment_decode_time.assign(m_init.tracks.size(), 0);
    std::uint64_t previous_traf_end = moof_offset; // for a run with no base of its own
    bool first_traf = true;
    std::uint64_t const body_offset = moof_offset + header_size;
    std::vector<std::vector<std::uint8_t>> pssh;
    bool const read = for_each_child(body, [&](Box const& box) {
        if (box.type == "pssh") {
            std::span<std::uint8_t const> const whole = whole_box(box);
            pssh.emplace_back(whole.begin(), whole.end());
            return true;
        }
        if (box.type != "traf")
            return true;
        std::optional<Box> const tfhd = find_child(box.body, "tfhd");
        if (!tfhd)
            return false;
        int version = 0;
        std::uint32_t flags = 0;
        std::span<std::uint8_t const> d;
        if (!full_box(tfhd->body, version, flags, d) || d.size() < 4)
            return false;
        std::uint32_t const track_id = be32(d.data());
        std::size_t track_index = m_init.tracks.size();
        for (std::size_t i = 0; i < m_init.tracks.size(); ++i) {
            if (m_init.tracks[i].id == track_id)
                track_index = i;
        }
        if (track_index == m_init.tracks.size())
            return true; // a track the moov does not have: passed over
        Mp4Track const& track = m_init.tracks[track_index];
        std::size_t at = 4;
        std::uint64_t base = moof_offset;
        bool base_given = false;
        if (flags & 0x1) {
            if (at + 8 > d.size())
                return false;
            base = be64(d.data() + at);
            base_given = true;
            at += 8;
        }
        if (flags & 0x2)
            at += 4; // sample description index
        std::uint32_t default_duration = track.default_sample_duration;
        std::uint32_t default_size = track.default_sample_size;
        std::uint32_t default_flags = track.default_sample_flags;
        if (flags & 0x8) {
            if (at + 4 > d.size())
                return false;
            default_duration = be32(d.data() + at);
            at += 4;
        }
        if (flags & 0x10) {
            if (at + 4 > d.size())
                return false;
            default_size = be32(d.data() + at);
            at += 4;
        }
        if (flags & 0x20) {
            if (at + 4 > d.size())
                return false;
            default_flags = be32(d.data() + at);
            at += 4;
        }
        bool const base_is_moof = flags & 0x20000;
        if (!base_given && !base_is_moof && !first_traf)
            base = previous_traf_end;
        first_traf = false;
        std::int64_t decode_time = m_fragment_decode_time[track_index];
        if (std::optional<Box> const tfdt = find_child(box.body, "tfdt")) {
            int v = 0;
            std::uint32_t f = 0;
            std::span<std::uint8_t const> t;
            if (!full_box(tfdt->body, v, f, t))
                return false;
            if (v == 1 && t.size() >= 8)
                decode_time = static_cast<std::int64_t>(be64(t.data()));
            else if (v == 0 && t.size() >= 4)
                decode_time = be32(t.data());
        }
        std::uint64_t data_at = base;
        std::size_t const first_sample = m_samples.size();
        bool ok = for_each_child(box.body, [&](Box const& run) {
            if (run.type != "trun")
                return true;
            int v = 0;
            std::uint32_t f = 0;
            std::span<std::uint8_t const> t;
            if (!full_box(run.body, v, f, t) || t.size() < 4)
                return false;
            std::uint32_t const count = be32(t.data());
            if (count > k_most_samples || m_samples.size() + count > k_most_pending_fragment_samples)
                return false;
            std::size_t p = 4;
            std::uint64_t run_at = data_at;
            if (f & 0x1) {
                if (p + 4 > t.size())
                    return false;
                run_at = base + static_cast<std::int64_t>(static_cast<std::int32_t>(be32(t.data() + p)));
                p += 4;
            }
            std::uint32_t first_flags = default_flags;
            if (f & 0x4) {
                if (p + 4 > t.size())
                    return false;
                first_flags = be32(t.data() + p);
                p += 4;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                Sample sample;
                sample.track = track_id;
                sample.offset = run_at;
                sample.duration = default_duration;
                sample.size = default_size;
                std::uint32_t sample_flags = i == 0 ? first_flags : default_flags;
                if (f & 0x100) {
                    if (p + 4 > t.size())
                        return false;
                    sample.duration = be32(t.data() + p);
                    p += 4;
                }
                if (f & 0x200) {
                    if (p + 4 > t.size())
                        return false;
                    sample.size = be32(t.data() + p);
                    p += 4;
                }
                if (f & 0x400) {
                    if (p + 4 > t.size())
                        return false;
                    sample_flags = be32(t.data() + p);
                    p += 4;
                }
                if (f & 0x800) {
                    if (p + 4 > t.size())
                        return false;
                    sample.composition_offset = static_cast<std::int32_t>(be32(t.data() + p));
                    p += 4;
                }
                sample.key = !(sample_flags & 0x10000);
                sample.decode_time = decode_time;
                decode_time += sample.duration;
                run_at += sample.size;
                m_samples.push_back(sample);
            }
            data_at = run_at;
            return true;
        });
        if (!ok)
            return false;
        // A protected track's samples, each with how it is encrypted: the
        // fragment's 'senc', or what its 'saiz' and 'saio' point at — by
        // offsets from the same base the runs' data offsets count from
        // (23001-7 §7.1), here in this moof.
        if (track.protection) {
            std::vector<Mp4SeigEntry> own_seig;
            if (!read_seig_descriptions(box.body, own_seig))
                return false;
            auto const aux_at = [&](std::uint64_t offset, std::size_t size) -> std::optional<std::span<std::uint8_t const>> {
                std::uint64_t const where = base + offset;
                if (where < body_offset || where - body_offset > body.size() || size > body.size() - (where - body_offset))
                    return std::nullopt;
                return body.subspan(static_cast<std::size_t>(where - body_offset), size);
            };
            std::size_t const count = m_samples.size() - first_sample;
            std::vector<std::optional<SampleEncryption>> encryptions;
            if (!read_sample_encryption(box.body, track, own_seig, count, aux_at, encryptions))
                return false;
            for (std::size_t i = 0; i < count; ++i)
                m_samples[first_sample + i].encryption = std::move(encryptions[i]);
        }
        m_fragment_decode_time[track_index] = decode_time;
        previous_traf_end = data_at;
        return true;
    });
    if (read && !pssh.empty())
        m_fragment_pssh.push_back(std::move(pssh));
    return read;
}

void Mp4Parser::emit_ready()
{
    if (m_samples.empty())
        return;
    // Each known sample whose bytes have arrived, in the order they were
    // named; an mdat is dropped once every sample it holds is out.
    std::vector<Sample> kept;
    for (Sample const& s : m_samples) {
        Mdat const* holder = nullptr;
        for (Mdat const& m : m_mdats) {
            if (s.offset >= m.offset && s.offset + s.size <= m.offset + m.bytes.size()) {
                holder = &m;
                break;
            }
        }
        if (!holder) {
            kept.push_back(s);
            continue;
        }
        Mp4Track const* const track = m_init.track(s.track);
        if (!track) {
            continue; // a sample of no track: dropped
        }
        Mp4Frame frame;
        frame.track = s.track;
        frame.key = s.key;
        // The edit list's start moves both times, as ffmpeg's and Chrome's
        // readers move them: the first frame's decode time comes out negative
        // when its composition offset is what the edit takes off.
        frame.decode_time_ns = to_ns(s.decode_time - track->edit_media_time, track->timescale);
        frame.time_ns = to_ns(s.decode_time + s.composition_offset - track->edit_media_time, track->timescale);
        frame.duration_ns = static_cast<std::uint64_t>(to_ns(s.duration, track->timescale));
        std::size_t const start = static_cast<std::size_t>(s.offset - holder->offset);
        frame.data.assign(holder->bytes.begin() + static_cast<std::ptrdiff_t>(start),
            holder->bytes.begin() + static_cast<std::ptrdiff_t>(start + s.size));
        frame.encryption = s.encryption;
        m_frames.push_back(std::move(frame));
    }
    m_samples = std::move(kept);
    // mdats fully given out, or whose offsets no sample can name any more
    // (every pending sample lies past them), are let go — but for a plain
    // file whose tables may still come: those are kept until the moov.
    if (m_init_segments > 0 && !m_mdats.empty()) {
        std::uint64_t lowest_pending = std::uint64_t(-1);
        for (Sample const& s : m_samples)
            lowest_pending = std::min(lowest_pending, s.offset);
        std::erase_if(m_mdats, [&](Mdat const& m) {
            bool const complete = !m_mdat_left || &m != &m_mdats.back();
            return complete && (m_init.fragmented || m_samples.empty() || m.offset + m.bytes.size() <= lowest_pending);
        });
    }
}

std::vector<Mp4Frame> Mp4Parser::take_frames()
{
    std::vector<Mp4Frame> out = std::move(m_frames);
    m_frames.clear();
    return out;
}

std::vector<std::vector<std::vector<std::uint8_t>>> Mp4Parser::take_fragment_pssh()
{
    std::vector<std::vector<std::vector<std::uint8_t>>> out = std::move(m_fragment_pssh);
    m_fragment_pssh.clear();
    return out;
}

bool Mp4Parser::at_segment_boundary() const
{
    return m_pending.empty() && !m_mdat_left && m_skip == 0;
}

void Mp4Parser::reset()
{
    m_pending.clear();
    m_skip = 0;
    m_mdat_left.reset();
    m_mdats.clear();
    m_samples.clear();
    m_frames.clear();
    m_stream_offset = 0;
    m_failed = false;
}

}
