#!/usr/bin/env python3
import csv
import json
from pathlib import Path
import sys


IMPLEMENTATION_ORDER = [
    "omni_taper_persistent",
    "daft_hashbrown_generic_staged",
    "daft_hashbrown_symbolized_staged",
]


def ns_to_ms(value):
    return value / 1_000_000.0


def load_estimates(root):
    criterion = root / "target" / "criterion"
    rows = []
    for path in criterion.rglob("estimates.json"):
        parts = path.relative_to(criterion).parts
        if len(parts) < 5 or parts[-2] != "new":
            continue
        if parts[0] != "real_multibatch_varchar":
            continue

        implementation = parts[1]
        workload = parts[2]
        data = json.loads(path.read_text())
        mean = data["mean"]
        rows.append(
            {
                "workload": workload,
                "implementation": implementation,
                "mean_ms": ns_to_ms(mean["point_estimate"]),
                "low_ms": ns_to_ms(mean["confidence_interval"]["lower_bound"]),
                "high_ms": ns_to_ms(mean["confidence_interval"]["upper_bound"]),
            }
        )
    return rows


def write_csv(rows, out_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "workload",
                "implementation",
                "mean_ms",
                "low_ms",
                "high_ms",
                "relative_to_taper",
            ],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_markdown(rows, out_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    workloads = sorted({row["workload"] for row in rows})
    by_key = {(row["workload"], row["implementation"]): row for row in rows}

    lines = [
        "# real-multibatch-bench Summary",
        "",
        "Times are Criterion mean estimates in milliseconds. Ratios are relative to `omni_taper_persistent` for the same workload.",
        "",
        "| Workload | Taper ms | Daft generic ms | Daft symbolized ms | Generic/Taper | Symbolized/Taper | Winner |",
        "|---|---:|---:|---:|---:|---:|---|",
    ]

    for workload in workloads:
        taper = by_key.get((workload, "omni_taper_persistent"))
        generic = by_key.get((workload, "daft_hashbrown_generic_staged"))
        symbolized = by_key.get((workload, "daft_hashbrown_symbolized_staged"))

        values = [row for row in [taper, generic, symbolized] if row]
        winner = min(values, key=lambda row: row["mean_ms"])["implementation"] if values else ""

        taper_ms = taper["mean_ms"] if taper else None
        generic_ratio = generic["mean_ms"] / taper_ms if taper_ms and generic else None
        symbolized_ratio = symbolized["mean_ms"] / taper_ms if taper_ms and symbolized else None

        lines.append(
            "| {workload} | {taper} | {generic} | {symbolized} | {generic_ratio} | {symbolized_ratio} | `{winner}` |".format(
                workload=workload,
                taper=f"{taper_ms:.3f}" if taper else "",
                generic=f"{generic['mean_ms']:.3f}" if generic else "",
                symbolized=f"{symbolized['mean_ms']:.3f}" if symbolized else "",
                generic_ratio=f"{generic_ratio:.2f}x" if generic_ratio else "",
                symbolized_ratio=f"{symbolized_ratio:.2f}x" if symbolized_ratio else "",
                winner=winner,
            )
        )

    lines.extend(
        [
            "",
            "Interpretation guardrail: this is a Rust model of persistent Taper versus modeled Daft-style staged hashbrown, not a production Omni-vs-Daft claim.",
            "",
        ]
    )
    out_path.write_text("\n".join(lines))


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
    rows = load_estimates(root)
    if not rows:
        raise SystemExit(f"No Criterion estimates found under {root / 'target' / 'criterion'}")

    taper_by_workload = {
        row["workload"]: row["mean_ms"]
        for row in rows
        if row["implementation"] == "omni_taper_persistent"
    }
    for row in rows:
        taper = taper_by_workload.get(row["workload"])
        row["relative_to_taper"] = row["mean_ms"] / taper if taper else ""

    rows.sort(
        key=lambda row: (
            row["workload"],
            IMPLEMENTATION_ORDER.index(row["implementation"])
            if row["implementation"] in IMPLEMENTATION_ORDER
            else len(IMPLEMENTATION_ORDER),
        )
    )

    write_csv(rows, root / "results" / "summary.csv")
    write_markdown(rows, root / "results" / "summary.md")
    print(f"Wrote {root / 'results' / 'summary.md'}")
    print(f"Wrote {root / 'results' / 'summary.csv'}")


if __name__ == "__main__":
    main()

