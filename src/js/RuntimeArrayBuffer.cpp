#include "js/Runtime.h"

// ArrayBuffer (§25.1), DataView (§25.3), and the storage half of the typed
// arrays (§10.4.5): the bytes, the element types read and written through
// them, the bounds arithmetic of both kinds of view, and the exotic
// property behaviour of an Integer-Indexed object. %TypedArray%, its nine
// kinds and their methods are in RuntimeTypedArray.cpp.
//
// A typed array reads and writes in little-endian order — the platform
// order everywhere this engine runs, spelled out rather than trusted — and
// a DataView in the order each call asks for.

#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

// The largest buffer the engine makes. Past it, CreateByteDataBlock's
// RangeError (§6.2.9.1) is the answer rather than an attempt.
constexpr double max_buffer_byte_length = 2147483648.0; // 2^31

// ---- the element conversions (§7.1.6–§7.1.12)

std::uint8_t to_uint8_clamp(double number)
{
    // ToUint8Clamp (§7.1.12): NaN and everything at or below 0 become 0,
    // everything at or above 255 becomes 255, and between them a tie
    // rounds to even.
    if (std::isnan(number) || number <= 0)
        return 0;
    if (number >= 255)
        return 255;
    double const f = std::floor(number);
    if (f + 0.5 < number)
        return static_cast<std::uint8_t>(f + 1);
    if (number < f + 0.5)
        return static_cast<std::uint8_t>(f);
    return static_cast<std::uint8_t>(std::fmod(f, 2) == 0 ? f : f + 1);
}

// IEEE 754 binary16 from a double, rounded to nearest with ties to even:
// the value is scaled so that one unit in the last place is 1, rounded
// once, and reassembled. frexp, ldexp and nearbyint are exact, so every
// platform agrees.
std::uint16_t double_to_half(double number)
{
    if (std::isnan(number))
        return 0x7E00;
    std::uint16_t const sign = std::signbit(number) ? 0x8000 : 0;
    double const magnitude = std::fabs(number);
    if (std::isinf(magnitude))
        return static_cast<std::uint16_t>(sign | 0x7C00);
    if (magnitude == 0)
        return sign;
    int exponent = 0;
    std::frexp(magnitude, &exponent); // magnitude = f × 2^exponent, f in [0.5, 1)
    int const e = exponent - 1; // magnitude = (2f) × 2^e, 2f in [1, 2)
    if (e < -14) {
        // Subnormal, or below the smallest one: the value in units of
        // 2^-24, rounded. 1024 of them is the smallest normal, whose bit
        // pattern is exactly that number.
        double const units = std::nearbyint(std::ldexp(magnitude, 24));
        return static_cast<std::uint16_t>(sign | static_cast<std::uint16_t>(units));
    }
    double mantissa = std::nearbyint(std::ldexp(magnitude, 10 - e)); // in [1024, 2048]
    int biased = e + 15;
    if (mantissa == 2048) {
        mantissa = 1024;
        ++biased;
    }
    if (biased >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00);
    return static_cast<std::uint16_t>(sign | (biased << 10) | (static_cast<int>(mantissa) - 1024));
}

double half_to_double(std::uint16_t bits)
{
    int const exponent = (bits >> 10) & 0x1F;
    int const mantissa = bits & 0x3FF;
    double magnitude = 0;
    if (exponent == 0)
        magnitude = std::ldexp(static_cast<double>(mantissa), -24);
    else if (exponent == 31)
        magnitude = mantissa == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
    else
        magnitude = std::ldexp(1.0 + static_cast<double>(mantissa) / 1024.0, exponent - 15);
    return (bits & 0x8000) != 0 ? -magnitude : magnitude;
}

// A double narrowed to a float. C++ leaves a value past the largest float
// undefined; IEEE 754 rounds it to infinity from the midpoint between
// that float and 2^128 on, and so does this.
float double_to_float(double number)
{
    constexpr double overflow = 0x1.ffffffp127;
    if (std::isfinite(number) && std::fabs(number) >= overflow)
        return number < 0 ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    return static_cast<float>(number);
}

void store_bits(std::uint8_t* out, std::uint64_t bits, std::size_t size, bool little_endian)
{
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t const shift = little_endian ? i : size - 1 - i;
        out[i] = static_cast<std::uint8_t>(bits >> (8 * shift));
    }
}

std::uint64_t load_bits(std::uint8_t const* in, std::size_t size, bool little_endian)
{
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t const shift = little_endian ? i : size - 1 - i;
        bits |= static_cast<std::uint64_t>(in[i]) << (8 * shift);
    }
    return bits;
}

