#include "gateway_utils.h"
#include "mongoose.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int gw_hex_to_bytes(const char *hex, uint8_t *dst, size_t hex_len) {
  if (hex_len % 2 != 0) return -1;
  size_t byte_len = hex_len / 2;
  for (size_t i = 0; i < byte_len; i++) {
    unsigned int b;
    if (sscanf(hex + i * 2, "%2x", &b) != 1) return -1;
    dst[i] = (uint8_t)b;
  }
  return (int)byte_len;
}

int gw_psk_to_key(const char *psk, size_t psk_len, uint8_t *key) {
  mg_sha256(key, (uint8_t*)psk, psk_len);
  return 0;
}