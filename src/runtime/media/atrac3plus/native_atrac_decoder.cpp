// media::AtracDecoder implementation on top of the project's own ATRAC3plus decoder, installed as
// the runtime's decoder factory at boot (installNativeAtracDecoder).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "../atrac_decoder.h"
#include "atrac3plus.h"

namespace vc2rt::media {
namespace {

inline int16_t toS16(float f) {
    const int v = (int)lrintf(f * 32768.0f);
    return (int16_t)std::clamp(v, -32768, 32767);
}

class NativeAtrac3PlusDecoder final : public AtracDecoder {
public:
    explicit NativeAtrac3PlusDecoder(const AtracCodecParams& p) {
        params_ = p;
        ok_ = dec_.init(std::clamp(p.channels, 1, 2));
    }
    bool valid() const { return ok_; }

    bool decodeFrame(const uint8_t* data, size_t size, int16_t* out, int outChannels) override {
        float* planes[2] = {left_, right_};
        const bool ok = data && size && dec_.decodeFrame(data, size, planes);
        if (!ok) {
            memset(left_, 0, sizeof left_);
            memset(right_, 0, sizeof right_);
        }
        const bool stereo = dec_.channels() >= 2;
        const float* r = stereo ? right_ : left_;
        if (outChannels == 2) {
            for (int i = 0; i < at3p::kFrameSamples; i++) {
                out[i * 2] = toS16(left_[i]);
                out[i * 2 + 1] = toS16(r[i]);
            }
        } else {
            for (int i = 0; i < at3p::kFrameSamples; i++) out[i] = toS16(stereo ? (left_[i] + r[i]) * 0.5f : left_[i]);
        }
        return ok;
    }

    void reset() override { dec_.reset(); }
    const char* name() const override { return "native-atrac3plus"; }

private:
    at3p::Decoder dec_;
    bool ok_ = false;
    float left_[at3p::kFrameSamples] = {};
    float right_[at3p::kFrameSamples] = {};
};

}  // namespace

std::unique_ptr<AtracDecoder> createNativeAtracDecoder(const AtracCodecParams& p) {
    if (p.codec == AtracCodec::Atrac3Plus) {
        auto d = std::make_unique<NativeAtrac3PlusDecoder>(p);
        if (d->valid()) return d;
    }
    return nullptr;  // createAtracDecoder falls back to silence
}

bool installNativeAtracDecoder() {
    setAtracDecoderFactory(createNativeAtracDecoder);
    return true;
}

}  // namespace vc2rt::media
