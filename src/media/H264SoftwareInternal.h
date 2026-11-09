#pragma once

// The software H.264 decoder's own types, shared by its parts: the slice
// data's bit reader and CABAC engine (9.3), the decoded pictures with the
// motion each keeps for the pictures after it, the state of each
// macroblock, and the functions of each stage —
//   H264SoftwareParse.cpp    the macroblock layer, CAVLC and CABAC (7.3.5, 9.2, 9.3)
//   H264SoftwarePredict.cpp  intra prediction (8.3), motion vectors (8.4.1), inter prediction (8.4.2)
//   H264SoftwareResidual.cpp scaling and the inverse transforms (8.5)
//   H264SoftwareDeblock.cpp  the deblocking filter (8.7)
//   H264Software.cpp         a picture's slices, and the accelerator around them
// Frames only, 8 bits, 4:2:0 — what the profiles the web serves carry.

#include "media/H264.h"
#include "media/H264Accelerator.h"
#include "media/H264SoftwareResidual.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sashfold::media::h264sw {

// Bits of an RBSP, read most significant first; past the end reads zeros
// and marks the reader as overrun.
class BitReader {
public:
    BitReader() = default;
    BitReader(std::uint8_t const* data, std::size_t size, std::size_t bit_position)
        : m_data(data)
        , m_size(size)
        , m_position(bit_position)
    {
    }
    std::uint32_t bit()
    {
        if (m_position >= m_size * 8) {
            m_overrun = true;
            ++m_position;
            return 0;
        }
        std::uint32_t const b = (m_data[m_position >> 3] >> (7 - (m_position & 7))) & 1;
        ++m_position;
        return b;
    }
    std::uint32_t bits(int n)
    {
        // Up to 25 bits from one big-endian word where four bytes remain.
        if (n <= 25 && (m_position >> 3) + 4 <= m_size) {
            std::uint8_t const* p = m_data + (m_position >> 3);
            std::uint32_t const word = (std::uint32_t { p[0] } << 24) | (std::uint32_t { p[1] } << 16) | (std::uint32_t { p[2] } << 8) | p[3];
            m_position += static_cast<std::size_t>(n);
            return n == 0 ? 0 : (word << ((m_position - static_cast<std::size_t>(n)) & 7)) >> (32 - n);
        }
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }
    // The next n bits (n <= 16) without taking them.
    std::uint32_t peek(int n) const
    {
        std::uint32_t v = 0;
        std::size_t p = m_position;
        for (int i = 0; i < n; ++i, ++p)
            v = (v << 1) | (p < m_size * 8 ? (m_data[p >> 3] >> (7 - (p & 7))) & 1u : 0u);
        return v;
    }
    void skip(int n) { m_position += static_cast<std::size_t>(n); }
    // ue(v); a code of 31 leading zeros or more (2^31 - 1 and up) is no
    // value slice data carries, so it marks the data broken and every
    // value read fits an int.
    std::uint32_t ue()
    {
        int zeros = 0;
        while (bit() == 0) {
            if (++zeros >= 31 || m_overrun) {
                m_overrun = true;
                return 0;
            }
        }
        return ((1u << zeros) - 1) + bits(zeros);
    }
    std::int32_t se()
    {
        std::uint32_t const k = ue();
        return (k & 1) ? static_cast<std::int32_t>((k + 1) / 2) : -static_cast<std::int32_t>(k / 2);
    }
    // te(v) with a range of 0 to max (7.2): one inverted bit when max is 1.
    std::uint32_t te(int max) { return max > 1 ? ue() : (bit() ^ 1u); }
    bool byte_aligned() const { return (m_position & 7) == 0; }
    std::size_t position() const { return m_position; }
    bool overrun() const { return m_overrun || m_position > m_size * 8; }
    // 7.2 more_rbsp_data(): data before the stop bit.
    bool more_rbsp_data() const;
    std::uint8_t const* data() const { return m_data; }
    std::size_t size() const { return m_size; }
    // Where the arithmetic decoder hands the data back (I_PCM's samples
    // follow it): its read position, and whether it read past the end.
    void resume_at(std::size_t bit_position, bool overrun)
    {
        m_position = bit_position;
        m_overrun = m_overrun || overrun;
    }

private:
    std::uint8_t const* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_position = 0;
    bool m_overrun = false;
};