// The conversion for an element without the interpreter, for the one path
// that has none: a [[DefineOwnProperty]] called directly with a value the
// interpreter's wrapper has not converted. Every caller in the engine
// converts first, so an object here is unreachable; it becomes NaN (or a
// zero BigInt) rather than a call.
Value element_without_script(Heap& heap, ElementType type, Value const& value)
{
    if (is_bigint_element(type))
        return value.is_bigint() ? value : Value::bigint(heap.bigint(BigInteger()));
    switch (value.type()) {
    case Value::Type::Number:
        return value;
    case Value::Type::Null:
        return Value::number(0);
    case Value::Type::Boolean:
        return Value::number(value.as_boolean() ? 1 : 0);
    case Value::Type::String:
        return Value::number(string_to_number(value.as_string()->view()));
    case Value::Type::Undefined:
    case Value::Type::Empty:
    case Value::Type::Object:
    case Value::Type::Symbol:
    case Value::Type::BigInt:
        break;
    }
    return Value::number(std::numeric_limits<double>::quiet_NaN());
}

bool is_integral(double number)
{
    return std::isfinite(number) && std::floor(number) == number;
}

NativeFunction::Callback requires_new(std::string name)
{
    return [name](Interpreter& in, Value const&, Args) -> std::optional<Value> {
        return in.throw_type_error("Constructor " + name + " requires 'new'");
    };
}

std::optional<ArrayBufferObject*> this_array_buffer(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::ArrayBuffer)
        return in.throw_type_error("Method ArrayBuffer.prototype." + std::string(method) + " called on incompatible receiver " + in.describe(this_value));
    return static_cast<ArrayBufferObject*>(this_value.as_object());
}

std::optional<DataViewObject*> this_data_view(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::DataView)
        return in.throw_type_error("Method DataView.prototype." + std::string(method) + " called on incompatible receiver " + in.describe(this_value));
    return static_cast<DataViewObject*>(this_value.as_object());
}

// A relative position argument (§25.1.6.7 steps 6–11): negative counts
// from the end, both ends clamp, undefined is the fallback.
std::optional<double> relative_position(Interpreter& in, Value const& argument_value, double length, double fallback)
{
    if (argument_value.is_undefined())
        return fallback;
    std::optional<double> const relative = in.to_integer_or_infinity(argument_value);
    if (!relative)
        return std::nullopt;
    if (*relative < 0)
        return std::max(length + *relative, 0.0);
    return std::min(*relative, length);
}

// ArrayBufferCopyAndDetach (§25.1.3.3): the bytes move to a fresh buffer
// — resizable like the old one when asked, fixed otherwise — and the old
// one is detached.
std::optional<Value> copy_and_detach(Interpreter& in, Value const& this_value, Value const& new_length, bool preserve_resizability, std::string_view method)
{
    std::optional<ArrayBufferObject*> const found = this_array_buffer(in, this_value, method);
    if (!found)
        return std::nullopt;
    ArrayBufferObject& buffer = **found;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    double new_byte_length = static_cast<double>(buffer.byte_length());
    if (!new_length.is_undefined()) {
        std::optional<double> const index = in.to_index(new_length);
        if (!index)
            return std::nullopt;
        new_byte_length = *index;
    }
    if (buffer.is_detached())
        return in.throw_type_error("Cannot perform ArrayBuffer.prototype." + std::string(method) + " on a detached ArrayBuffer");
    std::optional<double> new_max_byte_length;
    if (preserve_resizability && buffer.is_resizable())
        new_max_byte_length = static_cast<double>(*buffer.max_byte_length());
    std::optional<ArrayBufferObject*> const fresh = allocate_array_buffer(in, nullptr, new_byte_length, new_max_byte_length);
    if (!fresh)
        return std::nullopt;
    std::size_t const count = std::min(static_cast<std::size_t>(new_byte_length), buffer.byte_length());
    if (count > 0)
        std::memcpy((*fresh)->data(), buffer.data(), count);
    buffer.detach();
    return Value::object(*fresh);
}

// GetViewValue (§25.3.1.5).
std::optional<Value> get_view_value(Interpreter& in, Value const& this_value, Value const& request_index, Value const& little_endian_value,
    ElementType type, std::string_view method)
{
    std::optional<DataViewObject*> const found = this_data_view(in, this_value, method);
    if (!found)
        return std::nullopt;
    DataViewObject& view = **found;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<double> const get_index = in.to_index(request_index);
    if (!get_index)
        return std::nullopt;
    bool const little_endian = Interpreter::to_boolean(little_endian_value);
    if (view.is_out_of_bounds())
        return in.throw_type_error("Cannot perform DataView.prototype." + std::string(method) + " on a detached or out-of-bounds DataView");
    double const view_size = static_cast<double>(view.view_byte_length());
    double const size = static_cast<double>(element_size(type));
    if (*get_index + size > view_size)
        return in.throw_range_error("Offset is outside the bounds of the DataView");
    std::size_t const buffer_index = static_cast<std::size_t>(*get_index) + view.byte_offset();
    return read_element_value(in.heap(), type, view.buffer()->data() + buffer_index, little_endian);
}

