#pragma once
// H.264 (Annex-B) decoder on top of the Media Foundation H.264 decoder MFT that ships with Windows
// (CLSID_CMSH264DecoderMFT, falling back to any synchronous decoder found with MFTEnumEx).
// Input: one access unit at a time; output: NV12 frames.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct IMFTransform;

namespace vc2rt::media {

struct VideoFrame {
    int width = 0, height = 0;  // visible size
    int stride = 0;             // bytes per luma row (also per interleaved chroma row)
    std::vector<uint8_t> nv12;  // stride*height luma, then stride*height/2 CbCr
    int64_t pts = -1;           // 90 kHz (as passed with the access unit)

    const uint8_t* luma() const { return nv12.data(); }
    const uint8_t* chroma() const { return nv12.data() + (size_t)stride * height; }
};

class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Creates the decoder MFT. Returns false (and sets error()) if Media Foundation or an H.264
    // decoder is unavailable.
    bool open();
    void close();
    bool isOpen() const { return mft_ != nullptr; }

    // Submits one access unit (Annex-B). Decoded frames become available via popFrame().
    bool decode(const uint8_t* au, size_t size, int64_t pts90k);
    // Signals end of stream: all frames still held by the decoder are output.
    void drain();
    // Discards everything in flight (seek / stop).
    void flush();

    bool hasFrame() const { return !frames_.empty(); }
    bool popFrame(VideoFrame& f);
    size_t queuedFrames() const { return frames_.size(); }

    const std::string& error() const { return error_; }
    const std::string& decoderName() const { return name_; }

private:
    bool setOutputType();
    void pullOutput();
    bool setInputType();

    IMFTransform* mft_ = nullptr;
    bool streaming_ = false;
    bool comInit_ = false;
    bool mfStarted_ = false;
    int outW_ = 0, outH_ = 0, outStride_ = 0;
    int visW_ = 0, visH_ = 0;
    uint32_t outBufferSize_ = 0;
    bool providesSamples_ = false;
    std::deque<VideoFrame> frames_;
    std::string error_, name_;
};

// Converts an NV12 frame to the PSP pixel format `pixelMode` (0 BGR5650, 1 ABGR5551, 2 ABGR4444,
// 3 ABGR8888) into `dst` with a stride of `dstStridePixels` pixels, BT.601 limited range like the
// PSP's AVC colour space converter. Writes min(width, dstStride) x height pixels.
void convertNv12ToPsp(const VideoFrame& f, uint8_t* dst, int dstStridePixels, int pixelMode, int maxRows = 1 << 30);
// Same but to RGBA8888 rows (for tests / screenshots).
void convertNv12ToRgba(const VideoFrame& f, uint8_t* dst, int dstStrideBytes);

}  // namespace vc2rt::media
