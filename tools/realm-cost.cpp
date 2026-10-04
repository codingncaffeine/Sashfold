// realm_cost says what the script world costs to make: an engine with the
// language alone, a further realm of it, a page's realm with every Web
// interface, a frame a page keeps, and what parsing a script file keeps.
// Each figure is time and resident memory per one made, with the heap's
// cells by kind and the lazy census (what was described, what was made at
// once) beside it — the figures tests/test_realm_lazy.cpp holds under a
// ceiling, here with their units. A build tool, never shipped.
//
//   realm_cost                          the standing figures
//   realm_cost --frames N [--rounds R] [--keep]
//                                       a page appends N iframes (and removes
//                                       them unless --keep), R times over
//   realm_cost --parse <file> ...       each file parsed as a classic script
//
// SASHFOLD_LAZY=0 gives the same figures with everything made at once.
#include "bindings/Realm.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Parser.h"
#include "js/Strings.h"
#include "net/Url.h"
#include "platform/Memory.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;

namespace {

double megabytes(std::size_t bytes)
{
    return static_cast<double>(bytes) / 1048576.0;
}

double megabytes_between(std::size_t after, std::size_t before)
{
    return (static_cast<double>(after) - static_cast<double>(before)) / 1048576.0;
}

double ms_since(std::chrono::steady_clock::time_point started)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

// The heap's live cells by kind, the kinds with the most bytes first.
void print_heap(js::Heap& heap, char const* what)
{
    heap.set_on_collect([what](js::Heap::Collection const& collection) {
        std::printf("  %s: %zu cells, %.2f MB live\n", what, collection.live_cells, megabytes(collection.live_bytes));
        std::size_t shown = 0;
        for (js::Heap::Collection::Kind const& kind : collection.live_kinds) {
            if (shown++ == 6)
                break;
            std::printf("    %-26s %6zu cells %6.2f MB  (%.0f B each)\n", kind.name.c_str(), kind.cells, megabytes(kind.bytes),
                static_cast<double>(kind.bytes) / static_cast<double>(kind.cells));
        }
    });
    heap.collect();
    heap.set_on_collect({});
}

void print_census(js::Heap const& heap)
{
    js::Heap::LazyCensus const& census = heap.lazy_census();
    std::printf("  natives described %llu, made at once %llu, made later %llu; values described %llu, made %llu; closures kept %s\n",
        static_cast<unsigned long long>(census.natives_described), static_cast<unsigned long long>(census.natives_made_at_once),
        static_cast<unsigned long long>(census.natives_made_later), static_cast<unsigned long long>(census.values_described),
        static_cast<unsigned long long>(census.values_made), js::lazy_natives() ? "in each realm's table" : "in their functions");
}

struct MadePage {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<bindings::Realm> realm;
    double clock = 1000;
};

std::unique_ptr<MadePage> make_page()
{
    auto page = std::make_unique<MadePage>();
    bindings::HostHooks hooks;
    MadePage* const raw = page.get();
    hooks.now = [raw] { return raw->clock; };
    page->realm = std::make_unique<bindings::Realm>(*page->document, *net::parse_url("https://example.test/"), std::move(hooks));
    return page;
}

void standing_figures()
{
    constexpr int count = 20;
    std::printf("mode: %s\n", js::lazy_natives() ? "made at first use" : "everything made at once (SASHFOLD_LAZY=0)");
    {
        std::vector<std::unique_ptr<js::Interpreter>> engines;
        std::size_t const resident = platform::resident_set_bytes();
        auto const started = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i)
            engines.push_back(std::make_unique<js::Interpreter>());
        double const ms = ms_since(started);
        std::printf("an engine with the language alone: %.3f ms, %.2f MB resident\n", ms / count,
            megabytes_between(platform::resident_set_bytes(), resident) / count);
        print_heap(engines[0]->heap(), "its heap");
        print_census(engines[0]->heap());
        std::size_t const resident_more = platform::resident_set_bytes();
        auto const started_more = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i)
            (void)engines[0]->create_realm();
        std::printf("a further realm of the language on it: %.3f ms, %.2f MB resident\n", ms_since(started_more) / count,
            megabytes_between(platform::resident_set_bytes(), resident_more) / count);
    }
    {
        std::vector<std::unique_ptr<MadePage>> pages;
        std::size_t const resident = platform::resident_set_bytes();
        auto const started = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i)
            pages.push_back(make_page());
        double const ms = ms_since(started);
        std::printf("a page's realm (its own engine, every Web interface): %.2f ms, %.2f MB resident\n", ms / count,
            megabytes_between(platform::resident_set_bytes(), resident) / count);
        js::Heap& heap = pages[0]->realm->interpreter().heap();
        print_heap(heap, "its heap");
        print_census(heap);
    }
}

