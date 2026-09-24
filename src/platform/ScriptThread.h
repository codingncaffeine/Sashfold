#pragma once

// A thread with a stack a script engine can run on. A thread the standard
// library starts has the platform's default — half a megabyte on macOS, one
// on Windows, eight on Linux — and the engine measures a script's recursion
// against the stack it is on (platform::js_stack_budget_for): on a small one
// a page's recursion that V8 runs to the end is a RangeError here. Every
// thread that runs script is started through this, with script_stack_bytes
// of stack. POSIX threads, which every lane has, take a size; on Windows the
// size given to a thread is what it commits, so the reserve the executable
// was linked with (-Wl,--stack, the same figure) is left to apply.

#include "platform/Memory.h"

#include <functional>
#include <memory>
#include <utility>

#include <pthread.h>

namespace sashfold::platform {

class ScriptThread {
public:
    explicit ScriptThread(std::function<void()> body)
        : m_body(std::make_unique<std::function<void()>>(std::move(body)))
    {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
#ifndef _WIN32
        pthread_attr_setstacksize(&attributes, script_stack_bytes);
#endif
        m_started = pthread_create(&m_thread, &attributes, &ScriptThread::run, m_body.get()) == 0;
        pthread_attr_destroy(&attributes);
        // Without a thread the body never runs: run it here, late but whole,
        // rather than leave a worker that never starts and never ends.
        if (!m_started)
            (*m_body)();
    }
    ScriptThread(ScriptThread&& other) noexcept
        : m_body(std::move(other.m_body))
        , m_thread(other.m_thread)
        , m_started(std::exchange(other.m_started, false))
    {
    }
    ScriptThread& operator=(ScriptThread&& other) noexcept
    {
        m_body = std::move(other.m_body);
        m_thread = other.m_thread;
        m_started = std::exchange(other.m_started, false);
        return *this;
    }
    ScriptThread(ScriptThread const&) = delete;
    ScriptThread& operator=(ScriptThread const&) = delete;
    ~ScriptThread() = default;

    void join()
    {
        if (m_started)
            pthread_join(m_thread, nullptr);
        m_started = false;
    }

private:
    static void* run(void* body)
    {
        (*static_cast<std::function<void()>*>(body))();
        return nullptr;
    }
    std::unique_ptr<std::function<void()>> m_body; // stays put while the thread runs it
    pthread_t m_thread {};
    bool m_started = false;
};

}
