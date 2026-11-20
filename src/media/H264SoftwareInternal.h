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
// Frames, fields and MBAFF frames, 8 bits, 4:2:0 — what the profiles the
// web serves carry. A field is decoded into every other row of its frame,
// and so is an MBAFF field macroblock into its pair's.

#include "media/H264.h"
#include "media/H264Accelerator.h"
#include "media/H264SoftwareResidual.h"

#include <array>
#include <bit>
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

// 9.3.1.2 and 9.3.3.2: the arithmetic decoding engine. It reads its data
// two bytes ahead: `low` holds codIOffset followed by `bits` bits not yet
// taken, so codIOffset >= codIRange is low >= codIRange << bits, and
// RenormD only moves `bits`. A plain value, so the residual's loops keep a
// copy in registers.
struct CabacEngine {
    std::uint8_t const* data = nullptr;
    std::size_t size = 0;
    std::size_t next = 0; // the next byte to load
    int bits = 0;
    std::uint32_t range = 0;
    std::uint32_t low = 0;

    // A context's bin (`context`: pStateIdx * 2 + valMPS); in H264SoftwareParse.cpp.
    int decision(std::uint8_t& context);
    // 9.3.3.2.3.
    int bypass()
    {
        if (bits < 1)
            refill();
        --bits;
        std::uint32_t const scaled = range << bits;
        if (low >= scaled) {
            low -= scaled;
            return 1;
        }
        return 0;
    }
    // 9.3.3.2.4.
    int terminate()
    {
        range -= 2;
        if (low >= (range << bits))
            return 1;
        renormalize();
        return 0;
    }
    // As many doublings as take the range back to nine bits, in one step.
    void renormalize()
    {
        if (range < 256) {
            int const shift = std::countl_zero(range) - 23;
            range <<= shift;
            if (bits < shift)
                refill();
            bits -= shift;
        }
    }
    // Two more bytes below `low` (zeros past the end; overrun() tells).
    void refill()
    {
        low = (low << 16) | (std::uint32_t { byte(next) } << 8) | byte(next + 1);
        next += 2;
        bits += 16;
    }
    std::uint8_t byte(std::size_t i) const { return i < size ? data[i] : 0; }
    std::size_t consumed() const { return next * 8 - static_cast<std::size_t>(bits); }
};

// The engine and its 1024 contexts.
class Cabac {
public:
    void init_contexts(int slice_qp, int kind); // kind: 0 for I slices, 1 + cabac_init_idc
    // From the reader's position, which must be byte-aligned (9.3.1.2).
    void init_engine(BitReader& reader);
    int decision(int ctx_idx) { return m_engine.decision(m_context[static_cast<std::size_t>(ctx_idx)]); }
    int bypass() { return m_engine.bypass(); }
    int terminate() { return m_engine.terminate(); }
    // The bits the engine has taken (9.3.3.2.2.3 has nothing more to read
    // after a terminate of 1), given back to the reader.
    void hand_back(BitReader& reader) const { reader.resume_at(m_engine.consumed(), overrun()); }
    bool overrun() const { return m_engine.consumed() > m_engine.size * 8; }
    CabacEngine& engine() { return m_engine; }
    std::uint8_t* contexts() { return m_context.data(); }

private:
    CabacEngine m_engine;
    // Per context: pStateIdx * 2 + valMPS.
    std::array<std::uint8_t, 1024> m_context {};
};

struct Mv {
    std::int16_t x = 0;
    std::int16_t y = 0;
    bool operator==(Mv const&) const = default;
};

// How a frame was coded (Table 8-7's PicCodingStruct): as a frame, as a
// frame of macroblock pairs (MBAFF), or as two fields.
enum class Coding : std::uint8_t {
    Frame,
    Mbaff,
    Fields,
};

