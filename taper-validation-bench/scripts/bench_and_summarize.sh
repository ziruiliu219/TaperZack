#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$ROOT/rust"
cargo bench --bench taper_validation

cd "$ROOT/cpp"
rm -rf build target
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
mkdir -p target
./build/taper_validation --benchmark_format=json --benchmark_out=target/cpp-results.json

cd "$ROOT"
python3 scripts/summarize_results.py .
