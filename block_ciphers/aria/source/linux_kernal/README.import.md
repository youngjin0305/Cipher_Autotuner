## Linux ARIA Import Notes

Upstream source:
- Linux kernel x86 ARIA AES-NI/AVX and AES-NI/AVX2 implementation files

Imported files:
- `aesni_avx/aria-aesni-avx-asm_64.S`
- `aesni_avx/aria_aesni_avx_glue.c`
- `aesni_avx2/aria-aesni-avx2-asm_64.S`
- `aesni_avx2/aria_aesni_avx2_glue.c`

Imported into this repo:
- 2026-04-13

Local adaptation notes:
- The imported `*_glue.c` files are kept as references only and are not built in userspace.
- Userspace wrappers live under `block_ciphers/aria/impl/linux_aesni_avx/` and `block_ciphers/aria/impl/linux_aesni_avx2/`.
- Minimal compatibility headers for asm symbols, constants, and context offsets live under `block_ciphers/aria/source/linux_kernal/common/`.
- The asm sources were patched only at the include/macro boundary so they can build outside the kernel.
