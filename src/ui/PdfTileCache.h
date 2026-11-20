#pragma once

// The PDF viewer's drawn tiles, kept for when the view comes back to them
// (a scroll back, a zoom out and in again): encoded pictures by a key that
// says what was drawn, at most `capacity` bytes in all, the least recently
// used let go first. Used from any thread.

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::ui {

class PdfTileCache {
public:
    using Bytes = std::shared_ptr<std::vector<std::uint8_t> const>;

    explicit PdfTileCache(std::size_t capacity)
        : m_capacity(capacity)
    {
    }

    // The tile, now the most recently used; null when it is not kept.
    Bytes find(std::string const& key)
    {
        std::lock_guard const lock(m_mutex);
        auto const it = m_index.find(key);
        if (it == m_index.end())
            return nullptr;
        m_order.splice(m_order.begin(), m_order, it->second);
        return it->second->second;
    }

    // Keeps a tile (replacing one of the same key), then lets the least
    // recently used go until the whole fits; one larger than the capacity is not kept.
    void insert(std::string const& key, Bytes bytes)
    {
        if (!bytes)
            return;
        std::lock_guard const lock(m_mutex);
        if (auto const it = m_index.find(key); it != m_index.end()) {
            m_bytes -= it->second->second->size();
            m_order.erase(it->second);
            m_index.erase(it);
        }
        if (bytes->size() > m_capacity)
            return;
        m_bytes += bytes->size();
        m_order.emplace_front(key, std::move(bytes));
        m_index[key] = m_order.begin();
        while (m_bytes > m_capacity && !m_order.empty()) {
            auto& last = m_order.back();
            m_bytes -= last.second->size();
            m_index.erase(last.first);
            m_order.pop_back();
        }
    }

    std::size_t bytes() const
    {
        std::lock_guard const lock(m_mutex);
        return m_bytes;
    }
    std::size_t count() const
    {
        std::lock_guard const lock(m_mutex);
        return m_order.size();
    }

private:
    mutable std::mutex m_mutex;
    std::size_t m_capacity;
    std::size_t m_bytes = 0;
    std::list<std::pair<std::string, Bytes>> m_order; // most recently used first
    std::unordered_map<std::string, std::list<std::pair<std::string, Bytes>>::iterator> m_index;
};

}
