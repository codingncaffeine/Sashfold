#pragma once

// T1, the baseline compiler (_plans/js-JIT-DESIGN.md §5 and §5.1): a code
// block's bytecode turned into machine code in one pass. The code is
// T0's frame's own: it keeps nothing in machine registers between
// instructions, so it can be entered at any instruction — its entry jumps
// on `frame.pc` through a table of every instruction's address — and left
// at any instruction for the run loop. In this first slice every
// instruction is a call to the interpreter's own handler for it
// (Interpreter::Impl::vm_step): call-threaded code, the same semantics as
// T0 by construction. The inline templates of the hot opcodes come next.

#include "platform/ExecutableMemory.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace sashfold::js {

struct CodeBlock;
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

// What the code calls for an instruction it has no template for: runs the
// instruction at frame.pc and answers its RunStatus, zero-extended.
using StepHelper = std::uint32_t (*)(void* interpreter, Frame* frame, Frame** next);

// A status no RunStatus has: the code was entered at, or jumped to, a pc
// outside the block — the engine's fault, reported as one.
inline constexpr std::uint32_t bad_pc = 0xFF;

struct Code {
    // RunStatus run(interpreter, frame, &next, &frame.pc): from the
    // instruction at frame.pc until the frame ends, suspends, throws past
    // its handlers or switches to another frame (`next`).
    using Entry = std::uint32_t (*)(void* interpreter, Frame* frame, Frame** next, std::uint32_t* pc);

    explicit Code(platform::ExecutableMemory memory_)
        : memory(std::move(memory_))
    {
    }

    platform::ExecutableMemory memory;
    Entry entry = nullptr;
    std::uint32_t instructions = 0;
    std::size_t code_bytes = 0; // the instructions' code, without the table
};

// The block's machine code; null when this build has no backend, the OS
// refuses the memory or the assembler fails.
std::unique_ptr<Code> compile(CodeBlock const&, StepHelper);

// When code blocks get machine code: never (T0 alone, the default while
// the tier is being built), or at a block's first run (`eager`, the
// differential mode every suite runs in as well). SASHFOLD_JIT=eager in
// the environment, read once; a test sets it to run both ways.
enum class Mode : std::uint8_t { Off, Eager };
Mode mode();
void set_mode(Mode);

}