// A decoded frame: its planes at the coded size, and per 4x4 luma block
// the motion of each list (for the direct prediction of the pictures that
// take it as their colocated one, 8.4.1.2) with the picture each
// reference index named. The motion is kept in frame macroblock rows: a
// frame macroblock's in its own row, a field macroblock's — of a field
// picture, or of an MBAFF pair — in row 2 * (its row in the field) +
// (0 top, 1 bottom), which is where Table 8-8 looks for it.
struct Picture {
    int width = 0; // coded, in luma samples
    int height = 0;
    int mb_width = 0;
    int mb_height = 0;
    std::vector<std::uint8_t> y, cb, cr;
    std::array<std::vector<Mv>, 2> mv; // [list][block], block = (mb_row * 4 + y4) * (mb_width * 4) + mb_x * 4 + x4
    std::array<std::vector<std::int8_t>, 2> ref_idx; // -1: the list not used
    std::array<std::vector<std::int32_t>, 2> ref_pic; // the picture the block's reference index named (RefEntry::id)
    std::vector<std::uint8_t> mb_intra; // per macroblock (in the rows above)
    std::vector<std::uint8_t> mb_field; // per macroblock: a field macroblock
    std::int32_t serial = 0; // unique per decoded frame (both its fields share it)
    int poc_top = 0;
    int poc_bottom = 0;
    Coding coding = Coding::Frame;
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

// A reference in a slice's list, as the decoding needs it: a frame, or
// one field of it.
struct RefEntry {
    Picture const* picture = nullptr;
    int structure = 0; // 0 the frame, 1 its top field, 2 its bottom field
    bool long_term = false;
    int poc = 0;
    int poc_top = 0; // its frame's fields' counts, as the buffer has them marked
    int poc_bottom = 0;
    // What the deblocking and the colocated motion compare: the frame and
    // the field told apart.
    std::int32_t id() const { return picture ? picture->serial * 4 + structure : -1; }
    // The samples: a field is every other row of its frame.
    std::uint8_t const* luma() const { return picture->y.data() + (structure == 2 ? picture->width : 0); }
    std::uint8_t const* chroma(int c) const { return (c == 0 ? picture->cb : picture->cr).data() + (structure == 2 ? picture->width / 2 : 0); }
    int luma_stride() const { return picture->width << (structure != 0 ? 1 : 0); }
    int chroma_stride() const { return (picture->width / 2) << (structure != 0 ? 1 : 0); }
    int luma_height() const { return picture->height >> (structure != 0 ? 1 : 0); }
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
    bool field = false; // a field macroblock (of a field picture, or of an MBAFF field pair)
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
    int current_poc = 0; // PicOrderCnt( CurrPic ): the frame's, or the field's
    int poc_top = 0; // the current frame's fields' counts
    int poc_bottom = 0;
    int structure = 0; // the current picture: 0 a frame, 1 a top field, 2 a bottom field
    bool mbaff = false; // MbaffFrameFlag
    // MBAFF: a field macroblock's lists, [0 top, 1 bottom macroblock][list]
    // (8.4.2.1: entry 2i the same-parity field of frame entry i, 2i + 1
    // the other), and their implicit weights (8.4.3, field counts).
    std::array<std::array<std::vector<RefEntry>, 2>, 2> field_list;
    std::array<std::vector<std::array<int, 2>>, 2> field_implicit;
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

// Where the parser puts each macroblock's syntax: begin() gives a cleared
// one to parse into, end() says it is whole and may be reconstructed —
// at once, or by the picture's threads (H264Software.cpp). A macroblock
// begun and never ended is not reconstructed.
class MacroblockSink {
public:
    virtual ~MacroblockSink() = default;
    virtual MbSyntax& begin(int mb_addr) = 0;
    virtual void end(Slice const& slice, int mb_addr) = 0;
};

// The picture's state while its slices decode: the frame it is (or the
// field of it), seen as the picture's own rows of macroblocks.
struct Frame {
    Picture* picture = nullptr;
    MacroblockSink* sink = nullptr;
    std::vector<MbState> mbs; // per macroblock address of the picture
    std::vector<int> slice_alpha; // per slice number: FilterOffsetA, FilterOffsetB, disable_deblocking_filter_idc
    std::vector<int> slice_beta;
    std::vector<int> slice_disable;
    int chroma_qp_offset[2] = { 0, 0 };
    int structure = 0; // 0 a frame, 1 its top field, 2 its bottom field
    bool mbaff = false;
    int mb_width = 0; // the picture's, in macroblocks (a field: half the frame's rows)
    int mb_height = 0;
    // The picture's samples: the frame's planes, or every other row of them.
    std::uint8_t* luma = nullptr;
    std::uint8_t* chroma[2] = { nullptr, nullptr };
    int luma_stride = 0;
    int chroma_stride = 0;

