#include "js/Runtime.h"

// The intrinsics behind generators and async functions (§27.3, §27.5,
// §27.7): %GeneratorFunction% and its prototype, %GeneratorPrototype%
// with next/return/throw, %AsyncFunction% and its prototype. None of them
// is a property of the global object; a script reaches them through the
// prototype chain of a generator or async function it wrote.

#include "js/Evaluator.h"
#include "js/Vm.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

std::optional<GeneratorObject*> this_generator(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::Generator)
        return in.throw_type_error(std::string(method) + " method called on incompatible receiver " + in.describe(this_value));
    return static_cast<GeneratorObject*>(this_value.as_object());
}

// CreateDynamicFunction (§20.2.1.1.1) for the generator and async kinds:
// the last argument is the body, the rest join as the parameter list.
std::optional<Value> dynamic_function(Interpreter& in, Args arguments, DynamicFunctionKind kind)
{
    Interpreter::Roots const roots(in);
    std::u16string parameters;
    std::u16string body;
    for (std::size_t k = 0; k < arguments.size(); ++k) {
        std::optional<JsString*> const text = in.to_string(arguments[k]);
        if (!text)
            return std::nullopt;
        in.root(Value::string(*text));
        if (k + 1 == arguments.size()) {
            body = (*text)->data();
        } else {
            if (k > 0)
                parameters += u",";
            parameters += (*text)->data();
        }
    }
    return in.create_dynamic_function(parameters, body, kind);
}

// A constructor like %GeneratorFunction%: called or constructed, it makes
// a function of its kind from source text; its [[Prototype]] is
// %Function% and its `prototype` the kind's function prototype, frozen.
NativeFunction* dynamic_function_constructor(Interpreter& in, std::string_view name, Object& function_prototype, DynamicFunctionKind kind)
{
    NativeFunction* constructor = in.new_native(
        name, 1,
        [kind](Interpreter& interp, Value const&, Args arguments) -> std::optional<Value> { return dynamic_function(interp, arguments, kind); },
        [kind](Interpreter& interp, Args arguments, Object*) -> std::optional<Value> { return dynamic_function(interp, arguments, kind); });
    constructor->set_prototype(in.intrinsics().function_constructor);
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(&function_prototype), frozen_attributes);
    function_prototype.put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), Configurable);
    return constructor;
}

} // namespace

void install_generators(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();

    // %GeneratorFunction.prototype% (§27.3.3) and %GeneratorPrototype%
    // (§27.5.1), each pointing at the other.
    i.generator_function_prototype = in.new_object(i.function_prototype);
    i.generator_function_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("GeneratorFunction")), Configurable);
    i.generator_prototype = in.new_object(i.iterator_prototype);
    i.generator_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Generator")), Configurable);
    i.generator_prototype->put(PropertyKey::atom(atoms.constructor), Value::object(i.generator_function_prototype), Configurable);
    i.generator_function_prototype->put(PropertyKey::atom(atoms.prototype), Value::object(i.generator_prototype), Configurable);

    define_method(in, *i.generator_prototype, "next", 1, [](Interpreter& interp, Value const& this_value, Args arguments) -> std::optional<Value> {
        std::optional<GeneratorObject*> const generator = this_generator(interp, this_value, "next");
        if (!generator)
            return std::nullopt;
        return interp.impl().generator_resume(**generator, ResumeKind::Normal, argument(arguments, 0));
    });
    define_method(in, *i.generator_prototype, "return", 1, [](Interpreter& interp, Value const& this_value, Args arguments) -> std::optional<Value> {
        std::optional<GeneratorObject*> const generator = this_generator(interp, this_value, "return");
        if (!generator)
            return std::nullopt;
        return interp.impl().generator_resume(**generator, ResumeKind::Return, argument(arguments, 0));
    });
    define_method(in, *i.generator_prototype, "throw", 1, [](Interpreter& interp, Value const& this_value, Args arguments) -> std::optional<Value> {
        std::optional<GeneratorObject*> const generator = this_generator(interp, this_value, "throw");
        if (!generator)
            return std::nullopt;
        return interp.impl().generator_resume(**generator, ResumeKind::Throw, argument(arguments, 0));
    });

    // %GeneratorFunction% (§27.3.1).
    i.generator_function = dynamic_function_constructor(in, "GeneratorFunction", *i.generator_function_prototype, DynamicFunctionKind::Generator);

    // %AsyncFunction.prototype% (§27.7.3) and %AsyncFunction% (§27.7.1).
    i.async_function_prototype = in.new_object(i.function_prototype);
    i.async_function_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("AsyncFunction")), Configurable);
    i.async_function = dynamic_function_constructor(in, "AsyncFunction", *i.async_function_prototype, DynamicFunctionKind::Async);

    // %AsyncGeneratorFunction.prototype% (§27.4.3), %AsyncGeneratorPrototype%
    // (§27.6.1) — off %AsyncIteratorPrototype% — and %AsyncGeneratorFunction%
    // (§27.4.1). The three methods answer a promise whatever the receiver: one
    // that is not an async generator rejects it.
    i.async_generator_function_prototype = in.new_object(i.function_prototype);
    i.async_generator_function_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("AsyncGeneratorFunction")), Configurable);
    i.async_generator_prototype = in.new_object(i.async_iterator_prototype);
    i.async_generator_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("AsyncGenerator")), Configurable);
    i.async_generator_prototype->put(PropertyKey::atom(atoms.constructor), Value::object(i.async_generator_function_prototype), Configurable);
    i.async_generator_function_prototype->put(PropertyKey::atom(atoms.prototype), Value::object(i.async_generator_prototype), Configurable);
    struct Method {
        char const* name;
        ResumeKind kind;
    };
    for (Method const method : { Method { "next", ResumeKind::Normal }, Method { "return", ResumeKind::Return }, Method { "throw", ResumeKind::Throw } }) {
        ResumeKind const kind = method.kind;
        define_method(in, *i.async_generator_prototype, method.name, 1, [kind](Interpreter& interp, Value const& this_value, Args arguments) -> std::optional<Value> {
            Interpreter::Roots const roots(interp);
            interp.root(this_value);
            std::optional<PromiseCapability> const capability = new_promise_capability(interp, Value::object(interp.intrinsics().promise_constructor));
            if (!capability)
                return std::nullopt;
            interp.root(capability->promise);
            interp.root(capability->resolve);
            interp.root(capability->reject);
            if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::AsyncGenerator) {
                interp.throw_type_error("AsyncGenerator method called on incompatible receiver " + interp.describe(this_value));
                Value const thrown = interp.take_exception();
                interp.root(thrown);
                Value const reject_arguments[1] = { thrown };
                if (!interp.call(capability->reject, Value::undefined(), reject_arguments))
                    return std::nullopt;
                return capability->promise;
            }
            return interp.impl().async_generator_enqueue(*static_cast<AsyncGeneratorObject*>(this_value.as_object()), kind, argument(arguments, 0), *capability);
        });
    }
    i.async_generator_function = dynamic_function_constructor(in, "AsyncGeneratorFunction", *i.async_generator_function_prototype, DynamicFunctionKind::AsyncGenerator);
}

}
