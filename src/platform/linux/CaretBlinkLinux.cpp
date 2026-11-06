#include "platform/CaretBlink.h"

// The caret's blink as the Linux desktop sets it (CaretBlink.h has the
// order). The files are read at once; the portal is asked on a thread of
// its own, so a desktop with no portal never holds the window up.

#include "platform/linux/DBus.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

namespace sashfold::platform {

namespace {

// -1: blinking off. 0: not known yet (the defaults stand in).
std::atomic<int> g_half_ms { 0 };
std::atomic<int> g_timeout_ms { 10000 };
std::atomic<bool> g_started { false };

std::string config_home()
{
    if (char const* const xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return xdg;
    if (char const* const home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config";
    return {};
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// One key of one section of an INI file (kdeglobals, settings.ini).
std::optional<std::string> ini_value(std::string const& path, std::string_view section, std::string_view key)
{
    std::ifstream file(path);
    if (!file)
        return std::nullopt;
    std::string line;
    bool in_section = false;
    while (std::getline(file, line)) {
        std::string_view const text = trimmed(line);
        if (text.empty() || text.front() == '#' || text.front() == ';')
            continue;
        if (text.front() == '[') {
            in_section = text == "[" + std::string(section) + "]";
            continue;
        }
        if (!in_section)
            continue;
        std::size_t const equals = text.find('=');
        if (equals == std::string_view::npos)
            continue;
        // KDE keys may carry a locale or a flag in brackets: CursorBlinkRate[$i].
        std::string_view name = trimmed(text.substr(0, equals));
        if (std::size_t const bracket = name.find('['); bracket != std::string_view::npos)
            name = name.substr(0, bracket);
        if (name == key)
            return std::string(trimmed(text.substr(equals + 1)));
    }
    return std::nullopt;
}

std::optional<int> number(std::optional<std::string> const& text)
{
    if (!text || text->empty())
        return std::nullopt;
    char* end = nullptr;
    long const value = std::strtol(text->c_str(), &end, 10);
    if (end == text->c_str())
        return std::nullopt;
    return static_cast<int>(value);
}

std::optional<bool> truth(std::optional<std::string> const& text)
{
    if (!text)
        return std::nullopt;
    if (*text == "true" || *text == "1")
        return true;
    if (*text == "false" || *text == "0")
        return false;
    return std::nullopt;
}

void set(std::optional<int> cycle_ms, std::optional<bool> blinks, std::optional<int> timeout_s)
{
    if (blinks && !*blinks) {
        g_half_ms = -1;
        return;
    }
    int const cycle = cycle_ms.value_or(1200);
    g_half_ms = cycle <= 0 ? -1 : std::max(50, cycle / 2);
    if (timeout_s)
        g_timeout_ms = *timeout_s <= 0 ? 0 : *timeout_s * 1000;
}

bool kde_session()
{
    char const* const desktop = std::getenv("XDG_CURRENT_DESKTOP");
    return desktop && std::string_view(desktop).find("KDE") != std::string_view::npos;
}

// A KDE session: kdeglobals' CursorBlinkRate is Qt's flash time, a whole
// cycle, 0 for no blinking; Qt never rests the caret.
bool read_kde()
{
    if (!kde_session())
        return false;
    std::optional<int> const rate = number(ini_value(config_home() + "/kdeglobals", "KDE", "CursorBlinkRate"));
    set(rate.value_or(1000), std::nullopt, 0);
    return true;
}

bool read_gtk_settings()
{
    std::string const path = config_home() + "/gtk-3.0/settings.ini";
    std::optional<bool> const blinks = truth(ini_value(path, "Settings", "gtk-cursor-blink"));
    std::optional<int> const time = number(ini_value(path, "Settings", "gtk-cursor-blink-time"));
    std::optional<int> const timeout = number(ini_value(path, "Settings", "gtk-cursor-blink-timeout"));
    if (!blinks && !time && !timeout)
        return false;
    set(time, blinks, timeout.value_or(10));
    return true;
}

// org.freedesktop.portal.Settings.ReadOne (or the older Read, whose answer
// is a variant inside a variant) of a boolean or a 32-bit number.
std::optional<std::uint32_t> portal_read(dbus::Connection& connection, std::string_view key)
{
    for (char const* member : { "ReadOne", "Read" }) {
        dbus::Writer arguments;
        arguments.string("org.gnome.desktop.interface");
        arguments.string(key);
        dbus::Message call;
        call.destination = "org.freedesktop.portal.Desktop";
        call.path = "/org/freedesktop/portal/desktop";
        call.interface = "org.freedesktop.portal.Settings";
        call.member = member;
        call.signature = "ss";
        call.body = arguments.take();
        std::uint32_t const serial = connection.send(std::move(call));
        if (serial == 0)
            return std::nullopt;
        for (int waited = 0; waited < 20; ++waited) {
            std::optional<dbus::Message> const answer = connection.receive(100);
            if (connection.closed())
                return std::nullopt;
            if (!answer || answer->reply_serial != serial)
                continue;
            if (answer->type != dbus::MessageType::MethodReturn)
                break; // try the older method, or give up
            dbus::Reader reader(answer->body);
            std::optional<std::string> type = reader.signature();
            if (type && *type == "v")
                type = reader.signature();
            if (!type || (*type != "b" && *type != "i" && *type != "u") || !reader.align(4))
                return std::nullopt;
            return reader.uint32();
        }
    }
    return std::nullopt;
}

bool read_portal()
{
    std::optional<dbus::SocketAddress> const address = dbus::session_bus_address();
    if (!address)
        return false;
    std::unique_ptr<dbus::Connection> const connection = dbus::Connection::open(*address);
    if (!connection)
        return false;
    std::optional<std::uint32_t> const blinks = portal_read(*connection, "cursor-blink");
    if (!blinks)
        return false;
    std::optional<std::uint32_t> const time = portal_read(*connection, "cursor-blink-time");
    std::optional<std::uint32_t> const timeout = portal_read(*connection, "cursor-blink-timeout");
    set(time ? std::optional<int>(static_cast<int>(*time)) : std::nullopt, *blinks != 0,
        timeout ? std::optional<int>(static_cast<int>(*timeout)) : std::optional<int>(10));
    return true;
}

} // namespace

void start_caret_blink_lookup()
{
    if (g_started.exchange(true))
        return;
    if (read_kde())
        return;
    // GTK's file first, at once; the portal's answer, when it comes,
    // is the desktop's own and wins.
    read_gtk_settings();
    std::thread([] { read_portal(); }).detach();
}

std::optional<CaretBlink> caret_blink()
{
    int const half = g_half_ms.load();
    if (half < 0)
        return std::nullopt;
    return CaretBlink { half == 0 ? 600 : half, g_timeout_ms.load() };
}

}
