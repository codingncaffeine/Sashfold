#pragma once

// Transforms (css-transforms-1, css-transforms-2): the matrix a box's
// `transform`, `translate`, `rotate` and `scale` come to, the functions it
// is written with, and how two of them meet between keyframes.
//
// A box is flat: its matrix has four rows for the functions that turn it
// out of the page, and what is drawn is what that does to the box's own
// plane (PlaneTransform), every box flattened into the one around it.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::css {

// A 4x4 matrix as matrix3d() writes one: column by column, applied to
// column vectors. `m[column][row]`.
struct Matrix4 {
    std::array<std::array<double, 4>, 4> m { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };

    static Matrix4 translation(double x, double y, double z = 0);
    static Matrix4 scaling(double x, double y, double z = 1);
    // About the axis (x, y, z), which need not be a unit vector; a null
    // axis turns nothing.
    static Matrix4 rotation(double x, double y, double z, double degrees);
    static Matrix4 skewing(double degrees_x, double degrees_y);
    // perspective(depth): depth in px, held to 1 at least.
    static Matrix4 perspective(double depth);
    // matrix(a, b, c, d, e, f).
    static Matrix4 affine(double a, double b, double c, double d, double e, double f);

    // `inner` first, then this one.
    Matrix4 operator*(Matrix4 const& inner) const;
    bool operator==(Matrix4 const&) const = default;
    bool is_identity() const;
    // Whether matrix(a, b, c, d, e, f) says all of it.
    bool is_2d() const;
    // Whether it only moves the box along the page.
    bool is_translation_2d() const;
    double determinant() const;
    std::optional<Matrix4> inverse() const;
    // Whether the box's back is what faces the viewer.
    bool shows_back() const;
};

// What a matrix does to the points of the plane z = 0, seen flat: three
// rows, row by row, projective when the matrix has perspective in it.
struct PlaneTransform {
    std::array<double, 9> h { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

    static PlaneTransform of(Matrix4 const& matrix);
    static PlaneTransform translation(double x, double y);
    // `inner` first, then this one.
    PlaneTransform operator*(PlaneTransform const& inner) const;
    bool operator==(PlaneTransform const&) const = default;
    bool is_identity() const;
    bool is_affine() const { return h[6] == 0 && h[7] == 0 && h[8] == 1; }
    std::optional<PlaneTransform> inverse() const;
    // Where a point goes; false when it lands behind the viewer.
    bool map(double x, double y, double& out_x, double& out_y) const;
    // The box around where a rectangle goes: its corners' images, cut at
    // the viewer's plane first. Empty (false) when nothing of it is in
    // front of the viewer.
    bool map_rect(double x, double y, double width, double height, double& left, double& top, double& right,
        double& bottom) const;
    // The same transform about another origin: one given for points
    // measured from (x, y), for points measured from the page's corner.
    PlaneTransform about(double x, double y) const;
};

// A length that may hold a percentage of the box: px + percent / 100 * size.
struct TransformLength {
    double px = 0;
    double percent = 0;
    bool operator==(TransformLength const&) const = default;
    double resolve(double size) const { return px + percent * size / 100.0; }
};

struct TransformList;

// One function of a transform list, with its lengths computed.
struct TransformFunction {
    enum class Kind : std::uint8_t {
        Matrix, // n[0..5]
        Matrix3d, // n[0..15]
        Translate, // x, y
        TranslateX, // x
        TranslateY, // y
        TranslateZ, // n[0] px
        Translate3d, // x, y, n[0] px
        Scale, // n[0], n[1]
        ScaleX, // n[0]
        ScaleY, // n[1]
        ScaleZ, // n[2]
        Scale3d, // n[0..2]
        Rotate, // n[3] degrees
        RotateX,
        RotateY,
        RotateZ,
        Rotate3d, // axis n[0..2], n[3] degrees
        Skew, // n[0], n[1] degrees
        SkewX, // n[0]
        SkewY, // n[1]
        Perspective, // n[0] px; negative: none
        // Between two lists that do not pair up function by function
        // (css-transforms-2 §9 "interpolate the matrices"): their matrices
        // met at n[0], found when the box's size is known, since either
        // may hold percentages.
        Mix,
    };
    Kind kind = Kind::Matrix;
    TransformLength x;
    TransformLength y;
    std::array<double, 16> n {};
    std::shared_ptr<TransformList const> from; // Mix
    std::shared_ptr<TransformList const> to; // Mix

