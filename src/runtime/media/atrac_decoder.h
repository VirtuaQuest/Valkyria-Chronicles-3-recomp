#pragma once
// Pluggable ATRAC3 / ATRAC3plus frame decoder.
//
// Both the movie path (sceMpegAtracDecode) and sceAtrac3plus decode through this interface. The
// runtime's own decoder (src/runtime/media/atrac3plus/) is installed at boot with
// installNativeAtracDecoder(). Without a factory, or for a codec the factory declines, the
// default implementation produces silence of the correct length, so timing, sample counts and
// buffer handling stay exact either way.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace vc2rt::media {

enum class AtracCodec : int {
    Atrac3Plus = 0x1000,  // PSP_CODEC_AT3PLUS: 2048 samples per frame
    Atrac3 = 0x1001,      // PSP_CODEC_AT3: 1024 samples per frame
};

struct AtracCodecParams {
    AtracCodec codec = AtracCodec::Atrac3Plus;
    int channels = 2;           // encoded channels (1 or 2)
    int sampleRate = 44100;
    int frameBytes = 0;         // bytes of one encoded frame (WAVE block align / PSMF frame size)
    std::vector<uint8_t> extra; // codec specific data from the container (fmt extension), may be empty
};

inline int atracSamplesPerFrame(AtracCodec c) { return c == AtracCodec::Atrac3 ? 1024 : 2048; }

class AtracDecoder {
public:
    virtual ~AtracDecoder() = default;
    // Decodes one encoded frame of `params().frameBytes` bytes (fewer allowed for a truncated last
    // frame) into `outChannels` interleaved s16 channels (1 or 2; mono sources are duplicated for
    // stereo output). Always writes samplesPerFrame() sample frames. Returns false if the frame
    // could not be decoded (the output is then silence).
    virtual bool decodeFrame(const uint8_t* data, size_t size, int16_t* out, int outChannels) = 0;
    // Discards inter-frame state (after a seek / loop).
    virtual void reset() = 0;
    // A short name for logs ("silence", "ffmpeg", ...).
    virtual const char* name() const = 0;

    const AtracCodecParams& params() const { return params_; }
    int samplesPerFrame() const { return atracSamplesPerFrame(params_.codec); }

protected:
    AtracCodecParams params_;
};

using AtracDecoderFactory = std::function<std::unique_ptr<AtracDecoder>(const AtracCodecParams&)>;

// Creates a decoder through the installed factory (default: silence).
std::unique_ptr<AtracDecoder> createAtracDecoder(const AtracCodecParams& params);
// Installs a different decoder implementation (nullptr restores the default).
void setAtracDecoderFactory(AtracDecoderFactory factory);
// The built-in implementation: correct length, all zero samples.
std::unique_ptr<AtracDecoder> createSilentAtracDecoder(const AtracCodecParams& params);

// The project's own decoder (media/atrac3plus/). Returns nullptr for an unsupported codec or
// channel layout.
std::unique_ptr<AtracDecoder> createNativeAtracDecoder(const AtracCodecParams& params);
// Makes createNativeAtracDecoder the factory used by createAtracDecoder. Returns true.
bool installNativeAtracDecoder();

}  // namespace vc2rt::media
