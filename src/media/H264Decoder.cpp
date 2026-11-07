#include "media/H264Decoder.h"

#include <algorithm>

namespace sashfold::media {

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
        bool const held = std::any_of(m_dpb.begin(), m_dpb.end(), [slot](Entry const& e) { return e.slot == slot; });
        if (!held)
            return slot;
    }
    return -1;
}

// 8.2.5.3: when the buffer is full of references, the short-term one with
// the smallest FrameNumWrap is let go.
void H264Decoder::sliding_window(H264Sps const& sps)
{
    int const most = std::max(1, sps.max_num_ref_frames);
    if (static_cast<int>(m_dpb.size()) < most)
        return;
    auto oldest = m_dpb.end();
    for (auto it = m_dpb.begin(); it != m_dpb.end(); ++it) {
        if (it->long_term)
            continue;
        if (oldest == m_dpb.end() || it->frame_num_wrap < oldest->frame_num_wrap)
            oldest = it;
    }
    if (oldest != m_dpb.end())
        m_dpb.erase(oldest);
}

// 8.2.5.4: the memory management control operations of the current picture.
bool H264Decoder::apply_mmco(H264SliceHeader const& slice, H264Sps const& sps, H264PocState::Poc& poc, bool& current_long_term,
    int& current_long_term_idx)
{
    int const max_frame_num = 1 << sps.log2_max_frame_num;
    int const curr_pic_num = slice.frame_num;
    for (H264Mmco const& m : slice.mmco) {
        switch (m.op) {
        case 1: { // a short-term picture let go, by its picture number
            int pic_num_x = curr_pic_num - m.difference_of_pic_nums;
            std::erase_if(m_dpb, [&](Entry const& e) { return !e.long_term && e.frame_num_wrap == pic_num_x; });
            break;
        }
        case 2: // a long-term picture let go
            std::erase_if(m_dpb, [&](Entry const& e) { return e.long_term && e.long_term_frame_idx == m.long_term_pic_num; });
            break;
        case 3: { // a short-term picture becomes long-term
            int const pic_num_x = curr_pic_num - m.difference_of_pic_nums;
            std::erase_if(m_dpb, [&](Entry const& e) { return e.long_term && e.long_term_frame_idx == m.long_term_frame_idx; });
            for (Entry& e : m_dpb) {
                if (!e.long_term && e.frame_num_wrap == pic_num_x) {
                    e.long_term = true;
                    e.long_term_frame_idx = m.long_term_frame_idx;
                }
            }
            break;
        }
        case 4: // the most long-term indices there may be
            m_max_long_term_frame_idx = m.max_long_term_frame_idx_plus1 - 1;
            std::erase_if(m_dpb, [&](Entry const& e) { return e.long_term && e.long_term_frame_idx > m_max_long_term_frame_idx; });
            break;
        case 5: { // everything let go; the picture counts start over at this one
            m_dpb.clear();
            m_max_long_term_frame_idx = -1;
            int const temp = std::min(poc.top, poc.bottom);
            poc.top -= temp;
            poc.bottom -= temp;
            break;
        }
        case 6: // the current picture as a long-term reference
            std::erase_if(m_dpb, [&](Entry const& e) { return e.long_term && e.long_term_frame_idx == m.long_term_frame_idx; });
            current_long_term = true;
            current_long_term_idx = m.long_term_frame_idx;
            break;
        default:
            return false;
        }
    }
    (void)max_frame_num;
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
        for (Entry& e : m_dpb) {
            if (!e.long_term)
                e.frame_num_wrap = e.frame_num > expected ? e.frame_num - max_frame_num : e.frame_num;
        }
        sliding_window(sps);
        Entry gap;
        gap.slot = -1;
        gap.frame_num = expected;
        gap.frame_num_wrap = expected;
        m_dpb.push_back(gap);
        m_counts.gaps++;
        expected = (expected + 1) % max_frame_num;
    }
}

