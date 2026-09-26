#pragma once

// A stack whose elements never move and whose memory is never given back
// while the stack lives: the interpreter's root stack and its context
// stack push and pop on every call, and a reference handed out to an
// element must hold while deeper calls push more. A deque keeps the
// references but frees a block the moment it empties and allocates it
// again on the next push, so a stack that sits at a block boundary — as
// the root stack does for the length of a call — costs two allocations a
// call. Here a block once made stays for reuse.

#include <cstddef>
#include <memory>
#include <vector>

namespace sashfold::js {

template<typename T, std::size_t BlockSize = 256>
class ChunkedStack {
public:
    std::size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }

    T& push_back(T const& value)
    {
        std::size_t const block = m_size / BlockSize;
        if (block == m_blocks.size())
            m_blocks.push_back(std::make_unique<T[]>(BlockSize));
        T& slot = m_blocks[block][m_size % BlockSize];
        slot = value;
        ++m_size;
        return slot;
    }
    void pop_back() { --m_size; }
    T& back() { return (*this)[m_size - 1]; }
    T const& back() const { return (*this)[m_size - 1]; }
    // Shrinks to `count` elements; never grows.
    void resize(std::size_t count)
    {
        if (count < m_size)
            m_size = count;
    }
    T& operator[](std::size_t index) { return m_blocks[index / BlockSize][index % BlockSize]; }
    T const& operator[](std::size_t index) const { return m_blocks[index / BlockSize][index % BlockSize]; }

    template<typename Visitor>
    void for_each(Visitor&& visit) const
    {
        for (std::size_t i = 0; i < m_size; ++i)
            visit((*this)[i]);
    }

private:
    std::vector<std::unique_ptr<T[]>> m_blocks;
    std::size_t m_size = 0;
};

}
