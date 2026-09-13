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
#include "net/Filters.h"
#include "ui/Browser.h"

#include <functional>
#include <map>
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
    std::string cookies_for(net::Url const& url, std::string_view container = {}) override;
    void set_cookie(net::Url const& url, std::string_view set_cookie_line, std::string_view container = {}) override;
    std::size_t blocked_requests() const override { return m_blocked; }
    net::Blocklists const* content_lists() const override { return &m_blocklists; }

    // The lists every request is judged by. A navigation a list refuses
    // fails with an error beginning "blocked by " or, for a site to keep
    // away from, "kept off by ", then the list's name, a colon, the rule.
    void set_blocklists(net::Blocklists lists) { m_blocklists = std::move(lists); }
    net::Blocklists const& blocklists() const { return m_blocklists; }

    net::MemoryCache& cache() { return m_cache; }
    net::ConnectionPool& pool() { return m_pool; }
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

    net::CookieJar m_cookies; // the default container's
    std::map<std::string, net::CookieJar> m_container_jars; // by name; the cache and the pool are shared
    net::MemoryCache m_cache;
    net::ConnectionPool m_pool;
    net::Blocklists m_blocklists;
    std::size_t m_blocked = 0;
};

}
