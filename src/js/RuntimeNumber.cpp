#include "js/Runtime.h"

// Boolean (§20.3), Number (§21.1), Math (§21.3) and the global object's
// own functions and values (§19.1, §19.2): eval, isFinite, isNaN,
// parseFloat, parseInt, the URI coders, and Annex B's escape/unescape.

#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

double number_exponentiate(double base, double exponent)
{
    // Number::exponentiate (§6.1.6.1.3): the cases where the language and
    // the C library disagree are spelled out.
    if (std::isnan(exponent))
        return std::numeric_limits<double>::quiet_NaN();
    if (exponent == 0)
        return 1;
    if (std::isnan(base))
        return std::numeric_limits<double>::quiet_NaN();
    if (std::isinf(exponent) && std::abs(base) == 1)
        return std::numeric_limits<double>::quiet_NaN();
    return std::pow(base, exponent);
}

namespace {

constexpr double max_safe_integer = 9007199254740991.0;

Value number_string(Interpreter& in, double value)
{
    std::optional<JsString*> const text = in.to_string(Value::number(value));
    return Value::string(*text);
}

// ------------------------------------------------------------- Boolean

void install_boolean_library(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    NativeFunction* constructor = in.new_native(
        "Boolean", 1,
        [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
            return Value::boolean(Interpreter::to_boolean(argument(args, 0)));
        },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            bool const value = Interpreter::to_boolean(argument(args, 0));
            std::optional<Object*> const prototype = interp.get_prototype_from_constructor(new_target, interp.intrinsics().boolean_prototype);
            if (!prototype)
                return std::nullopt;
            return Value::object(interp.heap().allocate<PrimitiveObject>(*prototype, Object::Class::Boolean, Value::boolean(value)));
        });
    i.boolean_constructor = constructor;
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(i.boolean_prototype), frozen_attributes);
    i.boolean_prototype->put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("Boolean"), Value::object(constructor), builtin_attributes);
    define_method(in, *i.boolean_prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<bool> const value = this_boolean_value(interp, this_value, "toString");
        if (!value)
            return std::nullopt;
        return Value::string(*value ? interp.atoms().true_ : interp.atoms().false_);
    });
    define_method(in, *i.boolean_prototype, "valueOf", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<bool> const value = this_boolean_value(interp, this_value, "valueOf");
        if (!value)
            return std::nullopt;
        return Value::boolean(*value);
    });
}

// -------------------------------------------------------------- Number

std::optional<Value> number_to_string_method(Interpreter& interp, Value const& this_value, Args args)
{
    // §21.1.3.6: the radix defaults to 10 and must be 2 … 36.
    std::optional<double> const value = this_number_value(interp, this_value, "toString");
    if (!value)
        return std::nullopt;
    int radix = 10;
    if (!argument(args, 0).is_undefined()) {
        std::optional<double> const requested = interp.to_integer_or_infinity(argument(args, 0));
        if (!requested)
            return std::nullopt;
        if (*requested < 2 || *requested > 36)
            return interp.throw_range_error("toString() radix must be between 2 and 36");
        radix = static_cast<int>(*requested);
    }
    if (radix == 10)
        return number_string(interp, *value);
    return Value::string(interp.string(std::u16string_view(number_to_string(*value, radix))));
}

