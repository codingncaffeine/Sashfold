#pragma once

// Parametric stereo (ISO/IEC 14496-3 §8.6.4), the tool HE-AAC v2 adds on
// top of spectral band replication: a mono core carries, inside its SBR
// payload, a few parameters a band — the level difference between the two
// channels (IID) and their correlation (ICC) — and the decoder rebuilds the
// pair from the one channel and a decorrelated copy of it, mixed band by
// band in the QMF domain the SBR tool already works in. This is the
// baseline decoder (Annex 8.A): the 20-band layout (34-band parameters
// mapped onto it), QMF bands 0 to 2 split finer by the hybrid filters, the
// all-pass decorrelator with its transient ducking, and the mixing matrices
// interpolated across each envelope by either mixing procedure (A, or
// the unrestricted decoder's B); the phase parameters (IPD/OPD) a stream
// may carry in its extension are read past, and a 34-band stream is mixed
// in the 20 bands.

#include <array>
#include <complex>
#include <cstddef>
#include <memory>

namespace sashfold::media {

class SbrBits;

// One band's mixing matrix (8.6.4, stereo processing): left = h11 s + h21 d,
// right = h12 s + h22 d, from the mono signal s and its decorrelated copy d,
// for an IID and ICC index; procedure B when `procedure_b` (ICC modes 3-5).
struct PsMatrix {
    float h11 = 0, h12 = 0, h21 = 0, h22 = 0;
};
PsMatrix ps_mixing_matrix(bool procedure_b, bool fine_iid, int iid_index, int icc_index);

class PsDecoder {
public:
    using Complex = std::complex<float>;

    PsDecoder();
    ~PsDecoder();
    PsDecoder(PsDecoder const&) = delete;
    PsDecoder& operator=(PsDecoder const&) = delete;

    // Reads one ps_data() (8.3) of an SBR extension, given the bits the
    // extension has left; returns the bits it took (never more than given).
    std::size_t read(SbrBits& bits, std::size_t bits_left);

    // Whether the stream has carried parametric stereo: the element then
    // plays as two channels, from the frame it was first met in on.
    bool active() const;

    // Starts a frame's 32 columns: the parameters read for it (or the last
    // frame's, when it carried none) decoded into envelopes. `usb`: the
    // first QMF band above the SBR range, where the processing stops.
    void begin_frame(int usb);

    // One QMF column of the frame (0 to 31): `left` holds the 64 bands the
    // SBR tool assembled and becomes the left channel's; `ahead` holds QMF
    // bands 0 to 2 six columns later — the delay of the hybrid filters
    // that split them, which the other bands are not put through; `right`
    // is written with the right channel's 64 bands.
    void apply(int column, std::array<Complex, 64>& left, std::array<Complex, 3> const& ahead, std::array<Complex, 64>& right);

    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
