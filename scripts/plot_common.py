#!/usr/bin/env python3
"""Shared helpers for benchmark plotting scripts."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cache")

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.ticker import LogLocator

DEFAULT_INPUT = Path("out/summary_stats.csv")
DEFAULT_OUTPUT_DIR = Path("figures")
DEFAULT_POLICY_INPUT = Path("out/autotune_policy.json")
DEFAULT_COARSE_INPUT = Path("out/autotune_coarse.csv")
DEFAULT_REFINED_INPUT = Path("out/autotune_refined.csv")
IMPLEMENTATION_ORDER = [
    "ref",
    "linux_aesni_avx",
    "linux_aesni_avx2",
    "linux_gfni_avx512",
]
DISPLAY_LABELS = {
    "ref": "Ref",
    "linux_aesni_avx": "AES-NI+AVX",
    "linux_aesni_avx2": "AES-NI+AVX2",
    "linux_gfni_avx512": "GFNI+AVX-512",
}
REGION_LABELS = {
    "ref": "Ref",
    "linux_aesni_avx": "AVX",
    "linux_aesni_avx2": "AVX2",
    "linux_gfni_avx512": "GFNI/AVX-512",
}
PLOT_STYLES = {
    "ref": {
        "color": "#0173B2",
        "linestyle": "-",
        "marker": "o",
        "markerfacecolor": "#0173B2",
        "markeredgecolor": "#0173B2",
        "markeredgewidth": 0.0,
    },
    "linux_aesni_avx": {
        "color": "#DE8F05",
        "linestyle": "-",
        "marker": "o",
        "markerfacecolor": "#DE8F05",
        "markeredgecolor": "#DE8F05",
        "markeredgewidth": 0.0,
    },
    "linux_aesni_avx2": {
        "color": "#029E73",
        "linestyle": "-",
        "marker": "o",
        "markerfacecolor": "#029E73",
        "markeredgecolor": "#029E73",
        "markeredgewidth": 0.0,
    },
    "linux_gfni_avx512": {
        "color": "#CC78BC",
        "linestyle": "-",
        "marker": "o",
        "markerfacecolor": "#CC78BC",
        "markeredgecolor": "#CC78BC",
        "markeredgewidth": 0.0,
    },
}
REGION_FILL_COLORS = {
    "ref": "#dbeafe",
    "linux_aesni_avx": "#fee2e2",
    "linux_aesni_avx2": "#dcfce7",
    "linux_gfni_avx512": "#f3e8ff",
}
POLICY_HATCH = {
    "plain": None,
    "mixed": "//",
}
REPRESENTATIVE_TICKS = [16, 32, 64, 128, 256, 512, 1024, 4096, 16384]
SUMMARY_DEFAULT_KEY_BITS = 128
METRIC_SPECS = {
    "ns_byte": {
        "candidates": [
            "trimmed_mean_ns_per_byte",
            "ns_per_byte",
            "ns_per_byte_trimmed_mean_corrected",
            "median_ns_per_byte",
        ],
        "label": "Time per byte (ns/byte)",
        "stem": "ns_byte",
    },
    "ns_call": {
        "candidates": [
            "trimmed_mean_ns_per_call",
            "ns_per_call",
            "ns_per_call_trimmed_mean_corrected",
            "ns_per_call_trimmed_mean",
            "median_ns_per_call",
            "ns_per_call_p50",
        ],
        "label": "Time per call (ns/call)",
        "stem": "ns_call",
    },
}
EFFECTIVE_PATH_TO_IMPL = {
    "ref": "ref",
    "ref_fallback": "ref",
    "linux_aesni_avx": "linux_aesni_avx",
    "linux_aesni_avx_plus_ref_tail": "linux_aesni_avx",
    "linux_aesni_avx2": "linux_aesni_avx2",
    "linux_aesni_avx2_plus_avx_tail": "linux_aesni_avx2",
    "linux_aesni_avx2_plus_ref_tail": "linux_aesni_avx2",
    "linux_aesni_avx2_plus_avx_ref_tail": "linux_aesni_avx2",
    "linux_gfni_avx512": "linux_gfni_avx512",
    "linux_gfni_avx_16way": "linux_gfni_avx512",
    "linux_gfni_avx2_32way": "linux_gfni_avx512",
    "linux_gfni_mixed_width": "linux_gfni_avx512",
    "linux_gfni_plus_ref_tail": "linux_gfni_avx512",
    "mixed_effective_path": "ref",
}


def build_parser(description: str) -> argparse.ArgumentParser:
    """Create a standard CLI parser for plotting scripts."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "-i",
        "--input",
        "--policy",
        dest="input",
        default=str(DEFAULT_INPUT),
        help=f"Path to summary benchmark CSV (default: {DEFAULT_INPUT})",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        "--out-dir",
        dest="output_dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help=f"Directory for figure outputs (default: {DEFAULT_OUTPUT_DIR})",
    )
    parser.add_argument(
        "--run-id",
        help="Only use rows from the specified run_id",
    )
    parser.add_argument(
        "--scenario",
        help="Only use rows from the specified scenario",
    )
    parser.add_argument(
        "--key-bits",
        type=int,
        choices=[128, 192, 256],
        help="Only plot rows for the specified ARIA key size",
    )
    parser.add_argument(
        "--min-len",
        type=int,
        help="Only plot input lengths greater than or equal to this value",
    )
    parser.add_argument(
        "--max-len",
        type=int,
        help="Only plot input lengths less than or equal to this value",
    )
    return parser


