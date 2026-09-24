#include "bindings/Internal.h"

// Geometry Interfaces (geometry-1): DOMPointReadOnly and DOMPoint, and
// DOMMatrixReadOnly and DOMMatrix: a 4x4 matrix that knows whether it is
// still a 2D one, made from a sequence of 6 or 16 numbers, from a CSS
// transform list, or from a dictionary; multiplied, inverted, turned into a
// point's transform, and serialized as CSS writes matrix() and matrix3d().
// The canvas context's getTransform() answers with one, and its
// setTransform(), a pattern's setTransform() and Path2D.addPath() read the
// DOMMatrix2DInit dictionary through matrix_2d_from_init.

#include "js/Object.h"
#include "js/Runtime.h"
#include "js/Strings.h"
#include "paint/Canvas2D.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

namespace {

constexpr double pi = 3.14159265358979323846;

// Turns take the canvas's own sine and cosine rather than the C library's,
// whose results differ between systems: a DOMMatrix handed to a canvas's
// setTransform() reaches its pixels.
canvas::SineCosine turn_by_degrees(double degrees) { return canvas::sine_cosine(degrees * pi / 180); }

double tangent_of_degrees(double degrees)
{
    canvas::SineCosine const turned = turn_by_degrees(degrees);
    return turned.sine / turned.cosine;
}

// m[i * 4 + j] is the specification's m(i+1)(j+1): i picks the column a
// point's coordinate multiplies, j the coordinate it adds to.
struct Matrix4 {
    std::array<double, 16> m { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    bool is_2d = true;

    double& at(int i, int j) { return m[static_cast<std::size_t>(i * 4 + j)]; }
    double at(int i, int j) const { return m[static_cast<std::size_t>(i * 4 + j)]; }

    // this x other: `other` applies to a point first.
    Matrix4 multiply(Matrix4 const& other) const
    {
        Matrix4 out;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                double sum = 0;
                for (int k = 0; k < 4; ++k)
                    sum += at(k, j) * other.at(i, k);
                out.at(i, j) = sum;
            }
        }
        out.is_2d = is_2d && other.is_2d;
        return out;
    }

    bool is_identity() const
    {
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                if (at(i, j) != (i == j ? 1.0 : 0.0))
                    return false;
            }
        }
        return true;
    }

    static Matrix4 from_2d(double a, double b, double c, double d, double e, double f)
    {
        Matrix4 out;
        out.at(0, 0) = a;
        out.at(0, 1) = b;
        out.at(1, 0) = c;
        out.at(1, 1) = d;
        out.at(3, 0) = e;
        out.at(3, 1) = f;
        return out;
    }
};

class DomMatrixObject final : public js::Object {
public:
    DomMatrixObject(js::Object* prototype, Matrix4 the_matrix)
        : Object(prototype, Class::Host)
        , matrix(the_matrix)
    {
    }
    Matrix4 matrix;
};

class DomPointObject final : public js::Object {
public:
    explicit DomPointObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    double x = 0;
    double y = 0;
    double z = 0;
    double w = 1;
};

std::optional<DomMatrixObject*> this_matrix(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* matrix = dynamic_cast<DomMatrixObject*>(this_value.as_object()))
            return matrix;
    }
    return interp.throw_type_error("Illegal invocation");
}

std::optional<DomPointObject*> this_point(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* point = dynamic_cast<DomPointObject*>(this_value.as_object()))
            return point;
    }
    return interp.throw_type_error("Illegal invocation");
}

js::Value new_matrix(Realm::Internals& in, Matrix4 const& matrix, bool mutable_one)
{
    return js::Value::object(in.interpreter.heap().allocate<DomMatrixObject>(
        in.prototype(mutable_one ? "DOMMatrix" : "DOMMatrixReadOnly"), matrix));
}

std::optional<double> number_argument(js::Interpreter& interp, Args args, std::size_t index, double fallback)
{
    js::Value const value = js::argument(args, index);
    if (value.is_undefined())
        return fallback;
    return interp.to_number(value);
}

std::string number_text(double value) { return encode_utf8(js::number_to_string(value)); }

// --- Dictionaries -----------------------------------------------------------------

struct MatrixFields {
    std::optional<double> a, b, c, d, e, f;
    std::optional<bool> is_2d;
    std::optional<double> m[16];
};

// The members of a DOMMatrixInit read in the order WebIDL reads a
// dictionary's: by name.
std::optional<MatrixFields> read_matrix_fields(Realm::Internals& in, js::Value const& value, bool three_d)
{
    MatrixFields fields;
    if (value.is_undefined() || value.is_null())
        return fields;
    if (!value.is_object())
        return in.interpreter.throw_type_error("The value is not a matrix dictionary.");
    js::Interpreter& interp = in.interpreter;
    js::Object& object = *value.as_object();
    auto const read = [&](std::string_view name, std::optional<double>& out) -> bool {
        std::optional<js::Value> const got = interp.get(object, interp.key(name));
        if (!got)
            return false;
        if (got->is_undefined())
            return true;
        std::optional<double> const number = interp.to_number(*got);
        if (!number)
            return false;
        out = *number;
        return true;
    };
    if (!read("a", fields.a) || !read("b", fields.b) || !read("c", fields.c) || !read("d", fields.d) || !read("e", fields.e)
        || !read("f", fields.f))
        return std::nullopt;
    if (three_d) {
        std::optional<js::Value> const flag = interp.get(object, interp.key("is2D"));
        if (!flag)
            return std::nullopt;
        if (!flag->is_undefined())
            fields.is_2d = js::Interpreter::to_boolean(*flag);
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            bool const two_d_member = (i == 0 || i == 1 || i == 3) && (j == 0 || j == 1);
            if (!three_d && !two_d_member)
                continue;
            std::string const name = "m" + std::to_string(i + 1) + std::to_string(j + 1);
            if (!read(name, fields.m[i * 4 + j]))
                return std::nullopt;
        }
    }
    return fields;
}

