#include "platform/linux/FidoHidLinux.h"

#include "auth/HidDescriptor.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <mutex>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef HIDIOCSOUTPUT
// Linux 5.11 on; spelled here for headers older than the kernel.
#define HIDIOCSOUTPUT(len) _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x0B, len)
#endif

namespace sashfold::platform {

namespace {

// The descriptor and uevent files are small; a file past these sizes is
// not what sysfs writes.
constexpr std::size_t descriptor_limit = 4096; // HID_MAX_DESCRIPTOR_SIZE
constexpr std::size_t uevent_limit = 4096;

std::optional<std::vector<std::uint8_t>> read_small_file(std::string const& path, std::size_t limit)
{
    int const fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    std::vector<std::uint8_t> content(limit + 1);
    std::size_t filled = 0;
    while (filled < content.size()) {
        ssize_t const n = read(fd, content.data() + filled, content.size() - filled);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        filled += static_cast<std::size_t>(n);
    }
    close(fd);
    if (filled > limit)
        return std::nullopt;
    content.resize(filled);
    return content;
}

struct Found {
    auth::DeviceInfo info;
    auth::FidoReportLayout layout;
};

// HID_ID=0003:00001050:00000407 and HID_NAME=... from the device's uevent.
void read_identity(std::string const& folder, auth::DeviceInfo& info)
{
    auto const uevent = read_small_file(folder + "/device/uevent", uevent_limit);
    if (!uevent)
        return;
    std::string_view text(reinterpret_cast<char const*>(uevent->data()), uevent->size());
    while (!text.empty()) {
        std::size_t const end = text.find('\n');
        std::string_view const line = text.substr(0, end);
        if (line.starts_with("HID_NAME="))
            info.product = std::string(line.substr(9));
        if (line.starts_with("HID_ID=") && line.size() >= 7 + 4 + 1 + 8 + 1 + 8) {
            auto const hex = [](std::string_view digits) {
                std::uint32_t value = 0;
                for (char const c : digits) {
                    int const n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
                    value = value << 4 | static_cast<std::uint32_t>(n);
                }
                return value;
            };
            info.vendor_id = static_cast<std::uint16_t>(hex(line.substr(12, 8)));
            info.product_id = static_cast<std::uint16_t>(hex(line.substr(21, 8)));
        }
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
}

std::vector<Found> enumerate(std::string const& sysfs_root, std::string const& device_root)
{
    std::vector<Found> found;
    DIR* const directory = opendir(sysfs_root.c_str());
    if (directory == nullptr)
        return found;
    while (dirent const* const entry = readdir(directory)) {
        std::string_view const name = entry->d_name;
        if (!name.starts_with("hidraw"))
            continue;
        std::string const folder = sysfs_root + "/" + std::string(name);
        auto const descriptor = read_small_file(folder + "/device/report_descriptor", descriptor_limit);
        if (!descriptor)
            continue;
        auto const layout = auth::parse_fido_report_descriptor(*descriptor);
        if (!layout)
            continue;
        Found device;
        device.layout = *layout;
        device.info.path = device_root + "/" + std::string(name);
        device.info.input_report_size = layout->input_bytes;
        device.info.output_report_size = layout->output_bytes;
        read_identity(folder, device.info);
        found.push_back(std::move(device));
    }
    closedir(directory);
    std::ranges::sort(found, [](Found const& a, Found const& b) { return a.info.path < b.info.path; });
    return found;
}

class HidrawConnection final : public auth::HidConnection {
public:
    HidrawConnection(int fd, bool write_by_request, auth::FidoReportLayout layout)
        : m_fd(fd)
        , m_write_by_request(write_by_request)
        , m_layout(layout)
    {
    }
    ~HidrawConnection() override { close(m_fd); }

    bool write_report(std::span<std::uint8_t const> report) override
    {
        // The report number first, 0 for a device that does not number
        // its reports (the kernel's hidraw interface wants it either way).
        // Then the report, held to the device's report length: zeros after
        // a shorter one.
        std::vector<std::uint8_t> buffer(m_layout.output_bytes + 1, 0);
        buffer[0] = m_layout.output_report_id;
        std::copy_n(report.begin(), std::min(report.size(), buffer.size() - 1), buffer.begin() + 1);
        while (true) {
            long const n = m_write_by_request ? ioctl(m_fd, HIDIOCSOUTPUT(buffer.size()), buffer.data())
                                              : static_cast<long>(write(m_fd, buffer.data(), buffer.size()));
            if (n < 0 && errno == EINTR)
                continue;
            return n == static_cast<long>(buffer.size()) || (m_write_by_request && n >= 0);
        }
    }

    Read read_report(std::vector<std::uint8_t>& report, std::chrono::milliseconds timeout) override
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pollfd poll_fd { m_fd, POLLIN, 0 };
            int const ready = poll(&poll_fd, 1, static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, left.count())));
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0)
                return Read::Gone;
            if (ready == 0)
                return Read::Timeout;
            if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                return Read::Gone;
            std::vector<std::uint8_t> buffer(m_layout.input_bytes + 1);
            ssize_t const n = read(m_fd, buffer.data(), buffer.size());
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n <= 0)
                return Read::Gone; // ENODEV when the key is pulled out
            buffer.resize(static_cast<std::size_t>(n));
            std::size_t start = 0;
            if (m_layout.numbered) {
                // Another report of the same interface is not ours.
                if (buffer.empty() || buffer[0] != m_layout.input_report_id)
                    continue;
                start = 1;
            }
            report.assign(buffer.begin() + static_cast<std::ptrdiff_t>(start), buffer.end());
            report.resize(m_layout.input_bytes, 0);
            return Read::Report;
        }
    }

