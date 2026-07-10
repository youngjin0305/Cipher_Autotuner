#!/usr/bin/env python3
"""Plot corrected trimmed mean ns/byte versus input length."""

from __future__ import annotations

from plot_common import (
    IMPLEMENTATION_ORDER,
    build_metric_parser,
    configure_matplotlib,
    display_label,
    filter_rows,
    filter_rows_by_length_range,
    filter_rows_by_key_bits,
    grouped_metric_by_impl,
    implementation_style,
    legend_style_kwargs,
    load_summary_rows,
    representative_ticks,
    resolve_metric_column,
    resolve_repo_path,
    save_figure,
    select_rows,
    style_axes,
    sorted_lengths,
    available_key_bits,
    plt,
)


def plot_for_key_bits(rows: list[dict[str, str]], output_dir, key_bits: int) -> None:
    """Render one ns/byte landscape figure for a single key size."""
    metric_column, metric_label, metric_stem = resolve_metric_column(rows, "ns_byte")
    series_by_impl = grouped_metric_by_impl(rows, IMPLEMENTATION_ORDER, metric_column)
    all_lengths = sorted_lengths(rows)
    tick_lengths = representative_ticks(all_lengths)

    configure_matplotlib()

    fig, ax = plt.subplots()
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")

    for impl in IMPLEMENTATION_ORDER:
        series = series_by_impl.get(impl, [])
        if not series:
            continue

        impl_lengths = [length for length, _ in series]
        metric_values = [value for _, value in series]
        style = implementation_style(impl)

        ax.plot(
            impl_lengths,
            metric_values,
            label=display_label(impl),
            linewidth=1.7,
            markersize=4.8,
            **style,
        )

    ax.set_xscale("log", base=2)
    ax.set_xticks(tick_lengths)
    ax.set_xticklabels([str(length) for length in tick_lengths])
    ax.set_yscale("log", base=10)
    ax.set_xlabel("Input length (bytes)")
    ax.set_ylabel(metric_label)
    ax.set_title(f"ARIA Performance Landscape ({key_bits}-bit key)")
    style_axes(ax, y_minor_log=True)
    ax.grid(True, which="major", axis="x", color="#eef2f6", linewidth=0.6)
    ax.grid(False, which="minor", axis="x")
    ax.legend(loc="upper right", **legend_style_kwargs())

    fig.tight_layout()
    outputs = save_figure(fig, output_dir, f"fig_perf_key{key_bits}_{metric_stem}")
    plt.close(fig)

    print("Saved figure outputs:")
    for output_path in outputs:
        print(output_path)


def main() -> None:
    parser = build_metric_parser(
        "Plot corrected trimmed mean ns/byte by input length for the main ARIA implementations.",
        default_metric="ns_byte",
    )
    args = parser.parse_args()

    input_path = resolve_repo_path(args.input)
    output_dir = resolve_repo_path(args.output_dir)
    print(f"Summary input: {input_path}")
    print(f"Figure output dir: {output_dir}")

    rows = load_summary_rows(input_path)
    rows = select_rows(rows, run_id=args.run_id, scenario=args.scenario)
    rows = filter_rows(rows, IMPLEMENTATION_ORDER)
    rows = filter_rows_by_length_range(rows, min_len=args.min_len, max_len=args.max_len)

    selected_key_bits = [args.key_bits] if args.key_bits is not None else available_key_bits(rows)
    for key_bits in selected_key_bits:
        key_rows = filter_rows_by_key_bits(rows, key_bits)
        plot_for_key_bits(key_rows, output_dir, key_bits)


if __name__ == "__main__":
    main()
