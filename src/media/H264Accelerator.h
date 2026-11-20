#pragma once

// H.264 decoded by the machine's video hardware, as every browser decodes
// it where there is some: the decoder is handed each picture's slices with
// what their headers say (media/H264.h) and the decoded picture buffer's
// references by slot, and reads the slice data itself. On Linux there are
// two ways to it, Vulkan Video first, then VA-API (media/Vp9Accelerator.h
// says why in that order), each loaded at run time.
//
// The buffer's bookkeeping — which slot holds which reference, the marking
// of 8.2.5, the order counts — is the host's (media/H264Decoder.h); the
// accelerator keeps a picture per slot and decodes into the one it is
// told, reading the references it is given.

#include "media/H264.h"
#include "media/Vp9Accelerator.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media {

// One reference picture the buffer holds, as the decoder names it: a
// frame, or the fields of one (8.2.5: each field of a frame store is
// marked on its own when the stream codes fields).
struct H264Reference {
    int slot = -1;
    bool long_term = false; // both fields long-term
    int frame_num = 0; // FrameNum of a short-term reference, LongTermFrameIdx of a long-term one
    int poc_top = 0;
    int poc_bottom = 0;
    // Per field, top then bottom: 0 not a reference (or not decoded), 1
    // short-term, 2 long-term. Both 0: as long_term says, for both fields.
    std::array<std::uint8_t, 2> marking { 0, 0 };
    int short_frame_num = 0; // FrameNum, where a field is short-term
    int long_term_frame_idx = 0; // LongTermFrameIdx, where a field is long-term
    std::uint8_t fields = 3; // the fields decoded: 1 top, 2 bottom, 3 both
};

// One picture to decode: its slices (NAL units, the header byte first),
// what the first slice's header says of it, and the buffer around it.
struct H264PictureInfo {
    H264Sps const* sps = nullptr;
    H264Pps const* pps = nullptr;
    std::vector<std::span<std::uint8_t const>> slices;
    std::vector<H264SliceHeader> headers; // one per slice, in the same order
    H264SliceHeader const* first_slice = nullptr;
    int poc_top = 0;
    int poc_bottom = 0;
    int frame_num = 0;
    // A field (field_pic_flag) and which; the second field of a frame is
    // decoded into the slot that holds its first.
    bool field_pic = false;
    bool bottom_field = false;
    bool second_field = false;
    bool idr = false;
    bool is_reference = false;
    bool long_term = false; // kept as a long-term reference (an IDR's flag, or MMCO 6)
    int long_term_frame_idx = 0;
    bool intra = true; // every slice an I slice
    int slot = -1; // where the picture goes
    std::vector<H264Reference> references;
};

// A reference as a slice's lists see it: its slot and its numbers for the
// picture being decoded (8.2.4.1).
struct H264ListPicture {
    int slot = -1;
    bool long_term = false;
    int frame_num = 0; // FrameNum, or LongTermFrameIdx
    int pic_num = 0; // PicNum (FrameNumWrap) of a short-term frame, LongTermPicNum of a long-term one
    int poc = 0; // the frame's PicOrderCnt, or the field's
    int poc_top = 0;
    int poc_bottom = 0;
    // What the entry is: 0 the frame (or complementary field pair), 1 its
    // top field, 2 its bottom field.
    int structure = 0;
    // As H264Reference has them, per field.
    std::array<std::uint8_t, 2> marking { 0, 0 };
    int frame_num_wrap = 0;
    int long_term_frame_idx = 0;
    std::uint8_t fields = 3; // the fields decoded: 1 top, 2 bottom, 3 both
};

// 8.2.4.1: the picture's references with their numbers, in the order
// given, as frames (each field's marking kept beside).
std::vector<H264ListPicture> h264_list_pictures(H264PictureInfo const& picture, int max_frame_num);

// 8.2.4.2 and 8.2.4.3: a slice's reference list `list` (0 or 1) — the
// initial order (P: short-term by descending PicNum, then long-term by
// ascending LongTermPicNum; B: by order count around the current
// picture; a field's lists alternating the fields' parities, 8.2.4.2.5),
// then the modifications its header carries, cut to the active count.
// A frame's list holds frames; a field's (header.field_pic) the fields
// of the frames, `current_poc` then being the current field's.
std::vector<H264ListPicture> h264_reference_list(std::vector<H264ListPicture> const& references, H264SliceHeader const& header,
    std::size_t list, bool is_b, int current_poc, int max_frame_num, int current_frame_num);

class H264Accelerator {
public:
    // The hardware decoder for H.264 at 8 bits, 4:2:0, progressive, or
    // null with why in `error`.
    static std::unique_ptr<H264Accelerator> open(std::string& error, VideoApi = VideoApi::Any);
    virtual ~H264Accelerator() = default;

    // What decodes: the device and its driver, for the media trace.
    virtual std::string const& device() const = 0;
    // The processor's decoder (media/H264Software.h), not the hardware's.
    virtual bool is_software() const { return false; }
    // A driver known to predict from the wrong picture once a long-term
    // reference is held, or after frame_num wraps with more than one
    // reference (NVIDIA's Vulkan Video: ITU conformance streams MR1_BT_A,
    // CABA3_TOSHIBA_E, which ffmpeg's Vulkan path decodes wrong too).
    virtual bool mishandles_long_term_and_wrap() const { return false; }
    // How many pictures the buffer holds (slots 0 to slots() - 1).
    virtual int slots() const = 0;
    // The parameter sets the stream uses, all of them, given again whole
    // whenever one changes.
    virtual bool set_parameter_sets(std::vector<H264Sps> const& sps, std::vector<H264Pps> const& pps) = 0;
    // Decodes one picture into its slot. False when the hardware refuses it.
    virtual bool decode(H264PictureInfo const& picture) = 0;
    // A slot's picture, read back and cropped as the sequence says.
    virtual std::optional<Nv12Picture> read(int slot) = 0;
    // Forgets the pictures, as at a new stream.
    virtual void reset() = 0;
};

}
