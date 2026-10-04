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
#include <vector>

namespace sashfold::js {

struct CodeBlock;
class FeedbackVector;
class Frame;
class JsString;
struct PropertySite;
class Value;
struct Instruction;

}

namespace sashfold::js::jit {

// Whether this build can run machine code it writes: x86-64 on Linux (the
// System V convention) and on Windows (Win64's, with its unwind data), and
// AArch64 on Linux and macOS (AAPCS64; Apple silicon's MAP_JIT).
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__linux__) || defined(_WIN32))
inline constexpr bool available = true;
#elif defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
inline constexpr bool available = true;
#else
inline constexpr bool available = false;
#endif

// What the code calls for an instruction it has no template for, or whose
// fast path missed: runs the instruction at frame.pc and answers its
// RunStatus, zero-extended.
using StepHelper = std::uint32_t (*)(void* interpreter, Frame* frame, Frame** next);

// The calls the code makes into the engine. Besides the step, each is a
// plain function that neither throws nor collects, so the code calls it
// with nothing written back to the frame.
struct Helpers {
    StepHelper step = nullptr;
    // A global name's value as its site's answer gives it (a data property
    // of the global object, a script's let or const), or 0 — the empty
    // value — when the site has no answer that holds.
    std::uint64_t (*global)(void* interpreter, PropertySite* site, JsString* name) = nullptr;
    // A named read of an object (the value's bits) as its site's answer
    // gives it when the answer calls nothing — an own or inherited data
    // property, an absent one, an array's length — or 0 when it has none
    // that holds.
    std::uint64_t (*named)(void* interpreter, PropertySite* site, Value const* base, JsString* name) = nullptr;
    // A Call (the frame's pc already past it, its top written back): the
    // site recorded and a plain script function entered (Switched, `next`
    // the callee), or any other callee called and its result in place
    // (Stepped); Threw, or Stepped at a handler that caught the throw.
    std::uint32_t (*call)(void* interpreter, Frame* frame, Frame** next, Instruction const* call) = nullptr;
    // A Return (the top written back): Switched to the caller (`next`), or
    // Completed.
    std::uint32_t (*ret)(void* interpreter, Frame* frame, Frame** next) = nullptr;
};

// A status no RunStatus has: the code was entered at, or jumped to, a pc
// outside the block — the engine's fault, reported as one.
inline constexpr std::uint32_t bad_pc = 0xFF;

// Where the code finds what it reads of the engine's structures: offsets
// inside them as this build lays them out, measured by the engine
// (MachineLayout in Vm.cpp), and where the interpreter keeps the two
// counters the code moves. An offset the engine could not vouch for
// leaves its template out (`arrays` false: dense reads go to the step).
struct Layout {
    std::int32_t frame_code = 0; // the frame's CodeBlock
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
    bool switches = false; // a CodeBlock's machine code, a unique_ptr as this library lays it out:
    std::int32_t block_code = 0; // jit::Code*
    std::int32_t interpreter_ic_hits = 0; // from the interpreter the code is given (uint64)
    std::int32_t* budget = nullptr; // the interrupt budget, decremented at each back-edge
};

struct Code {
    // RunStatus run(interpreter, frame, &next): from the instruction at
    // frame.pc until the frame ends, suspends, throws past its handlers or
    // switches to a frame whose block has no machine code (`next`). A
    // switch to one that has some — a call the run loop makes inline, its
    // return — the code makes itself, so the frame it leaves in need not be
    // the one it was entered with: `next` names the frame it was running
    // when it leaves (the last frame a step switched to, which it took up),
    // or stays null when it never switched.
    using Entry = std::uint32_t (*)(void* interpreter, Frame* frame, Frame** next);

    // Where it lies in the interpreter's code space (which owns the memory),
    // its data after its instructions.
    std::byte const* start = nullptr;
    std::size_t size = 0;
    Entry entry = nullptr;
    std::uint32_t instructions = 0;
    std::uint32_t inline_instructions = 0; // the ones with a template
    std::size_t code_bytes = 0; // the instructions' code, without the table
    // Each instruction's address, by pc: where other code that switches to
    // a frame of this block jumps.
    void const* const* table = nullptr;
    // The block's one copy of the step: a function of its own to Win64's
    // unwinder (its frame is the return address and one allocation).
    std::byte const* step_stub = nullptr;
};

// Where machine code is written: chunks of executable memory an interpreter
// owns, each block's code after the last one's and sealed once written —
// no mapping per block, and small blocks share pages. Let go of with every
// block's code, so it must outlive the code blocks (the interpreter declares
// it before them).
class CodeSpace {
public:
    // A writable place for `bytes` of code, 16-aligned; null when the OS
    // refuses memory.
    std::byte* reserve(std::size_t bytes);
    // The code just written at the last place reserved sealed executable,
    // and on Windows its frames described: its function table, two entries
    // (the block's code, its step stub), `entry` bytes in. False when the
    // OS refuses.
    bool seal(std::byte* place, std::size_t bytes, std::size_t entry);
    std::size_t chunks() const { return m_chunks.size(); }

private:
    static constexpr std::size_t chunk_bytes = 256 * 1024;
    std::vector<platform::ExecutableMemory> m_chunks;
    std::size_t m_used = 0; // of the last chunk
};

// The block's machine code; null when this build has no backend, the OS
// refuses the memory or the assembler fails. `feedback` is the block's
// vector (null when it has no sites): the code reads its records as they
// change, by address.
std::unique_ptr<Code> compile(CodeBlock const&, FeedbackVector*, Layout const&, Helpers const&, CodeSpace&);
// The AArch64 code generator's (BaselineA64.cpp), which compile uses on
// that architecture; built and callable everywhere, so its output can be
// written and checked on any machine (it runs only on AArch64).
std::unique_ptr<Code> compile_a64(CodeBlock const&, FeedbackVector*, Layout const&, Helpers const&, CodeSpace&);

// When code blocks get machine code: never (T0 alone), at a block's first
// run (`eager`, the differential mode every suite runs in as well), or
// when it is hot (`tiered`): `threshold()` calls and loop back-edges
// counted together, a loop taken into the code at its next back-edge.
// The default is tiered; SASHFOLD_JIT=0 (or off) and =eager in the
// environment choose the others, read once (SASHFOLD_JIT_THRESHOLD for the
// count); a test sets them to run each way.
// The most instructions a block may have and be compiled: a larger one
// stays in T0 (SpiderMonkey's baseline caps a script's length the same
// way; a bundle's run-once module function is what it keeps out).
inline constexpr std::size_t max_instructions = 50000;

enum class Mode : std::uint8_t { Off, Eager, Tiered };
Mode mode();
void set_mode(Mode);
std::uint32_t threshold();
void set_threshold(std::uint32_t);

}
