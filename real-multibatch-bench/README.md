# Real Multi-Batch Varchar Aggregation Benchmark

This crate is intentionally separate from `../daft-bench`.

It benchmarks the question:

```text
Omni/Taper persistent serialized aggregation
vs
Daft real staged hashbrown multi-column aggregation
```

The original `daft-bench` benchmark is a one-shot hash table microbenchmark. This
crate models multiple input batches:

- Taper keeps one `TaperColumnSerializeHandler` across all batches.
- Daft/hashbrown creates partial grouped outputs per batch, concatenates those
  partial rows, then performs a final hashbrown grouped aggregation.

Daft variants:

- `daft_generic`: `hashbrown<IndexHash, group_id, IdentityBuildHasher>` with
  row comparator.
- `daft_symbolized`: string columns are symbolized to dense `u32` IDs per
  stage; exactly two string columns with no integer columns are packed into
  one `u64` hashbrown key.

Run:

```bash
cargo bench --bench real_multibatch_varchar
```

To run the benchmark and write readable summaries:

```bash
scripts/bench_and_summarize.sh
```

This writes:

- `results/summary.md`
- `results/summary.csv`
