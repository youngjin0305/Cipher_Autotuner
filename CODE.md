# Code Implementation Guide

## 1. 문서 목적

이 문서는 ARIA를 case study로 사용하는 Runtime Dispatch Autotuning Framework가 코드 수준에서
어떻게 구성되고 동작하는지 설명한다. 특히 다음 내용을 구현 파일과 연결하여 정리한다.

- ARIA 후보 구현을 하나의 공통 인터페이스로 등록한 방법
- 실행 시간을 측정하고 통계값을 계산하는 방법
- Coarse Scan과 Fine Scan으로 Policy를 생성하는 방법
- 생성한 Policy의 유효성과 exhaustive search 대비 정확도를 검증하는 방법
- Baseline, Static Dispatch, Auto-tuned Dispatch를 평가하는 방법
- CSV 결과를 어떤 기준으로 Figure로 변환하는지
- 실험 전체를 한 번에 재현하는 방법

이 프레임워크에는 히스테리시스, 비용 모델, 머신러닝, model-guided tuning이 포함되지 않는다.
현재 구현된 Coarse Scan과 Fine Scan을 안정적으로 실행하고 평가하는 데 필요한 코드만 사용한다.

## 2. 전체 실행 구조

```text
CPU Feature Detection
        │
        ▼
지원 가능한 ARIA 후보 등록
Ref / AES-NI+AVX / AES-NI+AVX2 / GFNI+AVX-512
        │
        ▼
Coarse Scan ── 후보별 반복 측정 ── winner와 경계 후보 결정
        │
        ▼
Fine Scan ─── 경계 후보 구간을 16-byte 간격으로 재측정
        │
        ▼
측정점 병합 및 기존 안정화 규칙 적용
        │
        ▼
연속 길이 구간을 Policy bucket으로 변환
        │
        ▼
Policy 연속성·정렬·CPU 지원 여부 검증 후 설치
        │
        ├── Runtime / Auto-tuned Dispatch 평가
        ├── Exhaustive Policy Validation
        └── CSV 출력 → Python/Matplotlib → PNG Figure
```

핵심 실행 진입점은 `bench/runner/main.c`이며, 실제 자동 튜닝 순서는
`block_ciphers/aria/dispatch/autotune.c`의 `aria_autotune_run()`에 구현되어 있다.

## 3. 소스 디렉터리 역할

| 경로 | 역할 |
|---|---|
| `block_ciphers/aria/api/aria_api.h` | 모든 ARIA 구현이 따르는 공통 인터페이스 |
| `block_ciphers/aria/impl/ref/` | 단일 블록 Reference 구현 adapter |
| `block_ciphers/aria/impl/linux_aesni_avx/` | Linux AES-NI+AVX 16-way assembly adapter |
| `block_ciphers/aria/impl/linux_aesni_avx2/` | Linux AES-NI+AVX2 32-way mixed-width adapter |
| `block_ciphers/aria/impl/linux_gfni_avx512/` | GFNI 64/32/16-way mixed-width adapter |
| `block_ciphers/aria/source/linux_kernal/` | 가져온 Linux assembly와 비교용 원본 glue |
| `block_ciphers/aria/dispatch/cpu_features.c` | CPU 및 OS SIMD context 지원 검출 |
| `block_ciphers/aria/dispatch/runtime_dispatch.c` | Static Heuristic 및 Policy Map lookup |
| `block_ciphers/aria/dispatch/autotune.c` | Coarse/Fine Scan, Policy 생성과 exhaustive 검증 |
| `block_ciphers/aria/dispatch/policy_metrics.c` | Policy 경계 및 mismatch 지표 계산 |
| `bench/runner/bench_measure.c` | 공통 시간 측정과 통계 처리 |
| `bench/runner/dispatch_evaluation.c` | 평가 mode별 성능 측정과 비교 |
| `scripts/plot_common.py` | CSV loader, 색상, 축, PNG 저장 등 공통 plotting 코드 |
| `scripts/plot_*.py` | Figure별 데이터 선택과 시각화 |
| `scripts/run_autotune_and_plot.sh` | 테스트, 실험, Figure 생성을 묶은 통합 실행 스크립트 |

