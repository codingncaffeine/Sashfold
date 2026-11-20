#include "media/H264Decoder.h"

#include <algorithm>
#include <optional>

namespace sashfold::media {

namespace {

// The pic_parameter_set_id of a slice: the third ue(v) of its header.
std::optional<int> slice_pps_id(std::span<std::uint8_t const> nal)
{
    std::vector<std::uint8_t> const rbsp = h264_rbsp(nal.subspan(0, std::min<std::size_t>(nal.size(), 16)));
    std::size_t at = 0;
    auto bit = [&]() -> int {
        if (at >= rbsp.size() * 8)
            return -1;
        int const b = (rbsp[at >> 3] >> (7 - (at & 7))) & 1;
        ++at;
        return b;
    };
    std::uint32_t value = 0;
    for (int i = 0; i < 3; ++i) {
        int zeros = 0;
        int b;
        while ((b = bit()) == 0) {
            if (++zeros > 31)
                return std::nullopt;
        }
        if (b < 0)
            return std::nullopt;
        value = 0;
        for (int k = 0; k < zeros; ++k) {
            int const v = bit();
            if (v < 0)
                return std::nullopt;
            value = (value << 1) | static_cast<std::uint32_t>(v);
        }
        value += (1u << zeros) - 1;
    }
    if (value > 255)
        return std::nullopt;
    return static_cast<int>(value);
}

// 7.4.1.2.4: whether slice `s` begins a picture other than `prev`'s.
bool begins_new_picture(H264SliceHeader const& prev, H264SliceHeader const& s, H264Sps const& sps)
{
    if (s.frame_num != prev.frame_num || s.pps_id != prev.pps_id || s.field_pic != prev.field_pic || s.bottom_field != prev.bottom_field)
        return true;
    if ((s.nal_ref_idc == 0) != (prev.nal_ref_idc == 0))
        return true;
    if (sps.pic_order_cnt_type == 0 && (s.pic_order_cnt_lsb != prev.pic_order_cnt_lsb || s.delta_pic_order_cnt_bottom != prev.delta_pic_order_cnt_bottom))
        return true;
    if (sps.pic_order_cnt_type == 1 && s.delta_pic_order_cnt != prev.delta_pic_order_cnt)
        return true;
    if (s.idr != prev.idr)
        return true;
    return s.idr && prev.idr && s.idr_pic_id != prev.idr_pic_id;
}

}

H264Decoder::H264Decoder(std::unique_ptr<H264Accelerator> accelerator)
    : m_accelerator(std::move(accelerator))
{
}

bool H264Decoder::set_configuration(std::span<std::uint8_t const> avcc)
{
    std::optional<H264DecoderConfiguration> const config = parse_avcc(avcc);
    if (!config)
        return false;
    m_length_size = config->length_size;
    for (std::vector<std::uint8_t> const& nal : config->sps) {
        if (std::optional<H264Sps> sps = parse_h264_sps(nal))
            m_sps[sps->id] = std::move(*sps);
    }
    for (std::vector<std::uint8_t> const& nal : config->pps) {
        // The PPS reads its scaling lists against its SPS when that is known.
        std::optional<H264Pps> pps = parse_h264_pps(nal, nullptr);
        if (!pps)
            continue;
        auto const sps = m_sps.find(pps->sps_id);
        if (sps != m_sps.end())
            pps = parse_h264_pps(nal, &sps->second);
        if (pps)
            m_pps[pps->id] = std::move(*pps);
    }
    m_sets_changed = true;
    return !m_sps.empty() && !m_pps.empty();
}

void H264Decoder::reset()
{
    m_dpb.clear();
    m_poc.reset();
    m_prev_ref_frame_num = 0;
    m_max_long_term_frame_idx = -1;
    m_started = false;
    m_first_field = FirstField {};
    m_accelerator->reset();
}

bool H264Decoder::take_parameter_sets(std::vector<H264Nal> const& nals)
{
    bool changed = false;
    for (H264Nal const& nal : nals) {
        if (nal.type == 7) {
            if (std::optional<H264Sps> sps = parse_h264_sps(nal.bytes)) {
                m_sps[sps->id] = std::move(*sps);
                changed = true;
            }
        }
    }
    for (H264Nal const& nal : nals) {
        if (nal.type == 8) {
            std::optional<H264Pps> pps = parse_h264_pps(nal.bytes, nullptr);
            if (!pps)
                continue;
            auto const sps = m_sps.find(pps->sps_id);
            if (sps != m_sps.end())
                pps = parse_h264_pps(nal.bytes, &sps->second);
            if (pps) {
                m_pps[pps->id] = std::move(*pps);
                changed = true;
            }
        }
    }
    if (changed)
        m_sets_changed = true;
    return changed;
}

int H264Decoder::free_slot() const
{
    for (int slot = 0; slot < m_accelerator->slots(); ++slot) {
        bool const held = std::any_of(m_dpb.begin(), m_dpb.end(), [slot](Entry const& e) { return e.slot == slot; })
            || (m_first_field.active && m_first_field.slot == slot);
        if (!held)
            return slot;
    }
    return -1;
}

void H264Decoder::drop_unreferenced()
{
    std::erase_if(m_dpb, [](Entry const& e) { return !e.referenced(); });
}

// 8.2.5.3: when the buffer is full of references, the short-term frame
// (or field pair, or lone field) with the smallest FrameNumWrap is let go.
void H264Decoder::sliding_window(H264Sps const& sps, int)
{
    int const most = std::max(1, sps.max_num_ref_frames);
    int count = 0;
    for (Entry const& e : m_dpb)
        count += (e.has(1) ? 1 : 0) + (e.has(2) ? 1 : 0);
    if (count < most)
        return;
    auto oldest = m_dpb.end();
    for (auto it = m_dpb.begin(); it != m_dpb.end(); ++it) {
        if (!it->has(1))
            continue;
        if (oldest == m_dpb.end() || it->frame_num_wrap < oldest->frame_num_wrap)
            oldest = it;
    }
    if (oldest != m_dpb.end()) {
        for (std::uint8_t& m : oldest->marking) {
            if (m == 1)
                m = 0;
        }
    }
    drop_unreferenced();
}

// 8.2.5.4: the memory management control operations of the current
// picture, applied to frames when it is a frame and to single fields when
// it is a field.
bool H264Decoder::apply_mmco(H264SliceHeader const& slice, H264PocState::Poc& poc, int current_slot, bool& current_long_term,
    int& current_long_term_idx)
{
    bool const field = slice.field_pic;
    int const parity = slice.bottom_field ? 1 : 0;
    int const curr_pic_num = field ? 2 * slice.frame_num + 1 : slice.frame_num;
    // 8.2.4.1: a field's numbers count the fields of its parity odd.
    auto pic_num = [&](Entry const& e, int f) { return field ? 2 * e.frame_num_wrap + (f == parity ? 1 : 0) : e.frame_num_wrap; };
    auto long_term_pic_num = [&](Entry const& e, int f) { return field ? 2 * e.long_term_frame_idx + (f == parity ? 1 : 0) : e.long_term_frame_idx; };
    // The long-term pictures holding LongTermFrameIdx `idx` let go, but
    // for the frame `keep` (the other field of the one being marked).
    auto free_long_term_idx = [&](int idx, Entry const* keep) {
        for (Entry& e : m_dpb) {
            if (&e == keep || e.long_term_frame_idx != idx)
                continue;
            for (std::uint8_t& m : e.marking) {
                if (m == 2)
                    m = 0;
            }
        }
    };
    for (H264Mmco const& m : slice.mmco) {
        switch (m.op) {
        case 1: { // a short-term picture let go, by its picture number
            int const pic_num_x = curr_pic_num - m.difference_of_pic_nums;
            for (Entry& e : m_dpb) {
                if (!field) {
                    if (e.marking[0] == 1 && e.marking[1] == 1 && e.frame_num_wrap == pic_num_x)
                        e.marking = { 0, 0 };
                    continue;
                }
                for (int f = 0; f < 2; ++f) {
                    if (e.marking[static_cast<std::size_t>(f)] == 1 && pic_num(e, f) == pic_num_x)
                        e.marking[static_cast<std::size_t>(f)] = 0;
                }
            }
            break;
        }
        case 2: // a long-term picture let go
            for (Entry& e : m_dpb) {
                if (!field) {
                    if (e.marking[0] == 2 && e.marking[1] == 2 && e.long_term_frame_idx == m.long_term_pic_num)
                        e.marking = { 0, 0 };
                    continue;
                }
                for (int f = 0; f < 2; ++f) {
                    if (e.marking[static_cast<std::size_t>(f)] == 2 && long_term_pic_num(e, f) == m.long_term_pic_num)
                        e.marking[static_cast<std::size_t>(f)] = 0;
                }
            }
            break;
        case 3: { // a short-term picture becomes long-term
            int const pic_num_x = curr_pic_num - m.difference_of_pic_nums;
            Entry* target = nullptr;
            int target_field = -1;
            for (Entry& e : m_dpb) {
                if (!field) {
                    if (e.marking[0] == 1 && e.marking[1] == 1 && e.frame_num_wrap == pic_num_x)
                        target = &e;
                    continue;
                }
                for (int f = 0; f < 2; ++f) {
                    if (e.marking[static_cast<std::size_t>(f)] == 1 && pic_num(e, f) == pic_num_x) {
                        target = &e;
                        target_field = f;
                    }
                }
            }
            free_long_term_idx(m.long_term_frame_idx, field ? target : nullptr);
            if (target) {
                if (field)
                    target->marking[static_cast<std::size_t>(target_field)] = 2;
                else
                    target->marking = { 2, 2 };
                target->long_term_frame_idx = m.long_term_frame_idx;
            }
            break;
        }
        case 4: // the most long-term indices there may be
            m_max_long_term_frame_idx = m.max_long_term_frame_idx_plus1 - 1;
            for (Entry& e : m_dpb) {
                for (std::uint8_t& mark : e.marking) {
                    if (mark == 2 && e.long_term_frame_idx > m_max_long_term_frame_idx)
                        mark = 0;
                }
            }
            break;
        case 5: { // everything let go; the picture counts start over at this one
            m_dpb.clear();
            m_max_long_term_frame_idx = -1;
            if (field) {
                poc.top = 0;
                poc.bottom = 0;
            } else {
                int const temp = std::min(poc.top, poc.bottom);
                poc.top -= temp;
                poc.bottom -= temp;
            }
            break;
        }
        case 6: { // the current picture as a long-term reference
            Entry const* same_frame = nullptr;
            for (Entry const& e : m_dpb) {
                if (field && e.slot == current_slot)
                    same_frame = &e;
            }
            free_long_term_idx(m.long_term_frame_idx, same_frame);
            current_long_term = true;
            current_long_term_idx = m.long_term_frame_idx;
            break;
        }
        default:
            return false;
        }
    }
    drop_unreferenced();
    return true;
}

// 8.2.5.2: frames the stream left out are stood in for by references
// that were never decoded, so the window slides as the encoder's did.
void H264Decoder::fill_frame_num_gap(H264SliceHeader const& slice, H264Sps const& sps)
{
    int const max_frame_num = 1 << sps.log2_max_frame_num;
    int expected = (m_prev_ref_frame_num + 1) % max_frame_num;
    int guard = 0;
    while (expected != slice.frame_num && guard++ < 64) {
        for (Entry& e : m_dpb)
            e.frame_num_wrap = e.frame_num > expected ? e.frame_num - max_frame_num : e.frame_num;
        sliding_window(sps, -1);
        Entry gap;
        gap.slot = -1;
        gap.frame_num = expected;
        gap.frame_num_wrap = expected;
        gap.marking = { 1, 1 };
        m_dpb.push_back(gap);
        m_counts.gaps++;
        m_prev_ref_frame_num = expected;
        expected = (expected + 1) % max_frame_num;
    }
}

std::optional<Nv12Picture> H264Decoder::decode(std::span<std::uint8_t const> access_unit)
{
    std::vector<H264Nal> const nals = m_length_size > 0 ? split_h264_length_prefixed(access_unit, m_length_size) : split_h264_annex_b(access_unit);
    // The unit's slices, cut into pictures where 7.4.1.2.4 says one begins
    // (a sample may hold both fields of a frame), each read with its sets:
    // a parameter set ends the picture before it (7.4.1.2.3) and is in
    // force from there on, so one sent again between two fields with new
    // contents does not reach back to the first.
    std::optional<Nv12Picture> out;
    Picture unit;
    for (H264Nal const& nal : nals) {
        if (nal.type == 7 || nal.type == 8) {
            if (!unit.headers.empty()) {
                if (std::optional<Nv12Picture> picture = decode_picture(unit))
                    out = std::move(picture);
                unit = Picture {};
            }
            take_parameter_sets(std::vector<H264Nal> { nal });
            continue;
        }
        if (nal.type != 1 && nal.type != 5)
            continue;
        std::optional<int> const pps_id = slice_pps_id(nal.bytes);
        auto const pps = pps_id ? m_pps.find(*pps_id) : m_pps.end();
        auto const sps = pps != m_pps.end() ? m_sps.find(pps->second.sps_id) : m_sps.end();
        if (sps == m_sps.end()) {
            m_counts.failed++;
            return out;
        }
        std::optional<H264SliceHeader> const header = parse_h264_slice_header(nal.bytes, sps->second, pps->second);
        if (!header) {
            m_counts.failed++;
            return out;
        }
        if (!unit.headers.empty() && begins_new_picture(unit.headers.back(), *header, *unit.sps)) {
            if (std::optional<Nv12Picture> picture = decode_picture(unit))
                out = std::move(picture);
            unit = Picture {};
        }
        if (unit.headers.empty()) {
            unit.sps = &sps->second;
            unit.pps = &pps->second;
        }
        unit.slices.push_back(nal.bytes);
        unit.headers.push_back(*header);
    }
    if (!unit.headers.empty()) {
        if (std::optional<Nv12Picture> picture = decode_picture(unit))
            out = std::move(picture);
    }
    return out;
}

std::optional<Nv12Picture> H264Decoder::decode_picture(Picture const& unit)
{
    H264SliceHeader const& first = unit.headers.front();
    H264Sps const* const sps = unit.sps;
    H264Pps const* const pps = unit.pps;
    H264PictureInfo picture;
    picture.first_slice = &first;
    picture.slices = unit.slices;
    picture.headers = unit.headers;
    for (H264SliceHeader const& header : unit.headers) {
        if (header.slice_type != H264SliceType::I && header.slice_type != H264SliceType::SI)
            picture.intra = false;
    }
    // Beyond 4:2:0 at 8 bits is not written; fields and MBAFF frames are
    // the processor's decoder's alone (the hardware paths are handed
    // frames only, so such a stream falls back to it).
    if (sps->chroma_format_idc != 1 || sps->bit_depth_luma != 8 || (!sps->frame_mbs_only && !m_accelerator->is_software())) {
        m_counts.failed++;
        return std::nullopt;
    }
    bool const field = first.field_pic;
    // 7.4.3 (the second field of a pair): the field after a first field of
    // the other parity, with its frame_num, a reference if that was one.
    bool const second = field && m_first_field.active && m_first_field.bottom != first.bottom_field && m_first_field.frame_num == first.frame_num
        && m_first_field.reference == first.is_reference() && (!first.idr || m_first_field.idr);
    if (!second)
        m_first_field.active = false; // a first field left without its second stays as it is
    if (!m_started && !first.idr) {
        m_counts.failed++; // a stream must begin at an IDR
        return std::nullopt;
    }
    // A picture the driver is known to predict wrongly is not given to
    // it; the caller takes the stream to another decoder.
    if (m_accelerator->mishandles_long_term_and_wrap() && !first.idr) {
        bool const long_term = std::any_of(m_dpb.begin(), m_dpb.end(), [](Entry const& e) { return e.has(2); })
            || std::any_of(first.mmco.begin(), first.mmco.end(), [](H264Mmco const& m) { return m.op == 3 || m.op == 6; });
        bool const wraps = sps->max_num_ref_frames > 1 && first.frame_num < m_prev_ref_frame_num;
        if (long_term || wraps) {
            m_counts.declined++;
            return std::nullopt;
        }
    }
    if (!second) {
        if (first.idr) {
            m_dpb.clear();
            m_max_long_term_frame_idx = first.long_term_reference ? 0 : -1;
            m_started = true;
        } else if (first.frame_num != m_prev_ref_frame_num && first.frame_num != (m_prev_ref_frame_num + 1) % (1 << sps->log2_max_frame_num)) {
            fill_frame_num_gap(first, *sps);
        }
    }
    if (m_sets_changed) {
        std::vector<H264Sps> all_sps;
        std::vector<H264Pps> all_pps;
        for (auto const& [id, s] : m_sps)
            all_sps.push_back(s);
        for (auto const& [id, p] : m_pps)
            all_pps.push_back(p);
        if (!m_accelerator->set_parameter_sets(all_sps, all_pps)) {
            m_counts.failed++;
            return std::nullopt;
        }
        m_sets_changed = false;
    }
    // 8.2.4.1: picture numbers for this picture's references.
    int const max_frame_num = 1 << sps->log2_max_frame_num;
    for (Entry& e : m_dpb)
        e.frame_num_wrap = e.frame_num > first.frame_num ? e.frame_num - max_frame_num : e.frame_num;
    H264PocState::Poc poc = m_poc.next(first, *sps);

    int const slot = second ? m_first_field.slot : free_slot();
    if (slot < 0) {
        m_counts.failed++;
        return std::nullopt;
    }
    picture.sps = sps;
    picture.pps = pps;
    picture.poc_top = poc.top;
    picture.poc_bottom = poc.bottom;
    picture.frame_num = first.frame_num;
    picture.field_pic = field;
    picture.bottom_field = first.bottom_field;
    picture.second_field = second;
    picture.idr = first.idr;
    picture.is_reference = first.is_reference();
    picture.slot = slot;
    for (Entry const& e : m_dpb) {
        if (e.slot < 0)
            continue; // a skipped frame: nothing to read
        H264Reference r;
        r.slot = e.slot;
        r.long_term = e.marking[0] == 2 && e.marking[1] == 2;
        r.frame_num = r.long_term ? e.long_term_frame_idx : e.frame_num;
        r.poc_top = e.poc_top;
        r.poc_bottom = e.poc_bottom;
        r.marking = e.marking;
        r.short_frame_num = e.frame_num;
        r.long_term_frame_idx = e.long_term_frame_idx;
        r.fields = e.fields;
        picture.references.push_back(r);
    }
    // The marking that follows (8.2.5.1), decided before the decode so the
    // accelerator knows whether the picture is kept.
    bool current_long_term = false;
    int current_long_term_idx = 0;
    if (first.idr && first.long_term_reference) {
        current_long_term = true;
        current_long_term_idx = 0;
    }
    picture.long_term = current_long_term;
    picture.long_term_frame_idx = current_long_term_idx;

    if (!m_accelerator->decode(picture)) {
        m_counts.failed++;
        if (field && second)
            m_first_field.active = false;
        return std::nullopt;
    }

    int const parity = first.bottom_field ? 1 : 0;
    if (first.is_reference()) {
        if (!first.idr) {
            if (first.adaptive_ref_pic_marking) {
                if (!apply_mmco(first, poc, slot, current_long_term, current_long_term_idx))
                    m_dpb.clear();
            } else {
                // 8.2.5.3: the second field of a pair whose first is a
                // short-term reference joins it; anything else may push
                // the oldest out.
                bool joins = false;
                for (Entry const& e : m_dpb)
                    joins = joins || (second && e.slot == slot && e.marking[static_cast<std::size_t>(1 - parity)] == 1);
                if (!joins)
                    sliding_window(*sps, slot);
            }
        }
        Entry* entry = nullptr;
        if (second) {
            for (Entry& e : m_dpb) {
                if (e.slot == slot)
                    entry = &e;
            }
        }
        if (!entry) {
            // The buffer holds at most the sequence's references; one more
            // than that, from a stream that marks badly, loses its oldest.
            while (static_cast<int>(m_dpb.size()) >= std::max(1, std::min(16, sps->max_num_ref_frames)))
                m_dpb.erase(m_dpb.begin());
            entry = &m_dpb.emplace_back();
            entry->slot = slot;
            entry->fields = 0;
        }
        std::uint8_t const mark = current_long_term ? 2 : 1;
        if (field)
            entry->marking[static_cast<std::size_t>(parity)] = mark;
        else
            entry->marking = { mark, mark };
        if (current_long_term)
            entry->long_term_frame_idx = current_long_term_idx;
        entry->frame_num = first.has_mmco5() ? 0 : first.frame_num;
        entry->frame_num_wrap = entry->frame_num;
        if (!field || parity == 0)
            entry->poc_top = poc.top;
        if (!field || parity == 1)
            entry->poc_bottom = poc.bottom;
        entry->fields = static_cast<std::uint8_t>(entry->fields | (field ? 1 << parity : 3));
        m_prev_ref_frame_num = first.has_mmco5() ? 0 : first.frame_num;
    }
    if (field && !second) {
        m_first_field.active = true;
        m_first_field.slot = slot;
        m_first_field.frame_num = first.frame_num;
        m_first_field.bottom = first.bottom_field;
        m_first_field.reference = first.is_reference();
        m_first_field.idr = first.idr;
        return std::nullopt;
    }
    m_first_field.active = false;
    std::optional<Nv12Picture> out = m_accelerator->read(slot);
    m_counts.decoded++;
    return out;
}

std::vector<H264ListPicture> h264_list_pictures(H264PictureInfo const& picture, int max_frame_num)
{
    std::vector<H264ListPicture> out;
    for (H264Reference const& r : picture.references) {
        H264ListPicture e;
        e.slot = r.slot;
        e.long_term = r.long_term;
        e.frame_num = r.frame_num;
        bool const marked = r.marking[0] != 0 || r.marking[1] != 0;
        e.marking = marked ? r.marking : (r.long_term ? std::array<std::uint8_t, 2> { 2, 2 } : std::array<std::uint8_t, 2> { 1, 1 });
        int const short_num = marked ? r.short_frame_num : r.frame_num;
        e.long_term_frame_idx = marked ? r.long_term_frame_idx : r.frame_num;
        e.frame_num_wrap = short_num > picture.frame_num ? short_num - max_frame_num : short_num;
        e.pic_num = r.long_term ? e.long_term_frame_idx : e.frame_num_wrap;
        e.poc_top = r.poc_top;
        e.poc_bottom = r.poc_bottom;
        e.fields = r.fields;
        // 8.2.1: a frame's (or pair's) count is its fields' least; a lone field's its own.
        e.poc = r.fields == 1 ? r.poc_top : (r.fields == 2 ? r.poc_bottom : std::min(r.poc_top, r.poc_bottom));
        out.push_back(e);
    }
    return out;
}

namespace {

// 8.2.4.2.5: fields taken from an ordered list of frames, alternating
// parity from the current field's, each field marked `mark` (1 short-term,
// 2 long-term); when one parity runs out, the rest of the other in order.
void alternate_fields(std::vector<H264ListPicture const*> const& frames, std::uint8_t mark, int parity, std::vector<H264ListPicture>& out)
{
    std::size_t next[2] = { 0, 0 }; // per parity: where its search stands
    auto take = [&](int f) -> H264ListPicture const* {
        std::size_t& i = next[f];
        while (i < frames.size() && frames[i]->marking[static_cast<std::size_t>(f)] != mark)
            ++i;
        return i < frames.size() ? frames[i++] : nullptr;
    };
    auto field_of = [&](H264ListPicture const& frame, int f) {
        H264ListPicture e = frame;
        e.structure = 1 + f;
        e.long_term = mark == 2;
        e.poc = f == 0 ? frame.poc_top : frame.poc_bottom;
        // 8.2.4.1, (8-30) to (8-33).
        e.pic_num = 2 * (e.long_term ? frame.long_term_frame_idx : frame.frame_num_wrap) + (f == parity ? 1 : 0);
        return e;
    };
    int want = parity;
    for (;;) {
        H264ListPicture const* const frame = take(want);
        if (!frame) {
            while (H264ListPicture const* const rest = take(1 - want))
                out.push_back(field_of(*rest, 1 - want));
            return;
        }
        out.push_back(field_of(*frame, want));
        want = 1 - want;
    }
}

bool same_picture(H264ListPicture const& a, H264ListPicture const& b) { return a.slot == b.slot && a.structure == b.structure; }

}

std::vector<H264ListPicture> h264_reference_list(std::vector<H264ListPicture> const& references, H264SliceHeader const& header,
    std::size_t list, bool is_b, int current_poc, int max_frame_num, int current_frame_num)
{
    bool const field = header.field_pic;
    int const parity = header.bottom_field ? 1 : 0;
    std::vector<H264ListPicture> out;
    if (!field) {
        // Frames: a frame or pair whose fields are both short-term, or both long-term.
        std::vector<H264ListPicture> short_term;
        std::vector<H264ListPicture> long_term;
        for (H264ListPicture const& e : references) {
            if (e.marking[0] == 1 && e.marking[1] == 1)
                short_term.push_back(e);
            else if (e.marking[0] == 2 && e.marking[1] == 2)
                long_term.push_back(e);
        }
        for (H264ListPicture& e : long_term) {
            e.long_term = true;
            e.pic_num = e.long_term_frame_idx;
        }
        std::sort(long_term.begin(), long_term.end(), [](H264ListPicture const& a, H264ListPicture const& b) { return a.pic_num < b.pic_num; });
        if (!is_b) {
            std::sort(short_term.begin(), short_term.end(), [](H264ListPicture const& a, H264ListPicture const& b) { return a.pic_num > b.pic_num; });
            out = short_term;
            out.insert(out.end(), long_term.begin(), long_term.end());
        } else {
            std::vector<H264ListPicture> before;
            std::vector<H264ListPicture> after;
            for (H264ListPicture const& e : short_term)
                (e.poc <= current_poc ? before : after).push_back(e);
            std::sort(before.begin(), before.end(), [](H264ListPicture const& a, H264ListPicture const& b) { return a.poc > b.poc; });
            std::sort(after.begin(), after.end(), [](H264ListPicture const& a, H264ListPicture const& b) { return a.poc < b.poc; });
            if (list == 0) {
                out = before;
                out.insert(out.end(), after.begin(), after.end());
            } else {
                out = after;
                out.insert(out.end(), before.begin(), before.end());
            }
            out.insert(out.end(), long_term.begin(), long_term.end());
            if (list == 1 && out.size() > 1) {
                // 8.2.4.2.3: a list 1 that equals list 0 swaps its first two.
                std::vector<H264ListPicture> l0 = before;
                l0.insert(l0.end(), after.begin(), after.end());
                l0.insert(l0.end(), long_term.begin(), long_term.end());
                bool same = l0.size() == out.size();
                for (std::size_t i = 0; same && i < out.size(); ++i)
                    same = l0[i].slot == out[i].slot;
                if (same)
                    std::swap(out[0], out[1]);
            }
        }
    } else {
        // Fields (8.2.4.2.2, 8.2.4.2.4): the frames holding a short-term
        // field, and those holding a long-term one, ordered, then their
        // fields alternated (8.2.4.2.5).
        std::vector<H264ListPicture const*> short_frames;
        std::vector<H264ListPicture const*> long_frames;
        for (H264ListPicture const& e : references) {
            if (e.marking[0] == 1 || e.marking[1] == 1)
                short_frames.push_back(&e);
            if (e.marking[0] == 2 || e.marking[1] == 2)
                long_frames.push_back(&e);
        }
        std::sort(long_frames.begin(), long_frames.end(), [](H264ListPicture const* a, H264ListPicture const* b) { return a->long_term_frame_idx < b->long_term_frame_idx; });
        auto build = [&](std::size_t which) {
            std::vector<H264ListPicture const*> frames = short_frames;
            if (!is_b) {
                std::stable_sort(frames.begin(), frames.end(), [](H264ListPicture const* a, H264ListPicture const* b) { return a->frame_num_wrap > b->frame_num_wrap; });
            } else {
                std::vector<H264ListPicture const*> before;
                std::vector<H264ListPicture const*> after;
                for (H264ListPicture const* e : frames)
                    (e->poc <= current_poc ? before : after).push_back(e);
                std::stable_sort(before.begin(), before.end(), [](H264ListPicture const* a, H264ListPicture const* b) { return a->poc > b->poc; });
                std::stable_sort(after.begin(), after.end(), [](H264ListPicture const* a, H264ListPicture const* b) { return a->poc < b->poc; });
                frames = which == 0 ? before : after;
                std::vector<H264ListPicture const*> const& rest = which == 0 ? after : before;
                frames.insert(frames.end(), rest.begin(), rest.end());
            }
            std::vector<H264ListPicture> fields;
            alternate_fields(frames, 1, parity, fields);
            alternate_fields(long_frames, 2, parity, fields);
            return fields;
        };
        out = build(list);
        if (is_b && list == 1 && out.size() > 1) {
            std::vector<H264ListPicture> const l0 = build(0);
            bool same = l0.size() == out.size();
            for (std::size_t i = 0; same && i < out.size(); ++i)
                same = same_picture(l0[i], out[i]);
            if (same)
                std::swap(out[0], out[1]);
        }
    }
    int const active = list == 0 ? header.num_ref_idx_l0_active : header.num_ref_idx_l1_active;
    // 8.2.4.3: each modification puts a named picture at the next index,
    // the rest sliding along (a picture named twice is kept once).
    int const max_pic_num = field ? 2 * max_frame_num : max_frame_num;
    int const curr_pic_num = field ? 2 * current_frame_num + 1 : current_frame_num;
    int pic_num_pred = curr_pic_num;
    std::size_t ref_idx = 0;
    if (out.size() < static_cast<std::size_t>(active))
        out.resize(static_cast<std::size_t>(active)); // room for the moves; empty entries drop out below
    // The picture a modification names: a short-term one by PicNum, a
    // long-term one by LongTermPicNum, a frame or a field as the current
    // picture is.
    auto find = [&](bool long_term, int number) -> std::optional<H264ListPicture> {
        std::uint8_t const mark = long_term ? 2 : 1;
        for (H264ListPicture const& e : references) {
            if (!field) {
                if (e.marking[0] == mark && e.marking[1] == mark && (long_term ? e.long_term_frame_idx : e.frame_num_wrap) == number) {
                    H264ListPicture frame = e;
                    frame.long_term = long_term;
                    frame.pic_num = number;
                    return frame;
                }
                continue;
            }
            for (int f = 0; f < 2; ++f) {
                if (e.marking[static_cast<std::size_t>(f)] != mark)
                    continue;
                int const n = 2 * (long_term ? e.long_term_frame_idx : e.frame_num_wrap) + (f == parity ? 1 : 0);
                if (n != number)
                    continue;
                H264ListPicture one = e;
                one.structure = 1 + f;
                one.long_term = long_term;
                one.pic_num = n;
                one.poc = f == 0 ? e.poc_top : e.poc_bottom;
                return one;
            }
        }
        return std::nullopt;
    };
    for (H264RefListModification const& m : header.ref_list_modifications[list]) {
        if (m.idc > 2)
            break;
        std::optional<H264ListPicture> wanted;
        if (m.idc == 0 || m.idc == 1) {
            int pic_num_no_wrap = m.idc == 0 ? pic_num_pred - m.value : pic_num_pred + m.value;
            if (pic_num_no_wrap < 0)
                pic_num_no_wrap += max_pic_num;
            else if (pic_num_no_wrap >= max_pic_num)
                pic_num_no_wrap -= max_pic_num;
            pic_num_pred = pic_num_no_wrap;
            int const pic_num = pic_num_no_wrap > curr_pic_num ? pic_num_no_wrap - max_pic_num : pic_num_no_wrap;
            wanted = find(false, pic_num);
        } else {
            wanted = find(true, m.value);
        }
        if (!wanted || ref_idx >= out.size())
            continue;
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(ref_idx), *wanted);
        ++ref_idx;
        // The same picture further along goes.
        for (std::size_t i = ref_idx; i < out.size(); ++i) {
            if (same_picture(out[i], *wanted)) {
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    std::erase_if(out, [](H264ListPicture const& e) { return e.slot < 0; });
    if (out.size() > static_cast<std::size_t>(active))
        out.resize(static_cast<std::size_t>(active));
    return out;
}

}
