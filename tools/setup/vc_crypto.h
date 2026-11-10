/* What Virtual Console packages need (vc.c): AES-128-CBC decryption and SHA-1. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Decrypts len bytes (a multiple of 16) in place; iv is updated to continue from them. */
void aes128_cbc_decrypt(const uint8_t key[16], uint8_t iv[16], uint8_t *buf, size_t len);
void sha1(const void *data, size_t len, uint8_t out[20]);