## 4. ARIA 구현 공통 인터페이스

각 구현은 `aria_impl_t` 구조체로 등록한다.

```c
typedef struct aria_impl {
  const char *name;
  aria_init_fn init;
  aria_encrypt_fn encrypt;
  aria_support_fn is_supported;
  aria_effective_path_fn effective_path;
  aria_execution_path_fn execution_path;
} aria_impl_t;
```

각 필드의 의미는 다음과 같다.

| 필드 | 의미 |
|---|---|
| `name` | CSV와 Policy에서 사용하는 후보 구현 이름 |
| `init` | 키 크기별 ARIA context 및 round key 초기화 |
| `encrypt` | 동일한 함수 형식으로 여러 길이의 ECB 데이터를 암호화 |
| `is_supported` | 현재 CPU와 OS에서 후보를 실행할 수 있는지 확인 |
| `effective_path` | 후보 이름이 아니라 해당 길이에서 실제 사용한 구현 계열 기록 |
| `execution_path` | 64/32/16-way chunk 수와 Ref tail 블록 수 계산 |

이 구조 덕분에 benchmark와 autotuner는 구현별 내부 코드를 알 필요 없이 동일한 함수 포인터를
순회하면서 측정할 수 있다. CPU에서 지원되지 않는 후보는 `is_supported()` 결과를 확인한 뒤
측정과 Policy 후보에서 제외한다.

### 4.1 Mixed-width 처리

Linux 원본 ECB glue와 동일하게 SIMD 후보는 처리할 수 있는 가장 넓은 chunk부터 실행한다.

```text
linux_aesni_avx:
AVX 16-way 반복 → Ref 1-way tail

linux_aesni_avx2:
AVX2 32-way 반복 → AVX 16-way remainder → Ref 1-way tail

linux_gfni_avx512:
GFNI 64-way 반복 → GFNI/AVX2 32-way → GFNI/AVX 16-way → Ref tail
```

`execution_path` callback은 실제 암호화 함수와 같은 분해 규칙으로 다음 정보를 계산한다.

```text
gfni_64way_chunk_count
avx2_chunk_count
avx_chunk_count
ref_tail_block_count
execution_path
```

예를 들어 AVX2 후보의 784-byte 입력은 49블록이므로 다음과 같이 기록된다.

```text
avx2_32way*1 + avx_16way*1 + ref*1
```

이 정보는 성능 이상이 실제 SIMD 경로에서 발생했는지 확인할 수 있도록 benchmark 및 validation
CSV에 포함한다.

## 5. CPU Feature Detection과 Dispatch

CPU feature 검출은 `cpu_features.c`에서 수행한다. AES-NI, AVX, AVX2, AVX-512, GFNI뿐 아니라
OS가 XMM/YMM/ZMM 상태를 보존할 수 있는지도 확인한다. 결과는 프로세스 최초 요청 시 계산하고
cache하므로 반복되는 Runtime Dispatch의 timed path에서 CPUID나 XGETBV를 다시 실행하지 않는다.

### 5.1 Static Heuristic Dispatch

`aria_runtime_dispatch_scenario()`는 CPU 지원 여부와 고정 길이 기준으로 구현을 선택한다.
High-performance scenario의 기본 길이 기준은 다음과 같다.

```text
length < 256                 → Ref
256 <= length < 512          → AVX, 지원되는 경우
512 <= length < 1024         → AVX2, 지원되는 경우
1024 <= length               → GFNI, 지원되는 경우
```

상위 구현이 지원되지 않으면 사용 가능한 하위 구현으로 내려간다. Low-power scenario는 Ref를
선택한다. 이 고정 규칙은 Auto-tuning과 비교하기 위한 Static Heuristic이며 학습이나 비용 모델을
사용하지 않는다.

### 5.2 Auto-tuned Dispatch