// SetViewValue (§25.3.1.6): the value is converted before the view is
// checked, since the conversion may run script that detaches it.
std::optional<Value> set_view_value(Interpreter& in, Value const& this_value, Value const& request_index, Value const& value,
    Value const& little_endian_value, ElementType type, std::string_view method)
{
    std::optional<DataViewObject*> const found = this_data_view(in, this_value, method);
    if (!found)
        return std::nullopt;
    DataViewObject& view = **found;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(value);
    std::optional<double> const get_index = in.to_index(request_index);
    if (!get_index)
        return std::nullopt;
    std::optional<Value> const numeric = to_element_value(in, type, value);
    if (!numeric)
        return std::nullopt;
    in.root(*numeric);
    bool const little_endian = Interpreter::to_boolean(little_endian_value);
    if (view.is_out_of_bounds())
        return in.throw_type_error("Cannot perform DataView.prototype." + std::string(method) + " on a detached or out-of-bounds DataView");
    double const view_size = static_cast<double>(view.view_byte_length());
    double const size = static_cast<double>(element_size(type));
    if (*get_index + size > view_size)
        return in.throw_range_error("Offset is outside the bounds of the DataView");
    std::size_t const buffer_index = static_cast<std::size_t>(*get_index) + view.byte_offset();
    write_element_value(type, view.buffer()->data() + buffer_index, *numeric, little_endian);
    return Value::undefined();
}

} // namespace

// ------------------------------------------------------ the element types

std::string_view element_type_name(ElementType type)
{
    switch (type) {
    case ElementType::Int8: return "Int8Array";
    case ElementType::Uint8: return "Uint8Array";
    case ElementType::Uint8Clamped: return "Uint8ClampedArray";
    case ElementType::Int16: return "Int16Array";
    case ElementType::Uint16: return "Uint16Array";
    case ElementType::Int32: return "Int32Array";
    case ElementType::Uint32: return "Uint32Array";
    case ElementType::Float16: return "Float16Array";
    case ElementType::Float32: return "Float32Array";
    case ElementType::Float64: return "Float64Array";
    case ElementType::BigInt64: return "BigInt64Array";
    case ElementType::BigUint64: return "BigUint64Array";
    }
    return "Int8Array";
}

std::optional<Value> to_element_value(Interpreter& in, ElementType type, Value const& value)
{
    // The conversion an element's kind asks for: ToBigInt for the BigInt
    // kinds (a Number is a TypeError there), ToNumber for the rest.
    if (is_bigint_element(type)) {
        std::optional<BigInt*> const big = in.to_bigint(value);
        if (!big)
            return std::nullopt;
        return Value::bigint(*big);
    }
    std::optional<double> const number = in.to_number(value);
    if (!number)
        return std::nullopt;
    return Value::number(*number);
}

void write_element_value(ElementType type, std::uint8_t* out, Value const& numeric, bool little_endian)
{
    if (is_bigint_element(type)) {
        // ToBigInt64 and ToBigUint64 (§7.1.15, §7.1.16): the low 64 bits.
        std::uint64_t const bits = numeric.is_bigint() ? numeric.as_bigint()->value().to_uint64_wrapping() : 0;
        store_bits(out, bits, 8, little_endian);
        return;
    }
    write_element(type, out, numeric.is_number() ? numeric.as_number() : std::numeric_limits<double>::quiet_NaN(), little_endian);
}

Value read_element_value(Heap& heap, ElementType type, std::uint8_t const* in, bool little_endian)
{
    if (type == ElementType::BigInt64)
        return Value::bigint(heap.bigint(BigInteger::from_int64(static_cast<std::int64_t>(load_bits(in, 8, little_endian)))));
    if (type == ElementType::BigUint64)
        return Value::bigint(heap.bigint(BigInteger::from_uint64(load_bits(in, 8, little_endian))));
    return Value::number(read_element(type, in, little_endian));
}

void write_element(ElementType type, std::uint8_t* out, double number, bool little_endian)
{
    // NumericToRawBytes (§25.1.3.17): the integer kinds through ToUint32's
    // modular wrap and then narrowed, the clamped byte its own way, the
    // floats by their bit patterns.
    switch (type) {
    case ElementType::Int8:
    case ElementType::Uint8:
    case ElementType::Int16:
    case ElementType::Uint16:
    case ElementType::Int32:
    case ElementType::Uint32:
        store_bits(out, Interpreter::double_to_uint32(number), element_size(type), little_endian);
        return;
    case ElementType::Uint8Clamped:
        out[0] = to_uint8_clamp(number);
        return;
    case ElementType::Float16:
        store_bits(out, double_to_half(number), 2, little_endian);
        return;
    case ElementType::Float32: {
        float const single = double_to_float(number);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &single, sizeof bits);
        store_bits(out, bits, 4, little_endian);
        return;
    }
    case ElementType::Float64: {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &number, sizeof bits);
        store_bits(out, bits, 8, little_endian);
        return;
    }
    case ElementType::BigInt64:
    case ElementType::BigUint64:
        // A Number never reaches a BigInt kind (write_element_value converts first).
        store_bits(out, 0, 8, little_endian);
        return;
    }
}

