#include "core/Bitmap.h"
#include "core/Png.h"
#include "css/StyleResolver.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "paint/Painter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Renders every page in the pages directory and compares the PNG bytes with
// the committed golden. The renderer is deterministic by construction, so
// goldens are byte-identical on every OS and compiler — a mismatch is a real
// change. Run with --update to re-bless after an intentional one.

using namespace sashfold;

namespace {

std::optional<std::vector<std::uint8_t>> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::vector<std::uint8_t> data;
    std::ostringstream stream;
    stream << file.rdbuf();
    std::string const text = std::move(stream).str();
    data.assign(text.begin(), text.end());
    return data;
}

std::vector<std::uint8_t> render_page_bytes(std::filesystem::path const& path, int width)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream stream;
    stream << file.rdbuf();
    auto document = html::parse_document_bytes(std::move(stream).str());
    css::StyleMap const styles = css::resolve_styles(*document);
    layout::LayoutResult const page = layout::layout_document(*document, styles,
        static_cast<float>(width));
    int const height = std::max(1, static_cast<int>(page.page_height + 0.5f));
    Bitmap canvas(width, height, page.canvas_background);
    paint::paint_page(canvas, page);
    return encode_png(canvas);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: reftest <pages-dir> <goldens-dir> [--update]\n";
        return 2;
    }
    std::filesystem::path const pages_dir = argv[1];
    std::filesystem::path const goldens_dir = argv[2];
    bool const update = argc > 3 && std::string_view(argv[3]) == "--update";

    std::vector<std::filesystem::path> pages;
    for (auto const& entry : std::filesystem::directory_iterator(pages_dir)) {
        if (entry.path().extension() == ".html")
            pages.push_back(entry.path());
    }
    std::sort(pages.begin(), pages.end());
    if (pages.empty()) {
        std::cerr << "no pages found in " << pages_dir << "\n";
        return 2;
    }

    int failures = 0;
    for (std::filesystem::path const& page : pages) {
        std::vector<std::uint8_t> const actual = render_page_bytes(page, 800);
        std::filesystem::path golden_path = goldens_dir / page.filename();
        golden_path.replace_extension(".png");

        if (update) {
            std::ofstream out(golden_path, std::ios::binary);
            out.write(reinterpret_cast<char const*>(actual.data()),
                static_cast<std::streamsize>(actual.size()));
            std::cout << "blessed " << golden_path.filename().string() << " (" << actual.size()
                      << " bytes)\n";
            continue;
        }

        std::optional<std::vector<std::uint8_t>> const golden = read_file(golden_path);
        if (!golden) {
            std::cerr << "MISSING GOLDEN " << golden_path.string() << " (run with --update)\n";
            ++failures;
            continue;
        }
        if (actual == *golden) {
            std::cout << "PASS " << page.filename().string() << "\n";
            continue;
        }
        ++failures;
        std::filesystem::path const actual_path
            = std::filesystem::path("reftest-actual") / golden_path.filename();
        std::filesystem::create_directories(actual_path.parent_path());
        std::ofstream out(actual_path, std::ios::binary);
        out.write(reinterpret_cast<char const*>(actual.data()),
            static_cast<std::streamsize>(actual.size()));
        std::cerr << "FAIL " << page.filename().string() << " — bytes differ; actual written to "
                  << actual_path.string() << "\n";
    }

    if (failures != 0) {
        std::cerr << failures << " reftest(s) failed\n";
        return 1;
    }
    std::cout << "all " << pages.size() << " reftests byte-identical\n";
    return 0;
}
