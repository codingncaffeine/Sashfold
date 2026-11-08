/* sbr-encode: 16-bit PCM to HE-AAC (AAC-LC with spectral band replication)
 * in ADTS, through the Fraunhofer FDK AAC encoder library — an encoder
 * independent of ours, for the fixtures tools/sbr-make-tests.sh writes.
 * Not part of the build: the script compiles it where libfdk-aac is present.
 *
 *   sbr-encode <in.s16> <rate> <channels> <bitrate> <out.adts>
 */
#include <fdk-aac/aacenc_lib.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: sbr-encode <in.s16> <rate> <channels> <bitrate> <out.adts>\n");
        return 2;
    }
    int const rate = atoi(argv[2]);
    int const channels = atoi(argv[3]);
    int const bitrate = atoi(argv[4]);
    FILE* in = fopen(argv[1], "rb");
    FILE* out = fopen(argv[5], "wb");
    if (!in || !out) {
        fprintf(stderr, "sbr-encode: cannot open the files\n");
        return 1;
    }
    HANDLE_AACENCODER encoder;
    if (aacEncOpen(&encoder, 0, (UINT)channels) != AACENC_OK)
        return 1;
    if (aacEncoder_SetParam(encoder, AACENC_AOT, AOT_SBR) != AACENC_OK
        || aacEncoder_SetParam(encoder, AACENC_SAMPLERATE, (UINT)rate) != AACENC_OK
        || aacEncoder_SetParam(encoder, AACENC_CHANNELMODE, channels == 1 ? MODE_1 : MODE_2) != AACENC_OK
        || aacEncoder_SetParam(encoder, AACENC_CHANNELORDER, 1) != AACENC_OK
        || aacEncoder_SetParam(encoder, AACENC_BITRATE, (UINT)bitrate) != AACENC_OK
        || aacEncoder_SetParam(encoder, AACENC_TRANSMUX, TT_MP4_ADTS) != AACENC_OK
        || aacEncoder_SetParam(encoder, AACENC_AFTERBURNER, 1) != AACENC_OK
        || aacEncEncode(encoder, NULL, NULL, NULL, NULL) != AACENC_OK) {
        fprintf(stderr, "sbr-encode: the encoder refused the configuration\n");
        return 1;
    }
    AACENC_InfoStruct info;
    if (aacEncInfo(encoder, &info) != AACENC_OK)
        return 1;
    int const frame = (int)info.frameLength * channels;
    INT_PCM* pcm = calloc((size_t)frame, sizeof(INT_PCM));
    UCHAR bytes[8192];
    for (;;) {
        size_t const got = fread(pcm, sizeof(INT_PCM), (size_t)frame, in);
        AACENC_BufDesc in_desc = { 0 }, out_desc = { 0 };
        AACENC_InArgs in_args = { 0 };
        AACENC_OutArgs out_args = { 0 };
        void* in_ptr = pcm;
        INT in_id = IN_AUDIO_DATA, in_size = (INT)(got * sizeof(INT_PCM)), in_el = (INT)sizeof(INT_PCM);
        void* out_ptr = bytes;
        INT out_id = OUT_BITSTREAM_DATA, out_size = (INT)sizeof(bytes), out_el = 1;
        in_desc.numBufs = 1;
        in_desc.bufs = &in_ptr;
        in_desc.bufferIdentifiers = &in_id;
        in_desc.bufSizes = &in_size;
        in_desc.bufElSizes = &in_el;
        out_desc.numBufs = 1;
        out_desc.bufs = &out_ptr;
        out_desc.bufferIdentifiers = &out_id;
        out_desc.bufSizes = &out_size;
        out_desc.bufElSizes = &out_el;
        in_args.numInSamples = got ? (INT)got : -1; // -1 flushes
        AACENC_ERROR const err = aacEncEncode(encoder, &in_desc, &out_desc, &in_args, &out_args);
        if (err == AACENC_ENCODE_EOF)
            break;
        if (err != AACENC_OK) {
            fprintf(stderr, "sbr-encode: encoding failed (%d)\n", (int)err);
            return 1;
        }
        fwrite(bytes, 1, (size_t)out_args.numOutBytes, out);
    }
    aacEncClose(&encoder);
    free(pcm);
    fclose(in);
    fclose(out);
    return 0;
}
