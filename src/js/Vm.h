#pragma once

// The frame a compiled body runs on, and the objects that keep a
// suspended one: a generator, an async function's context, a for-in
// enumerator. A running frame lives on the interpreter's stacks — its
// registers and operand stack on the value stack, its environments on the
// environment stack, itself on the frame stack — and is no cell. A body
// that suspends is copied out into a SavedFrame cell when its driver
// returns and copied back onto the stacks when it is resumed (copy on
// suspend, as V8's and SpiderMonkey's generators are).

#include "js/Bytecode.h"
#include "js/Evaluator.h"
#include "js/Heap.h"
#include "js/Object.h"
#include "js/Value.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace sashfold::js {

// A class between its ClassScope and its ClassFinish (§15.7.14): the
// scope with the name binding, what the heritage settled, the prototype
// and the constructor, the private names, and the static elements that
// run at the end. Kept on the frame, so a body that suspends inside a
// computed key keeps its class, and dropped by a handler that unwinds past
// it.
class ClassBuilder : public Cell {
public:
    ClassNode const* node = nullptr;
    Environment* class_env = nullptr; // the scope's own, or the outer one when the scope makes none
    bool owns_class_env = true;
    PrivateEnvironment* outer_private = nullptr; // restored when the class is done
    PrivateEnvironment* private_env = nullptr; // the class's own, or the outer one
    bool saved_strict = false;
    PropertyKey name_key; // an anonymous class's binding name, if any
    bool has_name_key = false;
    Object* proto = nullptr;
    ScriptFunction* constructor = nullptr;
    struct StaticElement {
        ClassElement const* element;
        PropertyKey key;
    };
    std::vector<StaticElement> statics;

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override { return sizeof(*this) + statics.size() * sizeof(StaticElement); }
};

// A frame's operand stack: the area of the value stack above its registers,
// sized by the compiler (max_stack, and one more for the value a resumed
// body is handed), under the names of the vector the run loop was written
// against. Only the top frame runs, so a push past the compiler's count
// still lands in free stack and never in another frame; the end of the
// whole value stack is the one limit, and reaching it is the compiler's
// fault, not the page's.
class OperandStack {
public:
    void reset(Value* base, std::uint32_t capacity, Value const* limit)
    {
        m_base = base;
        m_top = base;
        m_capacity = capacity;
        m_limit = limit;
    }
    std::size_t size() const { return static_cast<std::size_t>(m_top - m_base); }
    Value* data() { return m_base; }
    Value const* data() const { return m_base; }
    Value& operator[](std::size_t i) { return m_base[i]; }
    Value const& operator[](std::size_t i) const { return m_base[i]; }
    Value& back() { return m_top[-1]; }
    Value const& back() const { return m_top[-1]; }
    void push_back(Value const& value)
    {
        if (m_top >= m_limit) [[unlikely]]
            overflow();
        *m_top++ = value;
    }
    void pop_back() { --m_top; }
    // Cut back to `count`, or grow to it with undefined.
    void resize(std::size_t count)
    {
        while (size() < count)
            push_back(Value::undefined());
        m_top = m_base + count;
    }
    void clear() { m_top = m_base; }
    // The slots the frame's extent covers: the area reserved for it, or
    // more if pushes went past it.
    std::size_t extent() const { return size() > m_capacity ? size() : m_capacity; }

private:
    [[noreturn]] static void overflow();
    Value* m_base = nullptr;
    Value* m_top = nullptr;
    Value const* m_limit = nullptr;
    std::uint32_t m_capacity = 0;
};

// A frame's registers: the first slots of its extent of the value stack.
class RegisterFile {
public:
    void reset(Value* base, std::uint32_t count)
    {
        m_base = base;
        m_count = count;
    }
    std::size_t size() const { return m_count; }
    Value* data() { return m_base; }
    Value& operator[](std::size_t i) { return m_base[i]; }
    Value const& operator[](std::size_t i) const { return m_base[i]; }

private:
    Value* m_base = nullptr;
    std::uint32_t m_count = 0;
};

