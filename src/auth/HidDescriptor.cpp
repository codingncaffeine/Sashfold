#include "auth/HidDescriptor.h"

#include <array>
#include <vector>

namespace sashfold::auth {

namespace {

struct Globals {
    std::uint32_t usage_page = 0;
    std::uint32_t report_size = 0; // bits
    std::uint32_t report_count = 0;
    std::uint8_t report_id = 0;
};

// The largest report the HID class lets a device describe is far below
// this; a total past it is a malformed (or hostile) descriptor.
constexpr std::uint64_t most_bits = 1u << 20;

}

std::optional<FidoReportLayout> parse_fido_report_descriptor(std::span<std::uint8_t const> descriptor)
{
    Globals globals;
    std::vector<Globals> stack;
    std::vector<std::uint32_t> usages; // local usages, extended to 32 bits with the page
    int depth = 0;
    bool in_fido = false; // inside the FIDO application collection
    bool fido_found = false;
    bool numbered = false;
    std::array<std::uint64_t, 256> input_bits {};
    std::array<std::uint64_t, 256> output_bits {};

    std::size_t offset = 0;
    while (offset < descriptor.size()) {
        std::uint8_t const prefix = descriptor[offset++];
        if (prefix == 0xfe) {
            // A long item (§6.2.2.3): its data size, its tag, its data.
            if (descriptor.size() - offset < 2)
                return std::nullopt;
            std::size_t const size = descriptor[offset];
            offset += 2;
            if (descriptor.size() - offset < size)
                return std::nullopt;
            offset += size;
            continue;
        }
        std::size_t const size = (prefix & 0x03) == 3 ? 4 : (prefix & 0x03);
        if (descriptor.size() - offset < size)
            return std::nullopt;
        std::uint32_t data = 0;
        for (std::size_t i = 0; i < size; ++i)
            data |= std::uint32_t { descriptor[offset + i] } << (8 * i);
        offset += size;
        std::uint8_t const type = (prefix >> 2) & 0x03;
        std::uint8_t const tag = prefix >> 4;

        if (type == 1) { // global (§6.2.2.7)
            switch (tag) {
            case 0x0:
                globals.usage_page = data;
                break;
            case 0x7:
                globals.report_size = data;
                break;
            case 0x8:
                if (data == 0 || data > 0xff)
                    return std::nullopt; // report ID 0 is reserved
                globals.report_id = static_cast<std::uint8_t>(data);
                numbered = true;
                break;
            case 0x9:
                globals.report_count = data;
                break;
            case 0xa: // push
                if (stack.size() >= 16)
                    return std::nullopt;
                stack.push_back(globals);
                break;
            case 0xb: // pop
                if (stack.empty())
                    return std::nullopt;
                globals = stack.back();
                stack.pop_back();
                break;
            default:
                break; // logical and physical extents, units: not needed here
            }
            continue;
        }
        if (type == 2) { // local (§6.2.2.8)
            if (tag == 0x0) {
                // A 4-byte usage names its page in the high half.
                std::uint32_t const usage = size == 4 ? data : (globals.usage_page << 16 | (data & 0xffff));
                if (usages.size() < 256)
                    usages.push_back(usage);
            }
            continue;
        }
        if (type == 0) { // main (§6.2.2.4)
            switch (tag) {
            case 0xa: { // collection
                std::uint8_t const kind = static_cast<std::uint8_t>(data);
                if (depth == 0 && kind == 0x01 && !usages.empty()
                    && usages.front() == (std::uint32_t { fido_usage_page } << 16 | fido_usage_ctaphid)) {
                    in_fido = true;
                    fido_found = true;
                }
                ++depth;
                if (depth > 32)
                    return std::nullopt;
                break;
            }
            case 0xc: // end collection
                if (depth == 0)
                    return std::nullopt;
                --depth;
                if (depth == 0)
                    in_fido = false;
                break;
            case 0x8: // input
            case 0x9: { // output
                std::uint64_t const bits = std::uint64_t { globals.report_size } * globals.report_count;
                if (bits > most_bits)
                    return std::nullopt;
                if (in_fido) {
                    auto& totals = tag == 0x8 ? input_bits : output_bits;
                    totals[globals.report_id] += bits;
                    if (totals[globals.report_id] > most_bits)
                        return std::nullopt;
                }
                break;
            }
            default:
                break; // feature
            }
            usages.clear();
            continue;
        }
        // Type 3 is reserved: skipped with its data.
    }
    if (!fido_found || depth != 0)
        return std::nullopt;

    FidoReportLayout layout;
    layout.numbered = numbered;
    auto const pick = [](std::array<std::uint64_t, 256> const& totals, std::size_t& bytes, std::uint8_t& id) {
        for (std::size_t i = 0; i < totals.size(); ++i) {
            if (totals[i] != 0) {
                if (totals[i] % 8 != 0)
                    return false;
                bytes = static_cast<std::size_t>(totals[i] / 8);
                id = static_cast<std::uint8_t>(i);
                return true;
            }
        }
        return false;
    };
    if (!pick(input_bits, layout.input_bytes, layout.input_report_id) || !pick(output_bits, layout.output_bytes, layout.output_report_id))
        return std::nullopt;
    // CTAPHID needs room for an initialization packet's 7-byte header and data.
    if (layout.input_bytes < 8 || layout.output_bytes < 8)
        return std::nullopt;
    return layout;
}

}
