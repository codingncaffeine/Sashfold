#include "Test.h"

#include "net/FetchPool.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Fetches carried out off the caller's thread, several at a time: the work
// runs together, a ticket waits for its answer and gives it once, the waker
// is called for each, and a pool that goes away answers what never began.

using namespace sashfold;
using namespace sashfold::net;

namespace {

FetchResult answer(std::string const& words)
{
    FetchResponse response;
    response.status = 200;
    response.body.assign(words.begin(), words.end());
    return { std::move(response), "" };
}

std::string body_of(FetchResult const& result)
{
    return result.response ? std::string(result.response->body.begin(), result.response->body.end()) : "error: " + result.error;
}

double ms_since(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}

}

int main()
{
    // Eight pieces of work that each wait 150 ms are done in well under the
    // 1200 ms they would take one after another — and every ticket gives its
    // own answer, once.
    {
        FetchPool pool(8);
        std::atomic<int> woken { 0 };
        pool.set_on_done([&woken] { ++woken; });
        auto const started = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<FetchTicket>> tickets;
        for (int i = 0; i < 8; ++i) {
            tickets.push_back(pool.submit([i] {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                return answer("number " + std::to_string(i));
            }));
        }
        CHECK(pool.outstanding() > 0);
        for (int i = 0; i < 8; ++i)
            CHECK_EQ(body_of(tickets[static_cast<std::size_t>(i)]->take()), "number " + std::to_string(i));
        double const took = ms_since(started);
        CHECK(took >= 140);
        CHECK(took < 900);
        for (auto const& ticket : tickets)
            CHECK(ticket->done());
        // The waker is called after the ticket is answered: give the last
        // call a moment to land.
        for (int wait = 0; wait < 200 && woken.load() < 8; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(woken.load(), 8);
        for (int wait = 0; wait < 200 && pool.outstanding() != 0; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(pool.outstanding(), std::size_t { 0 });
        // Taken once.
        CHECK_EQ(body_of(tickets[0]->take()), std::string("error: the fetch's result was taken already"));
    }

    // One thread takes the work in the order it was asked for, one piece at
    // a time.
    {
        FetchPool pool(1);
        std::vector<int> order;
        std::mutex order_lock;
        std::atomic<int> running { 0 };
        std::atomic<int> most { 0 };
        std::vector<std::shared_ptr<FetchTicket>> tickets;
        for (int i = 0; i < 5; ++i) {
            tickets.push_back(pool.submit([&, i] {
                int const now = ++running;
                if (now > most.load())
                    most = now;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                {
                    std::lock_guard<std::mutex> const lock(order_lock);
                    order.push_back(i);
                }
                --running;
                return answer("done");
            }));
        }
        for (auto const& ticket : tickets)
            ticket->take();
        CHECK_EQ(most.load(), 1);
        CHECK(order == (std::vector<int> { 0, 1, 2, 3, 4 }));
    }

    // A ticket not yet answered says so, and take() waits for it.
    {
        FetchPool pool(2);
        std::atomic<bool> release { false };
        std::shared_ptr<FetchTicket> const ticket = pool.submit([&release] {
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return answer("late");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(!ticket->done());
        std::thread releaser([&release] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release = true;
        });
        CHECK_EQ(body_of(ticket->take()), std::string("late"));
        CHECK(ticket->done());
        releaser.join();
    }

    // A pool that goes away waits for what is running and answers what
    // never began: nobody is left waiting for ever.
    {
        std::shared_ptr<FetchTicket> running;
        std::shared_ptr<FetchTicket> queued;
        std::atomic<bool> began { false };
        {
            FetchPool pool(1);
            running = pool.submit([&began] {
                began = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                return answer("finished");
            });
            queued = pool.submit([] { return answer("never"); });
            while (!began.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK_EQ(body_of(running->take()), std::string("finished"));
        CHECK_EQ(body_of(queued->take()), std::string("error: the session ended before the fetch began"));
    }

    return test::report("fetch pool");
}
