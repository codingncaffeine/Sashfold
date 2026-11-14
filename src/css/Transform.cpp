#include "css/Transform.h"

#include "css/Easing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sashfold::css {

namespace {

constexpr double pi = 3.14159265358979323846;

// The sine and cosine of an angle in degrees, exact at the quarter turns:
// rotate(90deg) is a matrix of zeros and ones, not of 6e-17s.
void sin_cos(double degrees, double& sine, double& cosine)
{
    double const turned = std::fmod(degrees, 360.0);
    double const quarter = turned / 90.0;
    if (quarter == std::floor(quarter)) {
        int const index = ((static_cast<int>(quarter) % 4) + 4) % 4;
        static constexpr double sines[] = { 0, 1, 0, -1 };
        static constexpr double cosines[] = { 1, 0, -1, 0 };
        sine = sines[index];
        cosine = cosines[index];
        return;
    }
    double const radians = turned * pi / 180.0;
    sine = std::sin(radians);
    cosine = std::cos(radians);
}

double tangent(double degrees)
{
    double const turned = std::fmod(degrees, 180.0);
    if (turned == 0)
        return 0;
    if (turned == 45 || turned == -135)
        return 1;
    if (turned == -45 || turned == 135)
        return -1;
    return std::tan(turned * pi / 180.0);
}

double lerp(double a, double b, double p) { return a + (b - a) * p; }

TransformLength lerp(TransformLength const& a, TransformLength const& b, double p)
{
    return TransformLength { lerp(a.px, b.px, p), lerp(a.percent, b.percent, p) };
}

std::string number(double value) { return serialize_css_number(value); }

std::string length(TransformLength const& value)
{
    if (value.percent == 0)
        return number(value.px) + "px";
    if (value.px == 0)
        return number(value.percent) + "%";
    return "calc(" + number(value.percent) + "% + " + number(value.px) + "px)";
}

// --- What a matrix is made of ---------------------------------------------------

// A 2D matrix as a move, a turn, a residue of shear, and a scale along each
// axis: its linear part is turn * residue * scale.
struct Parts2d {
    double move_x = 0;
    double move_y = 0;
    double scale_x = 1;
    double scale_y = 1;
    double degrees = 0;
    double k11 = 1, k12 = 0, k21 = 0, k22 = 1; // k[row][column]
};

std::optional<Parts2d> parts_2d(Matrix4 const& matrix)
{
    double a = matrix.m[0][0];
    double b = matrix.m[0][1];
    double c = matrix.m[1][0];
    double d = matrix.m[1][1];
    double const determinant = a * d - b * c;
    if (determinant == 0)
        return std::nullopt;
    Parts2d parts;
    parts.move_x = matrix.m[3][0];
    parts.move_y = matrix.m[3][1];
    parts.scale_x = std::hypot(a, b);
    parts.scale_y = std::hypot(c, d);
    // A flip is given to one axis.
    if (determinant < 0) {
        if (a < d)
            parts.scale_x = -parts.scale_x;
        else
            parts.scale_y = -parts.scale_y;
    }
    a /= parts.scale_x;
    b /= parts.scale_x;
    c /= parts.scale_y;
    d /= parts.scale_y;
    double const angle = std::atan2(b, a);
    parts.degrees = angle * 180.0 / pi;
    // The turn taken back out: what is left holds the shear.
    double const cosine = std::cos(angle);
    double const sine = std::sin(angle);
    parts.k11 = cosine * a + sine * b;
    parts.k21 = -sine * a + cosine * b;
    parts.k12 = cosine * c + sine * d;
    parts.k22 = -sine * c + cosine * d;
    return parts;
}

Matrix4 from_parts(Parts2d const& parts)
{
    double sine = 0;
    double cosine = 1;
    sin_cos(parts.degrees, sine, cosine);
    // turn * residue
    double const l11 = cosine * parts.k11 - sine * parts.k21;
    double const l12 = cosine * parts.k12 - sine * parts.k22;
    double const l21 = sine * parts.k11 + cosine * parts.k21;
    double const l22 = sine * parts.k12 + cosine * parts.k22;
    return Matrix4::affine(l11 * parts.scale_x, l21 * parts.scale_x, l12 * parts.scale_y, l22 * parts.scale_y,
        parts.move_x, parts.move_y);
}

Matrix4 interpolate_2d(Parts2d a, Parts2d b, double p)
{
    // One flipped along x and the other along y: the same picture as a
    // half turn, which is the shorter way between them.
    if ((a.scale_x < 0 && b.scale_y < 0) || (a.scale_y < 0 && b.scale_x < 0)) {
        a.scale_x = -a.scale_x;
        a.scale_y = -a.scale_y;
        a.degrees += a.degrees < 0 ? 180 : -180;
    }
    // Not the long way round.
    if (a.degrees == 0)
        a.degrees = 360;
    if (b.degrees == 0)
        b.degrees = 360;
    if (std::fabs(a.degrees - b.degrees) > 180) {
        if (a.degrees > b.degrees)
            a.degrees -= 360;
        else
            b.degrees -= 360;
    }
    Parts2d out;
    out.move_x = lerp(a.move_x, b.move_x, p);
    out.move_y = lerp(a.move_y, b.move_y, p);
    out.scale_x = lerp(a.scale_x, b.scale_x, p);
    out.scale_y = lerp(a.scale_y, b.scale_y, p);
    out.degrees = lerp(a.degrees, b.degrees, p);
    out.k11 = lerp(a.k11, b.k11, p);
    out.k12 = lerp(a.k12, b.k12, p);
    out.k21 = lerp(a.k21, b.k21, p);
    out.k22 = lerp(a.k22, b.k22, p);
    return from_parts(out);
}

// A matrix as perspective * move * turn * shear * scale (css-transforms-2
// §11, after Thomas, "Decomposing a matrix into simple transformations").
struct Parts3d {
    std::array<double, 4> perspective { 0, 0, 0, 1 };
    std::array<double, 3> move { 0, 0, 0 };
    std::array<double, 3> scale { 1, 1, 1 };
    std::array<double, 3> skew { 0, 0, 0 }; // xy, xz, yz
    std::array<double, 4> turn { 0, 0, 0, 1 }; // a quaternion: x, y, z, w
};

using Vector3 = std::array<double, 3>;

double dot(Vector3 const& a, Vector3 const& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vector3 cross(Vector3 const& a, Vector3 const& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
double norm(Vector3 const& a) { return std::sqrt(dot(a, a)); }
Vector3 scaled(Vector3 const& a, double by) { return { a[0] * by, a[1] * by, a[2] * by }; }
Vector3 combined(Vector3 const& a, Vector3 const& b, double with_a, double with_b)
{
    return { a[0] * with_a + b[0] * with_b, a[1] * with_a + b[1] * with_b, a[2] * with_a + b[2] * with_b };
}

std::optional<Parts3d> parts_3d(Matrix4 matrix)
{
    if (matrix.m[3][3] == 0)
        return std::nullopt;
    double const w = matrix.m[3][3];
    for (auto& column : matrix.m) {
        for (double& value : column)
            value /= w;
    }
    // The matrix without its perspective: what the bottom row is solved
    // against, and singular when the whole is.
    Matrix4 affine = matrix;
    for (int column = 0; column < 3; ++column)
        affine.m[static_cast<std::size_t>(column)][3] = 0;
    affine.m[3][3] = 1;
    std::optional<Matrix4> const inverse = affine.inverse();
    if (!inverse)
        return std::nullopt;
    Parts3d parts;
    if (matrix.m[0][3] != 0 || matrix.m[1][3] != 0 || matrix.m[2][3] != 0) {
        // bottom row = perspective * affine, so perspective = bottom row * inverse.
        for (std::size_t column = 0; column < 4; ++column) {
            double sum = 0;
            for (std::size_t k = 0; k < 4; ++k)
                sum += matrix.m[k][3] * inverse->m[column][k];
            parts.perspective[column] = sum;
        }
    }
    for (std::size_t i = 0; i < 3; ++i)
        parts.move[i] = matrix.m[3][i];
    // Where each axis goes: made orthonormal one after the other, the
    // lengths taken off as scale and the leanings as shear.
    std::array<Vector3, 3> axis;
    for (std::size_t i = 0; i < 3; ++i)
        axis[i] = { matrix.m[i][0], matrix.m[i][1], matrix.m[i][2] };
    parts.scale[0] = norm(axis[0]);
    if (parts.scale[0] == 0)
        return std::nullopt;
    axis[0] = scaled(axis[0], 1 / parts.scale[0]);
    parts.skew[0] = dot(axis[0], axis[1]);
    axis[1] = combined(axis[1], axis[0], 1, -parts.skew[0]);
    parts.scale[1] = norm(axis[1]);
    if (parts.scale[1] == 0)
        return std::nullopt;
    axis[1] = scaled(axis[1], 1 / parts.scale[1]);
    parts.skew[0] /= parts.scale[1];
    parts.skew[1] = dot(axis[0], axis[2]);
    axis[2] = combined(axis[2], axis[0], 1, -parts.skew[1]);
    parts.skew[2] = dot(axis[1], axis[2]);
    axis[2] = combined(axis[2], axis[1], 1, -parts.skew[2]);
    parts.scale[2] = norm(axis[2]);
    if (parts.scale[2] == 0)
        return std::nullopt;
    axis[2] = scaled(axis[2], 1 / parts.scale[2]);
    parts.skew[1] /= parts.scale[2];
    parts.skew[2] /= parts.scale[2];
    // A mirrored frame: turned back, the mirror given to the scale.
    if (dot(axis[0], cross(axis[1], axis[2])) < 0) {
        for (std::size_t i = 0; i < 3; ++i) {
            parts.scale[i] = -parts.scale[i];
            axis[i] = scaled(axis[i], -1);
        }
    }
    // The turn, as a quaternion. q[row][column] of the turn's matrix is
    // axis[column][row].
    auto const q = [&](std::size_t row, std::size_t column) { return axis[column][row]; };
    parts.turn[0] = 0.5 * std::sqrt(std::max(1 + q(0, 0) - q(1, 1) - q(2, 2), 0.0));
    parts.turn[1] = 0.5 * std::sqrt(std::max(1 - q(0, 0) + q(1, 1) - q(2, 2), 0.0));
    parts.turn[2] = 0.5 * std::sqrt(std::max(1 - q(0, 0) - q(1, 1) + q(2, 2), 0.0));
    parts.turn[3] = 0.5 * std::sqrt(std::max(1 + q(0, 0) + q(1, 1) + q(2, 2), 0.0));
    if (q(2, 1) < q(1, 2))
        parts.turn[0] = -parts.turn[0];
    if (q(0, 2) < q(2, 0))
        parts.turn[1] = -parts.turn[1];
    if (q(1, 0) < q(0, 1))
        parts.turn[2] = -parts.turn[2];
    return parts;
}

Matrix4 turn_matrix(std::array<double, 4> const& quaternion)
{
    double const x = quaternion[0];
    double const y = quaternion[1];
    double const z = quaternion[2];
    double const w = quaternion[3];
    Matrix4 out;
    // m[column][row]
    out.m[0][0] = 1 - 2 * (y * y + z * z);
    out.m[1][0] = 2 * (x * y - z * w);
    out.m[2][0] = 2 * (x * z + y * w);
    out.m[0][1] = 2 * (x * y + z * w);
    out.m[1][1] = 1 - 2 * (x * x + z * z);
    out.m[2][1] = 2 * (y * z - x * w);
    out.m[0][2] = 2 * (x * z - y * w);
    out.m[1][2] = 2 * (y * z + x * w);
    out.m[2][2] = 1 - 2 * (x * x + y * y);
    return out;
}

Matrix4 from_parts(Parts3d const& parts)
{
    Matrix4 shear;
    shear.m[1][0] = parts.skew[0];
    shear.m[2][0] = parts.skew[1];
    shear.m[2][1] = parts.skew[2];
    Matrix4 const affine = Matrix4::translation(parts.move[0], parts.move[1], parts.move[2]) * turn_matrix(parts.turn)
        * shear * Matrix4::scaling(parts.scale[0], parts.scale[1], parts.scale[2]);
    Matrix4 out = affine;
    // bottom row = perspective * affine
    for (std::size_t column = 0; column < 4; ++column) {
        double sum = 0;
        for (std::size_t k = 0; k < 4; ++k)
            sum += parts.perspective[k] * affine.m[column][k];
        out.m[column][3] = sum;
    }
    return out;
}

std::array<double, 4> slerp(std::array<double, 4> const& a, std::array<double, 4> const& b, double p)
{
    double product = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    product = std::clamp(product, -1.0, 1.0);
    if (std::fabs(product) == 1.0)
        return a;
    double const theta = std::acos(product);
    double const w = std::sin(p * theta) / std::sqrt(1 - product * product);
    double const from = std::cos(p * theta) - product * w;
    return { a[0] * from + b[0] * w, a[1] * from + b[1] * w, a[2] * from + b[2] * w, a[3] * from + b[3] * w };
}

Matrix4 interpolate_3d(Parts3d const& a, Parts3d const& b, double p)
{
    Parts3d out;
    for (std::size_t i = 0; i < 4; ++i)
        out.perspective[i] = lerp(a.perspective[i], b.perspective[i], p);
    for (std::size_t i = 0; i < 3; ++i) {
        out.move[i] = lerp(a.move[i], b.move[i], p);
        out.scale[i] = lerp(a.scale[i], b.scale[i], p);
        out.skew[i] = lerp(a.skew[i], b.skew[i], p);
    }
    out.turn = slerp(a.turn, b.turn, p);
    return from_parts(out);
}

// --- Function against function --------------------------------------------------

using Kind = TransformFunction::Kind;

enum class Family : std::uint8_t { Translate, Scale, Rotate, Skew, Matrix, Perspective, None };

Family family_of(Kind kind)
{
    switch (kind) {
    case Kind::Translate:
    case Kind::TranslateX:
    case Kind::TranslateY:
    case Kind::TranslateZ:
    case Kind::Translate3d:
        return Family::Translate;
    case Kind::Scale:
    case Kind::ScaleX:
    case Kind::ScaleY:
    case Kind::ScaleZ:
    case Kind::Scale3d:
        return Family::Scale;
    case Kind::Rotate:
    case Kind::RotateX:
    case Kind::RotateY:
    case Kind::RotateZ:
    case Kind::Rotate3d:
        return Family::Rotate;
    case Kind::Skew:
    case Kind::SkewX:
    case Kind::SkewY:
        return Family::Skew;
    case Kind::Matrix:
    case Kind::Matrix3d:
        return Family::Matrix;
    case Kind::Perspective:
        return Family::Perspective;
    case Kind::Mix:
        return Family::None;
    }
    return Family::None;
}

bool pairs_up(TransformFunction const& a, TransformFunction const& b)
{
    Family const family = family_of(a.kind);
    return family != Family::None && family == family_of(b.kind);
}

// The function that does nothing, of the same kind: what `none` and the
// end of a shorter list stand in for.
TransformFunction identity_of(TransformFunction const& like)
{
    TransformFunction out;
    out.kind = like.kind;
    switch (family_of(like.kind)) {
    case Family::Scale:
        out.n[0] = out.n[1] = out.n[2] = 1;
        out.n[15] = like.n[15];
        break;
    case Family::Rotate:
        out.n[0] = like.n[0];
        out.n[1] = like.n[1];
        out.n[2] = like.n[2];
        break;
    case Family::Matrix: {
        Matrix4 const identity;
        if (like.kind == Kind::Matrix) {
            out.n = { 1, 0, 0, 1, 0, 0 };
        } else {
            for (std::size_t i = 0; i < 16; ++i)
                out.n[i] = identity.m[i / 4][i % 4];
        }
        break;
    }
    case Family::Perspective:
        out.n[0] = -1;
        break;
    case Family::Translate:
    case Family::Skew:
        out.n[15] = like.n[15];
        break;
    case Family::None:
        out.kind = Kind::Matrix;
        out.n = { 1, 0, 0, 1, 0, 0 };
        break;
    }
    return out;
}

// The axis a rotation turns about, as a unit vector.
Vector3 axis_of(TransformFunction const& rotation)
{
    switch (rotation.kind) {
    case Kind::RotateX: return { 1, 0, 0 };
    case Kind::RotateY: return { 0, 1, 0 };
    case Kind::Rotate3d: {
        Vector3 const axis { rotation.n[0], rotation.n[1], rotation.n[2] };
        double const size = norm(axis);
        return size == 0 ? Vector3 { 0, 0, 1 } : scaled(axis, 1 / size);
    }
    default: return { 0, 0, 1 };
    }
}

TransformFunction matrix_function(Matrix4 const& matrix)
{
    TransformFunction out;
    if (matrix.is_2d()) {
        out.kind = Kind::Matrix;
        out.n = { matrix.m[0][0], matrix.m[0][1], matrix.m[1][0], matrix.m[1][1], matrix.m[3][0], matrix.m[3][1] };
    } else {
        out.kind = Kind::Matrix3d;
        for (std::size_t i = 0; i < 16; ++i)
            out.n[i] = matrix.m[i / 4][i % 4];
    }
    return out;
}

TransformFunction mix_of(std::vector<TransformFunction> from, std::vector<TransformFunction> to, double p)
{
    TransformFunction out;
    out.kind = Kind::Mix;
    out.n[0] = p;
    out.from = std::make_shared<TransformList const>(TransformList { std::move(from) });
    out.to = std::make_shared<TransformList const>(TransformList { std::move(to) });
    return out;
}

bool has_percent(TransformFunction const& f) { return f.x.percent != 0 || f.y.percent != 0; }

TransformFunction interpolate_pair(TransformFunction const& a, TransformFunction const& b, double p)
{
    TransformFunction out;
    switch (family_of(a.kind)) {
    case Family::Translate: {
        auto const flat = [](Kind kind) { return kind == Kind::Translate || kind == Kind::TranslateX || kind == Kind::TranslateY; };
        out.kind = a.kind == b.kind ? a.kind : (flat(a.kind) && flat(b.kind) ? Kind::Translate : Kind::Translate3d);
        out.x = lerp(a.x, b.x, p);
        out.y = lerp(a.y, b.y, p);
        out.n[0] = lerp(a.n[0], b.n[0], p);
        out.n[15] = a.kind == b.kind ? std::max(a.n[15], b.n[15]) : 2;
        return out;
    }
    case Family::Scale: {
        auto const flat = [](Kind kind) { return kind == Kind::Scale || kind == Kind::ScaleX || kind == Kind::ScaleY; };
        out.kind = a.kind == b.kind ? a.kind : (flat(a.kind) && flat(b.kind) ? Kind::Scale : Kind::Scale3d);
        for (std::size_t i = 0; i < 3; ++i)
            out.n[i] = lerp(a.n[i], b.n[i], p);
        out.n[15] = a.kind == b.kind ? std::max(a.n[15], b.n[15]) : 2;
        return out;
    }
    case Family::Skew: {
        out.kind = a.kind == b.kind ? a.kind : Kind::Skew;
        out.n[0] = lerp(a.n[0], b.n[0], p);
        out.n[1] = lerp(a.n[1], b.n[1], p);
        out.n[15] = a.kind == b.kind ? std::max(a.n[15], b.n[15]) : 2;
        return out;
    }
    case Family::Rotate: {
        Vector3 axis_a = axis_of(a);
        Vector3 axis_b = axis_of(b);
        // A turn of nothing is about any axis.
        if (a.n[3] == 0)
            axis_a = axis_b;
        else if (b.n[3] == 0)
            axis_b = axis_a;
        bool const same_axis = std::fabs(axis_a[0] - axis_b[0]) < 1e-9 && std::fabs(axis_a[1] - axis_b[1]) < 1e-9
            && std::fabs(axis_a[2] - axis_b[2]) < 1e-9;
        if (!same_axis)
            return mix_of({ a }, { b }, p);
        if (a.kind == b.kind && a.kind != Kind::Rotate3d) {
            out.kind = a.kind;
        } else if (axis_a[0] == 0 && axis_a[1] == 0 && axis_a[2] == 1 && a.kind != Kind::Rotate3d && b.kind != Kind::Rotate3d) {
            out.kind = Kind::Rotate;
        } else {
            out.kind = Kind::Rotate3d;
        }
        out.n[0] = axis_a[0];
        out.n[1] = axis_a[1];
        out.n[2] = axis_a[2];
        if (out.kind != Kind::Rotate3d)
            out.n[0] = out.n[1] = out.n[2] = 0;
        out.n[3] = lerp(a.n[3], b.n[3], p);
        return out;
    }
    case Family::Matrix:
        return matrix_function(interpolate(a.matrix(0, 0), b.matrix(0, 0), p));
    case Family::Perspective: {
        // perspective(d) meets perspective(e) in 1/d, the bottom row's own
        // number, which is what interpolating their matrices comes to;
        // none is a depth without end.
        double const from = a.n[0] < 0 ? 0 : 1 / std::max(a.n[0], 1.0);
        double const to = b.n[0] < 0 ? 0 : 1 / std::max(b.n[0], 1.0);
        double const met = lerp(from, to, p);
        out.kind = Kind::Perspective;
        out.n[0] = met <= 0 ? -1 : 1 / met;
        return out;
    }
    case Family::None:
        break;
    }
    return mix_of({ a }, { b }, p);
}

}

// --- Matrix4 -------------------------------------------------------------------

Matrix4 Matrix4::translation(double x, double y, double z)
{
    Matrix4 out;
    out.m[3][0] = x;
    out.m[3][1] = y;
    out.m[3][2] = z;
    return out;
}

Matrix4 Matrix4::scaling(double x, double y, double z)
{
    Matrix4 out;
    out.m[0][0] = x;
    out.m[1][1] = y;
    out.m[2][2] = z;
    return out;
}

Matrix4 Matrix4::rotation(double x, double y, double z, double degrees)
{
    double const size = std::sqrt(x * x + y * y + z * z);
    Matrix4 out;
    if (size == 0)
        return out;
    x /= size;
    y /= size;
    z /= size;
    double s = 0;
    double c = 1;
    sin_cos(degrees, s, c);
    double const t = 1 - c;
    out.m[0][0] = t * x * x + c;
    out.m[1][0] = t * x * y - s * z;
    out.m[2][0] = t * x * z + s * y;
    out.m[0][1] = t * x * y + s * z;
    out.m[1][1] = t * y * y + c;
    out.m[2][1] = t * y * z - s * x;
    out.m[0][2] = t * x * z - s * y;
    out.m[1][2] = t * y * z + s * x;
    out.m[2][2] = t * z * z + c;
    return out;
}

Matrix4 Matrix4::skewing(double degrees_x, double degrees_y)
{
    Matrix4 out;
    out.m[1][0] = tangent(degrees_x);
    out.m[0][1] = tangent(degrees_y);
    return out;
}

Matrix4 Matrix4::perspective(double depth)
{
    Matrix4 out;
    out.m[2][3] = -1 / std::max(depth, 1.0);
    return out;
}

Matrix4 Matrix4::affine(double a, double b, double c, double d, double e, double f)
{
    Matrix4 out;
    out.m[0][0] = a;
    out.m[0][1] = b;
    out.m[1][0] = c;
    out.m[1][1] = d;
    out.m[3][0] = e;
    out.m[3][1] = f;
    return out;
}

Matrix4 Matrix4::operator*(Matrix4 const& inner) const
{
    Matrix4 out;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            double sum = 0;
            for (std::size_t k = 0; k < 4; ++k)
                sum += m[k][row] * inner.m[column][k];
            out.m[column][row] = sum;
        }
    }
    return out;
}