double read_element(ElementType type, std::uint8_t const* in, bool little_endian)
{
    // RawBytesToNumeric (§25.1.3.16).
    switch (type) {
    case ElementType::Int8:
        return static_cast<double>(static_cast<std::int8_t>(in[0]));
    case ElementType::Uint8:
    case ElementType::Uint8Clamped:
        return static_cast<double>(in[0]);
    case ElementType::Int16:
        return static_cast<double>(static_cast<std::int16_t>(static_cast<std::uint16_t>(load_bits(in, 2, little_endian))));
    case ElementType::Uint16:
        return static_cast<double>(load_bits(in, 2, little_endian));
    case ElementType::Int32:
        return static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(load_bits(in, 4, little_endian))));
    case ElementType::Uint32:
        return static_cast<double>(load_bits(in, 4, little_endian));
    case ElementType::Float16:
        return half_to_double(static_cast<std::uint16_t>(load_bits(in, 2, little_endian)));
    case ElementType::Float32: {
        std::uint32_t const bits = static_cast<std::uint32_t>(load_bits(in, 4, little_endian));
        float single = 0;
        std::memcpy(&single, &bits, sizeof single);
        return static_cast<double>(single);
    }
    case ElementType::Float64: {
        std::uint64_t const bits = load_bits(in, 8, little_endian);
        double number = 0;
        std::memcpy(&number, &bits, sizeof number);
        return number;
    }
    case ElementType::BigInt64:
        return static_cast<double>(static_cast<std::int64_t>(load_bits(in, 8, little_endian)));
    case ElementType::BigUint64:
        return static_cast<double>(load_bits(in, 8, little_endian));
    }
    return 0;
}

// ------------------------------------------------------ TypedArrayObject

TypedArrayObject::TypedArrayObject(Object* prototype, ElementType type, ArrayBufferObject* the_buffer, std::size_t offset,
    std::optional<std::size_t> count)
    : Object(prototype, Class::TypedArray)
    , m_buffer(the_buffer)
    , m_byte_offset(offset)
    , m_length(count)
    , m_type(type)
{
}

void TypedArrayObject::attach(ArrayBufferObject* the_buffer, std::size_t offset, std::optional<std::size_t> count)
{
    m_buffer = the_buffer;
    m_byte_offset = offset;
    m_length = count;
}

bool TypedArrayObject::is_out_of_bounds() const
{
    // IsTypedArrayOutOfBounds (§10.4.5.12), with a view that has no buffer
    // yet — one still being constructed — counted as out of bounds too.
    if (m_buffer == nullptr || m_buffer->is_detached())
        return true;
    std::size_t const buffer_length = m_buffer->byte_length();
    std::size_t const end = m_length ? m_byte_offset + *m_length * element_size() : buffer_length;
    return m_byte_offset > buffer_length || end > buffer_length;
}

std::size_t TypedArrayObject::length() const
{
    // TypedArrayLength (§10.4.5.13): a fixed count, or as many whole
    // elements as the buffer holds past the offset.
    if (is_out_of_bounds())
        return 0;
    if (m_length)
        return *m_length;
    return (m_buffer->byte_length() - m_byte_offset) / element_size();
}

bool TypedArrayObject::is_valid_index(double index) const
{
    // IsValidIntegerIndex (§10.4.5.15): an integer that is not −0, within
    // the current length of a view that is in bounds.
    if (!is_integral(index))
        return false;
    if (index == 0 && std::signbit(index))
        return false;
    if (index < 0)
        return false;
    return index < static_cast<double>(length());
}

Value TypedArrayObject::get_element(std::size_t index) const
{
    return read_element_value(*heap(), m_type, m_buffer->data() + m_byte_offset + index * element_size(), true);
}

void TypedArrayObject::set_element(std::size_t index, Value const& numeric)
{
    write_element_value(m_type, m_buffer->data() + m_byte_offset + index * element_size(), numeric, true);
}

std::optional<double> TypedArrayObject::numeric_index(PropertyKey const& key)
{
    // CanonicalNumericIndexString (§7.1.21) over a key: an index key is
    // one by construction; an atom is one when it survives a round trip
    // through ToNumber and ToString, or spells "-0". No canonical numeric
    // string starts with anything but a digit, a minus, an I or an N,
    // which spares every ordinary name the round trip.
    if (key.is_index())
        return static_cast<double>(key.as_index());
    if (!key.is_atom())
        return std::nullopt;
    std::u16string_view const text = key.as_atom()->view();
    if (text.empty())
        return std::nullopt;
    char16_t const first = text[0];
    bool const could_be = (first >= u'0' && first <= u'9') || first == u'-' || first == u'I' || first == u'N';
    if (!could_be || !is_canonical_numeric_string(text))
        return std::nullopt;
    if (text == u"-0")
        return -0.0;
    return string_to_number(text);
}

std::optional<bool> TypedArrayObject::define_numeric(Interpreter& in, double index, PropertyDescriptor const& desc)
{
    // [[DefineOwnProperty]] (§10.4.5.3) for a numeric key: the index must
    // be valid and the descriptor must ask for nothing an element cannot
    // be; then the value — converted by ToNumber, which may run script and
    // even detach the buffer — lands only if the index is still valid
    // (TypedArraySetElement, §10.4.5.17).
    if (!is_valid_index(index))
        return false;
    if (desc.configurable && !*desc.configurable)
        return false;
    if (desc.enumerable && !*desc.enumerable)
        return false;
    if (desc.is_accessor())
        return false;
    if (desc.writable && !*desc.writable)
        return false;
    if (desc.value) {
        Interpreter::Roots const roots(in);
        in.root(Value::object(this));
        std::optional<Value> const numeric = to_element_value(in, m_type, *desc.value);
        if (!numeric)
            return std::nullopt;
        if (is_valid_index(index))
            set_element(static_cast<std::size_t>(index), *numeric);
    }
    return true;
}

