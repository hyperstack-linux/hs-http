#include "../include/http2.h"
#include "hpack_static.c"
#include <stdio.h>
#include <string.h>

static int huffman_decode(const uint8_t *src, size_t src_len, char *dst,
                          size_t dst_size) {
  if (dst_size == 0)
    return -1;

  uint64_t acc = 0;
  int acc_bits = 0;
  size_t dst_pos = 0;

  for (size_t i = 0; i < src_len; i++) {
    acc = (acc << 8) | src[i];
    acc_bits += 8;

    while (acc_bits >= 5) {
      int found = 0;
      for (int bl = 5; bl <= 30 && bl <= acc_bits; bl++) {
        uint32_t cand =
            (uint32_t)((acc >> (acc_bits - bl)) & ((1ULL << bl) - 1));
        for (int sym = 0; sym < 256; sym++) {
          if (huff_sym[sym].bits == bl && huff_sym[sym].code == cand) {
            if (dst_pos < dst_size - 1)
              dst[dst_pos++] = (char)sym;
            acc_bits -= bl;
            found = 1;
            break;
          }
        }
        if (found)
          break;
      }
      if (!found)
        break;
    }
  }

  if (dst_pos < dst_size)
    dst[dst_pos] = '\0';
  return (int)dst_pos;
}

static int decode_int(const uint8_t **p, size_t *remaining, int prefix_bits,
                      int *value) {
  if (*remaining == 0)
    return -1;
  uint8_t first = **p;
  uint32_t result = first & ((1U << prefix_bits) - 1U);
  (*p)++;
  (*remaining)--;

  if (result < (1U << prefix_bits) - 1U) {
    *value = (int)result;
    return 0;
  }

  uint32_t m = 0;
  while (*remaining > 0) {
    uint8_t b = **p;
    (*p)++;
    (*remaining)--;
    result += (b & 0x7FU) << m;
    if ((b & 0x80U) == 0) {
      if (result > 0x7FFFFFFF)
        return -1;
      *value = (int)result;
      return 0;
    }
    m += 7;
    if (m > 28)
      return -1;
  }
  return -1;
}

int hpack_decode_method(const uint8_t *data, size_t len,
                        char *out_method, size_t out_method_size) {
  if (!out_method || out_method_size == 0)
    return -1;
  out_method[0] = '\0';

  const uint8_t *p = data;
  size_t remaining = len;

  while (remaining > 0) {
    uint8_t byte = *p;

    if (byte & 0x80) {
      int index;
      if (decode_int(&p, &remaining, 7, &index) < 0)
        break;

      if (index > 0 && index < HPACK_STATIC_TABLE_SIZE) {
        const char *name = hpack_static_table[index][0];
        const char *value = hpack_static_table[index][1];

        if (name && value && strcmp(name, ":method") == 0) {
          size_t copy_len = strlen(value);
          if (copy_len >= out_method_size)
            copy_len = out_method_size - 1;
          memcpy(out_method, value, copy_len);
          out_method[copy_len] = '\0';
          return 0;
        }
      }
    } else if ((byte & 0xC0) == 0x40) {
      int name_index;
      if (decode_int(&p, &remaining, 6, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      if (name_index == 0) {
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) < 0)
            break;
          p += name_len;
          remaining -= name_len;
        } else {
          if (remaining < (size_t)name_len)
            break;
          memcpy(name_buf, p, name_len);
          name_buf[name_len] = '\0';
          p += name_len;
          remaining -= name_len;
          name = name_buf;
        }
      } else if (name_index < HPACK_STATIC_TABLE_SIZE) {
        name = hpack_static_table[name_index][0];
      }

      if (remaining == 0)
        break;

      int h_bit = (*p & 0x80);
      int val_len;
      if (decode_int(&p, &remaining, 7, &val_len) < 0)
        break;
      if (val_len < 0 || remaining < (size_t)val_len)
        break;

      if (name && strcmp(name, ":method") == 0) {
        if (h_bit) {
          huffman_decode(p, (size_t)val_len, out_method, out_method_size);
        } else {
          size_t copy_len = (size_t)val_len < out_method_size - 1
                                ? (size_t)val_len
                                : out_method_size - 1;
          memcpy(out_method, p, copy_len);
          out_method[copy_len] = '\0';
        }
        return 0;
      }
      p += val_len;
      remaining -= val_len;

    } else if ((byte & 0xF0) == 0x00) {
      int name_index;
      if (decode_int(&p, &remaining, 4, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      if (name_index == 0) {
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) < 0)
            break;
          p += name_len;
          remaining -= name_len;
        } else {
          if (remaining < (size_t)name_len)
            break;
          memcpy(name_buf, p, name_len);
          name_buf[name_len] = '\0';
          p += name_len;
          remaining -= name_len;
          name = name_buf;
        }
      } else if (name_index < HPACK_STATIC_TABLE_SIZE) {
        name = hpack_static_table[name_index][0];
      }

      if (remaining == 0)
        break;

      int h_bit = (*p & 0x80);
      int val_len;
      if (decode_int(&p, &remaining, 7, &val_len) < 0)
        break;
      if (val_len < 0 || remaining < (size_t)val_len)
        break;

      if (name && strcmp(name, ":method") == 0) {
        if (h_bit) {
          huffman_decode(p, (size_t)val_len, out_method, out_method_size);
        } else {
          size_t copy_len = (size_t)val_len < out_method_size - 1
                                ? (size_t)val_len
                                : out_method_size - 1;
          memcpy(out_method, p, copy_len);
          out_method[copy_len] = '\0';
        }
        return 0;
      }
      p += val_len;
      remaining -= val_len;
    } else {
      break;
    }
  }

  return -1;
}

