#include "Test.h"
#include "TestDecoder.h"

#include "core/Base64.h"
#include "media/VideoPipeline.h"
#include "net/Url.h"
#include "ui/Browser.h"
#include "ui/Theme.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Each tab's page in an engine on a thread of its own: what the window's
// thread calls — the shell that keeps the chrome — is never held by a page,
// two pages run at the same time, what the reader does to a page still
// reaches it, and a page's video plays on while the page is busy.

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

// How long the page that plays a video keeps its thread while it plays.
constexpr int video_busy_ms = 600;

// A page by its path: /busy spins for busy_ms in a timer and then says
// done; /click says clicked when it is clicked; /green is a green page;
// /video plays the stream it is given through a MediaSource, says playing
// when it does, and a moment later spins for video_busy_ms before it says
// done.
struct Pages final : ui::Loader {
    std::string video_stream; // base64, for /video

    net::FetchResult load(net::Url const& url, std::string const&, bool, std::string_view) override
    {
        std::string html;
        std::string const path = url.serialize_path();
        if (path.find("video") != std::string::npos) {
            html = "<title>video</title><body style='margin:0;background:rgb(0,0,160)'>"
                   "<video id=v style='display:block;width:320px;height:180px'></video><script>"
                   "var v = document.getElementById('v'), ms = new MediaSource();"
                   "ms.addEventListener('sourceopen', function () {"
                   "  var sb = ms.addSourceBuffer('video/webm; codecs=\"vp9\"');"
                   "  sb.addEventListener('updateend', function () { ms.endOfStream(); }, { once: true });"
                   "  sb.appendBuffer(Uint8Array.from(atob('"
                + video_stream
                + "'), function (c) { return c.charCodeAt(0); }));"
                  "}, { once: true });"
                  "v.src = URL.createObjectURL(ms);"
                  "v.addEventListener('playing', function () {"
                  "  document.title = 'playing';"
                  "  setTimeout(function () { var until = Date.now() + "
                + std::to_string(video_busy_ms)
                + "; while (Date.now() < until) { } document.title = 'done'; }, 120);"
                  "}, { once: true });"
                  "v.play();</script>";
        } else if (path.find("busy") != std::string::npos) {
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

// What the window showed of a video while its page played it and then
// kept its own thread: how many different pictures reached the frame
// between the page saying it plays and saying it is done, and the longest
// the frame went without a new one.
struct Shown {
    int pictures = 0;
    double longest_wait_ms = 0;
    double took_ms = 0;
    Color beside; // the page, beside the video
    bool done = false;
};

Shown watch_a_video(bool layers)
{
    Pages pages;
    {
        auto const fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / "vp9-144p.webm";
        std::ifstream in(fixture, std::ios::binary);
        std::vector<std::uint8_t> const stream { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        pages.video_stream = base64_encode(stream);
    }
    Loop loop;
    ui::Browser browser(pages, ui::Theme {}, 800, 600);
    // A decoder of the test's own: each frame shown is another grey, so
    // that one picture is seen to give way to the next.
    browser.set_video_opener([](std::string&) { return std::make_shared<media::VideoPipeline>(std::make_unique<test::TestAccelerator>(true)); });
    browser.set_video_layers(layers);
    browser.set_page_threads(loop.waker());
    browser.set_pages_in_engines(true);
    browser.open(url_of("https://video.test/video"));
    Shown shown;
    if (!loop.until(browser, [&] { return browser.page_title() == "playing"; }))
        return shown;
    Rect const page = browser.chrome_layout().content;
    steady::time_point const from = steady::now();
    steady::time_point last_at = from;
    std::optional<Color> last;
    while (ms_since(from) < 10000) {
        if (browser.load_ready())
            browser.tick();
        browser.run_scripts();
        if (browser.page_title() == "done") {
            shown.done = true;
            break;
        }
        if (browser.needs_paint()) {
            Bitmap const& frame = browser.frame();
            Color const at = frame.pixel(page.x + 160, page.y + 90); // the middle of the video
            shown.beside = frame.pixel(page.x + 500, page.y + 90);
            if (!last || !(at == *last)) {
                if (last) {
                    ++shown.pictures;
                    shown.longest_wait_ms = std::max(shown.longest_wait_ms, ms_since(last_at));
                }
                last = at;
                last_at = steady::now();
            }
        }
        std::unique_lock<std::mutex> lock(loop.mutex);
        loop.woken.wait_for(lock, std::chrono::milliseconds(2), [&loop] { return loop.wake; });
        loop.wake = false;
    }
    shown.longest_wait_ms = std::max(shown.longest_wait_ms, ms_since(last_at));
    shown.took_ms = ms_since(from);
    return shown;
}

// A video's pictures do not wait for the page's thread: while the page
// spins for six tenths of a second the window goes on showing a new picture
// every thirtieth — put on the page by the compositor, which is no thread
// of the page's nor the window's. The control is the same page with its
// video left to its own painting: there the pictures stop for as long as
// the page spins, which is what a reader saw as a stutter.
void test_a_video_plays_on_while_its_page_is_busy()
{
    Shown const apart = watch_a_video(true);
    CHECK(apart.done);
    CHECK(apart.took_ms >= video_busy_ms);
    // About twenty pictures come due in the seven tenths of a second. A
    // shared runner that wakes this loop late sees fewer of them, so the
    // bands are drawn from the control below and not from a good machine:
    // more pictures than a video its page paints may show, and no wait half
    // as long as the page was busy.
    CHECK(apart.pictures >= 10);
    CHECK(apart.longest_wait_ms < video_busy_ms / 2);
    // Beside the video the page is its own blue, and no hole shows.
    CHECK((apart.beside == Color::rgb(0, 0, 160)));

    Shown const painted = watch_a_video(false);
    std::cout << "a video while its page is busy: " << apart.pictures << " pictures, the longest wait "
              << apart.longest_wait_ms << " ms; painted by its page: " << painted.pictures << " pictures, "
              << painted.longest_wait_ms << " ms\n";
    CHECK(painted.done);
    CHECK(painted.took_ms >= video_busy_ms);
    CHECK(painted.pictures <= 8);
    CHECK(painted.longest_wait_ms >= video_busy_ms - 50);
    CHECK((painted.beside == Color::rgb(0, 0, 160)));
}

} // namespace

int main()
{
    test_a_whole_shell_is_held_by_a_busy_page();
    test_the_shell_is_not_held_by_a_busy_page();
    test_two_pages_run_at_the_same_time();
    test_what_the_reader_does_reaches_the_page();
    test_a_video_plays_on_while_its_page_is_busy();
    return sashfold::test::report("page_threads");
}
