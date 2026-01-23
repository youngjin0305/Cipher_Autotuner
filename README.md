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