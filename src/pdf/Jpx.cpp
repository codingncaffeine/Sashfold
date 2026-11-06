#include "pdf/ImageCodecs.h"

// JPEG 2000 pictures, PDF's /JPXDecode (ISO 32000-2 §7.4.9), written from
// ITU-T T.800 (ISO/IEC 15444-1): the JP2 file's boxes (Annex I) or a bare
// codestream, its markers (Annex A), each tile's packets (tier 2, Annex
// B), each code-block's passes (tier 1, Annex D, JpxTier1.cpp), the
// dequantization (Annex E), the inverse wavelet transforms (Annex F, the
// reversible 5/3 and the irreversible 9/7, by lifting with symmetric
// extension) and the component transforms and level shift (Annex G).
//
// Not yet read (the picture still decodes where it does not need them,
// else it is declined): progression order changes (POC), packed packet
// headers (PPM, PPT). Region of interest (RGN, max-shift) is read.

#include "pdf/JpxTier1.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::pdf {

namespace {

using namespace jpx;

// --- Reading -------------------------------------------------------------

struct Reader {
    std::span<std::uint8_t const> data;
    std::size_t at = 0;
    bool failed = false;

    bool has(std::size_t n) const { return at + n <= data.size(); }
    std::uint32_t u8()
    {
        if (!has(1)) {
            failed = true;
            return 0;
        }
        return data[at++];
    }
    std::uint32_t u16() { return (u8() << 8) | u8(); }
    std::uint32_t u32() { return (u16() << 16) | u16(); }
};

// --- The codestream's parameters -----------------------------------------

struct Component {
    int depth = 8; // bits
    bool is_signed = false;
    int dx = 1; // XRsiz
    int dy = 1;
};

struct CodingStyle {
    int levels = 5; // decomposition levels
    int block_w = 6; // code-block width exponent
    int block_h = 6;
    BlockStyle block;
    bool reversible = true; // 5/3, else 9/7
    std::array<std::uint8_t, 33> precinct_w {}; // exponents, per resolution
    std::array<std::uint8_t, 33> precinct_h {};
};

struct Quantization {
    int style = 0; // 0 none, 1 scalar derived, 2 scalar expounded
    int guard = 2;
    std::vector<std::pair<int, int>> steps; // (exponent, mantissa) per subband, in order
};

struct Defaults {
    int progression = 0; // LRCP, RLCP, RPCL, PCRL, CPRL
    int layers = 1;
    bool mct = false;
    bool sop = false;
    bool eph = false;
    std::vector<CodingStyle> coding; // per component
    std::vector<Quantization> quantization; // per component
    std::vector<int> roi_shift; // per component
};

struct Codestream {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the image area
    int tx0 = 0, ty0 = 0, tw = 0, th = 0; // the tile grid
    std::vector<Component> components;
    Defaults main;
    // Each tile's own parameters (when its tile-part headers change them),
    // and its data, tile-parts in order.
    std::map<int, Defaults> tile_parameters;
    std::map<int, std::vector<std::uint8_t>> tile_data;
};

int ceil_div(int a, int b)
{
    return static_cast<int>((static_cast<std::int64_t>(a) + b - 1) / b);
}

int ceil_div_pow2(int a, int e)
{
    return static_cast<int>((static_cast<std::int64_t>(a) + (std::int64_t { 1 } << e) - 1) >> e);
}

bool read_coding_style_parameters(Reader& r, CodingStyle& style, bool precincts)
{
    style.levels = static_cast<int>(r.u8());
    style.block_w = static_cast<int>(r.u8()) + 2;
    style.block_h = static_cast<int>(r.u8()) + 2;
    std::uint32_t const flags = r.u8();
    style.block.bypass = flags & 0x01;
    style.block.reset = flags & 0x02;
    style.block.terminate_all = flags & 0x04;
    style.block.causal = flags & 0x08;
    style.block.predictable = flags & 0x10;
    style.block.segmentation = flags & 0x20;
    style.reversible = r.u8() == 1;
    if (style.levels > 32 || style.block_w > 10 || style.block_h > 10 || style.block_w + style.block_h > 12)
        return false;
    for (int i = 0; i <= style.levels; ++i) {
        std::uint32_t const size = precincts ? r.u8() : 0xFF;
        style.precinct_w[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(precincts ? (size & 0x0F) : 15);
        style.precinct_h[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(precincts ? (size >> 4) : 15);
    }
    return !r.failed;
}

bool read_quantization(Reader& r, std::size_t end, Quantization& q)
{
    std::uint32_t const s = r.u8();
    q.guard = static_cast<int>(s >> 5);
    q.style = static_cast<int>(s & 0x1F);
    q.steps.clear();
    while (r.at < end && !r.failed) {
        if (q.style == 0) {
            q.steps.push_back({ static_cast<int>(r.u8() >> 3), 0 });
        } else {
            std::uint32_t const v = r.u16();
            q.steps.push_back({ static_cast<int>(v >> 11), static_cast<int>(v & 0x7FF) });
        }
    }
    return !r.failed && q.style <= 2 && !q.steps.empty();
}

// Reads the markers of a header (the main one, or a tile-part's into its
// tile's parameters) up to SOT or SOD.
bool read_header_markers(Reader& r, Codestream& cs, Defaults& d, bool main)
{
    while (!r.failed) {
        if (!r.has(2))
            return false;
        std::uint32_t const marker = r.u16();
        if (marker == 0xFF90 || marker == 0xFF93) { // SOT or SOD: the caller's
            r.at -= 2;
            return true;
        }
        std::uint32_t const length = r.u16();
        if (length < 2 || !r.has(length - 2))
            return false;
        std::size_t const end = r.at + length - 2;
        std::size_t const count = cs.components.size();
        switch (marker) {
        case 0xFF51: { // SIZ
            if (!main)
                return false;
            r.u16(); // Rsiz
            cs.x1 = static_cast<int>(r.u32());
            cs.y1 = static_cast<int>(r.u32());
            cs.x0 = static_cast<int>(r.u32());
            cs.y0 = static_cast<int>(r.u32());
            cs.tw = static_cast<int>(r.u32());
            cs.th = static_cast<int>(r.u32());
            cs.tx0 = static_cast<int>(r.u32());
            cs.ty0 = static_cast<int>(r.u32());
            std::uint32_t const n = r.u16();
            if (n == 0 || n > 16384)
                return false;
            cs.components.resize(n);
            for (Component& c : cs.components) {
                std::uint32_t const s = r.u8();
                c.depth = static_cast<int>(s & 0x7F) + 1;
                c.is_signed = s & 0x80;
                c.dx = static_cast<int>(r.u8());
                c.dy = static_cast<int>(r.u8());
                if (c.depth > 24 || c.dx == 0 || c.dy == 0)
                    return false;
            }
            if (cs.x1 <= cs.x0 || cs.y1 <= cs.y0 || cs.tw <= 0 || cs.th <= 0 || cs.tx0 > cs.x0 || cs.ty0 > cs.y0
                || cs.x1 < 0 || cs.y1 < 0)
                return false;
            d.coding.assign(n, CodingStyle {});
            d.quantization.assign(n, Quantization {});
            d.roi_shift.assign(n, 0);
            break;
        }
        case 0xFF52: { // COD
            std::uint32_t const scod = r.u8();
            d.progression = static_cast<int>(r.u8());
            d.layers = static_cast<int>(r.u16());
            d.mct = r.u8() != 0;
            d.sop = scod & 0x02;
            d.eph = scod & 0x04;
            CodingStyle style;
            if (!read_coding_style_parameters(r, style, scod & 0x01))
                return false;
            // COD sets every component's, which a COC may then change.
            for (CodingStyle& each : d.coding)
                each = style;
            if (d.progression > 4 || d.layers == 0)
                return false;
            break;
        }
        case 0xFF53: { // COC
            std::uint32_t const c = count > 256 ? r.u16() : r.u8();
            std::uint32_t const scoc = r.u8();
            if (c >= count)
                return false;
            if (!read_coding_style_parameters(r, d.coding[c], scoc & 0x01))
                return false;
            break;
        }
        case 0xFF5C: { // QCD
            Quantization q;
            if (!read_quantization(r, end, q))
                return false;
            for (Quantization& each : d.quantization)
                each = q;
            break;
        }
        case 0xFF5D: { // QCC
            std::uint32_t const c = count > 256 ? r.u16() : r.u8();
            if (c >= count || !read_quantization(r, end, d.quantization[c]))
                return false;
            break;
        }
        case 0xFF5E: { // RGN
            std::uint32_t const c = count > 256 ? r.u16() : r.u8();
            std::uint32_t const style = r.u8();
            std::uint32_t const shift = r.u8();
            if (c >= count || style != 0)
                return false;
            d.roi_shift[c] = static_cast<int>(shift);
            break;
        }
        case 0xFF5F: // POC
        case 0xFF60: // PPM
        case 0xFF61: // PPT
            return false;
        default:
            break; // COM, TLM, PLM, PLT, CRG: nothing a decoder needs
        }
        if (r.failed || r.at > end)
            return false;
        r.at = end;
    }
    return false;
}

std::optional<Codestream> read_codestream(std::span<std::uint8_t const> data)
{
    Reader r { data };
    if (r.u16() != 0xFF4F) // SOC
        return std::nullopt;
    Codestream cs;
    if (!read_header_markers(r, cs, cs.main, true) || cs.components.empty())
        return std::nullopt;
    int const tiles_across = ceil_div(cs.x1 - cs.tx0, cs.tw);
    int const tiles_down = ceil_div(cs.y1 - cs.ty0, cs.th);
    while (r.has(2) && !r.failed) {
        std::uint32_t const marker = r.u16();
        if (marker == 0xFFD9) // EOC
            break;
        if (marker != 0xFF90) // SOT
            return std::nullopt;
        std::size_t const start = r.at - 2;
        r.u16(); // Lsot
        int const tile = static_cast<int>(r.u16());
        std::uint32_t const psot = r.u32();
        std::uint32_t const part = r.u8();
        r.u8(); // TNsot
        if (tile >= tiles_across * tiles_down)
            return std::nullopt;
        // A tile's first part may change its parameters; they start as the main ones.
        auto [parameters, fresh] = cs.tile_parameters.try_emplace(tile, cs.main);
        (void)fresh;
        if (!read_header_markers(r, cs, parameters->second, false) && part == 0)
            return std::nullopt;
        if (r.u16() != 0xFF93) // SOD
            return std::nullopt;
        std::size_t const end = psot == 0 ? data.size() : std::min(data.size(), start + psot);
        if (end < r.at)
            return std::nullopt;
        std::vector<std::uint8_t>& bytes = cs.tile_data[tile];
        bytes.insert(bytes.end(), data.begin() + static_cast<std::ptrdiff_t>(r.at), data.begin() + static_cast<std::ptrdiff_t>(end));
        r.at = end;
    }
    return cs;
}

// --- Tier 2: a tile's packets --------------------------------------------

// The bits of packet headers: after a 0xFF byte the next gives seven.
struct HeaderBits {
    std::span<std::uint8_t const> data;
    std::size_t at = 0;
    std::uint32_t byte = 0;
    int left = 0;

    int bit()
    {
        if (left == 0) {
            bool const after_ff = byte == 0xFF;
            byte = at < data.size() ? data[at] : 0;
            ++at;
            left = after_ff ? 7 : 8;
        }
        --left;
        return static_cast<int>((byte >> left) & 1);
    }
    std::uint32_t bits(int n)
    {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | static_cast<std::uint32_t>(bit());
        return v;
    }
    // The header's end: its last byte whole, and when that byte is 0xFF the
    // byte after it, whose stuffed bit begins it (B.10.1).
    void finish()
    {
        if (byte == 0xFF)
            ++at;
        left = 0;
        byte = 0;
    }
};

// A tag tree (B.10.2) over a grid of leaves.
class TagTree {
public:
    TagTree(int w, int h)
    {
        int lw = w, lh = h;
        do {
            m_levels.push_back({ lw, lh, m_nodes.size() });
            m_nodes.resize(m_nodes.size() + static_cast<std::size_t>(lw * lh));
            lw = (lw + 1) / 2;
            lh = (lh + 1) / 2;
        } while (m_levels.back().w * m_levels.back().h > 1);
    }

    // Whether the leaf's value is below `threshold`, reading what it takes.
    bool decode(HeaderBits& bits, int x, int y, int threshold)
    {
        std::array<Node*, 40> stack {};
        int depth = 0;
        for (std::size_t level = 0; level < m_levels.size(); ++level) {
            Level const& l = m_levels[level];
            stack[static_cast<std::size_t>(depth++)] = &m_nodes[l.first + static_cast<std::size_t>((y >> level) * l.w + (x >> level))];
        }
        int low = 0;
        for (int i = depth - 1; i >= 0; --i) {
            Node& node = *stack[static_cast<std::size_t>(i)];
            if (low > node.low)
                node.low = low;
            else
                low = node.low;
            while (low < threshold && low < node.value) {
                if (bits.bit())
                    node.value = low;
                else
                    ++low;
            }
            node.low = low;
        }
        return stack[0]->value < threshold;
    }
    int value(int x, int y) const { return m_nodes[static_cast<std::size_t>(y * m_levels[0].w + x)].value; }

private:
    struct Node {
        int value = 1 << 20;
        int low = 0;
    };
    struct Level {
        int w, h;
        std::size_t first;
    };
    std::vector<Node> m_nodes;
    std::vector<Level> m_levels;
};

struct CodeBlock {
    int x0, y0, x1, y1; // in the subband
    bool included = false;
    int zero_planes = 0;
    int lblock = 3;
    int passes = 0; // so far
    std::vector<std::uint8_t> data;
    std::vector<int> segment_passes; // passes in each segment so far
    std::vector<std::size_t> segment_bytes;
};

struct PrecinctBand {
    int blocks_w = 0, blocks_h = 0;
    std::vector<CodeBlock> blocks;
    std::optional<TagTree> inclusion;
    std::optional<TagTree> zero_planes;
};

struct Subband {
    Band band;
    int x0, y0, x1, y1;
    int level; // nb: its decomposition level
    float step = 1; // the quantization step, 1 when reversible
    int magnitude_planes = 0; // Mb
    std::vector<std::int32_t> coefficients; // times two, as tier 1 gives them
};

struct Resolution {
    int x0, y0, x1, y1;
    std::vector<Subband> bands; // LL alone at 0, else HL, LH, HH
    int precincts_w = 0, precincts_h = 0;
    int pw = 15, ph = 15; // precinct exponents at this resolution
    // precinct -> per band
    std::vector<std::vector<PrecinctBand>> precincts;
};

struct TileComponent {
    int x0, y0, x1, y1;
    std::vector<Resolution> resolutions;
    CodingStyle style;
};

// The code-blocks of one band within one precinct.
PrecinctBand make_precinct_band(Subband const& band, Resolution const& res, int precinct, CodingStyle const& style, int r)
{
    PrecinctBand pb;
    int const px = precinct % res.precincts_w;
    int const py = precinct / res.precincts_w;
    // The precinct in the band's coordinates: halved past resolution 0.
    int const shift = r == 0 ? 0 : 1;
    int const pw = res.pw - shift;
    int const ph = res.ph - shift;
    int const origin_x = (res.x0 >> res.pw) << res.pw;
    int const origin_y = (res.y0 >> res.ph) << res.ph;
    int const rx0 = origin_x + px * (1 << res.pw);
    int const ry0 = origin_y + py * (1 << res.ph);
    int const bx0 = std::max(band.x0, ceil_div_pow2(rx0, shift));
    int const by0 = std::max(band.y0, ceil_div_pow2(ry0, shift));
    int const bx1 = std::min(band.x1, ceil_div_pow2(rx0, shift) + (1 << pw));
    int const by1 = std::min(band.y1, ceil_div_pow2(ry0, shift) + (1 << ph));
    if (bx1 <= bx0 || by1 <= by0)
        return pb;
    int const cbw = std::min(style.block_w, pw);
    int const cbh = std::min(style.block_h, ph);
    int const gx0 = (bx0 >> cbw) << cbw;
    int const gy0 = (by0 >> cbh) << cbh;
    pb.blocks_w = ceil_div_pow2(bx1 - gx0, cbw);
    pb.blocks_h = ceil_div_pow2(by1 - gy0, cbh);
    if (pb.blocks_w <= 0 || pb.blocks_h <= 0 || pb.blocks_w * pb.blocks_h > (1 << 20))
        return PrecinctBand {};
    for (int j = 0; j < pb.blocks_h; ++j) {
        for (int i = 0; i < pb.blocks_w; ++i) {
            CodeBlock block {};
            block.x0 = std::max(bx0, gx0 + (i << cbw));
            block.y0 = std::max(by0, gy0 + (j << cbh));
            block.x1 = std::min(bx1, gx0 + ((i + 1) << cbw));
            block.y1 = std::min(by1, gy0 + ((j + 1) << cbh));
            pb.blocks.push_back(block);
        }
    }
    pb.inclusion.emplace(pb.blocks_w, pb.blocks_h);
    pb.zero_planes.emplace(pb.blocks_w, pb.blocks_h);
    return pb;
}

int read_pass_count(HeaderBits& bits)
{
    if (!bits.bit())
        return 1;
    if (!bits.bit())
        return 2;
    std::uint32_t const two = bits.bits(2);
    if (two != 3)
        return 3 + static_cast<int>(two);
    std::uint32_t const five = bits.bits(5);
    if (five != 31)
        return 6 + static_cast<int>(five);
    return 37 + static_cast<int>(bits.bits(7));
}

int floor_log2(int v)
{
    int l = 0;
    while (v > 1) {
        v >>= 1;
        ++l;
    }
    return l;
}

// One packet: its header, then its code-blocks' bytes. False on data
// that cannot be a packet.
bool read_packet(std::span<std::uint8_t const> data, std::size_t& at, Defaults const& d, TileComponent& tc, int r, int precinct,
    int layer)
{
    Resolution& res = tc.resolutions[static_cast<std::size_t>(r)];
    if (precinct >= static_cast<int>(res.precincts.size()))
        return true;
    if (d.sop && at + 6 <= data.size() && data[at] == 0xFF && data[at + 1] == 0x91)
        at += 6;
    HeaderBits bits { data.subspan(at) };
    struct Contribution {
        CodeBlock* block;
        std::vector<std::pair<int, std::size_t>> segments; // passes, bytes
    };
    std::vector<Contribution> contributions;
    if (bits.bit()) {
        for (std::size_t b = 0; b < res.bands.size(); ++b) {
            PrecinctBand& pb = res.precincts[static_cast<std::size_t>(precinct)][b];
            for (int j = 0; j < pb.blocks_h; ++j) {
                for (int i = 0; i < pb.blocks_w; ++i) {
                    CodeBlock& block = pb.blocks[static_cast<std::size_t>(j * pb.blocks_w + i)];
                    bool included;
                    if (!block.included) {
                        included = pb.inclusion->decode(bits, i, j, layer + 1);
                    } else {
                        included = bits.bit();
                    }
                    if (!included)
                        continue;
                    if (!block.included) {
                        int planes = 0;
                        while (!pb.zero_planes->decode(bits, i, j, planes + 1)) {
                            if (++planes > 64)
                                return false;
                        }
                        block.zero_planes = pb.zero_planes->value(i, j);
                        block.included = true;
                    }
                    int const passes = read_pass_count(bits);
                    while (bits.bit()) {
                        if (++block.lblock > 32)
                            return false;
                    }
                    // The new passes, segment by segment.
                    Contribution contribution { &block, {} };
                    int pass = block.passes;
                    int left = passes;
                    int open = block.segment_passes.empty() ? 0 : block.segment_passes.back();
                    int first_of_open = pass - open;
                    while (left > 0) {
                        int room = segment_pass_limit(tc.style.block, first_of_open) - open;
                        if (room <= 0) {
                            first_of_open = pass;
                            open = 0;
                            room = segment_pass_limit(tc.style.block, pass);
                            contribution.segments.push_back({ 0, 0 });
                        } else if (contribution.segments.empty()) {
                            contribution.segments.push_back({ 0, 0 });
                        }
                        int const take = std::min(room, left);
                        contribution.segments.back().first += take;
                        pass += take;
                        open += take;
                        left -= take;
                    }
                    for (auto& segment : contribution.segments) {
                        int const length_bits = block.lblock + floor_log2(segment.first);
                        if (length_bits > 32)
                            return false;
                        segment.second = bits.bits(length_bits);
                    }
                    contributions.push_back(std::move(contribution));
                }
            }
        }
    }
    bits.finish();
    at += bits.at;
    if (d.eph && at + 2 <= data.size() && data[at] == 0xFF && data[at + 1] == 0x92)
        at += 2;
    // The bodies, in the same order.
    for (Contribution& contribution : contributions) {
        CodeBlock& block = *contribution.block;
        bool continues = !block.segment_passes.empty()
            && block.segment_passes.back() < segment_pass_limit(tc.style.block, block.passes - block.segment_passes.back());
        for (std::size_t s = 0; s < contribution.segments.size(); ++s) {
            auto const [passes, bytes] = contribution.segments[s];
            if (at + bytes > data.size())
                return false;
            block.data.insert(block.data.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                data.begin() + static_cast<std::ptrdiff_t>(at + bytes));
            at += bytes;
            if (s == 0 && continues) {
                block.segment_passes.back() += passes;
                block.segment_bytes.back() += bytes;
            } else {
                block.segment_passes.push_back(passes);
                block.segment_bytes.push_back(bytes);
            }
            block.passes += passes;
        }
    }
    return true;
}

// The tile's packets in the order its progression says (B.12).
bool read_packets(std::span<std::uint8_t const> data, Defaults const& d, std::vector<TileComponent>& components,
    std::vector<Component> const& image_components, int tile_x0, int tile_y0)
{
    std::size_t at = 0;
    int max_levels = 0;
    for (TileComponent const& tc : components)
        max_levels = std::max(max_levels, tc.style.levels);
    auto const packet = [&](int c, int r, int p, int l) {
        TileComponent& tc = components[static_cast<std::size_t>(c)];
        if (r > tc.style.levels)
            return true;
        return read_packet(data, at, d, tc, r, p, l);
    };
    auto const max_precincts = [&](int c, int r) {
        TileComponent const& tc = components[static_cast<std::size_t>(c)];
        if (r > tc.style.levels)
            return 0;
        return static_cast<int>(tc.resolutions[static_cast<std::size_t>(r)].precincts.size());
    };
    int const nc = static_cast<int>(components.size());
    auto const at_end = [&] { return at >= data.size(); };
    switch (d.progression) {
    case 0: // LRCP
        for (int l = 0; l < d.layers; ++l)
            for (int r = 0; r <= max_levels; ++r)
                for (int c = 0; c < nc; ++c)
                    for (int p = 0; p < max_precincts(c, r); ++p)
                        if (at_end() || !packet(c, r, p, l))
                            return at_end();
        return true;
    case 1: // RLCP
        for (int r = 0; r <= max_levels; ++r)
            for (int l = 0; l < d.layers; ++l)
                for (int c = 0; c < nc; ++c)
                    for (int p = 0; p < max_precincts(c, r); ++p)
                        if (at_end() || !packet(c, r, p, l))
                            return at_end();
        return true;
    default:
        break;
    }
    // The position-major orders: precincts visited by where they lie on the
    // image grid (B.12.1.3-5), each component's at its own resolution.
    struct Visit {
        int c, r, p;
        std::int64_t y, x;
    };
    std::vector<Visit> visits;
    for (int c = 0; c < nc; ++c) {
        TileComponent const& tc = components[static_cast<std::size_t>(c)];
        Component const& ic = image_components[static_cast<std::size_t>(c)];
        for (int r = 0; r <= tc.style.levels; ++r) {
            Resolution const& res = tc.resolutions[static_cast<std::size_t>(r)];
            int const down = tc.style.levels - r;
            for (int p = 0; p < static_cast<int>(res.precincts.size()); ++p) {
                int const px = p % res.precincts_w;
                int const py = p / res.precincts_w;
                // The precinct's top-left on the image grid.
                std::int64_t const rx = std::max<std::int64_t>(res.x0, ((res.x0 >> res.pw) + px) << res.pw);
                std::int64_t const ry = std::max<std::int64_t>(res.y0, ((res.y0 >> res.ph) + py) << res.ph);
                std::int64_t const gx = std::max<std::int64_t>(tile_x0, (rx << down) * ic.dx);
                std::int64_t const gy = std::max<std::int64_t>(tile_y0, (ry << down) * ic.dy);
                visits.push_back({ c, r, p, gy, gx });
            }
        }
    }
    auto const order = [&](Visit const& a, Visit const& b) {
        switch (d.progression) {
        case 2: // RPCL
            if (a.r != b.r)
                return a.r < b.r;
            if (a.y != b.y)
                return a.y < b.y;
            if (a.x != b.x)
                return a.x < b.x;
            return a.c < b.c;
        case 3: // PCRL
            if (a.y != b.y)
                return a.y < b.y;
            if (a.x != b.x)
                return a.x < b.x;
            if (a.c != b.c)
                return a.c < b.c;
            return a.r < b.r;
        default: // CPRL
            if (a.c != b.c)
                return a.c < b.c;
            if (a.y != b.y)
                return a.y < b.y;
            if (a.x != b.x)
                return a.x < b.x;
            return a.r < b.r;
        }
    };
    std::stable_sort(visits.begin(), visits.end(), order);
    for (Visit const& v : visits) {
        for (int l = 0; l < d.layers; ++l) {
            if (at_end() || !packet(v.c, v.r, v.p, l))
                return at_end();
        }
    }
    return true;
}

// --- Annex F: the inverse wavelet transforms ------------------------------

// One line's synthesis in place: `x` holds the interleaved samples of the
// signal [i0, i1) (x[0] is sample i0), low-pass at even absolute indices.
void synthesize_53(std::vector<float>& line, int i0, int i1)
{
    int const n = i1 - i0;
    if (n == 1) {
        if (i0 & 1)
            line[0] /= 2; // F.3.7: a lone odd sample is a high-pass one
        return;
    }
    // Symmetric extension (F.3.7).
    auto const at = [&](int i) -> float& {
        while (i < i0 || i >= i1)
            i = i < i0 ? 2 * i0 - i : 2 * (i1 - 1) - i;
        return line[static_cast<std::size_t>(i - i0)];
    };
    // Even samples first, then odd (F-6, F-7), integer arithmetic.
    std::vector<float> even(static_cast<std::size_t>(n));
    for (int i = i0 + (i0 & 1); i < i1; i += 2) {
        float const left = at(i - 1);
        float const right = at(i + 1);
        even[static_cast<std::size_t>(i - i0)] = at(i) - std::floor((left + right + 2) / 4);
    }
    for (int i = i0 + (i0 & 1); i < i1; i += 2)
        line[static_cast<std::size_t>(i - i0)] = even[static_cast<std::size_t>(i - i0)];
    for (int i = i0 + 1 - (i0 & 1); i < i1; i += 2)
        line[static_cast<std::size_t>(i - i0)] = at(i) + std::floor((at(i - 1) + at(i + 1)) / 2);
}

void synthesize_97(std::vector<float>& line, int i0, int i1)
{
    int const n = i1 - i0;
    if (n == 1) {
        if (i0 & 1)
            line[0] /= 2;
        return;
    }
    constexpr float alpha = -1.586134342059924f;
    constexpr float beta = -0.052980118572961f;
    constexpr float gamma = 0.882911075530934f;
    constexpr float delta = 0.443506852043971f;
    constexpr float k = 1.230174104914001f;
    auto const at = [&](int i) -> float& {
        while (i < i0 || i >= i1)
            i = i < i0 ? 2 * i0 - i : 2 * (i1 - 1) - i;
        return line[static_cast<std::size_t>(i - i0)];
    };
    int const first_even = i0 + (i0 & 1);
    int const first_odd = i0 + 1 - (i0 & 1);
    for (int i = first_even; i < i1; i += 2)
        at(i) *= k;
    for (int i = first_odd; i < i1; i += 2)
        at(i) *= 1 / k;
    for (int i = first_even; i < i1; i += 2)
        at(i) -= delta * (at(i - 1) + at(i + 1));
    for (int i = first_odd; i < i1; i += 2)
        at(i) -= gamma * (at(i - 1) + at(i + 1));
    for (int i = first_even; i < i1; i += 2)
        at(i) -= beta * (at(i - 1) + at(i + 1));
    for (int i = first_odd; i < i1; i += 2)
        at(i) -= alpha * (at(i - 1) + at(i + 1));
}

// A tile-component's samples from its subbands (F.3.1-F.3.3).
std::vector<float> reconstruct(TileComponent& tc)
{
    Resolution const& base = tc.resolutions[0];
    int w = base.x1 - base.x0;
    int h = base.y1 - base.y0;
    std::vector<float> plane(static_cast<std::size_t>(std::max(0, w * h)));
    {
        Subband const& ll = base.bands[0];
        for (std::size_t i = 0; i < plane.size(); ++i)
            plane[i] = static_cast<float>(ll.coefficients[i]) / 2 * ll.step;
    }
    for (std::size_t r = 1; r < tc.resolutions.size(); ++r) {
        Resolution const& res = tc.resolutions[r];
        Resolution const& lower = tc.resolutions[r - 1];
        int const u0 = res.x0, u1 = res.x1, v0 = res.y0, v1 = res.y1;
        int const nw = u1 - u0, nh = v1 - v0;
        std::vector<float> next(static_cast<std::size_t>(std::max(0, nw * nh)), 0.0f);
        // 2D_INTERLEAVE: each sample from the band its parities name.
        auto const band_value = [&](Subband const& band, int bx, int by) {
            if (bx < band.x0 || bx >= band.x1 || by < band.y0 || by >= band.y1)
                return 0.0f;
            std::size_t const i = static_cast<std::size_t>((by - band.y0) * (band.x1 - band.x0) + (bx - band.x0));
            return static_cast<float>(band.coefficients[i]) / 2 * band.step;
        };
        for (int v = v0; v < v1; ++v) {
            for (int u = u0; u < u1; ++u) {
                bool const high_x = u & 1;
                bool const high_y = v & 1;
                float value;
                if (!high_x && !high_y) {
                    int const lx = u / 2 - ceil_div_pow2(u0, 1) + 0;
                    int const ly = v / 2 - ceil_div_pow2(v0, 1);
                    int const lw = lower.x1 - lower.x0;
                    value = (lx >= 0 && ly >= 0 && lx < lw && ly < lower.y1 - lower.y0)
                        ? plane[static_cast<std::size_t>(ly * lw + lx)]
                        : 0.0f;
                } else if (high_x && !high_y) {
                    value = band_value(res.bands[0], u / 2, v / 2); // HL
                } else if (!high_x && high_y) {
                    value = band_value(res.bands[1], u / 2, v / 2); // LH
                } else {
                    value = band_value(res.bands[2], u / 2, v / 2); // HH
                }
                next[static_cast<std::size_t>((v - v0) * nw + (u - u0))] = value;
            }
        }
        // HOR_SR then VER_SR.
        std::vector<float> line;
        for (int v = 0; v < nh; ++v) {
            line.assign(next.begin() + v * nw, next.begin() + (v + 1) * nw);
            if (tc.style.reversible)
                synthesize_53(line, u0, u1);
            else
                synthesize_97(line, u0, u1);
            std::copy(line.begin(), line.end(), next.begin() + v * nw);
        }
        line.resize(static_cast<std::size_t>(nh));
        for (int u = 0; u < nw; ++u) {
            for (int v = 0; v < nh; ++v)
                line[static_cast<std::size_t>(v)] = next[static_cast<std::size_t>(v * nw + u)];
            if (tc.style.reversible)
                synthesize_53(line, v0, v1);
            else
                synthesize_97(line, v0, v1);
            for (int v = 0; v < nh; ++v)
                next[static_cast<std::size_t>(v * nw + u)] = line[static_cast<std::size_t>(v)];
        }
        plane = std::move(next);
        w = nw;
        h = nh;
    }
    return plane;
}

// --- A tile ----------------------------------------------------------------

bool decode_tile(Codestream const& cs, int tile, Defaults const& d, std::vector<std::vector<float>>& planes,
    std::vector<std::array<int, 4>>& extents)
{
    int const across = ceil_div(cs.x1 - cs.tx0, cs.tw);
    int const p = tile % across;
    int const q = tile / across;
    int const tx0 = std::max(cs.tx0 + p * cs.tw, cs.x0);
    int const ty0 = std::max(cs.ty0 + q * cs.th, cs.y0);
    int const tx1 = std::min(cs.tx0 + (p + 1) * cs.tw, cs.x1);
    int const ty1 = std::min(cs.ty0 + (q + 1) * cs.th, cs.y1);
    std::vector<TileComponent> components(cs.components.size());
    for (std::size_t c = 0; c < components.size(); ++c) {
        TileComponent& tc = components[c];
        Component const& ic = cs.components[c];
        tc.style = d.coding[c];
        tc.x0 = ceil_div(tx0, ic.dx);
        tc.y0 = ceil_div(ty0, ic.dy);
        tc.x1 = ceil_div(tx1, ic.dx);
        tc.y1 = ceil_div(ty1, ic.dy);
        int const levels = tc.style.levels;
        Quantization const& quant = d.quantization[c];
        int band_index = 0;
        for (int r = 0; r <= levels; ++r) {
            Resolution res {};
            int const down = levels - r;
            res.x0 = ceil_div_pow2(tc.x0, down);
            res.y0 = ceil_div_pow2(tc.y0, down);
            res.x1 = ceil_div_pow2(tc.x1, down);
            res.y1 = ceil_div_pow2(tc.y1, down);
            res.pw = tc.style.precinct_w[static_cast<std::size_t>(r)];
            res.ph = tc.style.precinct_h[static_cast<std::size_t>(r)];
            if (res.x1 > res.x0 && res.y1 > res.y0) {
                res.precincts_w = ceil_div_pow2(res.x1, res.pw) - (res.x0 >> res.pw);
                res.precincts_h = ceil_div_pow2(res.y1, res.ph) - (res.y0 >> res.ph);
            }
            std::vector<Band> kinds = r == 0 ? std::vector<Band> { Band::LL } : std::vector<Band> { Band::HL, Band::LH, Band::HH };
            for (Band kind : kinds) {
                Subband band {};
                band.band = kind;
                band.level = r == 0 ? levels : levels - r + 1;
                int const xo = (kind == Band::HL || kind == Band::HH) ? 1 : 0;
                int const yo = (kind == Band::LH || kind == Band::HH) ? 1 : 0;
                int const nb = band.level;
                std::int64_t const shift = std::int64_t { 1 } << (nb > 0 ? nb - 1 : 0);
                band.x0 = static_cast<int>((tc.x0 - shift * xo + (std::int64_t { 1 } << nb) - 1) >> nb);
                band.y0 = static_cast<int>((tc.y0 - shift * yo + (std::int64_t { 1 } << nb) - 1) >> nb);
                band.x1 = static_cast<int>((tc.x1 - shift * xo + (std::int64_t { 1 } << nb) - 1) >> nb);
                band.y1 = static_cast<int>((tc.y1 - shift * yo + (std::int64_t { 1 } << nb) - 1) >> nb);
                if (r == 0) {
                    band.x0 = res.x0;
                    band.y0 = res.y0;
                    band.x1 = res.x1;
                    band.y1 = res.y1;
                }
                // Annex E: the step and the magnitude's bit-planes.
                int const gain = kind == Band::LL ? 0 : kind == Band::HH ? 2 : 1;
                int exponent = 0, mantissa = 0;
                if (quant.style == 1) {
                    exponent = quant.steps[0].first - levels + nb;
                    mantissa = quant.steps[0].second;
                    if (r == 0)
                        exponent = quant.steps[0].first;
                } else if (static_cast<std::size_t>(band_index) < quant.steps.size()) {
                    exponent = quant.steps[static_cast<std::size_t>(band_index)].first;
                    mantissa = quant.steps[static_cast<std::size_t>(band_index)].second;
                } else {
                    return false;
                }
                band.magnitude_planes = quant.guard + exponent - 1;
                int const range = ic.depth + gain;
                band.step = quant.style == 0
                    ? 1.0f
                    : std::ldexp(1.0f + static_cast<float>(mantissa) / 2048.0f, range - exponent);
                std::size_t const area = static_cast<std::size_t>(std::max(0, band.x1 - band.x0))
                    * static_cast<std::size_t>(std::max(0, band.y1 - band.y0));
                band.coefficients.assign(area, 0);
                res.bands.push_back(std::move(band));
                ++band_index;
            }
            if (static_cast<std::int64_t>(res.precincts_w) * res.precincts_h > (1 << 20))
                return false;
            for (int pr = 0; pr < res.precincts_w * res.precincts_h; ++pr) {
                std::vector<PrecinctBand> per_band;
                for (Subband const& band : res.bands)
                    per_band.push_back(make_precinct_band(band, res, pr, tc.style, r));
                res.precincts.push_back(std::move(per_band));
            }
            tc.resolutions.push_back(std::move(res));
        }
    }
    auto const found = cs.tile_data.find(tile);
    if (found != cs.tile_data.end())
        read_packets(found->second, d, components, cs.components, tx0, ty0);
    // Tier 1 for every code-block, into its subband.
    std::vector<Segment> segments;
    std::vector<std::int32_t> coefficients;
    for (std::size_t c = 0; c < components.size(); ++c) {
        TileComponent& tc = components[c];
        int const roi = d.roi_shift[c];
        for (Resolution& res : tc.resolutions) {
            for (auto& per_band : res.precincts) {
                for (std::size_t b = 0; b < per_band.size(); ++b) {
                    Subband& band = res.bands[b];
                    for (CodeBlock& block : per_band[b].blocks) {
                        if (!block.included || block.passes == 0)
                            continue;
                        segments.clear();
                        std::size_t offset = 0;
                        for (std::size_t s = 0; s < block.segment_passes.size(); ++s) {
                            std::span<std::uint8_t const> const bytes(block.data.data() + offset, block.segment_bytes[s]);
                            segments.push_back({ bytes, block.segment_passes[s] });
                            offset += block.segment_bytes[s];
                        }
                        int const coded_planes = band.magnitude_planes + roi - block.zero_planes;
                        if (coded_planes <= 0)
                            continue;
                        int const bw = block.x1 - block.x0;
                        int const bh = block.y1 - block.y0;
                        decode_code_block(segments, bw, bh, coded_planes, band.band, tc.style.block, coefficients);
                        int const band_w = band.x1 - band.x0;
                        for (int y = 0; y < bh; ++y) {
                            for (int x = 0; x < bw; ++x) {
                                std::int32_t value = coefficients[static_cast<std::size_t>(y * bw + x)];
                                if (roi > 0) {
                                    // Max-shift: the region's coefficients were raised past the rest.
                                    std::int32_t const magnitude = value < 0 ? -value : value;
                                    if (magnitude >= (std::int32_t { 2 } << roi))
                                        value = value < 0 ? -(magnitude >> roi) : (magnitude >> roi);
                                }
                                band.coefficients[static_cast<std::size_t>((block.y0 - band.y0 + y) * band_w + (block.x0 - band.x0 + x))] = value;
                            }
                        }
                    }
                }
            }
        }
    }
    for (std::size_t c = 0; c < components.size(); ++c) {
        planes[c] = reconstruct(components[c]);
        extents[c] = { components[c].x0, components[c].y0, components[c].x1, components[c].y1 };
    }
    return true;
}

// --- JP2 boxes (Annex I) ---------------------------------------------------

struct Jp2 {
    std::span<std::uint8_t const> codestream;
    int enumerated_space = 0; // colr method 1: 16 sRGB, 17 gray, 18 sYCC, 12 CMYK
    // pclr: entries x columns, and their depths
    std::vector<std::vector<std::int32_t>> palette; // per column
    std::vector<int> palette_depths;
    // cmap: (component, palette column or -1) per output channel
    std::vector<std::pair<int, int>> mapping;
    // cdef: channel -> type (0 color, 1 opacity, 2 premultiplied) and association
    std::vector<std::array<int, 3>> channels; // channel, type, association
};

std::optional<Jp2> read_boxes(std::span<std::uint8_t const> data)
{
    Jp2 out;
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0x4F) {
        out.codestream = data;
        return out;
    }
    auto const walk = [&](auto const& self, std::span<std::uint8_t const> boxes) -> bool {
        Reader r { boxes };
        while (r.has(8)) {
            std::size_t const start = r.at;
            std::uint64_t length = r.u32();
            std::uint32_t const type = r.u32();
            std::size_t header = 8;
            if (length == 1) {
                length = (static_cast<std::uint64_t>(r.u32()) << 32) | r.u32();
                header = 16;
            } else if (length == 0) {
                length = boxes.size() - start;
            }
            if (length < header)
                return false;
            if (start + length > boxes.size()) {
                // A file cut short: its codestream is read as far as it goes.
                if (type != 0x6A703263)
                    return false;
                length = boxes.size() - start;
            }
            std::span<std::uint8_t const> const body = boxes.subspan(start + header, static_cast<std::size_t>(length) - header);
            Reader b { body };
            switch (type) {
            case 0x6A703268: // jp2h
                if (!self(self, body))
                    return false;
                break;
            case 0x636F6C72: { // colr
                std::uint32_t const method = b.u8();
                b.u8();
                b.u8();
                if (method == 1 && out.enumerated_space == 0)
                    out.enumerated_space = static_cast<int>(b.u32());
                break;
            }
            case 0x70636C72: { // pclr
                std::uint32_t const entries = b.u16();
                std::uint32_t const columns = b.u8();
                if (entries == 0 || entries > 1024 || columns == 0)
                    return false;
                for (std::uint32_t i = 0; i < columns; ++i)
                    out.palette_depths.push_back(static_cast<int>(b.u8() & 0x7F) + 1);
                out.palette.assign(columns, std::vector<std::int32_t>(entries));
                for (std::uint32_t e = 0; e < entries; ++e) {
                    for (std::uint32_t i = 0; i < columns; ++i) {
                        int const depth = out.palette_depths[i];
                        std::uint32_t v = 0;
                        for (int k = 0; k < (depth + 7) / 8; ++k)
                            v = (v << 8) | b.u8();
                        out.palette[i][e] = static_cast<std::int32_t>(v);
                    }
                }
                break;
            }
            case 0x636D6170: // cmap
                while (b.has(4)) {
                    int const component = static_cast<int>(b.u16());
                    int const kind = static_cast<int>(b.u8());
                    int const column = static_cast<int>(b.u8());
                    out.mapping.push_back({ component, kind == 1 ? column : -1 });
                }
                break;
            case 0x63646566: { // cdef
                std::uint32_t const n = b.u16();
                for (std::uint32_t i = 0; i < n && b.has(6); ++i)
                    out.channels.push_back({ static_cast<int>(b.u16()), static_cast<int>(b.u16()), static_cast<int>(b.u16()) });
                break;
            }
            case 0x6A703263: // jp2c
                if (out.codestream.empty())
                    out.codestream = body;
                break;
            default:
                break;
            }
            r.at = start + static_cast<std::size_t>(length);
        }
        return true;
    };
    if (!walk(walk, data) || out.codestream.empty())
        return std::nullopt;
    return out;
}

std::uint8_t to_byte(float value, int depth)
{
    float const top = static_cast<float>((1 << depth) - 1);
    float const v = std::clamp(value, 0.0f, top);
    return static_cast<std::uint8_t>(std::lround(v * 255.0f / top));
}

} // namespace

std::optional<CodecPicture> decode_jpx(std::vector<std::uint8_t> const& data, std::size_t max_pixels)
{
    std::optional<Jp2> const jp2 = read_boxes(data);
    if (!jp2)
        return std::nullopt;
    std::optional<Codestream> const cs = read_codestream(jp2->codestream);
    if (!cs)
        return std::nullopt;
    int const width = cs->x1 - cs->x0;
    int const height = cs->y1 - cs->y0;
    if (width <= 0 || height <= 0 || static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > max_pixels)
        return std::nullopt;
    std::size_t const nc = cs->components.size();
    // Each component on its own grid, whole image.
    std::vector<std::vector<float>> image(nc);
    std::vector<std::array<int, 4>> grid(nc);
    for (std::size_t c = 0; c < nc; ++c) {
        Component const& ic = cs->components[c];
        grid[c] = { ceil_div(cs->x0, ic.dx), ceil_div(cs->y0, ic.dy), ceil_div(cs->x1, ic.dx), ceil_div(cs->y1, ic.dy) };
        image[c].assign(static_cast<std::size_t>(grid[c][2] - grid[c][0]) * static_cast<std::size_t>(grid[c][3] - grid[c][1]), 0.0f);
    }
    int const tiles = ceil_div(cs->x1 - cs->tx0, cs->tw) * ceil_div(cs->y1 - cs->ty0, cs->th);
    if (tiles <= 0 || tiles > 65535)
        return std::nullopt;
    for (int t = 0; t < tiles; ++t) {
        auto const own = cs->tile_parameters.find(t);
        Defaults const& d = own != cs->tile_parameters.end() ? own->second : cs->main;
        std::vector<std::vector<float>> planes(nc);
        std::vector<std::array<int, 4>> extents(nc);
        if (!decode_tile(*cs, t, d, planes, extents))
            return std::nullopt;
        // Annex G: the component transform, then the level shift.
        if (d.mct && nc >= 3 && planes[0].size() == planes[1].size() && planes[0].size() == planes[2].size()) {
            bool const reversible = d.coding[0].reversible;
            for (std::size_t i = 0; i < planes[0].size(); ++i) {
                float const y0 = planes[0][i], y1 = planes[1][i], y2 = planes[2][i];
                if (reversible) {
                    float const g = y0 - std::floor((y2 + y1) / 4);
                    planes[0][i] = y2 + g;
                    planes[1][i] = g;
                    planes[2][i] = y1 + g;
                } else {
                    planes[0][i] = y0 + 1.402f * y2;
                    planes[1][i] = y0 - 0.34413f * y1 - 0.71414f * y2;
                    planes[2][i] = y0 + 1.772f * y1;
                }
            }
        }
        for (std::size_t c = 0; c < nc; ++c) {
            Component const& ic = cs->components[c];
            float const shift = ic.is_signed ? 0.0f : static_cast<float>(1 << (ic.depth - 1));
            auto const [x0, y0, x1, y1] = extents[c];
            int const gw = grid[c][2] - grid[c][0];
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float value = planes[c][static_cast<std::size_t>((y - y0) * (x1 - x0) + (x - x0))] + shift;
                    if (d.coding[c].reversible)
                        value = std::round(value);
                    image[c][static_cast<std::size_t>((y - grid[c][1]) * gw + (x - grid[c][0]))] = value;
                }
            }
        }
    }
    // The channels: through the palette when there is one, alpha apart.
    struct Channel {
        std::vector<float> values; // on the full image grid
        int depth;
    };
    std::vector<Channel> channels;
    auto const full = [&](std::size_t c) {
        // A subsampled component stretched to the image grid.
        Component const& ic = cs->components[c];
        std::vector<float> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        int const gw = grid[c][2] - grid[c][0];
        int const gh = grid[c][3] - grid[c][1];
        for (int y = 0; y < height; ++y) {
            int const gy = std::clamp((cs->y0 + y) / ic.dy - grid[c][1], 0, gh - 1);
            for (int x = 0; x < width; ++x) {
                int const gx = std::clamp((cs->x0 + x) / ic.dx - grid[c][0], 0, gw - 1);
                out[static_cast<std::size_t>(y * width + x)] = image[c][static_cast<std::size_t>(gy * gw + gx)];
            }
        }
        return out;
    };
    if (!jp2->palette.empty() && !jp2->mapping.empty()) {
        for (auto const& [component, column] : jp2->mapping) {
            if (component < 0 || static_cast<std::size_t>(component) >= nc)
                return std::nullopt;
            std::vector<float> source = full(static_cast<std::size_t>(component));
            if (column < 0) {
                channels.push_back({ std::move(source), cs->components[static_cast<std::size_t>(component)].depth });
                continue;
            }
            if (static_cast<std::size_t>(column) >= jp2->palette.size())
                return std::nullopt;
            std::vector<std::int32_t> const& entries = jp2->palette[static_cast<std::size_t>(column)];
            for (float& v : source) {
                int const index = std::clamp(static_cast<int>(v), 0, static_cast<int>(entries.size()) - 1);
                v = static_cast<float>(entries[static_cast<std::size_t>(index)]);
            }
            channels.push_back({ std::move(source), jp2->palette_depths[static_cast<std::size_t>(column)] });
        }
    } else {
        for (std::size_t c = 0; c < nc; ++c)
            channels.push_back({ full(c), cs->components[c].depth });
    }
    // cdef: which channel is opacity.
    int alpha = -1;
    for (auto const& [channel, type, association] : jp2->channels) {
        (void)association;
        if ((type == 1 || type == 2) && channel >= 0 && static_cast<std::size_t>(channel) < channels.size())
            alpha = channel;
    }
    std::vector<Channel> color;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (static_cast<int>(i) != alpha)
            color.push_back(std::move(channels[i]));
    }
    CodecPicture picture;
    picture.width = width;
    picture.height = height;
    std::size_t const pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (alpha >= 0) {
        Channel const& a = channels[static_cast<std::size_t>(alpha)];
        picture.alpha.resize(pixels);
        for (std::size_t i = 0; i < pixels; ++i)
            picture.alpha[i] = to_byte(a.values[i], a.depth);
    }
    // sYCC (enumerated 18) to RGB, as T.800 Annex I says such a file is shown.
    if (jp2->enumerated_space == 18 && color.size() >= 3 && !(cs->main.mct)) {
        float const half = static_cast<float>(1 << (color[1].depth - 1));
        for (std::size_t i = 0; i < pixels; ++i) {
            float const y = color[0].values[i];
            float const cb = color[1].values[i] - half;
            float const cr = color[2].values[i] - half;
            color[0].values[i] = y + 1.402f * cr;
            color[1].values[i] = y - 0.344136f * cb - 0.714136f * cr;
            color[2].values[i] = y + 1.772f * cb;
        }
    }
    picture.components = color.size() >= 4 ? 4 : color.size() >= 3 ? 3 : 1;
    picture.samples.resize(pixels * static_cast<std::size_t>(picture.components));
    for (std::size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < picture.components; ++c) {
            Channel const& channel = color[static_cast<std::size_t>(c)];
            picture.samples[i * static_cast<std::size_t>(picture.components) + static_cast<std::size_t>(c)]
                = to_byte(channel.values[i], channel.depth);
        }
    }
    return picture;
}

}
