#pragma once

// The shell's Loader: navigations go through the fetch choke point with the
// session's cookie jar and cache, and the file: scheme serves local pages —
// a shell concern, so the engine's fetch stays network-only. The session's
// blocklists are asked before any request leaves: a refused request costs
// no connection at all.

#include "net/Cache.h"
#include "net/Connections.h"
#include "net/Cookies.h"
#include "net/Csp.h"
#include "net/FetchPool.h"
#include "net/Filters.h"
#include "ui/Browser.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::ui {

class ShellLoader final : public Loader {
public:
    net::FetchResult load(net::Url const& url, std::string const& referrer,
        bool bypass_cache, std::string_view container = {}) override;
    net::FetchResult load_subresource(net::Url const& url, net::Url const& first_party,
        std::string const& referrer, net::ResourceKind kind = net::ResourceKind::Other,
        net::RequestGuard const& guard = {}, std::string_view container = {}) override;
    net::FetchResult load_resource(net::Url const& url, net::Url const& first_party,
        std::string const& referrer, net::ResourceRequest const& request,
        net::RequestGuard const& guard = {}, std::string_view container = {}) override;
    std::shared_ptr<net::FetchTicket> prefetch(net::Url const& url, net::Url const& first_party,
        std::string const& referrer, net::ResourceKind kind, std::string_view container = {}) override;
    std::shared_ptr<net::FetchTicket> load_ahead(net::Url const& url, std::string const& referrer,
        bool bypass_cache, std::string_view container = {}) override;
    std::string cookies_for(net::Url const& url, std::string_view container = {}) override;
    void set_cookie(net::Url const& url, std::string_view set_cookie_line, std::string_view container = {}) override;
    std::size_t blocked_requests() const override { return m_blocked.load(); }
    net::Blocklists const* content_lists() const override { return &m_blocklists; }

    // The lists every request is judged by. A navigation a list refuses
    // fails with an error beginning "blocked by " or, for a site to keep
    // away from, "kept off by ", then the list's name, a colon, the rule.
    void set_blocklists(net::Blocklists lists) { m_blocklists = std::move(lists); }
    net::Blocklists const& blocklists() const { return m_blocklists; }

    net::HttpCache& cache() { return m_cache; }
    net::ConnectionPool& pool() { return m_pool; }

    // What the session's fetches cost, by what they were for: the account
    // --render's report and --bench write and tools/perf-census.sh ranks.
    // A request the lists or the page's policy refused makes no entry, and
    // neither does a file: load; a fetch answered by the session cache is
    // counted, with no exchange behind it.
    struct Census {
        struct Kind {
            int fetches = 0; // through the choke point
            int cached = 0; // of them, answered by the cache with no request
            int failed = 0; // of them, without a response
            net::FetchTiming timing; // summed: the exchanges, the bytes, the milliseconds
            void add(Kind const& other);
        };
        Kind document; // navigations
        Kind subdocument; // frames, objects and embeds
        Kind stylesheet;
        Kind script;
        Kind image;
        Kind font;
        Kind xhr; // fetch() and XMLHttpRequest
        Kind other; // media and the rest
        Kind& of(net::ResourceKind kind);
        Kind const& of(net::ResourceKind kind) const;
        Kind total() const;
    };
    Census census() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_census;
    }
    // The cookie jar of a container — the default's for an empty name —
    // made on first use; and the names of the containers that have one.
    net::CookieJar& cookies(std::string_view container = {});
    std::vector<std::string> container_names() const;

private:
    // The refusal for a request — the lists', then the page's guard's —
    // or nullopt when it may go. Asked of the URL requested and of every
    // redirect hop alike.
    std::optional<std::string> refusal(net::Url const& url, net::Url const* first_party, net::ResourceKind kind,
        net::RequestGuard const& guard, bool redirected);
    // The hop callback for a fetch: upgrades the hop when the guard asks,
    // then judges it.
    std::function<std::optional<std::string>(net::Url&)> hop_refusal(net::Url const* first_party, net::ResourceKind kind,
        net::RequestGuard const& guard);
    // Enters a fetch's outcome and cost into the census, and hands it on.
    net::FetchResult noted(net::ResourceKind kind, net::FetchResult result);
    // The exchange itself, for a request already judged: what
    // load_subresource does when nothing was asked for ahead, and what a
    // prefetch does on a thread of the pool's.
    net::FetchResult fetch_subresource(net::Url const& url, net::Url const& first_party, std::string const& referrer,
        net::ResourceKind kind, net::RequestGuard const& guard, std::string const& container);
    // What was asked for ahead and not yet claimed, by kind, container and
    // address.
    struct Ahead {
        std::shared_ptr<net::FetchTicket> ticket;
        std::int64_t asked_at = 0; // unix seconds
    };
    static std::string ahead_key(net::Url const& url, net::ResourceKind kind, std::string_view container);

    // The loader's fetches run on several threads at once (net::FetchPool):
    // the cache, the pool and the jars each keep their own lock, and this one
    // keeps the census and the map the containers' jars are found in. The
    // lists are set before the first fetch and only read after it.
    mutable std::mutex m_mutex;
    Census m_census;
    net::CookieJar m_cookies; // the default container's
    std::map<std::string, net::CookieJar> m_container_jars; // by name; the cache and the pool are shared
    net::HttpCache m_cache;
    net::ConnectionPool m_pool;
    net::Blocklists m_blocklists;
    std::atomic<std::size_t> m_blocked { 0 };
    std::map<std::string, Ahead> m_ahead; // under m_mutex
    // Last, so that it goes first: its threads use everything above.
    net::FetchPool m_fetches { 8 };
};

}
