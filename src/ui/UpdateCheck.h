#pragma once

// Word of a newer release: the first half of updates (the half that swaps
// a downloaded build for the new one is not written yet).
//
// Once a day the shell asks the project's releases page which release is
// the latest — one request, with no cookies and no referrer, whose answer
// is a redirect to that release's page, read from the Location header and
// not followed — and when that release is newer than this build the main
// menu and about:sashfold say so. Nothing is downloaded and nothing is
// replaced. A new profile is not asked about for its first day, so a first
// run reaches no server the reader did not name; --no-update-check (or
// SASHFOLD_UPDATE_CHECK=0) turns the question off.
//
// What is here is the part with no network in it: versions, the answer's
// reading, and the two lines the profile keeps (update.txt).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sashfold::ui {

inline constexpr std::string_view latest_release_address = "https://github.com/codingncaffeine/Sashfold/releases/latest";
inline constexpr std::int64_t update_check_interval = 24 * 60 * 60; // seconds

// "0.2.1" or "v0.2.1" as its three numbers; nothing for anything else (a
// pre-release's "0.3.0-rc1" is not a release to be told of).
std::optional<std::array<std::uint32_t, 3>> parse_release_version(std::string_view text);
// Is `candidate` a release later than `current`? False when either is not
// a version.
bool is_newer_release(std::string_view candidate, std::string_view current);
// The version a redirect to a release's page names
// (…/releases/tag/v0.2.1 → "0.2.1"); nothing when it names none.
std::optional<std::string> release_named_by(std::string_view location);
// The page a release is described on.
std::string release_page_address(std::string_view version);

// What the profile remembers between runs.
struct UpdateState {
    std::int64_t checked = 0; // unix seconds of the last question; 0 never
    std::string latest; // the latest release it was told of; empty none
};
UpdateState parse_update_state(std::string_view text);
std::string serialize_update_state(UpdateState const&);
// Is it time to ask again?
bool update_check_due(UpdateState const&, std::int64_t now);

}
