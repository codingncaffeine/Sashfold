#include "js/Runtime.h"

#include "js/Object.h"
#include "js/Strings.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

// BigInt (§21.2): the function that converts, its two width-fitting
// statics, and a prototype of three methods. Not a constructor: `new
// BigInt` is a TypeError, and a wrapper object comes only from Object().

namespace sashfold::js {

namespace {

using Args = std::span<Value const>;

// thisBigIntValue (§21.2.3): the primitive, or the one inside a wrapper.
std::optional<BigInteger const*> this_bigint_value(Interpreter& interp, Value const& this_value, std::string_view method)
{
    if (this_value.is_bigint())
        return &this_value.as_bigint()->value();
    if (this_value.is_object() && this_value.as_object()->class_id() == Object::Class::BigInt)
        return &static_cast<PrimitiveObject*>(this_value.as_object())->primitive().as_bigint()->value();
    return interp.throw_type_error("BigInt.prototype." + std::string(method) + " requires that 'this' be a BigInt");
}

// NumberToBigInt (§21.2.1.1.1): only an integer converts.
std::optional<Value> number_to_bigint(Interpreter& interp, double number)
{
    std::optional<BigInteger> value = BigInteger::from_double(number);
    if (!value)
        return interp.throw_range_error(
            "The number " + number_to_utf8(number) + " cannot be converted to a BigInt because it is not an integer");
    return interp.bigint(std::move(*value));
}

// The `bits` of asIntN and asUintN: ToIndex, so a RangeError past 2^53 - 1.
std::optional<std::size_t> bits_argument(Interpreter& interp, Value const& value)
{
    std::optional<double> const index = interp.to_index(value);
    if (!index)
        return std::nullopt;
    return static_cast<std::size_t>(*index);
}

} // namespace

void install_bigint(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    i.bigint_prototype = in.heap().allocate<Object>(i.object_prototype, Object::Class::Object);
    NativeFunction* constructor = in.new_native(
        "BigInt", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            // §21.2.1.1: the primitive with a number hint; a Number must be
            // an integer, anything else goes through ToBigInt.
            std::optional<Value> const primitive = interp.to_primitive(argument(args, 0), PreferredType::Number);
            if (!primitive)
                return std::nullopt;
            if (primitive->is_number())
                return number_to_bigint(interp, primitive->as_number());
            std::optional<BigInt*> const value = interp.to_bigint(*primitive);
            if (!value)
                return std::nullopt;
            return Value::bigint(*value);
        });
    i.bigint_constructor = constructor;
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(i.bigint_prototype), frozen_attributes);
    i.bigint_prototype->put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("BigInt"), Value::object(constructor), builtin_attributes);

    define_method(in, *constructor, "asIntN", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<std::size_t> const bits = bits_argument(interp, argument(args, 0));
        if (!bits)
            return std::nullopt;
        std::optional<BigInt*> const value = interp.to_bigint(argument(args, 1));
        if (!value)
            return std::nullopt;
        return interp.bigint(BigInteger::as_int_n(*bits, (*value)->value()));
    });
    define_method(in, *constructor, "asUintN", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<std::size_t> const bits = bits_argument(interp, argument(args, 0));
        if (!bits)
            return std::nullopt;
        std::optional<BigInt*> const value = interp.to_bigint(argument(args, 1));
        if (!value)
            return std::nullopt;
        return interp.bigint(BigInteger::as_uint_n(*bits, (*value)->value()));
    });

    Object& prototype = *i.bigint_prototype;
    define_method(in, prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<BigInteger const*> const value = this_bigint_value(interp, this_value, "toString");
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
        return Value::string(interp.string(std::string_view((*value)->to_string(radix))));
    });
    define_method(in, prototype, "toLocaleString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<BigInteger const*> const value = this_bigint_value(interp, this_value, "toLocaleString");
        if (!value)
            return std::nullopt;
        return Value::string(interp.string(std::string_view((*value)->to_string())));
    });
    define_method(in, prototype, "valueOf", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        if (this_value.is_bigint())
            return this_value;
        std::optional<BigInteger const*> const value = this_bigint_value(interp, this_value, "valueOf");
        if (!value)
            return std::nullopt;
        return interp.bigint(**value);
    });
    prototype.put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), Value::string(in.atom("BigInt")), Configurable);
}

}