int frames(int count, int rounds, bool keep)
{
    std::unique_ptr<MadePage> const page = make_page();
    bindings::Realm& realm = *page->realm;
    html::parse_document_bytes_into(*page->document, "<!DOCTYPE html><body></body>", &realm);
    realm.document_parsed();
    while (realm.run_pending()) {
    }
    js::Heap& heap = realm.interpreter().heap();
    std::size_t live = 0;
    heap.set_on_collect([&live](js::Heap::Collection const& collection) { live = collection.live_bytes; });
    heap.collect();
    std::size_t const resident_start = platform::resident_set_bytes();
    std::size_t const live_start = live;
    std::string const script = "for (var i = 0; i < " + std::to_string(count)
        + "; i++) { var f = document.createElement('iframe'); document.body.appendChild(f); f.contentWindow.x = i;" + (keep ? "" : " f.remove();") + " }";
    for (int round = 1; round <= rounds; ++round) {
        auto const started = std::chrono::steady_clock::now();
        js::Outcome const outcome = realm.run(script, "frames");
        double const ms = ms_since(started);
        while (realm.run_pending()) {
        }
        heap.collect();
        std::printf("round %d: %d frames %s (%s), %.2f ms each; since the start: resident %+.1f MB (%.2f MB a frame), live script heap %+.2f MB\n", round,
            count, keep ? "kept" : "made and removed", outcome.ok ? "ok" : "threw", ms / count,
            megabytes_between(platform::resident_set_bytes(), resident_start),
            megabytes_between(platform::resident_set_bytes(), resident_start) / (static_cast<double>(count) * (keep ? round : 1)),
            megabytes_between(live, live_start));
    }
    return 0;
}

int parse(std::span<char* const> files)
{
    for (char const* const path : files) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "realm_cost: cannot read %s\n", path);
            return 2;
        }
        std::string const bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        js::Interpreter interpreter;
        std::u16string source = js::utf16_from_utf8(bytes);
        std::size_t const resident = platform::resident_set_bytes();
        auto const started = std::chrono::steady_clock::now();
        std::unique_ptr<js::Program> program;
        {
            js::Parser parser(interpreter.heap(), std::move(source));
            program = parser.parse_program(path);
            if (!program) {
                std::printf("%s: syntax error: %s\n", path, parser.error() ? parser.error()->message.c_str() : "?");
                continue;
            }
        }
        double const ms = ms_since(started);
        double const kept = megabytes_between(platform::resident_set_bytes(), resident);
        double const size = megabytes(bytes.size());
        std::printf("%s: %.2f MB parsed in %.0f ms (%.1f MB/s); resident after the parse %+.1f MB = %.1f bytes a source byte; %zu scopes\n", path, size, ms,
            size / (ms / 1000.0), kept, kept / size, program->scopes().size());
    }
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc == 1) {
        standing_figures();
        return 0;
    }
    if (std::strcmp(argv[1], "--frames") == 0 && argc >= 3) {
        int rounds = 1;
        bool keep = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--keep") == 0)
                keep = true;
            else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
                rounds = std::atoi(argv[++i]);
        }
        return frames(std::atoi(argv[2]), rounds, keep);
    }
    if (std::strcmp(argv[1], "--parse") == 0 && argc >= 3)
        return parse(std::span<char* const>(argv + 2, static_cast<std::size_t>(argc - 2)));
    std::fputs("usage: realm_cost | realm_cost --frames N [--rounds R] [--keep] | realm_cost --parse <file> ...\n", stderr);
    return 2;
}
