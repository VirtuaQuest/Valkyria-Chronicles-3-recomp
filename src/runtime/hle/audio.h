#pragma once
// sceAudio (PCM output channels + the Output2/SRC channel) and debug helpers shared with sceSas.
#include <cstdio>

#include "../core/runtime.h"

namespace vc2rt {

void audioInit();

// Minimal streaming WAV writer for debug dumps (VC2_AUDIO_DUMP / VC2_SAS_DUMP). The header is
// rewritten every second of audio so the file stays valid even if the process is killed.
class WavDump {
public:
    ~WavDump() { close(); }
    bool open(const char* path, int rate, int channels);
    bool isOpen() const { return f_ != nullptr; }
    void write(const s16* samples, size_t frames);
    void close();

private:
    void writeHeader();
    FILE* f_ = nullptr;
    int rate_ = 44100, channels_ = 2;
    u64 frames_ = 0, framesAtHeader_ = 0;
};

// Opens `dump` if the environment variable `envVar` names a file. Returns true if dumping.
bool openDumpFromEnv(WavDump& dump, const char* envVar, int rate, int channels);

}  // namespace vc2rt