std::optional<PropertyDescriptor> TypedArrayObject::get_own_property(PropertyKey const& key) const
{
    // §10.4.5.1: an element is a writable, enumerable, configurable data
    // property; a numeric name that is no valid index is nothing at all.
    if (std::optional<double> const index = numeric_index(key)) {
        if (!is_valid_index(*index))
            return std::nullopt;
        return PropertyDescriptor::data(get_element(static_cast<std::size_t>(*index)), default_attributes);
    }
    return Object::get_own_property(key);
}

bool TypedArrayObject::define_own_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    // The script-free half of §10.4.5.3: the interpreter's wrapper
    // (Interpreter::define_own_property) routes a numeric key through
    // define_numeric first, so a value that arrives here is a Number.
    if (std::optional<double> const index = numeric_index(key)) {
        if (!is_valid_index(*index))
            return false;
        if (desc.configurable && !*desc.configurable)
            return false;
        if (desc.enumerable && !*desc.enumerable)
            return false;
        if (desc.is_accessor())
            return false;
        if (desc.writable && !*desc.writable)
            return false;
        if (desc.value)
            set_element(static_cast<std::size_t>(*index), element_without_script(*heap(), m_type, *desc.value));
        return true;
    }
    return Object::define_own_property(key, desc);
}

bool TypedArrayObject::has_property(PropertyKey const& key) const
{
    // §10.4.5.2: a numeric name is present exactly when it is a valid
    // index, and the chain is never asked about one.
    if (std::optional<double> const index = numeric_index(key))
        return is_valid_index(*index);
    return Object::has_property(key);
}

std::optional<Value> TypedArrayObject::get(Interpreter& interpreter, PropertyKey const& key, Value const& receiver)
{
    // §10.4.5.4: TypedArrayGetElement — undefined for an invalid index, and
    // no look up the chain either way.
    if (std::optional<double> const index = numeric_index(key)) {
        if (!is_valid_index(*index))
            return Value::undefined();
        return get_element(static_cast<std::size_t>(*index));
    }
    return Object::get(interpreter, key, receiver);
}

std::optional<bool> TypedArrayObject::set(Interpreter& interpreter, PropertyKey const& key, Value const& value, Value const& receiver)
{
    // §10.4.5.5: with the typed array itself as receiver the value is
    // converted first (ToNumber may run script) and stored if the index is
    // still valid — a write past the end is no error. With another
    // receiver an invalid index is silently fine, and a valid one goes the
    // ordinary way, to land on that receiver.
    if (std::optional<double> const index = numeric_index(key)) {
        if (receiver.is_object() && receiver.as_object() == this) {
            Interpreter::Roots const roots(interpreter);
            interpreter.root(Value::object(this));
            std::optional<Value> const numeric = to_element_value(interpreter, m_type, value);
            if (!numeric)
                return std::nullopt;
            if (is_valid_index(*index))
                set_element(static_cast<std::size_t>(*index), *numeric);
            return true;
        }
        if (!is_valid_index(*index))
            return true;
    }
    return Object::set(interpreter, key, value, receiver);
}

bool TypedArrayObject::delete_property(PropertyKey const& key)
{
    // §10.4.5.6: an element cannot be deleted; a numeric name that is no
    // element deletes fine.
    if (std::optional<double> const index = numeric_index(key))
        return !is_valid_index(*index);
    return Object::delete_property(key);
}

std::vector<PropertyKey> TypedArrayObject::own_keys() const
{
    // §10.4.5.7: every index in order, then the ordinary keys, none of
    // which is numeric — a numeric name is never stored as a property.
    std::vector<PropertyKey> const base = Object::own_keys();
    std::size_t const count = length();
    std::vector<PropertyKey> keys;
    keys.reserve(count + base.size());
    for (std::size_t i = 0; i < count; ++i)
        keys.push_back(PropertyKey::index(static_cast<std::uint32_t>(i)));
    keys.insert(keys.end(), base.begin(), base.end());
    return keys;
}

void TypedArrayObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_buffer);
}

// -------------------------------------------------------- DataViewObject

DataViewObject::DataViewObject(Object* prototype, ArrayBufferObject* the_buffer, std::size_t offset, std::optional<std::size_t> count)
    : Object(prototype, Class::DataView)
    , m_buffer(the_buffer)
    , m_byte_offset(offset)
    , m_byte_length(count)
{
}

bool DataViewObject::is_out_of_bounds() const
{
    // IsViewOutOfBounds (§25.3.1.3).
    if (m_buffer->is_detached())
        return true;
    std::size_t const buffer_length = m_buffer->byte_length();
    std::size_t const end = m_byte_length ? m_byte_offset + *m_byte_length : buffer_length;
    return m_byte_offset > buffer_length || end > buffer_length;
}

