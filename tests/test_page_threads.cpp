#include "Test.h"

#include "net/Url.h"
#include "ui/Browser.h"
#include "ui/Theme.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

// Each tab's page in an engine on a thread of its own: what the window's
// thread calls — the shell that keeps the chrome — is never held by a page,
// two pages run at the same time, and what the reader does to a page still
// reaches it.

using namespace sashfold;

namespace {

using steady = std::chrono::steady_clock;

double ms_since(steady::time_point from)
{
    return std::chrono::duration<double, std::milli>(steady::now() - from).count();
}

net::Url url_of(std::string_view text)
{
    std::optional<net::Url> const url = net::parse_url(text);
    if (!url)
        std::abort();
    return *url;
}

// How long a busy page's script holds its thread.
constexpr int busy_ms = 800;

// A page by its path: /busy spins for busy_ms in a timer and then says
// done; /click says clicked when it is clicked; /green is a green page.
struct Pages final : ui::Loader {
    net::FetchResult load(net::Url const& url, std::string const&, bool, std::string_view) override
    {
        std::string html;
        std::string const path = url.serialize_path();
        if (path.find("busy") != std::string::npos) {
            // (A moment after it is shown, so that it is seen waiting first.)
            html = "<title>busy</title><script>setTimeout(function () { var until = Date.now() + " + std::to_string(busy_ms)
                + "; while (Date.now() < until) { } document.title = 'done'; }, 150);</script>";
        } else if (path.find("click") != std::string::npos) {
            html = "<title>waiting</title><body style='margin:0'>"
                   "<div style='width:400px;height:300px' onclick=\"document.title = 'clicked'\">press</div>";
        } else {
            html = "<title>green</title><body style='margin:0;background:rgb(0,128,0)'><p>green</p>";
        }
        net::FetchResponse response;
        response.status = 200;
        response.status_text = "OK";
        response.headers.push_back({ "Content-Type", "text/html" });
        response.body.assign(html.begin(), html.end());
        response.final_url = url;
        return { std::move(response), "" };
    }
};

// What the window's loop is, for a test: the shell hears its pages and
// takes its steps until something is so, woken by the pages' threads.
struct Loop {
    std::mutex mutex;
    std::condition_variable woken;
    bool wake = false;

    std::function<void()> waker()
    {
        return [this] {
            {
                std::lock_guard<std::mutex> const lock(mutex);
                wake = true;
            }
            woken.notify_one();
        };
    }

