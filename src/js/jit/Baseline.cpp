#include "js/jit/Baseline.h"

#include "js/Bytecode.h"
#include "js/jit/AssemblerX64.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sashfold::js::jit {

namespace {

// The code's registers for its whole run (callee-saved, so a call keeps
// them): the interpreter, the frame, where the next frame goes, the
// frame's pc, and the step helper.
constexpr Reg interpreter_reg = Reg::rbx;
constexpr Reg frame_reg = Reg::r12;
constexpr Reg next_reg = Reg::r13;
constexpr Reg pc_reg = Reg::r14;
constexpr Reg step_reg = Reg::r15;

}

std::unique_ptr<Code> compile(CodeBlock const& block, StepHelper step)
{
    if constexpr (!available) {
        static_cast<void>(block);
        static_cast<void>(step);
        return nullptr;
    } else {
        auto const count = static_cast<std::uint32_t>(block.code.size());
        auto const stepped = static_cast<std::int32_t>(RunStatus::Stepped);
        AssemblerX64 a;
        Label const dispatch = a.label();
        Label const exit = a.label();
        Label const outside = a.label();
        Label const table = a.label();
        std::vector<Label> at(count);
        for (Label& label : at)
            label = a.label();

        // The prologue: a frame-pointer frame, the five callee-saved
        // registers the code keeps, and the stack 16-aligned for its calls
        // (the return address and six pushes are 56 bytes, so 8 more).
        a.push(Reg::rbp);
        a.mov(Reg::rbp, Reg::rsp);
        a.push(interpreter_reg);
        a.push(frame_reg);
        a.push(next_reg);
        a.push(pc_reg);
        a.push(step_reg);
        a.sub(Reg::rsp, 8);
        a.mov(interpreter_reg, Reg::rdi);
        a.mov(frame_reg, Reg::rsi);
        a.mov(next_reg, Reg::rdx);
        a.mov(pc_reg, Reg::rcx);
        a.mov_imm64(step_reg, reinterpret_cast<std::uint64_t>(step));
        a.jmp(dispatch);

        // Each instruction: its pc stored, so that the handler runs this
        // instruction whatever the frame said, then on to the next
        // instruction when the frame goes on there, through the table when
        // it goes on elsewhere, and out with any other status.
        for (std::uint32_t i = 0; i < count; ++i) {
            a.bind(at[i]);
            a.mov32(Mem::at(pc_reg), i);
            a.mov(Reg::rdi, interpreter_reg);
            a.mov(Reg::rsi, frame_reg);
            a.mov(Reg::rdx, next_reg);
            a.call(step_reg);
            a.cmp32(Reg::rax, stepped);
            a.j(Cond::NotEqual, exit);
            if (i + 1 < count) {
                a.cmp32(Mem::at(pc_reg), static_cast<std::int32_t>(i + 1));
                a.j(Cond::NotEqual, dispatch);
            } else {
                a.jmp(dispatch);
            }
        }
        std::size_t const code_bytes = a.size();

        // frame.pc to its instruction's code, or out when it names none.
        a.bind(dispatch);
        a.mov32(Reg::rax, Mem::at(pc_reg));
        a.cmp32(Reg::rax, static_cast<std::int32_t>(count));
        a.j(Cond::AboveOrEqual, outside);
        a.lea(Reg::rcx, table);
        a.jmp(Mem::at_index(Reg::rcx, Reg::rax, 8));
        a.bind(outside);
        a.mov_imm64(Reg::rax, bad_pc);
        a.bind(exit);
        a.add(Reg::rsp, 8);
        a.pop(step_reg);
        a.pop(pc_reg);
        a.pop(next_reg);
        a.pop(frame_reg);
        a.pop(interpreter_reg);
        a.pop(Reg::rbp);
        a.ret();

        // The table: each instruction's absolute address, written once the
        // code's place is known.
        a.align(8);
        a.bind(table);
        for (std::uint32_t i = 0; i < count; ++i)
            a.emit_u64(0);
        if (!a.finish())
            return nullptr;

        std::optional<platform::ExecutableMemory> memory = platform::ExecutableMemory::allocate(a.size());
        if (!memory)
            return nullptr;
        std::byte* const base = memory->data();
        std::memcpy(base, a.bytes().data(), a.size());
        std::uint32_t const table_offset = a.offset_of(table);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto const address = reinterpret_cast<std::uint64_t>(base + a.offset_of(at[i]));
            std::memcpy(base + table_offset + 8 * static_cast<std::size_t>(i), &address, sizeof address);
        }
        if (!memory->seal())
            return nullptr;
        auto code = std::make_unique<Code>(std::move(*memory));
        code->entry = reinterpret_cast<Code::Entry>(const_cast<std::byte*>(code->memory.code()));
        code->instructions = count;
        code->code_bytes = code_bytes;
        return code;
    }
}

}

namespace sashfold::js::jit {

namespace {

std::atomic<int> chosen_mode { -1 };

}

Mode mode()
{
    int value = chosen_mode.load(std::memory_order_relaxed);
    if (value < 0) {
        char const* const asked = std::getenv("SASHFOLD_JIT");
        value = asked != nullptr && std::strcmp(asked, "eager") == 0 ? static_cast<int>(Mode::Eager) : static_cast<int>(Mode::Off);
        chosen_mode.store(value, std::memory_order_relaxed);
    }
    return static_cast<Mode>(value);
}

void set_mode(Mode chosen)
{
    chosen_mode.store(static_cast<int>(chosen), std::memory_order_relaxed);
}

}
