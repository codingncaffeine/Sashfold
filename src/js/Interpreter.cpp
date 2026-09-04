#include "js/Interpreter.h"

// The evaluator is not written yet. What is here is the one member the
// object model already calls - recording a thrown value - so that the
// finished modules link; it is replaced whole when the evaluator lands.

namespace sashfold::js {

std::nullopt_t Interpreter::throw_value(Value value)
{
    m_exception = value;
    m_has_exception = true;
    return std::nullopt;
}

std::nullopt_t Interpreter::throw_error(ErrorType, std::string_view message)
{
    return throw_value(Value::string(m_heap->string(message)));
}

}

// Likewise placeholders until the evaluator lands: nothing calls them yet
// with a live interpreter.
namespace sashfold::js {

std::optional<Value> Interpreter::call(Value const&, Value const&, std::span<Value const>)
{
    return throw_error(ErrorType::TypeError, "the evaluator is not written yet");
}

std::optional<double> Interpreter::to_number(Value const& value)
{
    return value.is_number() ? std::optional<double>(value.as_number()) : throw_error(ErrorType::TypeError, "the evaluator is not written yet");
}

std::optional<std::uint32_t> Interpreter::to_uint32(Value const& value)
{
    return value.is_number() ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(value.as_number())) : throw_error(ErrorType::TypeError, "the evaluator is not written yet");
}

std::optional<Value> ScriptFunction::call(Interpreter& interpreter, Value const&, std::span<Value const>)
{
    return interpreter.throw_error(ErrorType::TypeError, "the evaluator is not written yet");
}

std::optional<Value> ScriptFunction::construct(Interpreter& interpreter, std::span<Value const>, Object*)
{
    return interpreter.throw_error(ErrorType::TypeError, "the evaluator is not written yet");
}

}