// 9.3.1.2 and 9.3.3.2: the arithmetic decoding engine, its 1024 contexts.
// The engine reads its data two bytes ahead: m_low holds codIOffset
// followed by m_bits bits not yet taken, so codIOffset >= codIRange is
// m_low >= codIRange << m_bits, and RenormD only moves m_bits.
class Cabac {
public:
    void init_contexts(int slice_qp, int kind); // kind: 0 for I slices, 1 + cabac_init_idc
    // From the reader's position, which must be byte-aligned (9.3.1.2).
    void init_engine(BitReader& reader);
    int decision(int ctx_idx);
    int bypass();
    int terminate();
    // The bits the engine has taken (9.3.3.2.2.3 has nothing more to read
    // after a terminate of 1), given back to the reader.
    void hand_back(BitReader& reader) const { reader.resume_at(consumed(), overrun()); }
    bool overrun() const { return consumed() > m_size * 8; }

private:
    void renormalize();
    void refill();
    std::size_t consumed() const { return m_next * 8 - static_cast<std::size_t>(m_bits); }

    std::uint8_t const* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_next = 0; // the next byte to load
    int m_bits = 0;
    std::uint32_t m_range = 0;
    std::uint32_t m_low = 0;
    std::array<std::uint8_t, 1024> m_state {};
    std::array<std::uint8_t, 1024> m_mps {};
};

struct Mv {
    std::int16_t x = 0;
    std::int16_t y = 0;
    bool operator==(Mv const&) const = default;
};

// A decoded picture: its planes at the coded size, and per 4x4 luma block
// the motion of each list (for the direct prediction of the pictures that
// take it as their colocated one, 8.4.1.2) with the picture each
// reference index named.
struct Picture {
    int width = 0; // coded, in luma samples
    int height = 0;
    int mb_width = 0;
    int mb_height = 0;
    std::vector<std::uint8_t> y, cb, cr;
    std::array<std::vector<Mv>, 2> mv; // [list][block], block = (mb_y * 4 + y4) * (mb_width * 4) + mb_x * 4 + x4
    std::array<std::vector<std::int8_t>, 2> ref_idx; // -1: the list not used
    std::array<std::vector<std::int32_t>, 2> ref_pic; // the serial of the picture the block's reference index named
    std::vector<std::uint8_t> mb_intra; // per macroblock
    std::int32_t serial = 0; // unique per decoded picture
    int poc = 0;
    bool valid = false;
    int crop_x = 0; // the shown rectangle, luma samples
    int crop_y = 0;
    int shown_width = 0;
    int shown_height = 0;

    void allocate(int mb_w, int mb_h);
    int stride() const { return width; }
    int chroma_stride() const { return width / 2; }
    int block_index(int x4, int y4) const { return y4 * mb_width * 4 + x4; }
};

// A reference in a slice's list, as the decoding needs it.
struct RefEntry {
    Picture const* picture = nullptr;
    bool long_term = false;
    int poc = 0;
};

enum class MbKind : std::uint8_t {
    I4x4,
    I8x8,
    I16x16,
    IPCM,
    P16x16,
    P16x8,
    P8x16,
    P8x8,
    PSkip,
    BDirect16x16,
    B16x16,
    B16x8,
    B8x16,
    B8x8,
    BSkip,
};

// Prediction of a partition from list 0, list 1 or both.
enum Pred : std::uint8_t {
    PredNone = 0,
    PredL0 = 1,
    PredL1 = 2,
    PredBi = 3,
    PredDirect = 4,
};

// What each macroblock of the picture being decoded keeps for its
// neighbors' parsing and prediction and for the deblocking.
struct MbState {
    int slice = -1; // the slice it belongs to; -1: not decoded
    MbKind kind = MbKind::PSkip;
    bool intra = false;
    bool skip = false;
    bool transform_8x8 = false;
    bool direct_16x16 = false; // B_Skip and B_Direct_16x16 (their CABAC context)
    int qp = 0; // QPY
    std::array<int, 2> qpc { 0, 0 }; // QP'C of Cb and Cr, for the deblocking (8.7.2.2)
    int cbp = 0;
    int intra_chroma_pred_mode = 0;
    int qp_delta = 0; // mb_qp_delta, for the next macroblock's context
    std::array<std::int8_t, 16> intra4x4_modes {}; // per 4x4 block (an 8x8 mode in each of its four); 2 (DC) when not intra NxN
    std::array<std::uint8_t, 16 + 8> total_coeff {}; // CAVLC's nC: luma 4x4 blocks, then Cb, Cr AC
    std::uint32_t nonzero = 0; // bit per luma 4x4 block with coefficients (8x8: all four), for the deblocking
    // CABAC's coded_block_flag of each block: bit 0..15 luma 4x4, 16..19 Cb AC, 20..23 Cr AC,
    // 24 luma DC, 25 Cb DC, 26 Cr DC.
    std::uint32_t cbf = 0;
    std::array<std::array<Mv, 16>, 2> mvd {}; // absolute mvd per 4x4 block, CABAC's context
    std::array<std::array<std::int8_t, 4>, 2> ref_idx {}; // per 8x8, for CABAC's ref_idx context
    std::array<std::uint8_t, 4> sub_direct {}; // a B_8x8 sub-macroblock predicted directly
};