bool Matrix4::is_identity() const { return *this == Matrix4 {}; }

bool Matrix4::is_2d() const
{
    return m[0][2] == 0 && m[0][3] == 0 && m[1][2] == 0 && m[1][3] == 0 && m[2][0] == 0 && m[2][1] == 0 && m[2][2] == 1
        && m[2][3] == 0 && m[3][2] == 0 && m[3][3] == 1;
}

bool Matrix4::is_translation_2d() const
{
    return is_2d() && m[0][0] == 1 && m[0][1] == 0 && m[1][0] == 0 && m[1][1] == 1;
}

namespace {

// The determinant of the 3x3 left when a column and a row are struck out.
double minor_of(Matrix4 const& matrix, std::size_t column, std::size_t row)
{
    std::array<std::array<double, 3>, 3> sub {};
    std::size_t c = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (i == column)
            continue;
        std::size_t r = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            if (j == row)
                continue;
            sub[c][r] = matrix.m[i][j];
            ++r;
        }
        ++c;
    }
    return sub[0][0] * (sub[1][1] * sub[2][2] - sub[2][1] * sub[1][2]) - sub[1][0] * (sub[0][1] * sub[2][2] - sub[2][1] * sub[0][2])
        + sub[2][0] * (sub[0][1] * sub[1][2] - sub[1][1] * sub[0][2]);
}

}

