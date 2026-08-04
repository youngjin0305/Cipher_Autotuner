#include "aria_api.h"
#include "autotune.h"
#include "runtime_dispatch.h"

#include <stdio.h>
#include <string.h>

static int expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "[FAIL] %s\n", message);
    return 0;
  }
  return 1;
}

int main(void) {
  aria_policy_range_t valid_policy[] = {
    {128, 16, 32, &aria_ref_impl},
    {128, 48, 64, &aria_ref_impl},
    {192, 16, 32, &aria_ref_impl},
    {192, 48, 64, &aria_ref_impl},
    {256, 16, 32, &aria_ref_impl},
    {256, 48, 64, &aria_ref_impl}
  };
  aria_policy_range_t invalid_gap[] = {
    {128, 16, 32, &aria_ref_impl},
    {128, 64, 80, &aria_ref_impl}
  };
  aria_policy_range_t invalid_alignment[] = {
    {128, 16, 31, &aria_ref_impl}
  };
  aria_autotune_config_t invalid_config;
  uint8_t input[16] = {0};
  uint8_t output[16] = {0};
  const int gfni_supported = aria_linux_gfni_avx512_impl.is_supported &&
                             aria_linux_gfni_avx512_impl.is_supported();

  if (!expect(strcmp(aria_linux_gfni_avx512_impl.effective_path(256),
                     "linux_gfni_avx_16way") == 0,
              "256-byte GFNI candidate must report its 16-way path") ||
      !expect(strcmp(aria_linux_gfni_avx512_impl.effective_path(512),
                     "linux_gfni_avx2_32way") == 0,
              "512-byte GFNI candidate must report its 32-way path") ||
      !expect(strcmp(aria_linux_gfni_avx512_impl.effective_path(1024),
                     "linux_gfni_avx512") == 0,
              "1024-byte GFNI candidate must report its native path") ||
      !expect(strcmp(aria_linux_gfni_avx512_impl.effective_path(528),
                     "linux_gfni_plus_ref_tail") == 0,
              "GFNI candidate must report a reference tail") ||
      !expect(strcmp(aria_linux_aesni_avx2_impl.effective_path(240),
                     "ref_fallback") == 0,
              "AVX2 candidate below 16 blocks must report Ref fallback") ||
      !expect(strcmp(aria_linux_aesni_avx2_impl.effective_path(256),
                     "linux_aesni_avx") == 0,
              "AVX2 candidate at 16 blocks must report AVX execution") ||
      !expect(strcmp(aria_linux_aesni_avx2_impl.effective_path(272),
                     "linux_aesni_avx_plus_ref_tail") == 0,
              "AVX2 candidate at 17 blocks must report AVX plus Ref") ||
      !expect(strcmp(aria_linux_aesni_avx2_impl.effective_path(512),
                     "linux_aesni_avx2") == 0,
              "AVX2 candidate at 32 blocks must report AVX2 execution") ||
      !expect(strcmp(aria_linux_aesni_avx2_impl.effective_path(768),
                     "linux_aesni_avx2_plus_avx_tail") == 0,
              "AVX2 candidate at 48 blocks must report AVX2 plus AVX") ||
      !expect(strcmp(aria_linux_aesni_avx2_impl.effective_path(784),
                     "linux_aesni_avx2_plus_avx_ref_tail") == 0,
              "AVX2 candidate at 49 blocks must report AVX2 plus AVX plus Ref") ||
      !expect((aria_runtime_dispatch(1024) == &aria_linux_gfni_avx512_impl) ==
                  gfni_supported,
              "static dispatch must select GFNI only when supported") ||
      !expect(aria_policy_install(valid_policy,
                                  sizeof(valid_policy) / sizeof(valid_policy[0])),
              "valid policy must install") ||
      !expect(aria_policy_count() == 6, "installed policy count") ||
      !expect(aria_autotuned_dispatch(128, 16) == &aria_ref_impl, "lower boundary") ||
      !expect(aria_autotuned_dispatch(128, 32) == &aria_ref_impl, "upper boundary") ||
      !expect(aria_autotuned_dispatch(128, 48) == &aria_ref_impl, "next lower boundary") ||
      !expect(aria_autotuned_dispatch(256, 64) == &aria_ref_impl, "last boundary") ||
      !expect(!aria_policy_install(invalid_gap,
                                  sizeof(invalid_gap) / sizeof(invalid_gap[0])),
              "policy with uncovered block length must be rejected") ||
      !expect(!aria_policy_install(invalid_alignment,
                                  sizeof(invalid_alignment) / sizeof(invalid_alignment[0])),
              "unaligned policy must be rejected") ||
      !expect(aria_policy_count() == 6, "failed install must preserve active policy") ||
      !expect(aria_autotuned_dispatch(128, 96) != NULL, "out-of-range fallback")) {
    return 1;
  }

  aria_policy_clear();
  aria_autotune_config_init(&invalid_config);
  invalid_config.min_len = 16;
  invalid_config.max_len = 16;
  invalid_config.refine_step = 0;
  if (!expect(aria_policy_count() == 0, "policy clear") ||
      !expect(aria_autotuned_dispatch(128, 16) != NULL, "empty-policy fallback") ||
      !expect(!aria_autotune_run(&invalid_config,
                                input,
                                output,
                                sizeof(input),
                                0,
                                1,
                                0,
                                STAT_MEDIAN),
              "zero refine step must be rejected before scanning")) {
    return 1;
  }

  printf("[OK] policy dispatch boundaries and fallback\n");
  return 0;
}
