#include "aria_api.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

static int hex2bytes(const char *hex, uint8_t *out, size_t out_len) {
  size_t n = 0;
  size_t i = 0;

  if (!hex || !out) {
    return -1;
  }

  while (hex[i] != '\0' && n < out_len) {
    while (hex[i] != '\0' && is_ws(hex[i])) {
      i++;
    }
    if (hex[i] == '\0') {
      break;
    }

    if (hex[i] == '\0' || hex[i + 1] == '\0') return -1;
    int hi = hex_value(hex[i]);
    int lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return -1;
    }

    out[n++] = (uint8_t)((hi << 4) | lo);
    i += 2;
  }

  return (n == out_len) ? 0 : -1;
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) {
  return memcmp(a, b, n) == 0;
}

static int expect_path(const aria_impl_t *impl,
                       size_t len,
                       size_t expected_avx2,
                       size_t expected_avx,
                       size_t expected_ref) {
  aria_execution_path_t path = {0, 0, 0, 0};

  if (!impl || !impl->execution_path) {
    printf("[FAIL] missing execution path metadata\n");
    return -1;
  }
  impl->execution_path(len, &path);
  if (path.avx2_32way_chunk_count != expected_avx2 ||
      path.avx_16way_chunk_count != expected_avx ||
      path.ref_tail_block_count != expected_ref) {
    printf("[FAIL] %s path at %zu bytes: got avx2=%zu avx=%zu ref=%zu; expected %zu/%zu/%zu\n",
           impl->name,
           len,
           path.avx2_32way_chunk_count,
           path.avx_16way_chunk_count,
           path.ref_tail_block_count,
           expected_avx2,
           expected_avx,
           expected_ref);
    return -1;
  }
  return 0;
}

static int run_dense_ref_comparison(const aria_impl_t *impl, int keybits) {
  uint8_t key[32];
  uint8_t input[4096];
  uint8_t expected[4096];
  uint8_t actual[4096];
  aria_ctx_t ref_ctx;
  aria_ctx_t impl_ctx;
  size_t len;

  if (!impl || !impl->init || !impl->encrypt) {
    return -1;
  }
  if (impl->is_supported && !impl->is_supported()) {
    printf("[SKIP] dense ref comparison (%s)\n", impl->name);
    return 0;
  }

  for (len = 0; len < sizeof(key); ++len) {
    key[len] = (uint8_t)(len * 13u + 7u);
  }
  for (len = 0; len < sizeof(input); ++len) {
    input[len] = (uint8_t)(len * 29u + (len >> 3));
  }
  memset(&ref_ctx, 0, sizeof(ref_ctx));
  memset(&impl_ctx, 0, sizeof(impl_ctx));
  aria_ref_impl.init(&ref_ctx, key, keybits);
  impl->init(&impl_ctx, key, keybits);

  for (len = ARIA_BLOCK_SIZE; len <= sizeof(input); len += ARIA_BLOCK_SIZE) {
    memset(expected, 0, len);
    memset(actual, 0, len);
    aria_ref_impl.encrypt(&ref_ctx, input, expected, len);
    impl->encrypt(&impl_ctx, input, actual, len);
    if (!bytes_eq(actual, expected, len)) {
      printf("[FAIL] dense ref comparison key=%d impl=%s len=%zu\n",
             keybits,
             impl->name,
             len);
      return -1;
    }
  }

  printf("[OK] dense ref comparison key=%d impl=%s lengths=16..4096\n",
         keybits,
         impl->name);
  return 0;
}

static void dump_hex(const char *tag, const uint8_t *x, size_t n) {
  printf("%s", tag);
  for (size_t i = 0; i < n; i++) {
    printf("%02x%s", x[i], (i + 1 == n) ? "" : " ");
  }
  printf("\n");
}

static int is_placeholder(const char *s) {
  if (!s || s[0] == '\0') {
    return 1;
  }
  return strcmp(s, "PLACEHOLDER") == 0;
}

static size_t impl_bulk_blocks(const aria_impl_t *impl) {
  if (!impl) {
    return 0;
  }
  if (strcmp(impl->name, "linux_aesni_avx") == 0) {
    return 16;
  }
  if (strcmp(impl->name, "linux_aesni_avx2") == 0) {
    return 32;
  }
  if (strcmp(impl->name, "linux_gfni_avx512") == 0) {
    return 64;
  }
  return 0;
}