double Matrix4::determinant() const
{
    double sum = 0;
    for (std::size_t column = 0; column < 4; ++column)
        sum += (column % 2 == 0 ? 1 : -1) * m[column][0] * minor_of(*this, column, 0);
    return sum;
}

std::optional<Matrix4> Matrix4::inverse() const
{
    double const det = determinant();
    if (det == 0 || !std::isfinite(det))
        return std::nullopt;
    Matrix4 out;
    // The adjugate: each cofactor, transposed.
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            double const cofactor = ((column + row) % 2 == 0 ? 1 : -1) * minor_of(*this, column, row);
            out.m[row][column] = cofactor / det;
        }
    }
    return out;
}

bool Matrix4::shows_back() const
{
    // The plane's normal (0, 0, 1) after the transform points away from
    // the viewer: the z of the inverse's transpose applied to it.
    double const det = determinant();
    if (det == 0)
        return false;
    return minor_of(*this, 2, 2) / det < 0;
}

// --- PlaneTransform -------------------------------------------------------------

PlaneTransform PlaneTransform::of(Matrix4 const& matrix)
{
    PlaneTransform out;
    out.h = { matrix.m[0][0], matrix.m[1][0], matrix.m[3][0], matrix.m[0][1], matrix.m[1][1], matrix.m[3][1], matrix.m[0][3],
        matrix.m[1][3], matrix.m[3][3] };
    return out;
}