// A frame's environments, innermost last: envs[0] is the body's own and
// back() the lexical one. They are its region of the environment stack,
// which grows at the stack's top, since only the frame that runs pushes and
// it is the top one. A class body that makes no scope of its own pushes the
// environment it is in again, so this is a stack, not the outer() chain.
class EnvStack {
public:
    void reset(Environment** base, Environment* const* limit)
    {
        m_base = base;
        m_size = 0;
        m_limit = limit;
    }
    std::size_t size() const { return m_size; }
    Environment** data() { return m_base; }
    Environment* const* data() const { return m_base; }
    Environment*& operator[](std::size_t i) { return m_base[i]; }
    Environment* operator[](std::size_t i) const { return m_base[i]; }
    Environment*& back() { return m_base[m_size - 1]; }
    Environment* back() const { return m_base[m_size - 1]; }
    // Whether one more fits: the run loop asks before each push and throws
    // the stack's RangeError when it does not.
    bool has_room() const { return m_base + m_size < m_limit; }
    void push_back(Environment* env) { m_base[m_size++] = env; }
    void pop_back() { --m_size; }
    void resize(std::size_t count) { m_size = static_cast<std::uint32_t>(count); } // only ever smaller
    void clear() { m_size = 0; }

private:
    Environment** m_base = nullptr;
    std::uint32_t m_size = 0;
    Environment* const* m_limit = nullptr;
};

// What a frame holds besides its storage, the same in a running frame and
// a saved one, so that saving and restoring copy it whole.
struct FrameState {
    CodeBlock const* code = nullptr;
    std::uint32_t pc = 0;
    // The call's arguments while its prologue runs (LoadArgument,
    // RestArguments, MakeArguments): the caller's own storage, rooted by
    // run_script_function for as long as the call lasts, and emptied
    // before a suspended body could outlive it.
    std::span<Value const> incoming;
    Value field_key; // a field initializer's key, for LoadFieldKey
    Environment* variable = nullptr;
    ScriptFunction* function = nullptr;
    Program const* program = nullptr;
    PrivateEnvironment* private_environment = nullptr;
    bool strict = false;
    // A body compiled with its bindings resolved keeps its call's `this`
    // (the hole in a derived constructor until super() binds it) and
    // new.target here; when its own scope materializes, that environment
    // (`function_env`) holds them instead, for the arrows inside that
    // reach them through the chain, and this frame reads them from there.
    Value this_value;
    Object* new_target = nullptr;
    Environment* function_env = nullptr;

    // Suspension. `result` carries the value out (yielded, awaited,
    // returned); the resume fields carry the completion back in, and the
    // run loop pushes `resume_value` before its first instruction when
    // `resume_pending` says a suspension point is waiting for it.
    Value result = Value::empty();
    ResumeKind resume_kind = ResumeKind::Normal;
    Value resume_value;
    bool resume_pending = false;
    bool result_is_iter_result = false; // a yield* handing the inner result through as it is

    void trace_state(Tracer&) const;
};

// A body running on the machine: its state, its registers and operand stack
// (its extent of the value stack), and its environments (its region of the
// environment stack). Frames sit on the interpreter's frame stack while they
// run (Interpreter::Impl::push_frame); the references and the classes under
// construction keep their vectors with the frame's slot of that stack, so
// their room is used again by the next call that takes the slot.
class Frame : public FrameState {
public:
    OperandStack stack;
    RegisterFile registers;
    EnvStack envs;
    std::vector<Reference> refs;
    std::vector<ClassBuilder*> builders; // the classes under construction, innermost last
    // A call the run loop made itself (Interpreter::Impl::enter_call): what
    // it replaced of the caller's, put back when the call returns or throws.
    bool inlined = false;
    RealmRecord* caller_realm = nullptr;
    RealmRecord* caller_script_realm = nullptr;

