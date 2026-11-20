#pragma once
// A HID link in memory, for the security-key tests: the host side is an
// auth::HidConnection, the device side a queue a test authenticator reads.
// As with hidraw, every open host connection receives every input report
// the device sends, and all of them write into the one device queue. A
// link can be unplugged at any moment: reads and writes then fail with
// Gone, as a device that has left the bus does.
#include "auth/Transport.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace sashfold::test {

class FakeLink : public std::enable_shared_from_this<FakeLink> {
public:
    struct HostQueue {
        std::deque<std::vector<std::uint8_t>> reports;
    };

    explicit FakeLink(std::size_t report_size = 64)
        : m_report_size(report_size)
    {
    }

    std::size_t report_size() const { return m_report_size; }

    // Host side.
    std::unique_ptr<auth::HidConnection> connect();

    // Device side: the next report a host wrote, waiting up to `timeout`.
    std::optional<std::vector<std::uint8_t>> device_read(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        m_changed.wait_for(lock, timeout, [&] { return !m_to_device.empty() || m_unplugged; });
        if (m_to_device.empty())
            return std::nullopt;
        auto report = std::move(m_to_device.front());
        m_to_device.pop_front();
        return report;
    }

    // Device side: one input report to every open host connection.
    void device_write(std::vector<std::uint8_t> report)
    {
        report.resize(m_report_size, 0);
        std::lock_guard lock(m_mutex);
        if (m_unplugged)
            return;
        for (auto const& weak : m_hosts) {
            if (auto host = weak.lock())
                host->reports.push_back(report);
        }
        m_changed.notify_all();
    }

    void unplug()
    {
        std::lock_guard lock(m_mutex);
        m_unplugged = true;
        m_changed.notify_all();
    }
    bool unplugged()
    {
        std::lock_guard lock(m_mutex);
        return m_unplugged;
    }

    // Host side, used by the connection.
    bool host_write(std::span<std::uint8_t const> report)
    {
        std::lock_guard lock(m_mutex);
        if (m_unplugged)
            return false;
        if (report.size() != m_report_size)
            return false;
        m_to_device.emplace_back(report.begin(), report.end());
        ++m_reports_written;
        m_changed.notify_all();
        return true;
    }

    auth::HidConnection::Read host_read(HostQueue& queue, std::vector<std::uint8_t>& report, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        m_changed.wait_for(lock, timeout, [&] { return !queue.reports.empty() || m_unplugged; });
        if (m_unplugged)
            return auth::HidConnection::Read::Gone;
        if (queue.reports.empty())
            return auth::HidConnection::Read::Timeout;
        report = std::move(queue.reports.front());
        queue.reports.pop_front();
        return auth::HidConnection::Read::Report;
    }

    std::shared_ptr<HostQueue> add_host()
    {
        auto queue = std::make_shared<HostQueue>();
        std::lock_guard lock(m_mutex);
        m_hosts.push_back(queue);
        return queue;
    }

    std::size_t reports_written()
    {
        std::lock_guard lock(m_mutex);
        return m_reports_written;
    }

private:
    std::size_t m_report_size;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::vector<std::uint8_t>> m_to_device;
    std::vector<std::weak_ptr<HostQueue>> m_hosts;
    bool m_unplugged = false;
    std::size_t m_reports_written = 0;
};

class FakeConnection final : public auth::HidConnection {
public:
    explicit FakeConnection(std::shared_ptr<FakeLink> link)
        : m_link(std::move(link))
        , m_queue(m_link->add_host())
    {
    }
    bool write_report(std::span<std::uint8_t const> report) override { return m_link->host_write(report); }
    Read read_report(std::vector<std::uint8_t>& report, std::chrono::milliseconds timeout) override
    {
        return m_link->host_read(*m_queue, report, timeout);
    }

private:
    std::shared_ptr<FakeLink> m_link;
    std::shared_ptr<FakeLink::HostQueue> m_queue;
};

inline std::unique_ptr<auth::HidConnection> FakeLink::connect() { return std::make_unique<FakeConnection>(shared_from_this()); }

}
