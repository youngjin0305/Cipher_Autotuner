# Cipher_Autotuner
빌드 후 실행
```
> cmake -S . -B build
> cmake --build build --config Release
> .\build\Release\bench_runner.exe
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
3. 실행 가능한 구조까지 코드 작성(암호는 aria의 ref 더미 사용)

### 예정
1. 짧은 길이에서도 cycles가 의미 있게 나오게 해서, 이후 오토튜닝/임계값 탐색이 가능하도록 만들기
2. 더미가 아니라 실제 암호 연산이 돌아가게 만들기 + correctness 보장
3. 멀티버전 구현 1개 추가(AVX2부터)
4. 실행 CPU에서 가능한 구현만 선택하도록 설정
5. 오토튜너(길이 구간별 best 캐시) 구현
6. 실험 자동화/리포트(FOM) 연결

