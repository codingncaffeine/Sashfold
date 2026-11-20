#include "platform/Accessibility.h"

#include <atomic>
#include <mutex>

namespace sashfold::platform {

namespace {

// The bridge, read without the lock only to learn that there is none (what
// a page's turn pays on a system without one); every call into it is made
// under the lock, which set_accessibility takes as well.
std::atomic<Accessibility*> g_accessibility { nullptr };
std::atomic<std::uint64_t> g_generation { 0 };
std::mutex g_lock;

}

void set_accessibility(Accessibility* accessibility)
{
    std::lock_guard<std::mutex> const hold(g_lock);
    g_accessibility.store(accessibility, std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool accessibility_wanted()
{
    if (!g_accessibility.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> const hold(g_lock);
    Accessibility* const bridge = g_accessibility.load(std::memory_order_acquire);
    return bridge && bridge->wanted();
}

bool with_accessibility(std::function<void(Accessibility&)> const& use)
{
    if (!g_accessibility.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> const hold(g_lock);
    Accessibility* const bridge = g_accessibility.load(std::memory_order_acquire);
    if (!bridge)
        return false;
    use(*bridge);
    return true;
}

std::uint64_t accessibility_generation() { return g_generation.load(std::memory_order_acquire); }

void wake_accessibility()
{
    with_accessibility([](Accessibility& bridge) { bridge.wake(); });
}

void wake_accessibility_window()
{
    with_accessibility([](Accessibility& bridge) { bridge.wake_window(); });
}

}