PlaneTransform PlaneTransform::translation(double x, double y)
{
    PlaneTransform out;
    out.h[2] = x;
    out.h[5] = y;
    return out;
}

PlaneTransform PlaneTransform::operator*(PlaneTransform const& inner) const
{
    PlaneTransform out;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            double sum = 0;
            for (std::size_t k = 0; k < 3; ++k)
                sum += h[row * 3 + k] * inner.h[k * 3 + column];
            out.h[row * 3 + column] = sum;
        }
    }
    return out;
}

bool PlaneTransform::is_identity() const { return *this == PlaneTransform {}; }

std::optional<PlaneTransform> PlaneTransform::inverse() const
{
    double const a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], i = h[7], j = h[8];
    double const det = a * (e * j - f * i) - b * (d * j - f * g) + c * (d * i - e * g);
    if (det == 0 || !std::isfinite(det))
        return std::nullopt;
    PlaneTransform out;
    out.h = { (e * j - f * i) / det, (c * i - b * j) / det, (b * f - c * e) / det, (f * g - d * j) / det, (a * j - c * g) / det,
        (c * d - a * f) / det, (d * i - e * g) / det, (b * g - a * i) / det, (a * e - b * d) / det };
    return out;
}

bool PlaneTransform::map(double x, double y, double& out_x, double& out_y) const
{
    double const w = h[6] * x + h[7] * y + h[8];
    if (!(w > 1e-9))
        return false;
    out_x = (h[0] * x + h[1] * y + h[2]) / w;
    out_y = (h[3] * x + h[4] * y + h[5]) / w;
    return true;
}

