// ICC color management (src/paint/Icc.h) against profiles written out here
// byte by byte, so no profile file is needed: matrix/TRC profiles with
// parametric and gamma curves, a lut8Type gray profile, a lut16Type CMYK
// print profile with a B2A0 (for its black point), a lutAToBType RGB
// profile with every element, and a profile whose A2B0 and A2B1 differ.
// The expected sRGB values were computed by LittleCMS 2 from these same
// bytes (black-point compensation on, no optimization, double precision);
// `test_icc --dump DIR` writes the profiles out to check them again.

#include "Test.h"

#include "core/Bitmap.h"
#include "paint/Icc.h"
#include "pdf/Document.h"
#include "pdf/Render.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& b, unsigned v)
{
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}

void put32(Bytes& b, std::uint32_t v)
{
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}

void put_sig(Bytes& b, char const* s)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(s[i]));
}

void put_s15(Bytes& b, double v)
{
    put32(b, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v * 65536.0))));
}

void pad4(Bytes& b)
{
    while (b.size() % 4)
        b.push_back(0);
}

Bytes xyz_tag(double x, double y, double z)
{
    Bytes b;
    put_sig(b, "XYZ ");
    put32(b, 0);
    put_s15(b, x);
    put_s15(b, y);
    put_s15(b, z);
    return b;
}

Bytes gamma_curve(double g)
{
    Bytes b;
    put_sig(b, "curv");
    put32(b, 0);
    put32(b, 1);
    put16(b, static_cast<unsigned>(std::lround(g * 256)));
    return b;
}

Bytes identity_curve()
{
    Bytes b;
    put_sig(b, "curv");
    put32(b, 0);
    put32(b, 0);
    return b;
}

Bytes table_curve(std::function<double(double)> const& f, int count)
{
    Bytes b;
    put_sig(b, "curv");
    put32(b, 0);
    put32(b, static_cast<std::uint32_t>(count));
    for (int i = 0; i < count; ++i)
        put16(b, static_cast<unsigned>(std::lround(std::clamp(f(i / double(count - 1)), 0.0, 1.0) * 65535)));
    return b;
}

Bytes parametric(int function, std::vector<double> const& params)
{
    Bytes b;
    put_sig(b, "para");
    put32(b, 0);
    put16(b, static_cast<unsigned>(function));
    put16(b, 0);
    for (double p : params)
        put_s15(b, p);
    return b;
}

struct ProfileWriter {
    char const* profile_class = "mntr";
    char const* space = "RGB ";
    char const* pcs = "XYZ ";
    int version = 4;
    std::vector<std::pair<std::string, Bytes>> tags;

    void add(char const* name, Bytes data) { tags.emplace_back(name, std::move(data)); }

    Bytes bytes() const
    {
        Bytes data;
        std::vector<std::pair<std::size_t, std::size_t>> places;
        std::size_t const table_end = 128 + 4 + tags.size() * 12;
        for (auto const& [name, tag] : tags) {
            pad4(data);
            places.emplace_back(table_end + data.size(), tag.size());
            data.insert(data.end(), tag.begin(), tag.end());
        }
        pad4(data);
        Bytes b;
        put32(b, static_cast<std::uint32_t>(table_end + data.size()));
        put32(b, 0);
        b.push_back(static_cast<std::uint8_t>(version));
        b.push_back(version >= 4 ? 0x40 : 0x10);
        put16(b, 0);
        put_sig(b, profile_class);
        put_sig(b, space);
        put_sig(b, pcs);
        b.resize(36, 0);
        put_sig(b, "acsp");
        b.resize(68, 0);
        put_s15(b, 0.9642);
        put_s15(b, 1.0);
        put_s15(b, 0.8249);
        b.resize(128, 0);
        put32(b, static_cast<std::uint32_t>(tags.size()));
        for (std::size_t i = 0; i < tags.size(); ++i) {
            put_sig(b, tags[i].first.c_str());
            put32(b, static_cast<std::uint32_t>(places[i].first));
            put32(b, static_cast<std::uint32_t>(places[i].second));
        }
        b.insert(b.end(), data.begin(), data.end());
        return b;
    }
};

