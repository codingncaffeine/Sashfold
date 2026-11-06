#include "pdf/Filters.h"

#include "core/Inflate.h"
#include "pdf/Parser.h"

#include <algorithm>
#include <cstdlib>

namespace sashfold::pdf {

namespace {

int hex_digit(std::uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int param_int(Dict const& params, char const* key, int fallback)
{
    Object const& value = params.get(key);
    return value.is_number() ? static_cast<int>(value.as_int(fallback)) : fallback;
}

} // namespace

std::string expand_filter_name(std::string const& name)
{
    if (name == "AHx")
        return "ASCIIHexDecode";
    if (name == "A85")
        return "ASCII85Decode";
    if (name == "LZW")
        return "LZWDecode";
    if (name == "Fl")
        return "FlateDecode";
    if (name == "RL")
        return "RunLengthDecode";
    if (name == "CCF")
        return "CCITTFaxDecode";
    if (name == "DCT")
        return "DCTDecode";
    return name;
}

bool is_image_filter(std::string const& name)
{
    std::string const full = expand_filter_name(name);
    return full == "DCTDecode" || full == "JPXDecode" || full == "JBIG2Decode" || full == "CCITTFaxDecode";
}

std::vector<std::uint8_t> ascii_hex_decode(std::vector<std::uint8_t> const& data)
{
    std::vector<std::uint8_t> out;
    out.reserve(data.size() / 2);
    int pending = -1;
    for (std::uint8_t const c : data) {
        if (c == '>')
            break;
        int const v = hex_digit(c);
        if (v < 0)
            continue;
        if (pending < 0) {
            pending = v;
        } else {
            out.push_back(static_cast<std::uint8_t>(pending * 16 + v));
            pending = -1;
        }
    }
    if (pending >= 0)
        out.push_back(static_cast<std::uint8_t>(pending * 16));
    return out;
}

std::vector<std::uint8_t> ascii85_decode(std::vector<std::uint8_t> const& data)
{
    std::vector<std::uint8_t> out;
    out.reserve(data.size() * 4 / 5);
    std::uint32_t group = 0;
    int count = 0;
    std::size_t i = 0;
    // A leading "<~" (PostScript's form) is allowed.
    while (i < data.size() && is_pdf_whitespace(data[i]))
        ++i;
    if (i + 1 < data.size() && data[i] == '<' && data[i + 1] == '~')
        i += 2;
    for (; i < data.size(); ++i) {
        std::uint8_t const c = data[i];
        if (c == '~')
            break;
        if (is_pdf_whitespace(c))
            continue;
        if (c == 'z' && count == 0) {
            out.insert(out.end(), 4, 0);
            continue;
        }
        if (c < '!' || c > 'u')
            continue;
        group = group * 85 + static_cast<std::uint32_t>(c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k)
                out.push_back(static_cast<std::uint8_t>(group >> (8 * k)));
            group = 0;
            count = 0;
        }
    }
    if (count > 1) {
        // A final partial group: padded with 'u', then count-1 bytes kept.
        for (int k = count; k < 5; ++k)
            group = group * 85 + 84;
        for (int k = 0; k < count - 1; ++k)
            out.push_back(static_cast<std::uint8_t>(group >> (8 * (3 - k))));
    }
    return out;
}

std::vector<std::uint8_t> run_length_decode(std::vector<std::uint8_t> const& data, std::size_t max_output)
{
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < data.size() && out.size() < max_output) {
        std::uint8_t const length = data[i++];
        if (length == 128)
            break;
        if (length < 128) {
            std::size_t const n = std::min<std::size_t>(length + 1u, data.size() - i);
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i),
                data.begin() + static_cast<std::ptrdiff_t>(i + n));
            i += n;
        } else {
            if (i >= data.size())
                break;
            out.insert(out.end(), 257u - length, data[i++]);
        }
    }
    if (out.size() > max_output)
        out.resize(max_output);
    return out;
}

