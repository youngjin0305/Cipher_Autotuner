#include "aria_api.h"
#include "aria-avx.h"
#include "cpu_features.h"

#include <assert.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define ARIA_LINUX_AESNI_AVX_BUILDABLE 1
#else
#define ARIA_LINUX_AESNI_AVX_BUILDABLE 0
#endif

static int aria_linux_aesni_avx_is_supported(void)
{
  return ARIA_LINUX_AESNI_AVX_BUILDABLE ? aria_cpu_has_aesni_avx() : 0;
}

static const char *aria_linux_aesni_avx_effective_path(size_t len)
{
  if (len < ARIA_AESNI_PARALLEL_BLOCK_SIZE) {
    return "ref_fallback";
  }
  if ((len % ARIA_AESNI_PARALLEL_BLOCK_SIZE) == 0) {
    return "linux_aesni_avx";
  }
  return "linux_aesni_avx_plus_ref_tail";
}

static void aria_linux_aesni_avx_encrypt(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len)
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

#if ARIA_LINUX_AESNI_AVX_BUILDABLE
  for (; off + ARIA_AESNI_PARALLEL_BLOCK_SIZE <= len; off += ARIA_AESNI_PARALLEL_BLOCK_SIZE) {
    aria_aesni_avx_encrypt_16way(ctx, out + off, in + off);
  }
#endif

  if (off < len) {
    aria_ref_impl.encrypt(ctx, in + off, out + off, len - off);
  }
}

const aria_impl_t aria_linux_aesni_avx_impl = {
  "linux_aesni_avx",
  aria_linux_aesni_init,
  aria_linux_aesni_avx_encrypt,
  aria_linux_aesni_avx_is_supported,
  aria_linux_aesni_avx_effective_path
};