bool PlaneTransform::map_rect(double x, double y, double width, double height, double& left, double& top, double& right,
    double& bottom) const
{
    struct Point {
        double x, y, w;
    };
    std::array<Point, 4> const corners { {
        { x, y, 1 },
        { x + width, y, 1 },
        { x + width, y + height, 1 },
        { x, y + height, 1 },
    } };
    std::vector<Point> shape;
    shape.reserve(8);
    for (Point const& corner : corners) {
        shape.push_back({ h[0] * corner.x + h[1] * corner.y + h[2], h[3] * corner.x + h[4] * corner.y + h[5],
            h[6] * corner.x + h[7] * corner.y + h[8] });
    }
    // Cut at the plane just in front of the viewer.
    constexpr double near = 1e-6;
    std::vector<Point> cut;
    cut.reserve(8);
    for (std::size_t i = 0; i < shape.size(); ++i) {
        Point const& from = shape[i];
        Point const& to = shape[(i + 1) % shape.size()];
        bool const from_in = from.w >= near;
        bool const to_in = to.w >= near;
        if (from_in)
            cut.push_back(from);
        if (from_in != to_in) {
            double const t = (near - from.w) / (to.w - from.w);
            cut.push_back({ from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, near });
        }
    }
    if (cut.empty())
        return false;
    left = top = std::numeric_limits<double>::max();
    right = bottom = std::numeric_limits<double>::lowest();
    for (Point const& point : cut) {
        double const px = point.x / point.w;
        double const py = point.y / point.w;
        left = std::min(left, px);
        top = std::min(top, py);
        right = std::max(right, px);
        bottom = std::max(bottom, py);
    }
    return true;
}