    bool operator==(TransformFunction const& other) const;
    Matrix4 matrix(double width, double height) const;
    // Whether the function reaches out of the page's plane as written.
    bool is_3d() const;
};

struct TransformList {
    std::vector<TransformFunction> functions;
    bool operator==(TransformList const&) const = default;
    Matrix4 matrix(double width, double height) const;
};

// The `translate`, `rotate` and `scale` properties (css-transforms-2 §5).
struct TranslateValue {
    TransformLength x;
    TransformLength y;
    double z = 0;
    bool operator==(TranslateValue const&) const = default;
};
struct RotateValue {
    double x = 0;
    double y = 0;
    double z = 1;
    double degrees = 0;
    bool operator==(RotateValue const&) const = default;
};
struct ScaleValue {
    double x = 1;
    double y = 1;
    double z = 1;
    bool operator==(ScaleValue const&) const = default;
};

enum class TransformBox : std::uint8_t { ViewBox, ContentBox, BorderBox, FillBox, StrokeBox };

// Everything a box's transform is made of. A style holds none of it until
// one of these properties is written.
struct Transforms {
    std::shared_ptr<TransformList const> list; // `transform`; null: none
    std::optional<TranslateValue> translate;
    std::optional<RotateValue> rotate;
    std::optional<ScaleValue> scale;
    TransformLength origin_x { 0, 50 };
    TransformLength origin_y { 0, 50 };
    double origin_z = 0;
    // Whether transform-origin was written at all: where none was, an
    // element inside an <svg> turns about its user space's origin and not
    // about its middle (SVG 2's own sheet says so).
    bool origin_set = false;
    TransformBox box = TransformBox::ViewBox;
    bool preserve_3d = false; // transform-style
    bool backface_hidden = false; // backface-visibility
    TransformLength perspective_origin_x { 0, 50 };
    TransformLength perspective_origin_y { 0, 50 };
    // Layout's own mark on the copy of a style it lays a vertical page out
    // with, the page turned on its side: that the box's width and height
    // have changed places there, and which way each axis then runs. The
    // transform itself stays the page's.
    static constexpr std::uint8_t frame_turned = 1;
    static constexpr std::uint8_t frame_up = 2;
    static constexpr std::uint8_t frame_left = 4;
    std::uint8_t frame = 0;

    bool operator==(Transforms const& other) const;
    // Whether the box is transformed at all: a stacking context, and the
    // holder of the positioned boxes inside it, even where the matrix
    // comes to the identity.
    bool any() const { return list != nullptr || translate.has_value() || rotate.has_value() || scale.has_value(); }
    // The box's matrix (css-transforms-2 §7), for a reference box of that
    // size, measured from the reference box's corner.
    Matrix4 matrix(double width, double height) const;
};

// A list as its computed value is written: the functions as given, their
// lengths in px.
std::string serialize(TransformList const& list);
// A matrix as getComputedStyle writes one: matrix(...) when that says all
// of it, matrix3d(...) otherwise.
std::string serialize(Matrix4 const& matrix);
std::string serialize(TranslateValue const& value);
std::string serialize(RotateValue const& value);
std::string serialize(ScaleValue const& value);

// One list toward another by `p` (css-transforms-2 §9): function by
// function while they pair up — the same function, or two of one
// primitive — and by their matrices from where they stop. Null stands for
// `none` on either side and may be the answer.
std::shared_ptr<TransformList const> interpolate(std::shared_ptr<TransformList const> const& a,
    std::shared_ptr<TransformList const> const& b, double p);
// `b` accumulated onto `a` `count` times (css-transforms-1 §13): function
// by function while they pair up — lengths and angles add, factors add
// their distance from one — else the matrices through what they are made
// of. Null stands for none.
std::shared_ptr<TransformList const> accumulate(std::shared_ptr<TransformList const> const& a,
    std::shared_ptr<TransformList const> const& b, double count);
// Two matrices met at `p` through what each decomposes to (css-transforms-1
// §11, css-transforms-2 §11); one that cannot be decomposed flips at the
// halfway point.
Matrix4 interpolate(Matrix4 const& a, Matrix4 const& b, double p);
// rotate toward rotate: about one axis the angle alone moves, between two
// axes the turn takes the short way round.
RotateValue interpolate(RotateValue const& a, RotateValue const& b, double p);

}