`aria_autotuned_dispatch(key_bits, len)`은 설치된 Policy Map에서 key size와 입력 길이를 포함하는
bucket을 찾아 구현 포인터를 반환한다. Policy가 없거나 범위를 벗어나면 Runtime Dispatch로
fallback한다. 실제 암호화는 반환된 동일 `aria_impl_t.encrypt` 함수를 호출한다.

## 6. 시간 측정 구현

공통 측정 코드는 `bench_measure.c`에 구현되어 있다. 모든 후보, Runtime Dispatch 및 Auto-tuned
Dispatch가 이 측정 코드를 공유하므로 비교 대상마다 타이머나 통계 처리 방식이 달라지지 않는다.

### 6.1 타이머

- Linux와 macOS: `clock_gettime()`의 `CLOCK_MONOTONIC_RAW` 또는 `CLOCK_MONOTONIC`
- Windows: `QueryPerformanceCounter()`와 `QueryPerformanceFrequency()`

Linux 타이머 frequency는 `1,000,000,000`으로 정의되므로 tick을 nanosecond로 직접 변환할 수 있다.
wall clock 변경의 영향을 받지 않는 monotonic clock을 사용한다.

### 6.2 Warm-up

측정 전에 암호화 함수를 `warmup` 횟수만큼 호출한다. 통합 benchmark의 기본 warm-up은 200회다.
이는 최초 instruction/data 접근이나 초기 실행 상태가 본 측정 표본에 직접 포함되는 것을 줄이기
위한 구현이다.

### 6.3 Adaptive inner iteration

짧은 암호화 호출 하나만 측정하면 timer 해상도와 호출 overhead 비중이 커진다. 이를 줄이기 위해
`pick_inner()`가 inner iteration을 1에서 시작해 두 배씩 증가시킨다. 누적 실행 시간이 목표 tick에
도달하면 해당 inner 값을 모든 outer sample에서 사용한다.

통합 실행의 목표 시간은 outer sample당 약 10ms이며, inner 상한은 `2^20`이다. 측정 시간이 0으로
나오는 경우에는 더 빠르게 inner를 증가시키는 보호 처리도 포함한다.

### 6.4 측정 루프와 최적화 방지

각 inner iteration은 다음 순서로 수행된다.

1. 입력 버퍼의 한 byte를 변경한다.
2. 선택한 ARIA `encrypt` 함수를 호출한다.
3. 출력의 처음, 중간, 마지막 byte를 `sink`에 섞는다.

입력을 변경하고 결과를 sink에 누적함으로써 컴파일러가 반복 호출을 상수 계산이나 사용되지 않는
결과로 제거하기 어렵게 만든다. 최종 sink는 실행 종료 시 출력되어 측정 경로가 실제로 소비됐음을
남긴다.

### 6.5 Empty-loop correction

각 outer sample에서 실제 함수 측정 직후 동일한 wrapper로 빈 함수 `empty_encrypt()`를 측정한다.

```text
corrected_ticks = total_ticks - empty_ticks
```

`total_ticks <= empty_ticks`인 표본은 유효한 corrected sample로 사용할 수 없으므로 폐기하고 다시
측정한다. 요청 outer sample 수의 최대 8배까지 재시도한다. CSV와 최종 통계는 유효한 corrected
sample만 사용한다.

### 6.6 시간 단위 변환

각 corrected sample은 다음 식으로 변환한다.

```text
ns_total = corrected_ticks × 10^9 / timer_frequency
ns_per_call = ns_total / inner_iterations
ns_per_byte = ns_total / (inner_iterations × message_length)
```

후보 winner와 Policy 검증의 기본 비교 기준은 `trimmed_mean_ns_per_call`이다. 메시지 크기가 같은
후보끼리 한 번의 요청을 완료하는 시간을 직접 비교하기 때문이다. `ns_per_byte`는 서로 다른 입력
길이에서 단위 byte당 효율을 관찰하는 성능 지형 Figure에 사용한다.

## 7. 통계 처리

각 outer sample의 corrected `ns_per_call`을 배열로 보관한 뒤 다음 통계를 계산한다.

