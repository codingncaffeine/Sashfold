#pragma once

// The shell's Loader: navigations go through the fetch choke point with the
// session's cookie jar and cache, and the file: scheme serves local pages —
// a shell concern, so the engine's fetch stays network-only.

#include "net/Cache.h"
#include "net/Connections.h"
#include "net/Cookies.h"
#include "ui/Browser.h"

namespace sashfold::ui {

class ShellLoader final : public Loader {
public:
    net::FetchResult load(net::Url const& url, std::string const& referrer,
        bool bypass_cache) override;
    net::FetchResult load_subresource(net::Url const& url, net::Url const& first_party,
        std::string const& referrer) override;
    std::string cookies_for(net::Url const& url) override;
    void set_cookie(net::Url const& url, std::string_view set_cookie_line) override;

    net::MemoryCache& cache() { return m_cache; }
    net::ConnectionPool& pool() { return m_pool; }
    net::CookieJar& cookies() { return m_cookies; }

private:
    net::CookieJar m_cookies;
    net::MemoryCache m_cache;
    net::ConnectionPool m_pool;
};

}