def build_metric_parser(
    description: str,
    *,
    default_metric: str,
) -> argparse.ArgumentParser:
    """Create a parser for line plots that supports metric selection."""
    parser = build_parser(description)
    parser.add_argument(
        "--metric",
        default=default_metric,
        choices=sorted(METRIC_SPECS.keys()),
        help=f"Metric alias to plot (default: {default_metric})",
    )
    return parser


def repo_root() -> Path:
    """Return the repository root based on this script location."""
    return Path(__file__).resolve().parent.parent


def resolve_repo_path(path_str: str) -> Path:
    """Resolve relative paths from the repository root."""
    path = Path(path_str)
    if path.is_absolute():
        return path
    return repo_root() / path


def fail(message: str) -> None:
    """Exit with a clear human-readable error."""
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def available_columns_message(columns: list[str]) -> str:
    """Return a compact string for error messages."""
    return ", ".join(columns) if columns else "<no columns>"


def require_columns(
    fieldnames: list[str],
    required_columns: list[str],
    path: Path,
) -> None:
    """Validate that the required columns are present."""
    missing = [column for column in required_columns if column not in fieldnames]
    if missing:
        fail(
            f"input file is missing required columns: {', '.join(missing)} ({path}). "
            f"Found columns: {available_columns_message(fieldnames)}"
        )


def load_csv_rows(
    csv_path: Path,
    *,
    required_columns: list[str] | None = None,
) -> tuple[list[str], list[dict[str, str]]]:
    """Load a CSV with validation and return fieldnames plus rows."""
    if not csv_path.exists():
        fail(f"input CSV not found: {csv_path}")

    try:
        with csv_path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                fail(f"input CSV has no header row: {csv_path}")
            fieldnames = list(reader.fieldnames)
            if required_columns:
                require_columns(fieldnames, required_columns, csv_path)
            rows = list(reader)
    except OSError as exc:
        fail(f"failed to read input CSV {csv_path}: {exc}")

    if not rows:
        fail(f"input CSV is empty: {csv_path}")

    return fieldnames, rows


def attach_summary_key_bits(
    rows: list[dict[str, str]],
    fieldnames: list[str],
) -> list[dict[str, str]]:
    """Ensure summary rows expose key_bits, inferring 128-bit for legacy CSVs."""
    if "key_bits" in fieldnames:
        return rows

    normalized_rows: list[dict[str, str]] = []
    for row in rows:
        normalized = dict(row)
        normalized["key_bits"] = str(SUMMARY_DEFAULT_KEY_BITS)
        normalized_rows.append(normalized)
    return normalized_rows


