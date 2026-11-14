#pragma once
// Raw DEFLATE (RFC 1951) decoder, used for CSO-compressed disc images. Small and dependency-free;
// written for this project.
#include <cstddef>

#include "types.h"

namespace vc2 {

// Decodes a raw deflate stream (no zlib/gzip header) from `in` into `out`. Returns false on
// malformed input or when the output would exceed `outCap`. `*outLen` receives the decoded size.
bool inflateRaw(const u8* in, size_t inLen, u8* out, size_t outCap, size_t* outLen);

}  // namespace vc2
