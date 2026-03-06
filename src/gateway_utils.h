#ifndef __GATEWAY_UTILS__H_
#define __GATEWAY_UTILS__H_

#include <stdint.h>
#include <stddef.h>

int  gw_hex_to_bytes(const char *hex, uint8_t *dst, size_t hex_len);
int  gw_psk_to_key(const char *psk, size_t psk_len, uint8_t *key);   // new

#endif