// "Validate and fixup" a DOMMatrix2DInit or DOMMatrixInit (geometry-1 section6.1).
std::optional<Matrix4> matrix_from_fields(Realm::Internals& in, MatrixFields const& fields, bool three_d)
{
    auto const conflict = [](std::optional<double> const& alias, std::optional<double> const& member) {
        if (!alias || !member)
            return false;
        return !(*alias == *member || (std::isnan(*alias) && std::isnan(*member)));
    };
    if (conflict(fields.a, fields.m[0]) || conflict(fields.b, fields.m[1]) || conflict(fields.c, fields.m[4])
        || conflict(fields.d, fields.m[5]) || conflict(fields.e, fields.m[12]) || conflict(fields.f, fields.m[13]))
        return in.interpreter.throw_type_error("The matrix's members disagree with their aliases.");
    Matrix4 out;
    out.m[0] = fields.m[0].value_or(fields.a.value_or(1));
    out.m[1] = fields.m[1].value_or(fields.b.value_or(0));
    out.m[4] = fields.m[4].value_or(fields.c.value_or(0));
    out.m[5] = fields.m[5].value_or(fields.d.value_or(1));
    out.m[12] = fields.m[12].value_or(fields.e.value_or(0));
    out.m[13] = fields.m[13].value_or(fields.f.value_or(0));
    if (!three_d)
        return out;
    static constexpr int zero_members[] = { 2, 3, 6, 7, 8, 9, 11, 14 };
    static constexpr int one_members[] = { 10, 15 };
    bool three_d_values = false;
    for (int const i : zero_members) {
        out.m[static_cast<std::size_t>(i)] = fields.m[i].value_or(0);
        if (out.m[static_cast<std::size_t>(i)] != 0)
            three_d_values = true;
    }
    for (int const i : one_members) {
        out.m[static_cast<std::size_t>(i)] = fields.m[i].value_or(1);
        if (out.m[static_cast<std::size_t>(i)] != 1)
            three_d_values = true;
    }
    if (fields.is_2d && *fields.is_2d && three_d_values)
        return in.interpreter.throw_type_error("A 2D matrix cannot have 3D members.");
    out.is_2d = fields.is_2d ? *fields.is_2d : !three_d_values;
    return out;
}

std::optional<Matrix4> matrix_from_init(Realm::Internals& in, js::Value const& value)
{
    if (value.is_object()) {
        if (auto* given = dynamic_cast<DomMatrixObject*>(value.as_object()))
            return given->matrix;
    }
    std::optional<MatrixFields> const fields = read_matrix_fields(in, value, true);
    if (!fields)
        return std::nullopt;
    return matrix_from_fields(in, *fields, true);
}

// --- Transform lists (css-transforms-1, for the constructor's string) -------------

struct TransformParser {
    std::string_view text;
    std::size_t at = 0;

    void skip_space()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r' || text[at] == '\f'))
            ++at;
    }

    // A number and its unit, lowercased.
    bool number(double& value, std::string& unit)
    {
        skip_space();
        std::size_t const start = at;
        if (at < text.size() && (text[at] == '+' || text[at] == '-'))
            ++at;
        bool digits = false;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            ++at;
            digits = true;
        }
        if (at < text.size() && text[at] == '.') {
            ++at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
                ++at;
                digits = true;
            }
        }
        if (!digits)
            return false;
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            std::size_t probe = at + 1;
            if (probe < text.size() && (text[probe] == '+' || text[probe] == '-'))
                ++probe;
            if (probe < text.size() && text[probe] >= '0' && text[probe] <= '9') {
                at = probe;
                while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                    ++at;
            }
        }
        value = std::strtod(std::string(text.substr(start, at - start)).c_str(), nullptr);
        unit.clear();
        while (at < text.size() && ((text[at] >= 'a' && text[at] <= 'z') || (text[at] >= 'A' && text[at] <= 'Z') || text[at] == '%')) {
            char const ch = text[at++];
            unit.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        return true;
    }
};

