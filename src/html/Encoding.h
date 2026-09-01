#pragma once

// Determining the character encoding of a byte stream (spec 13.2.3) plus the
// decoders the determination can name: BOM sniffing, the <meta> prescan, the
// WHATWG Encoding standard's label table (for the encodings we implement), and
// UTF-8 / UTF-16LE / UTF-16BE / windows-1252 decode. windows-1252 — the web's
// true default — is the final fallback.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace sashfold::html {

enum class Encoding {
    Utf8,
    Utf16Le,
    Utf16Be,
    Windows1252,
    XUserDefined, // label-table entry; decoded as windows-1252 when named by <meta>
};

// The Encoding standard's "get an encoding" for the labels we implement:
// ASCII-whitespace-trimmed, ASCII-case-insensitive. nullopt for everything else.
std::optional<Encoding> encoding_from_label(std::string_view label);

// Spec "prescan a byte stream to determine its encoding": scans up to the
// first 1024 bytes for <meta charset> / <meta http-equiv=content-type>,
// with the spec's post-rules applied (a 16-bit family label means UTF-8,
// x-user-defined means windows-1252). nullopt when nothing was found.
std::optional<Encoding> prescan_for_encoding(std::string_view bytes);

struct SniffedEncoding {
    Encoding encoding;
    std::size_t bom_length = 0; // bytes to strip before decoding
};

// BOM first (it outranks everything), then the meta prescan, then — a
// heuristic the spec sanctions for this step, and the right default for
// local files — UTF-8 when the whole stream is valid UTF-8, else the
// windows-1252 fallback.
SniffedEncoding sniff_encoding(std::string_view bytes);

// Decode with errors replaced by U+FFFD (never throws).
std::u32string decode(std::string_view bytes, Encoding encoding);

// sniff_encoding + BOM strip + decode, in one call.
std::u32string decode_document_bytes(std::string_view bytes);

}
