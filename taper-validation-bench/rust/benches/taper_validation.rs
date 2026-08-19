use criterion::{black_box, criterion_group, criterion_main, BenchmarkId, Criterion};
use std::collections::HashSet;
use taper_hashmap::column_marshaller::{ColumnDesc, ColumnInput, TaperColumnSerializeHandler};
use xxhash_rust::xxh3::xxh3_64_with_seed;

#[derive(Clone, Copy)]
struct Scenario {
    name: &'static str,
    num_batches: usize,
    batch_rows: usize,
    num_str_cols: usize,
    num_int_cols: usize,
    global_cardinality: u64,
    new_key_rate_ppm: u64,
    string_len: usize,
    seed: u64,
    initial_slot_capacity: usize,
}

struct Batch {
    str_cols: Vec<Vec<Vec<u8>>>,
    int_cols: Vec<Vec<i64>>,
    hashes: Vec<u64>,
    values: Vec<i64>,
}

struct Workload {
    scenario: Scenario,
    batches: Vec<Batch>,
    expected_groups: usize,
    expected_checksum: i64,
}

#[inline]
fn splitmix64(mut x: u64) -> u64 {
    x = x.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut z = x;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    z ^ (z >> 31)
}

fn make_string(key_id: u64, col: usize, len: usize) -> Vec<u8> {
    let mut salt = splitmix64(key_id ^ ((col as u64) << 32));
    let mut s = format!("{salt:016x}_k{key_id:016x}_c{col}");
    while s.len() < len {
        s.push('_');
        s.push_str(&format!("{salt:016x}"));
        salt = splitmix64(salt);
    }
    s.truncate(len);
    s.into_bytes()
}

#[inline]
fn value_for_row(key_id: u64, global_row: u64) -> i64 {
    (splitmix64(key_id ^ global_row.rotate_left(17)) % 997) as i64 + 1
}

fn logical_key_id(s: Scenario, global_row: u64) -> u64 {
    let draw = splitmix64(s.seed ^ global_row);
    if draw % 1_000_000 < s.new_key_rate_ppm {
        s.global_cardinality + global_row
    } else {
        splitmix64(s.seed.wrapping_add(global_row.wrapping_mul(0xd1b5_4a32_d192_ed03)))
            % s.global_cardinality
    }
}

fn generate_workload(s: Scenario) -> Workload {
    let mut batches = Vec::with_capacity(s.num_batches);
    let mut distinct = HashSet::new();
    let mut expected_checksum = 0i64;

    for batch_idx in 0..s.num_batches {
        let mut str_cols = vec![Vec::with_capacity(s.batch_rows); s.num_str_cols];
        let mut int_cols = vec![Vec::with_capacity(s.batch_rows); s.num_int_cols];
        let mut hashes = Vec::with_capacity(s.batch_rows);
        let mut values = Vec::with_capacity(s.batch_rows);

        for row_idx in 0..s.batch_rows {
            let global_row = (batch_idx * s.batch_rows + row_idx) as u64;
            let key_id = logical_key_id(s, global_row);
            distinct.insert(key_id);

            let mut hash = 0u64;
            for col in 0..s.num_str_cols {
                let bytes = make_string(key_id, col, s.string_len);
                hash = xxh3_64_with_seed(&bytes, hash);
                str_cols[col].push(bytes);
            }
            for col in 0..s.num_int_cols {
                let v = key_id as i64 * (97 + col as i64 * 31) + 1;
                hash = xxh3_64_with_seed(&v.to_le_bytes(), hash);
                int_cols[col].push(v);
            }

            let value = value_for_row(key_id, global_row);
            expected_checksum = expected_checksum.wrapping_add(value);
            hashes.push(hash);
            values.push(value);
        }

        batches.push(Batch { str_cols, int_cols, hashes, values });
    }

    Workload {
        scenario: s,
        batches,
        expected_groups: distinct.len(),
        expected_checksum,
    }
}

fn initial_chunks(workload: &Workload) -> usize {
    if workload.scenario.initial_slot_capacity > 0 {
        return ((workload.scenario.initial_slot_capacity + 7) / 8).max(1).next_power_of_two();
    }
    let min_slots = (workload.expected_groups * 2).max(workload.scenario.batch_rows);
    ((min_slots + 7) / 8).max(1).next_power_of_two()
}

fn scenario_param(s: Scenario) -> String {
    let name = scenario_display_name(s);
    let cap = if s.initial_slot_capacity == 0 {
        "auto".to_string()
    } else if s.initial_slot_capacity % 1024 == 0 {
        format!("{}k", s.initial_slot_capacity / 1024)
    } else {
        s.initial_slot_capacity.to_string()
    };
    if s.initial_slot_capacity == 0 {
        format!("{}_cap={}", name, cap)
    } else {
        format!(
            "{}_cap={}_card={}_newppm={}",
            name, cap, s.global_cardinality, s.new_key_rate_ppm
        )
    }
}

