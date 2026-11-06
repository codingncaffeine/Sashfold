// The desktop portal's file chooser (xdg-desktop-portal, FileChooser.OpenFile):
// a request object is made, its Response signal is heard, and the uris it
// carries are the chosen files. KDE, GNOME and the wlroots desktops answer it
// with their own dialogs.
#include "platform/FileChooser.h"
#include "platform/linux/DBus.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>

namespace sashfold::platform {

namespace {

// Skips one value of a complete type in a body (what a{sv} results carry
// besides the uris).
bool skip_value(dbus::Reader& reader, std::string_view signature, std::size_t& at);

// Where the complete type that starts at `at` ends; 0 when it does not.
std::size_t complete_type_end(std::string_view signature, std::size_t at)
{
    if (at >= signature.size())
        return 0;
    char const code = signature[at];
    if (code == 'a')
        return complete_type_end(signature, at + 1);
    if (code == '(' || code == '{') {
        char const close = code == '(' ? ')' : '}';
        std::size_t cursor = at + 1;
        while (cursor < signature.size() && signature[cursor] != close) {
            cursor = complete_type_end(signature, cursor);
            if (cursor == 0)
                return 0;
        }
        return cursor < signature.size() ? cursor + 1 : 0;
    }
    return at + 1;
}

bool skip_one(dbus::Reader& reader, std::string_view signature, std::size_t& at)
{
    if (at >= signature.size())
        return false;
    char const code = signature[at++];
    switch (code) {
    case 'y': return reader.byte().has_value();
    case 'b':
    case 'i':
    case 'u':
        return reader.align(4) && reader.skip(4);
    case 'n':
    case 'q':
        return reader.align(2) && reader.skip(2);
    case 'x':
    case 't':
    case 'd':
        return reader.align(8) && reader.skip(8);
    case 'h': return reader.align(4) && reader.skip(4);
    case 's':
    case 'o':
        return reader.string().has_value();
    case 'g': return reader.signature().has_value();
    case 'v': {
        std::optional<std::string> const inner = reader.signature();
        if (!inner)
            return false;
        std::size_t inner_at = 0;
        return skip_value(reader, *inner, inner_at);
    }
    case 'a': {
        std::optional<std::uint32_t> const length = reader.uint32();
        if (!length)
            return false;
        // The element's alignment: 8 for a struct or dict entry, else its own.
        char const element = at < signature.size() ? signature[at] : '\0';
        std::size_t const alignment = element == '(' || element == '{' || element == 'x' || element == 't' || element == 'd' ? 8
            : element == 'n' || element == 'q'                                                                   ? 2
            : element == 'y' || element == 'g' || element == 'v'                                                 ? 1
                                                                                                                 : 4;
        if (!reader.align(alignment))
            return false;
        std::size_t const end = reader.position() + *length;
        std::size_t const element_at = at;
        std::size_t const element_end = complete_type_end(signature, at);
        if (element_end == 0)
            return false;
        while (reader.position() < end && !reader.failed()) {
            std::size_t cursor = element_at;
            if (!skip_one(reader, signature, cursor))
                return false;
        }
        at = element_end;
        return reader.position() == end;
    }
    case '(':
    case '{': {
        if (!reader.align(8))
            return false;
        char const close = code == '(' ? ')' : '}';
        while (at < signature.size() && signature[at] != close) {
            if (!skip_one(reader, signature, at))
                return false;
        }
        ++at;
        return true;
    }
    default: return false;
    }
}

bool skip_value(dbus::Reader& reader, std::string_view signature, std::size_t& at)
{
    return skip_one(reader, signature, at);
}

// The portal's FileChooser filter list a(sa(us)): a name and, for each
// pattern, 0 and the glob or 1 and the type.
void write_filters(dbus::Writer& writer, std::vector<FileFilter> const& filters)
{
    writer.array(8, [&] {
        for (FileFilter const& filter : filters) {
            writer.align(8);
            writer.string(filter.name);
            writer.array(8, [&] {
                for (std::string const& pattern : filter.patterns) {
                    writer.align(8);
                    writer.uint32(0);
                    writer.string(pattern);
                }
                for (std::string const& type : filter.mime_types) {
                    writer.align(8);
                    writer.uint32(1);
                    writer.string(type);
                }
            });
        }
    });
}

class PortalChooser final : public FileChooser {
public:
    PortalChooser(FileChoice choice, std::function<void()> wake)
        : m_wake(std::move(wake))
    {
        m_thread = std::thread([this, choice = std::move(choice)] { run(choice); });
    }
    ~PortalChooser() override
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    std::optional<std::vector<std::string>> take_result() override
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        if (!m_result)
            return std::nullopt;
        std::optional<std::vector<std::string>> out = std::move(m_result);
        m_result.reset();
        m_taken = true;
        return out;
    }
    bool unavailable() const override { return m_unavailable; }

private:
    void finish(std::optional<std::vector<std::string>> result)
    {
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            if (result)
                m_result = std::move(result);
            else
                m_unavailable = true;
        }
        if (m_wake)
            m_wake();
    }

