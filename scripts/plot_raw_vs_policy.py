#!/usr/bin/env python3
"""Compare raw winners against the stabilized autotune policy."""

from __future__ import annotations

import argparse
import math

from plot_common import (
    IMPLEMENTATION_ORDER,
    DEFAULT_OUTPUT_DIR,
    configure_matplotlib,
    filter_rows,
    filter_rows_by_length_range,
    load_summary_rows,
    canonical_impl_from_effective_path,
    load_csv_rows,
    impl_legend_handles,
    implementation_style,
    load_autotune_rows,
    load_policy_rows,
    merge_length_regions,
    policy_note_is_mixed,
    region_fill_color,
    region_label,
    representative_ticks,
    resolve_metric_column,
    resolve_repo_path,
    save_figure,
    select_rows,
    style_axes,
    plt,
)


def build_parser() -> argparse.ArgumentParser:
    """Create the CLI parser for the raw-vs-policy comparison figure."""
    parser = argparse.ArgumentParser(
        description="Plot raw winner regions against the final stabilized autotune policy."
    )
    parser.add_argument("--summary", help="Path to summary_stats.csv")
    parser.add_argument("--raw-best", help="Path to raw_best_by_length.csv")
    parser.add_argument("--coarse", help="Path to autotune_coarse.csv")
    parser.add_argument("--refined", help="Path to autotune_refined.csv")
    parser.add_argument("--policy", required=True, help="Path to autotune_policy.json or CSV")
    parser.add_argument(
        "-o",
        "--output-dir",
        "--out-dir",
        dest="output_dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help=f"Directory for figure outputs (default: {DEFAULT_OUTPUT_DIR})",
    )
    parser.add_argument(
        "--key-bits",
        type=int,
        default=128,
        choices=[128, 192, 256],
        help="Key size to visualize (default: 128)",
    )
    parser.add_argument("--run-id", help="Only use summary rows from the specified run_id")
    parser.add_argument("--scenario", help="Only use summary rows from the specified scenario")
    parser.add_argument("--min-len", type=int, help="Only plot summary rows at or above this length")
    parser.add_argument("--max-len", type=int, help="Only plot summary rows at or below this length")
    return parser


def merge_autotune_rows(
    coarse_rows: list[dict[str, str]],
    refined_rows: list[dict[str, str]],
    key_bits: int,
) -> list[dict[str, str]]:
    """Merge coarse/refined rows, preferring refined rows for duplicate points."""
    merged: dict[tuple[int, int, str], dict[str, str]] = {}
    for row in coarse_rows:
        if int(row["key_bits"]) != key_bits:
            continue
        merged[(int(row["length"]), int(row["key_bits"]), row["impl"])] = row
    for row in refined_rows:
        if int(row["key_bits"]) != key_bits:
            continue
        merged[(int(row["length"]), int(row["key_bits"]), row["impl"])] = row
    return list(merged.values())


def raw_winner_regions(rows: list[dict[str, str]]) -> list[tuple[float, float, str]]:
    """Build merged raw winner regions from the autotune measurement rows."""
    lengths = sorted({int(row["length"]) for row in rows})
    winners: list[tuple[int, str]] = []
    for length in lengths:
        candidates = [
            row for row in rows
            if int(row["length"]) == length and int(row["is_raw_winner"]) == 1
        ]
        if not candidates:
            continue
        winners.append(
            (length, canonical_impl_from_effective_path(candidates[0]["effective_path"]))
        )
    if not winners:
        raise SystemExit("error: no raw winner rows were found in the supplied autotune CSVs")
    return merge_length_regions(winners)


def raw_winner_regions_from_best_csv(
    rows: list[dict[str, str]], key_bits: int
) -> list[tuple[float, float, str]]:
    """Build raw regions from the actual autotune winners and execution paths."""
    winners = [
        (
            int(row["input_len"]),
            canonical_impl_from_effective_path(row["raw_best_effective_path"]),
        )
        for row in rows
        if int(row["key_bits"]) == key_bits
    ]
    winners.sort(key=lambda item: item[0])
    if not winners:
        raise SystemExit(f"error: no raw winner rows found for key_bits={key_bits}")
    return merge_length_regions(winners)


def raw_winner_regions_from_summary(
    rows: list[dict[str, str]],
    key_bits: int,
) -> list[tuple[float, float, str]]:
    """Build raw winner regions directly from summary_stats.csv using ns/call."""
    key_rows = [row for row in rows if int(row["key_bits"]) == key_bits]
    if not key_rows:
        raise SystemExit(f"error: no summary rows found for key_bits={key_bits}")

    metric_column, _, _ = resolve_metric_column(key_rows, "ns_call")
    winners: list[tuple[int, str]] = []
    for length in sorted({int(row["len"]) for row in key_rows}):
        candidates = [row for row in key_rows if int(row["len"]) == length]
        candidates.sort(key=lambda row: float(row[metric_column]))
        winners.append(
            (
                length,
                canonical_impl_from_effective_path(candidates[0]["effective_path"]),
            )
        )

    if not winners:
        raise SystemExit("error: no raw winner rows were found in summary_stats.csv")
    return merge_length_regions(winners)


