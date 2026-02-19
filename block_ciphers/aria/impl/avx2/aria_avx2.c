#include "aria_api.h"
#include "aria_avx2_core.h"
#include <assert.h>

static void aria_avx2_encrypt(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len)
{
  if (!ctx || !in || !out) return;
  if (len == 0) return;

  assert((len & 15u) == 0);
  if ((len & 15u) != 0) return;

  size_t off = 0;

  for (; off + 64 <= len; off += 64) {
    aria_avx2_encrypt_4way(ctx, in + off, out + off);
  }

  if (off < len) {
    aria_ref_impl.encrypt(ctx, in + off, out + off, len - off);
  }
}

const aria_impl_t aria_avx2_impl = { "avx2", aria_avx2_encrypt };