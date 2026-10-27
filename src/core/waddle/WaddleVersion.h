#pragma once

namespace waddle {

// "WaddleOS 1.42": the number is the GitHub Actions build count, so it goes up
// with every build of waddle. Local builds show "WaddleOS (lokal)".
const char* version();

// Short git hash of this build, "" in local builds. Oppdater fastvare compares
// it with version.json.
const char* build();

}  // namespace waddle