void install_number_library(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    NativeFunction* constructor = in.new_native(
        "Number", 1,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            if (args.empty())
                return Value::number(0);
            std::optional<double> const number = interp.to_number(args[0]);
            if (!number)
                return std::nullopt;
            return Value::number(*number);
        },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            double number = 0;
            if (!args.empty()) {
                std::optional<double> const converted = interp.to_number(args[0]);
                if (!converted)
                    return std::nullopt;
                number = *converted;
            }
            std::optional<Object*> const prototype = interp.get_prototype_from_constructor(new_target, interp.intrinsics().number_prototype);
            if (!prototype)
                return std::nullopt;
            return Value::object(interp.heap().allocate<PrimitiveObject>(*prototype, Object::Class::Number, Value::number(number)));
        });
    i.number_constructor = constructor;
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(i.number_prototype), frozen_attributes);
    i.number_prototype->put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("Number"), Value::object(constructor), builtin_attributes);

    define_value(in, *constructor, "EPSILON", Value::number(std::numeric_limits<double>::epsilon()), frozen_attributes);
    define_value(in, *constructor, "MAX_SAFE_INTEGER", Value::number(max_safe_integer), frozen_attributes);
    define_value(in, *constructor, "MAX_VALUE", Value::number(std::numeric_limits<double>::max()), frozen_attributes);
    define_value(in, *constructor, "MIN_SAFE_INTEGER", Value::number(-max_safe_integer), frozen_attributes);
    define_value(in, *constructor, "MIN_VALUE", Value::number(std::numeric_limits<double>::denorm_min()), frozen_attributes);
    define_value(in, *constructor, "NaN", Value::number(std::numeric_limits<double>::quiet_NaN()), frozen_attributes);
    define_value(in, *constructor, "NEGATIVE_INFINITY", Value::number(-std::numeric_limits<double>::infinity()), frozen_attributes);
    define_value(in, *constructor, "POSITIVE_INFINITY", Value::number(std::numeric_limits<double>::infinity()), frozen_attributes);

    define_method(in, *constructor, "isFinite", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        return Value::boolean(value.is_number() && std::isfinite(value.as_number()));
    });
    define_method(in, *constructor, "isInteger", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        return Value::boolean(value.is_number() && std::isfinite(value.as_number()) && std::trunc(value.as_number()) == value.as_number());
    });
    define_method(in, *constructor, "isNaN", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        return Value::boolean(value.is_number() && std::isnan(value.as_number()));
    });
    define_method(in, *constructor, "isSafeInteger", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        if (!value.is_number() || !std::isfinite(value.as_number()))
            return Value::boolean(false);
        double const number = value.as_number();
        return Value::boolean(std::trunc(number) == number && std::abs(number) <= max_safe_integer);
    });
    // §21.1.2.12–13: Number.parseFloat and Number.parseInt are the very
    // same function objects as the globals.
    NativeFunction* parse_float_function = define_method(in, *in.global(), "parseFloat", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<JsString*> const text = interp.to_string(argument(args, 0));
        if (!text)
            return std::nullopt;
        return Value::number(parse_float((*text)->view()));
    });
    constructor->put(in.key("parseFloat"), Value::object(parse_float_function), builtin_attributes);
    NativeFunction* parse_int_function = define_method(in, *in.global(), "parseInt", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<JsString*> const text = interp.to_string(argument(args, 0));
        if (!text)
            return std::nullopt;
        interp.root(Value::string(*text));
        std::optional<std::int32_t> const radix = interp.to_int32(argument(args, 1));
        if (!radix)
            return std::nullopt;
        return Value::number(parse_int((*text)->view(), *radix));
    });
    constructor->put(in.key("parseInt"), Value::object(parse_int_function), builtin_attributes);

    Object& prototype = *i.number_prototype;
    define_method(in, prototype, "toExponential", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §21.1.3.2: the digit count is checked after the conversion,
        // and only when the number is finite.
        std::optional<double> const value = this_number_value(interp, this_value, "toExponential");
        if (!value)
            return std::nullopt;
        std::optional<double> const digits = interp.to_integer_or_infinity(argument(args, 0));
        if (!digits)
            return std::nullopt;
        if (!std::isfinite(*value))
            return number_string(interp, *value);
        if (*digits < 0 || *digits > 100)
            return interp.throw_range_error("toExponential() argument must be between 0 and 100");
        std::optional<int> const fraction_digits = argument(args, 0).is_undefined() ? std::nullopt : std::optional<int>(static_cast<int>(*digits));
        return Value::string(interp.string(std::u16string_view(number_to_exponential(*value, fraction_digits))));
    });
    define_method(in, prototype, "toFixed", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §21.1.3.3.
        std::optional<double> const value = this_number_value(interp, this_value, "toFixed");
        if (!value)
            return std::nullopt;
        std::optional<double> const digits = interp.to_integer_or_infinity(argument(args, 0));
        if (!digits)
            return std::nullopt;
        if (!std::isfinite(*digits) || *digits < 0 || *digits > 100)
            return interp.throw_range_error("toFixed() digits argument must be between 0 and 100");
        if (!std::isfinite(*value) || std::abs(*value) >= 1e21)
            return number_string(interp, *value);
        return Value::string(interp.string(std::u16string_view(number_to_fixed(*value, static_cast<int>(*digits)))));
    });
    define_method(in, prototype, "toLocaleString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<double> const value = this_number_value(interp, this_value, "toLocaleString");
        if (!value)
            return std::nullopt;
        return number_string(interp, *value);
    });
    define_method(in, prototype, "toPrecision", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §21.1.3.5.
        std::optional<double> const value = this_number_value(interp, this_value, "toPrecision");
        if (!value)
            return std::nullopt;
        if (argument(args, 0).is_undefined())
            return number_string(interp, *value);
        std::optional<double> const precision = interp.to_integer_or_infinity(argument(args, 0));
        if (!precision)
            return std::nullopt;
        if (!std::isfinite(*value))
            return number_string(interp, *value);
        if (*precision < 1 || *precision > 100)
            return interp.throw_range_error("toPrecision() argument must be between 1 and 100");
        return Value::string(interp.string(std::u16string_view(number_to_precision(*value, static_cast<int>(*precision)))));
    });
    define_method(in, prototype, "toString", 1, number_to_string_method);
    define_method(in, prototype, "valueOf", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<double> const value = this_number_value(interp, this_value, "valueOf");
        if (!value)
            return std::nullopt;
        return Value::number(*value);
    });
}

