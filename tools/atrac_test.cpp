// Developer self-test for the runtime's ATRAC3plus decoder (src/runtime/media/atrac3plus/).
//
// usage: atrac_test <outdir> <game.iso> [<game.iso> ...] [--movie NAME.PMF]
//          For every ISO: decodes PSP_GAME/SND0.AT3 (RIFF/WAVE at3+) and the ATRAC3plus audio of a
//          PMF movie (default: the largest .PMF on the disc) demuxed with media::PsDemuxer.
//          Writes <outdir>/<label>_<name>.wav (s16 stereo, all decoded samples including the
//          codec's priming delay) and <label>_<name>.frames (the raw codec frames, for comparing
//          against another decoder), and prints decode statistics and speed.
//        atrac_test file <in.at3> <out.wav>
//          Decodes a RIFF/WAVE ATRAC3plus file from disk.
//        atrac_test frames <in.frames> <out.wav>
//          Decodes a .frames dump written by this tool.
//
// .frames format (little endian): "AT3F", u32 channels, u32 frameCount, then per frame u32 size
// followed by the frame bytes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/iso9660.h"
#include "runtime/media/atrac_decoder.h"
#include "runtime/media/psmf.h"

using namespace vc2rt::media;
namespace fs = std::filesystem;

namespace {

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

struct Frames {
    int channels = 2;
    std::vector<std::vector<uint8_t>> frames;
};

bool parseAt3(const std::vector<uint8_t>& d, Frames& out, std::string& err) {
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) {
        err = "not RIFF/WAVE";
        return false;
    }
    size_t off = 12;
    int blockAlign = 0;
    bool plus = false;
    while (off + 8 <= d.size()) {
        const uint8_t* c = d.data() + off + 8;
        const uint32_t size = le32(d.data() + off + 4);
        if (!memcmp(d.data() + off, "fmt ", 4)) {
            const uint16_t tag = le16(c);
            out.channels = le16(c + 2);
            blockAlign = le16(c + 12);
            plus = tag == 0xFFFE;
            if (!plus) {
                err = "not ATRAC3plus (format tag " + std::to_string(tag) + ")";
                return false;
            }
        } else if (!memcmp(d.data() + off, "data", 4)) {
            if (!blockAlign) {
                err = "data before fmt";
                return false;
            }
            const size_t end = std::min(d.size(), off + 8 + (size_t)size);
            for (size_t p = off + 8; p + (size_t)blockAlign <= end; p += (size_t)blockAlign)
                out.frames.emplace_back(d.begin() + (ptrdiff_t)p, d.begin() + (ptrdiff_t)p + blockAlign);
            return true;
        }
        off += 8 + ((size + 1) & ~1u);
    }
    err = "no data chunk";
    return false;
}

bool demuxMovieAudio(const vc2::IsoImage& iso, const vc2::IsoEntry& e, Frames& out) {
    std::vector<uint8_t> hdr(2048);
    const uint64_t base = (uint64_t)e.lba * 2048;
    iso.readBytes(base, hdr.data(), hdr.size());
    PsmfHeader h;
    parsePsmfHeader(hdr.data(), hdr.size(), h);
    if (!h.valid()) return false;
    PsDemuxer dmx;
    dmx.setVideoStream(-1);
    dmx.setAudioStream(0);
    std::vector<uint8_t> pkt(2048);
    const uint32_t packets = h.streamSize / 2048;
    for (uint32_t i = 0; i <= packets; i++) {
        if (i < packets) {
            iso.readBytes(base + h.streamOffset + (uint64_t)i * 2048, pkt.data(), 2048);
            dmx.feedPacket(pkt.data(), 2048);
        } else {
            dmx.setEndOfStream();
        }
        AccessUnit au;
        while (dmx.popAudioFrame(au)) {
            if (out.frames.empty()) out.channels = ((au.frameHeader[2] >> 2) & 7) == 1 ? 1 : 2;
            out.frames.push_back(au.data);
        }
    }
    return !out.frames.empty();
}

