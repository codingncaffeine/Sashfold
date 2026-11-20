// The slice data and macroblock layer of ITU-T H.264 (7.3.4, 7.3.5) read
// with either entropy coder: CAVLC (9.2) or CABAC (9.3), and each
// macroblock handed on to be predicted and reconstructed as it is read.
#include "media/H264Data.h"
#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace sashfold::media::h264sw {

using namespace h264data;

bool BitReader::more_rbsp_data() const
{
    if (m_position >= m_size * 8)
        return false;
    std::size_t last = m_size;
    while (last > 0 && m_data[last - 1] == 0)
        --last;
    if (last == 0)
        return false;
    std::uint8_t const byte = m_data[last - 1];
    int trailing = 0;
    while (!((byte >> trailing) & 1))
        ++trailing;
    std::size_t const stop_bit = (last - 1) * 8 + static_cast<std::size_t>(7 - trailing);
    return m_position < stop_bit;
}

// Tables 9-44 and 9-45 indexed by a context's pStateIdx * 2 + valMPS: the
// range of its LPS, and the context after an MPS or an LPS (9.3.3.2.1.1,
// valMPS flipping on an LPS from state 0).
struct CabacTables {
    std::uint8_t lps_range[128][4];
    std::uint8_t after[128][2]; // [context][1 after an LPS]
    CabacTables()
    {
        for (int c = 0; c < 128; ++c) {
            int const state = c >> 1, mps = c & 1;
            for (int q = 0; q < 4; ++q)
                lps_range[c][q] = range_tab_lps[state][q];
            after[c][0] = static_cast<std::uint8_t>(trans_idx_mps[state] * 2 + mps);
            after[c][1] = static_cast<std::uint8_t>(trans_idx_lps[state] * 2 + (state == 0 ? 1 - mps : mps));
        }
    }
};
CabacTables const cabac_tables;

// 9.3.1.1.
void Cabac::init_contexts(int slice_qp, int kind)
{
    int const qp = std::clamp(slice_qp, 0, 51);
    for (int i = 0; i < 1024; ++i) {
        CabacInit const& c = cabac_init[i][kind];
        int const pre = std::clamp(((c.m * qp) >> 4) + c.n, 1, 126);
        m_context[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pre <= 63 ? (63 - pre) * 2 : (pre - 64) * 2 + 1);
    }
    // ctxIdx 276 does not adapt (end_of_slice_flag and the I_PCM bin).
    m_context[276] = 63 * 2;
}

// 9.3.1.2.
void Cabac::init_engine(BitReader& reader)
{
    m_engine.data = reader.data();
    m_engine.size = reader.size();
    m_engine.next = reader.position() / 8;
    m_engine.range = 510;
    m_engine.low = 0;
    m_engine.bits = -9; // codIOffset is the first nine bits
    m_engine.refill();
}

// 9.3.3.2.1, without a branch on the bin (the parser branches on it
// already): the LPS case chosen by a mask, and RenormD as one shift, since
// codIRange is below 512 and its leading zeros say how far it falls short
// of nine bits.
inline int CabacEngine::decision(std::uint8_t& context)
{
    std::uint32_t const lps = cabac_tables.lps_range[context][(range >> 6) & 3];
    std::uint32_t const mps_range = range - lps;
    std::uint32_t const scaled = mps_range << bits;
    std::uint32_t const is_lps = low >= scaled ? 1u : 0u;
    std::uint32_t const mask = 0u - is_lps;
    low -= scaled & mask;
    range = (lps & mask) | (mps_range & ~mask);
    int const bin = (context & 1) ^ static_cast<int>(is_lps);
    context = cabac_tables.after[context][is_lps];
    int const shift = std::countl_zero(range) - 23;
    range <<= shift;
    if (bits < shift)
        refill();
    bits -= shift;
    return bin;
}

SubMbInfo sub_mb_info(H264SliceType type, int t)
{
    if (type == H264SliceType::P || type == H264SliceType::SP) {
        static constexpr SubMbInfo p[4] = { { 1, 8, 8, PredL0 }, { 2, 8, 4, PredL0 }, { 2, 4, 8, PredL0 }, { 4, 4, 4, PredL0 } };
        return p[std::clamp(t, 0, 3)];
    }
    // Table 7-18.
    static constexpr SubMbInfo b[13] = {
        { 4, 4, 4, PredDirect },
        { 1, 8, 8, PredL0 },
        { 1, 8, 8, PredL1 },
        { 1, 8, 8, PredBi },
        { 2, 8, 4, PredL0 },
        { 2, 4, 8, PredL0 },
        { 2, 8, 4, PredL1 },
        { 2, 4, 8, PredL1 },
        { 2, 8, 4, PredBi },
        { 2, 4, 8, PredBi },
        { 4, 4, 4, PredL0 },
        { 4, 4, 4, PredL1 },
        { 4, 4, 4, PredBi },
    };
    return b[std::clamp(t, 0, 12)];
}