PlaneTransform PlaneTransform::about(double x, double y) const
{
    return translation(x, y) * *this * translation(-x, -y);
}

// --- Functions and lists --------------------------------------------------------

bool TransformFunction::operator==(TransformFunction const& other) const
{
    if (kind != other.kind || !(x == other.x) || !(y == other.y) || n != other.n)
        return false;
    auto const same = [](std::shared_ptr<TransformList const> const& a, std::shared_ptr<TransformList const> const& b) {
        return a == b || (a && b && *a == *b);
    };
    return same(from, other.from) && same(to, other.to);
}

Matrix4 TransformFunction::matrix(double width, double height) const
{
    switch (kind) {
    case Kind::Matrix: return Matrix4::affine(n[0], n[1], n[2], n[3], n[4], n[5]);
    case Kind::Matrix3d: {
        Matrix4 out;
        for (std::size_t i = 0; i < 16; ++i)
            out.m[i / 4][i % 4] = n[i];
        return out;
    }
    case Kind::Translate:
    case Kind::TranslateX:
    case Kind::TranslateY:
    case Kind::TranslateZ:
    case Kind::Translate3d:
        return Matrix4::translation(x.resolve(width), y.resolve(height), n[0]);
    case Kind::Scale:
    case Kind::ScaleX:
    case Kind::ScaleY:
    case Kind::ScaleZ:
    case Kind::Scale3d:
        return Matrix4::scaling(n[0], n[1], n[2]);
    case Kind::Rotate:
    case Kind::RotateZ:
        return Matrix4::rotation(0, 0, 1, n[3]);
    case Kind::RotateX: return Matrix4::rotation(1, 0, 0, n[3]);
    case Kind::RotateY: return Matrix4::rotation(0, 1, 0, n[3]);
    case Kind::Rotate3d: return Matrix4::rotation(n[0], n[1], n[2], n[3]);
    case Kind::Skew:
    case Kind::SkewX:
    case Kind::SkewY:
        return Matrix4::skewing(n[0], n[1]);
    case Kind::Perspective: return n[0] < 0 ? Matrix4 {} : Matrix4::perspective(n[0]);
    case Kind::Mix: {
        Matrix4 const a = from ? from->matrix(width, height) : Matrix4 {};
        Matrix4 const b = to ? to->matrix(width, height) : Matrix4 {};
        return interpolate(a, b, n[0]);
    }
    }
    return {};
}

bool TransformFunction::is_3d() const
{
    switch (kind) {
    case Kind::Matrix3d:
    case Kind::TranslateZ:
    case Kind::Translate3d:
    case Kind::ScaleZ:
    case Kind::Scale3d:
    case Kind::RotateX:
    case Kind::RotateY:
    case Kind::Rotate3d:
    case Kind::Perspective:
        return true;
    case Kind::Mix: {
        auto const any = [](std::shared_ptr<TransformList const> const& list) {
            return list && std::any_of(list->functions.begin(), list->functions.end(), [](TransformFunction const& f) { return f.is_3d(); });
        };
        return any(from) || any(to);
    }
    default: return false;
    }
}

Matrix4 TransformList::matrix(double width, double height) const
{
    Matrix4 out;
    for (TransformFunction const& function : functions)
        out = out * function.matrix(width, height);
    return out;
}

bool Transforms::operator==(Transforms const& other) const
{
    bool const same_list = list == other.list || (list && other.list && *list == *other.list);
    return same_list && translate == other.translate && rotate == other.rotate && scale == other.scale && origin_x == other.origin_x
        && origin_y == other.origin_y && origin_z == other.origin_z && origin_set == other.origin_set && box == other.box && preserve_3d == other.preserve_3d
        && backface_hidden == other.backface_hidden && perspective_origin_x == other.perspective_origin_x
        && perspective_origin_y == other.perspective_origin_y && frame == other.frame;
}

Matrix4 Transforms::matrix(double width, double height) const
{
    double const ox = origin_x.resolve(width);
    double const oy = origin_y.resolve(height);
    Matrix4 out = Matrix4::translation(ox, oy, origin_z);
    if (translate)
        out = out * Matrix4::translation(translate->x.resolve(width), translate->y.resolve(height), translate->z);
    if (rotate)
        out = out * Matrix4::rotation(rotate->x, rotate->y, rotate->z, rotate->degrees);
    if (scale)
        out = out * Matrix4::scaling(scale->x, scale->y, scale->z);
    if (list)
        out = out * list->matrix(width, height);
    return out * Matrix4::translation(-ox, -oy, -origin_z);
}