void writeFrames(const fs::path& file, const Frames& f) {
    FILE* fp = _wfopen(file.c_str(), L"wb");
    if (!fp) return;
    const uint32_t hdr[3] = {0x46335441u /* "AT3F" */, (uint32_t)f.channels, (uint32_t)f.frames.size()};
    fwrite(hdr, sizeof hdr, 1, fp);
    for (const auto& fr : f.frames) {
        const uint32_t n = (uint32_t)fr.size();
        fwrite(&n, 4, 1, fp);
        fwrite(fr.data(), 1, n, fp);
    }
    fclose(fp);
}

bool readFrames(const fs::path& file, Frames& f) {
    FILE* fp = _wfopen(file.c_str(), L"rb");
    if (!fp) return false;
    uint32_t hdr[3] = {};
    bool ok = fread(hdr, sizeof hdr, 1, fp) == 1 && hdr[0] == 0x46335441u;
    f.channels = (int)hdr[1];
    for (uint32_t i = 0; ok && i < hdr[2]; i++) {
        uint32_t n = 0;
        ok = fread(&n, 4, 1, fp) == 1 && n < (1u << 20);
        std::vector<uint8_t> fr(n);
        ok = ok && fread(fr.data(), 1, n, fp) == n;
        if (ok) f.frames.push_back(std::move(fr));
    }
    fclose(fp);
    return ok;
}

bool writeWav(const fs::path& file, const std::vector<int16_t>& pcm, int channels) {
    FILE* fp = _wfopen(file.c_str(), L"wb");
    if (!fp) return false;
    const uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
    uint8_t h[44] = {};
    auto put32 = [&](int o, uint32_t v) { memcpy(h + o, &v, 4); };
    auto put16 = [&](int o, uint16_t v) { memcpy(h + o, &v, 2); };
    memcpy(h, "RIFF", 4);
    put32(4, 36 + dataBytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, (uint16_t)channels);
    put32(24, 44100);
    put32(28, 44100 * 2 * channels);
    put16(32, (uint16_t)(2 * channels));
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, dataBytes);
    fwrite(h, 1, 44, fp);
    fwrite(pcm.data(), 2, pcm.size(), fp);
    fclose(fp);
    return true;
}

// Decodes all frames to interleaved stereo, prints statistics. Returns false on any failure.
bool decodeAll(const Frames& f, const fs::path& wav, const char* label) {
    AtracCodecParams p;
    p.codec = AtracCodec::Atrac3Plus;
    p.channels = f.channels;
    p.frameBytes = f.frames.empty() ? 0 : (int)f.frames[0].size();
    auto dec = createNativeAtracDecoder(p);
    if (!dec) {
        printf("%s: decoder refused %d channels\n", label, f.channels);
        return false;
    }
    const int spf = dec->samplesPerFrame();
    std::vector<int16_t> pcm((size_t)spf * 2 * f.frames.size());
    int failed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < f.frames.size(); i++)
        if (!dec->decodeFrame(f.frames[i].data(), f.frames[i].size(), pcm.data() + i * spf * 2, 2)) failed++;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int peak = 0, clipped = 0;
    double sum = 0;
    for (int16_t s : pcm) {
        peak = std::max(peak, std::abs((int)s));
        clipped += s == 32767 || s == -32768;
        sum += (double)s * s;
    }
    const double audio = (double)f.frames.size() * spf / 44100.0;
    printf("%s: %zu frames (%d ch, %d bytes), %.2f s audio, decoded in %.3f s (%.0fx real time), %d failed, "
           "peak %d, rms %.1f, clipped %d -> %s\n",
           label, f.frames.size(), f.channels, p.frameBytes, audio, secs, secs > 0 ? audio / secs : 0.0, failed, peak,
           pcm.empty() ? 0.0 : std::sqrt(sum / (double)pcm.size()), clipped, wav.filename().string().c_str());
    writeWav(wav, pcm, 2);
    return failed == 0 && peak > 0;
}

