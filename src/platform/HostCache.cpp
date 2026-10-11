#include "platform/HostCache.h"

#include <utility>

namespace sashfold::platform {

std::optional<std::vector<ResolvedAddress>> HostCache::find(std::string const& host, Clock::time_point now)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    auto const found = m_entries.find(host);
    if (found == m_entries.end())
        return std::nullopt;
    if (now - found->second.kept >= hold || now < found->second.kept) {
        m_entries.erase(found);
        return std::nullopt;
    }
    return found->second.addresses;
}

void HostCache::keep(std::string const& host, std::vector<ResolvedAddress> addresses, Clock::time_point now)
{
    if (addresses.empty())
        return;
    std::lock_guard<std::mutex> const lock(m_mutex);
    if (m_entries.size() >= most_hosts && !m_entries.contains(host)) {
        // Full: what has outlived its hold goes, and failing that the oldest.
        std::erase_if(m_entries, [&](auto const& entry) { return now - entry.second.kept >= hold; });
        if (m_entries.size() >= most_hosts) {
            auto oldest = m_entries.begin();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->second.kept < oldest->second.kept)
                    oldest = it;
            }
            m_entries.erase(oldest);
        }
    }
    m_entries[host] = Entry { std::move(addresses), now };
}

void HostCache::forget(std::string const& host)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    m_entries.erase(host);
}

void HostCache::clear()
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    m_entries.clear();
}

std::size_t HostCache::lookups() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_lookups;
}

void HostCache::count_lookup()
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    ++m_lookups;
}

HostCache& host_cache()
{
    static HostCache cache;
    return cache;
}

}
