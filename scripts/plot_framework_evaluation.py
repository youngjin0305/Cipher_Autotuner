#!/usr/bin/env python3
"""Plot final framework performance, policy quality, and search efficiency."""

from __future__ import annotations

import argparse

from plot_common import configure_matplotlib, load_csv_rows, resolve_repo_path, save_figure, style_axes, plt


MODE_ORDER = [
    "direct_reference",
    "best_static_implementation",
    "static_heuristic_dispatch",
    "autotuned_policy_dispatch",
]
MODE_LABELS = {
    "direct_reference": "Direct\nReference",
    "best_static_implementation": "Best\nStatic",
    "static_heuristic_dispatch": "Static\nHeuristic",
    "autotuned_policy_dispatch": "Auto-tuned\nPolicy",
}
MODE_COLORS = ["#6B7280", "#CC78BC", "#0173B2", "#029E73"]


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--evaluation-summary", required=True)
    result.add_argument("--validation-summary", required=True)
    result.add_argument("--autotune-metrics", required=True)
    result.add_argument("--out-dir", "--output-dir", dest="output_dir", default="figures")
    return result


def ordered_modes(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    by_mode = {row["dispatch_mode"]: row for row in rows}
    missing = [mode for mode in MODE_ORDER if mode not in by_mode]
    if missing:
        raise SystemExit(f"error: missing dispatch modes: {', '.join(missing)}")
    return [by_mode[mode] for mode in MODE_ORDER]


def labels_on_bars(ax, bars, decimals: int = 2, suffix: str = "") -> None:
    for bar in bars:
        value = float(bar.get_height())
        ax.annotate(
            f"{value:.{decimals}f}{suffix}",
            (bar.get_x() + bar.get_width() / 2, value),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=7.2,
        )


def main() -> None:
    args = parser().parse_args()
    _, evaluation = load_csv_rows(
        resolve_repo_path(args.evaluation_summary),
        required_columns=[
            "scenario", "profile", "dispatch_mode", "macro_avg_trimmed_mean_ns_per_call",
            "geomean_speedup_vs_direct_ref", "end_to_end_autotune_time_ms", "trim_ratio",
        ],
    )
    _, validation = load_csv_rows(
        resolve_repo_path(args.validation_summary),
        required_columns=[
            "key_bits", "exact_policy_match_accuracy_percent", "within_1_percent_accuracy",
            "mean_policy_regret_percent", "max_policy_regret_percent",
            "mean_symmetric_boundary_distance_bytes", "mismatched_ranges",
        ],
    )
    _, metrics_rows = load_csv_rows(
        resolve_repo_path(args.autotune_metrics),
        required_columns=[
            "autotune_search_time_ms", "autotune_end_to_end_time_ms",
            "autotune_candidate_measurement_count", "exhaustive_candidate_measurement_count",
            "measurement_reduction_percent",
        ],
    )
    evaluation = ordered_modes(evaluation)
    overall_validation = next((row for row in validation if row["key_bits"] == "all"), None)
    validation = [row for row in validation if row["key_bits"] != "all"]
    validation.sort(key=lambda row: int(row["key_bits"]))
    metrics = metrics_rows[0]
    mode_labels = [MODE_LABELS[row["dispatch_mode"]] for row in evaluation]
    key_labels = [f"{row['key_bits']}-bit" for row in validation]

    configure_matplotlib()
    fig, axes = plt.subplots(2, 3, figsize=(13.2, 7.2))
    fig.patch.set_facecolor("white")

    times = [float(row["macro_avg_trimmed_mean_ns_per_call"]) for row in evaluation]
    bars = axes[0, 0].bar(mode_labels, times, color=MODE_COLORS, width=0.65)
    axes[0, 0].set_title("Macro-averaged Trimmed-Mean\nExecution Time")
    axes[0, 0].set_ylabel("Time (ns/call, lower is better)")
    axes[0, 0].set_ylim(0, max(times) * 1.22)
    style_axes(axes[0, 0], y_minor_log=False)
    labels_on_bars(axes[0, 0], bars, 1)

    speedups = [float(row["geomean_speedup_vs_direct_ref"]) for row in evaluation]
    bars = axes[0, 1].bar(mode_labels, speedups, color=MODE_COLORS, width=0.65)
    axes[0, 1].axhline(1.0, color="#475569", linestyle="--", linewidth=1)
    axes[0, 1].set_title("Geometric-Mean Speedup\nover Direct Reference")
    axes[0, 1].set_ylabel("Speedup (higher is better)")
    axes[0, 1].set_ylim(0, max(speedups) * 1.25)
    style_axes(axes[0, 1], y_minor_log=False)
    labels_on_bars(axes[0, 1], bars, 2, "×")

    x = list(range(len(validation)))
    width = 0.34
    exact = [float(row["exact_policy_match_accuracy_percent"]) for row in validation]
    within_one = [float(row["within_1_percent_accuracy"]) for row in validation]
    axes[0, 2].bar([value - width / 2 for value in x], exact, width, label="Exact Match", color="#0173B2")
    axes[0, 2].bar([value + width / 2 for value in x], within_one, width, label="Within 1%", color="#029E73")
    axes[0, 2].set_xticks(x, key_labels)
    axes[0, 2].set_ylim(0, 108)
    axes[0, 2].set_title("Policy Accuracy")
    axes[0, 2].set_ylabel("Accuracy (%)")
    axes[0, 2].legend(loc="lower left")
    style_axes(axes[0, 2], y_minor_log=False)

    mean_regret = [float(row["mean_policy_regret_percent"]) for row in validation]
    max_regret = [float(row["max_policy_regret_percent"]) for row in validation]
    axes[1, 0].bar([value - width / 2 for value in x], mean_regret, width, label="Mean", color="#DE8F05")
    axes[1, 0].bar([value + width / 2 for value in x], max_regret, width, label="Maximum", color="#D55E00")
    axes[1, 0].set_xticks(x, key_labels)
    axes[1, 0].set_title("Policy Regret vs. Exhaustive Reference")
    axes[1, 0].set_ylabel("Regret (%) (lower is better)")
    axes[1, 0].legend(loc="upper left")
    style_axes(axes[1, 0], y_minor_log=False)

    distances = [float(row["mean_symmetric_boundary_distance_bytes"]) for row in validation]
    bars = axes[1, 1].bar(key_labels, distances, color="#CC78BC", width=0.62)
    axes[1, 1].set_title("Mean Symmetric Boundary Distance")
    axes[1, 1].set_ylabel("Distance (bytes, lower is better)")
    axes[1, 1].set_ylim(0, max(distances) * 1.25 if max(distances, default=0) > 0 else 1)
    style_axes(axes[1, 1], y_minor_log=False)
    labels_on_bars(axes[1, 1], bars, 1)

    axes[1, 2].axis("off")
    mismatches = "; ".join(f"{row['key_bits']}-bit: {row['mismatched_ranges']}" for row in validation)
    overall_line = ""
    if overall_validation is not None:
        overall_line = (
            "Overall exact / within 1%: "
            f"{float(overall_validation['exact_policy_match_accuracy_percent']):.1f}% / "
            f"{float(overall_validation['within_1_percent_accuracy']):.1f}%\n"
        )
    info = (
        f"Search time: {float(metrics['autotune_search_time_ms']):.1f} ms\n"
        f"End-to-end tuning time: {float(metrics['autotune_end_to_end_time_ms']):.1f} ms\n"
        f"Autotune candidate measurements: {metrics['autotune_candidate_measurement_count']}\n"
        f"Exhaustive candidate measurements: {metrics['exhaustive_candidate_measurement_count']}\n"
        f"Measurement reduction: {float(metrics['measurement_reduction_percent']):.1f}%\n\n"
        f"{overall_line}"
        f"Mismatched ranges\n{mismatches}"
    )
    axes[1, 2].text(0.02, 0.95, info, va="top", ha="left", fontsize=10, linespacing=1.45,
                    bbox={"boxstyle": "round,pad=0.5", "facecolor": "#f8fafc", "edgecolor": "#cbd5e1"})

    scenario = evaluation[0]["scenario"]
    profile = evaluation[0]["profile"]
    trim_ratio = f"{float(evaluation[0]['trim_ratio']):.2f}"
    tuning_time_ms = float(metrics["autotune_end_to_end_time_ms"])
    fig.suptitle(
        f"Autotuning Framework Evaluation (scenario={scenario}, profile={profile}, "
        f"trim ratio={trim_ratio}, policy generation={tuning_time_ms:.1f} ms)",
        fontsize=14,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    for output in save_figure(fig, resolve_repo_path(args.output_dir), "fig_framework_evaluation"):
        print(output)
    plt.close(fig)


if __name__ == "__main__":
    main()
