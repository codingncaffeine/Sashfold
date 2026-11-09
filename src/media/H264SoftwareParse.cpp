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

// 9.3.1.1.
void Cabac::init_contexts(int slice_qp, int kind)
{
    int const qp = std::clamp(slice_qp, 0, 51);
    for (int i = 0; i < 1024; ++i) {
        CabacInit const& c = cabac_init[i][kind];
        int const pre = std::clamp(((c.m * qp) >> 4) + c.n, 1, 126);
        if (pre <= 63) {
            m_state[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(63 - pre);
            m_mps[static_cast<std::size_t>(i)] = 0;
        } else {
            m_state[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pre - 64);
            m_mps[static_cast<std::size_t>(i)] = 1;
        }
    }
    // ctxIdx 276 does not adapt (end_of_slice_flag and the I_PCM bin).
    m_state[276] = 63;
    m_mps[276] = 0;
}

// 9.3.1.2.
void Cabac::init_engine(BitReader& reader)
{
    m_data = reader.data();
    m_size = reader.size();
    m_next = reader.position() / 8;
    m_range = 510;
    m_low = 0;
    m_bits = -9; // codIOffset is the first nine bits
    refill();
}

// Two more bytes below m_low (zeros past the end; overrun() tells).
void Cabac::refill()
{
    for (int i = 0; i < 2; ++i, ++m_next)
        m_low = (m_low << 8) | (m_next < m_size ? m_data[m_next] : 0u);
    m_bits += 16;
}

void Cabac::renormalize()
{
    // As many doublings as take the range back to nine bits, in one step.
    if (m_range < 256) {
        int const shift = std::countl_zero(m_range) - 23;
        m_range <<= shift;
        if (m_bits < shift)
            refill();
        m_bits -= shift;
    }
}

// 9.3.3.2.1.
int Cabac::decision(int ctx_idx)
{
    std::size_t const c = static_cast<std::size_t>(ctx_idx);
    std::uint8_t const state = m_state[c];
    std::uint32_t const lps = range_tab_lps[state][(m_range >> 6) & 3];
    m_range -= lps;
    int bin;
    std::uint32_t const scaled = m_range << m_bits;
    if (m_low >= scaled) {
        bin = m_mps[c] ^ 1;
        m_low -= scaled;
        m_range = lps;
        if (state == 0)
            m_mps[c] ^= 1;
        m_state[c] = trans_idx_lps[state];
    } else {
        bin = m_mps[c];
        m_state[c] = trans_idx_mps[state];
    }
    renormalize();
    return bin;
}

// 9.3.3.2.3.
int Cabac::bypass()
{
    if (m_bits < 1)
        refill();
    --m_bits;
    std::uint32_t const scaled = m_range << m_bits;
    if (m_low >= scaled) {
        m_low -= scaled;
        return 1;
    }
    return 0;
}

// 9.3.3.2.4.
int Cabac::terminate()
{
    m_range -= 2;
    if (m_low >= (m_range << m_bits))
        return 1;
    renormalize();
    return 0;
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
    {
    }

    bool run();

private:
    // Neighbors (6.4.9, frames only).
    bool available(int addr) const { return addr >= 0 && m_frame.mbs[static_cast<std::size_t>(addr)].slice == m_slice.number; }
    int mb_a() const { return m_x > 0 && available(m_addr - 1) ? m_addr - 1 : -1; }
    int mb_b() const { return m_y > 0 && available(m_addr - m_width) ? m_addr - m_width : -1; }
    MbState const& state(int addr) const { return m_frame.mbs[static_cast<std::size_t>(addr)]; }
    MbState& current() { return m_frame.mbs[static_cast<std::size_t>(m_addr)]; }
    // The 4x4 luma block left of / above raster block (x4, y4) of the
    // current macroblock: its macroblock (-1: none) and raster index.
    std::pair<int, int> left4(int x4, int y4) const
    {
        if (x4 > 0)
            return { m_addr, y4 * 4 + x4 - 1 };
        int const a = mb_a();
        return { a, y4 * 4 + 3 };
    }
    std::pair<int, int> above4(int x4, int y4) const
    {
        if (y4 > 0)
            return { m_addr, (y4 - 1) * 4 + x4 };
        int const b = mb_b();
        return { b, 12 + x4 };
    }

    bool macroblock_layer();
    void skip_macroblock();
    bool mb_pred(MbSyntax& mb);
    bool sub_mb_pred(MbSyntax& mb);
    bool residual(MbSyntax& mb);
    bool decode_mb_type(MbSyntax& mb, int& raw);
    void set_mb_type(MbSyntax& mb, int raw, H264SliceType as);

    // CAVLC.
    int read_vlc(Vlc const& v, int& a, int& b);
    bool cavlc_block(std::int32_t* levels, int start, int end, int max_coeff, int nc, int& total);
    int nc_luma(int x4, int y4) const;
    int nc_chroma(int c, int x2, int y2) const;
    // CABAC.
    bool cabac_block(std::int32_t* levels, int start, int end, int max_coeff, int cat, int cbf_inc, bool& coded);
    int cbf_inc_luma(int cat, int x4, int y4) const;
    int cbf_inc_dc(int cat, int c) const;
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
    Cabac m_cabac;
    int m_addr = 0;
    int m_x = 0;
    int m_y = 0;
    int m_qp = 26; // QPY of the previous macroblock in the slice
    int m_prev_addr = -1; // the previous macroblock in decoding order, in this slice
    bool m_prev_qp_delta_nonzero = false;
    bool m_p8x8ref0 = false; // CAVLC's P_8x8ref0: every reference index 0, none coded
};

// Static VLC tables built once from the generated data.
struct VlcSet {
    Vlc coeff[6];
    Vlc tz4x4[15];
    Vlc tz2x2[3];
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

// 9.2.1: nC of a luma 4x4 block from its neighbors' total coefficients.
int Parser::nc_luma(int x4, int y4) const
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
        return static_cast<int>(s.total_coeff[static_cast<std::size_t>(n.second)]);
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
        return static_cast<int>(s.total_coeff[static_cast<std::size_t>(16 + c * 4 + idx)]);
    };
    bool ha = false, hb = false;
    int const na = x2 > 0 ? n_of(m_addr, y2 * 2, ha) : n_of(mb_a(), y2 * 2 + 1, ha);
    int const nb = y2 > 0 ? n_of(m_addr, x2, hb) : n_of(mb_b(), 2 + x2, hb);
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
        int const value = std::clamp((code % 2 == 0) ? (code + 2) >> 1 : (-code - 1) >> 1, -32768, 32767);
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

int Parser::cbf_inc_luma(int cat, int x4, int y4) const
{
    auto cond = [&](std::pair<int, int> n) {
        if (n.first < 0)
            return cbf_cond(-1, false, false);
        MbState const& s = n.first == m_addr ? m_frame.mbs[static_cast<std::size_t>(m_addr)] : state(n.first);
        int const b8 = (n.second >> 3) * 2 + ((n.second & 3) >> 1);
        bool const has = s.kind != MbKind::IPCM && !s.skip && ((s.cbp >> b8) & 1);
        (void)cat;
        return cbf_cond(n.first, has, (s.cbf >> n.second) & 1);
    };
    return cond(left4(x4, y4)) + 2 * cond(above4(x4, y4));
}

int Parser::cbf_inc_dc(int cat, int c) const
{
    int const bit = cat == 0 ? 24 : 25 + c;
    auto cond = [&](int addr) {
        if (addr < 0)
            return cbf_cond(-1, false, false);
        MbState const& s = state(addr);
        bool const has = cat == 0 ? s.kind == MbKind::I16x16 : (!s.skip && s.kind != MbKind::IPCM && (s.cbp >> 4) != 0);
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
        return cbf_cond(addr, has, (s.cbf >> (16 + c * 4 + idx)) & 1);
    };
    int const a = x2 > 0 ? cond(m_addr, y2 * 2) : cond(mb_a(), y2 * 2 + 1);
    int const b = y2 > 0 ? cond(m_addr, x2) : cond(mb_b(), 2 + x2);
    return a + 2 * b;
}

// 7.3.5.3.3 with 9.3.3.1.1.9 and 9.3.3.1.3: residual_block_cabac.
bool Parser::cabac_block(std::int32_t* levels, int start, int end, int max_coeff, int cat, int cbf_inc, bool& coded)
{
    for (int i = start; i <= end; ++i)
        levels[i] = 0;
    // Table 9-40: ctxIdxBlockCatOffset of cbf, significance and levels.
    static constexpr int cbf_off[6] = { 0, 4, 8, 12, 16, 0 };
    static constexpr int sig_off[6] = { 0, 15, 29, 44, 47, 0 };
    static constexpr int abs_off[6] = { 0, 10, 20, 30, 39, 0 };
    if (max_coeff != 64) {
        coded = m_cabac.decision(85 + cbf_off[cat] + cbf_inc) != 0;
        if (!coded)
            return true;
    } else {
        coded = true;
    }
    int const count = end - start + 1;
    std::array<bool, 64> significant {};
    int num = count;
    for (int i = 0; i < num - 1; ++i) {
        int sig_ctx, last_ctx;
        if (cat == 5) {
            sig_ctx = 402 + significant_8x8_frame[i];
            last_ctx = 417 + last_significant_8x8[i];
        } else {
            int const inc = cat == 3 ? std::min(i, 2) : i;
            sig_ctx = 105 + sig_off[cat] + inc;
            last_ctx = 166 + sig_off[cat] + inc;
        }
        if (m_cabac.decision(sig_ctx)) {
            significant[static_cast<std::size_t>(i)] = true;
            if (m_cabac.decision(last_ctx)) {
                num = i + 1;
                break;
            }
        }
    }
    significant[static_cast<std::size_t>(num - 1)] = true;
    int eq1 = 0, gt1 = 0;
    int const base = cat == 5 ? 426 : 227 + abs_off[cat];
    for (int i = num - 1; i >= 0; --i) {
        if (!significant[static_cast<std::size_t>(i)])
            continue;
        // coeff_abs_level_minus1: TU prefix (cMax 14) with contexts, then Exp-Golomb k = 0 bypass.
        int inc0 = gt1 != 0 ? 0 : std::min(4, 1 + eq1);
        int value = 0;
        if (m_cabac.decision(base + inc0)) {
            int const inc = 5 + std::min(4 - (cat == 3 ? 1 : 0), gt1);
            value = 1;
            while (value < 14 && m_cabac.decision(base + inc))
                ++value;
            if (value == 14) {
                int k = 0;
                while (m_cabac.bypass()) {
                    value += 1 << k;
                    if (++k > 16) // past any level a stream may hold (7.4.5.3.3)
                        return false;
                }
                while (k-- > 0)
                    value += m_cabac.bypass() << k;
            }
        }
        int const level = std::min(value + 1, 32768);
        if (level == 1)
            ++eq1;
        else
            ++gt1;
        levels[start + i] = m_cabac.bypass() ? -level : level;
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
        return s.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(b8)] > 0 ? 1 : 0;
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
        return comp == 0 ? static_cast<int>(m.x) : static_cast<int>(m.y);
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
        out[zigzag4x4[start + i]] = list[i];
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
    m_addr = m_header.first_mb_in_slice;
    bool more = true;
    int skip_run = -1;
    while (more) {
        if (m_addr >= total)
            return false;
        m_x = m_addr % m_width;
        m_y = m_addr / m_width;
        if (m_slice.type != H264SliceType::I) {
            if (!m_cabac_mode) {
                if (skip_run < 0) {
                    skip_run = static_cast<int>(m_reader.ue());
                    if (m_reader.overrun())
                        return false;
                }
                if (skip_run > 0) {
                    --skip_run;
                    skip_macroblock();
                    ++m_addr;
                    if (skip_run == 0) {
                        more = m_reader.more_rbsp_data();
                        if (!more)
                            break;
                    }
                    continue;
                }
                skip_run = -1;
            } else {
                auto cond = [&](int addr) { return addr >= 0 && !state(addr).skip ? 1 : 0; };
                int const base = m_slice.type == H264SliceType::B ? 24 : 11;
                // the macroblock is not yet marked; its neighbors decide
                m_frame.mbs[static_cast<std::size_t>(m_addr)].slice = -1;
                bool const skipped = m_cabac.decision(base + cond(mb_a()) + cond(mb_b())) != 0;
                if (skipped) {
                    skip_macroblock();
                    more = !m_cabac.terminate();
                    ++m_addr;
                    continue;
                }
            }
        }
        if (!macroblock_layer())
            return false;
        if (!m_cabac_mode)
            more = m_reader.more_rbsp_data();
        else
            more = !m_cabac.terminate();
        if (m_reader.overrun() || (m_cabac_mode && m_cabac.overrun()))
            return false;
        ++m_addr;
    }
    return true;
}

void Parser::skip_macroblock()
{
    MbState& s = current();
    s = MbState {};
    s.slice = m_slice.number;
    s.skip = true;
    bool const b = m_slice.type == H264SliceType::B;
    s.kind = b ? MbKind::BSkip : MbKind::PSkip;
    s.direct_16x16 = b;
    s.qp = m_qp;
    s.intra4x4_modes.fill(2);
    MbSyntax& mb = m_frame.sink->begin(m_addr);
    mb.kind = s.kind;
    mb.part_pred[0] = b ? PredDirect : PredL0;
    m_prev_addr = m_addr;
    m_prev_qp_delta_nonzero = false;
    derive_motion(m_frame, m_slice, m_addr, mb);
    m_frame.sink->end(m_slice, m_addr);
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
                    // 8-72: the 4x4 block n = 1 (A) or 2 (B) of the 8x8 block holding the neighbor.
                    int const nx = nb.second & 3, ny = nb.second >> 2;
                    int const bx = (nx >> 1) * 2, by = (ny >> 1) * 2;
                    int const pick = is_a ? by * 4 + bx + 1 : (by + 1) * 4 + bx;
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
        int const active = list == 0 ? m_header.num_ref_idx_l0_active : m_header.num_ref_idx_l1_active;
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
        int const active = list == 0 ? m_header.num_ref_idx_l0_active : m_header.num_ref_idx_l1_active;
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

bool Parser::residual(MbSyntax& mb)
{
    MbState& s = current();
    int const cbp_luma = mb.cbp & 15;
    int const cbp_chroma = mb.cbp >> 4;
    std::array<std::int32_t, 64> list {};
    if (mb.kind == MbKind::I16x16) {
        int total = 0;
        bool coded = false;
        if (m_cabac_mode) {
            if (!cabac_block(list.data(), 0, 15, 16, 0, cbf_inc_dc(0, 0), coded))
                return false;
            if (coded)
                s.cbf |= 1u << 24;
        } else if (!cavlc_block(list.data(), 0, 15, 16, nc_luma(0, 0), total)) {
            return false;
        }
        put4x4(mb.luma_dc.data(), list.data(), 0, 16);
    }
    for (int i8 = 0; i8 < 4; ++i8) {
        if (mb.transform_8x8 && m_cabac_mode) {
            if (!(cbp_luma & (1 << i8)))
                continue;
            bool coded = false;
            if (!cabac_block(list.data(), 0, 63, 64, 5, 0, coded))
                return false;
            std::int32_t* out = mb.luma.data() + i8 * 64;
            bool any = false;
            for (int i = 0; i < 64; ++i) {
                out[zigzag8x8[i]] = list[static_cast<std::size_t>(i)];
                any |= list[static_cast<std::size_t>(i)] != 0;
            }
            int const bx = (i8 & 1) * 2, by = (i8 >> 1) * 2;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    int const r = (by + dy) * 4 + bx + dx;
                    s.cbf |= 1u << r;
                    if (any)
                        s.nonzero |= 1u << r;
                    s.total_coeff[static_cast<std::size_t>(r)] = any ? 1 : 0;
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
                    if (!cabac_block(list.data(), 0, 14, 15, 1, cbf_inc_luma(1, x4, y4), coded))
                        return false;
                    total = coded ? 1 : 0;
                } else if (!cavlc_block(list.data(), 0, 14, 15, nc_luma(x4, y4), total)) {
                    return false;
                }
                put4x4(mb.luma.data() + r * 16, list.data(), 1, 15);
            } else if (!mb.transform_8x8) {
                if (m_cabac_mode) {
                    if (!cabac_block(list.data(), 0, 15, 16, 2, cbf_inc_luma(2, x4, y4), coded))
                        return false;
                    total = coded ? 1 : 0;
                } else if (!cavlc_block(list.data(), 0, 15, 16, nc_luma(x4, y4), total)) {
                    return false;
                }
                put4x4(mb.luma.data() + r * 16, list.data(), 0, 16);
            } else {
                // CAVLC 8x8: four interleaved 4x4 lists.
                if (!cavlc_block(list.data(), 0, 15, 16, nc_luma(x4, y4), total))
                    return false;
                for (int i = 0; i < 16; ++i)
                    eight[static_cast<std::size_t>(4 * i + i4)] = list[static_cast<std::size_t>(i)];
            }
            bool any = false;
            for (int i = 0; i < (mb.kind == MbKind::I16x16 ? 15 : 16); ++i)
                any |= list[static_cast<std::size_t>(i)] != 0;
            any8 |= any;
            s.total_coeff[static_cast<std::size_t>(r)] = static_cast<std::uint8_t>(m_cabac_mode ? (any ? 1 : 0) : total);
            if (m_cabac_mode && coded)
                s.cbf |= 1u << r;
            if (any && !mb.transform_8x8)
                s.nonzero |= 1u << r;
        }
        if (mb.transform_8x8 && !m_cabac_mode && (cbp_luma & (1 << i8))) {
            std::int32_t* out = mb.luma.data() + i8 * 64;
            for (int i = 0; i < 64; ++i)
                out[zigzag8x8[i]] = eight[static_cast<std::size_t>(i)];
            if (any8) {
                int const bx = (i8 & 1) * 2, by = (i8 >> 1) * 2;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        s.nonzero |= 1u << ((by + dy) * 4 + bx + dx);
            }
        }
    }
    if (cbp_chroma & 3) {
        for (int c = 0; c < 2; ++c) {
            int total = 0;
            bool coded = false;
            if (m_cabac_mode) {
                if (!cabac_block(list.data(), 0, 3, 4, 3, cbf_inc_dc(3, c), coded))
                    return false;
                if (coded)
                    s.cbf |= 1u << (25 + c);
            } else if (!cavlc_block(list.data(), 0, 3, 4, -1, total)) {
                return false;
            }
            for (int i = 0; i < 4; ++i)
                mb.chroma_dc[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = list[static_cast<std::size_t>(i)];
        }
    }
    if (cbp_chroma & 2) {
        for (int c = 0; c < 2; ++c) {
            for (int b4 = 0; b4 < 4; ++b4) {
                int const x2 = b4 & 1, y2 = b4 >> 1;
                int total = 0;
                bool coded = false;
                if (m_cabac_mode) {
                    if (!cabac_block(list.data(), 0, 14, 15, 4, cbf_inc_chroma_ac(c, x2, y2), coded))
                        return false;
                    if (coded)
                        s.cbf |= 1u << (16 + c * 4 + b4);
                    total = coded ? 1 : 0;
                } else if (!cavlc_block(list.data(), 0, 14, 15, nc_chroma(c, x2, y2), total)) {
                    return false;
                }
                s.total_coeff[static_cast<std::size_t>(16 + c * 4 + b4)] = static_cast<std::uint8_t>(total);
                put4x4(mb.chroma_ac[static_cast<std::size_t>(c)].data() + b4 * 16, list.data(), 1, 15);
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
    s.intra4x4_modes.fill(2);
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
        for (std::uint8_t& v : mb.pcm_luma)
            v = static_cast<std::uint8_t>(m_reader.bits(8));
        for (std::uint8_t& v : mb.pcm_chroma)
            v = static_cast<std::uint8_t>(m_reader.bits(8));
        if (m_cabac_mode)
            m_cabac.init_engine(m_reader);
        // The deblocking filter takes an I_PCM macroblock's QP as 0 (8.7.2.2);
        // the next macroblock's QPY,PRED is still the one before it.
        s.qp = 0;
        s.cbp = 0x2f;
        s.cbf = 0x07ffffff;
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
                int const a = bx > 0 ? cond(m_addr, b8 - 1, true) : cond(mb_a(), b8 + 1, false);
                int const b = by > 0 ? cond(m_addr, b8 - 2, true) : cond(mb_b(), b8 + 2, false);
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
            int chroma = 0;
            if (m_cabac.decision(77 + ccond(mb_a(), 0) + 2 * ccond(mb_b(), 0)))
                chroma = 1 + m_cabac.decision(77 + 4 + ccond(mb_a(), 1) + 2 * ccond(mb_b(), 1));
            cbp |= chroma << 4;
        } else {
            std::uint32_t const code = m_reader.ue();
            if (code > 47)
                return false;
            cbp = s.intra ? cbp_intra[code] : cbp_inter[code];
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
        if (delta < -26 || delta > 25)
            return false;
        mb.qp_delta = delta;
        m_qp = (m_qp + delta + 52) % 52;
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