    void set_view(Picture& pic, int structure_, bool mbaff_)
    {
        picture = &pic;
        structure = structure_;
        mbaff = mbaff_;
        int const field = structure != 0 ? 1 : 0;
        mb_width = pic.mb_width;
        mb_height = pic.mb_height >> field;
        luma = pic.y.data() + (structure == 2 ? pic.width : 0);
        chroma[0] = pic.cb.data() + (structure == 2 ? pic.width / 2 : 0);
        chroma[1] = pic.cr.data() + (structure == 2 ? pic.width / 2 : 0);
        luma_stride = pic.width << field;
        chroma_stride = (pic.width / 2) << field;
    }
    // The macroblock's row among the picture's motion rows (Picture says how).
    int storage_row(int addr) const
    {
        if (mbaff)
            return ((addr >> 1) / mb_width) * 2 + (addr & 1);
        int const row = addr / mb_width;
        return structure == 0 ? row : row * 2 + (structure - 1);
    }
    int mb_column(int addr) const { return mbaff ? (addr >> 1) % mb_width : addr % mb_width; }
    // Index of the macroblock in the picture's per-macroblock arrays, and of its first 4x4 block in the motion arrays.
    int storage_mb(int addr) const { return storage_row(addr) * mb_width + mb_column(addr); }
    int block_base(int addr) const { return storage_row(addr) * 4 * (mb_width * 4) + mb_column(addr) * 4; }
    int block_stride() const { return mb_width * 4; }

    // Where a macroblock's samples are: its upper-left luma sample among
    // the rows it is made of — the frame's (structure 0) or one field's (1
    // top, 2 bottom): a field picture's, or an MBAFF field macroblock's.
    struct Place {
        int x = 0;
        int y = 0;
        int structure = 0;
    };
    Place place(int addr) const
    {
        if (!mbaff)
            return { (addr % mb_width) * 16, (addr / mb_width) * 16, structure };
        int const pair = addr >> 1, bottom = addr & 1;
        if (mbs[static_cast<std::size_t>(addr)].field)
            return { (pair % mb_width) * 16, (pair / mb_width) * 16, 1 + bottom };
        return { (pair % mb_width) * 16, (pair / mb_width) * 32 + bottom * 16, 0 };
    }
    // 6.4.12: the macroblock holding location (xN, yN) of macroblock
    // `addr` (luma: max 16 by 16; chroma 4:2:0: 8 by 8), and the location
    // inside it — `addr` -1 where there is none: outside the picture, not
    // decoded yet, or of a slice other than `slice`. Table 6-3 in fields
    // and frames, Table 6-4 in MBAFF frames (a field macroblock's rows
    // meeting a frame pair's, or the other way round).
    struct Location {
        int addr = -1;
        int x = 0;
        int y = 0;
    };
    Location neighbor(int addr, int x_n, int y_n, int max_w, int max_h, int slice) const
    {
        auto available = [&](int a) { return a >= 0 && a < static_cast<int>(mbs.size()) && mbs[static_cast<std::size_t>(a)].slice == slice; };
        if (y_n > max_h - 1 || (x_n > max_w - 1 && y_n >= 0))
            return {};
        if (!mbaff) {
            int const mx = addr % mb_width + (x_n < 0 ? -1 : (x_n >= max_w ? 1 : 0));
            int const my = addr / mb_width + (y_n < 0 ? -1 : 0);
            if (mx < 0 || mx >= mb_width || my < 0)
                return {};
            int const a = my * mb_width + mx;
            if (a != addr && !available(a))
                return {};
            return { a, (x_n + max_w) % max_w, (y_n + max_h) % max_h };
        }
        int const pair = addr >> 1, px = pair % mb_width, py = pair / mb_width;
        // The top macroblock of the pair dx, dy pairs away (6.4.10), or -1.
        auto pair_at = [&](int dx, int dy) {
            int const x = px + dx, y = py + dy;
            if (x < 0 || x >= mb_width || y < 0)
                return -1;
            int const a = 2 * (y * mb_width + x);
            return available(a) ? a : -1;
        };
        auto frame_pair = [&](int a) { return !mbs[static_cast<std::size_t>(a)].field; };
        bool const cur_frame = !mbs[static_cast<std::size_t>(addr)].field;
        bool const top = (addr & 1) == 0;
        int n = -1, y_m = y_n;
        if (x_n < 0 && y_n < 0) {
            if (cur_frame && top) {
                int const d = pair_at(-1, -1);
                n = d < 0 ? -1 : d + 1;
            } else if (cur_frame) {
                int const a = pair_at(-1, 0);
                if (a >= 0 && frame_pair(a)) {
                    n = a;
                } else if (a >= 0) {
                    n = a + 1;
                    y_m = (y_n + max_h) >> 1;
                }
            } else {
                int const d = pair_at(-1, -1);
                if (d >= 0 && top && frame_pair(d)) {
                    n = d + 1;
                    y_m = 2 * y_n;
                } else if (d >= 0) {
                    n = top ? d : d + 1;
                }
            }
        } else if (x_n < 0) {
            int const a = pair_at(-1, 0);
            if (a < 0)
                return {};
            bool const a_frame = frame_pair(a);
            if (cur_frame) {
                if (a_frame) {
                    n = top ? a : a + 1;
                } else {
                    n = a + (y_n % 2);
                    y_m = top ? y_n >> 1 : (y_n + max_h) >> 1;
                }
            } else if (a_frame) {
                n = y_n < max_h / 2 ? a : a + 1;
                y_m = (y_n << 1) + (top ? 0 : 1) - (y_n < max_h / 2 ? 0 : max_h);
            } else {
                n = top ? a : a + 1;
            }
        } else if (x_n < max_w) {
            if (y_n >= 0) {
                n = addr;
            } else if (cur_frame && !top) {
                n = addr - 1;
            } else {
                int const b = pair_at(0, -1);
                if (b >= 0 && !cur_frame && top && frame_pair(b)) {
                    n = b + 1;
                    y_m = 2 * y_n;
                } else if (b >= 0) {
                    n = !cur_frame && top ? b : b + 1;
                }
            }
        } else {
            if (cur_frame && !top)
                return {};
            int const c = pair_at(1, -1);
            if (c >= 0 && !cur_frame && top && frame_pair(c)) {
                n = c + 1;
                y_m = 2 * y_n;
            } else if (c >= 0) {
                n = !cur_frame && top ? c : c + 1;
            }
        }
        if (n < 0)
            return {};
        return { n, (x_n + max_w) % max_w, (y_m + max_h) % max_h };
    }

