// The desktop portal's printing (xdg-desktop-portal, Print.Print): the PDF
// goes over the bus as a file descriptor (a memory file), the portal shows
// the desktop's own print dialog for it and prints the file; the request's
// Response says printed, cancelled or failed. KDE and GNOME answer it.
#include "platform/linux/PrintPortal.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace sashfold::platform {

namespace {

class PortalPrint final : public PrintRequest {
public:
    PortalPrint(std::optional<dbus::SocketAddress> address, PrintJob job, std::function<void()> wake)
        : m_wake(std::move(wake))
    {
        m_thread = std::thread([this, address = std::move(address), job = std::move(job)] { run(address, job); });
    }
    ~PortalPrint() override
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
    }
    Outcome outcome() const override { return m_outcome; }

private:
    void finish(Outcome outcome)
    {
        m_outcome = outcome;
        if (m_wake)
            m_wake();
    }

    void run(std::optional<dbus::SocketAddress> const& address, PrintJob const& job)
    {
        std::unique_ptr<dbus::Connection> const connection = address ? dbus::Connection::open(*address) : nullptr;
        if (!connection || !connection->passes_fds()) {
            finish(Outcome::Unavailable);
            return;
        }
        // The document as a memory file the portal reads.
        int const fd = memfd_create("sashfold-print", MFD_CLOEXEC);
        if (fd < 0) {
            finish(Outcome::Unavailable);
            return;
        }
        std::size_t written = 0;
        while (written < job.pdf.size()) {
            ssize_t const n = write(fd, job.pdf.data() + written, job.pdf.size() - written);
            if (n <= 0) {
                close(fd);
                finish(Outcome::Failed);
                return;
            }
            written += static_cast<std::size_t>(n);
        }
        lseek(fd, 0, SEEK_SET);

        // The request's path is known before the call, so its Response is
        // listened for first and cannot be missed (as the file chooser does).
        std::string token = "sashfoldprint";
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
        // Print(parent_window, title, fd, options): with no token from a
        // PreparePrint, the portal shows its dialog for this file.
        dbus::Writer arguments;
        arguments.string(job.parent_window);
        arguments.string(job.title);
        arguments.uint32(0); // the file descriptor: the first passed
        arguments.array(8, [&] {
            auto const entry = [&](std::string_view key, auto&& value) {
                arguments.align(8);
                arguments.string(key);
                value();
            };
            entry("handle_token", [&] { arguments.variant_string(token); });
            entry("modal", [&] { arguments.variant_boolean(true); });
        });
        dbus::Message call;
        call.destination = "org.freedesktop.portal.Desktop";
        call.path = "/org/freedesktop/portal/desktop";
        call.interface = "org.freedesktop.portal.Print";
        call.member = "Print";
        call.signature = "ssha{sv}";
        call.body = arguments.take();
        call.fds = { fd };
        std::uint32_t const serial = connection->send(std::move(call));
        close(fd); // the bus holds its own copy
        if (serial == 0) {
            finish(Outcome::Unavailable);
            return;
        }
        bool accepted = false;
        auto const asked = std::chrono::steady_clock::now();
        while (!m_stop) {
            std::optional<dbus::Message> const message = connection->receive(200);
            if (connection->closed()) {
                finish(accepted ? Outcome::Failed : Outcome::Unavailable);
                return;
            }
            if (!message) {
                if (!accepted && std::chrono::steady_clock::now() - asked > std::chrono::seconds(5)) {
                    finish(Outcome::Unavailable);
                    return;
                }
                continue;
            }
            if (message->reply_serial == serial) {
                if (message->type != dbus::MessageType::MethodReturn) {
                    finish(Outcome::Unavailable); // no Print portal here
                    return;
                }
                accepted = true;
                continue;
            }
            if (message->type != dbus::MessageType::Signal || message->member != "Response" || message->path != request_path)
                continue;
            dbus::Reader reader(message->body);
            std::optional<std::uint32_t> const response = reader.uint32();
            finish(!response ? Outcome::Failed : *response == 0 ? Outcome::Printed : *response == 1 ? Outcome::Cancelled : Outcome::Failed);
            return;
        }
    }

    std::function<void()> m_wake;
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
    std::atomic<Outcome> m_outcome { Outcome::Pending };
};

}

std::unique_ptr<PrintRequest> print_document_on(dbus::SocketAddress address, PrintJob job, std::function<void()> wake)
{
    return std::make_unique<PortalPrint>(std::move(address), std::move(job), std::move(wake));
}

std::unique_ptr<PrintRequest> print_document(PrintJob job, std::function<void()> wake)
{
    return std::make_unique<PortalPrint>(dbus::session_bus_address(), std::move(job), std::move(wake));
}

}