std::vector<std::uint8_t> lzw_decode(std::vector<std::uint8_t> const& data, bool early_change, std::size_t max_output)
{
    std::vector<std::uint8_t> out;
    // The table as (prefix code, last byte, length) so a code's bytes are
    // written by walking back; codes 0-255 are the bytes, 256 clear, 257 end.
    struct Entry {
        int prefix = -1;
        std::uint8_t byte = 0;
        int length = 1;
    };
    std::vector<Entry> table(4096);
    for (int i = 0; i < 256; ++i)
        table[static_cast<std::size_t>(i)] = Entry { -1, static_cast<std::uint8_t>(i), 1 };
    int next_code = 258;
    int code_bits = 9;
    int previous = -1;
    std::uint32_t buffer = 0;
    int buffered = 0;
    std::size_t at = 0;
    std::vector<std::uint8_t> scratch;
    auto const write_code = [&](int code) {
        Entry const& entry = table[static_cast<std::size_t>(code)];
        scratch.resize(static_cast<std::size_t>(entry.length));
        int c = code;
        for (int k = entry.length - 1; k >= 0 && c >= 0; --k) {
            scratch[static_cast<std::size_t>(k)] = table[static_cast<std::size_t>(c)].byte;
            c = table[static_cast<std::size_t>(c)].prefix;
        }
        out.insert(out.end(), scratch.begin(), scratch.end());
        return scratch[0];
    };
    while (out.size() < max_output) {
        while (buffered < code_bits && at < data.size()) {
            buffer = (buffer << 8) | data[at++];
            buffered += 8;
        }
        if (buffered < code_bits)
            break;
        int const code = static_cast<int>((buffer >> (buffered - code_bits)) & ((1u << code_bits) - 1));
        buffered -= code_bits;
        if (code == 256) {
            next_code = 258;
            code_bits = 9;
            previous = -1;
            continue;
        }
        if (code == 257)
            break;
        if (previous < 0) {
            if (code > 255)
                break;
            write_code(code);
            previous = code;
            continue;
        }
        std::uint8_t first = 0;
        if (code < next_code) {
            first = write_code(code);
        } else if (code == next_code) {
            // KwKwK: the previous string plus its own first byte.
            std::uint8_t const prev_first = write_code(previous);
            out.push_back(prev_first);
            first = prev_first;
        } else {
            break;
        }
        if (next_code < 4096) {
            table[static_cast<std::size_t>(next_code)] =
                Entry { previous, first, table[static_cast<std::size_t>(previous)].length + 1 };
            ++next_code;
        }
        int const limit = early_change ? next_code + 1 : next_code;
        if (limit >= (1 << code_bits) && code_bits < 12)
            ++code_bits;
        previous = code;
    }
    if (out.size() > max_output)
        out.resize(max_output);
    return out;
}

