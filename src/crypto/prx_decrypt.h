#pragma once
// Game executable intake. Valkyria Chronicles III discs with the English fan-translation patch keep
// the original game executable already decrypted (SYSDIR/EBOOT.OLD), so this project needs no
// decryption and contains no console keys: plain ELF executables are passed through, and encrypted
// ones ("~PSP" containers) are reported as unsupported.
#include "../common/types.h"

namespace vc2 {

enum class DecryptResult { Ok, NotEncrypted, Encrypted, BadHeader };

const char* decryptResultString(DecryptResult r);

// If `in` is a plain ELF (optionally behind a "~SCE" header) copies it to `out` and returns
// NotEncrypted. Encrypted "~PSP" executables return Encrypted; anything else BadHeader.
DecryptResult decryptPspExecutable(const Bytes& in, Bytes& out);

// SHA-1 (Windows CNG), used to tie a built game executable to the disc it was built from.
bool sha1(const u8* data, size_t len, u8 out[20]);

}  // namespace vc2
