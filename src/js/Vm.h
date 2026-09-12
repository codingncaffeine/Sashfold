#pragma once

// The frame a compiled body runs on, and the objects that keep a
// suspended one: a generator, an async function's context, a for-in
// enumerator. A Frame is a heap cell — its operand stack, registers,
// references and environments are traced — so suspending a body is
// nothing more than returning from the run loop with the frame intact.

#include "js/Bytecode.h"
#include "js/Evaluator.h"
#include "js/Heap.h"
#include "js/Object.h"
#include "js/Value.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sashfold::js {

class Frame : public Cell {
public:
    CodeBlock const* code = nullptr;
    std::uint32_t pc = 0;
    std::vector<Value> stack;
    std::vector<Value> registers;
    std::vector<Reference> refs;
    std::vector<Environment*> envs; // envs.back() is the lexical environment; envs[0] the body's own
    Environment* variable = nullptr;
    ScriptFunction* function = nullptr;
    Program const* program = nullptr;
    PrivateEnvironment* private_environment = nullptr;
    bool strict = false;

    // Suspension. `result` carries the value out (yielded, awaited,
    // returned); the resume fields carry the completion back in, and the
    // run loop pushes `resume_value` before its first instruction when
    // `resume_pending` says a suspension point is waiting for it.
    Value result = Value::empty();
    ResumeKind resume_kind = ResumeKind::Normal;
    Value resume_value;
    bool resume_pending = false;
    bool result_is_iter_result = false; // a yield* handing the inner result through as it is

    Value& top() { return stack.back(); }
    Value& peek(std::size_t below) { return stack[stack.size() - 1 - below]; }
    void push(Value const& value) { stack.push_back(value); }
    Value pop()
    {
        Value value = stack.back();
        stack.pop_back();
        return value;
    }

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override
    {
        return sizeof(*this) + (stack.size() + registers.size()) * sizeof(Value) + refs.size() * sizeof(Reference)
            + envs.size() * sizeof(Environment*);
    }
};

// A generator (§27.5): its frame, suspended between calls to next(),
// and where in its life it stands. The frame is released once it is done.
class GeneratorObject : public Object {
public:
    enum class State : std::uint8_t { SuspendedStart, SuspendedYield, Executing, Completed };

    GeneratorObject(Object* prototype, Frame* frame)
        : Object(prototype, Class::Generator)
        , m_frame(frame)
    {
    }

    Frame* frame() const { return m_frame; }
    void release_frame() { m_frame = nullptr; }
    State state() const { return m_state; }
    void set_state(State state) { m_state = state; }

    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_frame);
    }

private:
    Frame* m_frame;
    State m_state = State::SuspendedStart;
};

// The state an async function keeps between an await and its resumption
// (§27.7.5.1): the frame and the promise capability the call handed out.
// Never a script value; the reaction closures hold it in a slot.
class AsyncContextObject : public Object {
public:
    AsyncContextObject(Object* prototype, Frame* frame, Value promise, Value resolve, Value reject)
        : Object(prototype, Class::AsyncContext)
        , m_frame(frame)
        , m_promise(promise)
        , m_resolve(resolve)
        , m_reject(reject)
    {
    }

    Frame* frame() const { return m_frame; }
    void release_frame() { m_frame = nullptr; }
    Value const& promise() const { return m_promise; }
    Value const& resolve() const { return m_resolve; }
    Value const& reject() const { return m_reject; }

    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_frame);
        tracer.visit(m_promise);
        tracer.visit(m_resolve);
        tracer.visit(m_reject);
    }

private:
    Frame* m_frame;
    Value m_promise;
    Value m_resolve;
    Value m_reject;
};

// An async-from-sync iterator (§27.1.6): the sync iterator a `for await`
// or an async `yield*` was given, whose results the prototype's methods
// hand back through promises, each value awaited. Made by GetIterator
// with the async hint; never constructed by script.
class AsyncFromSyncIteratorObject : public Object {
public:
    AsyncFromSyncIteratorObject(Object* prototype, IteratorRecord record)
        : Object(prototype, Class::AsyncFromSyncIterator)
        , m_record(std::move(record))
    {
    }

    IteratorRecord& record() { return m_record; }

    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_record.iterator);
        tracer.visit(m_record.next_method);
    }

private:
    IteratorRecord m_record;
};

// A for-in loop's enumerator as a register value: EnumerateObjectProperties
// in progress. Never a script value.
class ForInIteratorObject : public Object {
public:
    explicit ForInIteratorObject(Object* object)
        : Object(nullptr)
    {
        m_enumerator.object = object;
    }

    Interpreter::Impl::Enumerator& enumerator() { return m_enumerator; }

    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_enumerator.object);
        for (PropertyKey const& key : m_enumerator.keys)
            tracer.visit(key);
        for (JsString* name : m_enumerator.visited)
            tracer.visit(name);
    }

private:
    Interpreter::Impl::Enumerator m_enumerator;
};

}