def load_summary_rows(csv_path: Path) -> list[dict[str, str]]:
    """Load summary CSV rows and validate the shared columns."""
    fieldnames, rows = load_csv_rows(
        csv_path,
        required_columns=["run_id", "scenario", "len", "impl", "effective_path"],
    )
    return attach_summary_key_bits(rows, fieldnames)


def resolve_metric_column(rows: list[dict[str, str]], metric_alias: str) -> tuple[str, str, str]:
    """Resolve a metric alias to a concrete column name plus label and filename stem."""
    if not rows:
        fail("cannot resolve a metric column from empty input rows")
    if metric_alias not in METRIC_SPECS:
        fail(f"unknown metric alias '{metric_alias}'")

    columns = list(rows[0].keys())
    spec = METRIC_SPECS[metric_alias]
    for candidate in spec["candidates"]:
        if candidate in columns:
            return candidate, spec["label"], spec["stem"]

    fail(
        f"could not resolve metric '{metric_alias}'. "
        f"Tried columns: {', '.join(spec['candidates'])}. "
        f"Found columns: {available_columns_message(columns)}"
    )
    raise AssertionError("unreachable")


def load_policy_json(policy_path: Path) -> dict[str, object]:
    """Load and validate autotune_policy.json."""
    if not policy_path.exists():
        fail(f"policy JSON not found: {policy_path}")

    try:
        with policy_path.open(encoding="utf-8") as handle:
            data = json.load(handle)
    except OSError as exc:
        fail(f"failed to read policy JSON {policy_path}: {exc}")
    except json.JSONDecodeError as exc:
        fail(f"failed to parse policy JSON {policy_path}: {exc}")

    if "policy" not in data or not isinstance(data["policy"], list):
        fail(f"policy JSON does not contain a top-level 'policy' list: {policy_path}")
    return data


def load_policy_rows(policy_path: Path) -> list[dict[str, object]]:
    """Load policy data from JSON first, then CSV as a fallback."""
    if policy_path.suffix.lower() == ".json":
        data = load_policy_json(policy_path)
        groups = list(data["policy"])
        for group in groups:
            if isinstance(group, dict):
                group["source_file"] = str(policy_path)
        return groups

    fieldnames, rows = load_csv_rows(
        policy_path,
        required_columns=[
            "key_bits",
            "start_len",
            "end_len",
            "raw_chosen_impl",
            "policy_chosen_impl",
            "representative_effective_path",
            "note",
            "source_phase",
            "basis_used",
            "bucket_points",
        ],
    )
    _ = fieldnames
    grouped: dict[int, dict[str, object]] = {}
    for row in rows:
        key_bits = int(row["key_bits"])
        entry = grouped.setdefault(
            key_bits,
            {
                "key_bits": key_bits,
                "basis_used": row["basis_used"],
                "source_file": str(policy_path),
                "raw_threshold_summary": "unavailable_from_csv",
                "policy_threshold_summary": "unavailable_from_csv",
                "buckets": [],
            },
        )
        note = row.get("notes", row.get("note", ""))
        policy_impl = row.get("policy_impl", row.get("policy_chosen_impl", ""))
        source_phase = row.get("policy_basis_source", row.get("source_phase", ""))
        entry["buckets"].append(
            {
                "start_len": int(row["start_len"]),
                "end_len": int(row["end_len"]),
                "raw_chosen_impl": row["raw_chosen_impl"],
                "policy_chosen_impl": policy_impl,
                "representative_effective_path": row["representative_effective_path"],
                "note": note,
                "source_phase": source_phase,
                "basis_used": row.get("policy_basis", row["basis_used"]),
                "bucket_points": int(row.get("evidence_points", row["bucket_points"])),
                "policy_basis_metric": row.get(
                    "policy_basis_metric", "trimmed_mean_ns_per_call"
                ),
                "raw_winners_in_segment": row.get("raw_winners_in_segment", ""),
                "min_margin_pct": row.get("min_margin_pct", ""),
            }
        )
    return [grouped[key_bits] for key_bits in sorted(grouped)]