// The transform list as one matrix; nullopt for text that is not one, or
// that uses a unit other than px (which is relative to nothing here).
std::optional<Matrix4> parse_transform_list(std::string_view text)
{
    TransformParser p { text };
    p.skip_space();
    Matrix4 result;
    if (p.at == text.size())
        return std::nullopt;
    if (ascii_lower(text.substr(p.at)).starts_with("none")) {
        p.at += 4;
        p.skip_space();
        if (p.at == text.size())
            return result;
        return std::nullopt;
    }
    while (true) {
        p.skip_space();
        if (p.at == text.size())
            break;
        std::size_t const name_start = p.at;
        while (p.at < text.size() && text[p.at] != '(')
            ++p.at;
        if (p.at == text.size())
            return std::nullopt;
        std::string const name = ascii_lower(text.substr(name_start, p.at - name_start));
        ++p.at;
        struct Arg {
            double value;
            std::string unit;
        };
        std::vector<Arg> args;
        while (true) {
            p.skip_space();
            if (p.at < text.size() && text[p.at] == ')') {
                ++p.at;
                break;
            }
            if (!args.empty()) {
                if (p.at < text.size() && text[p.at] == ',')
                    ++p.at;
                else
                    return std::nullopt;
            }
            Arg arg { 0, {} };
            if (!p.number(arg.value, arg.unit))
                return std::nullopt;
            args.push_back(arg);
        }
        auto const length = [&](std::size_t i) -> std::optional<double> {
            if (i >= args.size())
                return 0.0;
            if (args[i].unit == "px" || (args[i].unit.empty() && args[i].value == 0))
                return args[i].value;
            return std::nullopt;
        };
        auto const angle = [&](std::size_t i) -> std::optional<double> {
            if (i >= args.size())
                return 0.0;
            std::string const& unit = args[i].unit;
            if (unit == "deg")
                return args[i].value;
            if (unit == "rad")
                return args[i].value * 180 / pi;
            if (unit == "grad")
                return args[i].value * 0.9;
            if (unit == "turn")
                return args[i].value * 360;
            if (unit.empty() && args[i].value == 0)
                return 0.0;
            return std::nullopt;
        };
        auto const plain = [&](std::size_t count) {
            if (args.size() != count)
                return false;
            for (Arg const& arg : args) {
                if (!arg.unit.empty())
                    return false;
            }
            return true;
        };
        Matrix4 step;
        if (name == "matrix") {
            if (!plain(6))
                return std::nullopt;
            step = Matrix4::from_2d(args[0].value, args[1].value, args[2].value, args[3].value, args[4].value, args[5].value);
        } else if (name == "matrix3d") {
            if (!plain(16))
                return std::nullopt;
            for (std::size_t i = 0; i < 16; ++i)
                step.m[i] = args[i].value;
            step.is_2d = false;
        } else if (name == "translate" || name == "translatex" || name == "translatey") {
            if (args.empty() || args.size() > (name == "translate" ? 2u : 1u))
                return std::nullopt;
            std::optional<double> const tx = length(0);
            std::optional<double> const ty = length(1);
            if (!tx || !ty)
                return std::nullopt;
            if (name == "translatey")
                step = Matrix4::from_2d(1, 0, 0, 1, 0, *tx);
            else
                step = Matrix4::from_2d(1, 0, 0, 1, *tx, name == "translate" ? *ty : 0);
        } else if (name == "scale" || name == "scalex" || name == "scaley") {
            if (args.empty() || args.size() > (name == "scale" ? 2u : 1u) || !plain(args.size()))
                return std::nullopt;
            double const sx = args[0].value;
            double const sy = args.size() > 1 ? args[1].value : sx;
            if (name == "scalex")
                step = Matrix4::from_2d(sx, 0, 0, 1, 0, 0);
            else if (name == "scaley")
                step = Matrix4::from_2d(1, 0, 0, sx, 0, 0);
            else
                step = Matrix4::from_2d(sx, 0, 0, sy, 0, 0);
        } else if (name == "rotate" || name == "rotatez") {
            std::optional<double> const degrees = angle(0);
            if (args.size() != 1 || !degrees)
                return std::nullopt;
            canvas::SineCosine const turned = turn_by_degrees(*degrees);
            step = Matrix4::from_2d(turned.cosine, turned.sine, -turned.sine, turned.cosine, 0, 0);
        } else if (name == "skew" || name == "skewx" || name == "skewy") {
            if (args.empty() || args.size() > (name == "skew" ? 2u : 1u))
                return std::nullopt;
            std::optional<double> const ax = angle(0);
            std::optional<double> const ay = angle(1);
            if (!ax || !ay)
                return std::nullopt;
            double const tx = tangent_of_degrees(*ax);
            double const ty = tangent_of_degrees(*ay);
            if (name == "skewx")
                step = Matrix4::from_2d(1, 0, tx, 1, 0, 0);
            else if (name == "skewy")
                step = Matrix4::from_2d(1, tx, 0, 1, 0, 0);
            else
                step = Matrix4::from_2d(1, name == "skew" ? ty : 0, tx, 1, 0, 0);
        } else {
            return std::nullopt;
        }
        result = result.multiply(step);
    }
    return result;
}

// --- Operations -------------------------------------------------------------------

Matrix4 translation(double tx, double ty, double tz)
{
    Matrix4 t;
    t.at(3, 0) = tx;
    t.at(3, 1) = ty;
    t.at(3, 2) = tz;
    t.is_2d = tz == 0;
    return t;
}

Matrix4 scaling(double sx, double sy, double sz)
{
    Matrix4 t;
    t.at(0, 0) = sx;
    t.at(1, 1) = sy;
    t.at(2, 2) = sz;
    t.is_2d = sz == 1;
    return t;
}