// --- As text --------------------------------------------------------------------

std::string serialize(Matrix4 const& matrix)
{
    std::string out;
    if (matrix.is_2d()) {
        out = "matrix(";
        double const values[] = { matrix.m[0][0], matrix.m[0][1], matrix.m[1][0], matrix.m[1][1], matrix.m[3][0], matrix.m[3][1] };
        for (std::size_t i = 0; i < 6; ++i)
            out += (i ? ", " : "") + number(values[i]);
        return out + ")";
    }
    out = "matrix3d(";
    for (std::size_t i = 0; i < 16; ++i)
        out += (i ? ", " : "") + number(matrix.m[i / 4][i % 4]);
    return out + ")";
}

std::string serialize(TransformList const& list)
{
    std::string out;
    for (TransformFunction const& f : list.functions) {
        if (!out.empty())
            out += ' ';
        bool const two = f.n[15] >= 2;
        switch (f.kind) {
        case Kind::Matrix:
        case Kind::Matrix3d:
            out += serialize(f.matrix(0, 0));
            break;
        case Kind::Translate:
            out += "translate(" + length(f.x) + (two ? ", " + length(f.y) : "") + ")";
            break;
        case Kind::TranslateX:
            out += "translateX(" + length(f.x) + ")";
            break;
        case Kind::TranslateY:
            out += "translateY(" + length(f.y) + ")";
            break;
        case Kind::TranslateZ:
            out += "translateZ(" + number(f.n[0]) + "px)";
            break;
        case Kind::Translate3d:
            out += "translate3d(" + length(f.x) + ", " + length(f.y) + ", " + number(f.n[0]) + "px)";
            break;
        case Kind::Scale:
            out += "scale(" + number(f.n[0]) + (two ? ", " + number(f.n[1]) : "") + ")";
            break;
        case Kind::ScaleX:
            out += "scaleX(" + number(f.n[0]) + ")";
            break;
        case Kind::ScaleY:
            out += "scaleY(" + number(f.n[1]) + ")";
            break;
        case Kind::ScaleZ:
            out += "scaleZ(" + number(f.n[2]) + ")";
            break;
        case Kind::Scale3d:
            out += "scale3d(" + number(f.n[0]) + ", " + number(f.n[1]) + ", " + number(f.n[2]) + ")";
            break;
        case Kind::Rotate:
            out += "rotate(" + number(f.n[3]) + "deg)";
            break;
        case Kind::RotateX:
            out += "rotateX(" + number(f.n[3]) + "deg)";
            break;
        case Kind::RotateY:
            out += "rotateY(" + number(f.n[3]) + "deg)";
            break;
        case Kind::RotateZ:
            out += "rotateZ(" + number(f.n[3]) + "deg)";
            break;
        case Kind::Rotate3d:
            out += "rotate3d(" + number(f.n[0]) + ", " + number(f.n[1]) + ", " + number(f.n[2]) + ", " + number(f.n[3]) + "deg)";
            break;
        case Kind::Skew:
            out += "skew(" + number(f.n[0]) + "deg" + (two ? ", " + number(f.n[1]) + "deg" : "") + ")";
            break;
        case Kind::SkewX:
            out += "skewX(" + number(f.n[0]) + "deg)";
            break;
        case Kind::SkewY:
            out += "skewY(" + number(f.n[1]) + "deg)";
            break;
        case Kind::Perspective:
            out += f.n[0] < 0 ? std::string("perspective(none)") : "perspective(" + number(f.n[0]) + "px)";
            break;
        case Kind::Mix:
            out += serialize(f.matrix(0, 0));
            break;
        }
    }
    return out.empty() ? "none" : out;
}

std::string serialize(TranslateValue const& value)
{
    std::string out = length(value.x);
    if (value.y.px != 0 || value.y.percent != 0 || value.z != 0)
        out += " " + length(value.y);
    if (value.z != 0)
        out += " " + number(value.z) + "px";
    return out;
}

std::string serialize(RotateValue const& value)
{
    if (value.x == 0 && value.y == 0)
        return number(value.degrees) + "deg";
    if (value.y == 0 && value.z == 0)
        return "x " + number(value.degrees) + "deg";
    if (value.x == 0 && value.z == 0)
        return "y " + number(value.degrees) + "deg";
    return number(value.x) + " " + number(value.y) + " " + number(value.z) + " " + number(value.degrees) + "deg";
}

std::string serialize(ScaleValue const& value)
{
    std::string out = number(value.x);
    if (value.z != 1)
        return out + " " + number(value.y) + " " + number(value.z);
    if (value.y != value.x)
        out += " " + number(value.y);
    return out;
}

// --- Between two transforms -----------------------------------------------------

Matrix4 interpolate(Matrix4 const& a, Matrix4 const& b, double p)
{
    if (a.is_2d() && b.is_2d()) {
        std::optional<Parts2d> const from = parts_2d(a);
        std::optional<Parts2d> const to = parts_2d(b);
        if (!from || !to)
            return p < 0.5 ? a : b;
        return interpolate_2d(*from, *to, p);
    }
    std::optional<Parts3d> const from = parts_3d(a);
    std::optional<Parts3d> const to = parts_3d(b);
    if (!from || !to)
        return p < 0.5 ? a : b;
    return interpolate_3d(*from, *to, p);
}