// A slice as its macroblocks read it.
struct Slice {
    H264Sps const* sps = nullptr;
    H264Pps const* pps = nullptr;
    H264SliceHeader const* header = nullptr;
    int number = 0;
    H264SliceType type = H264SliceType::I;
    int qp = 26; // SliceQPY
    std::array<std::vector<RefEntry>, 2> list;
    // 8.4.2.3: implicit weights, [ref0][ref1] (logWD 5).
    std::vector<std::array<int, 2>> implicit; // index ref0 * 32 + ref1: { w0, w1 }
    int weighted_mode = 0; // 0 default, 1 explicit, 2 implicit
    int current_poc = 0;
    LevelScales const* scales = nullptr;
};

// The macroblock's syntax as parsed, before its samples are made.
struct MbSyntax {
    MbKind kind = MbKind::PSkip;
    int i16_mode = 0;
    int cbp = 0;
    bool transform_8x8 = false;
    std::array<std::int8_t, 16> prev_flag {};
    std::array<std::int8_t, 16> rem_mode {};
    int chroma_mode = 0;
    int qp_delta = 0;
    int partitions = 1;
    std::array<Pred, 4> part_pred {}; // per macroblock partition (16x16: 1, 16x8 / 8x16: 2)
    std::array<int, 4> sub_type {}; // per 8x8: sub_mb_type
    std::array<std::array<std::int8_t, 4>, 2> ref_idx {}; // [list][partition or 8x8]
    std::array<std::array<Mv, 16>, 2> mvd {}; // [list][partition or 8x8 * 4 + sub-partition]
    // Coefficient levels: luma in raster block order of 4x4 (or 8x8 blocks
    // in [i8 * 64]), luma DC, chroma DC and AC.
    std::array<std::int32_t, 256> luma {};
    std::array<std::int32_t, 16> luma_dc {};
    std::array<std::array<std::int32_t, 4>, 2> chroma_dc {};
    std::array<std::array<std::int32_t, 64>, 2> chroma_ac {}; // [cb/cr][block * 16 + i]
    std::array<std::uint8_t, 256> pcm_luma {};
    std::array<std::uint8_t, 128> pcm_chroma {};
};

// Partition shape of the sub_mb_type of a P or B slice (Tables 7-17, 7-18).
struct SubMbInfo {
    int parts; // 1, 2 or 4
    int width; // in luma samples: 8 or 4
    int height;
    Pred pred;
};
SubMbInfo sub_mb_info(H264SliceType type, int sub_mb_type);

// The picture's state while its slices decode.
struct Frame {
    Picture* picture = nullptr;
    std::vector<MbState> mbs;
    std::vector<int> slice_alpha; // per slice number: FilterOffsetA, FilterOffsetB, disable_deblocking_filter_idc
    std::vector<int> slice_beta;
    std::vector<int> slice_disable;
    int chroma_qp_offset[2] = { 0, 0 };
};

// The decoding of one slice's macroblocks, from its data's first bit.
bool decode_slice_data(Frame& frame, Slice const& slice, BitReader& reader);

// The prediction and reconstruction of one macroblock (8.3 to 8.5).
void reconstruct_macroblock(Frame& frame, Slice const& slice, int mb_addr, MbSyntax const& mb);

// The motion of an inter macroblock (8.4.1): fills the picture's motion
// arrays for the macroblock.
void derive_motion(Frame& frame, Slice const& slice, int mb_addr, MbSyntax const& mb);

// 8.7 over the whole picture.
void deblock_picture(Frame& frame);

}