// A rotation by `degrees` about the axis (x, y, z) (css-transforms-2 section13).
Matrix4 rotation(double x, double y, double z, double degrees)
{
    Matrix4 t;
    double const length = std::sqrt(x * x + y * y + z * z);
    if (length == 0)
        return t;
    x /= length;
    y /= length;
    z /= length;
    canvas::SineCosine const half = canvas::sine_cosine(degrees * pi / 180 / 2);
    double const sc = half.sine * half.cosine;
    double const sq = half.sine * half.sine;
    t.at(0, 0) = 1 - 2 * (y * y + z * z) * sq;
    t.at(0, 1) = 2 * (x * y * sq + z * sc);
    t.at(0, 2) = 2 * (x * z * sq - y * sc);
    t.at(1, 0) = 2 * (x * y * sq - z * sc);
    t.at(1, 1) = 1 - 2 * (x * x + z * z) * sq;
    t.at(1, 2) = 2 * (y * z * sq + x * sc);
    t.at(2, 0) = 2 * (x * z * sq + y * sc);
    t.at(2, 1) = 2 * (y * z * sq - x * sc);
    t.at(2, 2) = 1 - 2 * (x * x + y * y) * sq;
    t.is_2d = x == 0 && y == 0;
    return t;
}

std::optional<Matrix4> inverted(Matrix4 const& matrix)
{
    if (matrix.is_2d) {
        double const a = matrix.at(0, 0), b = matrix.at(0, 1), c = matrix.at(1, 0), d = matrix.at(1, 1);
        double const e = matrix.at(3, 0), f = matrix.at(3, 1);
        double const det = a * d - b * c;
        if (det == 0 || !std::isfinite(det))
            return std::nullopt;
        return Matrix4::from_2d(d / det, -b / det, -c / det, a / det, (c * f - d * e) / det, (b * e - a * f) / det);
    }
    // Gauss-Jordan over the 4x4, in the mathematical layout.
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = matrix.at(c, r);
            a[r][c + 4] = r == c ? 1 : 0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
                pivot = r;
        }
        if (a[pivot][col] == 0 || !std::isfinite(a[pivot][col]))
            return std::nullopt;
        if (pivot != col) {
            for (int k = 0; k < 8; ++k)
                std::swap(a[pivot][k], a[col][k]);
        }
        double const div = a[col][col];
        for (int k = 0; k < 8; ++k)
            a[col][k] /= div;
        for (int r = 0; r < 4; ++r) {
            if (r == col)
                continue;
            double const factor = a[r][col];
            for (int k = 0; k < 8; ++k)
                a[r][k] -= factor * a[col][k];
        }
    }
    Matrix4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            out.at(c, r) = a[r][c + 4];
    }
    out.is_2d = false;
    return out;
}

Matrix4 inverse_or_nan(Matrix4 const& matrix)
{
    if (std::optional<Matrix4> const inverse = inverted(matrix))
        return *inverse;
    Matrix4 nan;
    nan.m.fill(std::nan(""));
    nan.is_2d = false;
    return nan;
}

std::string matrix_text(Matrix4 const& matrix)
{
    std::string out;
    if (matrix.is_2d) {
        double const values[6] = { matrix.at(0, 0), matrix.at(0, 1), matrix.at(1, 0), matrix.at(1, 1), matrix.at(3, 0), matrix.at(3, 1) };
        out = "matrix(";
        for (int i = 0; i < 6; ++i)
            out += (i ? ", " : "") + number_text(values[i]);
    } else {
        out = "matrix3d(";
        for (std::size_t i = 0; i < 16; ++i)
            out += (i ? ", " : "") + number_text(matrix.m[i]);
    }
    return out + ")";
}

// The point through the matrix (geometry-1 section6.5).
void transform_point(Matrix4 const& matrix, double p[4], double out[4])
{
    for (int j = 0; j < 4; ++j) {
        double sum = 0;
        for (int i = 0; i < 4; ++i)
            sum += matrix.at(i, j) * p[i];
        out[j] = sum;
    }
}

js::Value new_point(Realm::Internals& in, double const p[4], bool mutable_one)
{
    auto* point = in.interpreter.heap().allocate<DomPointObject>(in.prototype(mutable_one ? "DOMPoint" : "DOMPointReadOnly"));
    point->x = p[0];
    point->y = p[1];
    point->z = p[2];
    point->w = p[3];
    return js::Value::object(point);
}

// A DOMPointInit dictionary: x, y, z and w, read by name.
std::optional<std::array<double, 4>> point_from_init(Realm::Internals& in, js::Value const& value)
{
    std::array<double, 4> p { 0, 0, 0, 1 };
    if (value.is_undefined() || value.is_null())
        return p;
    if (!value.is_object())
        return in.interpreter.throw_type_error("The value is not a point dictionary.");
    js::Interpreter& interp = in.interpreter;
    char const* const names[4] = { "w", "x", "y", "z" };
    std::size_t const slots[4] = { 3, 0, 1, 2 };
    for (int k = 0; k < 4; ++k) {
        std::optional<js::Value> const got = interp.get(*value.as_object(), interp.key(names[k]));
        if (!got)
            return std::nullopt;
        if (got->is_undefined())
            continue;
        std::optional<double> const number = interp.to_number(*got);
        if (!number)
            return std::nullopt;
        p[slots[k]] = *number;
    }
    return p;
}