- Raw mean
- Trimmed mean
- Median 또는 P50
- P95, P99
- Minimum, Maximum
- Standard deviation
- Interquartile range, IQR
- Raw/trimmed mean ns per byte

기본 비교 통계는 10% trimmed mean이다. 표본을 정렬하고 양쪽에서
`floor(sample_count × 0.10)`개씩 제거한 뒤 남은 값의 평균을 계산한다. 양쪽 제거 후 최소 3개
표본이 남지 않는 설정은 autotune 시작 전에 거부한다.

Profile별 기본 outer sample 수는 다음과 같다.

| Profile | 범위 | Coarse outer | Fine/Validation outer |
|---|---:|---:|---:|
| `test` | 16~512 bytes | 15 | 25 |
| `full` | 16~4096 bytes | 21 | 41 |

Auto-tuning 측정 후보 순서는 key size와 길이에 따라 결정론적으로 회전한다. 특정 구현이 항상 첫 번째
또는 마지막에 측정되는 순서 편향을 줄이면서도 동일 조건에서 순서를 재현할 수 있도록 구현했다.

## 8. Coarse Scan 구현

`generate_coarse_lengths()`는 전체 범위를 16-byte 간격으로 모두 측정하지 않고 다음 지점을 합쳐
Coarse grid를 만든다.

1. 설정 범위의 첫 길이와 마지막 길이
2. 2의 거듭제곱 길이
3. 각 거듭제곱 구간의 중간인 1.5배 길이
4. 16-byte 기본 초기 anchor인 16, 32, 48 bytes
5. AVX 256-byte 구조 단위의 모든 배수와 각 지점의 ±16 bytes
6. AVX2 512-byte 구조 단위의 모든 배수와 각 지점의 ±16 bytes
7. GFNI 1024-byte 구조 단위의 모든 배수와 각 지점의 ±16 bytes

중복 지점은 제거하고 오름차순으로 정렬한다. SIMD 구조 단위의 배수와 인접 지점을 포함한 이유는
chunk가 새로 시작되는 정확한 지점과 마지막 Ref tail이 가장 큰 지점을 함께 관측하기 위해서다.
이는 Coarse/Fine Scan 구조 안에서 경계 누락을 막기 위한 grid 구성이다.

각 Coarse 길이와 128/192/256-bit key에 대해 지원 가능한 모든 후보를 `bench_run()`으로 측정한다.
가장 작은 `trimmed_mean_ns_per_call`을 raw winner로 저장하고, 두 번째 후보와의 차이는 다음 margin으로
기록한다.

```text
margin (%) = (second_time - best_time) / best_time × 100
```

## 9. Effective Path와 Policy Basis

작은 입력에서 SIMD 후보를 호출해도 실제로는 Ref 또는 더 좁은 SIMD 경로만 실행될 수 있다.
따라서 후보 등록 이름과 실제 실행 경로를 별도로 기록한다.

- Raw basis: 측정한 후보 구현 이름 자체를 비교
- Normalized basis: `effective_path`를 실제 구현 class로 변환해 비교

기본값은 `normalized` basis다. 예를 들어 AVX2 후보가 256 bytes에서 32-way AVX2 없이 16-way AVX만
실행하면 AVX class로 정규화한다. 동일 class를 실행한 여러 후보가 있으면 그중 가장 빠른 측정값을
해당 class의 값으로 사용한다.

`ref_fallback`은 기본적으로 Ref class로 합친다. Ref tail을 포함하는 mixed path의 처리 방식은
기존 `tail_policy` 설정에 따르며 기본 `native`에서는 SIMD 후보 class를 유지한다.

## 10. Fine Scan 구현

Coarse point를 key size와 길이순으로 비교하여 인접한 두 point의 winner가 달라지는 구간을 boundary
candidate로 만든다. 기본 normalized basis에서는 normalized winner가 달라진 경계만 Fine Scan
대상이다.

Fine Scan은 각 boundary candidate의 왼쪽 Coarse 길이부터 오른쪽 Coarse 길이까지
`refine_step` 간격으로 측정한다. 기본 `refine_step`은 ARIA block 크기와 같은 16 bytes다.
동일한 경계 구간이 겹치면 길이를 중복 측정하지 않는다.

