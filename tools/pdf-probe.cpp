// pdf_probe: what the engine's PDF reader makes of a file.
//
//   pdf_probe [--password P] file.pdf                 the summary, pdfinfo's field names
//   pdf_probe [--password P] --object N file.pdf      object N, written back out
//   pdf_probe [--password P] --content P file.pdf     page P's (1-based) content, decoded
//
// The summary's lines are named as pdfinfo names them (Pages, Title,
// Encrypted, "Page    N size") so tools/pdf-corpus.sh can compare the two.

#include "pdf/Document.h"

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
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--password" && i + 1 < argc)
            password = argv[++i];
        else if (arg == "--object" && i + 1 < argc)
            object_num = std::atoi(argv[++i]);
        else if (arg == "--content" && i + 1 < argc)
            content_page = std::atoi(argv[++i]);
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
