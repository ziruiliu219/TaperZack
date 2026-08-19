#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
cargo bench --bench real_multibatch_varchar
python3 scripts/summarize_results.py .

