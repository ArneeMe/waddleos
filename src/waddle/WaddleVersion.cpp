#include "WaddleVersion.h"

// Written by waddle-build.yml; missing in local builds. Only this file includes
// it, so a new build number recompiles one file instead of the whole firmware.
#if __has_include("WaddleBuild.h")
#include "WaddleBuild.h"
#else
#define WADDLE_VERSION "WaddleOS (lokal)"
#define WADDLE_BUILD ""
#endif

namespace waddle {

const char* version() { return WADDLE_VERSION; }

const char* build() { return WADDLE_BUILD; }

}  // namespace waddle
