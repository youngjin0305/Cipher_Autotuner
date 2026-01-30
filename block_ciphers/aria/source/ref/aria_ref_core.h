#ifndef KISA_ARIA_REF_CORE_H
#define KISA_ARIA_REF_CORE_H

typedef unsigned char Byte;

int EncKeySetup(const Byte *masterKey, Byte *roundKeys, int keyBits);
int DecKeySetup(const Byte *masterKey, Byte *roundKeys, int keyBits);
void Crypt(const Byte *plainText, int numberOfRounds, const Byte *roundKeys, Byte *cipherText);

#endif /* KISA_ARIA_REF_CORE_H */