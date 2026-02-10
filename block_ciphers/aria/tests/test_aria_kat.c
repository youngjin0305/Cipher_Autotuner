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

static int run_ecb_kat(const aria_impl_t *impl, const char *name, const char *key_hex, const char *pt_hex, const char *ct_hex, int keybits) {
  if (!impl || !impl->encrypt) {
    printf("[FAIL] %s (impl null)\n", name);
    return -1;
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
  aria_init(&ctx, key, keybits);

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

static int test_ecb_128_one(const aria_impl_t *impl) {
  const char *key_hex = "00112233445566778899aabbccddeeff";
  const char *pt_hex = "11111111aaaaaaaa11111111bbbbbbbb";
  const char *ct_hex = "c6ecd08e22c30abdb215cf74e2075e6e";

  char name[64];
  snprintf(name, sizeof(name), "ECB-128 (%s)", impl->name);
  return run_ecb_kat(impl, name, key_hex, pt_hex, ct_hex, 128);
}

static int test_ecb_192_one(const aria_impl_t *impl) {
  const char *key_hex = "00112233445566778899aabbccddeeff0011223344556677";
  const char *pt_hex = "11111111aaaaaaaa11111111bbbbbbbb";
  const char *ct_hex = "8d1470625f59ebacb0e55b534b3e462b";
  
  char name[64];
  snprintf(name, sizeof(name), "ECB-192 (%s)", impl->name);
  return run_ecb_kat(impl, name, key_hex, pt_hex, ct_hex, 192);
}

static int test_ecb_256_one(const aria_impl_t *impl) {
  const char *key_hex = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  const char *pt_hex = "11111111aaaaaaaa11111111bbbbbbbb";
  const char *ct_hex = "58a875e6044ad7fffa4f58420f7f442d";
  
  char name[64];
  snprintf(name, sizeof(name), "ECB-256 (%s)", impl->name);
  return run_ecb_kat(impl, name, key_hex, pt_hex, ct_hex, 256);
}

int main(void) {
  int rc = 0;

  const aria_impl_t *impls[] = { &aria_ref_impl, &aria_avx2_impl };
  for (size_t i = 0; i < sizeof(impls) / sizeof(impls[0]); i++) {
    rc |= test_ecb_128_one(impls[i]);
    rc |= test_ecb_192_one(impls[i]);
    rc |= test_ecb_256_one(impls[i]);
  }

  return rc ? 1 : 0;
}
