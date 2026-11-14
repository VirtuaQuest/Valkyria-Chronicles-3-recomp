#pragma once
#include <string>

#include "../core/runtime.h"

namespace vc2rt::kernel {

struct ResidentModule {
    u32 startFunc = 0, stopFunc = 0, gp = 0;
};
void registerResidentModule(const std::string& upperFileName, const ResidentModule& m);
void setMainModuleUid(u32 uid);
u32 exitCallbackId();

}  // namespace vc2rt::kernel
