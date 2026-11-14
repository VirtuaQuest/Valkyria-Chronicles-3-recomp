// Developer self-test for the movie path (src/runtime/media/): opens PSMF movies straight from the
// game ISO, demuxes them packet by packet like the sceMpeg ringbuffer does, decodes the video with
// the Media Foundation H.264 decoder and saves selected frames as PNG (WIC).
//
// usage: pmf_test <game.iso> <outdir> [MOVIE.PMF|all|atrac] [frame,frame,...]
//   default movie MV00.PMF, default frames 0,30,90. With "all", every movie on the disc is fully
//   demuxed/decoded and one frame from the middle of each is saved.
//   "atrac" drives the real sceAtrac3plus HLE handlers (src/runtime/hle/atrac.cpp) on a stub guest
//   memory with the disc's SND0.AT3: fully loaded, streamed through a 16 KB ring like CRI's ADXATR
//   layer does, and looped; checks sample counts against the file's header.
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "common/iso9660.h"
#include "runtime/core/runtime.h"
#include "runtime/hle/hle.h"
#include "runtime/kernel/kernel.h"
#include "runtime/media/atrac_decoder.h"
#include "runtime/media/h264_decoder.h"
#include "runtime/media/psmf.h"

#pragma comment(lib, "windowscodecs.lib")

using namespace vc2rt::media;

static bool savePng(const std::filesystem::path& file, const VideoFrame& f) {
    std::vector<uint8_t> rgba((size_t)f.width * f.height * 4);
    convertNv12ToRgba(f, rgba.data(), f.width * 4);
    // WIC's PNG encoder takes BGRA
    for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(f.width, f.height));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(fr->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat32bppBGRA &&
         SUCCEEDED(fr->WritePixels(f.height, f.width * 4, (UINT)rgba.size(), rgba.data())) && SUCCEEDED(fr->Commit()) &&
         SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    fac->Release();
    return ok;
}

struct MovieResult {
    int aus = 0, frames = 0, audioFrames = 0, ptsErrors = 0, lagged = 0;
    int64_t firstPts = -1, lastPts = -1, firstApts = -1, lastApts = -1;
    bool ok = false;
};

