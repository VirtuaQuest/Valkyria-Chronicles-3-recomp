#include "h264_decoder.h"

#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace vc2rt::media {

namespace {

template <class T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string hrText(const char* what, HRESULT hr) {
    char b[96];
    snprintf(b, sizeof b, "%s failed (hr=0x%08lx)", what, (unsigned long)hr);
    return b;
}

bool startMediaFoundation() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
    return ok;
}

LONGLONG ts90kTo100ns(int64_t t) { return (LONGLONG)(t * 10000000 / 90000); }
int64_t ts100nsTo90k(LONGLONG t) { return (int64_t)((t * 90000 + 5000000) / 10000000); }

}  // namespace

H264Decoder::~H264Decoder() { close(); }

bool H264Decoder::open() {
    close();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInit_ = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: the thread already has an apartment, fine
    if (!startMediaFoundation()) {
        error_ = "MFStartup failed";
        return false;
    }
    hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
    if (SUCCEEDED(hr)) {
        name_ = "Microsoft H264 Video Decoder MFT";
    } else {
        // e.g. Windows "N" editions without the media pack: try any registered synchronous decoder.
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_H264};
        MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_NV12};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr2 = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                                &acts, &count);
        if (SUCCEEDED(hr2)) {
            for (UINT32 i = 0; i < count && !mft_; i++) {
                if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft_)))) {
                    WCHAR* fn = nullptr;
                    UINT32 len = 0;
                    if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len))) {
                        char b[256];
                        WideCharToMultiByte(CP_UTF8, 0, fn, -1, b, sizeof b, nullptr, nullptr);
                        name_ = b;
                        CoTaskMemFree(fn);
                    }
                }
            }
            for (UINT32 i = 0; i < count; i++) acts[i]->Release();
            CoTaskMemFree(acts);
        }
        if (!mft_) {
            error_ = hrText("creating an H.264 decoder MFT", hr);
            return false;
        }
    }
    // Ask for frames as soon as they are decodable (PSP movies have no B-frames, so this keeps the
    // decoder output in lock step with the input like the PSP's AVC decoder).
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attrs->Release();
    }
    if (!setInputType()) {
        close();
        return false;
    }
    setOutputType();  // may legitimately fail until the first SPS is seen
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    streaming_ = true;
    return true;
}

void H264Decoder::close() {
    if (mft_) {
        if (streaming_) {
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        safeRelease(mft_);
    }
    streaming_ = false;
    frames_.clear();
    outW_ = outH_ = outStride_ = visW_ = visH_ = 0;
    outBufferSize_ = 0;
}

bool H264Decoder::setInputType() {
    IMFMediaType* t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) {
        error_ = hrText("MFCreateMediaType", hr);
        return false;
    }
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 30000, 1001);
    hr = mft_->SetInputType(0, t, 0);
    t->Release();
    if (FAILED(hr)) {
        error_ = hrText("IMFTransform::SetInputType(H264)", hr);
        return false;
    }
    return true;
}

bool H264Decoder::setOutputType() {
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = nullptr;
        HRESULT hr = mft_->GetOutputAvailableType(0, i, &t);
        if (FAILED(hr)) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub != MFVideoFormat_NV12) {
            t->Release();
            continue;
        }
        hr = mft_->SetOutputType(0, t, 0);
        if (FAILED(hr)) {
            t->Release();
            return false;
        }
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
        outW_ = (int)w;
        outH_ = (int)h;
        UINT32 stride = 0;
        outStride_ = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (int)(INT32)stride : outW_;
        if (outStride_ < 0) outStride_ = -outStride_;
        visW_ = outW_;
        visH_ = outH_;
        MFVideoArea area{};
        if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, nullptr))) {
            if (area.Area.cx > 0 && area.Area.cy > 0) {
                visW_ = std::min<int>(area.Area.cx, outW_);
                visH_ = std::min<int>(area.Area.cy, outH_);
            }
        }
        t->Release();
        MFT_OUTPUT_STREAM_INFO si{};
        if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &si))) {
            outBufferSize_ = si.cbSize;
            providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        }
        if (outBufferSize_ == 0) outBufferSize_ = (uint32_t)(outStride_ * outH_ * 3 / 2);
        return true;
    }
}

bool H264Decoder::decode(const uint8_t* au, size_t size, int64_t pts) {
    if (!mft_ || size == 0) return false;
    if (!streaming_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        streaming_ = true;
    }
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer((DWORD)size, &b))) {
        safeRelease(s);
        safeRelease(b);
        return false;
    }
    BYTE* p = nullptr;
    b->Lock(&p, nullptr, nullptr);
    memcpy(p, au, size);
    b->Unlock();
    b->SetCurrentLength((DWORD)size);
    s->AddBuffer(b);
    b->Release();
    if (pts >= 0) s->SetSampleTime(ts90kTo100ns(pts));
    s->SetSampleDuration(333667);
    HRESULT hr = mft_->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        pullOutput();
        hr = mft_->ProcessInput(0, s, 0);
    }
    s->Release();
    pullOutput();
    if (FAILED(hr)) {
        error_ = hrText("IMFTransform::ProcessInput", hr);
        return false;
    }
    return true;
}

