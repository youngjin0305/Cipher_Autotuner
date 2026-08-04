#!/usr/bin/env python3
"""Compare Best Fixed Implementation, Static Heuristic, and Auto-tuned maps."""

from __future__ import annotations

import argparse
import json

from plot_common import (
    IMPLEMENTATION_ORDER,
    configure_matplotlib,
    impl_legend_handles,
    implementation_style,
    load_csv_rows,
    load_policy_rows,
    region_fill_color,
    region_label,
    representative_ticks,
    resolve_repo_path,
    save_figure,
    style_axes,
    plt,
)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--evaluation", required=True, help="dispatch_evaluation.csv")
    result.add_argument("--policy", required=True, help="autotune_policy.csv or JSON")
    result.add_argument("--run-meta", required=True, help="run_meta.json")
    result.add_argument("--key-bits", type=int, default=128, choices=[128, 192, 256])
    result.add_argument("--out-dir", default="figures")
    return result


def merge_grid(grid: list[tuple[int, str]]) -> list[tuple[int, int, str]]:
    regions: list[tuple[int, int, str]] = []
    for length, impl in grid:
        if regions and regions[-1][2] == impl:
            regions[-1] = (regions[-1][0], length, impl)
        else:
            regions.append((length, length, impl))
    return regions


def draw_map(ax, regions: list[tuple[int, int, str]], title: str) -> None:
    for left, right, impl in regions:
        style = implementation_style(impl)
        ax.axvspan(
            left, right, ymin=0.12, ymax=0.88,
            facecolor=region_fill_color(impl), edgecolor=style["color"],
            linewidth=1.0, alpha=0.72,
        )
        ax.text(
            (left * right) ** 0.5, 0.5, region_label(impl),
            ha="center", va="center", fontsize=9, color=style["color"],
        )
    ax.set_ylim(0, 1)
    ax.set_yticks([])
    ax.set_title(title)
    style_axes(ax, y_minor_log=False)
    ax.grid(True, which="major", axis="x", color="#eef2f6", linewidth=0.7)
    ax.grid(False, axis="y")
    ax.spines["left"].set_visible(False)


def main() -> None:
    args = parser().parse_args()
    _, evaluation = load_csv_rows(
        resolve_repo_path(args.evaluation),
        required_columns=["dispatch_mode", "key_bits", "selected_implementation"],
    )
    with resolve_repo_path(args.run_meta).open(encoding="utf-8") as handle:
        meta = json.load(handle)
    policy_groups = load_policy_rows(resolve_repo_path(args.policy))
    group = next(
        (item for item in policy_groups if int(item["key_bits"]) == args.key_bits), None
    )
    if group is None:
        raise SystemExit(f"error: no policy for key_bits={args.key_bits}")

    buckets = list(group["buckets"])
    min_len = min(int(item["start_len"]) for item in buckets)
    max_len = max(int(item["end_len"]) for item in buckets)
    best_fixed = {
        row["selected_implementation"]
        for row in evaluation
        if row["dispatch_mode"] in {
            "best_fixed_implementation", "best_static_implementation"
        }
    }
    if len(best_fixed) != 1:
        raise SystemExit(
            "error: evaluation does not identify one Best Fixed Implementation"
        )
    best_impl = next(iter(best_fixed))

    active = meta.get("candidate_implementations", {})
    scenario = evaluation[0].get("scenario", "unknown")

    def heuristic(length: int) -> str:
        if scenario == "lowpower":
            return "ref"
        if length >= 1024 and active.get("linux_gfni_avx512") == "active":
            return "linux_gfni_avx512"
        if length >= 512 and active.get("linux_aesni_avx2") == "active":
            return "linux_aesni_avx2"
        if length >= 256 and active.get("linux_aesni_avx") == "active":
            return "linux_aesni_avx"
        return "ref"

    grid = list(range(min_len, max_len + 1, 16))
    maps = [
        [(min_len, max_len, best_impl)],
        merge_grid([(length, heuristic(length)) for length in grid]),
        [
            (int(item["start_len"]), int(item["end_len"]), str(item["policy_chosen_impl"]))
            for item in buckets
        ],
    ]
    titles = ["Best Fixed Implementation", "Static Heuristic", "Auto-tuned Policy"]
    visible = [
        impl for impl in IMPLEMENTATION_ORDER
        if any(region[2] == impl for regions in maps for region in regions)
    ]

    configure_matplotlib()
    fig, axes = plt.subplots(3, 1, figsize=(8.0, 5.0), sharex=True)
    for ax, regions, title in zip(axes, maps, titles):
        draw_map(ax, regions, title)
    axes[-1].set_xscale("log", base=2)
    ticks = representative_ticks(grid)
    axes[-1].set_xticks(ticks)
    axes[-1].set_xticklabels([str(value) for value in ticks])
    axes[-1].set_xlim(min_len, max_len)
    axes[-1].set_xlabel("Input length (bytes)")
    fig.legend(
        handles=impl_legend_handles(visible), loc="upper center",
        bbox_to_anchor=(0.5, 0.91), ncol=len(visible), frameon=False,
    )
    fig.suptitle(
        f"Policy Map Comparison ({args.key_bits}-bit key, scenario={scenario})", fontsize=13
    )
    fig.tight_layout(rect=(0, 0, 1, 0.84))
    outputs = save_figure(
        fig, resolve_repo_path(args.out_dir),
        f"fig_policy_comparison_key{args.key_bits}",
    )
    plt.close(fig)
    for output in outputs:
        print(output)


if __name__ == "__main__":
    main()
