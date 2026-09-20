#pragma once

// Fetches carried out off the caller's thread, several at a time. A page
// names dozens of stylesheets, scripts and pictures, and fetched one after
// another each waits on the network while the rest wait on it; asked for
// together, on threads of their own, they take about as long as the slowest
// of them. What is asked for is a piece of work that ends in a FetchResult —
// the session's cache, connection pool and cookie jars, which such work
// shares, each keep a lock of their own.
//
// The answer is a ticket: asked for when it is wanted, waited on when it is
// not there yet, and taken once.

#include "net/Http.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sashfold::net {

class FetchTicket {
public:
    // Whether the fetch has ended: take() will not wait.
    bool done() const;
    // The result, waited for if need be. Once: a second call has nothing.
    FetchResult take();

private:
    friend class FetchPool;
    mutable std::mutex m_mutex;
    std::condition_variable m_ended;
    std::optional<FetchResult> m_result;
    bool m_done = false;
};

class FetchPool {
public:
    using Work = std::function<FetchResult()>;

    // No thread is started until the first piece of work arrives.
    explicit FetchPool(std::size_t threads = 8);
    // What is running is waited for (a fetch ends with its sockets'
    // timeouts at the latest); what has not started is dropped, its ticket
    // answered with an error.
    ~FetchPool();
    FetchPool(FetchPool const&) = delete;
    FetchPool& operator=(FetchPool const&) = delete;

    std::shared_ptr<FetchTicket> submit(Work work);

    // Called on a worker's thread each time a fetch ends: what wakes a loop
    // that sleeps until there is something to do. Set before the first
    // submit().
    void set_on_done(std::function<void()> wake) { m_wake = std::move(wake); }

    // Fetches asked for and not yet ended.
    std::size_t outstanding() const;

private:
    struct Job {
        Work work;
        std::shared_ptr<FetchTicket> ticket;
    };
    void run();

    std::size_t m_thread_limit;
    mutable std::mutex m_mutex;
    std::condition_variable m_work_arrived;
    std::deque<Job> m_queue;
    std::vector<std::thread> m_threads;
    std::size_t m_idle = 0;
    std::size_t m_outstanding = 0;
    bool m_stopping = false;
    std::function<void()> m_wake;
};

}
