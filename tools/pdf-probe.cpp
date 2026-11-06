// pdf_probe: what the engine's PDF reader makes of a file.
//
//   pdf_probe [--password P] file.pdf                 the summary, pdfinfo's field names
//   pdf_probe [--password P] --object N file.pdf      object N, written back out
//   pdf_probe [--password P] --content P file.pdf     page P's (1-based) content, decoded
//   pdf_probe [--password P] --render P --dpi D --out page.png [--compare ref.png] file.pdf
//       page P drawn at D dots per inch (72 = one pixel per point); with
//       --compare (repeatable), how far it is from other pictures of the page
//       (poppler's and Ghostscript's at the same dpi): the mean channel
//       difference and the share of pixels off by more than 32 in any channel
//
// The summary's lines are named as pdfinfo names them (Pages, Title,
// Encrypted, "Page    N size") so tools/pdf-corpus.sh can compare the two.

#include "core/Png.h"
#include "pdf/Document.h"
#include "pdf/Render.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

void write_object(std::ostream& out, pdf::Object const& object, int depth)
{
    if (depth > 12) {
        out << "...";
        return;
    }
    if (object.is_null()) {
        out << "null";
    } else if (object.is_bool()) {
        out << (object.as_bool() ? "true" : "false");
    } else if (object.is_int()) {
        out << object.as_int();
    } else if (object.is_number()) {
        out << object.as_number();
    } else if (object.is_string()) {
        out << '(';
        for (char const c : object.as_string()) {
            auto const u = static_cast<unsigned char>(c);
            if (u < 32 || u > 126 || c == '(' || c == ')' || c == '\\') {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\%03o", u);
                out << buffer;
            } else {
                out << c;
            }
        }
        out << ')';
    } else if (object.is_name()) {
        out << '/' << object.as_name();
    } else if (object.is_array()) {
        out << '[';
        bool first = true;
        for (pdf::Object const& item : object.as_array()) {
            if (!first)
                out << ' ';
            first = false;
            write_object(out, item, depth + 1);
        }
        out << ']';
    } else if (object.is_ref()) {
        out << object.as_ref().num << ' ' << object.as_ref().gen << " R";
    } else {
        out << "<<";
        for (auto const& [key, value] : object.as_dict().entries()) {
            out << " /" << key << ' ';
            write_object(out, value, depth + 1);
        }
        out << " >>";
        if (object.is_stream())
            out << " stream(" << object.as_stream()->length << " bytes)";
    }
}

std::string format_size(double v)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", v);
    return buffer;
}

} // namespace

