#include "core/Json.h"
#include "net/Url.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

// Scores the URL parser against WPT's urltestdata.json: parse input against
// base, compare the serialization (and origin where given) or expect failure.
// The committed baseline only ratchets upward.

using namespace sashfold;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: wpt_url <urltestdata.json> <baseline-file>\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "cannot read " << argv[1] << "\n";
        return 2;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    auto const document = JsonValue::parse(std::move(stream).str());
    if (!document || !document->is_array()) {
        std::cerr << "fixture did not parse as a JSON array\n";
        return 2;
    }

    long baseline = 0;
    {
        std::ifstream baseline_file(argv[2]);
        std::string line;
        while (std::getline(baseline_file, line)) {
            if (!line.empty() && line[0] != '#') {
                baseline = std::stol(line);
                break;
            }
        }
    }

    long total = 0;
    long passed = 0;
    int printed = 0;
    int max_printed = 8;
    if (char const* env = std::getenv("SASHFOLD_PRINT_FAILURES"))
        max_printed = std::atoi(env);

    for (JsonValue const& entry : document->as_array()) {
        if (!entry.is_object())
            continue; // comment strings
        JsonValue const* input_value = entry.get("input");
        if (!input_value || !input_value->is_string())
            continue;
        ++total;

        std::optional<net::Url> base;
        if (JsonValue const* base_value = entry.get("base");
            base_value && base_value->is_string()) {
            base = net::parse_url(base_value->as_string());
            if (!base) {
                // A base that fails to parse fails the whole case unless the
                // case expects failure anyway.
                if (entry.get("failure")) {
                    ++passed;
                } else if (printed++ < max_printed) {
                    std::cerr << "FAIL (base did not parse) base="
                              << base_value->as_string() << "\n";
                }
                continue;
            }
        }

        std::optional<net::Url> const url
            = net::parse_url(input_value->as_string(), base ? &*base : nullptr);
        bool const expect_failure = entry.get("failure") != nullptr;

        if (expect_failure) {
            if (!url) {
                ++passed;
            } else if (printed++ < max_printed) {
                std::cerr << "FAIL (expected failure) input=" << input_value->as_string()
                          << " got=" << url->serialize() << "\n";
            }
            continue;
        }
        if (!url) {
            if (printed++ < max_printed)
                std::cerr << "FAIL (unexpected failure) input=" << input_value->as_string() << "\n";
            continue;
        }

        JsonValue const* href = entry.get("href");
        std::string const serialized = url->serialize();
        bool ok = href && href->is_string() && serialized == href->as_string();
        if (ok) {
            if (JsonValue const* origin = entry.get("origin"); origin && origin->is_string())
                ok = url->serialize_origin() == origin->as_string();
        }
        if (ok) {
            ++passed;
        } else if (printed++ < max_printed) {
            std::cerr << "FAIL input=" << input_value->as_string() << "\n  expected="
                      << (href && href->is_string() ? href->as_string() : std::string("?"))
                      << "\n  actual=  " << serialized << "\n";
        }
    }

    std::cout << "TOTAL: " << passed << "/" << total << " (baseline " << baseline << ")\n";
    if (passed < baseline) {
        std::cerr << "REGRESSION: score fell below the committed baseline\n";
        return 1;
    }
    if (passed > baseline)
        std::cout << "RATCHET: raise the baseline to " << passed << "\n";
    return 0;
}
