#include "aria_api.h"
#include "aria_ref_core.h"

#include <string.h>
#include <assert.h>

static void aria_avx2_encrypt(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len)
{
  if (!ctx || !in || !out) return;
  if (len == 0) return;

  assert((len & 15u) == 0);

  if ((len & 15u) != 0) return;

  for (size_t off = 0; off < len; off += 16) {
    Crypt((const Byte *)(in + off),
          ctx->rounds,
          (const Byte *)ctx->rk,
          (Byte *)(out + off));
  }
}

const aria_impl_t aria_avx2_impl = { "avx2", aria_avx2_encrypt };