// ---------------------------------------------------------------- Math

// Every Math function converts its arguments in order before computing
// anything (§21.3.2), which this helper does for the one-argument case.
using Unary = double (*)(double);

void define_unary(Interpreter& in, Object& math, std::string_view name, Unary function)
{
    define_method(in, math, name, 1, [function](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<double> const x = interp.to_number(argument(args, 0));
        if (!x)
            return std::nullopt;
        return Value::number(function(*x));
    });
}

double math_round(double x)
{
    // §21.3.2.28: halves round toward +∞, and the sign of a zero result
    // follows the argument.
    if (!std::isfinite(x) || x == 0)
        return x;
    if (x > 0 && x < 0.5)
        return 0.0;
    if (x < 0 && x >= -0.5)
        return -0.0;
    double const floor = std::floor(x);
    return x - floor >= 0.5 ? floor + 1 : floor;
}

double math_sign(double x)
{
    if (std::isnan(x) || x == 0)
        return x;
    return x > 0 ? 1.0 : -1.0;
}

double math_fround(double x)
{
    return static_cast<double>(static_cast<float>(x));
}

void install_math_library(Interpreter& in)
{
    Heap::NoCollect const guard(in.heap());
    Object* math = in.heap().allocate<Object>(in.intrinsics().object_prototype, Object::Class::Math);
    in.intrinsics().math = math;
    in.global()->put(in.key("Math"), Value::object(math), builtin_attributes);
    math->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), Value::string(in.atom("Math")), Configurable);

    define_value(in, *math, "E", Value::number(2.718281828459045), frozen_attributes);
    define_value(in, *math, "LN10", Value::number(2.302585092994046), frozen_attributes);
    define_value(in, *math, "LN2", Value::number(0.6931471805599453), frozen_attributes);
    define_value(in, *math, "LOG10E", Value::number(0.4342944819032518), frozen_attributes);
    define_value(in, *math, "LOG2E", Value::number(1.4426950408889634), frozen_attributes);
    define_value(in, *math, "PI", Value::number(3.141592653589793), frozen_attributes);
    define_value(in, *math, "SQRT1_2", Value::number(0.7071067811865476), frozen_attributes);
    define_value(in, *math, "SQRT2", Value::number(1.4142135623730951), frozen_attributes);

    define_unary(in, *math, "abs", [](double x) { return std::abs(x); });
    define_unary(in, *math, "acos", [](double x) { return std::acos(x); });
    define_unary(in, *math, "acosh", [](double x) { return std::acosh(x); });
    define_unary(in, *math, "asin", [](double x) { return std::asin(x); });
    define_unary(in, *math, "asinh", [](double x) { return std::asinh(x); });
    define_unary(in, *math, "atan", [](double x) { return std::atan(x); });
    define_unary(in, *math, "atanh", [](double x) { return std::atanh(x); });
    define_unary(in, *math, "cbrt", [](double x) {
        // The C library's cube root is a rounding off on some platforms
        // (27 is not always 3); the nearest of the three candidates whose
        // cube lands closest is the same everywhere.
        if (!std::isfinite(x) || x == 0)
            return x;
        double const estimate = std::cbrt(x);
        double best = estimate;
        double best_error = std::abs(estimate * estimate * estimate - x);
        for (double const candidate : { std::nextafter(estimate, -std::numeric_limits<double>::infinity()), std::nextafter(estimate, std::numeric_limits<double>::infinity()) }) {
            double const error = std::abs(candidate * candidate * candidate - x);
            if (error < best_error) {
                best = candidate;
                best_error = error;
            }
        }
        return best;
    });
    define_unary(in, *math, "ceil", [](double x) { return std::ceil(x); });
    define_unary(in, *math, "cos", [](double x) { return std::cos(x); });
    define_unary(in, *math, "cosh", [](double x) { return std::cosh(x); });
    define_unary(in, *math, "exp", [](double x) { return std::exp(x); });
    define_unary(in, *math, "expm1", [](double x) { return std::expm1(x); });
    define_unary(in, *math, "floor", [](double x) { return std::floor(x); });
    define_unary(in, *math, "fround", math_fround);
    define_unary(in, *math, "log", [](double x) { return std::log(x); });
    define_unary(in, *math, "log1p", [](double x) { return std::log1p(x); });
    define_unary(in, *math, "log10", [](double x) { return std::log10(x); });
    define_unary(in, *math, "log2", [](double x) { return std::log2(x); });
    define_unary(in, *math, "round", math_round);
    define_unary(in, *math, "sign", math_sign);
    define_unary(in, *math, "sin", [](double x) { return std::sin(x); });
    define_unary(in, *math, "sinh", [](double x) { return std::sinh(x); });
    define_unary(in, *math, "sqrt", [](double x) { return std::sqrt(x); });
    define_unary(in, *math, "tan", [](double x) { return std::tan(x); });
    define_unary(in, *math, "tanh", [](double x) { return std::tanh(x); });
    define_unary(in, *math, "trunc", [](double x) { return std::trunc(x); });

    define_method(in, *math, "atan2", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<double> const y = interp.to_number(argument(args, 0));
        if (!y)
            return std::nullopt;
        std::optional<double> const x = interp.to_number(argument(args, 1));
        if (!x)
            return std::nullopt;
        return Value::number(std::atan2(*y, *x));
    });
    define_method(in, *math, "clz32", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<std::uint32_t> const n = interp.to_uint32(argument(args, 0));
        if (!n)
            return std::nullopt;
        int count = 0;
        for (std::uint32_t bit = 0x80000000u; bit != 0 && (*n & bit) == 0; bit >>= 1)
            ++count;
        return Value::number(*n == 0 ? 32.0 : static_cast<double>(count));
    });
    define_method(in, *math, "hypot", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §21.3.2.18: every argument coerces first; an infinity wins over
        // a NaN; the sum is scaled so it cannot overflow prematurely.
        std::vector<double> values;
        for (Value const& arg : args) {
            std::optional<double> const x = interp.to_number(arg);
            if (!x)
                return std::nullopt;
            values.push_back(*x);
        }
        bool has_nan = false;
        double largest = 0;
        for (double const x : values) {
            if (std::isinf(x))
                return Value::number(std::numeric_limits<double>::infinity());
            if (std::isnan(x))
                has_nan = true;
            largest = std::max(largest, std::abs(x));
        }
        if (has_nan)
            return Value::number(std::numeric_limits<double>::quiet_NaN());
        if (largest == 0)
            return Value::number(0);
        double sum = 0;
        for (double const x : values) {
            double const scaled = x / largest;
            sum += scaled * scaled;
        }
        return Value::number(std::sqrt(sum) * largest);
    });
    define_method(in, *math, "imul", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<std::int32_t> const a = interp.to_int32(argument(args, 0));
        if (!a)
            return std::nullopt;
        std::optional<std::int32_t> const b = interp.to_int32(argument(args, 1));
        if (!b)
            return std::nullopt;
        std::uint32_t const product = static_cast<std::uint32_t>(*a) * static_cast<std::uint32_t>(*b);
        return Value::number(static_cast<double>(static_cast<std::int32_t>(product)));
    });
    for (bool const is_max : { true, false }) {
        define_method(in, *math, is_max ? "max" : "min", 2, [is_max](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            // §21.3.2.24–25: all arguments coerce before any NaN wins,
            // and +0 beats −0 for max, the reverse for min.
            std::vector<double> values;
            for (Value const& arg : args) {
                std::optional<double> const x = interp.to_number(arg);
                if (!x)
                    return std::nullopt;
                values.push_back(*x);
            }
            double result = is_max ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            for (double const x : values) {
                if (std::isnan(x))
                    return Value::number(std::numeric_limits<double>::quiet_NaN());
                if (is_max) {
                    if (x > result || (x == 0 && result == 0 && !std::signbit(x)))
                        result = x;
                } else {
                    if (x < result || (x == 0 && result == 0 && std::signbit(x)))
                        result = x;
                }
            }
            return Value::number(result);
        });
    }
    define_method(in, *math, "pow", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<double> const base = interp.to_number(argument(args, 0));
        if (!base)
            return std::nullopt;
        std::optional<double> const exponent = interp.to_number(argument(args, 1));
        if (!exponent)
            return std::nullopt;
        return Value::number(number_exponentiate(*base, *exponent));
    });
    // The generator lives in the closure, one per realm: no shared state
    // between the realms the test runner drives on several threads.
    auto generator = std::make_shared<std::mt19937_64>(std::random_device {}());
    define_method(in, *math, "random", 0, [generator](Interpreter&, Value const&, Args) -> std::optional<Value> {
        std::uint64_t const bits = (*generator)() >> 11; // 53 random bits
        return Value::number(static_cast<double>(bits) / 9007199254740992.0);
    });
}