fn scenario_display_name(s: Scenario) -> &'static str {
    if s.initial_slot_capacity != 0 && s.name == "2str_short_mostly_new" {
        "2str_short_reuse_small"
    } else {
        s.name
    }
}

fn run_taper(workload: &Workload) -> (usize, i64) {
    let s = workload.scenario;
    let mut descs = Vec::with_capacity(s.num_str_cols + s.num_int_cols);
    descs.extend((0..s.num_str_cols).map(|_| ColumnDesc::Varchar));
    descs.extend((0..s.num_int_cols).map(|_| ColumnDesc::Int64));

    let mut table = TaperColumnSerializeHandler::new(&descs, 8, initial_chunks(workload));

    for batch in &workload.batches {
        let str_slices: Vec<Vec<&[u8]>> = batch
            .str_cols
            .iter()
            .map(|col| col.iter().map(|s| s.as_slice()).collect())
            .collect();

        let mut columns = Vec::with_capacity(descs.len());
        for col in &str_slices {
            columns.push(ColumnInput::Varchar(col));
        }
        for col in &batch.int_cols {
            columns.push(ColumnInput::Int64(col));
        }

        table.emplace_table_with_decode(&batch.hashes, &columns, &batch.values);
    }

    (table.num_groups(), table.aggregate_i64_checksum())
}

fn scenarios() -> Vec<Scenario> {
    let base = vec![
        Scenario {
            name: "2str_long_reuse",
            num_batches: 1,
            batch_rows: 262_144,
            num_str_cols: 2,
            num_int_cols: 0,
            global_cardinality: 65_536,
            new_key_rate_ppm: 80_000,
            string_len: 32,
            seed: 0x1234,
            initial_slot_capacity: 0,
        },
        Scenario {
            name: "4str_long_reuse",
            num_batches: 1,
            batch_rows: 262_144,
            num_str_cols: 4,
            num_int_cols: 0,
            global_cardinality: 65_536,
            new_key_rate_ppm: 80_000,
            string_len: 32,
            seed: 0x2234,
            initial_slot_capacity: 0,
        },
        Scenario {
            name: "2str_2int_reuse",
            num_batches: 1,
            batch_rows: 262_144,
            num_str_cols: 2,
            num_int_cols: 2,
            global_cardinality: 65_536,
            new_key_rate_ppm: 80_000,
            string_len: 32,
            seed: 0x3234,
            initial_slot_capacity: 0,
        },
        Scenario {
            name: "2str_short_mostly_new",
            num_batches: 1,
            batch_rows: 262_144,
            num_str_cols: 2,
            num_int_cols: 0,
            global_cardinality: 65_536,
            new_key_rate_ppm: 750_000,
            string_len: 8,
            seed: 0x4234,
            initial_slot_capacity: 0,
        },
    ];

    let mut out = Vec::with_capacity(base.len() * 3);
    for scenario in base {
        for initial_slot_capacity in [16_384, 65_536, 0] {
            let mut s = scenario;
            s.initial_slot_capacity = initial_slot_capacity;
            s = fit_to_low_capacity(s);
            out.push(s);
        }
    }
    out
}

fn fit_to_low_capacity(mut s: Scenario) -> Scenario {
    if s.initial_slot_capacity == 0 {
        return s;
    }

    let target_groups = ((s.initial_slot_capacity as f64) * 0.80) as u64;
    let reuse_groups = (target_groups / 2).max(1);
    let unique_new_groups = target_groups.saturating_sub(reuse_groups);
    let total_rows = (s.num_batches * s.batch_rows) as u64;

    s.global_cardinality = reuse_groups;
    s.new_key_rate_ppm = ((unique_new_groups * 1_000_000) / total_rows).min(999_999);
    s
}

fn bench_taper_validation(c: &mut Criterion) {
    let mut group = c.benchmark_group("taper_validation_rust");
    group.sample_size(10);

    for scenario in scenarios() {
        let workload = generate_workload(scenario);
        let param = scenario_param(scenario);
        let (groups, checksum) = run_taper(&workload);
        assert_eq!(groups, workload.expected_groups, "group count mismatch for {}", param);
        assert_eq!(checksum, workload.expected_checksum, "checksum mismatch for {}", param);

        group.bench_with_input(BenchmarkId::from_parameter(param), &workload, |b, w| {
            b.iter(|| {
                let result = run_taper(black_box(w));
                black_box(result);
            });
        });
    }

    group.finish();
}

criterion_group!(benches, bench_taper_validation);
criterion_main!(benches);
