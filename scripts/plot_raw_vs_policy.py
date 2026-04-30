#!/usr/bin/env python3
"""Compare raw winners against the stabilized autotune policy."""

from __future__ import annotations

import argparse

from plot_common import (
    IMPLEMENTATION_ORDER,
    DEFAULT_OUTPUT_DIR,
    configure_matplotlib,
    impl_legend_handles,
    implementation_style,
    load_autotune_rows,
    load_policy_rows,
    merge_length_regions,
    policy_note_is_mixed,
    region_fill_color,
    region_label,
    representative_ticks,
    resolve_repo_path,
    save_figure,
    style_axes,
    plt,
)


def build_parser() -> argparse.ArgumentParser:
    """Create the CLI parser for the raw-vs-policy comparison figure."""
    parser = argparse.ArgumentParser(
        description="Plot raw winner regions against the final stabilized autotune policy."
    )
    parser.add_argument("--coarse", required=True, help="Path to autotune_coarse.csv")
    parser.add_argument("--refined", required=True, help="Path to autotune_refined.csv")
    parser.add_argument("--policy", required=True, help="Path to autotune_policy.json or CSV")
    parser.add_argument(
        "-o",
        "--output-dir",
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
        winners.append((length, candidates[0]["impl"]))
    if not winners:
        raise SystemExit("error: no raw winner rows were found in the supplied autotune CSVs")
    return merge_length_regions(winners)


def plot_regions(ax, regions, *, title: str, hatch_mixed: bool = False) -> None:
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

    coarse_rows = load_autotune_rows(resolve_repo_path(args.coarse))
    refined_rows = load_autotune_rows(resolve_repo_path(args.refined))
    policy_groups = load_policy_rows(resolve_repo_path(args.policy))
    output_dir = resolve_repo_path(args.output_dir)

    merged_rows = merge_autotune_rows(coarse_rows, refined_rows, args.key_bits)
    raw_regions = raw_winner_regions(merged_rows)

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

    configure_matplotlib()
    fig, axes = plt.subplots(2, 1, figsize=(7.2, 3.6), sharex=True)
    fig.patch.set_facecolor("white")

    plot_regions(axes[0], raw_regions, title=f"Raw Winner Map ({args.key_bits}-bit key)")
    plot_regions(
        axes[1],
        policy_regions,
        title=f"Stabilized Policy Map ({args.key_bits}-bit key)",
        hatch_mixed=True,
    )
    axes[1].set_xlabel("Input length (bytes)")
    axes[0].legend(handles=impl_legend_handles(), loc="upper center", ncol=3, frameon=False)

    fig.tight_layout()
    outputs = save_figure(fig, output_dir, f"fig_raw_vs_policy_key{args.key_bits}")
    plt.close(fig)

    print("Saved figure outputs:")
    for output_path in outputs:
        print(output_path)


if __name__ == "__main__":
    main()