// -------------------------------------------------------- the URI coders

bool is_alnum(char16_t c)
{
    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

constexpr std::u16string_view uri_reserved = u";/?:@&=+$,";
constexpr std::u16string_view uri_unreserved_marks = u"-_.!~*'()";

// Encode (§19.2.6.5): the code points outside `unescaped` become the
// percent-encoded bytes of their UTF-8; a lone surrogate is a URIError.
std::optional<Value> uri_encode(Interpreter& interp, std::u16string_view text, bool component)
{
    std::u16string out;
    for (std::size_t k = 0; k < text.size();) {
        char16_t const c = text[k];
        bool unescaped = is_alnum(c) || uri_unreserved_marks.find(c) != std::u16string_view::npos;
        if (!component && (uri_reserved.find(c) != std::u16string_view::npos || c == u'#'))
            unescaped = true;
        if (unescaped) {
            out += c;
            ++k;
            continue;
        }
        std::size_t units = 1;
        char32_t const code_point = code_point_at(text, k, &units);
        if (code_point >= 0xD800 && code_point <= 0xDFFF)
            return interp.throw_error(ErrorType::UriError, "URI malformed");
        k += units;
        std::string utf8;
        append_code_point(out, 0); // placeholder, replaced below
        out.pop_back();
        std::u16string one;
        append_code_point(one, code_point);
        utf8 = utf8_from_utf16(one);
        for (unsigned char const byte : utf8) {
            char const digits[] = "0123456789ABCDEF";
            out += u'%';
            out += static_cast<char16_t>(digits[byte >> 4]);
            out += static_cast<char16_t>(digits[byte & 15]);
        }
    }
    return Value::string(interp.string(std::u16string_view(out)));
}

int hex_value(char16_t c)
{
    if (c >= u'0' && c <= u'9')
        return c - u'0';
    if (c >= u'a' && c <= u'f')
        return 10 + c - u'a';
    if (c >= u'A' && c <= u'F')
        return 10 + c - u'A';
    return -1;
}

// Decode (§19.2.6.6): each run of %XX is read as UTF-8; a decoded single
// byte that is in the reserved set keeps its escaped spelling.
std::optional<Value> uri_decode(Interpreter& interp, std::u16string_view text, bool component)
{
    std::u16string out;
    std::size_t const length = text.size();
    for (std::size_t k = 0; k < length; ++k) {
        char16_t const c = text[k];
        if (c != u'%') {
            out += c;
            continue;
        }
        std::size_t const start = k;
        if (k + 2 >= length)
            return interp.throw_error(ErrorType::UriError, "URI malformed");
        int const high = hex_value(text[k + 1]);
        int const low = hex_value(text[k + 2]);
        if (high < 0 || low < 0)
            return interp.throw_error(ErrorType::UriError, "URI malformed");
        auto const byte = static_cast<unsigned char>(high * 16 + low);
        k += 2;
        if ((byte & 0x80) == 0) {
            auto const ch = static_cast<char16_t>(byte);
            bool const reserved = !component && (uri_reserved.find(ch) != std::u16string_view::npos || ch == u'#');
            if (reserved)
                out += text.substr(start, 3);
            else
                out += ch;
            continue;
        }
        int n = 0;
        if ((byte & 0xE0) == 0xC0)
            n = 2;
        else if ((byte & 0xF0) == 0xE0)
            n = 3;
        else if ((byte & 0xF8) == 0xF0)
            n = 4;
        else
            return interp.throw_error(ErrorType::UriError, "URI malformed");
        std::string bytes(1, static_cast<char>(byte));
        if (k + 3 * static_cast<std::size_t>(n - 1) >= length + 0 && k + 3 * static_cast<std::size_t>(n - 1) > length - 1)
            return interp.throw_error(ErrorType::UriError, "URI malformed");
        for (int j = 1; j < n; ++j) {
            ++k;
            if (k >= length || text[k] != u'%' || k + 2 >= length)
                return interp.throw_error(ErrorType::UriError, "URI malformed");
            int const h = hex_value(text[k + 1]);
            int const l = hex_value(text[k + 2]);
            if (h < 0 || l < 0)
                return interp.throw_error(ErrorType::UriError, "URI malformed");
            auto const continuation = static_cast<unsigned char>(h * 16 + l);
            if ((continuation & 0xC0) != 0x80)
                return interp.throw_error(ErrorType::UriError, "URI malformed");
            bytes += static_cast<char>(continuation);
            k += 2;
        }
        // The sequence must be the shortest form of a scalar value.
        char32_t code_point = 0;
        if (n == 2)
            code_point = ((byte & 0x1Fu) << 6) | (static_cast<unsigned char>(bytes[1]) & 0x3Fu);
        else if (n == 3)
            code_point = ((byte & 0x0Fu) << 12) | ((static_cast<unsigned char>(bytes[1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(bytes[2]) & 0x3Fu);
        else
            code_point = ((byte & 0x07u) << 18) | ((static_cast<unsigned char>(bytes[1]) & 0x3Fu) << 12)
                | ((static_cast<unsigned char>(bytes[2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(bytes[3]) & 0x3Fu);
        bool const overlong = (n == 2 && code_point < 0x80) || (n == 3 && code_point < 0x800) || (n == 4 && code_point < 0x10000);
        if (overlong || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
            return interp.throw_error(ErrorType::UriError, "URI malformed");
        append_code_point(out, code_point);
    }
    return Value::string(interp.string(std::u16string_view(out)));
}

// escape / unescape (B.2.1.1, B.2.1.2).
std::optional<Value> legacy_escape(Interpreter& interp, std::u16string_view text)
{
    constexpr std::u16string_view kept = u"@*_+-./";
    std::u16string out;
    char const digits[] = "0123456789ABCDEF";
    for (char16_t const c : text) {
        if (is_alnum(c) || kept.find(c) != std::u16string_view::npos) {
            out += c;
        } else if (c < 256) {
            out += u'%';
            out += static_cast<char16_t>(digits[c >> 4]);
            out += static_cast<char16_t>(digits[c & 15]);
        } else {
            out += u"%u";
            out += static_cast<char16_t>(digits[(c >> 12) & 15]);
            out += static_cast<char16_t>(digits[(c >> 8) & 15]);
            out += static_cast<char16_t>(digits[(c >> 4) & 15]);
            out += static_cast<char16_t>(digits[c & 15]);
        }
    }
    return Value::string(interp.string(std::u16string_view(out)));
}

std::optional<Value> legacy_unescape(Interpreter& interp, std::u16string_view text)
{
    std::u16string out;
    std::size_t const length = text.size();
    for (std::size_t k = 0; k < length; ++k) {
        char16_t c = text[k];
        if (c == u'%') {
            if (k + 5 < length && text[k + 1] == u'u') {
                int const a = hex_value(text[k + 2]);
                int const b = hex_value(text[k + 3]);
                int const d = hex_value(text[k + 4]);
                int const e = hex_value(text[k + 5]);
                if (a >= 0 && b >= 0 && d >= 0 && e >= 0) {
                    c = static_cast<char16_t>((a << 12) | (b << 8) | (d << 4) | e);
                    k += 5;
                }
            } else if (k + 2 < length) {
                int const a = hex_value(text[k + 1]);
                int const b = hex_value(text[k + 2]);
                if (a >= 0 && b >= 0) {
                    c = static_cast<char16_t>((a << 4) | b);
                    k += 2;
                }
            }
        }
        out += c;
    }
    return Value::string(interp.string(std::u16string_view(out)));
}

} // namespace

void install_boolean(Interpreter& in)
{
    install_boolean_library(in);
}

void install_number(Interpreter& in)
{
    install_number_library(in);
}

void install_math(Interpreter& in)
{
    install_math_library(in);
}

void install_global_functions(Interpreter& in)
{
    Heap::NoCollect const guard(in.heap());
    Object& global = *in.global();
    // §19.1: the value properties, none of them writable or configurable.
    define_value(in, global, "globalThis", Value::object(&global), builtin_attributes);
    define_value(in, global, "Infinity", Value::number(std::numeric_limits<double>::infinity()), frozen_attributes);
    define_value(in, global, "NaN", Value::number(std::numeric_limits<double>::quiet_NaN()), frozen_attributes);
    define_value(in, global, "undefined", Value::undefined(), frozen_attributes);

    in.intrinsics().eval = define_method(in, global, "eval", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §19.2.1, the indirect form: global scope, sloppy unless the
        // source itself says otherwise.
        Value const source = argument(args, 0);
        if (!source.is_string())
            return source;
        return interp.eval_in(source.as_string()->view(), nullptr, false, Value::empty());
    });
    define_method(in, global, "isFinite", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<double> const number = interp.to_number(argument(args, 0));
        if (!number)
            return std::nullopt;
        return Value::boolean(std::isfinite(*number));
    });
    define_method(in, global, "isNaN", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<double> const number = interp.to_number(argument(args, 0));
        if (!number)
            return std::nullopt;
        return Value::boolean(std::isnan(*number));
    });
    struct Coder {
        std::string_view name;
        bool decode;
        bool component;
    };
    constexpr Coder coders[] = {
        { "decodeURI", true, false },
        { "decodeURIComponent", true, true },
        { "encodeURI", false, false },
        { "encodeURIComponent", false, true },
    };
    for (Coder const& coder : coders) {
        bool const decode = coder.decode;
        bool const component = coder.component;
        define_method(in, global, coder.name, 1, [decode, component](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            std::optional<JsString*> const text = interp.to_string(argument(args, 0));
            if (!text)
                return std::nullopt;
            Interpreter::Roots const roots(interp);
            interp.root(Value::string(*text));
            return decode ? uri_decode(interp, (*text)->view(), component) : uri_encode(interp, (*text)->view(), component);
        });
    }
    define_method(in, global, "escape", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<JsString*> const text = interp.to_string(argument(args, 0));
        if (!text)
            return std::nullopt;
        return legacy_escape(interp, (*text)->view());
    });
    define_method(in, global, "unescape", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<JsString*> const text = interp.to_string(argument(args, 0));
        if (!text)
            return std::nullopt;
        return legacy_unescape(interp, (*text)->view());
    });
}

}
