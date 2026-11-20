// Finding security keys: report descriptors written by hand from the HID
// specification and CTAP 2.1 §11.2.8.2 (a FIDO key's 34 bytes, a composite
// device that numbers its reports, a keyboard that must be refused, broken
// ones), and the Linux hidraw transport pointed at a tree of files and
// FIFOs made here — never at the machine's /sys or /dev.
#include "AuthTestSupport.h"
#include "Test.h"

#include "auth/HidDescriptor.h"

#if defined(__linux__)
#include "platform/Confine.h"
#include "platform/linux/FidoHidLinux.h"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::from_hex;

namespace {

// CTAP 2.1 §11.2.8.2's descriptor in bytes (HID 1.11 §6.2.2 item codes):
// Usage Page 0xF1D0, Usage 1, Collection (Application), Usage 0x20 (data
// in), Logical 0..255, Report Size 8, Report Count 64, Input (Data, Var,
// Abs), Usage 0x21 (data out), the same extents, Output, End Collection.
char const* const fido_descriptor = "06d0f1 0901 a101"
                                    "0920 1500 26ff00 7508 9540 8102"
                                    "0921 1500 26ff00 7508 9540 9102"
                                    "c0";

// HID 1.11 Appendix E.6, the boot keyboard's descriptor.
char const* const keyboard_descriptor = "0501 0906 a101 0507 19e0 29e7 1500 2501 7501 9508 8102 9501 7508 8101"
                                        "9505 7501 0508 1901 2905 9102 9501 7503 9101 9506 7508 1500 2565 0507"
                                        "1900 2965 8100 c0";

// A composite device: a keyboard as report 1, the FIDO collection as
// report 2 with 32-byte packets, and a long item for good measure.
char const* const composite_descriptor = "0501 0906 a101 8501 0507 19e0 29e7 1500 2501 7501 9508 8102 c0"
                                         "fe0201aabb"
                                         "06d0f1 0901 a101 8502"
                                         "0920 1500 26ff00 7508 9520 8102"
                                         "0921 1500 26ff00 7508 9520 9102"
                                         "c0";

void descriptors()
{
    auto const fido = from_hex(fido_descriptor);
    CHECK_EQ(fido.size(), std::size_t { 34 });
    auto const layout = parse_fido_report_descriptor(fido);
    CHECK(layout.has_value());
    if (layout) {
        CHECK_EQ(layout->input_bytes, std::size_t { 64 });
        CHECK_EQ(layout->output_bytes, std::size_t { 64 });
        CHECK(!layout->numbered);
        CHECK_EQ(int(layout->input_report_id), 0);
    }
    CHECK(!parse_fido_report_descriptor(from_hex(keyboard_descriptor)));
    auto const composite = parse_fido_report_descriptor(from_hex(composite_descriptor));
    CHECK(composite.has_value());
    if (composite) {
        CHECK(composite->numbered);
        CHECK_EQ(int(composite->input_report_id), 2);
        CHECK_EQ(int(composite->output_report_id), 2);
        CHECK_EQ(composite->input_bytes, std::size_t { 32 });
    }
    // The FIDO usage written as one 4-byte usage (page in the high half).
    CHECK(parse_fido_report_descriptor(from_hex("0b0100d0f1 a101 0920 7508 9540 8102 0921 7508 9540 9102 c0")).has_value());
    // The FIDO usage on a collection that is not an application, or nested.
    CHECK(!parse_fido_report_descriptor(from_hex("06d0f1 0901 a100 0920 7508 9540 8102 0921 7508 9540 9102 c0")));
    CHECK(!parse_fido_report_descriptor(from_hex("0501 0902 a101 06d0f1 0901 a101 0920 7508 9540 8102 0921 7508 9540 9102 c0 c0")));
    // No output report, packets too small to carry a header, odd bit counts.
    CHECK(!parse_fido_report_descriptor(from_hex("06d0f1 0901 a101 0920 7508 9540 8102 c0")));
    CHECK(!parse_fido_report_descriptor(from_hex("06d0f1 0901 a101 0920 7508 9504 8102 0921 7508 9504 9102 c0")));
    CHECK(!parse_fido_report_descriptor(from_hex("06d0f1 0901 a101 0920 7507 9541 8102 0921 7508 9540 9102 c0")));
    // Every truncation of the good one: refused, never read past the end.
    for (std::size_t n = 0; n < fido.size(); ++n)
        CHECK(!parse_fido_report_descriptor(std::span<std::uint8_t const>(fido).first(n)));
    // Unbalanced collections, a pop with nothing pushed, report ID 0.
    CHECK(!parse_fido_report_descriptor(from_hex("c0")));
    CHECK(!parse_fido_report_descriptor(from_hex("b4")));
    CHECK(!parse_fido_report_descriptor(from_hex("8500")));
    // Push and pop restore the page and sizes.
    CHECK(parse_fido_report_descriptor(from_hex("06d0f1 a4 0501 b4 0901 a101 0920 7508 9540 8102 0921 7508 9540 9102 c0")).has_value());
    // A count that would overflow: refused.
    CHECK(!parse_fido_report_descriptor(from_hex("06d0f1 0901 a101 0920 77ffffffff 97ffffffff 8102 c0")));
}

#if defined(__linux__)

namespace fs = std::filesystem;

void write_file(fs::path const& path, std::vector<std::uint8_t> const& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_text(fs::path const& path, std::string const& text) { write_file(path, std::vector<std::uint8_t>(text.begin(), text.end())); }

void hidraw_tree()
{
    // The tree lives in the test's own working folder.
    fs::path const root = fs::current_path() / ("hidraw-tree-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::path const sys = root / "sys";
    fs::path const dev = root / "dev";
    fs::create_directories(sys);
    fs::create_directories(dev);
    // hidraw0: a keyboard; hidraw1: a key; hidraw2: a composite numbered device; hidraw3: no descriptor.
    write_file(sys / "hidraw0/device/report_descriptor", from_hex(keyboard_descriptor));
    write_file(sys / "hidraw1/device/report_descriptor", from_hex(fido_descriptor));
    write_text(sys / "hidraw1/device/uevent", "DRIVER=hid-generic\nHID_ID=0003:00001234:0000ABCD\nHID_NAME=Test Security Key\nHID_PHYS=usb-1/input0\n");
    write_file(sys / "hidraw2/device/report_descriptor", from_hex(composite_descriptor));
    fs::create_directories(sys / "hidraw3/device");
    write_text(sys / "unrelated/device/report_descriptor", "x");
    for (char const* const node : { "hidraw0", "hidraw1", "hidraw2", "hidraw3" })
        CHECK(mkfifo((dev / node).c_str(), 0600) == 0);

    auto transport = platform::hidraw_transport(sys.string(), dev.string());
    auto const devices = transport->devices();
    CHECK_EQ(devices.size(), std::size_t { 2 });
    if (devices.size() == 2) {
        CHECK_EQ(devices[0].path, (dev / "hidraw1").string());
        CHECK_EQ(devices[0].product, std::string("Test Security Key"));
        CHECK_EQ(devices[0].vendor_id, std::uint16_t { 0x1234 });
        CHECK_EQ(devices[0].product_id, std::uint16_t { 0xabcd });
        CHECK_EQ(devices[0].input_report_size, std::size_t { 64 });
        CHECK_EQ(devices[1].path, (dev / "hidraw2").string());
        CHECK_EQ(devices[1].input_report_size, std::size_t { 32 });
    }
    CHECK(platform::security_key_nodes(sys.string(), dev.string()) == std::vector<std::string>({ (dev / "hidraw1").string(), (dev / "hidraw2").string() }));

    // Packet I/O through the FIFO standing in for the node: what the
    // transport writes carries the report number first; what it reads is
    // the report alone (numbered: the ID taken off; another ID skipped).
    if (devices.size() == 2) {
        auto connection = transport->open(devices[1]);
        CHECK(connection != nullptr);
        int const other_end = open((dev / "hidraw2").c_str(), O_RDWR | O_NONBLOCK);
        CHECK(other_end >= 0);
        if (connection && other_end >= 0) {
            std::vector<std::uint8_t> packet(32, 0x11);
            CHECK(connection->write_report(packet));
            std::vector<std::uint8_t> written(64);
            ssize_t const n = read(other_end, written.data(), written.size());
            CHECK_EQ(n, ssize_t { 33 });
            CHECK_EQ(int(written[0]), 2);
            CHECK_EQ(int(written[1]), 0x11);
            // A report of the keyboard (ID 1), then ours (ID 2).
            std::vector<std::uint8_t> keyboard(9, 0);
            keyboard[0] = 1;
            std::vector<std::uint8_t> ours(33, 0x22);
            ours[0] = 2;
            // One at a time, so that the FIFO hands each over as one read,
            // as hidraw hands over one report per read.
            std::vector<std::uint8_t> report;
            CHECK(write(other_end, keyboard.data(), keyboard.size()) == ssize_t(keyboard.size()));
            CHECK(connection->read_report(report, std::chrono::milliseconds(150)) == HidConnection::Read::Timeout);
            CHECK(write(other_end, ours.data(), ours.size()) == ssize_t(ours.size()));
            CHECK(connection->read_report(report, std::chrono::milliseconds(500)) == HidConnection::Read::Report);
            CHECK_EQ(report.size(), std::size_t { 32 });
            CHECK(report == std::vector<std::uint8_t>(32, 0x22));
            close(other_end);
        }
        // Unnumbered: report number 0 in front of what is written.
        auto plain = transport->open(devices[0]);
        int const plain_end = open((dev / "hidraw1").c_str(), O_RDWR | O_NONBLOCK);
        if (plain && plain_end >= 0) {
            std::vector<std::uint8_t> packet(64, 0x33);
            CHECK(plain->write_report(packet));
            std::vector<std::uint8_t> written(128);
            CHECK_EQ(read(plain_end, written.data(), written.size()), ssize_t { 65 });
            CHECK_EQ(int(written[0]), 0);
            CHECK_EQ(int(written[64]), 0x33);
            std::vector<std::uint8_t> incoming(64, 0x44);
            CHECK(write(plain_end, incoming.data(), incoming.size()) == 64);
            std::vector<std::uint8_t> report;
            CHECK(plain->read_report(report, std::chrono::milliseconds(500)) == HidConnection::Read::Report);
            CHECK(report == incoming);
            // Silence: a timeout, on time.
            auto const start = std::chrono::steady_clock::now();
            CHECK(plain->read_report(report, std::chrono::milliseconds(150)) == HidConnection::Read::Timeout);
            CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(140));
            close(plain_end);
        }
        // A node the descriptor no longer calls a key is not opened.
        auth::DeviceInfo keyboard_node;
        keyboard_node.path = (dev / "hidraw0").string();
        CHECK(transport->open(keyboard_node) == nullptr);
    }

    // Keys coming and going: inotify on the device folder.
    CHECK(!transport->wait_for_change(std::chrono::milliseconds(100)));
    std::thread plug([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        write_file(sys / "hidraw4/device/report_descriptor", from_hex(fido_descriptor));
        mkfifo((dev / "hidraw4").c_str(), 0600);
    });
    auto const start = std::chrono::steady_clock::now();
    CHECK(transport->wait_for_change(std::chrono::milliseconds(2000)));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
    plug.join();
    CHECK_EQ(transport->devices().size(), std::size_t { 3 });
    // Something else in the folder is not a key's business.
    write_text(dev / "tty99", "");
    CHECK(!transport->wait_for_change(std::chrono::milliseconds(150)));
    fs::remove(dev / "hidraw4");
    fs::remove_all(sys / "hidraw4");
    CHECK(transport->wait_for_change(std::chrono::milliseconds(1000)));
    CHECK_EQ(transport->devices().size(), std::size_t { 2 });

    // The confinement's rule for a key's node (platform/Confine.h): in a
    // child, writes kept to one folder and the hidraw1 stand-in. The node
    // opens for writing; its neighbor in the same folder does not, and
    // nothing can be made there.
    {
        fs::path const kept = root / "kept";
        fs::create_directories(kept);
        write_text(dev / "neighbor", "x");
        pid_t const child = fork();
        if (child == 0) {
            std::string why_not;
            if (!platform::keep_writes_to({ kept.string() }, { (dev / "hidraw1").string(), (dev / "gone-already").string() }, &why_not))
                _exit(why_not.find("Landlock") != std::string::npos ? 77 : 10);
            int const node = open((dev / "hidraw1").c_str(), O_RDWR | O_NONBLOCK);
            if (node < 0)
                _exit(11);
            close(node);
            if (open((dev / "neighbor").c_str(), O_WRONLY) >= 0)
                _exit(12);
            if (open((dev / "new-file").c_str(), O_WRONLY | O_CREAT, 0600) >= 0)
                _exit(13);
            if (open((kept / "new-file").c_str(), O_WRONLY | O_CREAT, 0600) < 0)
                _exit(14);
            _exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        int const code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (code == 77)
            std::cout << "the kernel has no Landlock: the device rule is not checked here\n";
        else
            CHECK_EQ(code, 0);
    }

    // A folder that is not there: no keys, no crash.
    auto missing = platform::hidraw_transport((root / "nowhere").string(), (root / "nowhere").string());
    CHECK(missing->devices().empty());
    CHECK(missing->wait_for_change(std::chrono::milliseconds(10)));

    fs::remove_all(root);
}

#endif

}

int main()
{
    descriptors();
#if defined(__linux__)
    hidraw_tree();
#endif
    return sashfold::test::report("test_auth_hidraw");
}