namespace {

// Table 8-13 and Figure 8-9: the zig-zag scans as raster positions.
constexpr std::uint8_t zigzag4x4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
constexpr std::uint8_t zigzag8x8[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45,
    38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
// Tables 8-13 and 8-14: the field scans, for field macroblocks (down the
// columns before across, as a field's samples are twice as far apart
// vertically as horizontally).
constexpr std::uint8_t field4x4[16] = { 0, 4, 1, 8, 12, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
constexpr std::uint8_t field8x8[64] = { 0, 8, 16, 1, 9, 24, 32, 17, 2, 25, 40, 48, 56, 33, 10, 3, 18, 41, 49, 57, 26, 11,
    4, 19, 34, 42, 50, 58, 27, 12, 5, 20, 35, 43, 51, 59, 28, 13, 6, 21, 36, 44, 52, 60, 29, 14, 22, 37, 45, 53, 61, 30,
    7, 15, 38, 46, 54, 62, 23, 31, 39, 47, 55, 63 };

// luma4x4BlkIdx (6.4.3) to the raster index of its 4x4 block in the macroblock.
constexpr std::uint8_t blk_to_raster[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };

// Table 7-14's two partitions' predictions for B mb_type 4 to 21, by (mb_type - 4) / 2.
constexpr Pred b_pairs[9][2] = {
    { PredL0, PredL0 },
    { PredL1, PredL1 },
    { PredL0, PredL1 },
    { PredL1, PredL0 },
    { PredL0, PredBi },
    { PredL1, PredBi },
    { PredBi, PredL0 },
    { PredBi, PredL1 },
    { PredBi, PredBi },
};

bool is_inter(MbKind k) { return k >= MbKind::P16x16; }
bool uses_list(Pred p, int list) { return p != PredNone && p != PredDirect && (p & (1 << list)); }

// A VLC table for fast reading: the codes by length.
struct Vlc {
    struct Entry {
        std::uint16_t bits;
        std::uint8_t length;
        std::uint8_t a; // a value the code stands for
        std::uint8_t b;
    };
    std::vector<Entry> entries; // sorted by length
};

class Parser {
public:
    Parser(Frame& frame, Slice const& slice, BitReader& reader)
        : m_frame(frame)
        , m_slice(slice)
        , m_reader(reader)
        , m_sps(*slice.sps)
        , m_pps(*slice.pps)
        , m_header(*slice.header)
        , m_width(slice.sps->pic_width_in_mbs)
        , m_cabac_mode(slice.pps->entropy_coding_mode)
        , m_level_depth(std::max(frame.bit_depth_luma, frame.bit_depth_chroma))
    {
    }

    bool run();

private:
    // Neighbors (6.4.9 in fields and frames; 6.4.12.2's in MBAFF frames).
    bool available(int addr) const { return addr >= 0 && m_frame.mbs[static_cast<std::size_t>(addr)].slice == m_slice.number; }
    Frame::Location neighbor(int x, int y, int max = 16) const { return m_frame.neighbor(m_addr, x, y, max, max, m_slice.number); }
    int mb_a() const
    {
        if (m_mbaff)
            return neighbor(-1, 0).addr;
        return m_x > 0 && available(m_addr - 1) ? m_addr - 1 : -1;
    }
    int mb_b() const
    {
        if (m_mbaff)
            return neighbor(0, -1).addr;
        return m_y > 0 && available(m_addr - m_width) ? m_addr - m_width : -1;
    }
    MbState const& state(int addr) const { return m_frame.mbs[static_cast<std::size_t>(addr)]; }
    MbState& current() { return m_frame.mbs[static_cast<std::size_t>(m_addr)]; }
    // The 4x4 luma block left of / above raster block (x4, y4) of the
    // current macroblock: its macroblock (-1: none) and raster index.
    std::pair<int, int> left4(int x4, int y4) const
    {
        if (x4 > 0)
            return { m_addr, y4 * 4 + x4 - 1 };
        if (m_mbaff) {
            Frame::Location const n = neighbor(-1, y4 * 4);
            return { n.addr, (n.y >> 2) * 4 + (n.x >> 2) };
        }
        int const a = mb_a();
        return { a, y4 * 4 + 3 };
    }
    std::pair<int, int> above4(int x4, int y4) const
    {
        if (y4 > 0)
            return { m_addr, (y4 - 1) * 4 + x4 };
        if (m_mbaff) {
            Frame::Location const n = neighbor(x4 * 4, -1);
            return { n.addr, (n.y >> 2) * 4 + (n.x >> 2) };
        }
        int const b = mb_b();
        return { b, 12 + x4 };
    }
    // The same for a 4x4 chroma block (x2, y2) of 4:2:0 or 4:2:2 (6.4.11.5):
    // block index 0 to 3, or 0 to 7, two to a row.
    std::pair<int, int> left_chroma(int x2, int y2) const
    {
        if (x2 > 0)
            return { m_addr, y2 * 2 };
        if (m_mbaff) {
            Frame::Location const n = m_frame.neighbor(m_addr, -1, y2 * 4, 8, m_frame.mb_chroma_height, m_slice.number);
            return { n.addr, (n.y >> 2) * 2 + (n.x >> 2) };
        }
        return { mb_a(), y2 * 2 + 1 };
    }
    std::pair<int, int> above_chroma(int x2, int y2) const
    {
        if (y2 > 0)
            return { m_addr, (y2 - 1) * 2 + x2 };
        if (m_mbaff) {
            Frame::Location const n = m_frame.neighbor(m_addr, x2 * 4, -1, 8, m_frame.mb_chroma_height, m_slice.number);
            return { n.addr, (n.y >> 2) * 2 + (n.x >> 2) };
        }
        return { mb_b(), (m_frame.mb_chroma_height / 4 - 1) * 2 + x2 };
    }
    // 7.4.5.1: how many reference indices list `list` offers the current
    // macroblock — twice the slice's for an MBAFF field macroblock, whose
    // indices count fields.
    int active_references(int list) const
    {
        int const active = list == 0 ? m_header.num_ref_idx_l0_active : m_header.num_ref_idx_l1_active;
        return m_mbaff && m_field_mb ? active * 2 : active;
    }
    // Whether the current macroblock is a frame one beside a field one (or
    // the other way round): MBAFF's vertical motion and reference index
    // scale between them.
    int field_relation(int addr) const
    {
        if (!m_mbaff || addr < 0)
            return 0;
        bool const cur = m_frame.mbs[static_cast<std::size_t>(m_addr)].field, n = state(addr).field;
        return cur == n ? 0 : (cur ? 1 : -1); // 1: current field, neighbor frame; -1: the reverse
    }

    bool macroblock_layer();
    void mark_skipped();
    void finish_skip(int addr);
    void finish_pending_skip();
    bool inferred_field() const;
    bool read_field_flag();
    bool mb_pred(MbSyntax& mb);
    bool sub_mb_pred(MbSyntax& mb);
    bool residual(MbSyntax& mb);
    bool residual_luma(MbSyntax& mb, int k, std::int32_t* coeffs, std::int32_t* dc);
    bool decode_mb_type(MbSyntax& mb, int& raw);
    void set_mb_type(MbSyntax& mb, int raw, H264SliceType as);

    // CAVLC.
    int read_vlc(Vlc const& v, int& a, int& b);
    bool cavlc_block(std::int32_t* levels, int start, int end, int max_coeff, int nc, int& total);
    int nc_luma(int k, int x4, int y4) const;
    int nc_chroma(int c, int x2, int y2) const;
    // CABAC.
    bool cabac_block(std::int32_t* levels, int start, int end, int max_coeff, int cat, int cbf_inc, bool& coded);
    template<bool Field>
    static bool cabac_block(CabacEngine& engine, std::uint8_t* contexts, std::int32_t* levels, int start, int end, int max_coeff, int cat, int cbf_inc,
        bool& coded, int depth);
    int cbf_inc_luma(int k, int x4, int y4) const;
    int cbf_inc_8x8(int k, int b8) const;
    int cbf_inc_dc(bool luma_like, int k) const;
    int cbf_inc_chroma_ac(int c, int x2, int y2) const;
    int cbf_cond(int addr, bool block_available, bool block_cbf) const;
    int cabac_ref_idx(int list, int x4, int y4);
    int cabac_mvd(int list, int comp, int x4, int y4);
    int cabac_unary_mb_qp_delta();

    // Residual levels into the macroblock's arrays, inverse scanned.
    void put4x4(std::int32_t* out, std::int32_t const* list, int start, int count);

    Frame& m_frame;
    Slice const& m_slice;
    BitReader& m_reader;
    H264Sps const& m_sps;
    H264Pps const& m_pps;
    H264SliceHeader const& m_header;
    int m_width;
    bool m_cabac_mode;
    int m_level_depth; // the deeper bit depth: the levels' bound (7.4.5.3.3)
    Cabac m_cabac;
    int m_addr = 0;
    int m_x = 0;
    int m_y = 0;
    int m_qp = 26; // QPY of the previous macroblock in the slice
    int m_prev_addr = -1; // the previous macroblock in decoding order, in this slice
    bool m_prev_qp_delta_nonzero = false;
    bool m_p8x8ref0 = false; // CAVLC's P_8x8ref0: every reference index 0, none coded
    // The current macroblock is a field macroblock: its coefficients come
    // in the field scans (8.5.6, 8.5.7) with the field-coded contexts.
    bool m_field_mb = false;
    bool m_mbaff = false;
    bool m_pair_field = false; // MBAFF: the current pair's mb_field_decoding_flag (read or inferred)
    int m_pending_skip = -1; // MBAFF: a skipped top macroblock waiting for its pair's flag
    std::uint8_t const* m_scan4 = zigzag4x4;
    std::uint8_t const* m_scan8 = zigzag8x8;
    void set_field_mb(bool field)
    {
        m_field_mb = field;
        m_scan4 = field ? field4x4 : zigzag4x4;
        m_scan8 = field ? field8x8 : zigzag8x8;
    }
};

// Static VLC tables built once from the generated data.
struct VlcSet {
    Vlc coeff[6];
    Vlc tz4x4[15];
    Vlc tz2x2[3];
    Vlc tz2x4[7];
    Vlc rb[7];

    VlcSet()
    {
        auto add = [](Vlc& v, VlcCode c, int a, int b) {
            if (c.length)
                v.entries.push_back({ c.bits, c.length, static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b) });
        };
        auto sort = [](Vlc& v) { std::sort(v.entries.begin(), v.entries.end(), [](Vlc::Entry const& x, Vlc::Entry const& y) { return x.length < y.length; }); };
        for (int n = 0; n < 6; ++n) {
            for (int t1 = 0; t1 < 4; ++t1)
                for (int tc = 0; tc < 17; ++tc)
                    add(coeff[n], coeff_token[n][t1][tc], t1, tc);
            sort(coeff[n]);
        }
        for (int i = 0; i < 15; ++i) {
            for (int z = 0; z < 16; ++z)
                add(tz4x4[i], total_zeros_4x4[i][z], z, 0);
            sort(tz4x4[i]);
        }
        for (int i = 0; i < 3; ++i) {
            for (int z = 0; z < 4; ++z)
                add(tz2x2[i], total_zeros_2x2[i][z], z, 0);
            sort(tz2x2[i]);
        }
        for (int i = 0; i < 7; ++i) {
            for (int z = 0; z < 8; ++z)
                add(tz2x4[i], total_zeros_2x4[i][z], z, 0);
            sort(tz2x4[i]);
        }
        for (int i = 0; i < 7; ++i) {
            for (int r = 0; r < 15; ++r)
                add(rb[i], run_before[i][r], r, 0);
            sort(rb[i]);
        }
    }
};

VlcSet const& vlcs()
{
    static VlcSet const set;
    return set;
}

int Parser::read_vlc(Vlc const& v, int& a, int& b)
{
    std::uint32_t const window = m_reader.peek(16);
    for (Vlc::Entry const& e : v.entries) {
        if ((window >> (16 - e.length)) == e.bits) {
            m_reader.skip(e.length);
            a = e.a;
            b = e.b;
            return 0;
        }
    }
    return -1;
}

// 9.2.1: nC of a 4x4 block of component k coded as luma (0 Y; 1 Cb, 2 Cr
// of 4:4:4) from its neighbors' total coefficients of that component.
int Parser::nc_luma(int k, int x4, int y4) const
{
    auto n_of = [&](std::pair<int, int> n, bool& has) {
        has = n.first >= 0;
        if (!has)
            return 0;
        MbState const& s = state(n.first);
        if (s.kind == MbKind::IPCM)
            return 16;
        if (s.skip)
            return 0;
        return static_cast<int>(s.total_coeff[static_cast<std::size_t>(16 * k + n.second)]);
    };
    bool ha = false, hb = false;
    int const na = n_of(left4(x4, y4), ha);
    int const nb = n_of(above4(x4, y4), hb);
    if (ha && hb)
        return (na + nb + 1) >> 1;
    return ha ? na : (hb ? nb : 0);
}

int Parser::nc_chroma(int c, int x2, int y2) const
{
    auto n_of = [&](int addr, int idx, bool& has) {
        has = addr >= 0;
        if (!has)
            return 0;
        MbState const& s = state(addr);
        if (s.kind == MbKind::IPCM)
            return 16;
        if (s.skip)
            return 0;
        return static_cast<int>(s.total_coeff[static_cast<std::size_t>(16 + c * 16 + idx)]);
    };
    bool ha = false, hb = false;
    std::pair<int, int> const left = left_chroma(x2, y2), above = above_chroma(x2, y2);
    int const na = n_of(left.first, left.second, ha);
    int const nb = n_of(above.first, above.second, hb);
    if (ha && hb)
        return (na + nb + 1) >> 1;
    return ha ? na : (hb ? nb : 0);
}

// 9.2: residual_block_cavlc into `levels[start..end]` (list order).
bool Parser::cavlc_block(std::int32_t* levels, int start, int end, int max_coeff, int nc, int& total)
{
    VlcSet const& v = vlcs();
    int cls;
    if (nc == -1)
        cls = 4;
    else if (nc == -2)
        cls = 5;
    else if (nc < 2)
        cls = 0;
    else if (nc < 4)
        cls = 1;
    else if (nc < 8)
        cls = 2;
    else
        cls = 3;
    int trailing = 0, count = 0;
    if (read_vlc(v.coeff[cls], trailing, count) < 0)
        return false;
    total = count;
    for (int i = start; i <= end; ++i)
        levels[i] = 0;
    if (count == 0)
        return true;
    if (count > max_coeff)
        return false;
    std::array<int, 16> level {};
    int suffix_length = (count > 10 && trailing < 3) ? 1 : 0;
    for (int i = 0; i < count; ++i) {
        if (i < trailing) {
            level[static_cast<std::size_t>(i)] = m_reader.bit() ? -1 : 1;
            continue;
        }
        int prefix = 0;
        while (m_reader.bit() == 0) {
            if (++prefix > 32)
                return false;
        }
        int code = std::min(15, prefix) << suffix_length;
        int suffix_size = suffix_length;
        if (prefix == 14 && suffix_length == 0)
            suffix_size = 4;
        else if (prefix >= 15)
            suffix_size = prefix - 3;
        if (suffix_size > 0)
            code += static_cast<int>(m_reader.bits(suffix_size));
        if (prefix >= 15 && suffix_length == 0)
            code += 15;
        if (prefix >= 16)
            code += (1 << (prefix - 3)) - 4096;
        if (i == trailing && trailing < 3)
            code += 2;
        // 7.4.5.3.3 bounds a level to -2^(7 + bitDepth) .. 2^(7 + bitDepth) - 1;
        // held there so a broken stream cannot overflow the scaling.
        int const limit = 1 << (7 + m_level_depth);
        int const value = std::clamp((code % 2 == 0) ? (code + 2) >> 1 : (-code - 1) >> 1, -limit, limit - 1);
        level[static_cast<std::size_t>(i)] = value;
        if (suffix_length == 0)
            suffix_length = 1;
        if (std::abs(value) > (3 << (suffix_length - 1)) && suffix_length < 6)
            ++suffix_length;
    }
    int zeros_left = 0;
    if (count < end - start + 1) {
        int z = 0, unused = 0;
        if (max_coeff == 4) {
            if (read_vlc(v.tz2x2[count - 1], z, unused) < 0)
                return false;
        } else if (max_coeff == 8) { // 4:2:2's chroma DC (Table 9-9 b)
            if (read_vlc(v.tz2x4[count - 1], z, unused) < 0)
                return false;
        } else {
            if (read_vlc(v.tz4x4[count - 1], z, unused) < 0)
                return false;
        }
        zeros_left = z;
    }
    std::array<int, 16> run {};
    for (int i = 0; i < count - 1; ++i) {
        if (zeros_left > 0) {
            int r = 0, unused = 0;
            if (read_vlc(v.rb[std::min(zeros_left, 7) - 1], r, unused) < 0)
                return false;
            run[static_cast<std::size_t>(i)] = r;
        }
        zeros_left -= run[static_cast<std::size_t>(i)];
        if (zeros_left < 0)
            return false;
    }
    run[static_cast<std::size_t>(count - 1)] = zeros_left;
    int coeff = -1;
    for (int i = count - 1; i >= 0; --i) {
        coeff += run[static_cast<std::size_t>(i)] + 1;
        if (start + coeff > end)
            return false;
        levels[start + coeff] = level[static_cast<std::size_t>(i)];
    }
    return true;
}

// 9.3.3.1.1.9: condTermFlagN of coded_block_flag.
int Parser::cbf_cond(int addr, bool block_available, bool block_cbf) const
{
    bool const cur_intra = !is_inter(m_frame.mbs[static_cast<std::size_t>(m_addr)].kind);
    if (addr < 0)
        return cur_intra ? 1 : 0;
    MbState const& s = state(addr);
    if (s.kind == MbKind::IPCM)
        return 1;
    if (s.skip || !block_available)
        return 0;
    return block_cbf ? 1 : 0;
}

// cat 1, 2 and their Cb and Cr kin (7, 8, 11, 12): component k's 4x4
// blocks; a neighbor coded with the 8x8 transform gives its 8x8 block's
// flag, which its four 4x4 bits hold.
int Parser::cbf_inc_luma(int k, int x4, int y4) const
{
    auto cond = [&](std::pair<int, int> n) {
        if (n.first < 0)
            return cbf_cond(-1, false, false);
        MbState const& s = n.first == m_addr ? m_frame.mbs[static_cast<std::size_t>(m_addr)] : state(n.first);
        int const b8 = (n.second >> 3) * 2 + ((n.second & 3) >> 1);
        bool const has = s.kind != MbKind::IPCM && !s.skip && ((s.cbp >> b8) & 1);
        return cbf_cond(n.first, has, (s.cbf >> (16 * k + n.second)) & 1);
    };
    return cond(left4(x4, y4)) + 2 * cond(above4(x4, y4));
}

// cat 5, 9, 13 (4:4:4 alone has their flag): component k's 8x8 block b8,
// beside 8x8 blocks of neighbors coded with the 8x8 transform (6.4.11.2).
int Parser::cbf_inc_8x8(int k, int b8) const
{
    int const bx = b8 & 1, by = b8 >> 1;
    auto cond = [&](int addr, int nb8) {
        if (addr < 0)
            return cbf_cond(-1, false, false);
        MbState const& s = addr == m_addr ? m_frame.mbs[static_cast<std::size_t>(m_addr)] : state(addr);
        bool const has = s.kind != MbKind::IPCM && !s.skip && ((s.cbp >> nb8) & 1) && s.transform_8x8;
        return cbf_cond(addr, has, (s.cbf >> (16 * k + (nb8 >> 1) * 8 + (nb8 & 1) * 2)) & 1);
    };
    auto outside = [&](int x, int y, int non_mbaff_addr, int non_mbaff_b8) {
        if (!m_mbaff)
            return cond(non_mbaff_addr, non_mbaff_b8);
        Frame::Location const n = neighbor(x, y);
        return cond(n.addr, (n.y >> 3) * 2 + (n.x >> 3));
    };
    int const a = bx > 0 ? cond(m_addr, b8 - 1) : outside(-1, by * 8, mb_a(), b8 + 1);
    int const b = by > 0 ? cond(m_addr, b8 - 2) : outside(bx * 8, -1, mb_b(), b8 + 2);
    return a + 2 * b;
}

// cat 0, 6, 10 (`luma_like`: component k's Intra_16x16 DC) or cat 3 (the
// chroma DC of 4:2:0 and 4:2:2, component k - 1).
int Parser::cbf_inc_dc(bool luma_like, int k) const
{
    int const bit = 48 + k;
    auto cond = [&](int addr) {
        if (addr < 0)
            return cbf_cond(-1, false, false);
        MbState const& s = state(addr);
        bool const has = luma_like ? s.kind == MbKind::I16x16 : (!s.skip && s.kind != MbKind::IPCM && (s.cbp >> 4) != 0);
        return cbf_cond(addr, has, (s.cbf >> bit) & 1);
    };
    return cond(mb_a()) + 2 * cond(mb_b());
}

int Parser::cbf_inc_chroma_ac(int c, int x2, int y2) const
{
    auto cond = [&](int addr, int idx) {
        if (addr < 0)
            return cbf_cond(-1, false, false);
        MbState const& s = addr == m_addr ? m_frame.mbs[static_cast<std::size_t>(m_addr)] : state(addr);
        bool const has = !s.skip && s.kind != MbKind::IPCM && (s.cbp >> 4) == 2;
        return cbf_cond(addr, has, (s.cbf >> (16 + c * 16 + idx)) & 1);
    };
    std::pair<int, int> const left = left_chroma(x2, y2), above = above_chroma(x2, y2);
    return cond(left.first, left.second) + 2 * cond(above.first, above.second);
}

// 7.3.5.3.3 with 9.3.3.1.1.9 and 9.3.3.1.3: residual_block_cabac.
bool Parser::cabac_block(std::int32_t* levels, int start, int end, int max_coeff, int cat, int cbf_inc, bool& coded)
{
    // The engine in a local the compiler keeps in registers (the level
    // stores below could otherwise alias its fields), given back at the end.
    CabacEngine engine = m_cabac.engine();
    std::uint8_t* const contexts = m_cabac.contexts();
    // A field macroblock's significance map has contexts of its own (Table 9-34).
    int const depth = m_level_depth;
    bool const ok = m_field_mb ? cabac_block<true>(engine, contexts, levels, start, end, max_coeff, cat, cbf_inc, coded, depth)
                               : cabac_block<false>(engine, contexts, levels, start, end, max_coeff, cat, cbf_inc, coded, depth);
    m_cabac.engine() = engine;
    return ok;
}

template<bool Field>
bool Parser::cabac_block(CabacEngine& engine, std::uint8_t* contexts, std::int32_t* levels, int start, int end, int max_coeff, int cat, int cbf_inc,
    bool& coded, int depth)
{
    for (int i = start; i <= end; ++i)
        levels[i] = 0;
    // Tables 9-34 and 9-40: per ctxBlockCat (0 to 4 luma and 4:2:0 / 4:2:2
    // chroma, 5 luma 8x8, 6 to 9 and 10 to 13 the Cb and Cr of 4:4:4 coded
    // as luma's 0, 1, 2 and 5 are), ctxIdxOffset + ctxIdxBlockCatOffset.
    static constexpr int cbf_base[14] = { 85, 89, 93, 97, 101, 1012, 460, 464, 468, 1016, 472, 476, 480, 1020 };
    static constexpr int sig_frame[14] = { 105, 120, 134, 149, 152, 402, 484, 499, 513, 660, 528, 543, 557, 718 };
    static constexpr int sig_field[14] = { 277, 292, 306, 321, 324, 436, 776, 791, 805, 675, 820, 835, 849, 733 };
    static constexpr int last_frame[14] = { 166, 181, 195, 210, 213, 417, 572, 587, 601, 690, 616, 631, 645, 748 };
    static constexpr int last_field[14] = { 338, 353, 367, 382, 385, 451, 864, 879, 893, 699, 908, 923, 937, 757 };
    static constexpr int abs_base[14] = { 227, 237, 247, 257, 266, 426, 952, 962, 972, 708, 982, 992, 1002, 766 };
    // An 8x8 block has a coded_block_flag in 4:4:4 alone (7.3.5.3.3); cbf_inc -1 says it has none.
    if (cbf_inc >= 0) {
        coded = engine.decision(contexts[cbf_base[cat] + cbf_inc]) != 0;
        if (!coded)
            return true;
    } else {
        coded = true;
    }
    bool const eight = max_coeff == 64;
    // 9.3.3.1.3: chroma DC's ctxIdxInc is Min( numDecodAbsLevel / NumC8x8, 2 ), NumC8x8 2 for 4:2:2's eight levels.
    int const dc_shift = max_coeff == 8 ? 1 : 0;
    int const sig_coded = Field ? sig_field[cat] : sig_frame[cat];
    int const last_coded = Field ? last_field[cat] : last_frame[cat];
    constexpr std::uint8_t const* sig_8x8 = Field ? significant_8x8_field : significant_8x8_frame;
    int const count = end - start + 1;
    std::array<bool, 64> significant {};
    int num = count;
    for (int i = 0; i < num - 1; ++i) {
        int sig_ctx, last_ctx;
        if (eight) {
            sig_ctx = sig_coded + sig_8x8[i];
            last_ctx = last_coded + last_significant_8x8[i];
        } else {
            int const inc = cat == 3 ? std::min(i >> dc_shift, 2) : i;
            sig_ctx = sig_coded + inc;
            last_ctx = last_coded + inc;
        }
        if (engine.decision(contexts[sig_ctx])) {
            significant[static_cast<std::size_t>(i)] = true;
            if (engine.decision(contexts[last_ctx])) {
                num = i + 1;
                break;
            }
        }
    }
    significant[static_cast<std::size_t>(num - 1)] = true;
    int eq1 = 0, gt1 = 0;
    int const base = abs_base[cat];
    for (int i = num - 1; i >= 0; --i) {
        if (!significant[static_cast<std::size_t>(i)])
            continue;
        // coeff_abs_level_minus1: TU prefix (cMax 14) with contexts, then Exp-Golomb k = 0 bypass.
        int inc0 = gt1 != 0 ? 0 : std::min(4, 1 + eq1);
        int value = 0;
        if (engine.decision(contexts[base + inc0])) {
            int const inc = 5 + std::min(4 - (cat == 3 ? 1 : 0), gt1);
            value = 1;
            while (value < 14 && engine.decision(contexts[base + inc]))
                ++value;
            if (value == 14) {
                int k = 0;
                while (engine.bypass()) {
                    value += 1 << k;
                    if (++k > depth + 8) // past any level a stream may hold (7.4.5.3.3: 2^(7 + bitDepth))
                        return false;
                }
                while (k-- > 0)
                    value += engine.bypass() << k;
                value = std::min(value, (1 << (7 + depth)) - 1);
            }
        }
        int const level = value + 1;
        if (level == 1)
            ++eq1;
        else
            ++gt1;
        levels[start + i] = engine.bypass() ? -level : level;
    }
    return true;
}

// 9.3.3.1.1.6: ref_idx's first bin context.
int Parser::cabac_ref_idx(int list, int x4, int y4)
{
    auto cond = [&](std::pair<int, int> n) {
        if (n.first < 0)
            return 0;
        MbState const& s = n.first == m_addr ? current() : state(n.first);
        if (s.skip || s.intra)
            return 0;
        int const b8 = (n.second >> 3) * 2 + ((n.second & 3) >> 1);
        // (9-12): a field neighbor of a frame macroblock counts fields.
        int const zero_up_to = field_relation(n.first) < 0 ? 1 : 0;
        return s.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(b8)] > zero_up_to ? 1 : 0;
    };
    int const inc = cond(left4(x4, y4)) + 2 * cond(above4(x4, y4));
    if (!m_cabac.decision(54 + inc))
        return 0;
    int value = 1;
    if (!m_cabac.decision(54 + 4))
        return value;
    ++value;
    while (m_cabac.decision(54 + 5)) {
        if (++value > 32)
            break;
    }
    return value;
}

// 9.3.3.1.1.7 and 9.3.2.3 (UEG3, signed, uCoff 9): one mvd component.
int Parser::cabac_mvd(int list, int comp, int x4, int y4)
{
    auto abs_of = [&](std::pair<int, int> n) {
        if (n.first < 0)
            return 0;
        MbState const& s = n.first == m_addr ? current() : state(n.first);
        if (s.skip || s.intra)
            return 0;
        Mv const& m = s.mvd[static_cast<std::size_t>(list)][static_cast<std::size_t>(n.second)];
        if (comp == 0)
            return static_cast<int>(m.x);
        // (9-15), (9-16): vertical differences between frame and field macroblocks.
        int const relation = field_relation(n.first);
        return relation < 0 ? m.y * 2 : (relation > 0 ? m.y / 2 : static_cast<int>(m.y));
    };
    int const sum = abs_of(left4(x4, y4)) + abs_of(above4(x4, y4));
    int const inc = sum < 3 ? 0 : (sum > 32 ? 2 : 1);
    int const base = comp == 0 ? 40 : 47;
    if (!m_cabac.decision(base + inc))
        return 0;
    int value = 1;
    static constexpr int incs[9] = { 0, 3, 4, 5, 6, 6, 6, 6, 6 };
    while (value < 9 && m_cabac.decision(base + incs[value]))
        ++value;
    if (value >= 9) {
        int k = 3;
        while (m_cabac.bypass()) {
            value += 1 << k;
            if (++k > 16) // past any motion vector difference (Annex A's ranges)
                break;
        }
        while (k-- > 0)
            value += m_cabac.bypass() << k;
    }
    return m_cabac.bypass() ? -value : value;
}

int Parser::cabac_unary_mb_qp_delta()
{
    int const inc0 = m_prev_qp_delta_nonzero ? 1 : 0;
    if (!m_cabac.decision(60 + inc0))
        return 0;
    int k = 1;
    if (m_cabac.decision(60 + 2)) {
        ++k;
        while (m_cabac.decision(60 + 3)) {
            if (++k > 104)
                break;
        }
    }
    // Table 9-3: the mapped value.
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

void Parser::put4x4(std::int32_t* out, std::int32_t const* list, int start, int count)
{
    for (int i = 0; i < count; ++i)
        out[m_scan4[start + i]] = list[i];
}

void Parser::set_mb_type(MbSyntax& mb, int raw, H264SliceType as)
{
    mb.partitions = 1;
    if (as == H264SliceType::I) {
        if (raw == 0) {
            mb.kind = MbKind::I4x4;
        } else if (raw == 25) {
            mb.kind = MbKind::IPCM;
        } else {
            int const t = raw - 1;
            mb.kind = MbKind::I16x16;
            mb.i16_mode = t % 4;
            mb.cbp = ((t / 4) % 3) << 4 | (t >= 12 ? 15 : 0);
        }
        return;
    }
    if (as == H264SliceType::P) {
        switch (raw) {
        case 0:
            mb.kind = MbKind::P16x16;
            mb.part_pred[0] = PredL0;
            break;
        case 1:
            mb.kind = MbKind::P16x8;
            mb.partitions = 2;
            mb.part_pred[0] = mb.part_pred[1] = PredL0;
            break;
        case 2:
            mb.kind = MbKind::P8x16;
            mb.partitions = 2;
            mb.part_pred[0] = mb.part_pred[1] = PredL0;
            break;
        default:
            mb.kind = MbKind::P8x8;
            mb.partitions = 4;
            break;
        }
        return;
    }
    // B, Table 7-14.
    if (raw == 0) {
        mb.kind = MbKind::BDirect16x16;
        mb.part_pred[0] = PredDirect;
    } else if (raw <= 3) {
        mb.kind = MbKind::B16x16;
        mb.part_pred[0] = raw == 1 ? PredL0 : (raw == 2 ? PredL1 : PredBi);
    } else if (raw <= 21) {
        mb.kind = (raw % 2 == 0) ? MbKind::B16x8 : MbKind::B8x16;
        mb.partitions = 2;
        mb.part_pred[0] = b_pairs[(raw - 4) / 2][0];
        mb.part_pred[1] = b_pairs[(raw - 4) / 2][1];
    } else {
        mb.kind = MbKind::B8x8;
        mb.partitions = 4;
    }
}

// mb_type: ue(v) for CAVLC; Tables 9-36 and 9-37 for CABAC.
bool Parser::decode_mb_type(MbSyntax& mb, int& raw)
{
    H264SliceType const type = m_slice.type;
    if (!m_cabac_mode) {
        raw = static_cast<int>(m_reader.ue());
        if (type == H264SliceType::P) {
            if (raw >= 5) {
                set_mb_type(mb, raw - 5, H264SliceType::I);
                return raw - 5 <= 25;
            }
            set_mb_type(mb, raw, H264SliceType::P);
            if (raw == 4)
                mb.ref_idx[0] = { 0, 0, 0, 0 }; // P_8x8ref0
            return true;
        }
        if (type == H264SliceType::B) {
            if (raw >= 23) {
                set_mb_type(mb, raw - 23, H264SliceType::I);
                return raw - 23 <= 25;
            }
            set_mb_type(mb, raw, H264SliceType::B);
            return true;
        }
        set_mb_type(mb, raw, H264SliceType::I);
        return raw <= 25;
    }
    // The I macroblock types' bins after their first (prefix ctxIdxOffset 3, suffix 17 or 32).
    auto i_type = [&](bool suffix, int offset) {
        int first;
        if (suffix) {
            first = m_cabac.decision(offset);
        } else {
            auto cond = [&](int addr) { return addr >= 0 && state(addr).kind != MbKind::I4x4 && state(addr).kind != MbKind::I8x8 ? 1 : 0; };
            first = m_cabac.decision(offset + cond(mb_a()) + cond(mb_b()));
        }
        if (!first)
            return 0; // I_NxN
        if (m_cabac.terminate())
            return 25; // I_PCM
        int const b2 = suffix ? 1 : 3;
        int const b3 = suffix ? 2 : 4;
        int const luma = m_cabac.decision(offset + b2);
        int const chroma_nonzero = m_cabac.decision(offset + b3);
        int chroma = 0;
        if (chroma_nonzero)
            chroma = 1 + m_cabac.decision(offset + (suffix ? 2 : 5));
        int const m0 = m_cabac.decision(offset + (suffix ? 3 : (chroma_nonzero ? 6 : 6)));
        int const m1 = m_cabac.decision(offset + (suffix ? 3 : (chroma_nonzero ? 7 : 7)));
        return 1 + (m0 * 2 + m1) + 4 * chroma + 12 * luma;
    };
    if (type == H264SliceType::I) {
        raw = i_type(false, 3);
        set_mb_type(mb, raw, H264SliceType::I);
        return true;
    }
    if (type == H264SliceType::P) {
        if (m_cabac.decision(14)) {
            raw = i_type(true, 17);
            set_mb_type(mb, raw, H264SliceType::I);
            return true;
        }
        if (!m_cabac.decision(15))
            raw = m_cabac.decision(16) ? 3 : 0;
        else
            raw = m_cabac.decision(17) ? 1 : 2;
        set_mb_type(mb, raw, H264SliceType::P);
        return true;
    }
    // B.
    auto cond = [&](int addr) { return addr >= 0 && !state(addr).direct_16x16 ? 1 : 0; };
    if (!m_cabac.decision(27 + cond(mb_a()) + cond(mb_b()))) {
        raw = 0;
    } else if (!m_cabac.decision(27 + 3)) {
        raw = 1 + m_cabac.decision(27 + 5); // Table 9-41: bin 2 after b1 = 0 is ctxIdxInc 5
    } else {
        int bits = m_cabac.decision(27 + 4); // after b1 = 1, ctxIdxInc 4
        for (int i = 0; i < 3; ++i)
            bits = (bits << 1) | m_cabac.decision(27 + 5);
        if (bits == 13) { // 1 1 1 1 0 1: intra
            raw = i_type(true, 32);
            set_mb_type(mb, raw, H264SliceType::I);
            return true;
        }
        if (bits == 14)
            raw = 11;
        else if (bits == 15)
            raw = 22;
        else if (bits < 8)
            raw = 3 + bits;
        else
            raw = 12 + ((bits & 7) << 1 | m_cabac.decision(27 + 5));
    }
    set_mb_type(mb, raw, H264SliceType::B);
    return true;
}

bool Parser::run()
{
    m_qp = m_slice.qp;
    set_field_mb(m_slice.structure != 0);
    if (m_cabac_mode && (m_header.cabac_init_idc < 0 || m_header.cabac_init_idc > 2))
        return false; // 7.4.3: 0 to 2
    if (m_cabac_mode) {
        while (!m_reader.byte_aligned())
            m_reader.bit(); // cabac_alignment_one_bit
        int const kind = m_slice.type == H264SliceType::I ? 0 : 1 + m_header.cabac_init_idc;
        m_cabac.init_contexts(m_slice.qp, kind);
        m_cabac.init_engine(m_reader);
    }
    int const total = static_cast<int>(m_frame.mbs.size());
    m_mbaff = m_slice.mbaff;
    // 7.3.4: in MBAFF frames the slice starts at a pair, and its addresses
    // count the pairs' macroblocks top then bottom.
    m_addr = m_header.first_mb_in_slice * (m_mbaff ? 2 : 1);
    bool more = true;
    int skip_run = -1;
    bool prev_skipped = false;
    m_pending_skip = -1;
    while (more) {
        if (m_addr >= total)
            return false;
        bool const top = (m_addr & 1) == 0;
        if (m_mbaff) {
            // The pair's mb_field_decoding_flag until one is read: 7.4.4's inference.
            if (top)
                m_pair_field = inferred_field();
            set_field_mb(m_pair_field);
            current().field = m_pair_field;
        } else {
            m_x = m_addr % m_width;
            m_y = m_addr / m_width;
        }
        bool skipped = false;
        if (m_slice.type != H264SliceType::I) {
            if (!m_cabac_mode) {
                if (skip_run < 0) {
                    skip_run = static_cast<int>(m_reader.ue());
                    if (m_reader.overrun())
                        return false;
                }
                if (skip_run > 0) {
                    --skip_run;
                    skipped = true;
                } else {
                    skip_run = -1;
                }
            } else {
                auto cond = [&](int addr) { return addr >= 0 && !state(addr).skip ? 1 : 0; };
                int const base = m_slice.type == H264SliceType::B ? 24 : 11;
                // the macroblock is not yet marked; its neighbors decide
                m_frame.mbs[static_cast<std::size_t>(m_addr)].slice = -1;
                skipped = m_cabac.decision(base + cond(mb_a()) + cond(mb_b())) != 0;
            }
        }
        if (skipped) {
            mark_skipped();
            // A skipped top macroblock waits for the pair's flag, which
            // its bottom one may still carry (7.4.4's note).
            if (m_mbaff && top) {
                m_pending_skip = m_addr;
            } else {
                finish_pending_skip();
                finish_skip(m_addr);
            }
            if (!m_cabac_mode)
                more = skip_run > 0 || m_reader.more_rbsp_data();
            else
                more = (m_mbaff && top) || !m_cabac.terminate();
        } else {
            if (m_mbaff && (top || prev_skipped)) {
                m_pair_field = read_field_flag();
                set_field_mb(m_pair_field);
                current().field = m_pair_field;
                if (m_pending_skip >= 0)
                    m_frame.mbs[static_cast<std::size_t>(m_pending_skip)].field = m_pair_field;
            }
            finish_pending_skip();
            if (!macroblock_layer())
                return false;
            if (!m_cabac_mode)
                more = m_reader.more_rbsp_data();
            else
                more = (m_mbaff && top) || !m_cabac.terminate();
        }
        if (m_reader.overrun() || (m_cabac_mode && m_cabac.overrun()))
            return false;
        prev_skipped = skipped;
        ++m_addr;
    }
    finish_pending_skip();
    return true;
}

// 7.4.4: an MBAFF pair's field decoding flag where it carries none: its
// left neighbor's in the slice, else the one above's, else frame.
bool Parser::inferred_field() const
{
    int const pair = m_addr >> 1, px = pair % m_width;
    if (px > 0 && available(m_addr - 2))
        return state(m_addr - 2).field;
    if (pair >= m_width && available(2 * (pair - m_width)))
        return state(2 * (pair - m_width)).field;
    return false;
}

// mb_field_decoding_flag: u(1), or a bin whose context counts the field
// pairs left of and above this one (9.3.3.1.1.2).
bool Parser::read_field_flag()
{
    if (!m_cabac_mode)
        return m_reader.bit() != 0;
    int const pair = m_addr >> 1, px = pair % m_width;
    int inc = 0;
    if (px > 0 && available(m_addr - 2 - (m_addr & 1)) && state(m_addr - 2 - (m_addr & 1)).field)
        ++inc;
    int const above = 2 * (pair - m_width);
    if (pair >= m_width && available(above) && state(above).field)
        ++inc;
    return m_cabac.decision(70 + inc) != 0;
}

// A skipped macroblock's state, for its neighbors' parsing; its motion
// and samples follow in finish_skip().
void Parser::mark_skipped()
{
    MbState& s = current();
    s = MbState {};
    s.slice = m_slice.number;
    s.field = m_field_mb;
    s.skip = true;
    bool const b = m_slice.type == H264SliceType::B;
    s.kind = b ? MbKind::BSkip : MbKind::PSkip;
    s.direct_16x16 = b;
    s.qp = m_qp;
    s.intra4x4_modes.fill(2);
    m_prev_addr = m_addr;
    m_prev_qp_delta_nonzero = false;
}

void Parser::finish_skip(int addr)
{
    MbState const& s = state(addr);
    int const storage = m_mbaff ? m_frame.storage_mb(addr) : m_frame.storage_mb_at(m_x, m_y);
    m_frame.picture->mb_field[static_cast<std::size_t>(storage)] = s.field ? 1 : 0;
    MbSyntax& mb = m_frame.sink->begin(addr);
    mb.kind = s.kind;
    mb.part_pred[0] = s.kind == MbKind::BSkip ? PredDirect : PredL0;
    derive_motion(m_frame, m_slice, addr, mb);
    m_frame.sink->end(m_slice, addr);
}

void Parser::finish_pending_skip()
{
    if (m_pending_skip < 0)
        return;
    int const addr = m_pending_skip;
    m_pending_skip = -1;
    finish_skip(addr);
}

bool Parser::mb_pred(MbSyntax& mb)
{
    MbState& s = current();
    if (mb.kind == MbKind::I4x4 || mb.kind == MbKind::I8x8) {
        int const n = mb.kind == MbKind::I4x4 ? 16 : 4;
        for (int i = 0; i < n; ++i) {
            if (m_cabac_mode) {
                mb.prev_flag[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(m_cabac.decision(68));
                if (!mb.prev_flag[static_cast<std::size_t>(i)]) {
                    int v = m_cabac.decision(69);
                    v |= m_cabac.decision(69) << 1;
                    v |= m_cabac.decision(69) << 2;
                    mb.rem_mode[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(v);
                }
            } else {
                mb.prev_flag[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(m_reader.bit());
                if (!mb.prev_flag[static_cast<std::size_t>(i)])
                    mb.rem_mode[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(m_reader.bits(3));
            }
            // 8.3.1.1 and 8.3.2.1: the mode, from the neighbors' (stored as parsed, so later blocks see it).
            int x4, y4;
            if (n == 16) {
                int const r = blk_to_raster[i];
                x4 = r & 3;
                y4 = r >> 2;
            } else {
                x4 = (i & 1) * 2;
                y4 = (i >> 1) * 2;
            }
            auto mode_of = [&](std::pair<int, int> nb, bool is_a, bool& dc) -> int {
                if (nb.first < 0) {
                    dc = true;
                    return 2;
                }
                MbState const& ns = nb.first == m_addr ? s : state(nb.first);
                if (ns.intra == false && m_pps.constrained_intra_pred) {
                    dc = true;
                    return 2;
                }
                if (ns.kind != MbKind::I4x4 && ns.kind != MbKind::I8x8)
                    return 2;
                if (n == 4 && ns.kind == MbKind::I4x4 && nb.first != m_addr) {
                    // 8-72: the 4x4 block n = 1 (A) or 2 (B) of the 8x8 block
                    // holding the neighbor — n = 3 for the third 8x8 block of
                    // an MBAFF frame macroblock beside a field pair.
                    int const nx = nb.second & 3, ny = nb.second >> 2;
                    int const bx = (nx >> 1) * 2, by = (ny >> 1) * 2;
                    bool const third = is_a && m_mbaff && !m_field_mb && ns.field && i == 2;
                    int const pick = third ? (by + 1) * 4 + bx + 1 : (is_a ? by * 4 + bx + 1 : (by + 1) * 4 + bx);
                    return ns.intra4x4_modes[static_cast<std::size_t>(pick)];
                }
                return ns.intra4x4_modes[static_cast<std::size_t>(nb.second)];
            };
            bool dc = false;
            int const ma = mode_of(left4(x4, y4), true, dc);
            int const mb_ = mode_of(above4(x4, y4), false, dc);
            int const pred = dc ? 2 : std::min(ma, mb_);
            int mode;
            if (mb.prev_flag[static_cast<std::size_t>(i)])
                mode = pred;
            else
                mode = mb.rem_mode[static_cast<std::size_t>(i)] < pred ? mb.rem_mode[static_cast<std::size_t>(i)] : mb.rem_mode[static_cast<std::size_t>(i)] + 1;
            mb.rem_mode[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(mode); // the derived mode, from here on
            if (n == 16) {
                s.intra4x4_modes[static_cast<std::size_t>(y4 * 4 + x4)] = static_cast<std::int8_t>(mode);
            } else {
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        s.intra4x4_modes[static_cast<std::size_t>((y4 + dy) * 4 + x4 + dx)] = static_cast<std::int8_t>(mode);
            }
        }
    }
    if (mb.kind == MbKind::I4x4 || mb.kind == MbKind::I8x8 || mb.kind == MbKind::I16x16) {
        // 7.3.5.1: intra_chroma_pred_mode where the chroma has its own prediction (4:2:0, 4:2:2).
        if (m_frame.chroma_format != 1 && m_frame.chroma_format != 2)
            return true;
        if (m_cabac_mode) {
            auto cond = [&](int addr) {
                if (addr < 0)
                    return 0;
                MbState const& n = state(addr);
                return n.intra && n.kind != MbKind::IPCM && n.intra_chroma_pred_mode != 0 ? 1 : 0;
            };
            int v = 0;
            if (m_cabac.decision(64 + cond(mb_a()) + cond(mb_b()))) {
                v = 1;
                while (v < 3 && m_cabac.decision(64 + 3))
                    ++v;
            }
            mb.chroma_mode = v;
        } else {
            mb.chroma_mode = static_cast<int>(m_reader.ue());
            if (mb.chroma_mode > 3)
                return false;
        }
        s.intra_chroma_pred_mode = mb.chroma_mode;
        return true;
    }
    if (mb.kind == MbKind::BDirect16x16)
        return true;
    // Inter partitions: ref_idx l0, l1, then mvd l0, l1.
    int const parts = mb.partitions;
    auto part_origin = [&](int p, int& x4, int& y4) {
        if (mb.kind == MbKind::P16x8 || mb.kind == MbKind::B16x8) {
            x4 = 0;
            y4 = p * 2;
        } else if (mb.kind == MbKind::P8x16 || mb.kind == MbKind::B8x16) {
            x4 = p * 2;
            y4 = 0;
        } else {
            x4 = 0;
            y4 = 0;
        }
    };
    for (int list = 0; list < 2; ++list) {
        int const active = active_references(list);
        for (int p = 0; p < parts; ++p) {
            int value = -1;
            if (uses_list(mb.part_pred[static_cast<std::size_t>(p)], list)) {
                value = 0;
                if (active > 1) {
                    int x4, y4;
                    part_origin(p, x4, y4);
                    value = m_cabac_mode ? cabac_ref_idx(list, x4, y4) : static_cast<int>(m_reader.te(active - 1));
                    if (value >= active)
                        return false;
                }
            }
            mb.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(p)] = static_cast<std::int8_t>(value);
            // CABAC's context for later partitions: per 8x8.
            for (int b8 = 0; b8 < 4; ++b8) {
                bool covered = parts == 1 || ((mb.kind == MbKind::P16x8 || mb.kind == MbKind::B16x8) ? (b8 >> 1) == p : (b8 & 1) == p);
                if (covered)
                    s.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(b8)] = static_cast<std::int8_t>(value);
            }
        }
    }
    for (int list = 0; list < 2; ++list) {
        for (int p = 0; p < parts; ++p) {
            if (!uses_list(mb.part_pred[static_cast<std::size_t>(p)], list))
                continue;
            int x4, y4;
            part_origin(p, x4, y4);
            Mv d;
            if (m_cabac_mode) {
                d.x = static_cast<std::int16_t>(cabac_mvd(list, 0, x4, y4));
                d.y = static_cast<std::int16_t>(cabac_mvd(list, 1, x4, y4));
            } else {
                d.x = static_cast<std::int16_t>(m_reader.se());
                d.y = static_cast<std::int16_t>(m_reader.se());
            }
            mb.mvd[static_cast<std::size_t>(list)][static_cast<std::size_t>(p)] = d;
            Mv const a { static_cast<std::int16_t>(std::min(std::abs(d.x), 0x7fff)), static_cast<std::int16_t>(std::min(std::abs(d.y), 0x7fff)) };
            int const w = parts == 1 || mb.kind == MbKind::P16x8 || mb.kind == MbKind::B16x8 ? 4 : 2;
            int const h = parts == 1 || mb.kind == MbKind::P8x16 || mb.kind == MbKind::B8x16 ? 4 : 2;
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx)
                    s.mvd[static_cast<std::size_t>(list)][static_cast<std::size_t>((y4 + yy) * 4 + x4 + xx)] = a;
        }
    }
    return true;
}

bool Parser::sub_mb_pred(MbSyntax& mb)
{
    MbState& s = current();
    bool const b = m_slice.type == H264SliceType::B;
    for (int i = 0; i < 4; ++i) {
        int t;
        if (!m_cabac_mode) {
            t = static_cast<int>(m_reader.ue());
        } else if (!b) {
            // Table 9-38 (P): 1 → 0; 0 0 → 1; 0 1 1 → 2; 0 1 0 → 3.
            if (m_cabac.decision(21))
                t = 0;
            else if (!m_cabac.decision(22))
                t = 1;
            else
                t = m_cabac.decision(23) ? 2 : 3;
        } else {
            // Table 9-38 (B).
            if (!m_cabac.decision(36)) {
                t = 0;
            } else if (!m_cabac.decision(37)) {
                t = 1 + m_cabac.decision(39);
            } else if (m_cabac.decision(38)) {
                if (m_cabac.decision(39)) {
                    t = 11 + m_cabac.decision(39);
                } else {
                    t = 7 + (m_cabac.decision(39) << 1);
                    t += m_cabac.decision(39);
                }
            } else {
                t = 3 + (m_cabac.decision(39) << 1);
                t += m_cabac.decision(39);
            }
        }
        if (t > (b ? 12 : 3))
            return false;
        mb.sub_type[static_cast<std::size_t>(i)] = t;
        SubMbInfo const info = sub_mb_info(m_slice.type, t);
        mb.part_pred[static_cast<std::size_t>(i)] = info.pred;
        s.sub_direct[static_cast<std::size_t>(i)] = info.pred == PredDirect ? 1 : 0;
    }
    for (int list = 0; list < 2; ++list) {
        int const active = active_references(list);
        for (int i = 0; i < 4; ++i) {
            Pred const p = mb.part_pred[static_cast<std::size_t>(i)];
            int value = -1;
            if (p == PredDirect) {
                value = 0; // CABAC context: a direct partition counts as 0
            } else if (uses_list(p, list)) {
                value = 0;
                if (active > 1 && !m_p8x8ref0) {
                    value = m_cabac_mode ? cabac_ref_idx(list, (i & 1) * 2, (i >> 1) * 2) : static_cast<int>(m_reader.te(active - 1));
                    if (value >= active)
                        return false;
                }
            }
            mb.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = static_cast<std::int8_t>(p == PredDirect ? -1 : value);
            s.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = static_cast<std::int8_t>(value);
        }
    }
    for (int list = 0; list < 2; ++list) {
        for (int i = 0; i < 4; ++i) {
            Pred const p = mb.part_pred[static_cast<std::size_t>(i)];
            if (!uses_list(p, list))
                continue;
            SubMbInfo const info = sub_mb_info(m_slice.type, mb.sub_type[static_cast<std::size_t>(i)]);
            for (int j = 0; j < info.parts; ++j) {
                int const x4 = (i & 1) * 2 + (info.width == 4 ? (j & 1) : 0);
                int const y4 = (i >> 1) * 2 + (info.height == 4 ? (info.width == 4 ? (j >> 1) : j) : 0);
                Mv d;
                if (m_cabac_mode) {
                    d.x = static_cast<std::int16_t>(cabac_mvd(list, 0, x4, y4));
                    d.y = static_cast<std::int16_t>(cabac_mvd(list, 1, x4, y4));
                } else {
                    d.x = static_cast<std::int16_t>(m_reader.se());
                    d.y = static_cast<std::int16_t>(m_reader.se());
                }
                mb.mvd[static_cast<std::size_t>(list)][static_cast<std::size_t>(i * 4 + j)] = d;
                Mv const a { static_cast<std::int16_t>(std::min(std::abs(d.x), 0x7fff)), static_cast<std::int16_t>(std::min(std::abs(d.y), 0x7fff)) };
                for (int yy = 0; yy < info.height / 4; ++yy)
                    for (int xx = 0; xx < info.width / 4; ++xx)
                        s.mvd[static_cast<std::size_t>(list)][static_cast<std::size_t>((y4 + yy) * 4 + x4 + xx)] = a;
            }
        }
    }
    return true;
}

// 7.3.5.3: the macroblock's residual — luma, then the chroma of 4:2:0
// and 4:2:2 (a DC block per component, then its AC blocks), or 4:4:4's Cb
// and Cr coded as luma is.
bool Parser::residual(MbSyntax& mb)
{
    if (!residual_luma(mb, 0, mb.luma.data(), mb.luma_dc.data()))
        return false;
    int const format = m_frame.chroma_format;
    if (format == 3) {
        for (int c = 0; c < 2; ++c) {
            std::size_t const sc = static_cast<std::size_t>(c);
            if (!residual_luma(mb, 1 + c, mb.chroma_wide[sc].data(), mb.chroma_dc_wide[sc].data()))
                return false;
        }
        return true;
    }
    if (format == 0)
        return true;
    MbState& s = current();
    int const cbp_chroma = mb.cbp >> 4;
    // NumC8x8 (7.3.5.3): 1 for 4:2:0, 2 for 4:2:2's eight 4x4 blocks a component.
    int const c8x8 = format == 2 ? 2 : 1;
    int const dc_count = 4 * c8x8;
    std::array<std::int32_t, 64> list {};
    if (cbp_chroma & 3) {
        for (int c = 0; c < 2; ++c) {
            int total = 0;
            bool coded = false;
            if (m_cabac_mode) {
                if (!cabac_block(list.data(), 0, dc_count - 1, dc_count, 3, cbf_inc_dc(false, 1 + c), coded))
                    return false;
                if (coded)
                    s.cbf |= std::uint64_t { 1 } << (49 + c);
            } else if (!cavlc_block(list.data(), 0, dc_count - 1, dc_count, c8x8 == 2 ? -2 : -1, total)) {
                return false;
            }
            std::int32_t* const dc = format == 2 ? mb.chroma_dc_wide[static_cast<std::size_t>(c)].data() : mb.chroma_dc[static_cast<std::size_t>(c)].data();
            for (int i = 0; i < dc_count; ++i)
                dc[i] = list[static_cast<std::size_t>(i)];
        }
    }
    if (cbp_chroma & 2) {
        for (int c = 0; c < 2; ++c) {
            std::int32_t* const ac = format == 2 ? mb.chroma_wide[static_cast<std::size_t>(c)].data() : mb.chroma_ac[static_cast<std::size_t>(c)].data();
            for (int b4 = 0; b4 < dc_count; ++b4) {
                int const x2 = b4 & 1, y2 = b4 >> 1;
                int total = 0;
                bool coded = false;
                if (m_cabac_mode) {
                    if (!cabac_block(list.data(), 0, 14, 15, 4, cbf_inc_chroma_ac(c, x2, y2), coded))
                        return false;
                    if (coded)
                        s.cbf |= std::uint64_t { 1 } << (16 + c * 16 + b4);
                    total = coded ? 1 : 0;
                } else if (!cavlc_block(list.data(), 0, 14, 15, nc_chroma(c, x2, y2), total)) {
                    return false;
                }
                s.total_coeff[static_cast<std::size_t>(16 + c * 16 + b4)] = static_cast<std::uint8_t>(total);
                put4x4(ac + b4 * 16, list.data(), 1, 15);
            }
        }
    }
    return true;
}

// 7.3.5.3.1 residual_luma: component k (0 Y; 1 Cb and 2 Cr of 4:4:4,
// with their own block categories, contexts and counts) into `coeffs`
// (raster 4x4 blocks, or 8x8 blocks at [i8 * 64]) and `dc` (Intra_16x16's).
bool Parser::residual_luma(MbSyntax& mb, int k, std::int32_t* coeffs, std::int32_t* dc)
{
    MbState& s = current();
    int const cbp_luma = mb.cbp & 15;
    // Table 9-42's ctxBlockCat of the component's DC, AC, 4x4 and 8x8 blocks.
    static constexpr int cats[3][4] = { { 0, 1, 2, 5 }, { 6, 7, 8, 9 }, { 10, 11, 12, 13 } };
    int const* const cat = cats[k];
    int const bit0 = 16 * k; // its blocks' bits and counts
    bool const chroma444 = m_frame.chroma_format == 3;
    std::array<std::int32_t, 64> list {};
    if (mb.kind == MbKind::I16x16) {
        int total = 0;
        bool coded = false;
        if (m_cabac_mode) {
            if (!cabac_block(list.data(), 0, 15, 16, cat[0], cbf_inc_dc(true, k), coded))
                return false;
            if (coded)
                s.cbf |= std::uint64_t { 1 } << (48 + k);
        } else if (!cavlc_block(list.data(), 0, 15, 16, nc_luma(k, 0, 0), total)) {
            return false;
        }
        put4x4(dc, list.data(), 0, 16);
    }
    for (int i8 = 0; i8 < 4; ++i8) {
        if (mb.transform_8x8 && m_cabac_mode) {
            if (!(cbp_luma & (1 << i8)))
                continue;
            bool coded = false;
            if (!cabac_block(list.data(), 0, 63, 64, cat[3], chroma444 ? cbf_inc_8x8(k, i8) : -1, coded))
                return false;
            std::int32_t* out = coeffs + i8 * 64;
            bool any = false;
            for (int i = 0; i < 64; ++i) {
                out[m_scan8[i]] = list[static_cast<std::size_t>(i)];
                any |= list[static_cast<std::size_t>(i)] != 0;
            }
            int const bx = (i8 & 1) * 2, by = (i8 >> 1) * 2;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    int const r = (by + dy) * 4 + bx + dx;
                    if (coded)
                        s.cbf |= std::uint64_t { 1 } << (bit0 + r);
                    if (any && k == 0)
                        s.nonzero |= 1u << r;
                    s.total_coeff[static_cast<std::size_t>(bit0 + r)] = any ? 1 : 0;
                }
            continue;
        }
        std::array<std::int32_t, 64> eight {};
        bool any8 = false;
        for (int i4 = 0; i4 < 4; ++i4) {
            int const blk = i8 * 4 + i4;
            int const r = blk_to_raster[blk];
            int const x4 = r & 3, y4 = r >> 2;
            if (!(cbp_luma & (1 << i8)))
                continue;
            int total = 0;
            bool coded = false;
            if (mb.kind == MbKind::I16x16) {
                if (m_cabac_mode) {
                    if (!cabac_block(list.data(), 0, 14, 15, cat[1], cbf_inc_luma(k, x4, y4), coded))
                        return false;
                    total = coded ? 1 : 0;
                } else if (!cavlc_block(list.data(), 0, 14, 15, nc_luma(k, x4, y4), total)) {
                    return false;
                }
                put4x4(coeffs + r * 16, list.data(), 1, 15);
            } else if (!mb.transform_8x8) {
                if (m_cabac_mode) {
                    if (!cabac_block(list.data(), 0, 15, 16, cat[2], cbf_inc_luma(k, x4, y4), coded))
                        return false;
                    total = coded ? 1 : 0;
                } else if (!cavlc_block(list.data(), 0, 15, 16, nc_luma(k, x4, y4), total)) {
                    return false;
                }
                put4x4(coeffs + r * 16, list.data(), 0, 16);
            } else {
                // CAVLC 8x8: four interleaved 4x4 lists.
                if (!cavlc_block(list.data(), 0, 15, 16, nc_luma(k, x4, y4), total))
                    return false;
                for (int i = 0; i < 16; ++i)
                    eight[static_cast<std::size_t>(4 * i + i4)] = list[static_cast<std::size_t>(i)];
            }
            bool any = false;
            for (int i = 0; i < (mb.kind == MbKind::I16x16 ? 15 : 16); ++i)
                any |= list[static_cast<std::size_t>(i)] != 0;
            any8 |= any;
            s.total_coeff[static_cast<std::size_t>(bit0 + r)] = static_cast<std::uint8_t>(m_cabac_mode ? (any ? 1 : 0) : total);
            if (m_cabac_mode && coded)
                s.cbf |= std::uint64_t { 1 } << (bit0 + r);
            if (any && !mb.transform_8x8 && k == 0)
                s.nonzero |= 1u << r;
        }
        if (mb.transform_8x8 && !m_cabac_mode && (cbp_luma & (1 << i8))) {
            std::int32_t* out = coeffs + i8 * 64;
            for (int i = 0; i < 64; ++i)
                out[m_scan8[i]] = eight[static_cast<std::size_t>(i)];
            if (any8 && k == 0) {
                int const bx = (i8 & 1) * 2, by = (i8 >> 1) * 2;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        s.nonzero |= 1u << ((by + dy) * 4 + bx + dx);
            }
        }
    }
    return true;
}

bool Parser::macroblock_layer()
{
    MbState& s = current();
    s = MbState {};
    s.slice = m_slice.number;
    s.field = m_field_mb;
    s.intra4x4_modes.fill(2);
    int const storage = m_mbaff ? m_frame.storage_mb(m_addr) : m_frame.storage_mb_at(m_x, m_y);
    m_frame.picture->mb_field[static_cast<std::size_t>(storage)] = m_field_mb ? 1 : 0;
    m_p8x8ref0 = false;
    MbSyntax& mb = m_frame.sink->begin(m_addr);
    mb.ref_idx[0] = { -1, -1, -1, -1 };
    mb.ref_idx[1] = { -1, -1, -1, -1 };
    int raw = 0;
    if (!decode_mb_type(mb, raw))
        return false;
    if (!m_cabac_mode && m_slice.type == H264SliceType::P && raw == 4)
        m_p8x8ref0 = true;
    s.kind = mb.kind;
    s.intra = !is_inter(mb.kind);
    s.direct_16x16 = mb.kind == MbKind::BDirect16x16;
    if (mb.kind == MbKind::IPCM) {
        if (m_cabac_mode)
            m_cabac.hand_back(m_reader);
        while (!m_reader.byte_aligned())
            m_reader.bit();
        // 7.3.5: pcm_sample_luma and pcm_sample_chroma at their bit depths.
        int const luma_bits = m_frame.bit_depth_luma, chroma_bits = m_frame.bit_depth_chroma;
        int const chroma_samples = 2 * m_frame.mb_chroma_width * m_frame.mb_chroma_height;
        for (int i = 0; i < 256; ++i)
            mb.pcm[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(m_reader.bits(luma_bits));
        for (int i = 0; i < chroma_samples; ++i)
            mb.pcm[static_cast<std::size_t>(256 + i)] = static_cast<std::uint16_t>(m_reader.bits(chroma_bits));
        if (m_cabac_mode)
            m_cabac.init_engine(m_reader);
        // The deblocking filter takes an I_PCM macroblock's QP as 0 (8.7.2.2);
        // the next macroblock's QPY,PRED is still the one before it.
        s.qp = 0;
        s.cbp = 0x2f;
        s.cbf = ~std::uint64_t { 0 };
        s.total_coeff.fill(16);
        s.nonzero = 0xffff;
        m_prev_qp_delta_nonzero = false;
        m_prev_addr = m_addr;
        clear_intra_motion(m_frame, m_addr);
        m_frame.sink->end(m_slice, m_addr);
        return !m_reader.overrun();
    }
    bool no_sub_less_than_8x8 = true;
    if (mb.kind == MbKind::P8x8 || mb.kind == MbKind::B8x8) {
        if (!sub_mb_pred(mb))
            return false;
        for (int i = 0; i < 4; ++i) {
            SubMbInfo const info = sub_mb_info(m_slice.type, mb.sub_type[static_cast<std::size_t>(i)]);
            if (info.pred == PredDirect) {
                if (!m_sps.direct_8x8_inference)
                    no_sub_less_than_8x8 = false;
            } else if (info.parts > 1) {
                no_sub_less_than_8x8 = false;
            }
        }
    } else {
        if (m_pps.transform_8x8_mode && mb.kind == MbKind::I4x4) {
            bool t;
            if (m_cabac_mode) {
                auto cond = [&](int addr) { return addr >= 0 && state(addr).transform_8x8 ? 1 : 0; };
                t = m_cabac.decision(399 + cond(mb_a()) + cond(mb_b())) != 0;
            } else {
                t = m_reader.bit() != 0;
            }
            if (t) {
                mb.kind = MbKind::I8x8;
                s.kind = MbKind::I8x8;
                mb.transform_8x8 = true;
                s.transform_8x8 = true;
            }
        }
        if (!mb_pred(mb))
            return false;
    }
    if (mb.kind != MbKind::I16x16) {
        bool const has_chroma_dc = m_frame.chroma_format == 1 || m_frame.chroma_format == 2;
        int cbp;
        if (m_cabac_mode) {
            // 9.3.2.6 and 9.3.3.1.1.4: four luma bins, then up to two chroma.
            cbp = 0;
            for (int b8 = 0; b8 < 4; ++b8) {
                int const bx = b8 & 1, by = b8 >> 1;
                auto cond = [&](int addr, int nb8, bool in_current) {
                    if (in_current)
                        return ((cbp >> nb8) & 1) ? 0 : 1;
                    if (addr < 0)
                        return 0;
                    MbState const& n = state(addr);
                    if (n.kind == MbKind::IPCM)
                        return 0;
                    if (!n.skip && ((n.cbp >> nb8) & 1))
                        return 0;
                    return 1;
                };
                // 6.4.11.2: the 8x8 blocks left of and above this one.
                auto outside = [&](int x, int y) {
                    if (!m_mbaff)
                        return x < 0 ? cond(mb_a(), b8 + 1, false) : cond(mb_b(), b8 + 2, false);
                    Frame::Location const n = neighbor(x, y);
                    return cond(n.addr, (n.y >> 3) * 2 + (n.x >> 3), false);
                };
                int const a = bx > 0 ? cond(m_addr, b8 - 1, true) : outside(-1, by * 8);
                int const b = by > 0 ? cond(m_addr, b8 - 2, true) : outside(bx * 8, -1);
                cbp |= m_cabac.decision(73 + a + 2 * b) << b8;
            }
            auto ccond = [&](int addr, int bin) {
                if (addr < 0)
                    return 0;
                MbState const& n = state(addr);
                if (n.kind == MbKind::IPCM)
                    return 1;
                if (n.skip)
                    return 0;
                int const c = n.cbp >> 4;
                return bin == 0 ? (c != 0 ? 1 : 0) : (c == 2 ? 1 : 0);
            };
            // 9.3.2.6: no chroma bins where ChromaArrayType is 0 or 3.
            int chroma = 0;
            if (has_chroma_dc && m_cabac.decision(77 + ccond(mb_a(), 0) + 2 * ccond(mb_b(), 0)))
                chroma = 1 + m_cabac.decision(77 + 4 + ccond(mb_a(), 1) + 2 * ccond(mb_b(), 1));
            cbp |= chroma << 4;
        } else if (has_chroma_dc) {
            std::uint32_t const code = m_reader.ue();
            if (code > 47)
                return false;
            cbp = s.intra ? cbp_intra[code] : cbp_inter[code];
        } else {
            // Table 9-4 (b), ChromaArrayType 0 or 3: luma only.
            static constexpr std::uint8_t intra_luma[16] = { 15, 0, 7, 11, 13, 14, 3, 5, 10, 12, 1, 2, 4, 8, 6, 9 };
            static constexpr std::uint8_t inter_luma[16] = { 0, 1, 2, 4, 8, 3, 5, 10, 12, 15, 7, 11, 13, 14, 6, 9 };
            std::uint32_t const code = m_reader.ue();
            if (code > 15)
                return false;
            cbp = s.intra ? intra_luma[code] : inter_luma[code];
        }
        mb.cbp = cbp;
        if ((cbp & 15) && m_pps.transform_8x8_mode && s.intra == false && no_sub_less_than_8x8
            && (mb.kind != MbKind::BDirect16x16 || m_sps.direct_8x8_inference)) {
            bool t;
            if (m_cabac_mode) {
                auto cond = [&](int addr) { return addr >= 0 && state(addr).transform_8x8 ? 1 : 0; };
                t = m_cabac.decision(399 + cond(mb_a()) + cond(mb_b())) != 0;
            } else {
                t = m_reader.bit() != 0;
            }
            mb.transform_8x8 = t;
            s.transform_8x8 = t;
        }
    }
    s.cbp = mb.cbp;
    bool const has_residual = (mb.cbp & 15) || (mb.cbp >> 4) || mb.kind == MbKind::I16x16;
    if (has_residual) {
        int delta;
        if (m_cabac_mode)
            delta = cabac_unary_mb_qp_delta();
        else
            delta = m_reader.se();
        // 7.4.5: mb_qp_delta's range, and QPY wrapping in -QpBdOffsetY..51 (7-37).
        int const bd_offset = m_frame.qp_offset_luma;
        if (delta < -(26 + bd_offset / 2) || delta > 25 + bd_offset / 2)
            return false;
        mb.qp_delta = delta;
        m_qp = ((m_qp + delta + 52 + 2 * bd_offset) % (52 + bd_offset)) - bd_offset;
        m_prev_qp_delta_nonzero = delta != 0;
    } else {
        m_prev_qp_delta_nonzero = false;
    }
    s.qp = m_qp;
    if (is_inter(mb.kind))
        derive_motion(m_frame, m_slice, m_addr, mb);
    if (has_residual && !residual(mb))
        return false;
    m_prev_addr = m_addr;
    if (s.intra)
        clear_intra_motion(m_frame, m_addr);
    m_frame.sink->end(m_slice, m_addr);
    return !m_reader.overrun();
}

}

bool decode_slice_data(Frame& frame, Slice const& slice, BitReader& reader)
{
    Parser parser(frame, slice, reader);
    return parser.run();
}

}
