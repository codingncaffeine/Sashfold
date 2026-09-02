#pragma once

// The WHATWG "domain to ASCII" algorithm (beStrict=false): an all-ASCII
// domain is lowercased outright (web compat — Unicode ToASCII never runs);
// anything else takes UTS #46 processing (UseSTD3ASCIIRules=false,
// CheckHyphens=false, Transitional_Processing=false, VerifyDnsLength=false,
// IgnoreInvalidPunycode=false) over the generated mapping table (IdnaData.h,
// tools/gen-unicode) and NFC. Honest gaps: CheckBidi and CheckJoiners await
// their data tables (they join with complex-script support; no vendored
// WPT case exercises them yet).

#include <optional>
#include <string>
#include <string_view>

namespace sashfold::net {

// Maps, normalizes, validates, and Punycode-converts a domain. nullopt on
// any processing error (the URL parser treats that as host failure).
std::optional<std::string> domain_to_ascii(std::string_view domain_utf8);

// RFC 3492, one label at a time (no "xn--" prefix on either side).
std::optional<std::string> punycode_encode(std::u32string_view label);
std::optional<std::u32string> punycode_decode(std::string_view encoded);

}
