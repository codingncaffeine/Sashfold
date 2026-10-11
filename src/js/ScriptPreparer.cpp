#include "js/ScriptPreparer.h"

#include "js/Heap.h"
#include "js/Strings.h"
#include "platform/ScriptThread.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {

std::string_view without_byte_order_mark(std::string_view utf8)
{
    return utf8.starts_with("\xEF\xBB\xBF") ? utf8.substr(3) : utf8;
}

double ms_since(std::chrono::steady_clock::time_point started)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

// The threads the parses run on, shared by every page of the process: two,
// as a page rarely has more than two large scripts arriving at once and a
// parse is all processor. Script threads, for the parser's recursion. Made
// once and never ended, so no exit waits on a parse.
class ParsePool {
public:
    static ParsePool& the()
    {
        static ParsePool* const pool = new ParsePool();
        return *pool;
    }

    void submit(std::function<void()> work)
    {
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            m_work.push_back(std::move(work));
        }
        m_wake.notify_one();
    }

private:
    ParsePool()
    {
        for (int i = 0; i < 2; ++i)
            m_threads.emplace_back([this] { run(); });
    }

    void run()
    {
        while (true) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return !m_work.empty(); });
                work = std::move(m_work.front());
                m_work.pop_front();
            }
            work();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_work;
    std::vector<platform::ScriptThread> m_threads;
};

} // namespace

struct ScriptPreparer::Shared {
    struct Entry {
        enum class State { Offered, Queued, Parsing, Done, Failed };
        std::string name;
        std::string bytes; // without a byte-order mark
        State state = State::Offered;
        std::unique_ptr<Program> program;
        std::size_t code_units = 0;
        double parse_ms = 0;
    };

    std::mutex mutex;
    std::condition_variable changed;
    Heap* heap = nullptr; // while attached
    ParseOptions options;
    bool ended = false; // detached: nothing more is parsed
    int running = 0; // parses using the heap now
    std::map<std::string, std::shared_ptr<Entry>> entries;

    // Under the lock.
    static void start(std::shared_ptr<Shared> const& self, std::shared_ptr<Entry> const& entry)
    {
        entry->state = Entry::State::Queued;
        ParsePool::the().submit([self, entry] { parse(self, entry); });
    }

    static void parse(std::shared_ptr<Shared> const& self, std::shared_ptr<Entry> const& entry)
    {
        Heap* heap = nullptr;
        ParseOptions options;
        {
            std::lock_guard<std::mutex> const lock(self->mutex);
            // Taken back by the page, or the page has gone.
            if (entry->state != Entry::State::Queued || self->heap == nullptr)
                return;
            heap = self->heap;
            options = self->options;
            entry->state = Entry::State::Parsing;
            ++self->running;
        }
        auto const started = std::chrono::steady_clock::now();
        std::u16string source = utf16_from_utf8(entry->bytes);
        std::size_t const code_units = source.size();
        options.any_thread = true;
        Parser parser(*heap, std::move(source), options);
        std::unique_ptr<Program> program = parser.parse_program(entry->name);
        double const ms = ms_since(started);
        {
            std::lock_guard<std::mutex> const lock(self->mutex);
            entry->state = program ? Entry::State::Done : Entry::State::Failed;
            entry->program = std::move(program);
            entry->code_units = code_units;
            entry->parse_ms = ms;
            --self->running;
        }
        self->changed.notify_all();
    }
};

ScriptPreparer::ScriptPreparer()
    : m_shared(std::make_shared<Shared>())
{
}

ScriptPreparer::~ScriptPreparer()
{
    detach();
}

void ScriptPreparer::offer(std::string name, std::string utf8)
{
    std::lock_guard<std::mutex> const lock(m_shared->mutex);
    if (m_shared->ended || m_shared->entries.contains(name))
        return;
    auto entry = std::make_shared<Shared::Entry>();
    entry->name = name;
    entry->bytes = without_byte_order_mark(utf8);
    m_shared->entries.emplace(std::move(name), entry);
    if (m_shared->heap)
        Shared::start(m_shared, entry);
}

bool ScriptPreparer::attach(Heap& heap, ParseOptions options)
{
    std::lock_guard<std::mutex> const lock(m_shared->mutex);
    if (m_shared->ended || m_shared->heap)
        return false;
    m_shared->heap = &heap;
    m_shared->options = std::move(options);
    for (auto const& [name, entry] : m_shared->entries) {
        if (entry->state == Shared::Entry::State::Offered)
            Shared::start(m_shared, entry);
    }
    return true;
}

void ScriptPreparer::detach()
{
    std::unique_lock<std::mutex> lock(m_shared->mutex);
    m_shared->ended = true;
    m_shared->heap = nullptr;
    m_shared->changed.wait(lock, [this] { return m_shared->running == 0; });
    m_shared->entries.clear();
}

std::optional<ScriptPreparer::Taken> ScriptPreparer::take(Heap& heap, std::string const& name, std::string_view utf8)
{
    std::shared_ptr<Shared::Entry> entry;
    double waited_ms = 0;
    {
        std::unique_lock<std::mutex> lock(m_shared->mutex);
        auto const found = m_shared->entries.find(name);
        if (found == m_shared->entries.end() || m_shared->heap != &heap)
            return std::nullopt;
        entry = found->second;
        m_shared->entries.erase(found);
        using State = Shared::Entry::State;
        // Not begun: the page parses it itself rather than wait behind
        // the pool's other work.
        if (entry->state == State::Offered || entry->state == State::Queued) {
            entry->state = State::Failed;
            return std::nullopt;
        }
        auto const started = std::chrono::steady_clock::now();
        m_shared->changed.wait(lock, [&entry] { return entry->state == State::Done || entry->state == State::Failed; });
        waited_ms = ms_since(started);
        if (entry->state != State::Done)
            return std::nullopt;
    }
    if (entry->bytes != without_byte_order_mark(utf8))
        return std::nullopt;
    return Taken { std::move(entry->program), entry->code_units, entry->parse_ms, waited_ms };
}

}
