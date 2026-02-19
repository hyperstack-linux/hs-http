#include "../include/http2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char *hpack_encode_response(int status_code,
                                      const char *content_type,
                                      size_t content_length,
                                      size_t *out_size) {
  uint8_t status_idx = 0x88;
  if (status_code == 404)
    status_idx = 0x8d;
  else if (status_code == 500)
    status_idx = 0x8e;

  size_t ct_len = strlen(content_type);
  char cl_str[32];
  int cl_len = snprintf(cl_str, sizeof(cl_str), "%zu", content_length);

  size_t total = 1 + 2 + ct_len + 2 + cl_len;
  unsigned char *buf = malloc(total);
  if (!buf)
    return NULL;

  size_t pos = 0;
  buf[pos++] = status_idx;

  buf[pos++] = 0x5f;
  buf[pos++] = (uint8_t)ct_len;
  memcpy(buf + pos, content_type, ct_len);
  pos += ct_len;

  buf[pos++] = 0x5c;
  buf[pos++] = (uint8_t)cl_len;
  memcpy(buf + pos, cl_str, cl_len);
  pos += cl_len;

  *out_size = pos;
  return buf;
}
