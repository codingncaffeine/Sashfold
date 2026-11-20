#pragma once

// Color management by ICC profiles (ISO 15076-1 = ICC.1:2022, and the
// version 2 profiles of ICC.1:2001-04): a profile's header and tag table
// read defensively, its device-to-PCS transform made of the matrix and tone
// curves (rXYZ/gXYZ/bXYZ + rTRC/gTRC/bTRC, or kTRC) or of an A2B lookup
// table (lut8Type, lut16Type, lutAToBType), and the PCS taken on to sRGB
// for the screen: CIELAB to XYZ, the D50 PCS to sRGB's D65 white by the
// Bradford adaptation, the sRGB transfer curve (IEC 61966-2-1).
//
// The rendering intent picks the A2B table by the order ICC.1 §8 gives
// (A2B0, A2B1, A2B2, and A2B1 scaled by the media white for absolute
// colorimetric; A2B0 when the intent's own table is missing; the matrix
// and curves when there is no table). Black-point compensation
// (ISO 18619) maps the profile's black to sRGB's for every intent but
// absolute colorimetric, as the PDF viewers that run a color-management
// engine do.
//
// A transform is built once per profile and intent into a fast path,
// because pictures push millions of pixels through it: a matrix and three
// tables for matrix/TRC profiles; for table-based ones the profile's own
// lookup table with every curve in it sampled once (its grid interpolated
// tetrahedrally across three inputs, linearly across a fourth), the PCS
// to sRGB folded into one matrix, and the sRGB curve as a table; nothing
// at all for a profile that is sRGB.
//
// PDF's ICCBased, CalGray and CalRGB spaces use it (pdf/ColorSpace.cpp).
// A web picture's embedded profile (a PNG iCCP chunk, a JPEG APP2
// ICC_PROFILE segment) would go through the same Profile::parse and
// Transform::to_srgb8 where core/Png.cpp and core/Jpeg.cpp hand their
// decoded rows on; that is not wired yet.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::icc {

enum class Intent : std::uint8_t { Perceptual = 0, RelativeColorimetric = 1, Saturation = 2, AbsoluteColorimetric = 3 };

enum class DataSpace : std::uint8_t { Gray, RGB, CMYK, Lab, XYZ, Other };
enum class ProfileClass : std::uint8_t { Input, Display, Output, ColorSpace, Abstract, Link, NamedColor, Other };

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>; // row-major

// The PCS illuminant (ICC.1 §7.2.16) and sRGB's white (IEC 61966-2-1, x 0.3127 y 0.3290).
inline constexpr Vec3 k_d50 { 0.9642, 1.0, 0.8249 };
inline constexpr Vec3 k_d65 { 0.3127 / 0.3290, 1.0, (1 - 0.3127 - 0.3290) / 0.3290 };

Mat3 multiply(Mat3 const& a, Mat3 const& b);
Vec3 transform_by(Mat3 const& m, Vec3 const& v);
std::optional<Mat3> invert(Mat3 const& m);
// The Bradford chromatic adaptation from one white to another (ICC.1 Annex E).
Mat3 bradford(Vec3 const& from_white, Vec3 const& to_white);
// CIELAB relative to `white` to XYZ, and back (ISO/CIE 11664-4).
Vec3 lab_to_xyz(Vec3 const& lab, Vec3 const& white);
Vec3 xyz_to_lab(Vec3 const& xyz, Vec3 const& white);
// XYZ relative to the D50 PCS to linear sRGB (the sRGB primaries adapted
// to D50 by Bradford, inverted), unclipped.
Vec3 pcs_to_linear_srgb(Vec3 const& xyz);
// The sRGB transfer curve, linear light to the encoded value, both in [0, 1].
double srgb_encode(double linear);
double srgb_decode(double encoded);

// A tone curve: curveType (identity, a gamma, or a table) or
// parametricCurveType, functions 0 to 4 (ICC.1 §10.6, §10.18).
struct Curve {
    enum class Kind : std::uint8_t { Identity, Gamma, Table, Parametric };
    Kind kind = Kind::Identity;
    int function = 0; // parametric: 0 to 4
    std::array<double, 7> params {}; // g a b c d e f; Gamma: params[0]
    std::vector<double> table; // normalized to [0, 1]

    static Curve gamma(double g);
    double eval(double x) const;
};

// Black-point compensation (ISO 18619): D50 XYZ scaled per component so
// `black` goes to zero and the PCS white stays.
struct BlackPoint {
    Vec3 scale { 1, 1, 1 };
    Vec3 offset { 0, 0, 0 };
    static BlackPoint compensating(Vec3 const& black);
    Vec3 apply(Vec3 const& xyz) const;
};

class Profile;
struct CompiledTable; // a three- or four-input table laid out for the pixel loop (Icc.cpp)

