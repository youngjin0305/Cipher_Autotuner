# Runtime Dispatch Autotuning Framework

## Framework Scope

이 프로젝트는 입력 길이와 CPU feature에 따라 여러 후보 구현 중 하나를 선택하는 런타임
오토튜닝 및 디스패치 프레임워크이다. ARIA는 프레임워크를 검증하기 위한 case study이며,
현재 등록된 후보 구현은 다음과 같다.

- `ref`
- `linux_aesni_avx`
- `linux_aesni_avx2`
- `linux_gfni_avx512`

CPU feature detection 결과 지원되지 않는 SIMD 구현은 측정과 Policy 후보에서 제외된다. GFNI
후보는 AVX2, AVX-512F, AVX-512VL, GFNI와 OS의 XMM/YMM/ZMM context 지원을 모두 확인한다.
Auto-tuning은 기존 Coarse Scan으로 성능 전환 후보를 찾고, winner가 바뀌는 구간만 Fine Scan으로
재측정한다. 이 프로젝트에는 히스테리시스, 비용 모델, model-guided tuning, 머신러닝 또는 새로운
탐색 알고리즘이 포함되지 않는다.

공통 측정과 통계는 `bench_core`, Policy 품질 지표는 `policy_metrics`, ARIA 후보 등록과 실제
dispatch는 `block_ciphers/aria`에 위치한다. 평가 계층과 ARIA case study 코드는 분리되어 있지만,
현재 실행 가능한 candidate adapter는 ARIA만 제공한다.

`linux_gfni_avx512` adapter는 Linux kernel GFNI assembly를 userspace ABI에 연결한다. 원본 glue와
동일하게 GFNI/AVX-512 64-way, GFNI/AVX2 32-way, GFNI/AVX 16-way 순으로 처리하며 마지막 16-way
batch보다 작은 block tail만 `ref`로 이어진다. 원본 kernel glue 파일은 참고용으로만 보존하며
빌드하지 않는다. GFNI를 실제 실행할 수 없는 CPU에서도 프로젝트와 나머지 후보는 정상
빌드·실행되고, 해당 후보는 `run_meta.json`에 `unsupported_cpu_feature`로 기록된다.

## Build and Run

Release build와 테스트:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

`aria_kat`는 GFNI 후보에 대해 16/17, 32/33, 64/65-block 입력을 128/192/256-bit key로 검증한다.
GFNI/AVX-512를 사용할 수 없는 CPU에서는 이 항목만 `SKIP`되고 전체 테스트는 실패하지 않는다.
GFNI 지원 실험 머신에서는 해당 항목이 모두 `[OK]`인지 확인한 뒤 `full` profile을 실행한다.

빠른 동작 확인용 test profile:

```bash
bash scripts/run_autotune_and_plot.sh test out/autotune_test
```

논문 실험용 full profile:

```bash
bash scripts/run_autotune_and_plot.sh full out/autotune_full
```

개별 실행:

```bash
./build/bench_runner --autotune --autotune-profile test \
  --trim-ratio 0.10 --output-dir out/autotune_test

./build/bench_runner --autotune --autotune-profile full \
  --trim-ratio 0.10 --output-dir out/autotune_full --output-level debug
```

`test`는 코드와 파이프라인 확인용이고 `full`은 논문 결과 측정용이다. 서로 다른 profile의 결과를
같은 결과 디렉터리에 혼합하지 않는다. `test` 범위는 16-512 bytes, `full` 범위는 GFNI 64-way
경로를 충분히 포함하도록 16-4096 bytes이다. 통합 스크립트의 출력 디렉터리는 안전한 정리를
위해 `out/` 아래에 있어야 한다. 이전 1024-byte full 범위보다 exhaustive validation 지점이
늘어나므로 논문용 실행 시간도 증가한다.

주요 CLI 옵션:

