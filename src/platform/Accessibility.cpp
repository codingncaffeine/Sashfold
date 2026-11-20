#include "platform/Accessibility.h"

#include <atomic>

namespace sashfold::platform {

namespace {

std::atomic<Accessibility*> g_accessibility { nullptr };

}

void set_accessibility(Accessibility* accessibility) { g_accessibility.store(accessibility, std::memory_order_release); }

Accessibility* accessibility() { return g_accessibility.load(std::memory_order_acquire); }

}
