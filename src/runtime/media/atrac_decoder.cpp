#include "atrac_decoder.h"

#include <cstring>
#include <mutex>

namespace vc2rt::media {

namespace {

class SilentAtracDecoder final : public AtracDecoder {
public:
    explicit SilentAtracDecoder(const AtracCodecParams& p) { params_ = p; }
    bool decodeFrame(const uint8_t*, size_t, int16_t* out, int outChannels) override {
        memset(out, 0, (size_t)samplesPerFrame() * (size_t)outChannels * sizeof(int16_t));
        return true;
    }
    void reset() override {}
    const char* name() const override { return "silence"; }
};

std::mutex g_factoryMutex;
AtracDecoderFactory g_factory;

}  // namespace

std::unique_ptr<AtracDecoder> createSilentAtracDecoder(const AtracCodecParams& params) {
    return std::make_unique<SilentAtracDecoder>(params);
}

std::unique_ptr<AtracDecoder> createAtracDecoder(const AtracCodecParams& params) {
    AtracDecoderFactory f;
    {
        std::lock_guard<std::mutex> lk(g_factoryMutex);
        f = g_factory;
    }
    if (f) {
        if (auto d = f(params)) return d;
    }
    return createSilentAtracDecoder(params);
}

void setAtracDecoderFactory(AtracDecoderFactory factory) {
    std::lock_guard<std::mutex> lk(g_factoryMutex);
    g_factory = std::move(factory);
}

}  // namespace vc2rt::media