    // The rows of a structure: the frame's, or every other one of them.
    std::uint8_t* luma_rows(int s) const { return picture->y.data() + (s == 2 ? picture->width : 0); }
    std::uint8_t* chroma_rows(int c, int s) const { return (c == 0 ? picture->cb : picture->cr).data() + (s == 2 ? picture->width / 2 : 0); }
    int luma_rows_stride(int s) const { return picture->width << (s != 0 ? 1 : 0); }
    int chroma_rows_stride(int s) const { return (picture->width / 2) << (s != 0 ? 1 : 0); }
};

// The decoding of one slice's macroblocks, from its data's first bit.
bool decode_slice_data(Frame& frame, Slice const& slice, BitReader& reader);

// An intra macroblock's motion as later parsing and pictures read it: no
// reference in either list. Set as soon as the macroblock is parsed.
void clear_intra_motion(Frame& frame, int mb_addr);

// The prediction and reconstruction of one macroblock (8.3 to 8.5).
void reconstruct_macroblock(Frame& frame, Slice const& slice, int mb_addr, MbSyntax const& mb);

// The motion of an inter macroblock (8.4.1): fills the picture's motion
// arrays for the macroblock.
void derive_motion(Frame& frame, Slice const& slice, int mb_addr, MbSyntax const& mb);

// 8.7 over the whole picture, or one macroblock's edges (in raster order
// after its left and top neighbors, and after the top one's right one).
void deblock_picture(Frame& frame);
void deblock_macroblock(Frame& frame, int mb_x, int mb_y);
// An MBAFF frame's macroblock pair (both its macroblocks, top first), in
// raster order of the pairs.
void deblock_pair(Frame& frame, int pair);

}
