#include "ui/Downloads.h"

#include "core/Ascii.h"

#include <filesystem>
#include <fstream>

namespace sashfold::ui {

namespace {

std::string_view trim_ows(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

bool starts_with_ci(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && ascii_ci_equals(text.substr(0, prefix.size()), prefix);
}

std::string percent_decode(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()
            && is_ascii_hex_digit(static_cast<unsigned char>(text[i + 1]))
            && is_ascii_hex_digit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(hex_digit_value(static_cast<unsigned char>(text[i + 1])) * 16u
                + hex_digit_value(static_cast<unsigned char>(text[i + 2])));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// One parameter of a Content-Disposition value: `name=value` or
// `name*=charset'lang'percent-encoded` (RFC 6266 / RFC 8187).
struct Parameter {
    std::string name; // lowercase, without any trailing '*'
    std::string value; // decoded
    bool extended = false;
};

std::vector<Parameter> parse_disposition_parameters(std::string_view header)
{
    std::vector<Parameter> parameters;
    std::size_t start = header.find(';');
    while (start != std::string_view::npos) {
        ++start;
        // Find the end of this parameter: the next ';' outside quotes.
        bool quoted = false;
        std::size_t end = start;
        while (end < header.size() && (quoted || header[end] != ';')) {
            if (header[end] == '"')
                quoted = !quoted;
            else if (header[end] == '\\' && quoted && end + 1 < header.size())
                ++end;
            ++end;
        }
        std::string_view const part = trim_ows(header.substr(start, end - start));
        start = end < header.size() ? end : std::string_view::npos;
        std::size_t const equals = part.find('=');
        if (equals == std::string_view::npos)
            continue;
        Parameter parameter;
        std::string_view name = trim_ows(part.substr(0, equals));
        if (!name.empty() && name.back() == '*') {
            parameter.extended = true;
            name.remove_suffix(1);
        }
        for (char const c : name)
            parameter.name += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
        std::string_view value = trim_ows(part.substr(equals + 1));
        if (parameter.extended) {
            // charset'language'value — only the value matters to us.
            std::size_t const first = value.find('\'');
            std::size_t const second = first == std::string_view::npos ? first : value.find('\'', first + 1);
            if (second != std::string_view::npos)
                value = value.substr(second + 1);
            parameter.value = percent_decode(value);
        } else if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            for (std::size_t i = 1; i + 1 < value.size(); ++i) {
                if (value[i] == '\\' && i + 2 < value.size())
                    ++i;
                parameter.value += value[i];
            }
        } else {
            parameter.value = std::string(value);
        }
        parameters.push_back(std::move(parameter));
    }
    return parameters;
}

// One path component, never a path: separators, control characters, and
// the names Windows reserves are replaced or suffixed.
std::string sanitize_file_name(std::string name)
{
    std::string out;
    for (char const c : name) {
        unsigned char const byte = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<'
            || c == '>' || c == '|' || byte < 0x20 || byte == 0x7F)
            out += '_';
        else
            out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(0, 1);
    if (out.empty() || out == "." || out == "..")
        return "download";
    static constexpr std::string_view reserved[] = { "con", "prn", "aux", "nul", "com1", "com2",
        "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
        "lpt5", "lpt6", "lpt7", "lpt8", "lpt9" };
    std::string const stem = out.substr(0, out.find('.'));
    for (std::string_view const name_reserved : reserved) {
        if (ascii_ci_equals(stem, name_reserved))
            return out + "_";
    }
    if (out.size() > 200)
        out.resize(200);
    return out;
}

} // namespace

bool is_renderable_content_type(std::string const& content_type)
{
    std::string_view const type = trim_ows(content_type);
    if (type.empty())
        return true; // no type: the sniffless default is to try it as a page
    return starts_with_ci(type, "text/") || starts_with_ci(type, "application/xhtml+xml")
        || starts_with_ci(type, "application/json") || starts_with_ci(type, "application/xml")
        || starts_with_ci(type, "image/svg+xml") || starts_with_ci(type, "application/javascript")
        || starts_with_ci(type, "application/ecmascript");
}

std::string download_file_name(std::string const* content_disposition, net::Url const& url)
{
    if (content_disposition) {
        std::vector<Parameter> const parameters = parse_disposition_parameters(*content_disposition);
        for (Parameter const& parameter : parameters) {
            if (parameter.name == "filename" && parameter.extended && !parameter.value.empty())
                return sanitize_file_name(parameter.value);
        }
        for (Parameter const& parameter : parameters) {
            if (parameter.name == "filename" && !parameter.value.empty())
                return sanitize_file_name(parameter.value);
        }
    }
    if (!url.has_opaque_path && !url.path.empty() && !url.path.back().empty())
        return sanitize_file_name(percent_decode(url.path.back()));
    return "download";
}

DownloadResult save_download(std::string const& directory, std::string const& file_name,
    std::vector<std::uint8_t> const& bytes, net::Url const& source, std::string const& referrer)
{
    DownloadResult result;
    std::error_code error;
    std::filesystem::path const folder(directory);
    std::filesystem::create_directories(folder, error);
    if (!std::filesystem::is_directory(folder, error)) {
        result.error = "no downloads folder at " + directory;
        return result;
    }

    std::filesystem::path const wanted(file_name);
    std::string const stem = wanted.stem().string();
    std::string const extension = wanted.extension().string();
    std::filesystem::path target;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        std::string candidate = attempt == 0 ? stem : stem + " (" + std::to_string(attempt) + ")";
        candidate += extension;
        target = folder / candidate;
        if (!std::filesystem::exists(target, error))
            break;
    }
    if (std::filesystem::exists(target, error)) {
        result.error = "too many files named " + file_name + " in " + directory;
        return result;
    }

    {
        std::ofstream file(target, std::ios::binary);
        if (!file) {
            result.error = "cannot write " + target.string();
            return result;
        }
        file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            result.error = "write failed for " + target.string();
            return result;
        }
    }
    result.path = target.string();
    result.file_name = target.filename().string();

#ifdef _WIN32
    // The mark of the web: NTFS's Zone.Identifier alternate data stream, the
    // same one every browser writes, so Explorer and SmartScreen treat the
    // file as downloaded from the Internet zone.
    std::ofstream zone(target.string() + ":Zone.Identifier", std::ios::binary);
    if (zone) {
        std::string record = "[ZoneTransfer]\r\nZoneId=3\r\n";
        if (!referrer.empty())
            record += "ReferrerUrl=" + referrer + "\r\n";
        record += "HostUrl=" + source.serialize() + "\r\n";
        zone.write(record.data(), static_cast<std::streamsize>(record.size()));
        result.marked = static_cast<bool>(zone);
    }
#else
    (void)source;
    (void)referrer;
    // Linux (user.xdg.origin.url) and macOS (com.apple.quarantine) marks arrive
    // with their shells.
#endif
    return result;
}

}