// The matrix a sequence of 6 or 16 numbers gives.
std::optional<Matrix4> matrix_from_sequence(Realm::Internals& in, std::span<double const> values)
{
    if (values.size() == 6)
        return Matrix4::from_2d(values[0], values[1], values[2], values[3], values[4], values[5]);
    if (values.size() == 16) {
        Matrix4 out;
        for (std::size_t i = 0; i < 16; ++i)
            out.m[i] = values[i];
        out.is_2d = false;
        return out;
    }
    return in.interpreter.throw_type_error("A matrix is made of 6 or 16 numbers.");
}

std::optional<std::vector<double>> number_sequence(Realm::Internals& in, js::Value const& value)
{
    js::Interpreter& interp = in.interpreter;
    std::optional<std::vector<js::Value>> const list = interp.iterable_to_list(value);
    if (!list)
        return std::nullopt;
    std::vector<double> numbers;
    for (js::Value const& item : *list) {
        std::optional<double> const number = interp.to_number(item);
        if (!number)
            return std::nullopt;
        numbers.push_back(*number);
    }
    return numbers;
}

Native construct_matrix(js::Interpreter& interp, Args args, bool mutable_one)
{
    Realm::Internals& in = internals_of(interp);
    js::Value const init = js::argument(args, 0);
    if (init.is_undefined())
        return new_matrix(in, Matrix4 {}, mutable_one);
    if (init.is_string()) {
        if (in.worker != nullptr)
            return interp.throw_type_error("A matrix is made from a string only in a window.");
        std::optional<std::string> const text = in.to_utf8(init);
        if (!text)
            return std::nullopt;
        std::optional<Matrix4> const parsed = parse_transform_list(*text);
        if (!parsed)
            return in.throw_dom_exception("SyntaxError", "Failed to construct 'DOMMatrix': the string is not a transform list.");
        return new_matrix(in, *parsed, mutable_one);
    }
    if (!init.is_object())
        return interp.throw_type_error("Failed to construct 'DOMMatrix': the argument is neither a string nor a sequence.");
    std::optional<std::vector<double>> const numbers = number_sequence(in, init);
    if (!numbers)
        return std::nullopt;
    std::optional<Matrix4> const matrix = matrix_from_sequence(in, *numbers);
    if (!matrix)
        return std::nullopt;
    return new_matrix(in, *matrix, mutable_one);
}

// An operation that makes a new matrix from `this` (the read-only form)
// or changes `this` in place (the Self form, on DOMMatrix only).
using MatrixStep = std::function<std::optional<Matrix4>(Realm::Internals&, Matrix4 const&, Args)>;

void define_matrix_step(Realm::Internals& in, js::Object& readonly_proto, js::Object& mutable_proto, std::string_view name,
    int length, MatrixStep step)
{
    define_operation(in.interpreter, readonly_proto, name, length,
        [step](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
            if (!self)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<Matrix4> const result = step(internals, (*self)->matrix, args);
            if (!result)
                return std::nullopt;
            return new_matrix(internals, *result, true);
        });
    define_operation(in.interpreter, mutable_proto, std::string(name) + "Self", length,
        [step](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
            if (!self)
                return std::nullopt;
            std::optional<Matrix4> const result = step(internals_of(interp), (*self)->matrix, args);
            if (!result)
                return std::nullopt;
            (*self)->matrix = *result;
            return this_value;
        });
}

} // namespace

std::optional<Matrix2D> matrix_2d_from_init(Realm::Internals& in, js::Value const& init)
{
    if (init.is_object()) {
        if (auto* given = dynamic_cast<DomMatrixObject*>(init.as_object())) {
            Matrix4 const& m = given->matrix;
            return Matrix2D { m.at(0, 0), m.at(0, 1), m.at(1, 0), m.at(1, 1), m.at(3, 0), m.at(3, 1) };
        }
    }
    std::optional<MatrixFields> const fields = read_matrix_fields(in, init, false);
    if (!fields)
        return std::nullopt;
    std::optional<Matrix4> const m = matrix_from_fields(in, *fields, false);
    if (!m)
        return std::nullopt;
    return Matrix2D { m->at(0, 0), m->at(0, 1), m->at(1, 0), m->at(1, 1), m->at(3, 0), m->at(3, 1) };
}

js::Value new_dom_matrix_2d(Realm::Internals& in, Matrix2D const& matrix)
{
    return new_matrix(in, Matrix4::from_2d(matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f), true);
}

