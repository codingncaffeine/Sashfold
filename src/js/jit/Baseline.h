#pragma once

// T1, the baseline compiler (_plans/js-JIT-DESIGN.md §5 and §5.1): a code
// block's bytecode turned into machine code in one pass. The code is
// T0's frame's own: between instructions everything it has is in the
// frame (but the operand stack's top, which it keeps in a register and
// writes back before any call out), so it can be entered at any
// instruction — its entry jumps on `frame.pc` through a table of every
// instruction's address — and left at any instruction for the run loop.
// The hot instructions are written out inline (locals, constants, int32
// arithmetic and comparisons, jumps on booleans, the named-property cache,
// dense array reads), each with its fast path only; everything else, and
// every fast path's miss, is a call to the interpreter's own handler for
// the instruction (Interpreter::Impl::vm_step), so T1 computes what T0
// computes by construction.

#include "platform/ExecutableMemory.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace sashfold::js {

struct CodeBlock;
class FeedbackVector;
class Frame;

}

namespace sashfold::js::jit {

// Whether this build can run machine code it writes: x86-64 under the
// System V convention, on Linux, for now (the design's slices 4 and 5 add
// Windows, AArch64 and macOS).
#if defined(__x86_64__) && defined(__linux__)
inline constexpr bool available = true;
#else
inline constexpr bool available = false;
#endif

// What the code calls for an instruction it has no template for, or whose
// fast path missed: runs the instruction at frame.pc and answers its
// RunStatus, zero-extended.
using StepHelper = std::uint32_t (*)(void* interpreter, Frame* frame, Frame** next);

// A status no RunStatus has: the code was entered at, or jumped to, a pc
// outside the block — the engine's fault, reported as one.
inline constexpr std::uint32_t bad_pc = 0xFF;

// Where the code finds what it reads of the engine's structures: offsets
// inside them as this build lays them out, measured by the engine
// (MachineLayout in Vm.cpp), and where the interpreter keeps the two
// counters the code moves. An offset the engine could not vouch for
// leaves its template out (`arrays` false: dense reads go to the step).
struct Layout {
    std::int32_t frame_pc = 0;
    std::int32_t frame_stack_top = 0; // the operand stack's top (Value*)
    std::int32_t frame_registers = 0; // the registers' base (Value*)
    std::int32_t frame_this = 0; // a plain function's `this` (a Value)
    std::int32_t frame_function_env = 0; // the function's own environment, when made (a pointer)
    std::int32_t frame_envs_base = 0; // the environments (Environment**)
    std::int32_t frame_envs_size = 0; // how many (uint32)
    bool arguments = false; // the call's arguments, a span as this library lays it out:
    std::int32_t frame_incoming_data = 0; // Value const*
    std::int32_t frame_incoming_size = 0; // size_t
    std::int32_t cell_kind = 0; // the kind byte
    std::uint8_t kind_object = 0;
    std::int32_t object_shape = 0;
    std::int32_t object_slots = 0; // Value*
    std::int32_t object_class = 0; // the class byte
    std::uint8_t class_array = 0;
    std::int32_t shape_flags = 0; // the flags byte
    std::uint8_t shape_dictionary = 0;
    bool arrays = false;
    std::int32_t array_data = 0; // the dense storage (Value*)
    std::int32_t array_size = 0; // its length (uint32)
    bool scopes = false; // an environment's bindings, a vector as this library lays it out:
    std::int32_t env_outer = 0; // Environment*
    std::int32_t env_bindings_begin = 0; // Binding*
    std::int32_t env_bindings_end = 0; // Binding*
    std::int32_t binding_size = 0;
    std::int32_t binding_value = 0; // a Value
    std::int32_t binding_mutable = 0; // a bool
    std::int32_t binding_initialized = 0; // a bool
    std::int32_t interpreter_ic_hits = 0; // from the interpreter the code is given (uint64)
    std::int32_t* budget = nullptr; // the interrupt budget, decremented at each back-edge
};

struct Code {
    // RunStatus run(interpreter, frame, &next): from the instruction at
    // frame.pc until the frame ends, suspends, throws past its handlers or
    // switches to another frame (`next`).
    using Entry = std::uint32_t (*)(void* interpreter, Frame* frame, Frame** next);

    explicit Code(platform::ExecutableMemory memory_)
        : memory(std::move(memory_))
    {
    }

    platform::ExecutableMemory memory;
    Entry entry = nullptr;
    std::uint32_t instructions = 0;
    std::uint32_t inline_instructions = 0; // the ones with a template
    std::size_t code_bytes = 0; // the instructions' code, without the table
};

// The block's machine code; null when this build has no backend, the OS
// refuses the memory or the assembler fails. `feedback` is the block's
// vector (null when it has no sites): the code reads its records as they
// change, by address.
std::unique_ptr<Code> compile(CodeBlock const&, FeedbackVector*, Layout const&, StepHelper);

// When code blocks get machine code: never (T0 alone), at a block's first
// run (`eager`, the differential mode every suite runs in as well), or
// when it is hot (`tiered`): `threshold()` calls and loop back-edges
// counted together, a loop taken into the code at its next back-edge.
// SASHFOLD_JIT=0|eager|tiered in the environment, read once
// (SASHFOLD_JIT_THRESHOLD for the count); a test sets them to run each way.
enum class Mode : std::uint8_t { Off, Eager, Tiered };
Mode mode();
void set_mode(Mode);
std::uint32_t threshold();
void set_threshold(std::uint32_t);

}