Fine 결과와 Coarse 결과를 병합할 때 동일한 key size와 길이가 양쪽에 있으면 반복 횟수가 더 많은
Fine 결과를 우선한다. Fine Scan 대상이 없다면 빈 Fine 결과로 정상 종료하고 이후 Policy 생성으로
진행한다.

## 11. 측정점 정리와 Policy 생성

병합된 측정점에는 기존 안정화 규칙을 순서대로 적용한다.

1. Winner가 바뀌었지만 margin이 설정값보다 작은 point는 이전 안정 구간에 유지한다.
2. `stability_min_run`보다 짧은 winner run을 인접한 안정 run으로 합친다.
3. 양쪽 winner가 같은 단일 island를 제거한다.
4. `policy_min_bucket_points`보다 짧은 최종 bucket을 인접 bucket으로 합친다.

기본 margin은 5%, 최소 안정 run과 최소 Policy bucket은 각각 3개 측정점이다. 이 처리는 런타임
상태에 따라 경계를 계속 변경하는 히스테리시스가 아니다. 한 번의 측정 결과에서 짧은 잡음성 구간을
정리하여 정적인 Policy Map을 생성하는 기존 후처리다.

동일한 winner가 연속되는 측정점은 하나의 Policy bucket으로 합친다. 각 bucket은 다음 정보를 가진다.

```text
key_bits
start_len
end_len
policy_chosen_impl
representative_effective_path
source_phase
margin과 안정화 note
```

## 12. Policy 유효성 검증과 설치

생성된 Policy는 설치 전에 다음 조건을 검사한다.

- 구현 이름이 실제 등록된 구현인지
- 현재 CPU에서 해당 구현을 지원하는지
- `start_len <= end_len`인지
- 시작과 끝이 모두 16-byte 정렬인지
- 128/192/256-bit key 각각에서 `min_len`부터 `max_len`까지 빈 구간 없이 연속인지
- 마지막 bucket이 설정된 최대 길이까지 정확히 덮는지

하나라도 실패하면 Policy 전체를 설치하지 않고 기존 Policy를 clear한다. 검증을 통과하면
`aria_policy_install()`이 Runtime Dispatch에서 사용할 range table로 복사한다.

## 13. Exhaustive Policy Validation

Policy 설치 후 `validate_policy_against_full_search()`가 별도의 exhaustive pass를 수행한다.
Auto-tuning에 사용한 측정값을 정답으로 재사용하지 않고, 설정 범위의 모든 16-byte 정렬 길이를
다시 측정한다.

```text
16, 32, 48, ... , 4096 bytes
```

각 길이에서 모든 지원 후보를 다시 측정해 reference winner를 정하고, 설치된 Policy가 선택한 구현과
비교한다. 두 측정은 같은 `aria_impl_t.encrypt`와 `bench_run()`을 사용하므로 Auto-tuning과 실제
성능 평가가 서로 다른 암호화 경로를 사용하는 문제를 방지한다.

### 13.1 Policy 지표

```text
Exact Policy Accuracy
  = exact_match_points / validation_point_count × 100

Policy Regret (%)
  = (policy_time - exhaustive_best_time) / exhaustive_best_time × 100

Within-1% Accuracy
  = policy_time <= exhaustive_best_time × 1.01 인 지점 비율

Within-3% Accuracy
  = policy_time <= exhaustive_best_time × 1.03 인 지점 비율

Measurement Reduction (%)
  = (1 - autotune_candidate_measurements / exhaustive_candidate_measurements) × 100
```

Policy boundary와 exhaustive winner boundary 사이의 차이는 양방향 nearest-boundary distance 평균으로
계산한다. 한쪽 boundary만 비교하지 않고 Policy→Reference와 Reference→Policy를 모두 포함한다.

Mismatch는 16-byte step에서 연속된 불일치 point를 하나의 range로 합쳐 다음과 같이 출력한다.

```text
3328-3568;3840-4080
```

