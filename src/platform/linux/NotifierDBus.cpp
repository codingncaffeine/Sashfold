#include "platform/linux/NotifierDBus.h"

// The Desktop Notifications Specification
// (https://specifications.freedesktop.org/notification-spec/): the service
// org.freedesktop.Notifications on the session bus, its Notify and
// CloseNotification calls and its NotificationClosed, ActivationToken and
// ActionInvoked signals. A notice's picture goes as its "image-data" hint
// (the specification's raw RGBA, §Icons and Images). One thread holds the connection: it sends what the window's
// thread asks for and turns what comes back into events.

#include <atomic>
#include <cerrno>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace sashfold::platform {

namespace {

constexpr char const* service = "org.freedesktop.Notifications";
constexpr char const* object_path = "/org/freedesktop/Notifications";

class DBusNotifier final : public Notifier {
public:
    DBusNotifier(dbus::SocketAddress address, std::string application, std::function<void()> wake)
        : m_address(std::move(address))
        , m_application(std::move(application))
        , m_wake(std::move(wake))
        , m_poke(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    {
        m_thread = std::thread([this] { run(); });
    }

    ~DBusNotifier() override
    {
        m_stop = true;
        poke();
        if (m_thread.joinable())
            m_thread.join();
        if (m_poke >= 0)
            ::close(m_poke);
    }

    bool available() const override { return m_available; }

    std::uint64_t show(Notice const& notice, std::uint64_t replaces) override
    {
        std::uint64_t const id = m_next_id++;
        {
            std::lock_guard const lock(m_mutex);
            if (!m_available) {
                m_events.push_back(Event { Event::Kind::Failed, id, {} });
                if (m_wake)
                    m_wake();
                return id;
            }
            m_requests.push_back(Request { id, replaces, notice, false });
        }
        poke();
        return id;
    }

    void close(std::uint64_t id) override
    {
        {
            std::lock_guard const lock(m_mutex);
            m_requests.push_back(Request { id, 0, {}, true });
        }
        poke();
    }

    std::vector<Event> take_events() override
    {
        std::lock_guard const lock(m_mutex);
        return std::exchange(m_events, {});
    }

private:
    struct Request {
        std::uint64_t id = 0;
        std::uint64_t replaces = 0;
        Notice notice;
        bool close = false;
    };

    void poke()
    {
        std::uint64_t const one = 1;
        if (m_poke >= 0 && write(m_poke, &one, sizeof one) < 0) {
            // Full only when it is already readable: the thread wakes.
        }
    }

    void say(Event::Kind kind, std::uint64_t id, std::string activation_token = {})
    {
        {
            std::lock_guard const lock(m_mutex);
            m_events.push_back(Event { kind, id, std::move(activation_token) });
        }
        if (m_wake)
            m_wake();
    }

    dbus::Message call(char const* member, std::string signature, std::vector<std::uint8_t> body) const
    {
        dbus::Message message;
        message.destination = service;
        message.path = object_path;
        message.interface = service;
        message.member = member;
        message.signature = std::move(signature);
        message.body = std::move(body);
        return message;
    }

    // Nothing answers, or no longer does: whatever was asked and is still
    // waiting did not show, and what is asked from now on fails at once.
    void gone()
    {
        std::deque<Request> left;
        {
            std::lock_guard const lock(m_mutex);
            m_available = false;
            left.swap(m_requests);
        }
        for (Request const& request : left) {
            if (!request.close)
                say(Event::Kind::Failed, request.id);
        }
        if (m_wake)
            m_wake();
    }

    void run()
    {
        std::unique_ptr<dbus::Connection> connection = dbus::Connection::open(m_address);
        if (!connection) {
            gone();
            return;
        }
        // The service's signals, and only the service's: the bus passes on
        // a signal under this rule only from the name's owner.
        {
            dbus::Message match;
            match.destination = "org.freedesktop.DBus";
            match.path = "/org/freedesktop/DBus";
            match.interface = "org.freedesktop.DBus";
            match.member = "AddMatch";
            match.signature = "s";
            dbus::Writer rule;
            rule.string(std::string("type='signal',sender='") + service + "',interface='" + service + "',path='" + object_path + "'");
            match.body = rule.take();
            match.no_reply_expected = true;
            connection->send(std::move(match));
        }
        // What the service does with a body: with "body-markup" it reads
        // tags, and a page's text must be escaped. Its answer is also the
        // sign that there is a service at all.
        std::uint32_t const capabilities = connection->send(call("GetCapabilities", "", {}));
        bool markup = false;
        // Nothing is shown before that answer: how a body is to be written
        // is not known until it comes.
        bool ready = false;
        // Ours for the service's, the service's for ours, and the Notify
        // calls not yet answered.
        std::map<std::uint64_t, std::uint32_t> theirs;
        std::map<std::uint32_t, std::uint64_t> ours;
        std::map<std::uint32_t, std::uint64_t> asked;
        std::map<std::uint64_t, bool> close_when_shown;
        // The token the service sent ahead of a press (version 1.2 of the
        // specification), by its number, for the press that follows.
        std::map<std::uint32_t, std::string> tokens;

        while (!m_stop) {
            std::optional<dbus::Message> const message = connection->receive(-1, m_poke);
            if (connection->closed())
                break;
            if (message) {
                if (message->reply_serial == capabilities && capabilities != 0) {
                    if (message->type == dbus::MessageType::MethodReturn) {
                        dbus::Reader body(message->body);
                        if (std::optional<std::vector<std::string>> const list = body.string_array()) {
                            for (std::string const& capability : *list)
                                markup = markup || capability == "body-markup";
                        }
                        ready = true;
                    } else {
                        break; // no service answers to the name
                    }
                } else if (auto const pending = asked.find(message->reply_serial); pending != asked.end()) {
                    std::uint64_t const id = pending->second;
                    asked.erase(pending);
                    dbus::Reader body(message->body);
                    std::optional<std::uint32_t> const given = message->type == dbus::MessageType::MethodReturn ? body.uint32() : std::nullopt;
                    if (!given) {
                        close_when_shown.erase(id);
                        say(Event::Kind::Failed, id);
                    } else {
                        // A notice put in another's place has its number.
                        if (auto const before = ours.find(*given); before != ours.end())
                            theirs.erase(before->second);
                        theirs[id] = *given;
                        ours[*given] = id;
                        say(Event::Kind::Shown, id);
                        if (close_when_shown.erase(id) != 0)
                            send_close(*connection, *given);
                    }
                } else if (message->type == dbus::MessageType::Signal && message->interface == service) {
                    dbus::Reader body(message->body);
                    std::optional<std::uint32_t> const number = body.uint32();
                    auto const known = number ? ours.find(*number) : ours.end();
                    if (known != ours.end()) {
                        std::uint64_t const id = known->second;
                        if (message->member == "ActivationToken") {
                            if (std::optional<std::string> token = body.string())
                                tokens[*number] = std::move(*token);
                        } else if (message->member == "ActionInvoked") {
                            std::string token;
                            if (auto const given = tokens.find(*number); given != tokens.end()) {
                                token = std::move(given->second);
                                tokens.erase(given);
                            }
                            say(Event::Kind::Clicked, id, std::move(token));
                        } else if (message->member == "NotificationClosed") {
                            tokens.erase(*number);
                            ours.erase(known);
                            theirs.erase(id);
                            say(Event::Kind::Closed, id);
                        }
                    }
                }
            }
            std::uint64_t count = 0;
            if (read(m_poke, &count, sizeof count) < 0 && errno != EAGAIN)
                break;
            std::deque<Request> requests;
            if (ready) {
                std::lock_guard const lock(m_mutex);
                requests.swap(m_requests);
            }
            for (Request const& request : requests) {
                if (request.close) {
                    if (auto const shown = theirs.find(request.id); shown != theirs.end())
                        send_close(*connection, shown->second);
                    else
                        close_when_shown[request.id] = true; // its Notify is not answered yet
                    continue;
                }
                std::uint32_t replaces = 0;
                if (auto const before = theirs.find(request.replaces); before != theirs.end())
                    replaces = before->second;
                Notice const& notice = request.notice;
                std::string body = notice.site;
                if (!notice.body.empty())
                    body += (body.empty() ? "" : "\n") + notice.body;
                dbus::Writer arguments;
                arguments.string(m_application);
                arguments.uint32(replaces);
                arguments.string(m_application_icon);
                arguments.string(notice.title.empty() ? notice.site : notice.title);
                arguments.string(markup ? escape_notice_markup(body) : body);
                // One action, the notice itself: pressing it is "default".
                arguments.array(4, [&] {
                    arguments.string("default");
                    arguments.string("Show");
                });
                arguments.array(8, [&] {
                    arguments.align(8);
                    arguments.string("desktop-entry");
                    arguments.variant_string(m_application_icon);
                    if (notice.silent) {
                        arguments.align(8);
                        arguments.string("suppress-sound");
                        arguments.variant_boolean(true);
                    }
                    if (notice.picture) {
                        // (iiibiiay): width, height, the bytes a row, alpha,
                        // bits a sample, samples a pixel, then the pixels.
                        Bitmap const& picture = *notice.picture;
                        arguments.align(8);
                        arguments.string("image-data");
                        arguments.signature("(iiibiiay)");
                        arguments.align(8);
                        arguments.int32(picture.width());
                        arguments.int32(picture.height());
                        arguments.int32(picture.width() * 4);
                        arguments.boolean(true);
                        arguments.int32(8);
                        arguments.int32(4);
                        arguments.byte_array(picture.pixels());
                    }
                });
                arguments.int32(notice.stays ? 0 : -1); // never, or as long as the desktop likes
                std::uint32_t const serial = connection->send(call("Notify", "susssasa{sv}i", arguments.take()));
                if (serial == 0)
                    say(Event::Kind::Failed, request.id);
                else
                    asked[serial] = request.id;
            }
        }
        // Whatever was asked and never answered did not show.
        for (auto const& [serial, id] : asked)
            say(Event::Kind::Failed, id);
        gone();
    }

    void send_close(dbus::Connection& connection, std::uint32_t number) const
    {
        dbus::Writer arguments;
        arguments.uint32(number);
        dbus::Message message = call("CloseNotification", "u", arguments.take());
        message.no_reply_expected = true;
        connection.send(std::move(message));
    }

    dbus::SocketAddress m_address;
    std::string m_application;
    std::string m_application_icon = "sashfold"; // the desktop entry's name, and its icon's
    std::function<void()> m_wake;
    int m_poke = -1;
    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_available { true }; // until it is known that nothing answers
    std::atomic<std::uint64_t> m_next_id { 1 };
    std::mutex m_mutex;
    std::deque<Request> m_requests;
    std::vector<Event> m_events;
    std::thread m_thread;
};

}

std::unique_ptr<Notifier> open_notifier_on(dbus::SocketAddress address, std::string application, std::function<void()> wake)
{
    return std::make_unique<DBusNotifier>(std::move(address), std::move(application), std::move(wake));
}

std::unique_ptr<Notifier> open_notifier(std::string application, std::function<void()> wake)
{
    std::optional<dbus::SocketAddress> address = dbus::session_bus_address();
    if (!address)
        return nullptr;
    return open_notifier_on(std::move(*address), std::move(application), std::move(wake));
}

}