```text
--autotune
--autotune-profile test|full
--autotune-min-len <bytes>
--autotune-max-len <bytes>
--coarse-iterations <count>
--refine-iterations <count>
--refine-step <bytes>
--trim-ratio <ratio>
--policy-basis normalized|raw
--tail-policy native|conservative
--scenario server|lowpower
--output-dir <path>
--output-level default|debug|raw
```

`trim_ratio`는 `0 <= trim_ratio < 0.5` 범위여야 하며, 절사 후 최소 3개 sample이 남지 않는 설정은
오류로 종료된다. 기본값은 양쪽 각각 10%를 제거하는 `0.10`이다.

## Measurement Methodology

모든 성능 비교, winner 결정, throughput, speedup, Best Static 선택과 독립 Policy 검증은 같은
`trimmed_mean_ns_per_call` estimator를 사용한다.

각 `(mode, key_bits, message_length, implementation)` 측정 지점의 처리 순서는 다음과 같다.

1. timed region 밖에서 key setup을 수행한다.
2. 설정된 횟수만큼 warmup한다.
3. 목표 측정 시간에 맞게 inner iteration 수를 자동 선택한다.
4. 각 outer sample에서 암호화 inner loop를 측정한다.
5. 같은 outer sample에서 동일 loop 구조의 empty function을 측정한다.
6. sample별로 `(measured_ticks - empty_ticks) / inner_iterations`를 계산한다.
7. `measured_ticks <= empty_ticks`인 sample은 0으로 clamp하지 않고 폐기한 뒤 재측정한다.
8. 요청한 valid sample을 확보하거나 최대 재시도 횟수에 도달할 때까지 반복한다.
9. corrected outer samples를 정렬하고 양쪽을 절사한 뒤 남은 값의 산술평균을 계산한다.

정렬된 corrected sample을 다음과 같이 정의한다.

```text
x(1) <= x(2) <= ... <= x(n)
k = floor(n × trim_ratio)

trimmed_mean = Σ[i=k+1 to n-k] x(i) / (n - 2k)
```

empty-loop overhead는 trimming 전에 각 outer sample에서 독립적으로 차감한다. invalid sample은
최대 `BENCH_INVALID_SAMPLE_RETRIES` 배까지 재측정하며, 절사 후 최소
`BENCH_MIN_TRIMMED_SAMPLES=3`을 확보하지 못하면 해당 측정은 실패한다. raw mean, median,
standard deviation, IQR, min/max는 진단 목적으로만 기록하며 winner에는 사용하지 않는다.

검증 pass에서는 후보 측정 순서를 `(key index + length index)`에 따라 결정론적으로 회전시켜 항상
같은 후보가 먼저 측정되는 고정 순서 편향을 줄인다. 난수는 사용하지 않으므로 `random_seed`는
`null`이다.

## Dispatch Modes

### Direct Reference

`ref` 구현을 Policy lookup 없이 직접 호출한다. key setup은 timed region 전에 수행하며,
프레임워크 lookup overhead가 없는 기준점이다.

### Best Static Implementation

CPU가 지원하는 각 후보 구현을 전체 평가 grid에서 직접 측정한다. 각 후보의 지점별
`trimmed_mean_ns_per_call`을 동일 가중치로 macro-average하고, 가장 낮은 단일 구현을 모든
길이에 고정한다. 이 비교 대상은 단순히 빠른 SIMD 구현 하나를 고정한 효과와 길이별 Policy
선택 효과를 구분한다. lookup은 포함하지 않는다.

### Static Heuristic Dispatch

Auto-tuning과 무관한 기존 고정 규칙과 CPU feature detection을 사용한다. 현재 `server` 규칙은
코드의 `aria_runtime_dispatch_scenario()`가 단일 source of truth이다.

```text
length < 256: ref
length >= 256: CPU가 지원하면 linux_aesni_avx
length >= 512: CPU가 지원하면 linux_aesni_avx2
length >= 1024: CPU와 OS가 지원하면 linux_gfni_avx512
```

