#ifndef ARIA_API_H
#define ARIA_API_H

#include <stddef.h>
#include <stdint.h>

#define ARIA_BLOCK_SIZE 16u
#define ARIA_ROUND_KEY_BYTES (ARIA_BLOCK_SIZE * 17u)

typedef struct aria_ctx {
  int rounds;
  struct {
    uint8_t enc_key[ARIA_ROUND_KEY_BYTES];
    uint8_t dec_key[ARIA_ROUND_KEY_BYTES];
  } linux_compat;
  uint8_t ref_rk[ARIA_ROUND_KEY_BYTES];
  int keybits;
} aria_ctx_t;

typedef void (*aria_init_fn)(aria_ctx_t *ctx, const uint8_t *key, int keybits);
typedef void (*aria_encrypt_fn)(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len);
typedef int (*aria_support_fn)(void);
typedef const char *(*aria_effective_path_fn)(size_t len);

typedef struct aria_impl {
  const char *name;
  aria_init_fn init;
  aria_encrypt_fn encrypt;
  aria_support_fn is_supported;
  aria_effective_path_fn effective_path;
} aria_impl_t;

void aria_init(aria_ctx_t *ctx, const uint8_t *key, int keybits);
void aria_ref_init(aria_ctx_t *ctx, const uint8_t *key, int keybits);

extern const aria_impl_t aria_ref_impl;
extern const aria_impl_t aria_linux_aesni_avx_impl;
extern const aria_impl_t aria_linux_aesni_avx2_impl;
extern const aria_impl_t aria_linux_gfni_avx512_impl;

#endif
