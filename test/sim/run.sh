#!/usr/bin/env bash
# Builds the simulated mesh unit and runs the three-unit scenarios (test/sim/mesh_sim.py).
set -euo pipefail
cd "$(dirname "$0")/../.."
bash test/sim/build.sh build/sim
SIM_NODE=build/sim/sim_node python3 test/sim/mesh_sim.py "$@"