GFNI/AVX-512가 지원되지 않으면 AVX2, AVX2가 지원되지 않으면 AVX, AVX도 지원되지 않으면
`ref`로 fallback한다. `lowpower` scenario는 모든 길이에서 `ref`를 선택한다. 모든 timed 호출에
heuristic lookup 비용을 포함한다.

### Auto-tuned Policy Dispatch

Coarse Scan과 Fine Scan으로 생성한 `(key_bits, message_length)` Policy Map을 사용한다. 모든 timed
호출에 Policy lookup 비용을 포함하지만 Policy 생성 시간은 호출 실행시간에 합산하지 않는다.
Policy가 없거나 범위 밖이면 Static Heuristic Dispatch로 fallback한다. CPU 비호환 구현,
중첩 구간, 빈 16-byte 구간 또는 정렬되지 않은 Policy는 설치하지 않는다.

### Exhaustive Oracle Policy

Policy 생성이 끝난 뒤 별도의 exhaustive pass에서 모든 16-byte 지점과 지원 후보를 다시 측정해
`Exhaustive-search Reference Policy`를 구성한다. 이는 측정 잡음의 영향을 받는 독립적인 reference
결과이며 절대적인 이론적 ground truth가 아니다.

현재 구현은 Oracle winner와 후보별 시간으로 Auto-tuned Policy의 regret와 accuracy를 계산하지만,
Oracle Policy Dispatch 자체의 실행시간 막대는 생성하지 않는다. 런타임에는 하나의 active Policy
Map만 설치되므로 평가 도중 이를 Oracle Map으로 교체하면 Auto-tuned Policy 평가 상태를 오염시킬
수 있기 때문이다. 따라서 `fig_framework_evaluation.png`의 실행 성능 패널에는 Oracle 막대를
명시적으로 생략한다.

## Metrics

### Trimmed-Mean Execution Time

한 측정 지점의 corrected outer samples를 양쪽 절사한 평균이며 단위는 `ns/call`이다. 낮을수록
좋다. 여러 key size와 길이의 지점별 trimmed mean을 동일 가중치로 평균한 값은
`macro_avg_trimmed_mean_ns_per_call`로 별도 명명한다.

### Throughput

```text
throughput_bytes_per_sec = message_length × 1,000,000,000 / trimmed_mean_ns_per_call
throughput_mib_per_sec   = throughput_bytes_per_sec / (1024 × 1024)
```

높을수록 좋다. Summary의 `macro_avg_throughput_mib_per_sec`는 각 지점 throughput의 동일 가중치
macro-average이며 실제 workload-weighted throughput이 아니다.

### Geometric-Mean Speedup over Direct Reference

```text
speedup_vs_direct_ref = direct_ref_trimmed_mean_ns_per_call /
                        method_trimmed_mean_ns_per_call
```

여러 지점은 `geomean_speedup_vs_direct_ref`로 요약한다. 1보다 크면 Direct Reference보다 빠르다.

### Exact Policy Match Accuracy

독립 exhaustive-search reference winner와 Auto-tuned Policy가 같은 구현 class를 선택한 지점 비율이다.

```text
exact_policy_match_accuracy_percent = exact_match_points / validation_point_count × 100
```

`normalized` basis에서는 `ref_fallback`을 `ref` class로 합치고, `plus_ref_tail`은 `tail_policy`에
따라 분류한다. GFNI 16/32/64-way와 이들의 혼합 경로는 하나의 `linux_gfni_avx512` class로
정규화한다. GFNI bulk 뒤에 `ref` tail이 남는 경우 `native`에서는 GFNI class,
`conservative`에서는 `ref` class로 분류한다. `raw` basis에서는 후보 구현 이름을 그대로
비교한다. 높을수록 좋다.

### Within-1% / Within-3% Accuracy

Policy 구현 시간이 같은 exhaustive pass의 reference winner 시간보다 각각 1% 또는 3% 이내인
지점 비율이다. 구현 이름이 달라도 성능 손실이 허용 범위 안이면 일치로 계산한다.

### Policy Regret