std::string labelFor(const vc2::IsoImage& iso, const fs::path& file) {
    std::string v = iso.volumeId();
    while (!v.empty() && v.back() == ' ') v.pop_back();
    if (v.empty()) v = file.stem().string();
    for (char& c : v)
        if (!isalnum((unsigned char)c)) c = '_';
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && (!strcmp(argv[1], "file") || !strcmp(argv[1], "frames"))) {
        Frames f;
        std::string err;
        if (!strcmp(argv[1], "file")) {
            FILE* fp = fopen(argv[2], "rb");
            if (!fp) return printf("cannot open %s\n", argv[2]), 1;
            std::vector<uint8_t> d;
            uint8_t buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, fp)) > 0) d.insert(d.end(), buf, buf + n);
            fclose(fp);
            if (!parseAt3(d, f, err)) return printf("%s: %s\n", argv[2], err.c_str()), 1;
        } else if (!readFrames(argv[2], f)) {
            return printf("cannot read %s\n", argv[2]), 1;
        }
        return decodeAll(f, argv[3], argv[2]) ? 0 : 2;
    }
    if (argc < 3) {
        printf("usage: atrac_test <outdir> <game.iso> [<game.iso> ...] [--movie NAME.PMF]\n"
               "       atrac_test file <in.at3> <out.wav>\n"
               "       atrac_test frames <in.frames> <out.wav>\n");
        return 1;
    }
    const fs::path outDir = argv[1];
    fs::create_directories(outDir);
    std::string movie;
    std::vector<std::string> isos;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--movie") && i + 1 < argc) movie = vc2::toUpper(argv[++i]);
        else isos.push_back(argv[i]);
    }
    bool allOk = true;
    for (const std::string& path : isos) {
        vc2::IsoImage iso;
        std::string err;
        if (!iso.open(path, &err)) {
            printf("%s: %s\n", path.c_str(), err.c_str());
            allOk = false;
            continue;
        }
        const std::string label = labelFor(iso, path);
        printf("== %s (%s)\n", path.c_str(), label.c_str());

        Bytes snd;
        Frames sf;
        if (!iso.readFile("PSP_GAME/SND0.AT3", snd) || !parseAt3(snd, sf, err)) {
            printf("SND0.AT3: %s\n", err.empty() ? "missing" : err.c_str());
            allOk = false;
        } else {
            writeFrames(outDir / (label + "_SND0.frames"), sf);
            allOk &= decodeAll(sf, outDir / (label + "_SND0.wav"), "SND0.AT3");
        }

        // "--movie ALL" decodes every movie on the disc
        std::vector<const vc2::IsoEntry*> movies;
        const vc2::IsoEntry* best = nullptr;
        for (const auto& [name, e] : iso.entries()) {
            if (e.isDir || name.size() < 4 || name.compare(name.size() - 4, 4, ".PMF") != 0) continue;
            const std::string base = fs::path(name).filename().string();
            if (movie == "ALL") movies.push_back(&e);
            else if (!movie.empty() ? base == movie : (!best || e.size > best->size)) best = &e;
        }
        if (best) movies.push_back(best);
        if (movies.empty()) {
            printf("movie %s: not found\n", movie.c_str());
            allOk = false;
        }
        for (const vc2::IsoEntry* m : movies) {
            Frames mf;
            if (!demuxMovieAudio(iso, *m, mf)) {
                printf("%s: no ATRAC3plus audio found\n", m->path.c_str());
                continue;  // a few PMFs (e.g. ICON1.PMF) carry video only
            }
            const std::string stem = fs::path(m->path).stem().string();
            writeFrames(outDir / (label + "_" + stem + ".frames"), mf);
            allOk &= decodeAll(mf, outDir / (label + "_" + stem + ".wav"), m->path.c_str());
        }
    }
    return allOk ? 0 : 2;
}
