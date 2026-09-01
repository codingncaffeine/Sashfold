#pragma once

// The shell's Loader: navigations go through the fetch choke point with the
// session's cookie jar and cache, and the file: scheme serves local pages —
// a shell concern, so the engine's fetch stays network-only.

#include "net/Cache.h"
#include "net/Cookies.h"
#include "ui/Browser.h"

namespace sashfold::ui {

class ShellLoader final : public Loader {
public:
    net::FetchResult load(net::Url const& url, std::string const& referrer,
        bool bypass_cache) override;

    net::MemoryCache& cache() { return m_cache; }
    net::CookieJar& cookies() { return m_cookies; }

private:
    net::CookieJar m_cookies;
    net::MemoryCache m_cache;
};

}
