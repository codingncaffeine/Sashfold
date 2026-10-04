#pragma once

// The header every heap cell starts with (the collector is Heap.h): what a
// Value holding a cell needs to know of it — the kind byte its is_string()
// and the rest read — kept apart from the heap so that Value.h can define
// those reads wherever a Value is used, as JavaScriptCore keeps JSCell
// apart from the heap that allocates it.

#include <cstddef>
#include <cstdint>

namespace sashfold::js {

class Heap;
class Tracer;

// Which kind of cell a cell is, kept in the cell (JavaScriptCore's JSType
// byte): the four that are script values, numbered as Value::Type numbers
// them so that a cell value's type is its kind, and every other cell,
// which never is one.
enum class CellKind : std::uint8_t {
    Other = 0,
    String = 4,
    Object = 5,
    Symbol = 6,
    BigInt = 7,
};

class Cell {
public:
    Cell() = default;
    virtual ~Cell() = default;
    // Visit every cell this one keeps alive. Not recursive: the tracer
    // queues what it is shown, so a long prototype or scope chain costs
    // stack for nothing.
    virtual void trace(Tracer&) { }
    // For a cell that holds others weakly and asked its heap to be told
    // (Heap::hold_weakly): called once marking is done and before anything
    // is freed, to let go of what was not marked.
    virtual void clear_weak() { }
    // An estimate the collector's threshold is fed with; exactness is not
    // required, monotonic reasonableness is.
    virtual std::size_t size_in_bytes() const { return sizeof(*this); }

    // The heap that adopted this cell; null for a cell that belongs to no
    // heap. How an exotic object reaches its realm's atoms.
    Heap* heap() const { return m_heap; }
    bool marked() const; // reached by its heap's last collection
    // Which of the value kinds this cell is — what a Value holding it
    // answers to is_string() and the rest — or Other for a cell that is
    // never a script value (an environment, a frame, a realm).
    CellKind kind() const { return m_kind; }

protected:
    // For the four value kinds' constructors alone.
    explicit Cell(CellKind kind)
        : m_kind(kind)
    {
    }

private:
    friend class Heap;
    friend class Tracer;
    friend struct MachineLayout; // the offsets the machine code reads (Vm.cpp)
    Heap* m_heap = nullptr;
    // The mark of the collection that last reached this cell: a number the
    // heap advances per collection, so nothing need be cleared beforehand —
    // a cell not reached this time simply still carries an older one.
    std::uint32_t m_mark = 0;
    // In the padding after the mark: the cell is no larger for it.
    CellKind m_kind = CellKind::Other;
};

static_assert(sizeof(Cell) == 24, "the kind byte lives in the header's padding");

} // namespace sashfold::js
