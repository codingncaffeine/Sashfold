#pragma once
// Finding a FIDO key by its HID report descriptor (CTAP 2.1 §11.2.8.2): a
// top-level application collection of usage page 0xF1D0, usage 0x01
// (CTAPHID), whose input and output reports carry the packets. The
// descriptor is walked as the HID specification (Device Class Definition
// for HID 1.11, §6.2.2) defines its items: short items of 0, 1, 2 or 4
// bytes of data, long items skipped, global state with Push and Pop, local
// usages cleared after each main item. A descriptor that runs past its end
// or nests wrongly is refused. Every system's transport uses it; the bytes
// come from the device, so nothing is trusted.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sashfold::auth {

struct FidoReportLayout {
    std::size_t input_bytes = 0; // the packet size the device sends
    std::size_t output_bytes = 0; // the packet size it takes
    // The report IDs of the FIDO reports when the device numbers its
    // reports (0: it does not). With numbered reports every read on the
    // system's raw interface begins with the ID and every write must.
    std::uint8_t input_report_id = 0;
    std::uint8_t output_report_id = 0;
    bool numbered = false;
};

inline constexpr std::uint16_t fido_usage_page = 0xf1d0;
inline constexpr std::uint16_t fido_usage_ctaphid = 0x01;

// The FIDO reports' layout, or nullopt for a device that is not a FIDO key
// (or whose descriptor is malformed, or whose packets would be shorter
// than CTAPHID's 8-byte minimum).
std::optional<FidoReportLayout> parse_fido_report_descriptor(std::span<std::uint8_t const> descriptor);

}
