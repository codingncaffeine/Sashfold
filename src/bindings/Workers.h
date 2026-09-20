#pragma once

// Dedicated workers (HTML §10.2): a script a document starts with
// `new Worker(url)`, run in a realm of its own — its own engine, its own
// heap, its own event loop — that shares nothing with the document but the
// messages the two post each other, each serialized where it is posted and
// made again where it arrives.
//
// Where a worker runs is the host's choice. A host that keeps a WorkerThreads
// and names it in its pages' HostHooks gives every worker a thread of its
// own, so that nothing a worker computes holds the page or the window. A host
// that names none has its workers run on the page's thread, a turn at a time
// inside Realm::run_pending(), on the page's clock: a test or a headless run
// is then as deterministic with workers as without.

#include <cstddef>
#include <functional>
#include <memory>

namespace sashfold::bindings {

struct WorkerLink;

// The threads a host's workers run on. It outlives every page that names it
// and everything a worker's fetch hook touches ends after it: its destructor
// stops the workers still running and waits for their threads. A worker that
// has ended has its thread joined the next time one starts.
class WorkerThreads {
public:
    WorkerThreads();
    ~WorkerThreads();
    WorkerThreads(WorkerThreads const&) = delete;
    WorkerThreads& operator=(WorkerThreads const&) = delete;

    // Called on a worker's thread when the worker has something for its
    // document — a message, an error, a console line: the host wakes its
    // loop, which calls the page's run_pending() soon. Set it once, before
    // any page runs.
    void set_wake(std::function<void()> wake);
    // How many workers' threads have not ended yet.
    std::size_t running() const;

    // The bindings' side: runs `body` on a thread of its own, for the worker
    // `link` is of; and wakes the host.
    void start(std::shared_ptr<WorkerLink> link, std::function<void()> body);
    void wake() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
