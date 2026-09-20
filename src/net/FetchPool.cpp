#include "net/FetchPool.h"

#include <chrono>
#include <utility>

namespace sashfold::net {

bool FetchTicket::done() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_done;
}

bool FetchTicket::wait_for(int milliseconds) const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_ended.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return m_done; });
}

FetchResult FetchTicket::take()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_ended.wait(lock, [this] { return m_done; });
    if (!m_result)
        return { std::nullopt, "the fetch's result was taken already" };
    FetchResult result = std::move(*m_result);
    m_result.reset();
    return result;
}

FetchPool::FetchPool(std::size_t threads)
    : m_thread_limit(threads == 0 ? 1 : threads)
{
}

FetchPool::~FetchPool()
{
    std::deque<Job> dropped;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_stopping = true;
        dropped.swap(m_queue);
        m_outstanding -= dropped.size();
    }
    m_work_arrived.notify_all();
    for (Job& job : dropped) {
        std::lock_guard<std::mutex> const lock(job.ticket->m_mutex);
        job.ticket->m_result = FetchResult { std::nullopt, "the session ended before the fetch began" };
        job.ticket->m_done = true;
        job.ticket->m_ended.notify_all();
    }
    for (std::thread& thread : m_threads)
        thread.join();
}

std::shared_ptr<FetchTicket> FetchPool::submit(Work work)
{
    auto ticket = std::make_shared<FetchTicket>();
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_queue.push_back(Job { std::move(work), ticket });
        ++m_outstanding;
        // A thread for it when there is more work waiting than threads
        // waiting for work, and the limit allows another. Counted against
        // the queue, not against "is anyone idle": a thread that has been
        // woken for the piece before this one still counts as waiting until
        // it has the lock again, and that piece is still in the queue.
        if (m_queue.size() > m_waiting && m_threads.size() < m_thread_limit)
            m_threads.emplace_back([this] { run(); });
    }
    m_work_arrived.notify_one();
    return ticket;
}

std::size_t FetchPool::outstanding() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_outstanding;
}

void FetchPool::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_waiting;
            m_work_arrived.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            --m_waiting;
            if (m_queue.empty())
                return; // stopping, and nothing left to take
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        FetchResult result = job.work();
        {
            std::lock_guard<std::mutex> const lock(job.ticket->m_mutex);
            job.ticket->m_result = std::move(result);
            job.ticket->m_done = true;
        }
        job.ticket->m_ended.notify_all();
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            --m_outstanding;
        }
        if (m_wake)
            m_wake();
    }
}

}