std::size_t DataViewObject::view_byte_length() const
{
    // GetViewByteLength (§25.3.1.4); 0 out of bounds.
    if (is_out_of_bounds())
        return 0;
    if (m_byte_length)
        return *m_byte_length;
    return m_buffer->byte_length() - m_byte_offset;
}

void DataViewObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_buffer);
}

// ------------------------------------------------------- shared helpers

std::optional<ArrayBufferObject*> allocate_array_buffer(Interpreter& in, Object* new_target, double byte_length, std::optional<double> max_byte_length)
{
    // AllocateArrayBuffer (§25.1.3.1): the length against the maximum
    // first, then the prototype (a getter may run), then the bytes.
    if (max_byte_length && byte_length > *max_byte_length)
        return in.throw_range_error("ArrayBuffer byteLength exceeds its maxByteLength");
    Interpreter::Roots const roots(in);
    if (new_target != nullptr)
        in.root(Value::object(new_target));
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target, in.intrinsics().array_buffer_prototype);
    if (!prototype)
        return std::nullopt;
    in.root(Value::object(*prototype));
    if (byte_length > max_buffer_byte_length || (max_byte_length && *max_byte_length > max_buffer_byte_length))
        return in.throw_range_error("Array buffer allocation failed");
    std::optional<std::size_t> maximum;
    if (max_byte_length)
        maximum = static_cast<std::size_t>(*max_byte_length);
    return in.heap().allocate<ArrayBufferObject>(*prototype, static_cast<std::size_t>(byte_length), maximum);
}

std::optional<TypedArrayObject*> validate_typed_array(Interpreter& in, Value const& value, std::string_view method)
{
    // ValidateTypedArray (§23.2.4.4).
    if (!value.is_object() || value.as_object()->class_id() != Object::Class::TypedArray)
        return in.throw_type_error(std::string(method) + " called on incompatible receiver " + in.describe(value));
    auto* array = static_cast<TypedArrayObject*>(value.as_object());
    if (array->is_out_of_bounds())
        return in.throw_type_error("Cannot perform " + std::string(method) + " on a detached or out-of-bounds TypedArray");
    return array;
}

// ----------------------------------------------------------- ArrayBuffer