void H264Decoder::pullOutput() {
    for (int guard = 0; guard < 64; guard++) {
        if (outW_ == 0 && !setOutputType()) return;
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = 0;
        IMFSample* own = nullptr;
        if (!providesSamples_) {
            IMFMediaBuffer* b = nullptr;
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(outBufferSize_, &b))) {
                safeRelease(own);
                safeRelease(b);
                return;
            }
            own->AddBuffer(b);
            b->Release();
            ob.pSample = own;
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (ob.pSample && ob.pSample != own) ob.pSample->Release();
            safeRelease(own);
            outW_ = 0;
            if (!setOutputType()) return;
            continue;
        }
        if (FAILED(hr)) {  // MF_E_TRANSFORM_NEED_MORE_INPUT or a real error
            if (ob.pSample && ob.pSample != own) ob.pSample->Release();
            safeRelease(own);
            return;
        }
        IMFSample* out = ob.pSample;
        IMFMediaBuffer* buf = nullptr;
        if (out && SUCCEEDED(out->ConvertToContiguousBuffer(&buf))) {
            BYTE* base = nullptr;
            LONG pitch = 0;
            IMF2DBuffer* b2 = nullptr;
            bool locked2d = false;
            DWORD curLen = 0;
            if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&base, &pitch))) {
                locked2d = true;
            } else {
                safeRelease(b2);
                buf->Lock(&base, nullptr, &curLen);
                pitch = outStride_;
            }
            if (pitch < 0) pitch = -pitch;  // NV12 is never bottom-up; be defensive anyway
            VideoFrame f;
            f.width = visW_ & ~1;
            f.height = visH_ & ~1;
            f.stride = f.width;
            f.nv12.resize((size_t)f.stride * f.height * 3 / 2);
            const BYTE* srcY = base;
            const BYTE* srcUV = base + (size_t)pitch * outH_;
            for (int y = 0; y < f.height; y++) memcpy(f.nv12.data() + (size_t)y * f.stride, srcY + (size_t)y * pitch, f.width);
            uint8_t* dstUV = f.nv12.data() + (size_t)f.stride * f.height;
            for (int y = 0; y < f.height / 2; y++)
                memcpy(dstUV + (size_t)y * f.stride, srcUV + (size_t)y * pitch, f.width);
            LONGLONG t = 0;
            f.pts = SUCCEEDED(out->GetSampleTime(&t)) ? ts100nsTo90k(t) : -1;
            if (locked2d) {
                b2->Unlock2D();
                b2->Release();
            } else {
                buf->Unlock();
            }
            buf->Release();
            frames_.push_back(std::move(f));
        }
        if (out && out != own) out->Release();
        safeRelease(own);
    }
}

void H264Decoder::drain() {
    if (!mft_ || !streaming_) return;
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    pullOutput();
    streaming_ = false;  // START_OF_STREAM is sent again with the next input
}

void H264Decoder::flush() {
    if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    frames_.clear();
}

bool H264Decoder::popFrame(VideoFrame& f) {
    if (frames_.empty()) return false;
    f = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

// ------------------------------------------------------------------ colour conversion
namespace {
inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

template <class Emit>
void forEachPixel(const VideoFrame& f, int cols, int rows, Emit emit) {
    for (int y = 0; y < rows; y++) {
        const uint8_t* Y = f.luma() + (size_t)y * f.stride;
        const uint8_t* UV = f.chroma() + (size_t)(y >> 1) * f.stride;
        for (int x = 0; x < cols; x++) {
            const int c = 298 * (Y[x] - 16);
            const int d = UV[x & ~1] - 128;
            const int e = UV[(x & ~1) + 1] - 128;
            emit(x, y, clamp8((c + 409 * e + 128) >> 8), clamp8((c - 100 * d - 208 * e + 128) >> 8),
                 clamp8((c + 516 * d + 128) >> 8));
        }
    }
}
}  // namespace

void convertNv12ToPsp(const VideoFrame& f, uint8_t* dst, int dstStride, int mode, int maxRows) {
    const int cols = std::min(f.width, dstStride);
    const int rows = std::min(f.height, maxRows);
    switch (mode) {
        case 0:  // BGR5650
            forEachPixel(f, cols, rows, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                const uint16_t v = (uint16_t)((b >> 3) << 11 | (g >> 2) << 5 | (r >> 3));
                memcpy(dst + ((size_t)y * dstStride + x) * 2, &v, 2);
            });
            break;
        case 1:  // ABGR5551
            forEachPixel(f, cols, rows, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                const uint16_t v = (uint16_t)(0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3));
                memcpy(dst + ((size_t)y * dstStride + x) * 2, &v, 2);
            });
            break;
        case 2:  // ABGR4444
            forEachPixel(f, cols, rows, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                const uint16_t v = (uint16_t)(0xF000 | (b >> 4) << 8 | (g >> 4) << 4 | (r >> 4));
                memcpy(dst + ((size_t)y * dstStride + x) * 2, &v, 2);
            });
            break;
        default:  // ABGR8888 (bytes R, G, B, A)
            forEachPixel(f, cols, rows, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                uint8_t* p = dst + ((size_t)y * dstStride + x) * 4;
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = 0xFF;
            });
            break;
    }
}

void convertNv12ToRgba(const VideoFrame& f, uint8_t* dst, int dstStrideBytes) {
    forEachPixel(f, f.width, f.height, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = dst + (size_t)y * dstStrideBytes + (size_t)x * 4;
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 0xFF;
    });
}

}  // namespace vc2rt::media
