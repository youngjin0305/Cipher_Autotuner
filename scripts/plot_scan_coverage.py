#!/usr/bin/env python3
"""Plot the exact message lengths measured by Coarse and Fine Scan."""

from __future__ import annotations

import argparse

from plot_common import (
    configure_matplotlib,
    load_csv_rows,
    representative_ticks,
    resolve_repo_path,
    save_figure,
    style_axes,
    plt,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scan-points", required=True)
    parser.add_argument("--out-dir", default="figures")
    args = parser.parse_args()
    _, rows = load_csv_rows(
        resolve_repo_path(args.scan_points),
        required_columns=["key_bits", "input_len", "phase"],
    )
    key_sizes = sorted({int(row["key_bits"]) for row in rows})
    lengths = sorted({int(row["input_len"]) for row in rows})

    configure_matplotlib()
    fig, axes = plt.subplots(len(key_sizes), 1, figsize=(8.4, 5.3), sharex=True)
    if len(key_sizes) == 1:
        axes = [axes]
    colors = {"coarse": "#0173B2", "refined": "#D55E00"}
    labels = {"coarse": "Coarse Scan", "refined": "Fine Scan"}
    y_values = {"coarse": 1, "refined": 0}
    for ax, key_bits in zip(axes, key_sizes):
        for phase in ("coarse", "refined"):
            points = sorted(
                int(row["input_len"])
                for row in rows
                if int(row["key_bits"]) == key_bits and row["phase"] == phase
            )
            ax.scatter(
                points, [y_values[phase]] * len(points), s=22,
                color=colors[phase], marker="|", linewidths=1.6,
                label=labels[phase] if key_bits == key_sizes[0] else None,
            )
        ax.set_yticks([0, 1], ["Fine", "Coarse"])
        ax.set_ylim(-0.55, 1.55)
        ax.set_title(f"{key_bits}-bit key")
        style_axes(ax, y_minor_log=False)
        ax.grid(True, which="major", axis="x", color="#eef2f6", linewidth=0.7)
        ax.grid(False, axis="y")
    axes[-1].set_xscale("log", base=2)
    ticks = representative_ticks(lengths)
    axes[-1].set_xticks(ticks)
    axes[-1].set_xticklabels([str(value) for value in ticks])
    axes[-1].set_xlim(min(lengths), max(lengths))
    axes[-1].set_xlabel("Scanned input length (bytes)")
    handles, legend_labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles, legend_labels, loc="upper center", bbox_to_anchor=(0.5, 0.91),
        ncol=2, frameon=False,
    )
    fig.suptitle("Coarse and Fine Scan Coverage", fontsize=13)
    fig.tight_layout(rect=(0, 0, 1, 0.84))
    outputs = save_figure(fig, resolve_repo_path(args.out_dir), "fig_scan_coverage")
    plt.close(fig)
    for output in outputs:
        print(output)


if __name__ == "__main__":
    main()
