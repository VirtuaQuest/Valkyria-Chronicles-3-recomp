#pragma once
#include <string>

namespace vc2 {
class IsoImage;
}

namespace vc2rt {
vc2::IsoImage& disc();
namespace kernel {
void ioSetCwd(const std::string& cwd);
// Serves files missing under "MS0:/<prefix>/..." read-only from "<discDir>/..." on the disc.
void ioAddDiscOverlay(const std::string& ms0Prefix, const std::string& discDir);
}
}  // namespace vc2rt