def plot_regions(
    ax, regions, *, title: str, x_min: int, x_max: int, hatch_mixed: bool = False
) -> None:
    """Render a single horizontal categorical region map."""
    x_values: list[int] = []
    for region in regions:
        left = region["left"] if isinstance(region, dict) else region[0]
        right = region["right"] if isinstance(region, dict) else region[1]
        x_values.extend([int(left), int(right)])

    tick_lengths = representative_ticks(sorted(set(x_values)))

    for region in regions:
        if isinstance(region, dict):
            left = float(region["left"])
            right = float(region["right"])
            impl = str(region["impl"])
            mixed = bool(region.get("mixed", False))
        else:
            left, right, impl = region
            mixed = False
        left = max(left, float(x_min))
        right = min(right, float(x_max))
        if right < left:
            continue
        style = implementation_style(impl)
        patch = ax.axvspan(
            left,
            right,
            ymin=0.12,
            ymax=0.88,
            facecolor=region_fill_color(impl),
            edgecolor=style["color"],
            linewidth=1.0,
            alpha=0.72,
            zorder=1,
        )
        if hatch_mixed and mixed:
            patch.set_hatch("//")
        # On the logarithmic x-axis, labels in later short SIMD-tail regions
        # overlap even when each individual span can fit its own text. Keep
        # those spans visible by color and label only sufficiently wide ones.
        if right > left and math.log2(right / left) >= 0.45:
            ax.text(
                (left * right) ** 0.5,
                0.5,
                region_label(impl),
                ha="center",
                va="center",
                fontsize=9,
                color=style["color"],
                zorder=2,
            )

    ax.set_xscale("log", base=2)
    ax.set_xticks(tick_lengths)
    ax.set_xticklabels([str(length) for length in tick_lengths])
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(0.0, 1.0)
    ax.set_yticks([])
    ax.set_title(title)
    style_axes(ax, y_minor_log=False)
    ax.grid(True, which="major", axis="x", color="#eef2f6", linewidth=0.7)
    ax.grid(False, axis="y")
    ax.spines["left"].set_visible(False)


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    policy_groups = load_policy_rows(resolve_repo_path(args.policy))
    output_dir = resolve_repo_path(args.output_dir)
    print(f"Policy input: {resolve_repo_path(args.policy)}")
    if args.summary:
        print(f"Summary input: {resolve_repo_path(args.summary)}")
    if args.coarse:
        print(f"Coarse input: {resolve_repo_path(args.coarse)}")
    if args.refined:
        print(f"Refined input: {resolve_repo_path(args.refined)}")
    print(f"Figure output dir: {output_dir}")

    if args.raw_best:
        _, best_rows = load_csv_rows(
            resolve_repo_path(args.raw_best),
            required_columns=[
                "key_bits", "input_len", "raw_best_impl", "raw_best_effective_path"
            ],
        )
        raw_regions = raw_winner_regions_from_best_csv(best_rows, args.key_bits)
    elif args.coarse and args.refined:
        coarse_rows = load_autotune_rows(resolve_repo_path(args.coarse))
        refined_rows = load_autotune_rows(resolve_repo_path(args.refined))
        merged_rows = merge_autotune_rows(coarse_rows, refined_rows, args.key_bits)
        raw_regions = raw_winner_regions(merged_rows)
    elif args.summary:
        summary_rows = load_summary_rows(resolve_repo_path(args.summary))
        summary_rows = select_rows(summary_rows, run_id=args.run_id, scenario=args.scenario)
        summary_rows = filter_rows(summary_rows, IMPLEMENTATION_ORDER)
        summary_rows = filter_rows_by_length_range(
            summary_rows,
            min_len=args.min_len,
            max_len=args.max_len,
        )
        raw_regions = raw_winner_regions_from_summary(summary_rows, args.key_bits)
    else:
        raise SystemExit(
            "error: provide --raw-best, --summary, or both --coarse and --refined"
        )

    policy_group = None
    for group in policy_groups:
        if int(group["key_bits"]) == args.key_bits:
            policy_group = group
            break
    if policy_group is None:
        raise SystemExit(f"error: no policy buckets found for key_bits={args.key_bits}")

    policy_regions = []
    for bucket in policy_group["buckets"]:
        policy_regions.append(
            {
                "left": int(bucket["start_len"]),
                "right": int(bucket["end_len"]),
                "impl": bucket["policy_chosen_impl"],
                "mixed": policy_note_is_mixed(
                    str(bucket.get("note", "")),
                    str(bucket.get("representative_effective_path", "")),
                ),
            }
        )

    x_min = min(int(bucket["start_len"]) for bucket in policy_group["buckets"])
    x_max = max(int(bucket["end_len"]) for bucket in policy_group["buckets"])
    visible_impls = [
        impl
        for impl in IMPLEMENTATION_ORDER
        if any(region[2] == impl for region in raw_regions)
        or any(region["impl"] == impl for region in policy_regions)
    ]

    configure_matplotlib()
    fig, axes = plt.subplots(2, 1, figsize=(7.2, 4.2), sharex=True)
    fig.patch.set_facecolor("white")

    plot_regions(
        axes[0], raw_regions, title="Raw Winner Map",
        x_min=x_min, x_max=x_max,
    )
    plot_regions(
        axes[1],
        policy_regions,
        title="Stabilized Policy Map",
        x_min=x_min,
        x_max=x_max,
        hatch_mixed=True,
    )
    axes[1].set_xlabel("Input length (bytes)")
    fig.suptitle(f"Raw Winner vs. Stabilized Policy ({args.key_bits}-bit key)", fontsize=13)
    fig.legend(
        handles=impl_legend_handles(visible_impls), loc="upper center",
        bbox_to_anchor=(0.5, 0.90), ncol=len(visible_impls), frameon=False,
    )

    fig.tight_layout(rect=(0, 0, 1, 0.80))
    outputs = save_figure(fig, output_dir, f"fig_raw_vs_policy_key{args.key_bits}")
    plt.close(fig)

    print("Saved figure outputs:")
    for output_path in outputs:
        print(output_path)


if __name__ == "__main__":
    main()