static MovieResult runMovie(const vc2::IsoImage& iso, const vc2::IsoEntry& e, const std::filesystem::path& outDir,
                            const std::set<int>& want, bool middleOnly) {
    MovieResult r;
    std::vector<uint8_t> hdr(2048);
    const uint64_t base = (uint64_t)e.lba * 2048;
    iso.readBytes(base, hdr.data(), hdr.size());
    PsmfHeader h;
    parsePsmfHeader(hdr.data(), hdr.size(), h);
    printf("%s: PSMF v%d offset %u size %u (%u packets) %dx%d ts %lld..%lld (%.2fs) streams %zu\n", e.path.c_str(),
           h.version, h.streamOffset, h.streamSize, h.streamSize / 2048, h.width, h.height, (long long)h.firstTimestamp,
           (long long)h.lastTimestamp, h.durationTicks() / 90000.0, h.streams.size());
    if (!h.valid()) {
        printf("  invalid header\n");
        return r;
    }
    H264Decoder dec;
    if (!dec.open()) {
        printf("  decoder: %s\n", dec.error().c_str());
        return r;
    }
    AtracCodecParams ap;
    ap.frameBytes = 0;
    std::unique_ptr<AtracDecoder> atrac;
    std::vector<int16_t> pcm(2048 * 2);

    PsDemuxer dmx;
    dmx.setVideoStream(0);
    dmx.setAudioStream(0);
    const uint32_t packets = h.streamSize / 2048;
    std::vector<uint8_t> pkt(2048);
    const int middle = -1;
    int saved = 0;
    int64_t prevPts = -1;
    auto takeFrames = [&](int expectedTotal) {
        VideoFrame f;
        while (dec.popFrame(f)) {
            const int idx = r.frames++;
            bool save = middleOnly ? (idx == expectedTotal / 2) : want.count(idx) != 0;
            if (save) {
                char name[128];
                std::string stem = std::filesystem::path(e.path).stem().string();
                snprintf(name, sizeof name, "%s_f%04d.png", stem.c_str(), idx);
                bool ok = savePng(outDir / name, f);
                printf("  frame %d (%dx%d pts %lld) -> %s%s\n", idx, f.width, f.height, (long long)f.pts, name,
                       ok ? "" : " (WRITE FAILED)");
                saved++;
            }
        }
    };
    (void)middle;
    // Estimate the frame count for "middle" from the header duration.
    const int estFrames = (int)(h.durationTicks() / kVideoTimestampStep) + 1;
    for (uint32_t i = 0; i <= packets; i++) {
        if (i < packets) {
            iso.readBytes(base + h.streamOffset + (uint64_t)i * 2048, pkt.data(), 2048);
            dmx.feedPacket(pkt.data(), 2048);
        } else {
            dmx.setEndOfStream();
        }
        AccessUnit au;
        while (dmx.popVideoAu(au)) {
            r.aus++;
            if (r.firstPts < 0) r.firstPts = au.pts;
            if (prevPts >= 0 && au.pts != prevPts + kVideoTimestampStep) {
                if (r.ptsErrors++ < 4)
                    printf("  pts step at AU %d: %lld -> %lld (dts %lld)\n", r.aus - 1, (long long)prevPts, (long long)au.pts,
                           (long long)au.dts);
            }
            prevPts = au.pts;
            r.lastPts = au.pts;
            if (!dec.decode(au.data.data(), au.data.size(), au.pts)) printf("  decode error AU %d: %s\n", r.aus, dec.error().c_str());
            // sceMpegAvcDecode needs one picture out per AU in: count AUs that produced none.
            if (dec.queuedFrames() != 1) r.lagged++;
            takeFrames(estFrames);
        }
        while (dmx.popAudioFrame(au)) {
            if (!atrac) {
                ap.frameBytes = (int)au.data.size();
                ap.channels = ((au.frameHeader[2] >> 2) & 7) == 1 ? 1 : 2;
                atrac = createAtracDecoder(ap);
            }
            atrac->decodeFrame(au.data.data(), au.data.size(), pcm.data(), 2);
            if (r.firstApts < 0) r.firstApts = au.pts;
            r.lastApts = au.pts;
            r.audioFrames++;
        }
    }
    dec.drain();
    takeFrames(estFrames);
    const double vdur = r.aus / 29.97, adur = r.audioFrames * 2048 / 44100.0;
    printf("  video: %d AUs, %d frames decoded (%s), pts %lld..%lld, %d pts gaps; audio: %d frames (%.2fs vs video %.2fs), "
           "pts %lld..%lld, decoder %s; AUs without exactly one output frame: %d\n",
           r.aus, r.frames, dec.decoderName().c_str(), (long long)r.firstPts, (long long)r.lastPts, r.ptsErrors,
           r.audioFrames, adur, vdur, (long long)r.firstApts, (long long)r.lastApts, atrac ? atrac->name() : "-", r.lagged);
    r.ok = r.aus > 0 && r.frames == r.aus;
    return r;
}

// ------------------------------------------------------------------ sceAtrac3plus test harness
// Stub runtime environment for src/runtime/hle/atrac.cpp: guest memory, HLE registry, logger, delay.
u8* psp_mem = nullptr;
namespace vc2rt {
static HleModuleReg* g_testRegs = nullptr;
void registerHleModule(HleModuleReg* m) {
    m->next = g_testRegs;
    g_testRegs = m;
}
void logMsg(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (lvl < LogLevel::Info) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("    [%s] %s\n", tag, buf);
}
namespace kernel {
s32 waitCurrentFor(WaitType, u32, u64, bool) { return 0; }
}  // namespace kernel
}  // namespace vc2rt