추가로 mismatch point 수, interval 수, 가장 긴 mismatch 길이를 저장한다.

## 14. 최종 평가 코드

`dispatch_evaluation.c`는 128/192/256-bit key와 다음 대표 길이를 측정한다.

```text
16, 32, 64, 128, 192, 256, 320, 512, 1024, 2048, 4096
```

평가 mode는 다음 네 가지다.

| 코드상 mode | 의미 |
|---|---|
| `direct_reference` | 모든 길이를 Ref 구현으로 직접 실행하는 기준선 |
| `best_fixed_implementation` | 평가 범위 전체에 하나의 구현만 고정했을 때 macro-average가 가장 작은 구현 |
| `static_heuristic_dispatch` | CPU feature와 256/512/1024-byte 고정 경계를 사용하는 Runtime Dispatch |
| `autotuned_policy_dispatch` | Coarse/Fine Scan으로 생성·설치한 Policy Map lookup 결과 |

Best Fixed Implementation은 길이마다 exhaustive winner를 고르는 Oracle Policy가 아니다. 후보 하나를
전체 길이에 고정해 측정한 결과 중 평균 시간이 가장 작은 후보를 뜻한다.

Static 및 Auto-tuned mode는 lookup을 포함한 wrapper 전체를 다시 측정한다. 같은 구현을 직접 호출한
시간과의 차이를 `dispatch_overhead_ns_per_call`로 기록한다.

### 14.1 Throughput과 Speedup

```text
Throughput (bytes/s)
  = message_length × 10^9 / trimmed_mean_ns_per_call

Throughput (MiB/s)
  = Throughput (bytes/s) / (1024 × 1024)

Speedup over Direct Reference
  = direct_reference_time / selected_mode_time
```

길이별 throughput은 `dispatch_evaluation.csv`에 기록한다. 여러 key/길이를 합친 summary에서는
시간과 throughput은 산술 macro-average, speedup은 로그 평균 후 지수화한 geometric mean을 사용한다.

## 15. CSV 출력 구조

핵심 CSV의 역할은 다음과 같다.

| 파일 | 생성 코드 | 주요 내용 |
|---|---|---|
| `summary_stats.csv` | `main.c` | 대표 길이별 후보 성능과 실제 mixed-width path |
| `raw_samples.csv` | `main.c` | outer sample별 total/empty/corrected 시간 |
| `autotune_scan_points.csv` | `autotune.c` | Coarse/Fine에서 실제 측정한 길이 |
| `raw_best_by_length.csv` | `autotune.c` | 병합된 측정점의 raw winner와 effective path |
| `autotune_policy.csv` | `autotune.c` | 최종 Policy bucket |
| `autotune_policy.json` | `autotune.c` | 설정과 Policy를 함께 저장한 구조화 결과 |
| `policy_validation.csv` | `autotune.c` | 16-byte dense exhaustive 시간, winner, regret, mixed-width path |
| `policy_validation_summary.csv` | `autotune.c` | key별 Policy Accuracy, boundary, mismatch 요약 |
| `autotune_metrics.csv` | `autotune.c` | 생성 시간, 측정 수, exhaustive 대비 reduction |
| `dispatch_evaluation.csv` | `dispatch_evaluation.c` | mode/key/길이별 시간, throughput, speedup, dispatch overhead |
| `dispatch_evaluation_summary.csv` | `dispatch_evaluation.c` | mode별 macro/geometric summary |
| `framework_validation_summary.csv` | `autotune.c` | CPU, 정렬, 범위, fallback 등 기본 유효성 검사 |

`--output-level debug`에서는 `autotune_coarse.csv`, `autotune_refined.csv`,
`autotune_boundaries.csv`도 출력한다. `raw` level에서는 outer sample까지 보존한다.

## 16. Figure 생성 코드

Figure는 Python 3와 Matplotlib로 생성한다. 모든 plotting script는 `plot_common.py`의 CSV loader,
구현별 색상, 레이블, 축 스타일과 저장 함수를 공유한다. 입력 CSV에 필수 column이 없으면 임의로
추측하지 않고 오류로 종료한다.