static int run_ecb_kat(const aria_impl_t *impl, const char *name, const char *key_hex, const char *pt_hex, const char *ct_hex, int keybits) {
  if (!impl || !impl->init || !impl->encrypt) {
    printf("[FAIL] %s (impl null)\n", name);
    return -1;
  }
  if (impl->is_supported && !impl->is_supported()) {
    printf("[SKIP] %s\n", name);
    return 0;
  }
  
  if (is_placeholder(key_hex) || is_placeholder(pt_hex) || is_placeholder(ct_hex)) {
    printf("Fill KAT vectors in test_aria_kat.c\n");
    printf("[FAIL] %s\n", name);
    return -1;
  }

  uint8_t key[32] = {0};
  uint8_t pt[16] = {0};
  uint8_t ct_exp[16] = {0};
  uint8_t ct_got[16] = {0};

  size_t key_len = (size_t)keybits / 8;
  if (hex2bytes(key_hex, key, key_len) != 0 ||
      hex2bytes(pt_hex, pt, 16) != 0 ||
      hex2bytes(ct_hex, ct_exp, 16) != 0) {
    printf("[FAIL] %s \n", name);
    printf("  hex parse fail\n");
    return -1;
  }

  aria_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  impl->init(&ctx, key, keybits);

  impl->encrypt(&ctx, pt, ct_got, 16);

  if (!bytes_eq(ct_got, ct_exp, 16)) {
    printf("[FAIL] %s\n", name);
    dump_hex("  got: ", ct_got, 16);
    dump_hex("  exp: ", ct_exp, 16);
    return -1;
  }

  printf("[OK] %s\n", name);
  return 0;
}

static int run_ecb_kat_repeat_blocks(const aria_impl_t *impl,
                                     const char *name,
                                     const char *key_hex,
                                     const char *pt_hex,
                                     const char *ct_hex,
                                     int keybits,
                                     size_t blocks) {
  if (!impl || !impl->init || !impl->encrypt) {
    printf("[FAIL] %s (impl null)\n", name);
    return -1;
  }
  if (impl->is_supported && !impl->is_supported()) {
    printf("[SKIP] %s\n", name);
    return 0;
  }

  if (is_placeholder(key_hex) || is_placeholder(pt_hex) || is_placeholder(ct_hex)) {
    printf("Fill KAT vectors in test_aria_kat.c\n");
    printf("[FAIL] %s\n", name);
    return -1;
  }

  uint8_t key[32] = {0};
  uint8_t pt[16] = {0};
  uint8_t ct_exp[16] = {0};

  size_t key_len = (size_t)keybits / 8;
  if (hex2bytes(key_hex, key, key_len) != 0 ||
      hex2bytes(pt_hex, pt, 16) != 0 ||
      hex2bytes(ct_hex, ct_exp, 16) != 0) {
    printf("[FAIL] %s \n", name);
    printf("  hex parse fail\n");
    return -1;
  }

  uint8_t inbuf[16 * 65];
  uint8_t expbuf[16 * 65];
  uint8_t gotbuf[16 * 65];

  if (blocks == 0 || blocks > 65) {
    printf("[FAIL] %s\n", name);
    printf("  unsupported block count: %zu\n", blocks);
    return -1;
  }

  for (size_t k = 0; k < blocks; k++) {
    memcpy(inbuf + 16 * k, pt, 16);
    memcpy(expbuf + 16 * k, ct_exp, 16);
  }
  memset(gotbuf, 0, sizeof(gotbuf));

  aria_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  impl->init(&ctx, key, keybits);

  impl->encrypt(&ctx, inbuf, gotbuf, blocks * 16);

  if (!bytes_eq(gotbuf, expbuf, blocks * 16)) {
    printf("[FAIL] %s\n", name);
    for (size_t i = 0; i < blocks * 16; i++) {
      if (gotbuf[i] != expbuf[i]) {
        printf("  mismatch at byte %zu\n", i);
        break;
      }
    }
    dump_hex("  got0: ", gotbuf + 0, 16);
    dump_hex("  exp0: ", expbuf + 0, 16);
    if (blocks > 1) {
      dump_hex("  got1: ", gotbuf + 16, 16);
      dump_hex("  exp1: ", expbuf + 16, 16);
    }
    return -1;
  }

  printf("[OK] %s\n", name);
  return 0;
}

static int run_impl_shape_kats(const aria_impl_t *impl,
                               const char *key_hex,
                               const char *pt_hex,
                               const char *ct_hex,
                               int keybits) {
  size_t bulk_blocks = impl_bulk_blocks(impl);
  int rc = 0;
  char name[96];

  snprintf(name, sizeof(name), "ECB-%d bulk-tail (%s)", keybits, impl->name);
  if (strcmp(impl->name, "linux_gfni_avx512") == 0) {
    static const size_t gfni_shapes[] = {16, 17, 32, 33, 64, 65};
    for (size_t i = 0; i < sizeof(gfni_shapes) / sizeof(gfni_shapes[0]); ++i) {
      snprintf(name,
               sizeof(name),
               "ECB-%d %zublk (%s)",
               keybits,
               gfni_shapes[i],
               impl->name);
      rc |= run_ecb_kat_repeat_blocks(impl,
                                      name,
                                      key_hex,
                                      pt_hex,
                                      ct_hex,
                                      keybits,
                                      gfni_shapes[i]);
    }
    return rc;
  }
  if (bulk_blocks == 0) {
    rc |= run_ecb_kat_repeat_blocks(impl, name, key_hex, pt_hex, ct_hex, keybits, 5);
    return rc;
  }

  snprintf(name, sizeof(name), "ECB-%d bulk (%s)", keybits, impl->name);
  rc |= run_ecb_kat_repeat_blocks(impl, name, key_hex, pt_hex, ct_hex, keybits, bulk_blocks);

  snprintf(name, sizeof(name), "ECB-%d bulk+tail (%s)", keybits, impl->name);
  rc |= run_ecb_kat_repeat_blocks(impl, name, key_hex, pt_hex, ct_hex, keybits, bulk_blocks + 1);
  return rc;
}


