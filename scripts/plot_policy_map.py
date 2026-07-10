#!/usr/bin/env python3
"""Plot stabilized autotune policy buckets."""

from __future__ import annotations

from plot_common import (
    IMPLEMENTATION_ORDER,
    build_parser,
    configure_matplotlib,
    impl_legend_handles,
    implementation_style,
    load_policy_rows,
    policy_note_is_mixed,
    region_fill_color,
    region_label,
    representative_ticks,
    resolve_repo_path,
    save_figure,
    style_axes,
    plt,
)


def plot_policy_group(group: dict[str, object], output_dir) -> None:
    """Render one policy map for a single key size."""
    key_bits = int(group["key_bits"])
    buckets = list(group["buckets"])
    if not buckets:
        return

    lengths: list[int] = []
    basis_metrics: set[str] = set()
    basis_sources: set[str] = set()
    for bucket in buckets:
        lengths.append(int(bucket["start_len"]))
        lengths.append(int(bucket["end_len"]))
        basis_metrics.add(str(bucket.get("policy_basis_metric", "ns_per_call")))
        basis_sources.add(str(bucket.get("source_phase", bucket.get("policy_basis_source", ""))))
    tick_lengths = representative_ticks(sorted(set(lengths)))
    policy_source = str(group.get("source_file", "unknown"))
    basis_metric = ",".join(sorted(basis_metrics))
    basis_source = ",".join(sorted(source for source in basis_sources if source)) or "unknown"

    configure_matplotlib()
    fig, ax = plt.subplots(figsize=(7.2, 1.9))
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")

    for bucket in buckets:
        impl = str(bucket["policy_chosen_impl"])
        left = int(bucket["start_len"])
        right = int(bucket["end_len"])
        note = str(bucket.get("note", ""))
        effective_path = str(bucket.get("representative_effective_path", ""))
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
        if policy_note_is_mixed(note, effective_path):
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
    ax.set_xlim(min(lengths), max(lengths))
    ax.set_ylim(0.0, 1.0)
    ax.set_yticks([])
    ax.set_xlabel("Input length (bytes)")
    ax.set_title(f"Autotune Policy Map ({key_bits}-bit key, {basis_metric})")
    style_axes(ax, y_minor_log=False)
    ax.grid(True, which="major", axis="x", color="#eef2f6", linewidth=0.7)
    ax.grid(False, axis="y")
    ax.spines["left"].set_visible(False)
    ax.legend(handles=impl_legend_handles(), loc="upper center", ncol=3, frameon=False)
    fig.text(
        0.5,
        0.01,
        f"range={min(lengths)}-{max(lengths)} bytes | source={policy_source} | policy_source={basis_source}",
        ha="center",
        va="bottom",
        fontsize=7.5,
        color="#64748b",
    )

    fig.tight_layout(rect=(0, 0.08, 1, 1))
    outputs = save_figure(fig, output_dir, f"fig_policy_key{key_bits}")
    plt.close(fig)

    print("Saved figure outputs:")
    for output_path in outputs:
        print(output_path)


def main() -> None:
    parser = build_parser("Plot stabilized policy buckets from autotune policy JSON or CSV.")
    parser.set_defaults(input="out/policy_full_debug/autotune_policy.json")
    args = parser.parse_args()

    input_path = resolve_repo_path(args.input)
    output_dir = resolve_repo_path(args.output_dir)
    print(f"Policy input: {input_path}")
    print(f"Figure output dir: {output_dir}")
    policy_groups = load_policy_rows(input_path)

    if args.key_bits is not None:
        policy_groups = [group for group in policy_groups if int(group["key_bits"]) == args.key_bits]
        if not policy_groups:
            raise SystemExit(f"error: no policy buckets found for key_bits={args.key_bits}")

    for group in policy_groups:
        plot_policy_group(group, output_dir)


if __name__ == "__main__":
    main()
