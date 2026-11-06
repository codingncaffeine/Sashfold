#include "ui/UpdateCheck.h"

#include <charconv>

namespace sashfold::ui {

std::optional<std::array<std::uint32_t, 3>> parse_release_version(std::string_view text)
{
    if (!text.empty() && text.front() == 'v')
        text.remove_prefix(1);
    std::array<std::uint32_t, 3> numbers {};
    for (std::size_t part = 0; part < 3; ++part) {
        std::size_t const end = part < 2 ? text.find('.') : text.size();
        if (end == std::string_view::npos || end == 0 || end > 9)
            return std::nullopt;
        std::string_view const digits = text.substr(0, end);
        for (char const c : digits) {
            if (c < '0' || c > '9')
                return std::nullopt;
        }
        std::from_chars(digits.data(), digits.data() + digits.size(), numbers[part]);
        text.remove_prefix(part < 2 ? end + 1 : end);
    }
    return numbers;
}

bool is_newer_release(std::string_view candidate, std::string_view current)
{
    auto const theirs = parse_release_version(candidate);
    auto const ours = parse_release_version(current);
    return theirs && ours && *theirs > *ours;
}

std::optional<std::string> release_named_by(std::string_view location)
{
    constexpr std::string_view marker = "/releases/tag/";
    std::size_t const at = location.find(marker);
    if (at == std::string_view::npos)
        return std::nullopt;
    std::string_view tag = location.substr(at + marker.size());
    tag = tag.substr(0, tag.find_first_of("/?#"));
    auto const version = parse_release_version(tag);
    if (!version)
        return std::nullopt;
    return std::to_string((*version)[0]) + "." + std::to_string((*version)[1]) + "." + std::to_string((*version)[2]);
}

std::string release_page_address(std::string_view version)
{
    return "https://github.com/codingncaffeine/Sashfold/releases/tag/v" + std::string(version);
}

UpdateState parse_update_state(std::string_view text)
{
    UpdateState state;
    while (!text.empty()) {
        std::size_t const end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        std::size_t const space = line.find(' ');
        if (space == std::string_view::npos)
            continue;
        std::string_view const name = line.substr(0, space);
        std::string_view const value = line.substr(space + 1);
        if (name == "checked") {
            std::int64_t when = 0;
            auto const [stop, error] = std::from_chars(value.data(), value.data() + value.size(), when);
            if (error == std::errc() && stop == value.data() + value.size() && when > 0)
                state.checked = when;
        } else if (name == "latest" && parse_release_version(value)) {
            state.latest = std::string(value);
        }
    }
    return state;
}

std::string serialize_update_state(UpdateState const& state)
{
    std::string out = "checked " + std::to_string(state.checked) + "\n";
    if (!state.latest.empty())
        out += "latest " + state.latest + "\n";
    return out;
}

bool update_check_due(UpdateState const& state, std::int64_t now)
{
    // A clock set back does not put the question off for ever.
    return state.checked == 0 || now - state.checked >= update_check_interval || state.checked > now;
}

}
