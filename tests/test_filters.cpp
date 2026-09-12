#include "Test.h"

#include "net/Filters.h"
#include "net/Url.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// Content blocking: the Adblock Plus syntax — anchors, the separator, the
// wildcard, exceptions, the type, party, domain and case options, hosts
// files and bare URL lists — matched through the token index, and lists
// composed the way EasyList and a nefarious-site list are meant to be.

using namespace sashfold;
using namespace sashfold::net;

namespace {

Url url_of(std::string_view text)
{
    std::optional<Url> const url = parse_url(text);
    if (!url)
        std::abort();
    return *url;
}

// Whether `list` blocks a request for `target` from a page at `page` (an
// empty page is a navigation), of the given kind.
bool blocked(FilterList const& list, std::string_view target, std::string_view page = "",
    ResourceKind kind = ResourceKind::Image)
{
    Url const url = url_of(target);
    std::optional<Url> const first_party = page.empty() ? std::nullopt : std::optional<Url>(url_of(page));
    FilterRequest const request { &url, first_party ? &*first_party : nullptr, page.empty() ? ResourceKind::Document : kind };
    return list.decide(request).verdict == FilterList::Verdict::Block;
}

void test_registrable_domain()
{
    CHECK_EQ(registrable_domain("example.com"), "example.com");
    CHECK_EQ(registrable_domain("a.b.example.com"), "example.com");
    CHECK_EQ(registrable_domain("WWW.Example.COM"), "example.com");
    CHECK_EQ(registrable_domain("shop.example.co.uk"), "example.co.uk");
    CHECK_EQ(registrable_domain("example.co.uk"), "example.co.uk");
    CHECK_EQ(registrable_domain("a.gov.au"), "a.gov.au");
    CHECK_EQ(registrable_domain("localhost"), "localhost");
    CHECK_EQ(registrable_domain("10.0.0.1"), "10.0.0.1");
    CHECK_EQ(registrable_domain("[::1]"), "[::1]");
}

void test_anchors_and_wildcards()
{
    FilterList const list = FilterList::parse("[Adblock Plus 2.0]\n! comment\n||ads.example^\n|http://start.example/a\n"
                                              "/banner*.gif\n.png|\n||path.example/dir^\n",
        "anchors");
    CHECK_EQ(list.counts().rules, std::size_t { 5 });
    CHECK_EQ(list.counts().exceptions, std::size_t { 0 });
    // The domain anchor: the host, or a subdomain, never the path or a longer host.
    CHECK(blocked(list, "http://ads.example/x.js", "http://site.example/"));
    CHECK(blocked(list, "https://sub.ads.example/", "http://site.example/"));
    CHECK(blocked(list, "http://ads.example:8080/x", "http://site.example/"));
    CHECK(!blocked(list, "http://notads.example/", "http://site.example/"));
    CHECK(!blocked(list, "http://site.example/ads.example/", "http://site.example/"));
    // The start anchor.
    CHECK(blocked(list, "http://start.example/abc", "http://site.example/"));
    CHECK(!blocked(list, "https://start.example/abc", "http://site.example/"));
    // The wildcard, anywhere in the URL.
    CHECK(blocked(list, "http://site.example/img/banner_top.gif", "http://site.example/"));
    CHECK(!blocked(list, "http://site.example/img/banner_top.jpg", "http://site.example/"));
    // The end anchor.
    CHECK(blocked(list, "http://site.example/img/a.png", "http://site.example/"));
    CHECK(!blocked(list, "http://site.example/img/a.png?x", "http://site.example/"));
    // The separator: a separator character, or the end.
    CHECK(blocked(list, "http://path.example/dir", "http://site.example/"));
    CHECK(blocked(list, "http://path.example/dir?x=1", "http://site.example/"));
    CHECK(blocked(list, "http://path.example/dir/x", "http://site.example/"));
    CHECK(!blocked(list, "http://path.example/directory", "http://site.example/"));
    // Case does not matter unless asked.
    CHECK(blocked(list, "http://ADS.example/X.JS", "http://site.example/"));
    // A plain rule never blocks a navigation.
    CHECK(!blocked(list, "http://ads.example/"));
}

void test_options()
{
    FilterList const list = FilterList::parse(
        "||script.example^$script\n"
        "||noimage.example^$~image\n"
        "||third.example^$third-party\n"
        "||first.example^$~third-party\n"
        "||dom.example^$domain=a.example|~b.a.example\n"
        "||doc.example^$document\n"
        "||case.example/Path^$match-case\n"
        "||csp.example^$csp=default-src\n"
        "/regex.*banner/\n"
        "example.com##.ad\n",
        "options");
    CHECK_EQ(list.counts().rules, std::size_t { 7 });
    CHECK_EQ(list.counts().unsupported, std::size_t { 2 });
    CHECK_EQ(list.counts().cosmetic, std::size_t { 1 });
    // Types.
    CHECK(blocked(list, "http://script.example/a.js", "http://site.example/", ResourceKind::Script));
    CHECK(!blocked(list, "http://script.example/a.png", "http://site.example/", ResourceKind::Image));
    CHECK(!blocked(list, "http://noimage.example/a.png", "http://site.example/", ResourceKind::Image));
    CHECK(blocked(list, "http://noimage.example/a.js", "http://site.example/", ResourceKind::Script));
    // Party, by registrable domain.
    CHECK(blocked(list, "http://third.example/a", "http://site.example/"));
    CHECK(!blocked(list, "http://third.example/a", "http://www.third.example/"));
    CHECK(!blocked(list, "http://first.example/a", "http://site.example/"));
    CHECK(blocked(list, "http://first.example/a", "http://cdn.first.example/"));
    // The page's domain.
    CHECK(blocked(list, "http://dom.example/a", "http://a.example/"));
    CHECK(blocked(list, "http://dom.example/a", "http://c.a.example/"));
    CHECK(!blocked(list, "http://dom.example/a", "http://b.a.example/"));
    CHECK(!blocked(list, "http://dom.example/a", "http://z.example/"));
    // A navigation, only when the rule says document.
    CHECK(blocked(list, "http://doc.example/"));
    CHECK(!blocked(list, "http://script.example/"));
    // match-case, on the path (a host is lowercased by every URL parser).
    CHECK(blocked(list, "http://case.example/Path", "http://site.example/"));
    CHECK(!blocked(list, "http://case.example/path", "http://site.example/"));
}

void test_exceptions()
{
    FilterList const list = FilterList::parse("||ads.example^\n@@||ads.example/ok^\n||imp.example^$important\n@@||imp.example^\n", "exceptions");
    CHECK_EQ(list.counts().rules, std::size_t { 2 });
    CHECK_EQ(list.counts().exceptions, std::size_t { 2 });
    CHECK(blocked(list, "http://ads.example/x", "http://site.example/"));
    CHECK(!blocked(list, "http://ads.example/ok/x", "http://site.example/"));
    Url const url = url_of("http://ads.example/ok/x");
    Url const page = url_of("http://site.example/");
    FilterList::Decision const decision = list.decide(FilterRequest { &url, &page, ResourceKind::Image });
    CHECK(decision.verdict == FilterList::Verdict::Allow);
    CHECK_EQ(std::string(decision.rule), "@@||ads.example/ok^");
    // important beats an exception.
    CHECK(blocked(list, "http://imp.example/x", "http://site.example/"));
}

void test_hosts_and_bare_lists()
{
    FilterList const filters = FilterList::parse("0.0.0.0 hosts.example # a comment\n127.0.0.1 localhost\nbare.example\nhttp://exact.example/only\n", "hosts");
    CHECK_EQ(filters.counts().rules, std::size_t { 3 });
    CHECK(blocked(filters, "http://hosts.example/a", "http://site.example/"));
    CHECK(blocked(filters, "http://www.bare.example/a", "http://site.example/"));
    CHECK(blocked(filters, "http://exact.example/only?x", "http://site.example/"));
    CHECK(!blocked(filters, "http://exact.example/other", "http://site.example/"));
    CHECK(!blocked(filters, "http://localhost/", "http://site.example/"));
    // Not a nefarious list: navigations pass.
    CHECK(!blocked(filters, "http://hosts.example/"));
    // A nefarious list: the same lines keep the shell off the sites as pages too.
    FilterList const nefarious = FilterList::parse("phish.example\n0.0.0.0 malware.example\nhttps://evil.example/login\n||abp.example^\n", "nefarious", true);
    CHECK(nefarious.everything());
    CHECK(blocked(nefarious, "http://phish.example/login"));
    CHECK(blocked(nefarious, "http://malware.example/"));
    CHECK(blocked(nefarious, "https://evil.example/login?next=1"));
    CHECK(!blocked(nefarious, "https://evil.example/"));
    CHECK(blocked(nefarious, "http://abp.example/"));
    CHECK(blocked(nefarious, "http://phish.example/x.png", "http://site.example/"));
}

void test_tokens()
{
    // Rules whose pattern has no whole token still match, through the untokened bucket.
    FilterList const list = FilterList::parse("*ad*\n^banner^\n||x.example^\n", "tokens");
    CHECK(blocked(list, "http://site.example/thread/1", "http://site.example/"));
    CHECK(blocked(list, "http://site.example/a/banner/1", "http://site.example/"));
    CHECK(!blocked(list, "http://site.example/a/banners/1", "http://site.example/"));
    // A token bounded by a star on one side is not relied on: `ad*` must not
    // require "ad" as a whole token of the URL.
    FilterList const star = FilterList::parse("/ad*.js\n", "star");
    CHECK(blocked(star, "http://site.example/adserver.js", "http://site.example/"));
    CHECK(blocked(star, "http://site.example/ad.js", "http://site.example/"));
    CHECK(!blocked(star, "http://site.example/bad.js", "http://site.example/"));
}

void test_blocklists()
{
    Blocklists lists;
    lists.add(FilterList::parse("||ads.example^\n||imp.example^$important\n", "a"));
    lists.add(FilterList::parse("@@||ads.example/ok^\n@@||imp.example^\n", "b"));
    lists.add(FilterList::parse("phish.example\n@@||phish.example^\n", "n", true));
    CHECK_EQ(lists.list_count(), std::size_t { 3 });
    Url const page = url_of("http://site.example/");
    auto const ask = [&](std::string_view target, ResourceKind kind, bool navigation = false) {
        Url const url = url_of(target);
        return lists.blocks(FilterRequest { &url, navigation ? nullptr : &page, kind });
    };
    // A block in one list, lifted by an exception in another.
    CHECK(ask("http://ads.example/x", ResourceKind::Image).has_value());
    CHECK(!ask("http://ads.example/ok/x", ResourceKind::Image).has_value());
    if (auto const block = ask("http://ads.example/x", ResourceKind::Image)) {
        CHECK_EQ(block->rule, "||ads.example^");
        CHECK_EQ(block->list, "a");
        CHECK(!block->nefarious);
    }
    // important is not lifted.
    CHECK(ask("http://imp.example/x", ResourceKind::Script).has_value());
    // Nothing lifts a nefarious block, not even its own exception.
    if (auto const block = ask("http://phish.example/", ResourceKind::Document, true)) {
        CHECK(block->nefarious);
        CHECK_EQ(block->list, "n");
    } else {
        CHECK(false);
    }
    CHECK(ask("http://phish.example/pixel.png", ResourceKind::Image).has_value());
    // A request nothing names.
    CHECK(!ask("http://site.example/main.css", ResourceKind::Stylesheet).has_value());
    CHECK_EQ(lists.rule_count(), std::size_t { 5 }); // the nefarious list's exception was dropped
}

// The cosmetic rules: hides for every site and for named sites, negated
// domains, exceptions that lift a hide from any list, and the forms this
// engine leaves aside, counted.
void test_cosmetic()
{
    FilterList const a = FilterList::parse(
        "##.sponsored\n"
        "example.com##.ad\n"
        "example.com,news.example##.promo\n"
        "~example.com##.offsite\n"
        "sub.example.com,~deep.sub.example.com##.inner\n"
        "##.lifted\n"
        "example.com#@#.lifted\n"
        "example.com##div:has-text(x)\n"
        "example.com#?#.procedural\n"
        "example.com##+js(nowebrtc)\n"
        "example.com#$#body { color: red }\n",
        "a");
    CHECK_EQ(a.counts().cosmetic, std::size_t { 7 });
    CHECK_EQ(a.counts().unsupported, std::size_t { 4 });
    CHECK_EQ(a.cosmetic_rules().size(), std::size_t { 7 });
    std::vector<std::string> hide;
    std::vector<std::string> lift;
    a.cosmetic_for("www.example.com", hide, lift);
    CHECK_EQ(hide.size(), std::size_t { 4 }); // sponsored, ad, promo, lifted
    CHECK_EQ(lift.size(), std::size_t { 1 });
    CHECK(std::find(hide.begin(), hide.end(), ".offsite") == hide.end());
    hide.clear();
    lift.clear();
    a.cosmetic_for("other.test", hide, lift);
    CHECK_EQ(hide.size(), std::size_t { 3 }); // sponsored, offsite, lifted
    CHECK(lift.empty());
    hide.clear();
    a.cosmetic_for("deep.sub.example.com", hide, lift);
    CHECK(std::find(hide.begin(), hide.end(), ".inner") == hide.end());
    hide.clear();
    a.cosmetic_for("sub.example.com", hide, lift);
    CHECK(std::find(hide.begin(), hide.end(), ".inner") != hide.end());

    Blocklists lists;
    lists.add(a);
    lists.add(FilterList::parse("##.sponsored\n#@#.ad\nexample.com##.second\n", "b"));
    CHECK_EQ(lists.cosmetic_count(), std::size_t { 10 });
    std::vector<std::string> const on_example = lists.hidden_selectors("example.com");
    // .ad is lifted by the other list everywhere, .lifted on this site; the
    // repeated .sponsored appears once.
    CHECK_EQ(on_example.size(), std::size_t { 3 });
    CHECK(std::find(on_example.begin(), on_example.end(), ".sponsored") != on_example.end());
    CHECK(std::find(on_example.begin(), on_example.end(), ".promo") != on_example.end());
    CHECK(std::find(on_example.begin(), on_example.end(), ".second") != on_example.end());
    std::vector<std::string> const elsewhere = lists.hidden_selectors("");
    CHECK_EQ(elsewhere.size(), std::size_t { 3 }); // sponsored, offsite, lifted
    // A nefarious list contributes no hiding.
    lists.add(FilterList::parse("##.x\n", "n", true));
    CHECK_EQ(lists.hidden_selectors("").size(), std::size_t { 3 });
}

} // namespace

int main()
{
    test_registrable_domain();
    test_anchors_and_wildcards();
    test_options();
    test_exceptions();
    test_hosts_and_bare_lists();
    test_tokens();
    test_blocklists();
    test_cosmetic();
    return sashfold::test::report("filters");
}