### 16.1 공통 출력 기준

- 출력 형식: PNG만 생성
- 저장 해상도: 300 DPI
- 배경: 흰색
- 공통 font: DejaVu Sans
- 구현별 색상과 범례 순서 고정
- 긴 입력 길이 범위의 x축: log2
- 성능의 큰 값 범위를 비교하는 y축: 필요 시 log10
- 논문 Figure에 필요한 대표 tick만 표시
- 지원되지 않아 측정값이 `-1`인 후보는 곡선과 범례에서 제외

모든 Figure는 `save_figure()`를 통해 `fig_*.png` 한 개만 저장한다. SVG, PDF 또는 중복 해상도 파일은
생성하지 않는다.

### 16.2 Dense 성능 Figure

파일: `fig_perf_key128_ns_byte.png`

코드: `scripts/plot_ns_per_byte.py`

데이터는 sparse한 `summary_stats.csv`가 아니라 `policy_validation.csv`의 exhaustive 16-byte step
결과를 사용한다. 각 후보의 `trimmed_mean_ns_per_call`을 길이로 나눠 ns/byte로 변환한다.

```text
y = candidate_trimmed_mean_ns_per_call / message_length
```

x축은 log2, y축은 log10이다. Dense curve의 point marker는 256개 지점이 겹치지 않도록 제거한다.
256, 512, 1024 bytes에는 각각 AVX 16-way, AVX2 32-way, GFNI 64-way 구조 경계를 세로선으로
표시한다. CPU에서 지원되지 않은 후보는 표시하지 않는다.

### 16.3 Raw Winner vs. Policy Figure

파일: `fig_raw_vs_policy_key128.png`

코드: `scripts/plot_raw_vs_policy.py`

위쪽 Raw Winner Map은 `raw_best_by_length.csv`의 실제 winner를 사용한다. 후보 등록 이름이 아니라
`raw_best_effective_path`를 canonical implementation으로 변환하므로 작은 입력의 Ref fallback이나
AVX2 후보 내부의 AVX-only 실행을 잘못 AVX2로 표시하지 않는다.

아래 Stabilized Policy Map은 `autotune_policy.csv`의 bucket을 그대로 표시한다. 인접한 동일 winner
지점은 하나의 색상 영역으로 합친다. 로그 축에서 너무 좁은 영역은 색상은 유지하되 내부 텍스트만
생략하여 label 겹침을 방지한다.

### 16.4 Policy Map Comparison Figure

파일: `fig_policy_comparison_key128.png`

코드: `scripts/plot_policy_comparison.py`

다음 세 map을 동일한 16-byte grid와 x축으로 비교한다.

1. Best Fixed Implementation: 평가 범위 전체에 하나의 구현 고정
2. Static Heuristic: CPU feature와 고정 threshold로 계산
3. Auto-tuned Policy: 생성된 Policy bucket 사용

Best Fixed 후보는 `dispatch_evaluation.csv`, Static Heuristic의 CPU 활성 상태는 `run_meta.json`,
Auto-tuned 구간은 `autotune_policy.csv`에서 읽는다.

### 16.5 Scan Coverage Figure

파일: `fig_scan_coverage.png`

코드: `scripts/plot_scan_coverage.py`

`autotune_scan_points.csv`의 실제 측정 길이를 key size별로 표시한다. Coarse는 파란색, Fine은
주황색 vertical marker를 사용한다. 이 Figure로 Fine Scan이 전체 범위를 다시 탐색한 것이 아니라
Coarse boundary 후보 주변만 측정했는지 확인할 수 있다.

### 16.6 Framework Performance Figure

파일: `fig_framework_performance.png`

코드: `scripts/plot_framework_evaluation.py`

`dispatch_evaluation_summary.csv`에서 네 평가 mode의 macro-average trimmed mean 실행 시간과 Direct
Reference 대비 geometric-mean speedup을 두 subplot으로 표시한다. 시간은 낮을수록 좋고 speedup은
1보다 클수록 Direct Reference보다 빠르다.