std::vector<std::uint8_t> unpredict(std::vector<std::uint8_t> data, Dict const& params)
{
    int const predictor = param_int(params, "Predictor", 1);
    if (predictor <= 1)
        return data;
    int const colors = std::clamp(param_int(params, "Colors", 1), 1, 32);
    int const bpc = param_int(params, "BitsPerComponent", 8);
    int const columns = std::clamp(param_int(params, "Columns", 1), 1, 1 << 24);
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)
        return data;
    std::size_t const bits_per_pixel = static_cast<std::size_t>(colors) * static_cast<std::size_t>(bpc);
    std::size_t const row_bytes = (bits_per_pixel * static_cast<std::size_t>(columns) + 7) / 8;
    std::size_t const pixel_bytes = std::max<std::size_t>(1, bits_per_pixel / 8);
    if (predictor == 2) {
        // TIFF: each component the difference from the same component one pixel left.
        for (std::size_t row = 0; row + row_bytes <= data.size(); row += row_bytes) {
            std::uint8_t* const line = data.data() + row;
            if (bpc == 8) {
                for (std::size_t i = pixel_bytes; i < row_bytes; ++i)
                    line[i] = static_cast<std::uint8_t>(line[i] + line[i - pixel_bytes]);
            } else if (bpc == 16) {
                for (std::size_t i = pixel_bytes; i + 1 < row_bytes; i += 2) {
                    unsigned const v = ((line[i] << 8) | line[i + 1]) + ((line[i - pixel_bytes] << 8) | line[i - pixel_bytes + 1]);
                    line[i] = static_cast<std::uint8_t>(v >> 8);
                    line[i + 1] = static_cast<std::uint8_t>(v);
                }
            } else {
                // Sub-byte components: unpack, add, repack.
                std::size_t const samples = static_cast<std::size_t>(columns) * static_cast<std::size_t>(colors);
                unsigned const mask = (1u << bpc) - 1;
                auto const get = [&](std::size_t s) {
                    std::size_t const bit = s * static_cast<std::size_t>(bpc);
                    return (line[bit / 8] >> (8 - bpc - static_cast<int>(bit % 8))) & mask;
                };
                auto const put = [&](std::size_t s, unsigned v) {
                    std::size_t const bit = s * static_cast<std::size_t>(bpc);
                    int const shift = 8 - bpc - static_cast<int>(bit % 8);
                    line[bit / 8] = static_cast<std::uint8_t>((line[bit / 8] & ~(mask << shift)) | ((v & mask) << shift));
                };
                for (std::size_t s = static_cast<std::size_t>(colors); s < samples; ++s)
                    put(s, get(s) + get(s - static_cast<std::size_t>(colors)));
            }
        }
        return data;
    }
    // PNG (10-15): each row begins with its own filter type byte.
    std::vector<std::uint8_t> out;
    out.reserve(data.size());
    std::vector<std::uint8_t> previous(row_bytes, 0);
    std::vector<std::uint8_t> line(row_bytes, 0);
    for (std::size_t at = 0; at < data.size();) {
        std::uint8_t const type = data[at++];
        std::size_t const n = std::min(row_bytes, data.size() - at);
        std::fill(line.begin(), line.end(), 0);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at), n, line.begin());
        at += n;
        for (std::size_t i = 0; i < row_bytes; ++i) {
            int const left = i >= pixel_bytes ? line[i - pixel_bytes] : 0;
            int const up = previous[i];
            int const up_left = i >= pixel_bytes ? previous[i - pixel_bytes] : 0;
            int add = 0;
            switch (type) {
            case 1: add = left; break;
            case 2: add = up; break;
            case 3: add = (left + up) / 2; break;
            case 4: {
                int const p = left + up - up_left;
                int const pa = std::abs(p - left);
                int const pb = std::abs(p - up);
                int const pc = std::abs(p - up_left);
                add = (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : up_left);
                break;
            }
            default: break;
            }
            line[i] = static_cast<std::uint8_t>(line[i] + add);
        }
        out.insert(out.end(), line.begin(), line.begin() + static_cast<std::ptrdiff_t>(n));
        std::swap(previous, line);
        if (n < row_bytes)
            break;
    }
    return out;
}

std::vector<std::uint8_t> apply_filter(std::string const& name_in, std::vector<std::uint8_t> const& data,
    Dict const& params, std::size_t max_output)
{
    std::string const name = expand_filter_name(name_in);
    if (name == "FlateDecode")
        return unpredict(zlib_decompress_lenient(data, max_output), params);
    if (name == "LZWDecode")
        return unpredict(lzw_decode(data, param_int(params, "EarlyChange", 1) != 0, max_output), params);
    if (name == "ASCIIHexDecode")
        return ascii_hex_decode(data);
    if (name == "ASCII85Decode")
        return ascii85_decode(data);
    if (name == "RunLengthDecode")
        return run_length_decode(data, max_output);
    if (name == "Crypt")
        return data; // the Identity crypt filter; others are applied by the document
    return {};
}

}
