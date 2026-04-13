#include "aria-avx.h"
#include "aria_ref_core.h"

#include <string.h>

static void aria_linux_pack_words(uint8_t *dst, const uint8_t *src)
{
  size_t off;

  if (!dst || !src) {
    return;
  }

  for (off = 0; off < ARIA_ROUND_KEY_BYTES; off += 4) {
    dst[off + 0] = src[off + 3];
    dst[off + 1] = src[off + 2];
    dst[off + 2] = src[off + 1];
    dst[off + 3] = src[off + 0];
  }
}

void aria_linux_aesni_init(aria_ctx_t *ctx, const uint8_t *key, int keybits)
{
  if (!ctx) {
    return;
  }

  if (keybits != 128 && keybits != 192 && keybits != 256) {
    memset(ctx, 0, sizeof(*ctx));
    return;
  }
  if (!key) {
    memset(ctx, 0, sizeof(*ctx));
    return;
  }

  memset(ctx, 0, sizeof(*ctx));
  ctx->keybits = keybits;
  ctx->rounds = EncKeySetup((const Byte *)key, (Byte *)ctx->ref_rk, keybits);
  aria_linux_pack_words(ctx->linux_compat.enc_key, ctx->ref_rk);

  {
    uint8_t dec_rk[ARIA_ROUND_KEY_BYTES];

    (void)DecKeySetup((const Byte *)key, (Byte *)dec_rk, keybits);
    aria_linux_pack_words(ctx->linux_compat.dec_key, dec_rk);
  }
}
