# Cipher_Autotuner
빌드 후 실행
```
> cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
> cmake --build build --config Release

# Linux
> ./build/aria_kat
> ./build/bench_runner

# Windows
> .\build\Release\bench_runner.exe
> .\build\Release\aria_kat.exe 
```

지표 시각화
```
> python3 scripts/plot_ns_per_byte.py --input out/summary_stats.csv --output-dir figures
> python3 scripts/plot_best_impl.py --input out/summary_stats.csv --output-dir figures
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
```

### 진행 상황 기록
1. 프로젝트 구조 설계
2. CMake 빌드
3. 실행 가능한 구조까지 코드 작성
4. 짧은 길이에서도 cycles가 의미 있게 나오게 해서, 이후 오토튜닝/임계값 탐색이 가능하도록 만들기
5. KISA의 ARIA ref 코드 추가
6. 실제 암호 연산 추가
7. KAT test 추가

### 예정
1. 멀티버전 구현 1개 추가(AVX2부터)
2. 실행 CPU에서 가능한 구현만 선택하도록 설정
3. 오토튜너(길이 구간별 best 캐시) 구현
4. 실험 자동화/리포트(FOM) 연결