// Device values to sRGB by one profile and intent. Device values are in
// [0, 1] for gray, RGB and CMYK; a Lab profile's are L* in [0, 100] and
// a*, b* in [-128, 127].
class Transform {
public:
    int channels() const { return m_channels; }
    // An sRGB profile: the values pass through.
    bool is_identity() const { return m_kind == Kind::Identity; }
    // Whether the fast path runs the profile's lookup table (table-based profiles).
    bool is_table() const { return m_kind == Kind::Table; }

    // One color to sRGB in [0, 1], through the fast path.
    void to_srgb(float const* in, float* rgb) const;
    // `count` colors of channels() values each to 8-bit sRGB triples.
    void to_srgb8(float const* in, std::size_t count, std::uint8_t* rgb) const;
    // The exact evaluation in double precision that the fast path is built from.
    void to_srgb_exact(double const* in, double* rgb) const;

    // A calibrated space (PDF's CalGray, CalRGB): `curves` (one or three)
    // to linear values, `to_pcs` to D50 XYZ (columns past the curves'
    // count unused), then `black` compensated when it is not zero.
    static std::shared_ptr<Transform const> shaper_matrix(std::vector<Curve> curves, Mat3 const& to_pcs, Vec3 const& black);

private:
    friend class Profile;
    enum class Kind : std::uint8_t { Identity, ShaperMatrix, Table };

    void exact_pcs(double const* in, Vec3& xyz) const;
    void table_linear(float const* in, double* linear) const;
    void finish_shaper();

    Kind m_kind = Kind::Table;
    int m_channels = 3;
    // ShaperMatrix: the curves, then linear device to linear sRGB (3x3 and
    // an offset). Table: PCS XYZ to linear sRGB.
    std::vector<Curve> m_curves;
    std::vector<float> m_curve_tables; // channels x (k_curve_steps + 1)
    std::array<double, 12> m_to_linear {};
    // Table: the profile's own evaluation to the PCS first; for pictures,
    // the same compiled to floats when the table has three or four inputs.
    Profile const* m_profile = nullptr;
    std::shared_ptr<CompiledTable const> m_compiled;
    Intent m_intent = Intent::RelativeColorimetric;
    BlackPoint m_black;
};

class Profile : public std::enable_shared_from_this<Profile> {
public:
    // Null when the bytes are not a profile this module can use (an
    // unsupported class, a data space other than gray, RGB, CMYK or Lab, a
    // PCS other than XYZ or Lab, or no usable tags).
    static std::shared_ptr<Profile const> parse(std::span<std::uint8_t const> bytes);

    DataSpace data_space() const { return m_space; }
    ProfileClass profile_class() const { return m_class; }
    int channels() const;
    int major_version() const { return m_version; }
    Vec3 media_white() const { return m_white; }
    // The chromatic adaptation the profile's maker applied to reach the
    // PCS (chad), when it states one.
    std::optional<Mat3> const& adaptation() const { return m_chad; }

    // The transform by `intent`, built on first use; it keeps the profile
    // alive while it is held. Null when the profile has no table or matrix
    // to make one from.
    std::shared_ptr<Transform const> transform(Intent intent) const;

    // Device values (as Transform takes them) to PCS XYZ by an intent:
    // media-relative, or ICC-absolute for absolute colorimetric.
    bool to_pcs(Intent intent, double const* in, Vec3& xyz) const;
    // The table's or matrix's own PCS value (CIELAB or XYZ, `lab` says
    // which), media-relative; `fast` evaluates the table's curves from
    // their sampled tables.
    bool pcs_value(Intent intent, double const* in, Vec3& value, bool& lab, bool fast = false) const;
    // The black point ISO 18619 compensates for this intent (zero for none).
    Vec3 black_point(Intent intent) const;

    struct Lut;
    struct Clut;

    Profile();
    ~Profile();
    Profile(Profile const&) = delete;
    Profile& operator=(Profile const&) = delete;

private:
    std::unique_ptr<Transform> build(Intent intent) const;
    Vec3 absolute_scale(Intent intent) const;
    Lut const* table_for(Intent intent) const;
    bool is_matrix_shaper() const;
    bool darker_colorant(Intent intent, Vec3& black) const;
    bool perceptual_round_trip(Vec3& black) const;

    DataSpace m_space = DataSpace::Other;
    ProfileClass m_class = ProfileClass::Other;
    bool m_pcs_lab = false;
    int m_version = 4;
    Vec3 m_white = k_d50;
    std::optional<Mat3> m_chad;
    // Matrix/TRC.
    bool m_has_matrix = false;
    Mat3 m_matrix {}; // columns rXYZ gXYZ bXYZ
    std::vector<Curve> m_trc; // three, or one (kTRC)
    // A2B0, A2B1, A2B2 and B2A0.
    std::array<std::unique_ptr<Lut>, 3> m_a2b;
    std::unique_ptr<Lut> m_b2a0;

    mutable std::array<std::once_flag, 4> m_once;
    mutable std::array<std::unique_ptr<Transform const>, 4> m_transforms;
};

}
