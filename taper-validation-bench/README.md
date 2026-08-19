# Taper Rust/C++ Validation Benchmark

This directory is for validating the Rust Taper port against the standalone C++
Taper copy before using Rust as a proxy for Omni-style Taper performance.

It intentionally does not benchmark Daft.

## What It Measures

- One persistent Taper aggregation table across many input batches.
- Multiple varchar-heavy key shapes.
- Reused keys across batches and mostly-new keys across batches.
- Initial Taper slot-capacity variants:
  - `cap=16k`
  - `cap=64k`
  - `cap=auto` (pre-sized from expected groups)
- Final group count and aggregate checksum after all batches.

The Rust and C++ harnesses use the same deterministic workload formula, so each
scenario should produce the same expected group count and checksum in both
languages.

The fixed low-capacity variants intentionally reduce generated distinct-key
cardinality for those capacities only. They target about 80% occupancy so the
benchmark measures steady-state table behavior without exercising resize/rehash.
`cap=auto` keeps the original higher-cardinality workload shape.

Because of that reduction, the low-capacity short-string scenario is named
`2str_short_reuse_small`; only the `cap=auto` version keeps the original
`2str_short_mostly_new` name.

## Layout

- `rust/benches/taper_validation.rs`: Rust Criterion benchmark using
  `../daft-bench`.
- `cpp/src/taper_validation.cpp`: C++ Google Benchmark using `../cpp-hash`.
- `cpp/CMakeLists.txt`: standalone C++ build.

## Run Rust

```bash
cd Taper-Rust/taper-validation-bench/rust
cargo bench --bench taper_validation
```

## Run C++

```bash
cd Taper-Rust/taper-validation-bench/cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/taper_validation
```

To run Rust and C++ and write readable summaries:

```bash
scripts/bench_and_summarize.sh
```

This writes:

- `results/summary.md`
- `results/summary.csv`

You can also summarize already-generated results without rerunning:

```bash
python3 scripts/summarize_results.py .
```

## Interpreting Results

Use this benchmark as a validation gate:

1. Rust and C++ must report the same group count and aggregate checksum.
2. Scenario shapes should be compared one-for-one.
3. Remaining timing differences are port/codegen/allocator differences, not Daft
   design differences.
