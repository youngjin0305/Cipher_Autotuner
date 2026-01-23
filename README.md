# Cipher_Autotuner

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

진행 상황 기록
1. 프로젝트 구조 설계
2. CMake 빌드
3. 실행 가능한 구조까지 코드 작성(암호는 aria의 ref 더미 사용)
