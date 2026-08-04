#include "aria_api.h"
#include "aria-avx.h"
#include "cpu_features.h"

#include <assert.h>
#include <string.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define ARIA_LINUX_AESNI_AVX2_BUILDABLE 1
#else
#define ARIA_LINUX_AESNI_AVX2_BUILDABLE 0
#endif

static int aria_linux_aesni_avx2_is_supported(void)
{
  return ARIA_LINUX_AESNI_AVX2_BUILDABLE ? aria_cpu_has_aesni_avx2() : 0;
}

static const char *aria_linux_aesni_avx2_effective_path(size_t len)
{
  const size_t blocks = len / ARIA_BLOCK_SIZE;
  const size_t avx2_chunks = blocks / ARIA_AESNI_AVX2_PARALLEL_BLOCKS;
  const size_t remainder_after_avx2 = blocks % ARIA_AESNI_AVX2_PARALLEL_BLOCKS;
  const size_t avx_chunks = remainder_after_avx2 / ARIA_AESNI_PARALLEL_BLOCKS;
  const size_t ref_blocks = remainder_after_avx2 % ARIA_AESNI_PARALLEL_BLOCKS;

  if (avx2_chunks == 0 && avx_chunks == 0) {
    return "ref_fallback";
  }
  if (avx2_chunks == 0) {
    return ref_blocks == 0 ? "linux_aesni_avx" : "linux_aesni_avx_plus_ref_tail";
  }
  if (avx_chunks == 0 && ref_blocks == 0) {
    return "linux_aesni_avx2";
  }
  if (avx_chunks != 0 && ref_blocks == 0) {
    return "linux_aesni_avx2_plus_avx_tail";
  }
  if (avx_chunks == 0) {
    return "linux_aesni_avx2_plus_ref_tail";
  }
  return "linux_aesni_avx2_plus_avx_ref_tail";
}

static void aria_linux_aesni_avx2_execution_path(size_t len, aria_execution_path_t *path)
{
  size_t blocks;
  size_t remainder;

  if (!path) return;
  memset(path, 0, sizeof(*path));
  blocks = len / ARIA_BLOCK_SIZE;
  path->avx2_32way_chunk_count = blocks / ARIA_AESNI_AVX2_PARALLEL_BLOCKS;
  remainder = blocks % ARIA_AESNI_AVX2_PARALLEL_BLOCKS;
  path->avx_16way_chunk_count = remainder / ARIA_AESNI_PARALLEL_BLOCKS;
  path->ref_tail_block_count = remainder % ARIA_AESNI_PARALLEL_BLOCKS;
}

static void aria_linux_aesni_avx2_encrypt(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len)
{
  size_t off = 0;

  if (!ctx || !in || !out) {
    return;
  }
  if (len == 0) {
    return;
  }

  assert((len & (ARIA_BLOCK_SIZE - 1u)) == 0);
  if ((len & (ARIA_BLOCK_SIZE - 1u)) != 0) {
    return;
  }

#if ARIA_LINUX_AESNI_AVX2_BUILDABLE
  for (; off + ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE <= len; off += ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE) {
    aria_aesni_avx2_encrypt_32way(ctx, out + off, in + off);
  }
  for (; off + ARIA_AESNI_PARALLEL_BLOCK_SIZE <= len; off += ARIA_AESNI_PARALLEL_BLOCK_SIZE) {
    aria_aesni_avx_encrypt_16way(ctx, out + off, in + off);
  }
#endif

  if (off < len) {
    aria_ref_impl.encrypt(ctx, in + off, out + off, len - off);
  }
}

const aria_impl_t aria_linux_aesni_avx2_impl = {
  "linux_aesni_avx2",
  aria_linux_aesni_init,
  aria_linux_aesni_avx2_encrypt,
  aria_linux_aesni_avx2_is_supported,
  aria_linux_aesni_avx2_effective_path,
  aria_linux_aesni_avx2_execution_path
};