int main(int argc, char** argv)
{
    std::string password;
    std::string path;
    int object_num = -1;
    int content_page = -1;
    int render_page_number = -1;
    double dpi = 72;
    std::string out_path;
    std::vector<std::string> compare_paths;
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--password" && i + 1 < argc)
            password = argv[++i];
        else if (arg == "--object" && i + 1 < argc)
            object_num = std::atoi(argv[++i]);
        else if (arg == "--content" && i + 1 < argc)
            content_page = std::atoi(argv[++i]);
        else if (arg == "--render" && i + 1 < argc)
            render_page_number = std::atoi(argv[++i]);
        else if (arg == "--dpi" && i + 1 < argc)
            dpi = std::atof(argv[++i]);
        else if (arg == "--out" && i + 1 < argc)
            out_path = argv[++i];
        else if (arg == "--compare" && i + 1 < argc)
            compare_paths.push_back(argv[++i]);
        else
            path = arg;
    }
    if (path.empty()) {
        std::cerr << "usage: pdf_probe [--password P] [--object N | --content PAGE] file.pdf\n";
        return 2;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot read " << path << "\n";
        return 2;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    pdf::Document::Status status = pdf::Document::Status::Ok;
    std::shared_ptr<pdf::Document> const doc = pdf::Document::open(std::move(bytes), password, status);
    char const* const status_names[] = { "ok", "not a PDF", "needs a password", "unsupported", "damaged" };
    if (!doc) {
        std::cout << "Error: " << status_names[static_cast<int>(status)] << "\n";
        return 1;
    }
    if (object_num >= 0) {
        pdf::Object const object = doc->fetch(pdf::Ref { object_num, 0 });
        write_object(std::cout, object, 0);
        std::cout << "\n";
        if (pdf::Stream const* stream = object.as_stream()) {
            std::vector<std::uint8_t> const data = doc->decode(*stream);
            std::cout << "decoded " << data.size() << " bytes\n";
        }
        return 0;
    }
    if (render_page_number >= 1) {
        if (static_cast<std::size_t>(render_page_number) > doc->page_count())
            return 1;
        pdf::RenderOptions options;
        options.scale = dpi / 72.0;
        auto const start = std::chrono::steady_clock::now();
        std::optional<Bitmap> const picture = pdf::render_page(*doc, static_cast<std::size_t>(render_page_number - 1), options);
        double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!picture) {
            std::cout << "render failed\n";
            return 1;
        }
        if (!out_path.empty() && !write_png(out_path, *picture)) {
            std::cerr << "cannot write " << out_path << "\n";
            return 2;
        }
        std::cout << "rendered " << picture->width() << "x" << picture->height() << " in " << ms << " ms\n";
        for (std::string const& compare_path : compare_paths) {
            std::ifstream ref_file(compare_path, std::ios::binary);
            std::vector<std::uint8_t> const ref_bytes((std::istreambuf_iterator<char>(ref_file)), std::istreambuf_iterator<char>());
            std::optional<Bitmap> const reference = decode_png(ref_bytes);
            if (!reference) {
                std::cerr << "cannot read " << compare_path << "\n";
                return 2;
            }
            // Sizes may differ by a pixel (rounding of the page box): the overlap is compared.
            int const w = std::min(reference->width(), picture->width());
            int const h = std::min(reference->height(), picture->height());
            double sum = 0;
            std::size_t off = 0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    Color const a = picture->pixel(x, y);
                    Color const b = reference->pixel(x, y);
                    int const dr = std::abs(a.r - b.r);
                    int const dg = std::abs(a.g - b.g);
                    int const db = std::abs(a.b - b.b);
                    sum += dr + dg + db;
                    if (dr > 32 || dg > 32 || db > 32)
                        ++off;
                }
            }
            double const pixels = static_cast<double>(w) * h;
            std::printf("vs %s %dx%d: mean %.3f, off %.3f%%\n", compare_path.substr(compare_path.rfind('.', compare_path.size() - 5) + 1).c_str(),
                reference->width(), reference->height(), sum / (pixels * 3), 100.0 * static_cast<double>(off) / pixels);
        }
        return 0;
    }
    if (content_page >= 1) {
        if (static_cast<std::size_t>(content_page) > doc->page_count())
            return 1;
        pdf::Page const& page = doc->page(static_cast<std::size_t>(content_page - 1));
        pdf::Object const contents = doc->resolve(page.dict->get("Contents"));
        std::vector<pdf::Object> parts;
        if (contents.is_array()) {
            for (pdf::Object const& part : contents.as_array())
                parts.push_back(doc->resolve(part));
        } else {
            parts.push_back(contents);
        }
        for (pdf::Object const& part : parts) {
            if (pdf::Stream const* stream = part.as_stream()) {
                std::vector<std::uint8_t> const data = doc->decode(*stream);
                std::cout.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
                std::cout << "\n";
            }
        }
        return 0;
    }
    std::cout << "Title:          " << doc->title() << "\n";
    std::cout << "Producer:       " << doc->info_text("Producer") << "\n";
    std::cout << "Pages:          " << doc->page_count() << "\n";
    std::cout << "Encrypted:      " << (doc->encrypted() ? "yes" : "no") << "\n";
    std::cout << "PDF version:    " << doc->version() << "\n";
    std::cout << "Rebuilt:        " << (doc->repaired() ? "yes" : "no") << "\n";
    std::cout << "Status:         " << status_names[static_cast<int>(status)] << "\n";
    for (std::size_t i = 0; i < doc->page_count(); ++i) {
        pdf::Page const& page = doc->page(i);
        std::cout << "Page " << (i + 1) << " size: " << format_size(page.crop_box.width()) << " x "
                  << format_size(page.crop_box.height()) << " pts rot " << page.rotate << "\n";
    }
    return 0;
}
