#!/usr/bin/env bash
set -eu

profile="${1:-full}"
out_dir="${2:-out/policy_${profile}}"
output_level="${3:-default}"

case "$profile" in
  test|full) ;;
  *)
    echo "usage: $0 [test|full] [out_dir] [default|debug|raw]" >&2
    exit 2
    ;;
esac

case "$output_level" in
  default|debug|raw) ;;
  *)
    echo "usage: $0 [test|full] [out_dir] [default|debug|raw]" >&2
    exit 2
    ;;
esac

case "$profile" in
  test) plot_max_len=512 ;;
  full) plot_max_len=4096 ;;
esac

if [ ! -x ./build/aria_kat ] || [ ! -x ./build/bench_runner ]; then
  echo "missing build artifacts: expected ./build/aria_kat and ./build/bench_runner" >&2
  echo "configure/build the project first, then rerun this script." >&2
  exit 1
fi

case "$out_dir" in
  out/*)
    if [ -d "$out_dir" ]; then
      find "$out_dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    fi
    ;;
  *)
    echo "refusing to clean non-out directory: $out_dir" >&2
    echo "use an output path under out/ for integrated experiment runs." >&2
    exit 2
    ;;
esac

mkdir -p "$out_dir"

ctest --test-dir build --output-on-failure
./build/bench_runner --autotune --autotune-profile "$profile" --output-dir "$out_dir" --output-level "$output_level"

fig_dir="$out_dir/figures"
python3 scripts/plot_ns_per_byte.py --validation "$out_dir/policy_validation.csv" --out-dir "$fig_dir" --max-len "$plot_max_len" --key-bits 128
python3 scripts/plot_raw_vs_policy.py --raw-best "$out_dir/raw_best_by_length.csv" --policy "$out_dir/autotune_policy.csv" --out-dir "$fig_dir" --max-len "$plot_max_len" --key-bits 128
python3 scripts/plot_policy_comparison.py --evaluation "$out_dir/dispatch_evaluation.csv" --policy "$out_dir/autotune_policy.csv" --run-meta "$out_dir/run_meta.json" --out-dir "$fig_dir" --key-bits 128
python3 scripts/plot_scan_coverage.py --scan-points "$out_dir/autotune_scan_points.csv" --out-dir "$fig_dir"
python3 scripts/plot_framework_evaluation.py --evaluation-summary "$out_dir/dispatch_evaluation_summary.csv" --validation-summary "$out_dir/policy_validation_summary.csv" --autotune-metrics "$out_dir/autotune_metrics.csv" --out-dir "$fig_dir"

echo "Generated data files:"
find "$out_dir" -maxdepth 1 -type f | sort

echo "Generated figure files:"
find "$fig_dir" -maxdepth 1 -type f | sort