int hpack_decode_path(const uint8_t *data, size_t len, char *out_path,
                      size_t out_path_size) {
  if (!out_path || out_path_size == 0)
    return -1;
  out_path[0] = '\0';

  const uint8_t *p = data;
  size_t remaining = len;

  while (remaining > 0) {
    uint8_t byte = *p;

    if (byte & 0x80) {
      int index;
      if (decode_int(&p, &remaining, 7, &index) < 0)
        break;

      if (index > 0 && index < HPACK_STATIC_TABLE_SIZE) {
        const char *name = hpack_static_table[index][0];
        const char *value = hpack_static_table[index][1];

        if (name && value && strcmp(name, ":path") == 0) {
          size_t copy_len = strlen(value);
          if (copy_len >= out_path_size)
            copy_len = out_path_size - 1;
          memcpy(out_path, value, copy_len);
          out_path[copy_len] = '\0';
          return 0;
        }
      }
    } else if ((byte & 0xC0) == 0x40) {
      int name_index;
      if (decode_int(&p, &remaining, 6, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      if (name_index == 0) {
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) < 0)
            break;
          p += name_len;
          remaining -= name_len;
        } else {
          if (remaining < (size_t)name_len)
            break;
          memcpy(name_buf, p, name_len);
          name_buf[name_len] = '\0';
          p += name_len;
          remaining -= name_len;
          name = name_buf;
        }
      } else if (name_index < HPACK_STATIC_TABLE_SIZE) {
        name = hpack_static_table[name_index][0];
      }

      if (remaining == 0)
        break;

      int h_bit = (*p & 0x80);
      int val_len;
      if (decode_int(&p, &remaining, 7, &val_len) < 0)
        break;
      if (val_len < 0 || remaining < (size_t)val_len)
        break;

      if (name && strcmp(name, ":path") == 0) {
        if (h_bit) {
          huffman_decode(p, (size_t)val_len, out_path, out_path_size);
        } else {
          size_t copy_len = (size_t)val_len < out_path_size - 1
                                ? (size_t)val_len
                                : out_path_size - 1;
          memcpy(out_path, p, copy_len);
          out_path[copy_len] = '\0';
        }
        return 0;
      }
      p += val_len;
      remaining -= val_len;

    } else if ((byte & 0xF0) == 0x00) {
      int name_index;
      if (decode_int(&p, &remaining, 4, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      if (name_index == 0) {
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) < 0)
            break;
          p += name_len;
          remaining -= name_len;
        } else {
          if (remaining < (size_t)name_len)
            break;
          memcpy(name_buf, p, name_len);
          name_buf[name_len] = '\0';
          p += name_len;
          remaining -= name_len;
          name = name_buf;
        }
      } else if (name_index < HPACK_STATIC_TABLE_SIZE) {
        name = hpack_static_table[name_index][0];
      }

      if (remaining == 0)
        break;

      int h_bit = (*p & 0x80);
      int val_len;
      if (decode_int(&p, &remaining, 7, &val_len) < 0)
        break;
      if (val_len < 0 || remaining < (size_t)val_len)
        break;

      if (name && strcmp(name, ":path") == 0) {
        if (h_bit) {
          huffman_decode(p, (size_t)val_len, out_path, out_path_size);
        } else {
          size_t copy_len = (size_t)val_len < out_path_size - 1
                                ? (size_t)val_len
                                : out_path_size - 1;
          memcpy(out_path, p, copy_len);
          out_path[copy_len] = '\0';
        }
        return 0;
      }
      p += val_len;
      remaining -= val_len;
    } else {
      break;
    }
  }

  return -1;
}