private:
    int m_fd;
    bool m_write_by_request;
    auth::FidoReportLayout m_layout;
};

class HidrawTransport final : public auth::Transport {
public:
    HidrawTransport(std::string sysfs_root, std::string device_root)
        : m_sysfs_root(std::move(sysfs_root))
        , m_device_root(std::move(device_root))
    {
        m_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotify >= 0
            && inotify_add_watch(m_inotify, m_device_root.c_str(), IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO | IN_MOVED_FROM) < 0) {
            close(m_inotify);
            m_inotify = -1;
        }
    }
    ~HidrawTransport() override
    {
        if (m_inotify >= 0)
            close(m_inotify);
    }

    std::vector<auth::DeviceInfo> devices() override
    {
        std::vector<auth::DeviceInfo> out;
        for (auto const& found : enumerate(m_sysfs_root, m_device_root))
            out.push_back(found.info);
        return out;
    }

    std::unique_ptr<auth::HidConnection> open(auth::DeviceInfo const& device) override
    {
        // The layout is read again: the node may have been reused by another device.
        for (auto const& found : enumerate(m_sysfs_root, m_device_root)) {
            if (found.info.path != device.path)
                continue;
            int fd = ::open(device.path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
            bool by_request = false;
            if (fd < 0 && (errno == EACCES || errno == EPERM)) {
                // Not writable to us: the confinement's case above (a
                // device the user may not use at all fails here too).
                fd = ::open(device.path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
                by_request = true;
            }
            if (fd < 0)
                return nullptr;
            return std::make_unique<HidrawConnection>(fd, by_request, found.layout);
        }
        return nullptr;
    }

    bool wait_for_change(std::chrono::milliseconds timeout) override
    {
        std::lock_guard lock(m_mutex);
        if (m_inotify < 0) {
            usleep(static_cast<useconds_t>(timeout.count()) * 1000);
            return true;
        }
        pollfd poll_fd { m_inotify, POLLIN, 0 };
        int const ready = poll(&poll_fd, 1, static_cast<int>(timeout.count()));
        if (ready <= 0)
            return false;
        // Drain the events; any about a hidraw node counts.
        bool relevant = false;
        alignas(inotify_event) char buffer[4096];
        while (true) {
            ssize_t const n = read(m_inotify, buffer, sizeof buffer);
            if (n <= 0)
                break;
            for (ssize_t at = 0; at + static_cast<ssize_t>(sizeof(inotify_event)) <= n;) {
                inotify_event event;
                std::memcpy(&event, buffer + at, sizeof event);
                std::size_t const name_space = event.len;
                if (at + static_cast<ssize_t>(sizeof event + name_space) > n)
                    break;
                std::string_view const name(buffer + at + sizeof event, ::strnlen(buffer + at + sizeof event, name_space));
                relevant = relevant || name.starts_with("hidraw") || (event.mask & IN_Q_OVERFLOW) != 0;
                at += static_cast<ssize_t>(sizeof event + name_space);
            }
        }
        return relevant;
    }

private:
    std::string m_sysfs_root;
    std::string m_device_root;
    int m_inotify = -1;
    std::mutex m_mutex;
};

}

std::unique_ptr<auth::Transport> hidraw_transport(std::string sysfs_root, std::string device_root)
{
    return std::make_unique<HidrawTransport>(std::move(sysfs_root), std::move(device_root));
}

std::vector<std::string> security_key_nodes(std::string const& sysfs_root, std::string const& device_root)
{
    std::vector<std::string> nodes;
    for (auto const& found : enumerate(sysfs_root, device_root))
        nodes.push_back(found.info.path);
    return nodes;
}

}

namespace sashfold::auth {

std::unique_ptr<Transport> system_transport() { return platform::hidraw_transport(); }

}
