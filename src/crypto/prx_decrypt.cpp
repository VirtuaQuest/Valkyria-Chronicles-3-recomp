#include "prx_decrypt.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace vc2 {

const char* decryptResultString(DecryptResult r) {
    switch (r) {
        case DecryptResult::Ok: return "ok";
        case DecryptResult::NotEncrypted: return "not encrypted";
        case DecryptResult::Encrypted:
            return "the executable is encrypted; this project only builds from the decrypted executable on "
                   "discs with the English fan-translation patch applied (SYSDIR/EBOOT.OLD)";
        case DecryptResult::BadHeader: return "not a PSP executable";
    }
    return "?";
}

bool sha1(const u8* data, size_t len, u8 out[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0) == 0 && BCryptFinishHash(h, out, 20, 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

DecryptResult decryptPspExecutable(const Bytes& inRaw, Bytes& out) {
    const Bytes* in = &inRaw;
    Bytes stripped;
    if (inRaw.size() >= 0x40 && memcmp(inRaw.data(), "~SCE", 4) == 0) {
        u32 hdrLen = rd32(&inRaw[4]);
        if (hdrLen >= inRaw.size()) return DecryptResult::BadHeader;
        stripped.assign(inRaw.begin() + hdrLen, inRaw.end());
        in = &stripped;
    }
    if (in->size() >= 4 && memcmp(in->data(), "\x7F""ELF", 4) == 0) {
        out = *in;
        return DecryptResult::NotEncrypted;
    }
    if (in->size() >= 4 && memcmp(in->data(), "~PSP", 4) == 0) return DecryptResult::Encrypted;
    return DecryptResult::BadHeader;
}

}  // namespace vc2