// lut16Type: identity input and output tables of two entries, the grid from `node`.
Bytes lut16(int inputs, int outputs, int grid, std::function<void(double const*, double*)> const& node)
{
    Bytes b;
    put_sig(b, "mft2");
    put32(b, 0);
    b.push_back(static_cast<std::uint8_t>(inputs));
    b.push_back(static_cast<std::uint8_t>(outputs));
    b.push_back(static_cast<std::uint8_t>(grid));
    b.push_back(0);
    for (int i = 0; i < 9; ++i)
        put_s15(b, i % 4 == 0 ? 1 : 0);
    put16(b, 2);
    put16(b, 2);
    for (int i = 0; i < inputs; ++i) {
        put16(b, 0);
        put16(b, 65535);
    }
    std::size_t nodes = 1;
    for (int i = 0; i < inputs; ++i)
        nodes *= static_cast<std::size_t>(grid);
    for (std::size_t n = 0; n < nodes; ++n) {
        double in[8] {};
        std::size_t rest = n;
        for (int i = inputs - 1; i >= 0; --i) {
            in[i] = static_cast<double>(rest % static_cast<std::size_t>(grid)) / (grid - 1);
            rest /= static_cast<std::size_t>(grid);
        }
        double out[8] {};
        node(in, out);
        for (int o = 0; o < outputs; ++o)
            put16(b, static_cast<unsigned>(std::lround(std::clamp(out[o], 0.0, 1.0) * 65535)));
    }
    for (int o = 0; o < outputs; ++o) {
        put16(b, 0);
        put16(b, 65535);
    }
    return b;
}

// Legacy 16-bit CIELAB, normalized (ICC.1 Annex A).
void lab_legacy(double l, double a, double bb, double* out)
{
    out[0] = l / 100 * 65280.0 / 65535.0;
    out[1] = (a + 128) * 256.0 / 65535.0;
    out[2] = (bb + 128) * 256.0 / 65535.0;
}

// The profiles.

Bytes srgb_profile()
{
    // The sRGB primaries adapted to D50 and the IEC 61966-2-1 curve as parametricCurveType 3.
    ProfileWriter p;
    p.add("wtpt", xyz_tag(0.9642, 1.0, 0.8249));
    p.add("rXYZ", xyz_tag(0.4360747, 0.2225045, 0.0139322));
    p.add("gXYZ", xyz_tag(0.3850649, 0.7168786, 0.0971045));
    p.add("bXYZ", xyz_tag(0.1430804, 0.0606169, 0.7141733));
    Bytes const curve = parametric(3, { 2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045 });
    p.add("rTRC", curve);
    p.add("gTRC", curve);
    p.add("bTRC", curve);
    return p.bytes();
}

Bytes wide_profile(bool swap_red_blue = false)
{
    // A wide-gamut matrix (primaries of a common wide RGB space, adapted to D50) with gamma 2.2.
    ProfileWriter p;
    p.version = 2;
    p.add("wtpt", xyz_tag(0.9505, 1.0, 1.089));
    Bytes const red = xyz_tag(0.6097, 0.3111, 0.0195);
    Bytes const blue = xyz_tag(0.1492, 0.0632, 0.7446);
    p.add("rXYZ", swap_red_blue ? blue : red);
    p.add("gXYZ", xyz_tag(0.2053, 0.6257, 0.0609));
    p.add("bXYZ", swap_red_blue ? red : blue);
    p.add("rTRC", gamma_curve(2.2));
    p.add("gTRC", table_curve([](double x) { return std::pow(x, 1.8); }, 1024));
    p.add("bTRC", parametric(4, { 2.2, 0.9, 0.1, 0.5, 0.05, 0.0, 0.0 }));
    return p.bytes();
}

Bytes gray_lut8_profile()
{
    // lut8Type, gray to Lab: a tone table and a grid of two nodes from black to white.
    ProfileWriter p;
    p.profile_class = "scnr";
    p.space = "GRAY";
    p.pcs = "Lab ";
    p.add("wtpt", xyz_tag(0.9642, 1.0, 0.8249));
    Bytes b;
    put_sig(b, "mft1");
    put32(b, 0);
    b.push_back(1);
    b.push_back(3);
    b.push_back(2);
    b.push_back(0);
    for (int i = 0; i < 9; ++i)
        put_s15(b, i % 4 == 0 ? 1 : 0);
    for (int i = 0; i < 256; ++i)
        b.push_back(static_cast<std::uint8_t>(std::lround(std::sqrt(i / 255.0) * 255)));
    // Nodes: black L* 0, white L* 100 with a slight warm tint.
    std::uint8_t const nodes[6] = { 0, 128, 128, 255, 131, 136 };
    b.insert(b.end(), nodes, nodes + 6);
    for (int o = 0; o < 3; ++o) {
        for (int i = 0; i < 256; ++i)
            b.push_back(static_cast<std::uint8_t>(i));
    }
    p.add("A2B0", b);
    return p.bytes();
}

