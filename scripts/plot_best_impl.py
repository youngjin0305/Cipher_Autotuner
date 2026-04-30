#!/usr/bin/env python3
"""Plot the best implementation at each input length."""

from __future__ import annotations

from plot_common import (
    IMPLEMENTATION_ORDER,
    available_key_bits,
    build_metric_parser,
    canonical_impl_from_effective_path,
    configure_matplotlib,
    filter_rows,
    filter_rows_by_key_bits,
    implementation_style,
    load_summary_rows,
    merge_length_regions,
    region_fill_color,
    region_label,
    representative_ticks,
    resolve_metric_column,
    resolve_repo_path,
    save_figure,
    select_rows,
    style_axes,
    sorted_lengths,
    geometric_midpoint,
    plt,
)


def main() -> None:
    parser = build_metric_parser(
        "Plot the winning implementation at each input length for a selected metric.",
        default_metric="ns_byte",
    )
    args = parser.parse_args()

    input_path = resolve_repo_path(args.input)
    output_dir = resolve_repo_path(args.output_dir)

    rows = load_summary_rows(input_path)
    rows = select_rows(rows, run_id=args.run_id, scenario=args.scenario)
    rows = filter_rows(rows, IMPLEMENTATION_ORDER)
    metric_column, _, metric_stem = resolve_metric_column(rows, args.metric)

    selected_key_bits = [args.key_bits] if args.key_bits is not None else available_key_bits(rows)
    configure_matplotlib()

    for key_bits in selected_key_bits:
        key_rows = filter_rows_by_key_bits(rows, key_bits)
        all_lengths = sorted_lengths(key_rows)
        tick_lengths = representative_ticks(all_lengths)

        best_by_length: list[tuple[int, str]] = []
        for length in all_lengths:
            length_rows = [row for row in key_rows if int(row["len"]) == length]
            winner = min(length_rows, key=lambda row: float(row[metric_column]))
            canonical_impl = canonical_impl_from_effective_path(winner["effective_path"])
            best_by_length.append((length, canonical_impl))

        regions = merge_length_regions(best_by_length)

        fig, ax = plt.subplots(figsize=(6.8, 1.8))
        fig.patch.set_facecolor("white")
        ax.set_facecolor("white")

        for left, right, impl in regions:
            style = implementation_style(impl)
            ax.axvspan(
                left,
                right,
                facecolor=region_fill_color(impl),
                edgecolor="none",
                alpha=0.55,
                linewidth=0,
                zorder=0,
            )
            ax.text(
                geometric_midpoint(left, right),
                0.5,
                region_label(impl),
                ha="center",
                va="center",
                fontsize=9,
                color=style["color"],
                bbox={
                    "boxstyle": "round,pad=0.22",
                    "facecolor": "white",
                    "edgecolor": "none",
                    "alpha": 0.9,
                },
            )

        ax.vlines(all_lengths, ymin=0.08, ymax=0.16, colors="#9aa4af", linewidth=0.7)
        ax.set_xscale("log", base=2)
        ax.set_xticks(tick_lengths)
        ax.set_xticklabels([str(length) for length in tick_lengths])
        ax.set_xlim(regions[0][0], regions[-1][1])
        ax.set_ylim(0.0, 1.0)
        ax.set_xlabel("Input length (bytes)")
        ax.set_title(f"Best Implementation Regions ({key_bits}-bit key)")
        ax.set_yticks([])
        style_axes(ax, y_minor_log=False)
        ax.grid(True, which="major", axis="x", color="#eef2f6", linewidth=0.7)
        ax.grid(False, axis="y")
        ax.spines["left"].set_visible(False)
        ax.spines["bottom"].set_linewidth(0.9)

        fig.tight_layout()
        outputs = save_figure(fig, output_dir, f"fig_best_impl_key{key_bits}_{metric_stem}")
        plt.close(fig)

        print("Saved figure outputs:")
        for output_path in outputs:
            print(output_path)


if __name__ == "__main__":
    main()
