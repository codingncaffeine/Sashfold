#include "platform/Tls.h"

#include <utility>

// The one part of the TLS seam every OS shares: where the net layer leaves
// the HTTP fetch a validator may use for revocation lists.

namespace sashfold::platform {

namespace {

RevocationFetch& revocation_fetch_slot()
{
    static RevocationFetch fetch;
    return fetch;
}

}

void set_revocation_fetch(RevocationFetch fetch)
{
    revocation_fetch_slot() = std::move(fetch);
}

RevocationFetch const& revocation_fetch()
{
    return revocation_fetch_slot();
}

}
