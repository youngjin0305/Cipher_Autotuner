#ifndef ARIA_LINUX_ASM_COMPAT_H
#define ARIA_LINUX_ASM_COMPAT_H

#define SYM_FUNC_START_LOCAL(name) \
  .type name, @function; \
  name:

#define SYM_TYPED_FUNC_START(name) \
  .globl name; \
  .type name, @function; \
  name:

#define SYM_FUNC_END(name) \
  .size name, .-name

#define FRAME_BEGIN
#define FRAME_END
#define RET ret

#define ARIA_CTX_rounds 0
#define ARIA_CTX_enc_key 4
#define ARIA_CTX_dec_key (4 + (17 * 16))

#endif