void cmyk_node(double const* in, double* out, double lightness)
{
    double const c = in[0];
    double const m = in[1];
    double const y = in[2];
    double const k = in[3];
    double const l = lightness * (1 - 0.85 * k) * (1 - 0.35 * c - 0.45 * m - 0.1 * y + 0.12 * c * m);
    double const a = 70 * m - 40 * c - 5 * y;
    double const bb = 85 * y - 50 * c + 10 * m;
    lab_legacy(std::clamp(l, 0.0, 100.0), std::clamp(a, -128.0, 127.0), std::clamp(bb, -128.0, 127.0), out);
}

Bytes cmyk_profile()
{
    // A print profile on the Lab PCS (version 2): lut16Type tables with a
    // grid of 3 per ink; A2B0 slightly lighter than A2B1; a B2A0 back.
    ProfileWriter p;
    p.profile_class = "prtr";
    p.space = "CMYK";
    p.pcs = "Lab ";
    p.version = 2;
    p.add("wtpt", xyz_tag(0.95, 1.0, 0.80));
    p.add("A2B0", lut16(4, 3, 3, [](double const* in, double* out) { cmyk_node(in, out, 100); }));
    p.add("A2B1", lut16(4, 3, 3, [](double const* in, double* out) { cmyk_node(in, out, 96); }));
    p.add("B2A0", lut16(3, 4, 2, [](double const* in, double* out) {
        double const l = in[0] * 65535.0 / 65280.0;
        out[0] = out[1] = out[2] = 0.6 * (1 - l);
        out[3] = 0.9 * (1 - l);
    }));
    return p.bytes();
}