def load_autotune_rows(csv_path: Path) -> list[dict[str, str]]:
    """Load autotune coarse/refined CSV rows."""
    _, rows = load_csv_rows(
        csv_path,
        required_columns=[
            "key_bits",
            "length",
            "impl",
            "effective_path",
            "normalized_impl",
            "trimmed_mean_ns_per_call",
            "trimmed_mean_ns_per_byte",
            "is_raw_winner",
            "is_policy_winner",
            "phase",
        ],
    )
    return rows


def select_rows(
    rows: list[dict[str, str]],
    run_id: str | None,
    scenario: str | None,
) -> list[dict[str, str]]:
    """Apply shared run/scenario filtering for reproducible plots."""
    available_run_ids = sorted({row["run_id"] for row in rows})
    selected_run_id = run_id
    if selected_run_id is None:
        selected_run_id = available_run_ids[-1]
        if len(available_run_ids) > 1:
            print(
                "Selected run_id "
                f"'{selected_run_id}' from {len(available_run_ids)} available runs."
            )
    elif selected_run_id not in available_run_ids:
        fail(
            f"run_id '{selected_run_id}' not found. "
            f"Available run_id values: {', '.join(available_run_ids)}"
        )

    filtered_rows = [row for row in rows if row["run_id"] == selected_run_id]
    if not filtered_rows:
        fail(f"no rows found for run_id '{selected_run_id}'")

    available_scenarios = sorted({row["scenario"] for row in filtered_rows})
    selected_scenario = scenario
    if selected_scenario is None:
        if len(available_scenarios) == 1:
            selected_scenario = available_scenarios[0]
        else:
            fail(
                "multiple scenarios found for the selected run_id; "
                "please choose one with --scenario. "
                f"Available scenarios: {', '.join(available_scenarios)}"
            )
    elif selected_scenario not in available_scenarios:
        fail(
            f"scenario '{selected_scenario}' not found for run_id '{selected_run_id}'. "
            f"Available scenarios: {', '.join(available_scenarios)}"
        )

    filtered_rows = [
        row for row in filtered_rows if row["scenario"] == selected_scenario
    ]
    if not filtered_rows:
        fail(
            f"no rows found for run_id '{selected_run_id}' "
            f"and scenario '{selected_scenario}'"
        )

    return filtered_rows


def filter_rows(
    rows: list[dict[str, str]],
    implementations: list[str] | None = None,
) -> list[dict[str, str]]:
    """Keep only rows for the requested implementations."""
    if implementations is None:
        return rows

    allowed = set(implementations)
    filtered = [row for row in rows if row["impl"] in allowed]
    if not filtered:
        fail(
            "input CSV does not contain any of the requested implementations: "
            + ", ".join(implementations)
        )
    return filtered


def available_key_bits(rows: list[dict[str, str]]) -> list[int]:
    """Return sorted unique key sizes present in the rows."""
    key_values = sorted({int(row["key_bits"]) for row in rows})
    if not key_values:
        fail("no key_bits values available in the input rows")
    return key_values


def filter_rows_by_key_bits(
    rows: list[dict[str, str]],
    key_bits: int | None,
) -> list[dict[str, str]]:
    """Filter rows to a single key size when requested."""
    if key_bits is None:
        return rows

    filtered = [row for row in rows if int(row["key_bits"]) == key_bits]
    if not filtered:
        fail(
            f"no rows found for key_bits={key_bits}. "
            f"Available key_bits values: {', '.join(str(value) for value in available_key_bits(rows))}"
        )
    return filtered


