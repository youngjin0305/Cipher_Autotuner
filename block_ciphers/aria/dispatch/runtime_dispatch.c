#include "runtime_dispatch.h"

const aria_impl_t *aria_runtime_dispatch(size_t len) {
  (void)len;
  return &aria_ref_impl;
}