    // True when `so` became true within the time given.
    bool until(ui::Browser& browser, std::function<bool()> const& so, int within_ms = 10000)
    {
        steady::time_point const from = steady::now();
        for (;;) {
            if (browser.load_ready())
                browser.tick();
            browser.run_scripts();
            if (so())
                return true;
            if (ms_since(from) > within_ms)
                return false;
            std::unique_lock<std::mutex> lock(mutex);
            woken.wait_for(lock, std::chrono::milliseconds(5), [this] { return wake; });
            wake = false;
        }
    }
};

platform::KeyEvent chord(char32_t letter)
{
    platform::KeyEvent key;
    key.key = platform::Key::Letter;
    key.letter = letter;
    key.ctrl = true;
    return key;
}

// The control: a shell that is whole is held by the page for as long as its
// script runs — the check below can see a shell that waits.
void test_a_whole_shell_is_held_by_a_busy_page()
{
    Pages pages;
    ui::Browser browser(pages, ui::Theme {}, 800, 600);
    browser.open(url_of("https://busy.test/busy"));
    while (browser.has_pending_load())
        browser.tick();
    CHECK_EQ(browser.page_title(), "busy");
    steady::time_point const from = steady::now();
    while (browser.page_title() != "done" && ms_since(from) < 10000)
        browser.run_scripts();
    CHECK(ms_since(from) >= busy_ms);
}

void test_the_shell_is_not_held_by_a_busy_page()
{
    Pages pages;
    Loop loop;
    ui::Browser browser(pages, ui::Theme {}, 800, 600);
    browser.set_page_threads(loop.waker());
    browser.set_pages_in_engines(true);
    CHECK(browser.page_threads());
    browser.open(url_of("https://busy.test/busy"));
    CHECK(loop.until(browser, [&] { return browser.page_title() == "busy"; }));
    // The page's script now holds the page's thread. Everything the window's
    // loop does in a turn, and what a reader does to the chrome, is done at
    // once all the same: each call is timed, and the longest is a small part
    // of the time the page is busy for.
    steady::time_point const from = steady::now();
    double longest = 0;
    int turns = 0;
    while (browser.page_title() != "done" && ms_since(from) < 10000) {
        steady::time_point const turn = steady::now();
        browser.mouse_move(40 + turns % 50, 20); // along the tab strip
        browser.key_down(chord(U'L')); // the address bar takes the focus
        browser.text_input(U'a');
        if (browser.load_ready())
            browser.tick();
        browser.run_scripts();
        static_cast<void>(browser.cursor());
        static_cast<void>(browser.text_input_area());
        static_cast<void>(browser.window_title());
        static_cast<void>(browser.session_json());
        static_cast<void>(browser.frame());
        longest = std::max(longest, ms_since(turn));
        ++turns;
        std::unique_lock<std::mutex> lock(loop.mutex);
        loop.woken.wait_for(lock, std::chrono::milliseconds(2), [&loop] { return loop.wake; });
        loop.wake = false;
    }
    double const took = ms_since(from);
    CHECK_EQ(browser.page_title(), "done");
    CHECK(turns > 20); // the loop kept turning while the page was busy
    CHECK(longest < busy_ms / 2.0); // and no turn of it waited for the page
    CHECK(took < 10000);
    CHECK(browser.address_focused());
}

void test_two_pages_run_at_the_same_time()
{
    Pages pages;
    Loop loop;
    ui::Browser browser(pages, ui::Theme {}, 800, 600);
    browser.set_page_threads(loop.waker());
    browser.set_pages_in_engines(true);
    browser.open(url_of("https://one.test/green"));
    browser.new_tab();
    browser.open(url_of("https://two.test/green"));
    CHECK(loop.until(browser, [&] { return browser.tab_title(0) == "green" && browser.tab_title(1) == "green"; }));
    // Both pages busy from the same moment: done in the time one takes, not two.
    steady::time_point const from = steady::now();
    browser.select_tab(0);
    browser.open(url_of("https://one.test/busy"));
    browser.select_tab(1);
    browser.open(url_of("https://two.test/busy"));
    CHECK(loop.until(browser, [&] { return browser.tab_title(0) == "done" && browser.tab_title(1) == "done"; }));
    double const took = ms_since(from);
    CHECK(took >= busy_ms);
    CHECK(took < 2 * busy_ms);
    // (And the tab that is not shown said what its page is called now.)
    CHECK_EQ(browser.active_tab(), std::size_t { 1 });
}

void test_what_the_reader_does_reaches_the_page()
{
    Pages pages;
    Loop loop;
    ui::Browser browser(pages, ui::Theme {}, 800, 600);
    browser.set_page_threads(loop.waker());
    browser.set_pages_in_engines(true);
    browser.open(url_of("https://click.test/click"));
    CHECK(loop.until(browser, [&] { return browser.page_title() == "waiting"; }));
    Rect const page = browser.chrome_layout().content;
    CHECK(page.height > 300);
    browser.mouse_move(page.x + 50, page.y + 50);
    browser.mouse_down(page.x + 50, page.y + 50, 1);
    browser.mouse_up(page.x + 50, page.y + 50, 1);
    CHECK(loop.until(browser, [&] { return browser.page_title() == "clicked"; }));
    // The page's picture reaches the window's frame: a green page is green
    // where the page is drawn.
    browser.open(url_of("https://green.test/green"));
    CHECK(loop.until(browser, [&] {
        if (browser.page_title() != "green")
            return false;
        Bitmap const& frame = browser.frame();
        Color const at = frame.pixel(page.x + page.width / 2, page.y + page.height - 10);
        return at.r == 0 && at.g == 128 && at.b == 0;
    }));
    // A tab that is closed and asked back comes back where it was.
    browser.new_tab();
    CHECK_EQ(browser.tab_count(), std::size_t { 2 });
    browser.close_tab(0);
    CHECK_EQ(browser.tab_count(), std::size_t { 1 });
    browser.reopen_closed_tab();
    CHECK_EQ(browser.tab_count(), std::size_t { 2 });
    CHECK(loop.until(browser, [&] { return browser.tab_title(0) == "green"; }));
    CHECK_EQ(browser.active_tab(), std::size_t { 0 });
}

} // namespace

int main()
{
    test_a_whole_shell_is_held_by_a_busy_page();
    test_the_shell_is_not_held_by_a_busy_page();
    test_two_pages_run_at_the_same_time();
    test_what_the_reader_does_reaches_the_page();
    return sashfold::test::report("page_threads");
}