Bytes rgb_mab_profile()
{
    // lutAToBType with A curves, a CLUT of 3 per input (two bytes a value),
    // M curves, a matrix with an offset and B curves, to the XYZ PCS.
    Bytes b;
    put_sig(b, "mAB ");
    put32(b, 0);
    b.push_back(3);
    b.push_back(3);
    put16(b, 0);
    std::size_t const offsets_at = b.size();
    for (int i = 0; i < 5; ++i)
        put32(b, 0);
    auto const set_offset = [&](int which, std::size_t at) {
        for (int i = 0; i < 4; ++i)
            b[offsets_at + static_cast<std::size_t>(which) * 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(at >> (24 - 8 * i));
    };
    auto const curves = [&](Bytes const& one) {
        for (int i = 0; i < 3; ++i) {
            b.insert(b.end(), one.begin(), one.end());
            pad4(b);
        }
    };
    set_offset(0, b.size()); // B
    curves(identity_curve());
    set_offset(1, b.size()); // matrix
    double const matrix[12] = { 0.9, 0.05, 0.0, 0.0, 0.92, 0.04, 0.02, 0.0, 0.88, 0.01, 0.0, 0.02 };
    for (double v : matrix)
        put_s15(b, v);
    set_offset(2, b.size()); // M
    curves(parametric(0, { 1.1 }));
    set_offset(3, b.size()); // CLUT
    for (int i = 0; i < 16; ++i)
        b.push_back(i < 3 ? 3 : 0);
    b.push_back(2);
    b.push_back(0);
    b.push_back(0);
    b.push_back(0);
    for (int r = 0; r < 3; ++r) {
        for (int g = 0; g < 3; ++g) {
            for (int bl = 0; bl < 3; ++bl) {
                double const lr = r / 2.0;
                double const lg = g / 2.0;
                double const lb = bl / 2.0;
                double const x = 0.4361 * lr + 0.3851 * lg + 0.1431 * lb;
                double const y = 0.2225 * lr + 0.7169 * lg + 0.0606 * lb;
                double const z = 0.0139 * lr + 0.0971 * lg + 0.7142 * lb;
                for (double v : { x, y, z })
                    put16(b, static_cast<unsigned>(std::lround(std::clamp(v * 32768.0 / 65535.0, 0.0, 1.0) * 65535)));
            }
        }
    }
    pad4(b);
    set_offset(4, b.size()); // A
    curves(parametric(0, { 2.0 }));
    ProfileWriter p;
    p.profile_class = "spac";
    p.add("wtpt", xyz_tag(0.9642, 1.0, 0.8249));
    p.add("A2B0", b);
    return p.bytes();
}

struct Case {
    std::vector<float> in;
    int intent;
    std::array<int, 3> rgb; // LittleCMS's result, 8-bit
};

void check_profile(char const* name, Bytes const& bytes, std::vector<Case> const& cases, int tolerance)
{
    auto const profile = icc::Profile::parse(bytes);
    if (!CHECK(profile != nullptr)) {
        std::fprintf(stderr, "  %s did not parse\n", name);
        return;
    }
    for (Case const& c : cases) {
        auto const t = profile->transform(static_cast<icc::Intent>(c.intent));
        if (!CHECK(t != nullptr))
            continue;
        std::uint8_t fast[3];
        t->to_srgb8(c.in.data(), 1, fast);
        float single[3];
        t->to_srgb(c.in.data(), single);
        for (int k = 0; k < 3; ++k) {
            int const one = static_cast<int>(std::lround(single[k] * 255));
            bool const ok = std::abs(fast[k] - c.rgb[static_cast<std::size_t>(k)]) <= tolerance
                && std::abs(one - c.rgb[static_cast<std::size_t>(k)]) <= tolerance;
            if (!CHECK(ok))
                std::fprintf(stderr, "  %s intent %d channel %d: ours %d / %d, LittleCMS %d\n", name, c.intent, k, fast[k], one,
                    c.rgb[static_cast<std::size_t>(k)]);
        }
    }
}

void test_matrix_profiles()
{
    // sRGB by its numbers: no transform at all.
    auto const srgb = icc::Profile::parse(srgb_profile());
    CHECK(srgb != nullptr);
    if (srgb) {
        CHECK(srgb->transform(icc::Intent::RelativeColorimetric)->is_identity());
        CHECK(srgb->transform(icc::Intent::Perceptual)->is_identity());
        float const in[3] = { 0.25f, 0.5f, 0.75f };
        float out[3];
        srgb->transform(icc::Intent::RelativeColorimetric)->to_srgb(in, out);
        CHECK(out[0] == 0.25f && out[1] == 0.5f && out[2] == 0.75f);
    }
    // The matrix's columns are the primaries: with red's and blue's swapped, red shows blue.
    if (auto const swapped = icc::Profile::parse(wide_profile(true))) {
        float const red[3] = { 1, 0, 0 };
        float out[3];
        swapped->transform(icc::Intent::RelativeColorimetric)->to_srgb(red, out);
        CHECK(out[2] > 0.9f && out[0] < 0.1f);
    } else {
        CHECK(false);
    }
    // A wide-gamut profile with a gamma, a table and a parametric curve.
    check_profile("wide", wide_profile(),
        {
            { { 1, 0, 0 }, 1, { 255, 0, 0 } },
            { { 0, 1, 0 }, 1, { 0, 255, 0 } },
            { { 0, 0, 1 }, 1, { 0, 0, 255 } },
            { { 0.5f, 0.5f, 0.5f }, 1, { 121, 146, 141 } },
            { { 0.2f, 0.7f, 0.4f }, 1, { 0, 192, 113 } },
            { { 0.9f, 0.3f, 0.1f }, 3, { 255, 95, 41 } },
        },
        1);
}

void test_lut_profiles()
{
    check_profile("gray lut8", gray_lut8_profile(),
        {
            { { 0 }, 1, { 0, 0, 0 } },
            { { 0.25f }, 1, { 124, 118, 112 } },
            { { 0.6f }, 0, { 200, 190, 180 } },
            { { 1 }, 1, { 255, 253, 240 } },
        },
        2);
    check_profile("cmyk lut16", cmyk_profile(),
        {
            { { 0, 0, 0, 0 }, 1, { 243, 243, 243 } },
            { { 1, 0, 0, 0 }, 1, { 0, 171, 239 } },
            { { 0, 1, 0, 0 }, 0, { 236, 56, 118 } },
            { { 0.3f, 0.6f, 0.1f, 0.2f }, 1, { 169, 99, 123 } },
            { { 0.3f, 0.6f, 0.1f, 0.2f }, 0, { 176, 108, 131 } },
            { { 0.3f, 0.6f, 0.1f, 0.2f }, 2, { 176, 109, 132 } },
            { { 0.3f, 0.6f, 0.1f, 0.2f }, 3, { 168, 105, 124 } },
            { { 0, 0, 0, 1 }, 1, { 14, 14, 14 } },
            { { 1, 1, 1, 1 }, 1, { 40, 0, 0 } },
        },
        2);
    check_profile("rgb lutAToB", rgb_mab_profile(),
        {
            { { 0, 0, 0 }, 1, { 58, 0, 68 } },
            { { 1, 1, 1 }, 1, { 249, 240, 240 } },
            { { 0.8f, 0.2f, 0.4f }, 1, { 195, 11, 115 } },
            { { 0.1f, 0.9f, 0.5f }, 0, { 70, 211, 132 } },
        },
        2);
}

void test_intents_and_black_points()
{
    auto const cmyk = icc::Profile::parse(cmyk_profile());
    if (!CHECK(cmyk != nullptr))
        return;
    // A2B0 for perceptual, A2B1 for relative, A2B0 again for saturation
    // (it has no A2B2), A2B1 scaled by the media white for absolute.
    float const in[4] = { 0.3f, 0.6f, 0.1f, 0.2f };
    float p[3];
    float r[3];
    float s[3];
    float a[3];
    cmyk->transform(icc::Intent::Perceptual)->to_srgb(in, p);
    cmyk->transform(icc::Intent::RelativeColorimetric)->to_srgb(in, r);
    cmyk->transform(icc::Intent::Saturation)->to_srgb(in, s);
    cmyk->transform(icc::Intent::AbsoluteColorimetric)->to_srgb(in, a);
    CHECK(std::abs(p[1] - r[1]) > 0.01f);
    // Saturation runs A2B0 like perceptual, but with no table of its own
    // it has no black point to compensate, so it is a little off perceptual.
    CHECK(std::abs(p[1] - s[1]) < 0.01f);
    CHECK(std::abs(a[1] - r[1]) > 0.01f);
    // The black point of a print profile (version 2) for relative
    // colorimetric: the PCS black through B2A0 and back through A2B1.
    icc::Vec3 const black = cmyk->black_point(icc::Intent::RelativeColorimetric);
    CHECK(black[1] > 0.001 && black[1] < 0.2);
    CHECK(cmyk->black_point(icc::Intent::AbsoluteColorimetric)[1] == 0);
}

void test_broken_profiles()
{
    Bytes const good = wide_profile();
    CHECK(icc::Profile::parse(good) != nullptr);
    CHECK(icc::Profile::parse({}) == nullptr);
    CHECK(icc::Profile::parse(Bytes(good.begin(), good.begin() + 100)) == nullptr);
    // No 'acsp'.
    Bytes no_magic = good;
    no_magic[36] = 'x';
    CHECK(icc::Profile::parse(no_magic) == nullptr);
    // A tag count past the end of the profile.
    Bytes many = good;
    many[128] = 0x7F;
    CHECK(icc::Profile::parse(many) == nullptr);
    // A tag whose offset points past the end: that tag is dropped, and
    // without rXYZ there is no matrix, so nothing to transform by.
    Bytes far = good;
    for (std::size_t i = 0; i < 4; ++i)
        far[132 + 12 + 4 + i] = 0xFF;
    CHECK(icc::Profile::parse(far) == nullptr);
    // Truncated in the middle of its tags: the size in the header is
    // believed only as far as the bytes go.
    Bytes const cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
    CHECK(icc::Profile::parse(cut) == nullptr);
    // An unsupported class (a device link) and data space (YCbCr).
    Bytes link = good;
    link[12] = 'l';
    link[13] = 'i';
    link[14] = 'n';
    link[15] = 'k';
    CHECK(icc::Profile::parse(link) == nullptr);
    Bytes ycc = good;
    ycc[16] = 'Y';
    ycc[17] = 'C';
    ycc[18] = 'b';
    ycc[19] = 'r';
    CHECK(icc::Profile::parse(ycc) == nullptr);
    // Every truncation and every single-byte change of a table profile
    // either parses or does not; none may read out of bounds (the
    // sanitizer build checks that part).
    Bytes const lut = cmyk_profile();
    for (std::size_t n = 0; n < lut.size(); n += 7) {
        auto const p = icc::Profile::parse(Bytes(lut.begin(), lut.begin() + static_cast<std::ptrdiff_t>(n)));
        if (p && p->transform(icc::Intent::Perceptual)) {
            float const in[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
            float out[3];
            p->transform(icc::Intent::Perceptual)->to_srgb(in, out);
        }
    }
    for (std::size_t i = 128; i < lut.size(); i += 3) {
        Bytes changed = lut;
        changed[i] = static_cast<std::uint8_t>(changed[i] ^ 0xA5);
        if (auto const p = icc::Profile::parse(changed)) {
            if (auto const t = p->transform(icc::Intent::RelativeColorimetric)) {
                float const in[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
                std::uint8_t out[3];
                t->to_srgb8(in, 1, out);
            }
        }
    }
}

void test_color_math()
{
    // Bradford from a white to itself is the identity; D65 to D50 takes D65 to D50.
    icc::Mat3 const same = icc::bradford(icc::k_d50, icc::k_d50);
    for (std::size_t i = 0; i < 9; ++i)
        CHECK(std::abs(same[i] - (i % 4 == 0 ? 1.0 : 0.0)) < 1e-12);
    icc::Vec3 const d50 = icc::transform_by(icc::bradford(icc::k_d65, icc::k_d50), icc::k_d65);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(std::abs(d50[i] - icc::k_d50[i]) < 1e-9);
    // The PCS white is sRGB's white; Lab round trips.
    icc::Vec3 const white = icc::pcs_to_linear_srgb(icc::k_d50);
    for (double v : white)
        CHECK(std::abs(v - 1) < 1e-6);
    icc::Vec3 const lab { 52.3, -31.5, 18.25 };
    icc::Vec3 const back = icc::xyz_to_lab(icc::lab_to_xyz(lab, icc::k_d50), icc::k_d50);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(std::abs(back[i] - lab[i]) < 1e-9);
    CHECK(std::abs(icc::srgb_encode(icc::srgb_decode(0.5)) - 0.5) < 1e-12);
    // The five parametric functions at a point each (ICC.1 table 68).
    icc::Curve c;
    c.kind = icc::Curve::Kind::Parametric;
    c.params = { 2.0, 0.5, 0.25, 0.1, 0.2, 0.05, 0.01 };
    double const expected[5] = { 0.36, std::pow(0.55, 2.0), std::pow(0.55, 2.0) + 0.1, std::pow(0.55, 2.0), std::pow(0.55, 2.0) + 0.05 };
    for (int f = 0; f < 5; ++f) {
        c.function = f;
        CHECK(std::abs(c.eval(0.6) - expected[f]) < 1e-12);
    }
    c.function = 4;
    CHECK(std::abs(c.eval(0.1) - (0.1 * 0.1 + 0.01)) < 1e-12);
}

// A one-page file of 100 x 100 points; objects from 5 up are the caller's.
Bytes page_file(std::string const& content, std::string const& resources, std::vector<Bytes> const& extra)
{
    std::vector<Bytes> objects;
    auto const text = [](std::string const& s) { return Bytes(s.begin(), s.end()); };
    objects.push_back(text("<< /Type /Catalog /Pages 2 0 R >>"));
    objects.push_back(text("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"));
    objects.push_back(text("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources " + resources + " /Contents 4 0 R >>"));
    objects.push_back(text("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream"));
    objects.insert(objects.end(), extra.begin(), extra.end());
    Bytes out = text("%PDF-1.7\n");
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        Bytes const head = text(std::to_string(i + 1) + " 0 obj\n");
        out.insert(out.end(), head.begin(), head.end());
        out.insert(out.end(), objects[i].begin(), objects[i].end());
        Bytes const tail = text("\nendobj\n");
        out.insert(out.end(), tail.begin(), tail.end());
    }
    std::size_t const xref = out.size();
    std::string table = "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (std::size_t offset : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
        table += line;
    }
    table += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    Bytes const t = text(table);
    out.insert(out.end(), t.begin(), t.end());
    return out;
}

Bytes stream_object(std::string const& dict, Bytes const& data)
{
    std::string const head = "<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n";
    Bytes b(head.begin(), head.end());
    b.insert(b.end(), data.begin(), data.end());
    std::string const tail = "\nendstream";
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

void test_pdf_pages()
{
    // ICCBased fills by the graphics state's intent (ri), a cut profile
    // falling back to its /Alternate, and ICCBased images (one with its own
    // /Intent) through the pictures' path: LittleCMS's numbers again.
    Bytes const wide = wide_profile();
    Bytes const cmyk = cmyk_profile();
    Bytes const cut(wide.begin(), wide.begin() + 60);
    Bytes const gray_image { 128, 128, 128 };
    Bytes const cmyk_image { 77, 153, 26, 51 };
    std::string const content = "/W cs 0.5 0.5 0.5 sc 0 80 20 20 re f\n"
                                "/Cut cs 0 0 1 sc 20 80 20 20 re f\n"
                                "/P cs 0.3 0.6 0.1 0.2 sc 40 80 20 20 re f\n"
                                "/Perceptual ri 0.3 0.6 0.1 0.2 sc 60 80 20 20 re f\n"
                                "/RelativeColorimetric ri\n"
                                "q 20 0 0 20 0 40 cm /G Do Q\n"
                                "q 20 0 0 20 20 40 cm /C Do Q\n"
                                "q 20 0 0 20 40 40 cm /CP Do Q\n";
    std::string const resources = "<< /ColorSpace << /W [/ICCBased 5 0 R] /P [/ICCBased 6 0 R] /Cut [/ICCBased 7 0 R] >>"
                                  " /XObject << /G 8 0 R /C 9 0 R /CP 10 0 R >> >>";
    std::string const image = "/Type /XObject /Subtype /Image /Width 1 /Height 1 /BitsPerComponent 8 ";
    Bytes const file = page_file(content, resources,
        {
            stream_object("/N 3 /Alternate /DeviceRGB", wide),
            stream_object("/N 4 /Alternate /DeviceCMYK", cmyk),
            stream_object("/N 3 /Alternate /DeviceRGB", cut),
            stream_object(image + "/ColorSpace [/ICCBased 5 0 R]", gray_image),
            stream_object(image + "/ColorSpace [/ICCBased 6 0 R]", cmyk_image),
            stream_object(image + "/Intent /Perceptual /ColorSpace [/ICCBased 6 0 R]", cmyk_image),
        });
    pdf::Document::Status status {};
    auto const doc = pdf::Document::open(file, "", status);
    if (!CHECK(doc != nullptr))
        return;
    std::optional<Bitmap> const picture = pdf::render_page(*doc, 0);
    if (!CHECK(picture.has_value()))
        return;
    auto const near = [&](int x, int y, int r, int g, int b) {
        Color const c = picture->pixel(x, y);
        bool const ok = std::abs(c.r - r) <= 1 && std::abs(c.g - g) <= 1 && std::abs(c.b - b) <= 1;
        if (!ok)
            std::fprintf(stderr, "  pixel (%d, %d) is %d %d %d, wanted %d %d %d\n", x, y, c.r, c.g, c.b, r, g, b);
        return ok;
    };
    CHECK(near(10, 10, 121, 146, 141));
    CHECK(near(30, 10, 0, 0, 255));
    CHECK(near(50, 10, 169, 99, 123));
    CHECK(near(70, 10, 176, 108, 131));
    CHECK(near(10, 50, 121, 146, 142));
    CHECK(near(30, 50, 168, 99, 123));
    CHECK(near(50, 50, 176, 108, 131));
}

void dump(std::string const& dir)
{
    auto const write = [&](char const* name, Bytes const& b) {
        std::ofstream f(dir + "/" + name, std::ios::binary);
        f.write(reinterpret_cast<char const*>(b.data()), static_cast<std::streamsize>(b.size()));
    };
    write("srgb.icc", srgb_profile());
    write("wide.icc", wide_profile());
    write("gray-lut8.icc", gray_lut8_profile());
    write("cmyk-lut16.icc", cmyk_profile());
    write("rgb-mab.icc", rgb_mab_profile());
}

}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--dump") {
        dump(argv[2]);
        return 0;
    }
    test_color_math();
    test_matrix_profiles();
    test_lut_profiles();
    test_intents_and_black_points();
    test_broken_profiles();
    test_pdf_pages();
    return sashfold::test::report("test_icc");
}
