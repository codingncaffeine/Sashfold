#include "Test.h"

#include "net/Url.h"
#include "ui/Browser.h"
#include "ui/ShellLoader.h"
#include "ui/Theme.h"

#include <string>
#include <string_view>
#include <vector>

// The profile's pieces the window keeps between runs: the shell loader's
// cookie jar per container, and the shell's localStorage areas per
// container and origin, as the JSON they travel in.

using namespace sashfold;

namespace {

net::Url url_of(std::string_view text)
{
    std::optional<net::Url> const url = net::parse_url(text);
    if (!url)
        std::abort();
    return *url;
}

// Serves one page for every URL: a script that shows the origin's
// localStorage item `k` as the title, then sets it to the tab's word.
struct CannedLoader final : ui::Loader {
    net::FetchResult load(net::Url const& url, std::string const&, bool, std::string_view) override
    {
        net::FetchResponse response;
        response.status = 200;
        response.status_text = "OK";
        response.headers.push_back({ "Content-Type", "text/html" });
        std::string const html = "<title>none</title><script>document.title = 'k=' + (localStorage.getItem('k') || 'none');"
                                 " localStorage.setItem('k', 'set');</script>";
        response.body.assign(html.begin(), html.end());
        response.final_url = url;
        return { std::move(response), "" };
    }
};

void test_container_jars()
{
    ui::ShellLoader loader;
    net::Url const site = url_of("https://example.test/");
    loader.set_cookie(site, "a=1", "");
    loader.set_cookie(site, "a=2", "Work");
    loader.set_cookie(site, "b=3", "Work");
    CHECK_EQ(loader.cookies_for(site), std::string("a=1"));
    CHECK_EQ(loader.cookies_for(site, "Work"), std::string("a=2; b=3"));
    CHECK_EQ(loader.cookies_for(site, "Banking"), std::string(""));
    CHECK_EQ(loader.container_names().size(), 2u); // Work, and Banking made by the question
    CHECK_EQ(loader.cookies().size(), std::size_t { 1 });
    CHECK_EQ(loader.cookies("Work").size(), std::size_t { 2 });
}

void test_storage_round_trip()
{
    CannedLoader loader;
    ui::Browser browser(loader, ui::Theme {}, 800, 600);
    CHECK_EQ(browser.storage_changes(), std::uint64_t { 0 });
    CHECK(browser.restore_storage(R"({"version": 1, "areas": [
        {"container": "", "origin": "https://a.test", "items": [["k", "v"], ["other", "1"]]},
        {"container": "Work", "origin": "https://a.test", "items": [["k", "w"]]}]})"));
    CHECK(!browser.restore_storage("not json"));
    CHECK(!browser.restore_storage("{\"areas\": 3}"));
    // The default container's page sees v; the Work container's sees w;
    // a container with nothing stored sees none. Each page then writes.
    browser.open(url_of("https://a.test/"));
    browser.tick();
    CHECK_EQ(browser.page_title(), "k=v");
    browser.new_tab_in("Work");
    CHECK_EQ(browser.active_container(), "Work");
    browser.open(url_of("https://a.test/"));
    browser.tick();
    CHECK_EQ(browser.page_title(), "k=w");
    browser.new_tab_in("Banking");
    browser.open(url_of("https://a.test/"));
    browser.tick();
    CHECK_EQ(browser.page_title(), "k=none");
    CHECK_EQ(browser.storage_changes(), std::uint64_t { 3 });
    std::string const json = browser.storage_json();
    CHECK(json.find("\"container\": \"\", \"origin\": \"https://a.test\"") != std::string::npos);
    CHECK(json.find("[\"k\", \"set\"]") != std::string::npos);
    CHECK(json.find("[\"other\", \"1\"]") != std::string::npos);
    CHECK(json.find("\"container\": \"Banking\"") != std::string::npos);
    // The JSON reads back into a fresh shell as the same areas.
    ui::Browser again(loader, ui::Theme {}, 800, 600);
    CHECK(again.restore_storage(json));
    CHECK_EQ(again.storage_json(), json);
    again.new_tab_in("Banking");
    again.open(url_of("https://a.test/"));
    again.tick();
    CHECK_EQ(again.page_title(), "k=set");
}

} // namespace

int main()
{
    test_container_jars();
    test_storage_round_trip();
    return sashfold::test::report("profile");
}