    Value& top() { return stack.back(); }
    Value& peek(std::size_t below) { return stack[stack.size() - 1 - below]; }
    void push(Value const& value) { stack.push_back(value); }
    Value pop()
    {
        Value value = stack.back();
        stack.pop_back();
        return value;
    }

    // Every cell the frame keeps: its state's, its registers, its operand
    // stack to its depth, its environments, references and classes under
    // construction.
    void trace(Tracer&) const;
};

// A suspended body's frame: its state, with its registers, operand stack (to
// its depth), environments, references and classes under construction in
// storage of its own, until it is put back on the stacks to run again. Kept
// by the generator, or by the async body's context, that suspended it.
class SavedFrame : public Cell {
public:
    FrameState state;
    std::vector<Value> registers;
    std::vector<Value> stack;
    std::vector<Environment*> envs;
    std::vector<Reference> refs;
    std::vector<ClassBuilder*> builders;

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override
    {
        return sizeof(*this) + (registers.size() + stack.size()) * sizeof(Value) + envs.size() * sizeof(Environment*)
            + refs.size() * sizeof(Reference) + builders.size() * sizeof(ClassBuilder*);
    }
};

// A generator (§27.5): its frame, suspended between calls to next(),
// and where in its life it stands. The frame is released once it is done.
class GeneratorObject : public Object {
public:
    enum class State : std::uint8_t { SuspendedStart, SuspendedYield, Executing, Completed };

    GeneratorObject(Object* prototype, SavedFrame* frame)
        : Object(prototype, Class::Generator)
        , m_frame(frame)
    {
    }

    SavedFrame* frame() const { return m_frame; }
    void release_frame() { m_frame = nullptr; }
    void set_frame(SavedFrame* frame) { m_frame = frame; }
    State state() const { return m_state; }
    void set_state(State state) { m_state = state; }

    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_frame);
    }

private:
    SavedFrame* m_frame;
    State m_state = State::SuspendedStart;
};

// The state an async function keeps between an await and its resumption
// (§27.7.5.1): the frame and the promise capability the call handed out.
// Never a script value; the reaction closures hold it in a slot.
class AsyncContextObject : public Object {
public:
    AsyncContextObject(Object* prototype, SavedFrame* frame, Value promise, Value resolve, Value reject)
        : Object(prototype, Class::AsyncContext)
        , m_frame(frame)
        , m_promise(promise)
        , m_resolve(resolve)
        , m_reject(reject)
    {
    }

    SavedFrame* frame() const { return m_frame; }
    void release_frame() { m_frame = nullptr; }
    void set_frame(SavedFrame* frame) { m_frame = frame; }
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
    SavedFrame* m_frame;
    Value m_promise;
    Value m_resolve;
    Value m_reject;
};

// An async generator (§27.6): its frame, where in its life it stands, and
// the queue of requests — next, return or throw, each with the promise it
// was answered with — that the body serves one at a time.
class AsyncGeneratorObject : public Object {
public:
    enum class State : std::uint8_t { SuspendedStart, SuspendedYield, Executing, AwaitingReturn, Completed };
    struct Request {
        ResumeKind kind;
        Value value;
        PromiseCapability capability;
    };

    AsyncGeneratorObject(Object* prototype, SavedFrame* frame)
        : Object(prototype, Class::AsyncGenerator)
        , m_frame(frame)
    {
    }

    SavedFrame* frame() const { return m_frame; }
    void release_frame() { m_frame = nullptr; }
    void set_frame(SavedFrame* frame) { m_frame = frame; }
    State state() const { return m_state; }
    void set_state(State state) { m_state = state; }
    std::deque<Request>& queue() { return m_queue; }

    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_frame);
        for (Request const& request : m_queue) {
            tracer.visit(request.value);
            tracer.visit(request.capability.promise);
            tracer.visit(request.capability.resolve);
            tracer.visit(request.capability.reject);
        }
    }

private:
    SavedFrame* m_frame;
    State m_state = State::SuspendedStart;
    std::deque<Request> m_queue;
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