    void run(FileChoice const& choice)
    {
        std::optional<dbus::SocketAddress> const address = dbus::session_bus_address();
        std::unique_ptr<dbus::Connection> const connection = address ? dbus::Connection::open(*address) : nullptr;
        if (!connection) {
            finish(std::nullopt);
            return;
        }
        // The request's path is known before the call (the portal makes it
        // from this connection's name and the token), so its Response is
        // listened for first and cannot be missed.
        std::string token = "sashfold";
        std::random_device random;
        for (int i = 0; i < 8; ++i)
            token += "0123456789abcdef"[random() % 16];
        std::string sender = connection->unique_name();
        if (!sender.empty() && sender[0] == ':')
            sender.erase(0, 1);
        for (char& c : sender) {
            if (c == '.')
                c = '_';
        }
        std::string const request_path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
        {
            dbus::Message match;
            match.destination = "org.freedesktop.DBus";
            match.path = "/org/freedesktop/DBus";
            match.interface = "org.freedesktop.DBus";
            match.member = "AddMatch";
            match.signature = "s";
            dbus::Writer rule;
            rule.string("type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + request_path + "'");
            match.body = rule.take();
            match.no_reply_expected = true;
            connection->send(std::move(match));
        }
        dbus::Writer arguments;
        arguments.string(choice.parent_window);
        arguments.string(choice.title);
        arguments.array(8, [&] {
            auto const entry = [&](std::string_view key, auto&& value) {
                arguments.align(8);
                arguments.string(key);
                value();
            };
            entry("handle_token", [&] { arguments.variant_string(token); });
            entry("modal", [&] { arguments.variant_boolean(true); });
            entry("multiple", [&] { arguments.variant_boolean(choice.multiple); });
            if (choice.directory)
                entry("directory", [&] { arguments.variant_boolean(true); });
            if (!choice.filters.empty()) {
                entry("filters", [&] {
                    arguments.signature("a(sa(us))");
                    write_filters(arguments, choice.filters);
                });
            }
        });
        dbus::Message call;
        call.destination = "org.freedesktop.portal.Desktop";
        call.path = "/org/freedesktop/portal/desktop";
        call.interface = "org.freedesktop.portal.FileChooser";
        call.member = "OpenFile";
        call.signature = "ssa{sv}";
        call.body = arguments.take();
        std::uint32_t const serial = connection->send(std::move(call));
        if (serial == 0) {
            finish(std::nullopt);
            return;
        }
        bool accepted = false;
        auto const asked = std::chrono::steady_clock::now();
        while (!m_stop) {
            std::optional<dbus::Message> const message = connection->receive(200);
            if (connection->closed()) {
                finish(accepted ? std::optional<std::vector<std::string>>(std::vector<std::string> {}) : std::nullopt);
                return;
            }
            if (!message) {
                // No answer to the call at all in a few seconds: no portal.
                if (!accepted && std::chrono::steady_clock::now() - asked > std::chrono::seconds(5)) {
                    finish(std::nullopt);
                    return;
                }
                continue;
            }
            if (message->reply_serial == serial) {
                if (message->type != dbus::MessageType::MethodReturn) {
                    finish(std::nullopt); // no FileChooser here
                    return;
                }
                accepted = true;
                continue;
            }
            if (message->type != dbus::MessageType::Signal || message->member != "Response" || message->path != request_path)
                continue;
            dbus::Reader reader(message->body);
            std::optional<std::uint32_t> const response = reader.uint32();
            std::vector<std::string> paths;
            if (response && *response == 0) {
                std::optional<std::uint32_t> const length = reader.uint32();
                if (length && reader.align(8)) {
                    std::size_t const end = reader.position() + *length;
                    while (reader.position() < end && !reader.failed()) {
                        reader.align(8);
                        std::optional<std::string> const key = reader.string();
                        std::optional<std::string> const type = reader.signature();
                        if (!key || !type)
                            break;
                        if (*key == "uris" && *type == "as") {
                            if (std::optional<std::vector<std::string>> const uris = reader.string_array()) {
                                for (std::string const& uri : *uris) {
                                    if (std::optional<std::string> path = path_of_file_uri(uri))
                                        paths.push_back(std::move(*path));
                                }
                            }
                        } else {
                            std::size_t at = 0;
                            if (!skip_value(reader, *type, at))
                                break;
                        }
                    }
                }
            }
            finish(std::move(paths));
            return;
        }
    }

    std::function<void()> m_wake;
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_unavailable { false };
    std::mutex m_mutex;
    std::optional<std::vector<std::string>> m_result;
    bool m_taken = false;
};

}

std::unique_ptr<FileChooser> open_file_chooser(FileChoice choice, std::function<void()> wake)
{
    return std::make_unique<PortalChooser>(std::move(choice), std::move(wake));
}

}
