#pragma once

// The data: URL processor (Fetch spec §data-urls): split at the first comma,
// percent-decode the body, forgiving-base64 when the mediatype says so.
// The mediatype stays a raw string until a MIME parser earns its keep; an
// absent or unparseable one reads as text/plain;charset=US-ASCII.

#include "net/Url.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::net {

struct DataUrlPayload {
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
};

std::optional<DataUrlPayload> parse_data_url(Url const& url);

}