### 16.7 Policy Validation Figure

파일: `fig_policy_validation.png`

코드: `scripts/plot_framework_evaluation.py`

`policy_validation_summary.csv`와 `autotune_metrics.csv`를 사용해 다음을 표시한다.

- Exact Policy Accuracy와 Within-1% Accuracy
- Mean/Maximum Policy Regret
- Mean Symmetric Boundary Distance
- Search time과 end-to-end tuning time
- Auto-tuning/exhaustive candidate measurement 수
- Measurement reduction
- Mismatched ranges

성능 Figure와 Policy 품질 Figure를 분리하여 한 PNG에 서로 다른 척도의 지표가 과도하게 겹치지
않도록 구성했다.

## 17. 통합 실험 실행

Release build와 테스트:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

Full profile 실험과 모든 핵심 Figure 생성:

```bash
./scripts/run_autotune_and_plot.sh full out/policy_full default
```

통합 스크립트는 다음 순서로 실행된다.

1. 출력 경로가 `out/` 아래인지 확인한다.
2. 해당 실험 출력 디렉터리의 기존 결과만 정리한다.
3. 전체 CTest를 실행한다.
4. `bench_runner --autotune`을 실행한다.
5. dense 성능, Raw/Policy, Policy 비교, Scan coverage Figure를 생성한다.
6. Framework performance와 Policy validation Figure를 생성한다.
7. 생성된 CSV와 PNG 목록을 출력한다.

`default`, `debug`, `raw` output level을 선택할 수 있으며 논문용 최종 수치는 Release build와 full
profile을 사용한다. 진단을 위해 iteration 수를 낮춘 실행 결과는 코드 경로 확인에는 사용할 수
있지만 논문 최종 성능 수치로 사용하지 않는다.

## 18. 정확성 및 회귀 테스트

CTest에는 다음 검사가 포함된다.

| 테스트 | 검증 내용 |
|---|---|
| `bench_stats` | trimmed mean, percentile 등 통계 함수 |
| `aria_kat` | ARIA known-answer test 및 Ref 대비 ciphertext 일치 |
| `aria_policy_dispatch` | Policy 경계, gap, 정렬, fallback과 effective path |
| `aria_policy_metrics` | boundary distance와 mismatch range 계산 |

`aria_kat`는 128/192/256-bit key에 대해 16~4096 bytes의 모든 16-byte 정렬 길이를 Ref와 비교한다.
AVX2 mixed-width 경계인 240/256/272, 496/512/528, 752/768/784, 1008/1024/1040 bytes에서는
chunk count도 별도로 검사한다.

## 19. 논문에서 구현을 기술할 때의 핵심 요약

논문의 Implementation 또는 Experimental Methodology 절에서는 다음과 같이 요약할 수 있다.

1. 모든 후보 구현은 동일한 `aria_impl_t` interface로 등록된다.
2. CPU feature로 실행 불가능한 후보는 측정 전에 제외된다.
3. 측정은 monotonic timer, warm-up, adaptive inner iteration, empty-loop correction 및 10% trimmed
   mean을 공통으로 사용한다.
4. Coarse Scan은 log backbone과 SIMD 구조 경계를 측정한다.
5. Fine Scan은 Coarse winner가 바뀐 구간만 16-byte 간격으로 다시 측정한다.
6. 연속 winner를 정적인 Policy range로 만들고 전체 범위의 연속성 및 정렬을 검증한 뒤 설치한다.
7. Policy 품질은 별도의 16-byte step exhaustive pass와 비교한다.
8. 성능 평가는 Direct Reference, Best Fixed, Static Heuristic, Auto-tuned Policy를 동일 측정 코드로
   비교한다.
9. 모든 Figure는 CSV를 source of truth로 사용하며 Python/Matplotlib로 300-DPI PNG만 생성한다.

이 구조에서 ARIA는 framework를 검증하기 위한 case study이며, 측정·통계·Policy 검증·Figure 생성
코드는 후보 암호 구현과 가능한 한 분리되어 있다.
