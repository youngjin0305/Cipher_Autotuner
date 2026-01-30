#include "aria_api.h"
#include "aria_ref_core.h"

#include <string.h>
#include <assert.h>

void aria_init(aria_ctx_t *ctx, const uint8_t *key, int keybits)
{
  if (!ctx) return;

  if (keybits != 128 && keybits != 192 && keybits != 256) {
    memset(ctx, 0, sizeof(*ctx));
    return;
  }
  if (!key) {
    memset(ctx, 0, sizeof(*ctx));
    return;
  }

  ctx->keybits = keybits;
  ctx->rounds = EncKeySetup((const Byte *)key, (Byte *)ctx->rk, keybits);
}

static void aria_ref_encrypt(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len)
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

const aria_impl_t aria_ref_impl = {
  "ref",
  aria_ref_encrypt,
};
