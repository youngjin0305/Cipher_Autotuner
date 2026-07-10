# Cipher_Autotuner

다중 버전 ARIA 구현(`ref`, `linux_aesni_avx`, `linux_aesni_avx2`)의 성능을 측정하고,
입력 길이에 따른 런타임 디스패치 정책을 생성하기 위한 오토튜닝 프레임워크

## 실행

빌드
```
> cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
> cmake --build build --config Release
```

실험 스크립트
```
# 빠른 동작 확인
> bash scripts/run_autotune_and_plot.sh smoke out/autotune_smoke

# 논문/분석용 기본 실행
> bash scripts/run_autotune_and_plot.sh full out/autotune_full

# boundary/refinement CSV까지 필요할 때
> bash scripts/run_autotune_and_plot.sh full out/autotune_full_debug debug
```

통합 스크립트는 내부에서 아래 순서로 실행합니다.
1. `./build/aria_kat`
2. `./build/bench_runner --autotune ...`
3. plotting script 실행
4. 생성된 CSV/JSON/figure 목록 출력

기본 figure는 autotune policy 범위에 맞춰 그립니다.
현재 profile별 plot 범위는 `smoke=64`, `test=512`, `full=1024` bytes입니다.
`summary_stats.csv`에는 더 긴 benchmark 길이가 포함될 수 있지만, policy와 비교하는 figure에서는 이 범위로 잘라서 봅니다.

개별 명령이 필요할 때만 아래를 직접 실행합니다.
```
# KAT correctness 확인
> ./build/aria_kat

# 기본 benchmark
> ./build/bench_runner --output-dir out/bench_default

# autotune 직접 실행
> ./build/bench_runner --autotune --autotune-profile smoke --output-dir out/autotune_smoke
> ./build/bench_runner --autotune --autotune-profile full --output-dir out/autotune_full --output-level debug
```

Windows 실행 파일은 빌드 설정에 따라 아래 경로를 사용합니다.
```
# Windows
> .\build\Release\bench_runner.exe
> .\build\Release\aria_kat.exe 
```

## 오토튜닝 옵션

```
--autotune
--autotune-profile smoke | test | full
--output-dir <path>
--output-level default | debug | raw
--autotune-output-prefix <path>
```

권장 사용:
- `smoke`: 코드 변경 후 빠른 확인
- `full`: 논문/분석용 측정
- `default`: 핵심 CSV/JSON만 저장, 통합 스크립트 기본값
- `debug`: boundary/refinement 확인용 CSV까지 저장
- `raw`: outer sample 전체가 필요할 때만 사용

coarse scan 입력 길이는 아래 세 부류의 합집합으로 생성합니다.
- 로그 백본: 16B부터 `max_len`까지 octave별 `x1`, `x1.5` 지점. 단, ARIA block size인 16B 배수만 사용
- 구조점: 구현 처리 단위 `16`, `256`, `512` bytes의 `k*U`, `k*U-16`, `k*U+16` (`k=1..3`)
- 경계점: `min_len`, `max_len`

예를 들어 `full`에서는 `192`, `384`, `768` 같은 로그 백본 지점과 `240/256/272`, `496/512/528`, `1008/1024` 같은 구조점이 coarse 후보에 포함됩니다.

## 결과물 정리

```
summary_stats.csv       # 논문/그래프용 요약 성능 결과
run_meta.json           # 재현성 메타데이터
autotune_policy.csv     # 입력 길이 구간별 최종 정책
autotune_policy.json    # JSON 형태 최종 정책
figures/                # plotting 결과
```

추가 분석용 파일
```
results.csv             # benchmark outer 결과
autotune_coarse.csv
autotune_refined.csv
autotune_boundaries.csv
keysetup.csv
```

`--output-level raw`에서만 생성됨
```
raw_samples.csv
```

## Plotting

통합 스크립트를 쓰면 자동 실행.
```
> python3 scripts/plot_ns_per_call.py --input out/autotune_full/summary_stats.csv --out-dir figures
> python3 scripts/plot_ns_per_byte.py --input out/autotune_full/summary_stats.csv --out-dir figures
> python3 scripts/plot_policy_map.py --policy out/autotune_full/autotune_policy.csv --out-dir figures
> python3 scripts/plot_raw_vs_policy.py --summary out/autotune_full/summary_stats.csv --policy out/autotune_full/autotune_policy.csv --out-dir figures
> python3 scripts/plot_best_impl.py --input out/autotune_full/summary_stats.csv --out-dir figures
```

policy 범위와 x축을 맞추려면 `--max-len`을 지정합니다.
```
> python3 scripts/plot_ns_per_call.py --input out/autotune_full/summary_stats.csv --out-dir figures --max-len 1024
```

주요 figure:
```
fig_perf_key*_ns_call.*       # key size별 ns/call 성능 지형
fig_perf_key*_ns_byte.*       # key size별 ns/byte 보조 그래프
fig_policy_key*.*             # 최종 policy map
fig_raw_vs_policy_key*.*      # raw winner와 stabilized policy 비교
fig_best_impl_key*_ns_byte.*  # raw best implementation 그래프
```

- 프로젝트 임시 구조
```
\
|   CMakeLists.txt
|   LICENSE
|   README.md
|
+---bench
|   +---runner
|   \---scenarios
+---block_ciphers
|   \---aria
|       +---api
|       +---dispatch
|       |       autotune.c
|       |       runtime_dispatch.c
|       |
|       +---impl
|       |   +---avx2
|       |   +---avx512
|       |   +---gfni
|       |   +---ref
|       |   \---vaes
|       \---tests
+---common
|   +---include
|   +---platform
|   \---src
+---FOM
|       parse.py
|       summarize.py
|
\---scripts
        build.sh
        run_all.sh
        run_autotune_and_plot.sh
        plot_ns_per_call.py
        plot_ns_per_byte.py
        plot_policy_map.py
        plot_raw_vs_policy.py
        plot_best_impl.py
```

### 진행 상황 기록
1. 프로젝트 구조 설계
2. CMake 빌드
3. 실행 가능한 구조까지 코드 작성
4. 짧은 길이에서도 cycles가 의미 있게 나오게 해서, 이후 오토튜닝/임계값 탐색이 가능하도록 만들기
5. KISA의 ARIA ref 코드 추가
6. 실제 암호 연산 추가
7. KAT test 추가
8. Linux AES-NI/AVX, AES-NI/AVX2 구현 후보 연결
9. support check 기반 후보 필터링
10. 입력 길이 기반 rule-based 오토튜닝 구현
11. `effective_path` 기록 및 policy normalization 반영
12. `summary_stats.csv`, `run_meta.json`, policy CSV/JSON 산출물 정리
13. 논문용 성능 지형/policy/raw-vs-policy plotting 스크립트 정리

### 예정
1. full profile 측정 결과를 기반으로 정책 안정화 파라미터 점검
2. 생성된 정책 평가 코드 추가
3. 논문 본문/부록용 figure 선별
4. FOM 리포트 연결 보완
5. 필요 시 실제 런타임 디스패치 테이블 자동 반영
