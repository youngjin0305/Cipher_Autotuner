#include "aria_api.h"
#include "aria_ref_core.h"

#include <string.h>
#include <assert.h>

static int aria_ref_is_supported(void)
{
  return 1;
}

static const char *aria_ref_effective_path(size_t len)
{
  (void)len;
  return "ref";
}

void aria_ref_init(aria_ctx_t *ctx, const uint8_t *key, int keybits)
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
  ctx->rounds = EncKeySetup((const Byte *)key, (Byte *)ctx->ref_rk, keybits);
}

void aria_init(aria_ctx_t *ctx, const uint8_t *key, int keybits)
{
  aria_ref_init(ctx, key, keybits);
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
          (const Byte *)ctx->ref_rk,
          (Byte *)(out + off));
  }
}

const aria_impl_t aria_ref_impl = {
  "ref",
  aria_ref_init,
  aria_ref_encrypt,
  aria_ref_is_supported,
  aria_ref_effective_path
};