def filter_rows_by_length_range(
    rows: list[dict[str, str]],
    *,
    min_len: int | None = None,
    max_len: int | None = None,
    length_key: str = "len",
) -> list[dict[str, str]]:
    """Filter rows to an optional inclusive input-length range."""
    filtered = []
    for row in rows:
        length = int(row[length_key])
        if min_len is not None and length < min_len:
            continue
        if max_len is not None and length > max_len:
            continue
        filtered.append(row)

    if not filtered:
        bounds = []
        if min_len is not None:
            bounds.append(f">= {min_len}")
        if max_len is not None:
            bounds.append(f"<= {max_len}")
        fail(f"no rows found for length range {' and '.join(bounds)}")
    return filtered


def sorted_lengths(rows: list[dict[str, str]], *, length_key: str = "len") -> list[int]:
    """Return sorted unique lengths present in the input rows."""
    return sorted({int(row[length_key]) for row in rows})


def grouped_metric_by_impl(
    rows: list[dict[str, str]],
    implementations: list[str],
    metric_column: str,
    *,
    length_key: str = "len",
    impl_key: str = "impl",
) -> dict[str, list[tuple[int, float]]]:
    """Group the main plotting metric by implementation."""
    grouped: dict[str, list[tuple[int, float]]] = {impl: [] for impl in implementations}
    for row in rows:
        impl = row[impl_key]
        if impl not in grouped:
            continue
        grouped[impl].append((int(row[length_key]), float(row[metric_column])))

    for impl in implementations:
        grouped[impl].sort(key=lambda item: item[0])

    present = {impl for impl, values in grouped.items() if values}
    if not present:
        fail("no metric rows available after grouping input data")

    return grouped


def implementation_style(impl: str) -> dict[str, object]:
    """Return a grayscale-safe plotting style for an implementation."""
    return dict(PLOT_STYLES[impl])


def region_fill_color(impl: str) -> str:
    """Return a light fill color for best-region spans."""
    return REGION_FILL_COLORS[impl]


def display_label(impl: str) -> str:
    """Return the publication-friendly display label."""
    return DISPLAY_LABELS.get(impl, impl)


def region_label(impl: str) -> str:
    """Return the compact label used inside best-implementation regions."""
    return REGION_LABELS.get(impl, display_label(impl))


def canonical_impl_from_effective_path(effective_path: str) -> str:
    """Map an effective execution path to the canonical implementation region."""
    if effective_path not in EFFECTIVE_PATH_TO_IMPL:
        fail(
            "unknown effective_path "
            f"'{effective_path}'. Update EFFECTIVE_PATH_TO_IMPL in plot_common.py."
        )
    return EFFECTIVE_PATH_TO_IMPL[effective_path]


def representative_ticks(lengths: list[int]) -> list[int]:
    """Return a compact subset of x ticks suitable for paper figures."""
    available = set(lengths)
    ticks = [tick for tick in REPRESENTATIVE_TICKS if tick in available]
    return ticks if ticks else lengths


def geometric_midpoint(left: float, right: float) -> float:
    """Return the midpoint on a log axis."""
    return math.sqrt(left * right)


def region_boundaries(lengths: list[int]) -> list[float]:
    """Construct left/right boundaries using geometric midpoints."""
    if not lengths:
        fail("cannot compute region boundaries from an empty length list")
    if len(lengths) == 1:
        length = float(lengths[0])
        return [length / math.sqrt(2.0), length * math.sqrt(2.0)]

    boundaries: list[float] = []
    for index, length in enumerate(lengths):
        if index == 0:
            right_mid = geometric_midpoint(length, lengths[index + 1])
            left_edge = length * length / right_mid
            boundaries.append(left_edge)
        if index < len(lengths) - 1:
            boundaries.append(geometric_midpoint(length, lengths[index + 1]))
        else:
            left_mid = geometric_midpoint(lengths[index - 1], length)
            right_edge = length * length / left_mid
            boundaries.append(right_edge)
    return boundaries