static u32 hleCall(const char* name, std::initializer_list<u32> args) {
    for (vc2rt::HleModuleReg* m = vc2rt::g_testRegs; m; m = m->next)
        for (size_t i = 0; i < m->count; i++)
            if (strcmp(m->funcs[i].name, name) == 0) {
                PspContext c{};
                int r = 4;
                for (u32 a : args) c.r[r++] = a;
                m->funcs[i].fn(&c);
                return c.r[2];
            }
    printf("  no HLE function %s\n", name);
    exit(3);
}

static bool runAtracCase(const std::vector<u8>& file, const char* label, u32 bufSize, int loopNum) {
    using vc2rt::rd32;
    const u32 buf = 0x08900000, out = 0x08A00000, arg = 0x08B00000;
    const u32 initial = std::min<u32>(bufSize, (u32)file.size());
    memset(vc2rt::memPtr(buf), 0xEE, bufSize);
    memcpy(vc2rt::memPtr(buf), file.data(), initial);
    const u32 id = hleCall("sceAtracSetDataAndGetID", {buf, bufSize});
    if ((s32)id < 0) {
        printf("  %s: SetDataAndGetID failed %08x\n", label, id);
        return false;
    }
    hleCall("sceAtracGetSoundSample", {id, arg, arg + 4, arg + 8});
    const s32 end = (s32)rd32(arg), ls = (s32)rd32(arg + 4), le = (s32)rd32(arg + 8);
    hleCall("sceAtracGetChannel", {id, arg});
    const u32 ch = rd32(arg);
    hleCall("sceAtracGetBitrate", {id, arg});
    const u32 kbps = rd32(arg);
    u32 lr = hleCall("sceAtracSetLoopNum", {id, (u32)loopNum});
    hleCall("sceAtracGetRemainFrame", {id, arg});
    const s32 remain0 = (s32)rd32(arg);
    hleCall("sceAtracGetBufferInfoForResetting", {id, 0, arg});
    const u32 resetFilePos = rd32(arg + 12), resetMin = rd32(arg + 8);
    u64 total = 0, decodes = 0, refills = 0, refillBytes = 0;
    bool finished = false, ok = true;
    u32 firstSamples = 0;
    for (int guard = 0; guard < 200000 && !finished; guard++) {
        hleCall("sceAtracGetNextSample", {id, arg + 16});
        const u32 next = rd32(arg + 16);
        const u32 r = hleCall("sceAtracDecodeData", {id, out, arg, arg + 4, arg + 8});
        if (r == 0) {
            const u32 n = rd32(arg);
            if (n != next) {
                printf("  %s: GetNextSample said %u, decode gave %u\n", label, next, n);
                ok = false;
            }
            if (decodes++ == 0) firstSamples = n;
            total += n;
            finished = rd32(arg + 4) != 0;
        } else if (r == 0x80630024) {  // all data decoded
            finished = true;
        } else if (r != 0x80630023) {  // anything but "buffer empty" is a failure
            printf("  %s: DecodeData error %08x after %llu samples\n", label, r, (unsigned long long)total);
            ok = false;
            break;
        }
        // Keep the ring topped up the way the game's streaming layer does.
        hleCall("sceAtracGetStreamDataInfo", {id, arg + 20, arg + 24, arg + 28});
        const u32 wp = rd32(arg + 20), wb = rd32(arg + 24), ro = rd32(arg + 28);
        if (wb > 0 && ro < file.size()) {
            const u32 n = std::min<u32>(wb, (u32)file.size() - ro);
            memcpy(vc2rt::memPtr(wp), file.data() + ro, n);
            hleCall("sceAtracAddStreamData", {id, n});
            refills++;
            refillBytes += n;
        }
    }
    hleCall("sceAtracGetLoopStatus", {id, arg, arg + 4});
    const s32 loopsLeft = (s32)rd32(arg);
    u64 expected = (u64)end + 1;
    if (loopNum > 0 && le >= 0) expected += (u64)loopNum * (u64)(le - ls + 1);
    hleCall("sceAtracReleaseAtracID", {id});
    const bool pass = ok && finished && total == expected;
    printf("  %-26s id %u, %u ch, %u kbps, end %d, loop %d..%d (SetLoopNum %d -> %08x), remain0 %d, reset@0 file %u min %u: "
           "%llu decodes (first %u samples), %llu samples (expected %llu), %llu refills / %llu bytes, loops left %d -> %s\n",
           label, id, ch, kbps, end, ls, le, loopNum, lr, remain0, resetFilePos, resetMin, (unsigned long long)decodes,
           firstSamples, (unsigned long long)total, (unsigned long long)expected, (unsigned long long)refills,
           (unsigned long long)refillBytes, loopsLeft, pass ? "OK" : "FAIL");
    return pass;
}

