#pragma once

// The WHATWG "domain to ASCII" algorithm (beStrict=false): an all-ASCII
// domain is lowercased outright (web compat — Unicode ToASCII never runs);
// anything else takes UTS #46 processing (UseSTD3ASCIIRules=false,
// CheckHyphens=false, Transitional_Processing=false, VerifyDnsLength=false,
// IgnoreInvalidPunycode=false) over the generated mapping table (IdnaData.h,
// tools/gen-unicode) and NFC. CheckBidi runs over the whole domain with the
// bidi classes of core/BidiData.h (RFC 5893 §2). CheckJoiners holds a zero
// width joiner to its virama rule exactly; the non-joiner's joining-letter
// rule awaits the joining-type data, so a non-joiner is refused only where
// it certainly fails, at the start of a label.

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