def merge_length_regions(length_to_impl: list[tuple[int, str]]) -> list[tuple[float, float, str]]:
    """Merge adjacent length winners into span regions."""
    lengths = [length for length, _ in length_to_impl]
    boundaries = region_boundaries(lengths)
    regions: list[tuple[float, float, str]] = []
    current_impl = length_to_impl[0][1]
    current_left = boundaries[0]
    current_right = boundaries[1]

    for index in range(1, len(length_to_impl)):
        _, impl = length_to_impl[index]
        segment_left = boundaries[index]
        segment_right = boundaries[index + 1]
        if impl == current_impl:
            current_right = segment_right
            continue
        regions.append((current_left, current_right, current_impl))
        current_impl = impl
        current_left = segment_left
        current_right = segment_right

    regions.append((current_left, current_right, current_impl))
    return regions


def policy_note_is_mixed(note: str, effective_path: str) -> bool:
    """Return whether a policy bucket should receive a mixed-path marker."""
    return "mixed_effective_path" in note or effective_path == "mixed_effective_path"


def impl_legend_handles(implementations: list[str] | None = None) -> list[Patch]:
    """Return a consistent categorical legend for policy-style plots."""
    handles: list[Patch] = []
    for impl in implementations or IMPLEMENTATION_ORDER:
        handles.append(
            Patch(
                facecolor=region_fill_color(impl),
                edgecolor=implementation_style(impl)["color"],
                linewidth=1.0,
                label=region_label(impl),
            )
        )
    return handles


def style_axes(ax: plt.Axes, *, y_minor_log: bool = False) -> None:
    """Apply a clean paper-style axes treatment."""
    ax.set_facecolor("white")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color("#7c8794")
    ax.spines["bottom"].set_color("#7c8794")
    ax.spines["left"].set_linewidth(0.8)
    ax.spines["bottom"].set_linewidth(0.8)
    ax.tick_params(axis="both", colors="#334155", length=3.5, width=0.8)
    ax.grid(True, which="major", axis="y", color="#d7dde5", linewidth=0.8)
    ax.grid(False, which="major", axis="x")

    if y_minor_log:
        ax.yaxis.set_minor_locator(LogLocator(base=10.0, subs=(2, 5)))
        ax.grid(True, which="minor", axis="y", color="#eceff3", linewidth=0.6)


def legend_style_kwargs() -> dict[str, object]:
    """Return consistent legend styling for line plots."""
    return {
        "frameon": True,
        "fancybox": False,
        "edgecolor": "#d7dde5",
        "facecolor": "white",
        "framealpha": 1.0,
        "borderpad": 0.5,
        "columnspacing": 1.3,
        "handletextpad": 0.7,
    }


def ensure_output_dir(output_dir: Path) -> None:
    """Create the output directory if needed."""
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        fail(f"failed to create output directory {output_dir}: {exc}")


def save_figure(fig: plt.Figure, output_dir: Path, stem: str) -> list[Path]:
    """Save one publication-resolution PNG figure."""
    ensure_output_dir(output_dir)

    output_path = output_dir / f"{stem}.png"
    try:
        fig.savefig(output_path, dpi=300, bbox_inches="tight", format="png")
    except OSError as exc:
        fail(f"failed to write figure {output_path}: {exc}")

    return [output_path]


def configure_matplotlib() -> None:
    """Apply compact, paper-friendly defaults."""
    plt.rcParams.update(
        {
            "figure.figsize": (6.8, 4.0),
            "figure.dpi": 120,
            "figure.facecolor": "white",
            "axes.edgecolor": "#333333",
            "axes.labelsize": 11,
            "axes.titlesize": 12,
            "axes.facecolor": "white",
            "axes.linewidth": 0.8,
            "axes.grid": True,
            "grid.color": "#d0d0d0",
            "grid.linewidth": 0.8,
            "grid.alpha": 1.0,
            "legend.fontsize": 9.5,
            "legend.handlelength": 2.8,
            "xtick.labelsize": 9.5,
            "ytick.labelsize": 9.5,
            "lines.linewidth": 2.1,
            "lines.markersize": 6.2,
            "font.family": "DejaVu Sans",
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
            "savefig.facecolor": "white",
            "savefig.bbox": "tight",
            "legend.frameon": False,
        }
    )
