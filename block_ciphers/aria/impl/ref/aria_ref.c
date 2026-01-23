#include "aria_api.h"

#include <string.h>

static void aria_ref_encrypt(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len) 
{
  (void)ctx;
  if (len == 0) {
    return;
  }
  memcpy(out, in, len);
}

const aria_impl_t aria_ref_impl = {
  "ref",
  aria_ref_encrypt,
};