```text
policy_regret_percent =
  (policy_impl_trimmed_mean_ns_per_call - reference_winner_trimmed_mean_ns_per_call)
  / reference_winner_trimmed_mean_ns_per_call × 100
```

Policy와 reference winner 시간은 같은 validation pass의 후보 측정에서 가져온다. 음수 regret는
자동으로 0으로 숨기지 않고 측정 일관성 오류로 처리한다. mean, median, p95와 maximum을 key
size별로 기록하며 낮을수록 좋다.

### Mean Symmetric Boundary Distance

Exhaustive reference 경계 집합을 `B`, Policy 경계 집합을 `P`라 할 때 다음 양방향 최근접 거리의
평균이다.

```text
(Σ[p∈P] min[b∈B]|p-b| + Σ[b∈B] min[p∈P]|b-p|) / (|P| + |B|)
```

단위는 byte이며 낮을수록 좋다. 두 집합이 모두 비면 0, 한쪽만 비면 `max_len - min_len`을 maximum
penalty로 기록한다. Policy와 reference boundary count도 함께 저장한다. `all` 행의 거리는 key별
거리 결과를 각 key의 `policy_boundary_count + reference_boundary_count`로 가중한 값이며, 전체
accuracy와 regret는 모든 validation 지점을 합쳐 계산한다.

### Mismatched Range

exact match가 아닌 연속 16-byte 지점을 구간으로 병합한다. `256-320`은 256, 272, 288, 304,
320 bytes가 모두 불일치한다는 뜻이며 불일치가 없으면 `none`이다. point count, interval count와
가장 긴 interval의 byte 폭도 함께 기록한다.

### Auto-tuning Time and Measurement Reduction

- `autotune_search_time_ms`: Coarse/Fine 측정, 병합, bucket 생성, 유효성 검사와 Runtime Map 구성.
- `autotune_end_to_end_time_ms`: search에 Policy 설치와 CSV/JSON 기록을 더한 wall-clock 시간.
- exhaustive validation과 dispatch evaluation 시간은 두 값에 포함하지 않는다.

candidate measurement 한 건은 `(key_bits, message_length, implementation)` 조합의 독립 benchmark
한 번으로 정의한다. outer sample이나 inner 암호 함수 호출 수가 아니다.

```text
measurement_reduction_percent =
  (1 - autotune_candidate_measurement_count / exhaustive_candidate_measurement_count) × 100
```

높을수록 exhaustive scan보다 적은 후보 측정으로 Policy를 생성했다는 뜻이다.

### Dispatch Overhead

```text
dispatch_overhead_ns_per_call = dispatched_trimmed_mean_ns_per_call
                                - direct_selected_impl_trimmed_mean_ns_per_call
```

Static Heuristic와 Auto-tuned Policy가 선택한 동일 구현의 direct call과 비교한다. 독립 측정 잡음으로
작은 음수가 나올 수 있으며 이를 0으로 clamp하지 않는다. macro-average와 maximum을 기록한다.

## Macro-average 주의사항

평가 grid는 128/192/256-bit와 다음 길이 중 현재 Policy 범위에 포함되는 지점이다.

```text
16, 32, 64, 128, 192, 256, 320, 512, 1024, 2048, 4096 bytes
```

실행시간과 throughput macro-average는 모든 key size와 길이에 동일한 가중치를 준다. 실제 workload
분포를 반영한 값이 아니므로 특정 메시지 크기의 성능 주장은 summary 막대가 아니라
`dispatch_evaluation.csv`의 길이별 결과로 확인한다.

## Independent Policy Validation

Auto-tuning 측정값을 validation 정답으로 재사용하지 않는다. Policy를 설치한 뒤 별도의 exhaustive
measurement pass를 수행하며, Auto-tuning과 동일한 warmup, adaptive inner iteration, sample별
empty-loop correction, trim ratio와 trimmed-mean estimator를 사용한다. validation outer count는
Fine Scan 반복 횟수와 동일하므로 Coarse Scan과 같거나 더 많다.

Policy 품질은 다음 순서로 해석한다.