void install_array_buffer(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());

    i.array_buffer_prototype = in.new_object();
    Object& prototype = *i.array_buffer_prototype;
    NativeFunction* constructor = in.new_native("ArrayBuffer", 1, requires_new("ArrayBuffer"),
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            // §25.1.4.1: the length, then the maxByteLength option (read
            // from an options object, ToIndex'd), then AllocateArrayBuffer.
            Interpreter::Roots const roots(interp);
            if (new_target != nullptr)
                interp.root(Value::object(new_target));
            Value const options = argument(args, 1);
            interp.root(options);
            std::optional<double> const byte_length = interp.to_index(argument(args, 0));
            if (!byte_length)
                return std::nullopt;
            std::optional<double> max_byte_length;
            if (options.is_object()) {
                std::optional<Value> const requested = interp.get(*options.as_object(), interp.key("maxByteLength"));
                if (!requested)
                    return std::nullopt;
                if (!requested->is_undefined()) {
                    std::optional<double> const maximum = interp.to_index(*requested);
                    if (!maximum)
                        return std::nullopt;
                    max_byte_length = *maximum;
                }
            }
            std::optional<ArrayBufferObject*> const buffer = allocate_array_buffer(interp, new_target, *byte_length, max_byte_length);
            if (!buffer)
                return std::nullopt;
            return Value::object(*buffer);
        });
    constructor->put(PropertyKey::atom(atoms.prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(atoms.constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("ArrayBuffer"), Value::object(constructor), builtin_attributes);
    i.array_buffer_constructor = constructor;

    // ArrayBuffer.isView (§25.1.5.1) and get [Symbol.species] (§25.1.5.3).
    define_method(in, *constructor, "isView", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        if (!value.is_object())
            return Value::boolean(false);
        Object::Class const kind = value.as_object()->class_id();
        return Value::boolean(kind == Object::Class::TypedArray || kind == Object::Class::DataView);
    });
    NativeFunction* species = in.new_native("get [Symbol.species]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> {
        return this_value;
    });
    constructor->put_accessor(PropertyKey::symbol(atoms.symbol_species), species, nullptr, Configurable);

    // The prototype's getters (§25.1.6.1–.5, .8): a detached buffer has no
    // length and no maximum, a fixed one's maximum is its length.
    define_accessor(in, prototype, "byteLength", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<ArrayBufferObject*> const buffer = this_array_buffer(interp, this_value, "byteLength");
        if (!buffer)
            return std::nullopt;
        return Value::number(static_cast<double>((*buffer)->byte_length()));
    });
    define_accessor(in, prototype, "maxByteLength", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<ArrayBufferObject*> const buffer = this_array_buffer(interp, this_value, "maxByteLength");
        if (!buffer)
            return std::nullopt;
        if ((*buffer)->is_detached())
            return Value::number(0);
        if ((*buffer)->is_resizable())
            return Value::number(static_cast<double>(*(*buffer)->max_byte_length()));
        return Value::number(static_cast<double>((*buffer)->byte_length()));
    });
    define_accessor(in, prototype, "resizable", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<ArrayBufferObject*> const buffer = this_array_buffer(interp, this_value, "resizable");
        if (!buffer)
            return std::nullopt;
        return Value::boolean((*buffer)->is_resizable());
    });
    define_accessor(in, prototype, "detached", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<ArrayBufferObject*> const buffer = this_array_buffer(interp, this_value, "detached");
        if (!buffer)
            return std::nullopt;
        return Value::boolean((*buffer)->is_detached());
    });

    define_method(in, prototype, "slice", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §25.1.6.7: the species constructor makes the new buffer, which
        // must be a fresh, attached, large enough ArrayBuffer — and the
        // source may have been detached meanwhile.
        std::optional<ArrayBufferObject*> const found = this_array_buffer(interp, this_value, "slice");
        if (!found)
            return std::nullopt;
        ArrayBufferObject& source = **found;
        if (source.is_detached())
            return interp.throw_type_error("Cannot perform ArrayBuffer.prototype.slice on a detached ArrayBuffer");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        double const length = static_cast<double>(source.byte_length());
        std::optional<double> const first = relative_position(interp, argument(args, 0), length, 0);
        if (!first)
            return std::nullopt;
        std::optional<double> const final = relative_position(interp, argument(args, 1), length, length);
        if (!final)
            return std::nullopt;
        double const new_length = std::max(*final - *first, 0.0);
        std::optional<Value> const slice_constructor = interp.species_constructor(source, interp.intrinsics().array_buffer_constructor);
        if (!slice_constructor)
            return std::nullopt;
        interp.root(*slice_constructor);
        Value const arguments[1] = { Value::number(new_length) };
        std::optional<Value> const made = interp.construct(*slice_constructor, arguments);
        if (!made)
            return std::nullopt;
        if (!made->is_object() || made->as_object()->class_id() != Object::Class::ArrayBuffer)
            return interp.throw_type_error("ArrayBuffer species constructor did not return an ArrayBuffer");
        auto& target = *static_cast<ArrayBufferObject*>(made->as_object());
        if (target.is_detached())
            return interp.throw_type_error("ArrayBuffer species constructor returned a detached ArrayBuffer");
        if (&target == &source)
            return interp.throw_type_error("ArrayBuffer species constructor returned the same ArrayBuffer");
        if (static_cast<double>(target.byte_length()) < new_length)
            return interp.throw_type_error("ArrayBuffer species constructor returned a buffer that is too small");
        if (source.is_detached())
            return interp.throw_type_error("Cannot perform ArrayBuffer.prototype.slice on a detached ArrayBuffer");
        double const current_length = static_cast<double>(source.byte_length());
        if (*first < current_length) {
            std::size_t const count = static_cast<std::size_t>(std::min(new_length, current_length - *first));
            if (count > 0)
                std::memcpy(target.data(), source.data() + static_cast<std::size_t>(*first), count);
        }
        return *made;
    });
    define_method(in, prototype, "resize", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §25.1.6.6.
        std::optional<ArrayBufferObject*> const found = this_array_buffer(interp, this_value, "resize");
        if (!found)
            return std::nullopt;
        ArrayBufferObject& buffer = **found;
        if (!buffer.is_resizable())
            return interp.throw_type_error("ArrayBuffer.prototype.resize called on a fixed-length ArrayBuffer");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<double> const new_length = interp.to_index(argument(args, 0));
        if (!new_length)
            return std::nullopt;
        if (buffer.is_detached())
            return interp.throw_type_error("Cannot perform ArrayBuffer.prototype.resize on a detached ArrayBuffer");
        if (*new_length > static_cast<double>(*buffer.max_byte_length()))
            return interp.throw_range_error("ArrayBuffer.prototype.resize: the new length exceeds the maxByteLength");
        buffer.resize(static_cast<std::size_t>(*new_length));
        return Value::undefined();
    });
    define_method(in, prototype, "transfer", 0, [](Interpreter& interp, Value const& this_value, Args args) {
        return copy_and_detach(interp, this_value, argument(args, 0), true, "transfer");
    });
    define_method(in, prototype, "transferToFixedLength", 0, [](Interpreter& interp, Value const& this_value, Args args) {
        return copy_and_detach(interp, this_value, argument(args, 0), false, "transferToFixedLength");
    });
    prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("ArrayBuffer")), Configurable);
}

// -------------------------------------------------------------- DataView

