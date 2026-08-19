#!/usr/bin/env python3
import csv
import json
from pathlib import Path
import sys


def ns_to_ms(value):
    return value / 1_000_000.0


def load_rust_criterion(root):
    criterion = root / "rust" / "target" / "criterion"
    rows = []
    if not criterion.exists():
        return rows

    for path in criterion.rglob("estimates.json"):
        parts = path.relative_to(criterion).parts
        if len(parts) < 4 or parts[-2] != "new":
            continue
        if parts[0] != "taper_validation_rust":
            continue

        scenario = parts[1]
        data = json.loads(path.read_text())
        mean = data["mean"]
        rows.append(
            {
                "scenario": scenario,
                "implementation": "rust_taper",
                "mean_ms": ns_to_ms(mean["point_estimate"]),
                "low_ms": ns_to_ms(mean["confidence_interval"]["lower_bound"]),
                "high_ms": ns_to_ms(mean["confidence_interval"]["upper_bound"]),
            }
        )
    return rows


def load_cpp_google(root):
    rows = []
    for path in [
        root / "cpp" / "target" / "cpp-results.json",
        root / "cpp" / "build" / "cpp-results.json",
    ]:
        if not path.exists():
            continue
        data = json.loads(path.read_text())
        for bench in data.get("benchmarks", []):
            name = bench.get("name", "")
            if not name.startswith("taper_validation_cpp/"):
                continue
            scenario = name.split("/")[1]
            if bench.get("error_occurred"):
                rows.append(
                    {
                        "scenario": scenario,
                        "implementation": "cpp_taper",
                        "mean_ms": "",
                        "low_ms": "",
                        "high_ms": "",
                        "error": bench.get("error_message", "benchmark error"),
                    }
                )
                continue
            unit = bench.get("time_unit", "ns")
            value = bench.get("real_time", bench.get("cpu_time"))
            if value is None:
                continue
            if unit == "ns":
                mean_ms = value / 1_000_000.0
            elif unit == "us":
                mean_ms = value / 1_000.0
            elif unit == "ms":
                mean_ms = value
            else:
                mean_ms = value
            rows.append(
                {
                    "scenario": scenario,
                    "implementation": "cpp_taper",
                    "mean_ms": mean_ms,
                    "low_ms": "",
                    "high_ms": "",
                    "error": "",
                }
            )
    return rows


def write_csv(rows, out_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "scenario",
                "implementation",
                "mean_ms",
                "low_ms",
                "high_ms",
                "relative_to_cpp",
                "error",
            ],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_markdown(rows, out_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    scenarios = sorted({row["scenario"] for row in rows})
    by_key = {(row["scenario"], row["implementation"]): row for row in rows}

    lines = [
        "# taper-validation-bench Summary",
        "",
        "Times are milliseconds. Rust values come from Criterion means. C++ values come from Google Benchmark JSON when present.",
        "",
        "| Scenario | C++ Taper ms | Rust Taper ms | Rust/C++ |",
        "|---|---:|---:|---:|",
    ]

    for scenario in scenarios:
        cpp = by_key.get((scenario, "cpp_taper"))
        rust = by_key.get((scenario, "rust_taper"))
        cpp_ok = cpp and cpp.get("mean_ms") != ""
        ratio = rust["mean_ms"] / cpp["mean_ms"] if cpp_ok and rust and cpp["mean_ms"] else None
        cpp_text = ""
        if cpp:
            cpp_text = f"ERROR: {cpp.get('error', '')}" if cpp.get("error") else f"{cpp['mean_ms']:.3f}"
        lines.append(
            "| {scenario} | {cpp_ms} | {rust_ms} | {ratio} |".format(
                scenario=scenario,
                cpp_ms=cpp_text,
                rust_ms=f"{rust['mean_ms']:.3f}" if rust else "",
                ratio=f"{ratio:.2f}x" if ratio else "",
            )
        )

    lines.extend(
        [
            "",
            "Validation guardrail: Rust and C++ should also agree on group count and checksum during the benchmark run. Timing ratios are meaningful only after correctness passes.",
            "",
            "To include C++ in this summary, run the C++ benchmark with:",
            "",
            "```bash",
            "cd cpp",
            "./build/taper_validation --benchmark_format=json --benchmark_out=target/cpp-results.json",
            "```",
            "",
        ]
    )
    out_path.write_text("\n".join(lines))


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
    rows = load_rust_criterion(root) + load_cpp_google(root)
    if not rows:
        raise SystemExit("No Rust Criterion or C++ Google Benchmark results found")

    cpp_by_scenario = {
        row["scenario"]: row["mean_ms"]
        for row in rows
        if row["implementation"] == "cpp_taper" and row.get("mean_ms") != ""
    }
    for row in rows:
        cpp = cpp_by_scenario.get(row["scenario"])
        row.setdefault("error", "")
        row["relative_to_cpp"] = row["mean_ms"] / cpp if cpp and row.get("mean_ms") != "" else ""

    rows.sort(key=lambda row: (row["scenario"], row["implementation"]))
    write_csv(rows, root / "results" / "summary.csv")
    write_markdown(rows, root / "results" / "summary.md")
    print(f"Wrote {root / 'results' / 'summary.md'}")
    print(f"Wrote {root / 'results' / 'summary.csv'}")


if __name__ == "__main__":
    main()