static int test_ecb_128_one(const aria_impl_t *impl) {
  const char *key_hex = "00112233445566778899aabbccddeeff";
  const char *pt_hex = "11111111aaaaaaaa11111111bbbbbbbb";
  const char *ct_hex = "c6ecd08e22c30abdb215cf74e2075e6e";

  int rc = 0;

  char name1[64];
  snprintf(name1, sizeof(name1), "ECB-128 (%s)", impl->name);
  rc |= run_ecb_kat(impl, name1, key_hex, pt_hex, ct_hex, 128);

  char name4[64];
  snprintf(name4, sizeof(name4), "ECB-128 4blk (%s)", impl->name);
  rc |= run_ecb_kat_repeat_blocks(impl, name4, key_hex, pt_hex, ct_hex, 128, 4);
  rc |= run_impl_shape_kats(impl, key_hex, pt_hex, ct_hex, 128);

  return rc;
}

static int test_ecb_192_one(const aria_impl_t *impl) {
  const char *key_hex = "00112233445566778899aabbccddeeff0011223344556677";
  const char *pt_hex = "11111111aaaaaaaa11111111bbbbbbbb";
  const char *ct_hex = "8d1470625f59ebacb0e55b534b3e462b";

  int rc = 0;
  
  char name1[64];
  snprintf(name1, sizeof(name1), "ECB-192 (%s)", impl->name);
  rc |= run_ecb_kat(impl, name1, key_hex, pt_hex, ct_hex, 192);

  char name4[64];
  snprintf(name4, sizeof(name4), "ECB-192 4blk (%s)", impl->name);
  rc |= run_ecb_kat_repeat_blocks(impl, name4, key_hex, pt_hex, ct_hex, 192, 4);
  rc |= run_impl_shape_kats(impl, key_hex, pt_hex, ct_hex, 192);

  return rc;
}

static int test_ecb_256_one(const aria_impl_t *impl) {
  const char *key_hex = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  const char *pt_hex = "11111111aaaaaaaa11111111bbbbbbbb";
  const char *ct_hex = "58a875e6044ad7fffa4f58420f7f442d";

  int rc = 0;
  
  char name1[64];
  snprintf(name1, sizeof(name1), "ECB-256 (%s)", impl->name);
  rc |= run_ecb_kat(impl, name1, key_hex, pt_hex, ct_hex, 256);

  char name4[64];
  snprintf(name4, sizeof(name4), "ECB-256 4blk (%s)", impl->name);
  rc |= run_ecb_kat_repeat_blocks(impl, name4, key_hex, pt_hex, ct_hex, 256, 4);
  rc |= run_impl_shape_kats(impl, key_hex, pt_hex, ct_hex, 256);

  return rc;
}

int main(void) {
  int rc = 0;

  const aria_impl_t *impls[] = {
    &aria_ref_impl,
    &aria_linux_aesni_avx_impl,
    &aria_linux_aesni_avx2_impl,
    &aria_linux_gfni_avx512_impl
  };
  for (size_t i = 0; i < sizeof(impls) / sizeof(impls[0]); i++) {
    rc |= test_ecb_128_one(impls[i]);
    rc |= test_ecb_192_one(impls[i]);
    rc |= test_ecb_256_one(impls[i]);
    rc |= run_dense_ref_comparison(impls[i], 128);
    rc |= run_dense_ref_comparison(impls[i], 192);
    rc |= run_dense_ref_comparison(impls[i], 256);
  }

  rc |= expect_path(&aria_linux_aesni_avx_impl, 240, 0, 0, 15);
  rc |= expect_path(&aria_linux_aesni_avx_impl, 256, 0, 1, 0);
  rc |= expect_path(&aria_linux_aesni_avx_impl, 272, 0, 1, 1);

  rc |= expect_path(&aria_linux_aesni_avx2_impl, 240, 0, 0, 15);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 256, 0, 1, 0);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 272, 0, 1, 1);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 496, 0, 1, 15);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 512, 1, 0, 0);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 528, 1, 0, 1);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 752, 1, 0, 15);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 768, 1, 1, 0);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 784, 1, 1, 1);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 1008, 1, 1, 15);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 1024, 2, 0, 0);
  rc |= expect_path(&aria_linux_aesni_avx2_impl, 1040, 2, 0, 1);

  return rc ? 1 : 0;
}