1. 실제 Auto-tuned Policy Dispatch 실행 성능
2. Exhaustive reference 대비 Policy Regret
3. Exact Policy Match Accuracy와 Within-1%/3% Accuracy
4. Mean Symmetric Boundary Distance
5. Mismatched Range

## Output Files

핵심 출력:

| 파일 | 내용 |
|---|---|
| `dispatch_evaluation.csv` | mode/key/길이별 trimmed mean, throughput, speedup, lookup overhead 및 진단 통계 |
| `dispatch_evaluation_summary.csv` | mode별 macro-average, geometric-mean speedup과 overhead |
| `policy_validation.csv` | 독립 exhaustive winner, Policy 시간, regret, 허용 오차 정확도와 후보별 시간 |
| `policy_validation_summary.csv` | key별 행과 `all` 전체 행의 accuracy, regret, boundary와 mismatch 요약 |
| `autotune_metrics.csv` | search/end-to-end 시간, 측정 건수와 reduction |
| `framework_validation_summary.csv` | CPU 호환성, 범위, 정렬, fallback과 key별 Policy 생성 Pass/Fail |
| `autotune_policy.csv`, `autotune_policy.json` | 설치된 최종 Policy bucket |
| `summary_stats.csv` | 구현별 대표 성능 지형과 진단 통계 |
| `run_meta.json` | 실행 환경과 재현성 설정 |

`--output-level debug`는 `autotune_coarse.csv`, `autotune_refined.csv`,
`autotune_boundaries.csv`, `keysetup.csv`를 추가한다. `--output-level raw`는 outer sample별
`raw_samples.csv`도 추가한다.

CSV 스키마에서 대표 시간 명칭은 `trimmed_mean_ns_per_call`, 여러 지점 요약은
`macro_avg_trimmed_mean_ns_per_call`로 통일했다. 기존의 모호한 대표값 및 median 기반 명칭은 새
평가 스키마에서 제거했다.

## Figures

통합 스크립트는 PNG만 생성하며 중복을 피하기 위해 세 파일만 남긴다.

```text
fig_perf_key128_ns_byte.png
fig_raw_vs_policy_key128.png
fig_framework_evaluation.png
```

`fig_framework_evaluation.png`에는 다음이 포함된다.

- Macro-averaged Trimmed-Mean Execution Time
- Geometric-Mean Speedup over Direct Reference
- Exact Policy Match와 Within-1% Accuracy
- Mean/Maximum Policy Regret
- Mean Symmetric Boundary Distance
- search/end-to-end tuning time, 후보 측정 건수, measurement reduction과 mismatched ranges

Figure 제목은 scenario, profile, trim ratio와 end-to-end Policy 생성 시간을 기록한다. Throughput은
길이별 성능 Figure 및 CSV와 중복되므로 최종 요약 Figure에서는 speedup으로 교체했다.

개별 생성:

```bash
python3 scripts/plot_framework_evaluation.py \
  --evaluation-summary out/autotune_full/dispatch_evaluation_summary.csv \
  --validation-summary out/autotune_full/policy_validation_summary.csv \
  --autotune-metrics out/autotune_full/autotune_metrics.csv \
  --out-dir out/autotune_full/figures
```

## Reproducibility

`run_meta.json`은 timestamp, OS, CPU feature, compiler/version, build type, scenario/profile, key/길이,
warmup, adaptive inner iteration, outer sample, trim ratio와 규칙, estimator, empty-loop correction,
policy/tail basis, 후보 활성 상태, 결정론적 후보 순서와 측정 건수를 기록한다. 현재 실행 파일에서
안전하게 확인할 수 없는 git commit, CPU model과 compiler flags는 값을 추측하지 않고 각각
`null` 또는 `unknown`으로 기록한다.

논문 결과에는 같은 시스템 상태에서 실행한 `full` profile과 해당 `run_meta.json`을 함께 보존한다.
Figure 값은 summary CSV에서 직접 읽으며, 논문 결과와 test profile 결과를 혼합하지 않는다.
