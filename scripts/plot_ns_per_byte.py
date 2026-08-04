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
    load_csv_rows,
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


def plot_for_key_bits(
    rows: list[dict[str, str]], output_dir, key_bits: int, *, dense: bool = False
) -> None:
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
        if dense:
            style["marker"] = None

        ax.plot(
            impl_lengths,
            metric_values,
            label=display_label(impl),
            linewidth=1.25 if dense else 1.7,
            markersize=0 if dense else 4.8,
            **style,
        )

    present_impls = {impl for impl, series in series_by_impl.items() if series}
    structural_boundaries = []
    if "linux_aesni_avx" in present_impls:
        structural_boundaries.append((256, "AVX 16-way"))
    if "linux_aesni_avx2" in present_impls:
        structural_boundaries.append((512, "AVX2 32-way"))
    if "linux_gfni_avx512" in present_impls:
        structural_boundaries.append((1024, "GFNI 64-way"))
    for boundary, label in structural_boundaries:
        if all_lengths[0] <= boundary <= all_lengths[-1]:
            ax.axvline(boundary, color="#94a3b8", linestyle=":", linewidth=0.9, zorder=0)
            ax.annotate(
                label, (boundary, 1.0), xycoords=("data", "axes fraction"),
                xytext=(3, -4), textcoords="offset points", rotation=90,
                ha="left", va="top", fontsize=7.2, color="#64748b",
            )

    ax.set_xscale("log", base=2)
    ax.set_xticks(tick_lengths)
    ax.set_xticklabels([str(length) for length in tick_lengths])
    ax.set_yscale("log", base=10)
    ax.set_xlabel("Input length (bytes)")
    ax.set_ylabel(metric_label)
    prefix = "Dense " if dense else ""
    ax.set_title(f"{prefix}ARIA Performance Landscape ({key_bits}-bit key)")
    style_axes(ax, y_minor_log=True)
    ax.grid(True, which="major", axis="x", color="#eef2f6", linewidth=0.6)
    ax.grid(False, which="minor", axis="x")
    ax.legend(loc="lower left" if dense else "upper right", **legend_style_kwargs())

    fig.tight_layout()
    outputs = save_figure(fig, output_dir, f"fig_perf_key{key_bits}_{metric_stem}")
    plt.close(fig)

    print("Saved figure outputs:")
    for output_path in outputs:
        print(output_path)


def validation_rows(csv_path, *, min_len=None, max_len=None) -> list[dict[str, str]]:
    """Convert exhaustive policy-validation candidate times to plotting rows."""
    _, source_rows = load_csv_rows(
        csv_path,
        required_columns=[
            "key_bits", "message_length", "ref_trimmed_mean_ns_per_call",
            "linux_aesni_avx_trimmed_mean_ns_per_call",
            "linux_aesni_avx2_trimmed_mean_ns_per_call",
            "linux_gfni_avx512_trimmed_mean_ns_per_call",
        ],
    )
    columns = {
        "ref": "ref_trimmed_mean_ns_per_call",
        "linux_aesni_avx": "linux_aesni_avx_trimmed_mean_ns_per_call",
        "linux_aesni_avx2": "linux_aesni_avx2_trimmed_mean_ns_per_call",
        "linux_gfni_avx512": "linux_gfni_avx512_trimmed_mean_ns_per_call",
    }
    rows: list[dict[str, str]] = []
    for source in source_rows:
        length = int(source["message_length"])
        if min_len is not None and length < min_len:
            continue
        if max_len is not None and length > max_len:
            continue
        for impl, column in columns.items():
            ns_call = float(source[column])
            if ns_call < 0:
                continue
            rows.append(
                {
                    "key_bits": source["key_bits"],
                    "len": str(length),
                    "impl": impl,
                    "trimmed_mean_ns_per_call": f"{ns_call:.9f}",
                    "trimmed_mean_ns_per_byte": f"{ns_call / length:.12f}",
                }
            )
    if not rows:
        raise SystemExit("error: no usable exhaustive validation measurements")
    return rows


def main() -> None:
    parser = build_metric_parser(
        "Plot corrected trimmed mean ns/byte by input length for the main ARIA implementations.",
        default_metric="ns_byte",
    )
    parser.add_argument(
        "--validation",
        help="Use dense exhaustive measurements from policy_validation.csv",
    )
    args = parser.parse_args()

    output_dir = resolve_repo_path(args.output_dir)
    print(f"Figure output dir: {output_dir}")
    dense = args.validation is not None
    if dense:
        validation_path = resolve_repo_path(args.validation)
        print(f"Exhaustive validation input: {validation_path}")
        rows = validation_rows(validation_path, min_len=args.min_len, max_len=args.max_len)
    else:
        input_path = resolve_repo_path(args.input)
        print(f"Summary input: {input_path}")
        rows = load_summary_rows(input_path)
        rows = select_rows(rows, run_id=args.run_id, scenario=args.scenario)
        rows = filter_rows(rows, IMPLEMENTATION_ORDER)
        rows = filter_rows_by_length_range(rows, min_len=args.min_len, max_len=args.max_len)

    selected_key_bits = [args.key_bits] if args.key_bits is not None else available_key_bits(rows)
    for key_bits in selected_key_bits:
        key_rows = filter_rows_by_key_bits(rows, key_bits)
        plot_for_key_bits(key_rows, output_dir, key_bits, dense=dense)


if __name__ == "__main__":
    main()
