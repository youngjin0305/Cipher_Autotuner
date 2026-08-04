#include "aria_api.h"
#include "aria-avx.h"
#include "cpu_features.h"

#include <assert.h>
#include <string.h>

#if defined(ARIA_HAVE_LINUX_X86_ASM) && ARIA_HAVE_LINUX_X86_ASM
#define ARIA_LINUX_GFNI_AVX512_BUILDABLE 1
#else
#define ARIA_LINUX_GFNI_AVX512_BUILDABLE 0
#endif

static int aria_linux_gfni_avx512_is_supported(void)
{
  return ARIA_LINUX_GFNI_AVX512_BUILDABLE ? aria_cpu_has_gfni_avx512() : 0;
}

static const char *aria_linux_gfni_avx512_effective_path(size_t len)
{
  if (len < ARIA_AESNI_PARALLEL_BLOCK_SIZE) {
    return "ref_fallback";
  }
  if ((len % ARIA_GFNI_AVX512_PARALLEL_BLOCK_SIZE) == 0) {
    return "linux_gfni_avx512";
  }
  if ((len % ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE) == 0) {
    return len < ARIA_GFNI_AVX512_PARALLEL_BLOCK_SIZE
               ? "linux_gfni_avx2_32way"
               : "linux_gfni_mixed_width";
  }
  if ((len % ARIA_AESNI_PARALLEL_BLOCK_SIZE) == 0) {
    return len < ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE
               ? "linux_gfni_avx_16way"
               : "linux_gfni_mixed_width";
  }
  return "linux_gfni_plus_ref_tail";
}

static void aria_linux_gfni_avx512_execution_path(size_t len, aria_execution_path_t *path)
{
  size_t blocks;

  if (!path) return;
  memset(path, 0, sizeof(*path));
  blocks = len / ARIA_BLOCK_SIZE;
  path->gfni_64way_chunk_count = blocks / ARIA_GFNI_AVX512_PARALLEL_BLOCKS;
  blocks %= ARIA_GFNI_AVX512_PARALLEL_BLOCKS;
  path->avx2_32way_chunk_count = blocks / ARIA_AESNI_AVX2_PARALLEL_BLOCKS;
  blocks %= ARIA_AESNI_AVX2_PARALLEL_BLOCKS;
  path->avx_16way_chunk_count = blocks / ARIA_AESNI_PARALLEL_BLOCKS;
  path->ref_tail_block_count = blocks % ARIA_AESNI_PARALLEL_BLOCKS;
}

static void aria_linux_gfni_avx512_encrypt(const aria_ctx_t *ctx,
                                           const uint8_t *in,
                                           uint8_t *out,
                                           size_t len)
{
  size_t off = 0;

  if (!ctx || !in || !out || len == 0) {
    return;
  }

  assert((len & (ARIA_BLOCK_SIZE - 1u)) == 0);
  if ((len & (ARIA_BLOCK_SIZE - 1u)) != 0) {
    return;
  }

#if ARIA_LINUX_GFNI_AVX512_BUILDABLE
  for (; off + ARIA_GFNI_AVX512_PARALLEL_BLOCK_SIZE <= len;
       off += ARIA_GFNI_AVX512_PARALLEL_BLOCK_SIZE) {
    aria_gfni_avx512_encrypt_64way(ctx, out + off, in + off);
  }
  for (; off + ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE <= len;
       off += ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE) {
    aria_aesni_avx2_gfni_encrypt_32way(ctx, out + off, in + off);
  }
  for (; off + ARIA_AESNI_PARALLEL_BLOCK_SIZE <= len;
       off += ARIA_AESNI_PARALLEL_BLOCK_SIZE) {
    aria_aesni_avx_gfni_encrypt_16way(ctx, out + off, in + off);
  }
#endif

  if (off < len) {
    aria_ref_impl.encrypt(ctx, in + off, out + off, len - off);
  }
}

const aria_impl_t aria_linux_gfni_avx512_impl = {
  "linux_gfni_avx512",
  aria_linux_aesni_init,
  aria_linux_gfni_avx512_encrypt,
  aria_linux_gfni_avx512_is_supported,
  aria_linux_gfni_avx512_effective_path,
  aria_linux_gfni_avx512_execution_path
};