void install_data_view(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());

    i.data_view_prototype = in.new_object();
    Object& prototype = *i.data_view_prototype;
    NativeFunction* constructor = in.new_native("DataView", 1, requires_new("DataView"),
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            // §25.3.2.1: the buffer, the offset and the length are checked
            // before the prototype is read, and the buffer's state again
            // after, since reading the prototype may have run script.
            Interpreter::Roots const roots(interp);
            if (new_target != nullptr)
                interp.root(Value::object(new_target));
            Value const buffer_value = argument(args, 0);
            if (!buffer_value.is_object() || buffer_value.as_object()->class_id() != Object::Class::ArrayBuffer)
                return interp.throw_type_error("First argument to DataView constructor must be an ArrayBuffer");
            auto& buffer = *static_cast<ArrayBufferObject*>(buffer_value.as_object());
            interp.root(buffer_value);
            Value const length_value = argument(args, 2);
            interp.root(length_value);
            std::optional<double> const offset = interp.to_index(argument(args, 1));
            if (!offset)
                return std::nullopt;
            if (buffer.is_detached())
                return interp.throw_type_error("Cannot construct a DataView on a detached ArrayBuffer");
            double buffer_byte_length = static_cast<double>(buffer.byte_length());
            if (*offset > buffer_byte_length)
                return interp.throw_range_error("Start offset is outside the bounds of the buffer");
            std::optional<double> view_byte_length;
            if (length_value.is_undefined()) {
                if (!buffer.is_resizable())
                    view_byte_length = buffer_byte_length - *offset;
            } else {
                std::optional<double> const length = interp.to_index(length_value);
                if (!length)
                    return std::nullopt;
                if (*offset + *length > buffer_byte_length)
                    return interp.throw_range_error("Invalid DataView length");
                view_byte_length = *length;
            }
            std::optional<Object*> const proto = interp.get_prototype_from_constructor(new_target, interp.intrinsics().data_view_prototype);
            if (!proto)
                return std::nullopt;
            interp.root(Value::object(*proto));
            if (buffer.is_detached())
                return interp.throw_type_error("Cannot construct a DataView on a detached ArrayBuffer");
            buffer_byte_length = static_cast<double>(buffer.byte_length());
            if (*offset > buffer_byte_length)
                return interp.throw_range_error("Start offset is outside the bounds of the buffer");
            if (view_byte_length && *offset + *view_byte_length > buffer_byte_length)
                return interp.throw_range_error("Invalid DataView length");
            std::optional<std::size_t> count;
            if (view_byte_length)
                count = static_cast<std::size_t>(*view_byte_length);
            return Value::object(interp.heap().allocate<DataViewObject>(*proto, &buffer, static_cast<std::size_t>(*offset), count));
        });
    constructor->put(PropertyKey::atom(atoms.prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(atoms.constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("DataView"), Value::object(constructor), builtin_attributes);
    i.data_view_constructor = constructor;

    // The getters (§25.3.4.1–.3): the length and the offset of a view that
    // is out of bounds are a TypeError, not zero, unlike a typed array's.
    define_accessor(in, prototype, "buffer", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<DataViewObject*> const view = this_data_view(interp, this_value, "buffer");
        if (!view)
            return std::nullopt;
        return Value::object((*view)->buffer());
    });
    define_accessor(in, prototype, "byteLength", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<DataViewObject*> const view = this_data_view(interp, this_value, "byteLength");
        if (!view)
            return std::nullopt;
        if ((*view)->is_out_of_bounds())
            return interp.throw_type_error("Cannot read byteLength of a detached or out-of-bounds DataView");
        return Value::number(static_cast<double>((*view)->view_byte_length()));
    });
    define_accessor(in, prototype, "byteOffset", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<DataViewObject*> const view = this_data_view(interp, this_value, "byteOffset");
        if (!view)
            return std::nullopt;
        if ((*view)->is_out_of_bounds())
            return interp.throw_type_error("Cannot read byteOffset of a detached or out-of-bounds DataView");
        return Value::number(static_cast<double>((*view)->byte_offset()));
    });

    // getInt8 … setFloat64 (§25.3.4.5–.22): the one-byte kinds take no
    // byte-order argument, the rest default to big-endian.
    struct ViewMethod {
        char const* name;
        ElementType type;
    };
    constexpr ViewMethod view_methods[] = {
        { "Int8", ElementType::Int8 },
        { "Uint8", ElementType::Uint8 },
        { "Int16", ElementType::Int16 },
        { "Uint16", ElementType::Uint16 },
        { "Int32", ElementType::Int32 },
        { "Uint32", ElementType::Uint32 },
        { "Float16", ElementType::Float16 },
        { "Float32", ElementType::Float32 },
        { "Float64", ElementType::Float64 },
        { "BigInt64", ElementType::BigInt64 },
        { "BigUint64", ElementType::BigUint64 },
    };
    for (ViewMethod const& method : view_methods) {
        ElementType const type = method.type;
        bool const one_byte = element_size(type) == 1;
        std::string const getter = "get" + std::string(method.name);
        std::string const setter = "set" + std::string(method.name);
        define_method(in, prototype, getter, 1, [type, one_byte, getter](Interpreter& interp, Value const& this_value, Args args) {
            return get_view_value(interp, this_value, argument(args, 0), one_byte ? Value::boolean(true) : argument(args, 1), type, getter);
        });
        define_method(in, prototype, setter, 2, [type, one_byte, setter](Interpreter& interp, Value const& this_value, Args args) {
            return set_view_value(interp, this_value, argument(args, 0), argument(args, 1), one_byte ? Value::boolean(true) : argument(args, 2), type, setter);
        });
    }
    prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("DataView")), Configurable);
}

}
