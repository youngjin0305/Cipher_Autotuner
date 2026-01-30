#ifndef ARIA_API_H
#define ARIA_API_H

#include <stddef.h>
#include <stdint.h>

typedef struct aria_ctx {
  int rounds;
  int keybits;
  uint8_t rk[16 * 17];
} aria_ctx_t;

typedef void (*aria_encrypt_fn)(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len);

typedef struct aria_impl {
  const char *name;
  aria_encrypt_fn encrypt;
} aria_impl_t;

void aria_init(aria_ctx_t *ctx, const uint8_t *key, int keybits);

extern const aria_impl_t aria_ref_impl;

#endif
