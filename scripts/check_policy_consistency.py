#!/usr/bin/env python3
"""Compare summary raw winners against generated autotune policy intervals."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


IMPLS = ["ref", "linux_aesni_avx", "linux_aesni_avx2", "linux_gfni_avx512"]
EFFECTIVE_PATH_TO_IMPL = {
    "ref": "ref",
    "ref_fallback": "ref",
    "linux_aesni_avx2": "linux_aesni_avx2",
    "linux_aesni_avx2_plus_avx_tail": "linux_aesni_avx2",
    "linux_aesni_avx2_plus_ref_tail": "linux_aesni_avx2",
    "linux_aesni_avx2_plus_avx_ref_tail": "linux_aesni_avx2",
    "linux_aesni_avx": "linux_aesni_avx",
    "linux_aesni_avx_plus_ref_tail": "linux_aesni_avx",
    "linux_gfni_avx512": "linux_gfni_avx512",
    "linux_gfni_avx_16way": "linux_gfni_avx512",
    "linux_gfni_avx2_32way": "linux_gfni_avx512",
    "linux_gfni_mixed_width": "linux_gfni_avx512",
    "linux_gfni_plus_ref_tail": "linux_gfni_avx512",
    "mixed_effective_path": "ref",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Check whether policy intervals are explainable from summary "
            "trimmed-mean raw winners."
        )
    )
    parser.add_argument("--summary", required=True, help="Path to summary_stats.csv")
    parser.add_argument("--policy", required=True, help="Path to autotune_policy.csv")
    parser.add_argument(
        "--metric",
        default="trimmed_mean_ns_per_call",
        help="Summary metric column to compare (default: trimmed_mean_ns_per_call)",
    )
    parser.add_argument("--run-id", help="Only use one summary run_id")
    parser.add_argument("--scenario", help="Only use one summary scenario")
    return parser.parse_args()


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
    if not rows:
        raise SystemExit(f"error: empty CSV: {path}")
    return rows


def choose_summary_rows(
    rows: list[dict[str, str]],
    *,
    run_id: str | None,
    scenario: str | None,
) -> list[dict[str, str]]:
    run_ids = sorted({row["run_id"] for row in rows})
    selected_run_id = run_id or run_ids[-1]
    rows = [row for row in rows if row["run_id"] == selected_run_id]
    if not rows:
        raise SystemExit(f"error: no summary rows for run_id={selected_run_id}")

    scenarios = sorted({row["scenario"] for row in rows})
    selected_scenario = scenario or scenarios[0]
    if scenario is None and len(scenarios) > 1:
        raise SystemExit(
            "error: multiple scenarios in summary; pass --scenario. "
            f"available={', '.join(scenarios)}"
        )
    rows = [row for row in rows if row["scenario"] == selected_scenario]
    if not rows:
        raise SystemExit(f"error: no summary rows for scenario={selected_scenario}")

    print(f"Summary run_id: {selected_run_id}")
    print(f"Summary scenario: {selected_scenario}")
    return rows


def canonical_from_effective_path(path: str) -> str:
    if path not in EFFECTIVE_PATH_TO_IMPL:
        raise SystemExit(f"error: unknown effective_path: {path}")
    return EFFECTIVE_PATH_TO_IMPL[path]


def raw_best_by_length(
    rows: list[dict[str, str]],
    metric: str,
) -> dict[tuple[int, int], dict[str, object]]:
    if metric not in rows[0]:
        raise SystemExit(f"error: metric column not found in summary: {metric}")

    grouped: dict[tuple[int, int], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["impl"] not in IMPLS:
            continue
        grouped[(int(row["key_bits"]), int(row["len"]))].append(row)

    best: dict[tuple[int, int], dict[str, object]] = {}
    for key, candidates in grouped.items():
        candidates.sort(key=lambda row: float(row[metric]))
        winner = candidates[0]
        second = candidates[1] if len(candidates) > 1 else candidates[0]
        best_value = float(winner[metric])
        second_value = float(second[metric])
        margin_pct = 0.0
        if best_value > 0.0 and second_value > 0.0:
            margin_pct = max(0.0, ((second_value - best_value) / best_value) * 100.0)
        best[key] = {
            "raw_best_impl": winner["impl"],
            "raw_best_effective_path": winner["effective_path"],
            "raw_best_canonical": canonical_from_effective_path(winner["effective_path"]),
            "raw_best_value": best_value,
            "second_best_impl": second["impl"],
            "second_best_value": second_value,
            "margin_pct": margin_pct,
            "values": {row["impl"]: float(row[metric]) for row in candidates},
        }
    return best


def load_policy(path: Path) -> list[dict[str, object]]:
    rows = read_csv(path)
    policies = []
    for row in rows:
        policies.append(
            {
                "key_bits": int(row["key_bits"]),
                "start_len": int(row["start_len"]),
                "end_len": int(row["end_len"]),
                "policy_impl": row.get("policy_impl") or row.get("policy_chosen_impl"),
                "basis_metric": row.get("policy_basis_metric", "unknown"),
                "basis_source": row.get("policy_basis_source") or row.get("source_phase", "unknown"),
                "basis": row.get("policy_basis") or row.get("basis_used", "unknown"),
                "notes": row.get("notes") or row.get("note", ""),
                "raw_winners_in_segment": row.get("raw_winners_in_segment", ""),
                "min_margin_pct": row.get("min_margin_pct", ""),
            }
        )
    return policies


def policy_for_length(
    policies: list[dict[str, object]],
    key_bits: int,
    length: int,
) -> dict[str, object] | None:
    for policy in policies:
        if (
            int(policy["key_bits"]) == key_bits
            and int(policy["start_len"]) <= length <= int(policy["end_len"])
        ):
            return policy
    return None


def print_run_meta_hint(summary_path: Path, policy_path: Path) -> None:
    for directory in sorted({summary_path.parent, policy_path.parent}):
        meta_path = directory / "run_meta.json"
        if not meta_path.exists():
            continue
        try:
            data = json.loads(meta_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            print(f"Run meta: {meta_path} (invalid JSON)")
            continue
        print(f"Run meta: {meta_path}")
        print(f"  timestamp: {data.get('timestamp') or data.get('benchmark_timestamp')}")
        print(f"  command_line: {data.get('command_line')}")


def main() -> None:
    args = parse_args()
    summary_path = Path(args.summary)
    policy_path = Path(args.policy)

    print(f"Summary input: {summary_path}")
    print(f"Policy input: {policy_path}")
    print_run_meta_hint(summary_path, policy_path)

    summary_rows = choose_summary_rows(
        read_csv(summary_path),
        run_id=args.run_id,
        scenario=args.scenario,
    )
    policies = load_policy(policy_path)
    best = raw_best_by_length(summary_rows, args.metric)

    mismatch_counts: Counter[int] = Counter()
    checked_counts: Counter[int] = Counter()
    missing_policy_counts: Counter[int] = Counter()

    for key_bits in sorted({key for key, _ in best}):
        print(f"\n[{key_bits}-bit]")
        for _, length in sorted(key for key in best if key[0] == key_bits):
            point = best[(key_bits, length)]
            policy = policy_for_length(policies, key_bits, length)
            if policy is None:
                missing_policy_counts[key_bits] += 1
                print(f"  len={length}: no policy interval")
                continue
            checked_counts[key_bits] += 1
            policy_impl = str(policy["policy_impl"])
            raw_impl = str(point["raw_best_impl"])
            if policy_impl == raw_impl:
                continue

            mismatch_counts[key_bits] += 1
            policy_value = point["values"].get(policy_impl)
            policy_value_text = f"{policy_value:.6f}" if policy_value is not None else "missing"
            print(
                "  "
                f"len={length}: raw_best={raw_impl}({point['raw_best_value']:.6f}) "
                f"policy={policy_impl}({policy_value_text}) "
                f"margin_pct={point['margin_pct']:.3f} "
                f"basis={policy['basis']} source={policy['basis_source']} "
                f"segment={policy['start_len']}-{policy['end_len']} "
                f"raw_winners={policy['raw_winners_in_segment']} "
                f"notes={policy['notes']}"
            )

    print("\nSummary")
    for key_bits in sorted({key for key, _ in best}):
        print(
            f"  {key_bits}-bit: checked={checked_counts[key_bits]} "
            f"mismatches={mismatch_counts[key_bits]} "
            f"missing_policy={missing_policy_counts[key_bits]}"
        )


if __name__ == "__main__":
    main()