std::optional<Nv12Picture> H264Decoder::decode(std::span<std::uint8_t const> access_unit)
{
    std::vector<H264Nal> const nals = m_length_size > 0 ? split_h264_length_prefixed(access_unit, m_length_size) : split_h264_annex_b(access_unit);
    take_parameter_sets(nals);
    // The picture's slices, read with the first one's sets.
    H264PictureInfo picture;
    std::optional<H264SliceHeader> first;
    H264Sps const* sps = nullptr;
    H264Pps const* pps = nullptr;
    for (H264Nal const& nal : nals) {
        if (nal.type != 1 && nal.type != 5)
            continue;
        if (!first) {
            // The header names its PPS; the PPS its SPS. Read once with the
            // sets found by a first pass over the id alone.
            std::vector<std::uint8_t> const rbsp = h264_rbsp(nal.bytes);
            // first_mb_in_slice ue, slice_type ue, pps id ue: parsed by trying each PPS.
            for (auto const& [id, candidate] : m_pps) {
                auto const s = m_sps.find(candidate.sps_id);
                if (s == m_sps.end())
                    continue;
                std::optional<H264SliceHeader> header = parse_h264_slice_header(nal.bytes, s->second, candidate);
                if (header && header->pps_id == id) {
                    first = std::move(header);
                    sps = &s->second;
                    pps = &candidate;
                    break;
                }
            }
            if (!first) {
                m_counts.failed++;
                return std::nullopt;
            }
            picture.first_slice = &*first;
        }
        std::optional<H264SliceHeader> const header = parse_h264_slice_header(nal.bytes, *sps, *pps);
        if (!header) {
            m_counts.failed++;
            return std::nullopt;
        }
        if (header->slice_type != H264SliceType::I && header->slice_type != H264SliceType::SI)
            picture.intra = false;
        picture.slices.push_back(nal.bytes);
    }
    if (!first)
        return std::nullopt; // no picture in this unit
    if (first->field_pic || !sps->frame_mbs_only || sps->chroma_format_idc != 1 || sps->bit_depth_luma != 8) {
        m_counts.failed++; // interlaced and beyond 4:2:0 at 8 bits: not written yet
        return std::nullopt;
    }
    if (!m_started && !first->idr) {
        m_counts.failed++; // a stream must begin at an IDR
        return std::nullopt;
    }
    if (first->idr) {
        m_dpb.clear();
        m_max_long_term_frame_idx = first->long_term_reference ? 0 : -1;
        m_started = true;
    } else if (first->frame_num != m_prev_ref_frame_num && first->frame_num != (m_prev_ref_frame_num + 1) % (1 << sps->log2_max_frame_num)) {
        fill_frame_num_gap(*first, *sps);
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
    for (Entry& e : m_dpb) {
        if (!e.long_term)
            e.frame_num_wrap = e.frame_num > first->frame_num ? e.frame_num - max_frame_num : e.frame_num;
    }
    H264PocState::Poc poc = m_poc.next(*first, *sps);

    int const slot = free_slot();
    if (slot < 0) {
        m_counts.failed++;
        return std::nullopt;
    }
    picture.sps = sps;
    picture.pps = pps;
    picture.poc_top = poc.top;
    picture.poc_bottom = poc.bottom;
    picture.frame_num = first->frame_num;
    picture.idr = first->idr;
    picture.is_reference = first->is_reference();
    picture.slot = slot;
    for (Entry const& e : m_dpb) {
        if (e.slot < 0)
            continue; // a skipped frame: nothing to read
        H264Reference r;
        r.slot = e.slot;
        r.long_term = e.long_term;
        r.frame_num = e.long_term ? e.long_term_frame_idx : e.frame_num;
        r.poc_top = e.poc_top;
        r.poc_bottom = e.poc_bottom;
        picture.references.push_back(r);
    }
    // The marking that follows (8.2.5.1), decided before the decode so the
    // accelerator knows whether the picture is kept.
    bool current_long_term = false;
    int current_long_term_idx = 0;
    if (first->idr && first->long_term_reference) {
        current_long_term = true;
        current_long_term_idx = 0;
    }
    picture.long_term = current_long_term;
    picture.long_term_frame_idx = current_long_term_idx;

    if (!m_accelerator->decode(picture)) {
        m_counts.failed++;
        return std::nullopt;
    }
    std::optional<Nv12Picture> out = m_accelerator->read(slot);
    m_counts.decoded++;

    if (first->is_reference()) {
        if (!first->idr) {
            if (first->adaptive_ref_pic_marking) {
                if (!apply_mmco(*first, *sps, poc, current_long_term, current_long_term_idx))
                    m_dpb.clear();
            } else {
                sliding_window(*sps);
            }
        }
        Entry e;
        e.slot = slot;
        e.frame_num = first->has_mmco5() ? 0 : first->frame_num;
        e.frame_num_wrap = e.frame_num;
        e.long_term = current_long_term;
        e.long_term_frame_idx = current_long_term_idx;
        e.poc_top = poc.top;
        e.poc_bottom = poc.bottom;
        // The buffer holds at most the sequence's references; one more than
        // that, from a stream that marks badly, loses its oldest.
        while (static_cast<int>(m_dpb.size()) >= std::max(1, std::min(16, sps->max_num_ref_frames)))
            m_dpb.erase(m_dpb.begin());
        m_dpb.push_back(e);
        m_prev_ref_frame_num = first->has_mmco5() ? 0 : first->frame_num;
    }
    return out;
}

}