std::shared_ptr<TransformList const> interpolate(std::shared_ptr<TransformList const> const& a,
    std::shared_ptr<TransformList const> const& b, double p)
{
    if (!a && !b)
        return nullptr;
    std::vector<TransformFunction> from = a ? a->functions : std::vector<TransformFunction> {};
    std::vector<TransformFunction> to = b ? b->functions : std::vector<TransformFunction> {};
    // The shorter list goes on with functions that do nothing, each of the
    // kind the longer one has there.
    while (from.size() < to.size())
        from.push_back(identity_of(to[from.size()]));
    while (to.size() < from.size())
        to.push_back(identity_of(from[to.size()]));
    TransformList out;
    std::size_t i = 0;
    for (; i < from.size() && pairs_up(from[i], to[i]); ++i)
        out.functions.push_back(interpolate_pair(from[i], to[i], p));
    if (i < from.size()) {
        std::vector<TransformFunction> rest_from(from.begin() + static_cast<std::ptrdiff_t>(i), from.end());
        std::vector<TransformFunction> rest_to(to.begin() + static_cast<std::ptrdiff_t>(i), to.end());
        bool const sized = std::any_of(rest_from.begin(), rest_from.end(), has_percent)
            || std::any_of(rest_to.begin(), rest_to.end(), has_percent);
        if (sized) {
            out.functions.push_back(mix_of(std::move(rest_from), std::move(rest_to), p));
        } else {
            out.functions.push_back(matrix_function(
                interpolate(TransformList { rest_from }.matrix(0, 0), TransformList { rest_to }.matrix(0, 0), p)));
        }
    }
    return std::make_shared<TransformList const>(std::move(out));
}

std::shared_ptr<TransformList const> accumulate(std::shared_ptr<TransformList const> const& a,
    std::shared_ptr<TransformList const> const& b, double count)
{
    if (!a && !b)
        return nullptr;
    std::vector<TransformFunction> from = a ? a->functions : std::vector<TransformFunction> {};
    std::vector<TransformFunction> to = b ? b->functions : std::vector<TransformFunction> {};
    while (from.size() < to.size())
        from.push_back(identity_of(to[from.size()]));
    while (to.size() < from.size())
        to.push_back(identity_of(from[to.size()]));
    bool const paired = std::equal(from.begin(), from.end(), to.begin(), [](TransformFunction const& x, TransformFunction const& y) {
        return x.kind == y.kind && family_of(x.kind) != Family::Matrix && family_of(x.kind) != Family::None
            && (family_of(x.kind) != Family::Rotate || (x.n[0] == y.n[0] && x.n[1] == y.n[1] && x.n[2] == y.n[2]));
    });
    TransformList out;
    if (!paired) {
        // Through the matrices: the decomposed parts accumulate as the
        // functions do, which interpolation past one comes to.
        Matrix4 const base = TransformList { from }.matrix(0, 0);
        Matrix4 const added = TransformList { to }.matrix(0, 0);
        out.functions.push_back(matrix_function(interpolate(base, base * added, count)));
        return std::make_shared<TransformList const>(std::move(out));
    }
    for (std::size_t i = 0; i < from.size(); ++i) {
        TransformFunction f = from[i];
        TransformFunction const& g = to[i];
        switch (family_of(f.kind)) {
        case Family::Scale:
            for (std::size_t k = 0; k < 3; ++k)
                f.n[k] = f.n[k] + (g.n[k] - 1) * count;
            break;
        case Family::Rotate:
            f.n[3] += g.n[3] * count;
            break;
        case Family::Perspective: {
            double const x = f.n[0] < 0 ? 0 : 1 / std::max(f.n[0], 1.0);
            double const y = g.n[0] < 0 ? 0 : 1 / std::max(g.n[0], 1.0);
            double const sum = x + y * count;
            f.n[0] = sum <= 0 ? -1 : 1 / sum;
            break;
        }
        default:
            f.x = { f.x.px + g.x.px * count, f.x.percent + g.x.percent * count };
            f.y = { f.y.px + g.y.px * count, f.y.percent + g.y.percent * count };
            f.n[0] += g.n[0] * count;
            f.n[1] += g.n[1] * count;
            f.n[15] = std::max(f.n[15], g.n[15]);
            break;
        }
        out.functions.push_back(std::move(f));
    }
    return std::make_shared<TransformList const>(std::move(out));
}

RotateValue interpolate(RotateValue const& a, RotateValue const& b, double p)
{
    auto const unit = [](RotateValue const& value) {
        Vector3 const axis { value.x, value.y, value.z };
        double const size = norm(axis);
        return size == 0 ? Vector3 { 0, 0, 1 } : scaled(axis, 1 / size);
    };
    Vector3 axis_a = unit(a);
    Vector3 axis_b = unit(b);
    if (a.degrees == 0)
        axis_a = axis_b;
    else if (b.degrees == 0)
        axis_b = axis_a;
    if (std::fabs(axis_a[0] - axis_b[0]) < 1e-9 && std::fabs(axis_a[1] - axis_b[1]) < 1e-9 && std::fabs(axis_a[2] - axis_b[2]) < 1e-9)
        return RotateValue { axis_a[0], axis_a[1], axis_a[2], lerp(a.degrees, b.degrees, p) };
    auto const quaternion = [](Vector3 const& axis, double degrees) {
        double const half = degrees * pi / 360.0;
        double const s = std::sin(half);
        return std::array<double, 4> { axis[0] * s, axis[1] * s, axis[2] * s, std::cos(half) };
    };
    std::array<double, 4> const met = slerp(quaternion(axis_a, a.degrees), quaternion(axis_b, b.degrees), p);
    double const w = std::clamp(met[3], -1.0, 1.0);
    double const angle = 2 * std::acos(w);
    double const s = std::sqrt(1 - w * w);
    if (s < 1e-9)
        return RotateValue { 0, 0, 1, 0 };
    return RotateValue { met[0] / s, met[1] / s, met[2] / s, angle * 180.0 / pi };
}

}
