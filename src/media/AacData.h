#pragma once

// The tables of ISO/IEC 14496-3 an AAC decoder reads, written by
// tools/gen-aac-tables.py (AacData.cpp): the Huffman codebooks of the
// spectrum and the scalefactors, and the scalefactor bands by sampling rate.

#include <cstddef>
#include <cstdint>

namespace sashfold::media::aac_data {

struct HuffmanEntry {
    std::uint16_t index; // the codebook's index: the values packed, or the scalefactor's offset
    std::uint8_t length; // bits
    std::uint32_t code;
};

struct HuffmanBook {
    int dimension; // values a codeword stands for: 4, 2 (or 1 for the scalefactors')
    int bits; // per value, in the index
    int offset; // taken off each value (0 for an unsigned codebook)
    HuffmanEntry const* entries;
    std::size_t count;
};

// books[0] is codebook 1 ... books[10] codebook 11, books[11] the scalefactor codebook.
extern HuffmanBook const books[12];

struct BandTable {
    std::uint32_t sampling_rate;
    std::uint16_t const* long_offsets; // long_bands + 1 offsets, 0 to 1024
    int long_bands;
    std::uint16_t const* short_offsets; // short_bands + 1 offsets, 0 to 128
    int short_bands;
    int tns_max_long; // TNS reaches no higher than this band (Table 4.139)
    int tns_max_short;
};
extern BandTable const band_tables[];
extern std::size_t const band_table_count;

}