static int runAtracTests(const vc2::IsoImage& iso) {
    psp_mem = (u8*)VirtualAlloc(nullptr, 0x20000000, MEM_RESERVE, PAGE_NOACCESS);
    if (!psp_mem || !VirtualAlloc(psp_mem + 0x08000000, 0x02000000, MEM_COMMIT, PAGE_READWRITE)) {
        printf("cannot allocate guest memory\n");
        return 1;
    }
    Bytes file;
    if (!iso.readFile("PSP_GAME/SND0.AT3", file)) {
        printf("PSP_GAME/SND0.AT3 not found\n");
        return 1;
    }
    printf("SND0.AT3: %zu bytes\n", file.size());
    int fails = 0;
    fails += !runAtracCase(file, "all data loaded", (u32)file.size(), 0);
    fails += !runAtracCase(file, "streamed 16 KB", 16384, 0);
    fails += !runAtracCase(file, "streamed 16 KB, loop x2", 16384, 2);
    fails += !runAtracCase(file, "streamed 7 KB odd ring", 7000, 1);
    fails += !runAtracCase(file, "all data, loop x1", (u32)file.size(), 1);
    // Same file without its loop point ('smpl' chunk renamed): streamed-without-loop state.
    Bytes noLoop = file;
    for (size_t i = 12; i + 8 <= noLoop.size() && i < 4096; i++)
        if (memcmp(&noLoop[i], "smpl", 4) == 0) {
            memcpy(&noLoop[i], "junk", 4);
            break;
        }
    fails += !runAtracCase(noLoop, "no loop, streamed 16 KB", 16384, 0);
    fails += !runAtracCase(noLoop, "no loop, all data", (u32)noLoop.size(), 0);
    printf("atrac: %d case(s) failed\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pmf_test <game.iso> <outdir> [MOVIE.PMF|all|atrac] [frame,frame,...]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    vc2::IsoImage iso;
    std::string err;
    if (!iso.open(argv[1], &err)) {
        fprintf(stderr, "cannot open ISO: %s\n", err.c_str());
        return 1;
    }
    std::filesystem::path outDir = argv[2];
    std::filesystem::create_directories(outDir);
    std::string movie = argc > 3 ? argv[3] : "MV00.PMF";
    if (movie == "atrac") return runAtracTests(iso);
    std::set<int> want = {0, 30, 90};
    if (argc > 4) {
        want.clear();
        for (char* t = strtok(argv[4], ","); t; t = strtok(nullptr, ",")) want.insert(atoi(t));
    }
    int fails = 0, n = 0;
    for (auto& [path, e] : iso.entries()) {
        if (e.isDir || path.rfind("PSP_GAME/USRDIR/MOVIE/", 0) != 0) continue;
        std::string name = path.substr(22);
        if (movie != "all" && vc2::toUpper(movie) != name) continue;
        MovieResult r = runMovie(iso, e, outDir, want, movie == "all");
        n++;
        if (!r.ok) fails++;
    }
    printf("%d movie(s), %d failed\n", n, fails);
    return (n == 0 || fails) ? 1 : 0;
}