void install_geometry(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    // --- DOMPointReadOnly and DOMPoint ---
    auto const construct_point = [](bool mutable_one) {
        return [mutable_one](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            double p[4] = { 0, 0, 0, 1 };
            for (std::size_t i = 0; i < 4; ++i) {
                std::optional<double> const value = number_argument(interp, args, i, p[i]);
                if (!value)
                    return std::nullopt;
                p[i] = *value;
            }
            return new_point(internals, p, mutable_one);
        };
    };
    js::Object* point_readonly = define_interface(in, "DOMPointReadOnly", nullptr, construct_point(false), 0);
    js::Object* point = define_interface(in, "DOMPoint", point_readonly, construct_point(true), 0);
    struct PointMember {
        char const* name;
        double DomPointObject::*member;
    };
    for (PointMember const member : { PointMember { "x", &DomPointObject::x }, PointMember { "y", &DomPointObject::y },
             PointMember { "z", &DomPointObject::z }, PointMember { "w", &DomPointObject::w } }) {
        auto const field = member.member;
        define_getter(in, *point_readonly, member.name, [field](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<DomPointObject*> const self = this_point(interp, this_value);
            if (!self)
                return std::nullopt;
            return js::Value::number((*self)->*field);
        });
        define_getter(
            in, *point, member.name,
            [field](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<DomPointObject*> const self = this_point(interp, this_value);
                if (!self)
                    return std::nullopt;
                return js::Value::number((*self)->*field);
            },
            [field](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<DomPointObject*> const self = this_point(interp, this_value);
                if (!self)
                    return std::nullopt;
                std::optional<double> const value = interp.to_number(js::argument(args, 0));
                if (!value)
                    return std::nullopt;
                (*self)->*field = *value;
                return js::Value::undefined();
            });
    }
    define_operation(interpreter, *point_readonly, "matrixTransform", 0,
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<DomPointObject*> const self = this_point(interp, this_value);
            if (!self)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<Matrix4> const matrix = matrix_from_init(internals, js::argument(args, 0));
            if (!matrix)
                return std::nullopt;
            double p[4] = { (*self)->x, (*self)->y, (*self)->z, (*self)->w };
            double out[4];
            transform_point(*matrix, p, out);
            return new_point(internals, out, true);
        });
    define_operation(interpreter, *point_readonly, "toJSON", 0,
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<DomPointObject*> const self = this_point(interp, this_value);
            if (!self)
                return std::nullopt;
            js::Object* object = interp.new_object(interp.intrinsics().object_prototype);
            js::Interpreter::Roots const roots(interp);
            interp.root(js::Value::object(object));
            double const values[4] = { (*self)->x, (*self)->y, (*self)->z, (*self)->w };
            char const* const names[4] = { "x", "y", "z", "w" };
            for (int k = 0; k < 4; ++k)
                interp.create_data_property(*object, interp.key(names[k]), js::Value::number(values[k]));
            return js::Value::object(object);
        });
    for (bool const mutable_one : { false, true }) {
        js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key(mutable_one ? "DOMPoint" : "DOMPointReadOnly"));
        define_operation(interpreter, *constructor.as_object(), "fromPoint", 0,
            [mutable_one](js::Interpreter& interp, js::Value const&, Args args) -> Native {
                Realm::Internals& internals = internals_of(interp);
                std::optional<std::array<double, 4>> const p = point_from_init(internals, js::argument(args, 0));
                if (!p)
                    return std::nullopt;
                return new_point(internals, p->data(), mutable_one);
            });
    }

    // --- DOMMatrixReadOnly and DOMMatrix ---
    js::Object* readonly = define_interface(in, "DOMMatrixReadOnly", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native { return construct_matrix(interp, args, false); }, 0);
    js::Object* matrix = define_interface(in, "DOMMatrix", readonly,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native { return construct_matrix(interp, args, true); }, 0);

    struct Member {
        char const* name;
        int i;
        int j;
    };
    static constexpr Member members[] = {
        { "a", 0, 0 }, { "b", 0, 1 }, { "c", 1, 0 }, { "d", 1, 1 }, { "e", 3, 0 }, { "f", 3, 1 },
        { "m11", 0, 0 }, { "m12", 0, 1 }, { "m13", 0, 2 }, { "m14", 0, 3 },
        { "m21", 1, 0 }, { "m22", 1, 1 }, { "m23", 1, 2 }, { "m24", 1, 3 },
        { "m31", 2, 0 }, { "m32", 2, 1 }, { "m33", 2, 2 }, { "m34", 2, 3 },
        { "m41", 3, 0 }, { "m42", 3, 1 }, { "m43", 3, 2 }, { "m44", 3, 3 },
    };
    for (Member const& member : members) {
        int const i = member.i;
        int const j = member.j;
        auto getter = [i, j](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
            if (!self)
                return std::nullopt;
            return js::Value::number((*self)->matrix.at(i, j));
        };
        define_getter(in, *readonly, member.name, getter);
        bool const three_d_member = !((i == 0 || i == 1 || i == 3) && (j == 0 || j == 1));
        bool const default_one = i == j;
        define_getter(in, *matrix, member.name, getter,
            [i, j, three_d_member, default_one](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
                if (!self)
                    return std::nullopt;
                std::optional<double> const value = interp.to_number(js::argument(args, 0));
                if (!value)
                    return std::nullopt;
                (*self)->matrix.at(i, j) = *value;
                if (three_d_member && *value != (default_one ? 1.0 : 0.0))
                    (*self)->matrix.is_2d = false;
                return js::Value::undefined();
            });
    }
    define_getter(in, *readonly, "is2D", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        return js::Value::boolean((*self)->matrix.is_2d);
    });
    define_getter(in, *readonly, "isIdentity", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        return js::Value::boolean((*self)->matrix.is_identity());
    });

    define_matrix_step(in, *readonly, *matrix, "translate", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        js::Interpreter& interp = internals.interpreter;
        std::optional<double> const tx = number_argument(interp, args, 0, 0);
        std::optional<double> const ty = tx ? number_argument(interp, args, 1, 0) : std::nullopt;
        std::optional<double> const tz = ty ? number_argument(interp, args, 2, 0) : std::nullopt;
        if (!tz)
            return std::nullopt;
        return m.multiply(translation(*tx, *ty, *tz));
    });
    define_matrix_step(in, *readonly, *matrix, "scale", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        js::Interpreter& interp = internals.interpreter;
        double v[6] = { 1, 0, 1, 0, 0, 0 };
        for (std::size_t k = 0; k < 6; ++k) {
            std::optional<double> const value = number_argument(interp, args, k, v[k]);
            if (!value)
                return std::nullopt;
            v[k] = *value;
        }
        if (js::argument(args, 1).is_undefined())
            v[1] = v[0];
        return m.multiply(translation(v[3], v[4], v[5])).multiply(scaling(v[0], v[1], v[2])).multiply(translation(-v[3], -v[4], -v[5]));
    });
    define_matrix_step(in, *readonly, *matrix, "scale3d", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        js::Interpreter& interp = internals.interpreter;
        double v[4] = { 1, 0, 0, 0 };
        for (std::size_t k = 0; k < 4; ++k) {
            std::optional<double> const value = number_argument(interp, args, k, v[k]);
            if (!value)
                return std::nullopt;
            v[k] = *value;
        }
        return m.multiply(translation(v[1], v[2], v[3])).multiply(scaling(v[0], v[0], v[0])).multiply(translation(-v[1], -v[2], -v[3]));
    });
    define_operation(interpreter, *readonly, "scaleNonUniform", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        std::optional<double> const sx = number_argument(interp, args, 0, 1);
        std::optional<double> const sy = sx ? number_argument(interp, args, 1, 1) : std::nullopt;
        if (!sy)
            return std::nullopt;
        return new_matrix(internals_of(interp), (*self)->matrix.multiply(scaling(*sx, *sy, 1)), true);
    });
    define_matrix_step(in, *readonly, *matrix, "rotate", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        js::Interpreter& interp = internals.interpreter;
        std::optional<double> rx = number_argument(interp, args, 0, 0);
        std::optional<double> ry = rx ? number_argument(interp, args, 1, 0) : std::nullopt;
        std::optional<double> rz = ry ? number_argument(interp, args, 2, 0) : std::nullopt;
        if (!rz)
            return std::nullopt;
        if (js::argument(args, 1).is_undefined() && js::argument(args, 2).is_undefined()) {
            rz = *rx;
            rx = 0;
            ry = 0;
        }
        Matrix4 out = m.multiply(rotation(0, 0, 1, *rz));
        out = out.multiply(rotation(0, 1, 0, *ry));
        out = out.multiply(rotation(1, 0, 0, *rx));
        if (*rx != 0 || *ry != 0)
            out.is_2d = false;
        return out;
    });
    define_matrix_step(in, *readonly, *matrix, "rotateFromVector", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        js::Interpreter& interp = internals.interpreter;
        std::optional<double> const x = number_argument(interp, args, 0, 0);
        std::optional<double> const y = x ? number_argument(interp, args, 1, 0) : std::nullopt;
        if (!y)
            return std::nullopt;
        double const degrees = (*x == 0 && *y == 0) ? 0 : std::atan2(*y, *x) * 180 / pi;
        return m.multiply(rotation(0, 0, 1, degrees));
    });
    define_matrix_step(in, *readonly, *matrix, "rotateAxisAngle", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        js::Interpreter& interp = internals.interpreter;
        double v[4] = { 0, 0, 0, 0 };
        for (std::size_t k = 0; k < 4; ++k) {
            std::optional<double> const value = number_argument(interp, args, k, 0);
            if (!value)
                return std::nullopt;
            v[k] = *value;
        }
        Matrix4 out = m.multiply(rotation(v[0], v[1], v[2], v[3]));
        if (v[0] != 0 || v[1] != 0)
            out.is_2d = false;
        return out;
    });
    define_matrix_step(in, *readonly, *matrix, "skewX", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        std::optional<double> const sx = number_argument(internals.interpreter, args, 0, 0);
        if (!sx)
            return std::nullopt;
        return m.multiply(Matrix4::from_2d(1, 0, tangent_of_degrees(*sx), 1, 0, 0));
    });
    define_matrix_step(in, *readonly, *matrix, "skewY", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        std::optional<double> const sy = number_argument(internals.interpreter, args, 0, 0);
        if (!sy)
            return std::nullopt;
        return m.multiply(Matrix4::from_2d(1, tangent_of_degrees(*sy), 0, 1, 0, 0));
    });
    define_matrix_step(in, *readonly, *matrix, "multiply", 0, [](Realm::Internals& internals, Matrix4 const& m, Args args) -> std::optional<Matrix4> {
        std::optional<Matrix4> const other = matrix_from_init(internals, js::argument(args, 0));
        if (!other)
            return std::nullopt;
        return m.multiply(*other);
    });
    define_operation(interpreter, *matrix, "preMultiplySelf", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        std::optional<Matrix4> const other = matrix_from_init(internals_of(interp), js::argument(args, 0));
        if (!other)
            return std::nullopt;
        (*self)->matrix = other->multiply((*self)->matrix);
        return this_value;
    });
    define_operation(interpreter, *readonly, "flipX", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        return new_matrix(internals_of(interp), (*self)->matrix.multiply(Matrix4::from_2d(-1, 0, 0, 1, 0, 0)), true);
    });
    define_operation(interpreter, *readonly, "flipY", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        return new_matrix(internals_of(interp), (*self)->matrix.multiply(Matrix4::from_2d(1, 0, 0, -1, 0, 0)), true);
    });
    define_matrix_step(in, *readonly, *matrix, "invert", 0, [](Realm::Internals&, Matrix4 const& m, Args) -> std::optional<Matrix4> {
        return inverse_or_nan(m);
    });
    // The read-only form of invertSelf is named inverse.
    define_operation(interpreter, *readonly, "inverse", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        return new_matrix(internals_of(interp), inverse_or_nan((*self)->matrix), true);
    });
    readonly->delete_property(interpreter.key("invert"));
    define_operation(interpreter, *matrix, "setMatrixValue", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        std::optional<Matrix4> const parsed = parse_transform_list(*text);
        if (!parsed)
            return internals.throw_dom_exception("SyntaxError", "Failed to execute 'setMatrixValue': the string is not a transform list.");
        (*self)->matrix = *parsed;
        return this_value;
    });
    define_operation(interpreter, *readonly, "transformPoint", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::array<double, 4>> p = point_from_init(internals, js::argument(args, 0));
        if (!p)
            return std::nullopt;
        double out[4];
        transform_point((*self)->matrix, p->data(), out);
        return new_point(internals, out, true);
    });
    for (bool const wide : { false, true }) {
        define_operation(interpreter, *readonly, wide ? "toFloat64Array" : "toFloat32Array", 0,
            [wide](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
                if (!self)
                    return std::nullopt;
                Matrix4 const values = (*self)->matrix;
                std::optional<js::TypedArrayObject*> const array
                    = js::new_typed_array(interp, wide ? js::ElementType::Float64 : js::ElementType::Float32, 16);
                if (!array)
                    return std::nullopt;
                for (std::size_t i = 0; i < 16; ++i)
                    (*array)->set_element(i, values.m[i]);
                return js::Value::object(*array);
            });
    }
    define_operation(interpreter, *readonly, "toJSON", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        Matrix4 const m = (*self)->matrix;
        js::Object* object = interp.new_object(interp.intrinsics().object_prototype);
        js::Interpreter::Roots const roots(interp);
        interp.root(js::Value::object(object));
        for (Member const& member : members)
            interp.create_data_property(*object, interp.key(member.name), js::Value::number(m.at(member.i, member.j)));
        interp.create_data_property(*object, interp.key("is2D"), js::Value::boolean(m.is_2d));
        interp.create_data_property(*object, interp.key("isIdentity"), js::Value::boolean(m.is_identity()));
        return js::Value::object(object);
    });
    define_operation(interpreter, *readonly, "toString", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<DomMatrixObject*> const self = this_matrix(interp, this_value);
        if (!self)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        for (double const value : (*self)->matrix.m) {
            if (!std::isfinite(value))
                return internals.throw_dom_exception("InvalidStateError", "A matrix with a value that is not finite has no string.");
        }
        return internals.string(matrix_text((*self)->matrix));
    });

    for (bool const mutable_one : { false, true }) {
        js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key(mutable_one ? "DOMMatrix" : "DOMMatrixReadOnly"));
        js::Object& target = *constructor.as_object();
        define_operation(interpreter, target, "fromMatrix", 0, [mutable_one](js::Interpreter& interp, js::Value const&, Args args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Value const init = js::argument(args, 0);
            std::optional<MatrixFields> const fields = read_matrix_fields(internals, init, true);
            if (!fields)
                return std::nullopt;
            std::optional<Matrix4> const m = matrix_from_fields(internals, *fields, true);
            if (!m)
                return std::nullopt;
            return new_matrix(internals, *m, mutable_one);
        });
        for (bool const wide : { false, true }) {
            define_operation(interpreter, target, wide ? "fromFloat64Array" : "fromFloat32Array", 1,
                [mutable_one, wide](js::Interpreter& interp, js::Value const&, Args args) -> Native {
                    Realm::Internals& internals = internals_of(interp);
                    js::Value const value = js::argument(args, 0);
                    auto* array = value.is_object() && value.as_object()->class_id() == js::Object::Class::TypedArray
                        ? static_cast<js::TypedArrayObject*>(value.as_object())
                        : nullptr;
                    if (!array || array->element_type() != (wide ? js::ElementType::Float64 : js::ElementType::Float32))
                        return interp.throw_type_error(wide ? "The argument is not a Float64Array." : "The argument is not a Float32Array.");
                    std::vector<double> numbers;
                    for (std::size_t i = 0; i < array->length(); ++i)
                        numbers.push_back(array->get_element(i).as_number());
                    std::optional<Matrix4> const m = matrix_from_sequence(internals, numbers);
                    if (!m)
                        return std::nullopt;
                    return new_matrix(internals, *m, mutable_one);
                });
        }
    }
    // WebKitCSSMatrix is DOMMatrix under its old name.
    interpreter.global()->put(interpreter.key("WebKitCSSMatrix"),
        *interpreter.get(*interpreter.global(), interpreter.key("DOMMatrix")), js::builtin_attributes);
}

}
