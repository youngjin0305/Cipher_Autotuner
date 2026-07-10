#!/usr/bin/env bash
set -eu

profile="${1:-full}"
out_dir="${2:-out/policy_${profile}}"
output_level="${3:-default}"

case "$profile" in
  smoke|test|full) ;;
  *)
    echo "usage: $0 [smoke|test|full] [out_dir] [default|debug|raw]" >&2
    exit 2
    ;;
esac

case "$output_level" in
  default|debug|raw) ;;
  *)
    echo "usage: $0 [smoke|test|full] [out_dir] [default|debug|raw]" >&2
    exit 2
    ;;
esac

case "$profile" in
  smoke) plot_max_len=64 ;;
  test) plot_max_len=512 ;;
  full) plot_max_len=1024 ;;
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

./build/aria_kat
./build/bench_runner --autotune --autotune-profile "$profile" --output-dir "$out_dir" --output-level "$output_level"

fig_dir="$out_dir/figures"
python scripts/plot_ns_per_call.py --input "$out_dir/summary_stats.csv" --out-dir "$fig_dir" --max-len "$plot_max_len"
python scripts/plot_ns_per_byte.py --input "$out_dir/summary_stats.csv" --out-dir "$fig_dir" --max-len "$plot_max_len"
python scripts/plot_policy_map.py --policy "$out_dir/autotune_policy.csv" --out-dir "$fig_dir"
python scripts/plot_raw_vs_policy.py --summary "$out_dir/summary_stats.csv" --policy "$out_dir/autotune_policy.csv" --out-dir "$fig_dir" --max-len "$plot_max_len"
python scripts/plot_best_impl.py --input "$out_dir/summary_stats.csv" --out-dir "$fig_dir" --max-len "$plot_max_len"

echo "Generated data files:"
find "$out_dir" -maxdepth 1 -type f | sort

echo "Generated figure files:"
find "$fig_dir" -maxdepth 1 -type f | sort
