#include "include/http2.h"
#include "include/server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Built-in error pages (defined in handlers.c via xxd -i)
extern unsigned char ___src_errors_404_html[];
extern unsigned int ___src_errors_404_html_len;
extern unsigned char ___src_errors_500_html[];
extern unsigned int ___src_errors_500_html_len;

// Forward declarations
int mod_epoll(int fd, uint32_t events);

// Helper to read big-endian uint32
static uint32_t read_uint32(const uint8_t *buf) {
  return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
         ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

// HPACK static table - full RFC 7541 Appendix A (61 entries)
static const char *hpack_static_table[][2] = {
    {NULL, NULL},                         // 0 - unused
    {":authority", ""},                   // 1
    {":method", "GET"},                   // 2
    {":method", "POST"},                  // 3
    {":path", "/"},                       // 4
    {":path", "/index.html"},             // 5
    {":scheme", "http"},                  // 6
    {":scheme", "https"},                 // 7
    {":status", "200"},                   // 8
    {":status", "204"},                   // 9
    {":status", "206"},                   // 10
    {":status", "304"},                   // 11
    {":status", "400"},                   // 12
    {":status", "404"},                   // 13
    {":status", "500"},                   // 14
    {"accept-charset", ""},               // 15
    {"accept-encoding", "gzip, deflate"}, // 16
    {"accept-language", ""},              // 17
    {"accept-ranges", ""},                // 18
    {"accept", ""},                       // 19
    {"access-control-allow-origin", ""},  // 20
    {"age", ""},                          // 21
    {"allow", ""},                        // 22
    {"authorization", ""},                // 23
    {"cache-control", ""},                // 24
    {"content-disposition", ""},          // 25
    {"content-encoding", ""},             // 26
    {"content-language", ""},             // 27
    {"content-length", ""},               // 28
    {"content-location", ""},             // 29
    {"content-range", ""},                // 30
    {"content-type", ""},                 // 31
    {"cookie", ""},                       // 32
    {"date", ""},                         // 33
    {"etag", ""},                         // 34
    {"expect", ""},                       // 35
    {"expires", ""},                      // 36
    {"from", ""},                         // 37
    {"host", ""},                         // 38
    {"if-match", ""},                     // 39
    {"if-modified-since", ""},            // 40
    {"if-none-match", ""},                // 41
    {"if-range", ""},                     // 42
    {"if-unmodified-since", ""},          // 43
    {"last-modified", ""},                // 44
    {"link", ""},                         // 45
    {"location", ""},                     // 46
    {"max-forwards", ""},                 // 47
    {"proxy-authenticate", ""},           // 48
    {"proxy-authorization", ""},          // 49
    {"range", ""},                        // 50
    {"referer", ""},                      // 51
    {"refresh", ""},                      // 52
    {"retry-after", ""},                  // 53
    {"server", ""},                       // 54
    {"set-cookie", ""},                   // 55
    {"strict-transport-security", ""},    // 56
    {"transfer-encoding", ""},            // 57
    {"user-agent", ""},                   // 58
    {"vary", ""},                         // 59
    {"via", ""},                          // 60
    {"www-authenticate", ""},             // 61
};
#define HPACK_STATIC_TABLE_SIZE 62

// Huffman decoding for HPACK - full RFC 7541 Appendix B code table
// {code, bit_length} for symbols 0..255 + EOS(256)
static const struct {
  uint32_t code;
  uint8_t bits;
} huff_sym[257] = {
    {0x1ff8, 13},    {0x7fffd8, 23},   {0xfffffe2, 28},  {0xfffffe3, 28},
    {0xfffffe4, 28}, {0xfffffe5, 28},  {0xfffffe6, 28},  {0xfffffe7, 28},
    {0xfffffe8, 28}, {0xffffea, 24},   {0x3ffffffc, 30}, {0xfffffe9, 28},
    {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28},  {0xfffffec, 28},
    {0xfffffed, 28}, {0xfffffee, 28},  {0xfffffef, 28},  {0xffffff0, 28},
    {0xffffff1, 28}, {0xffffff2, 28},  {0x3ffffffe, 30}, {0xffffff3, 28},
    {0xffffff4, 28}, {0xffffff5, 28},  {0xffffff6, 28},  {0xffffff7, 28},
    {0xffffff8, 28}, {0xffffff9, 28},  {0xffffffa, 28},  {0xffffffb, 28},
    {0x14, 6},     // 32 ' '
    {0x3f8, 10},   // 33 '!'
    {0x3f9, 10},   // 34 '"'
    {0xffa, 12},   // 35 '#'
    {0x1ff9, 13},  // 36 '$'
    {0x15, 6},     // 37 '%'
    {0xf8, 8},     // 38 '&'
    {0x7fa, 11},   // 39 '\''
    {0x3fa, 10},   // 40 '('
    {0x3fb, 10},   // 41 ')'
    {0xf9, 8},     // 42 '*'
    {0x7fb, 11},   // 43 '+'
    {0xfa, 8},     // 44 ','
    {0x16, 6},     // 45 '-'
    {0x17, 6},     // 46 '.'
    {0x18, 6},     // 47 '/'
    {0x0, 5},      // 48 '0'
    {0x1, 5},      // 49 '1'
    {0x2, 5},      // 50 '2'
    {0x19, 6},     // 51 '3'
    {0x1a, 6},     // 52 '4'
    {0x1b, 6},     // 53 '5'
    {0x1c, 6},     // 54 '6'
    {0x1d, 6},     // 55 '7'
    {0x1e, 6},     // 56 '8'
    {0x1f, 6},     // 57 '9'
    {0x5c, 7},     // 58 ':'
    {0xfb, 8},     // 59 ';'
    {0x7ffc, 15},  // 60 '<'
    {0x20, 6},     // 61 '='
    {0xffb, 12},   // 62 '>'
    {0x3fc, 10},   // 63 '?'
    {0x1ffa, 13},  // 64 '@'
    {0x21, 6},     // 65 'A'
    {0x5d, 7},     // 66 'B'
    {0x5e, 7},     // 67 'C'
    {0x5f, 7},     // 68 'D'
    {0x60, 7},     // 69 'E'
    {0x61, 7},     // 70 'F'
    {0x62, 7},     // 71 'G'
    {0x63, 7},     // 72 'H'
    {0x64, 7},     // 73 'I'
    {0x65, 7},     // 74 'J'
    {0x66, 7},     // 75 'K'
    {0x67, 7},     // 76 'L'
    {0x68, 7},     // 77 'M'
    {0x69, 7},     // 78 'N'
    {0x6a, 7},     // 79 'O'
    {0x6b, 7},     // 80 'P'
    {0x6c, 7},     // 81 'Q'
    {0x6d, 7},     // 82 'R'
    {0x6e, 7},     // 83 'S'
    {0x6f, 7},     // 84 'T'
    {0x70, 7},     // 85 'U'
    {0x71, 7},     // 86 'V'
    {0x72, 7},     // 87 'W'
    {0xfc, 8},     // 88 'X'
    {0x73, 7},     // 89 'Y'
    {0xfd, 8},     // 90 'Z'
    {0x1ffb, 13},  // 91 '['
    {0x7fff0, 19}, // 92 '\\'
    {0x1ffc, 13},  // 93 ']'
    {0x3ffc, 14},  // 94 '^'
    {0x22, 6},     // 95 '_'
    {0x7ffd, 15},  // 96 '`'
    {0x3, 5},      // 97 'a'
    {0x23, 6},     // 98 'b'
    {0x4, 5},      // 99 'c'
    {0x24, 6},     // 100 'd'
    {0x5, 5},      // 101 'e'
    {0x25, 6},     // 102 'f'
    {0x26, 6},     // 103 'g'
    {0x27, 6},     // 104 'h'
    {0x6, 5},      // 105 'i'
    {0x74, 7},     // 106 'j'
    {0x75, 7},     // 107 'k'
    {0x28, 6},     // 108 'l'
    {0x29, 6},     // 109 'm'
    {0x2a, 6},     // 110 'n'
    {0x7, 5},      // 111 'o'
    {0x2b, 6},     // 112 'p'
    {0x76, 7},     // 113 'q'
    {0x2c, 6},     // 114 'r'
    {0x8, 5},      // 115 's'
    {0x9, 5},      // 116 't'
    {0x2d, 6},     // 117 'u'
    {0x77, 7},     // 118 'v'
    {0x78, 7},     // 119 'w'
    {0x79, 7},     // 120 'x'
    {0x7a, 7},     // 121 'y'
    {0x7b, 7},     // 122 'z'
    {0x7ffe, 15},  // 123 '{'
    {0x7fc, 11},   // 124 '|'
    {0x3ffd, 14},  // 125 '}'
    {0x1ffd, 13},  // 126 '~'
    {0xffffffc, 28}, {0xfffe6, 20},    {0x3fffd2, 22},   {0xfffe7, 20},
    {0xfffe8, 20},   {0x3fffd3, 22},   {0x3fffd4, 22},   {0x3fffd5, 22},
    {0x7fffd9, 23},  {0x3fffd6, 22},   {0x7fffda, 23},   {0x7fffdb, 23},
    {0x7fffdc, 23},  {0x7fffdd, 23},   {0x7fffde, 23},   {0xffffeb, 24},
    {0x7fffdf, 23},  {0xffffec, 24},   {0xffffed, 24},   {0x3fffd7, 22},
    {0x7fffe0, 23},  {0xffffee, 24},   {0x7fffe1, 23},   {0x7fffe2, 23},
    {0x7fffe3, 23},  {0x7fffe4, 23},   {0x1fffdc, 21},   {0x3fffd8, 22},
    {0x7fffe5, 23},  {0x3fffd9, 22},   {0x7fffe6, 23},   {0x7fffe7, 23},
    {0xffffef, 24},  {0x3fffda, 22},   {0x1fffdd, 21},   {0xfffe9, 20},
    {0x3fffdb, 22},  {0x3fffdc, 22},   {0x7fffe8, 23},   {0x7fffe9, 23},
    {0x1fffde, 21},  {0x7fffea, 23},   {0x3fffdd, 22},   {0x3fffde, 22},
    {0xfffff0, 24},  {0x1fffdf, 21},   {0x3fffdf, 22},   {0x7fffeb, 23},
    {0x7fffec, 23},  {0x1fffe0, 21},   {0x1fffe1, 21},   {0x3fffe0, 22},
    {0x1fffe2, 21},  {0x7fffed, 23},   {0x3fffe1, 22},   {0x7fffee, 23},
    {0x7fffef, 23},  {0xfffea, 20},    {0x3fffe2, 22},   {0x3fffe3, 22},
    {0x3fffe4, 22},  {0x7ffff0, 23},   {0x3fffe5, 22},   {0x3fffe6, 22},
    {0x7ffff1, 23},  {0x3ffffe0, 26},  {0x3ffffe1, 26},  {0xfffeb, 20},
    {0x7fff1, 19},   {0x3fffe7, 22},   {0x7ffff2, 23},   {0x3fffe8, 22},
    {0x1ffffec, 25}, {0x3ffffe2, 26},  {0x3ffffe3, 26},  {0x3ffffe4, 26},
    {0x7ffffde, 27}, {0x7ffffdf, 27},  {0x3ffffe5, 26},  {0xfffff1, 24},
    {0x1ffffed, 25}, {0x7fff2, 19},    {0x1fffe3, 21},   {0x3ffffe6, 26},
    {0x7ffffe0, 27}, {0x7ffffe1, 27},  {0x3ffffe7, 26},  {0x7ffffe2, 27},
    {0xfffff2, 24},  {0x1fffe4, 21},   {0x1fffe5, 21},   {0x3ffffe8, 26},
    {0x3ffffe9, 26}, {0xffffffd, 28},  {0x7ffffe3, 27},  {0x7ffffe4, 27},
    {0x7ffffe5, 27}, {0xfffec, 20},    {0xfffff3, 24},   {0xfffed, 20},
    {0x1fffe6, 21},  {0x3fffe9, 22},   {0x1fffe7, 21},   {0x1fffe8, 21},
    {0x7ffff3, 23},  {0x3fffea, 22},   {0x3fffeb, 22},   {0x1ffffee, 25},
    {0x1ffffef, 25}, {0xfffff4, 24},   {0xfffff5, 24},   {0x3ffffea, 26},
    {0x7ffff4, 23},  {0x3ffffeb, 26},  {0x7ffffe6, 27},  {0x3ffffec, 26},
    {0x3ffffed, 26}, {0x7ffffe7, 27},  {0x7ffffe8, 27},  {0x7ffffe9, 27},
    {0x7ffffea, 27}, {0x7ffffeb, 27},  {0xffffffe, 28},  {0x7ffffec, 27},
    {0x7ffffed, 27}, {0x7ffffee, 27},  {0x7ffffef, 27},  {0x7fffff0, 27},
    {0x3ffffee, 26}, {0x3fffffff, 30}, // 256 EOS
};

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

static int hpack_decode_method(const uint8_t *data, size_t len,
                               char *out_method, size_t out_method_size) {
  if (!out_method || out_method_size == 0)
    return -1;
  out_method[0] = '\0'; // Start empty!

  const uint8_t *p = data;
  size_t remaining = len;

  while (remaining > 0) {
    uint8_t byte = *p;
    // p++;
    // remaining--;

    if (byte & 0x80) {
      // Indexed Header Field - check static table
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
          return 0; // Found it!
        }
      }
    } else if ((byte & 0xC0) == 0x40) {
      // Literal Header Field with Incremental Indexing
      int name_index;
      if (decode_int(&p, &remaining, 6, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      if (name_index == 0) {
        // Name is literal
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          // Name is Huffman encoded
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) <
              0)
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
        // Name is from static table
        name = hpack_static_table[name_index][0];
      }

      if (remaining == 0)
        break;

      // Decode value
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

    } else if ((byte & 0xF0) == 0x10) {
      // Literal Header Field without Indexing
      int name_index;
      if (decode_int(&p, &remaining, 4, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      if (name_index == 0) {
        // Name is literal
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          // Name is Huffman encoded
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) <
              0)
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
        // Name is from static table
        name = hpack_static_table[name_index][0];
      }

      if (remaining == 0)
        break;

      // Decode value
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
      // Literal Header Field never Indexed
      int name_index;
      if (decode_int(&p, &remaining, 4, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      if (name_index == 0) {
        // Name is literal
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          // Name is Huffman encoded
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) <
              0)
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
        // Name is from static table
        name = hpack_static_table[name_index][0];
      }

      if (remaining == 0)
        break;

      // Decode value
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

  // If no :method found, return error
  return -1;
}

static int hpack_decode_path(const uint8_t *data, size_t len, char *out_path,
                             size_t out_path_size) {
  if (!out_path || out_path_size == 0)
    return -1;
  out_path[0] = '\0'; // Start empty!

  const uint8_t *p = data;
  size_t remaining = len;

  fprintf(stderr, "DEBUG: hpack_decode_path start, len=%zu\n", len);

  while (remaining > 0) {
    uint8_t byte = *p;
    // p++; // REMOVED: decode_int handles the first byte
    // remaining--; // REMOVED

    fprintf(stderr, "DEBUG: byte=0x%02X\n", byte);

    if (byte & 0x80) {
      // Indexed Header Field - check static table
      int index;
      if (decode_int(&p, &remaining, 7, &index) < 0)
        break;

      fprintf(stderr, "DEBUG: Indexed field index=%d\n", index);

      if (index > 0 && index < HPACK_STATIC_TABLE_SIZE) {
        const char *name = hpack_static_table[index][0];
        const char *value = hpack_static_table[index][1];

        fprintf(stderr, "DEBUG: Static table hit: %s = %s\n", name, value);

        if (name && value && strcmp(name, ":path") == 0) {
          size_t copy_len = strlen(value);
          if (copy_len >= out_path_size)
            copy_len = out_path_size - 1;
          memcpy(out_path, value, copy_len);
          out_path[copy_len] = '\0';
          return 0; // Found it!
        }
      }
    } else if ((byte & 0xC0) == 0x40) {
      // Literal Header Field with Incremental Indexing
      int name_index;
      if (decode_int(&p, &remaining, 6, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      fprintf(stderr, "DEBUG: Literal Inc Index, name_index=%d\n", name_index);

      if (name_index == 0) {
        // Name is literal
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          // Name is Huffman encoded
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) <
              0)
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
        // Name is from static table
        name = hpack_static_table[name_index][0];
      }

      // if (name) fprintf(stderr, "DEBUG: Literal Name: %s\n", name);

      if (remaining == 0)
        break;

      // Decode value
      int h_bit = (*p & 0x80);
      int val_len;
      if (decode_int(&p, &remaining, 7, &val_len) < 0)
        break;
      if (val_len < 0 || remaining < (size_t)val_len)
        break;

      fprintf(stderr, "DEBUG: Value len=%d, h_bit=%d\n", val_len, h_bit);

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
        fprintf(stderr, "DEBUG: Decoded path: %s\n", out_path);
        return 0;
      }
      p += val_len;
      remaining -= val_len;

    } else if ((byte & 0xF0) == 0x10) {
      // Literal Header Field without Indexing
      int name_index;
      if (decode_int(&p, &remaining, 4, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      fprintf(stderr, "DEBUG: Literal No Index, name_index=%d\n", name_index);

      if (name_index == 0) {
        // Name is literal
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          // Name is Huffman encoded
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) <
              0)
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
        // Name is from static table
        name = hpack_static_table[name_index][0];
      }

      // if (name) fprintf(stderr, "DEBUG: Literal Name: %s\n", name);

      if (remaining == 0)
        break;

      // Decode value
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
        fprintf(stderr, "DEBUG: Decoded path: %s\n", out_path);
        return 0;
      }
      p += val_len;
      remaining -= val_len;
    } else if ((byte & 0xF0) == 0x00) {
      // Literal Header Field never Indexed
      int name_index;
      if (decode_int(&p, &remaining, 4, &name_index) < 0)
        break;

      char name_buf[256] = {0};
      const char *name = NULL;

      fprintf(stderr, "DEBUG: Literal Never Index, name_index=%d\n",
              name_index);

      if (name_index == 0) {
        // Name is literal
        int h_bit = (remaining > 0) ? (*p & 0x80) : 0;
        int name_len;
        if (decode_int(&p, &remaining, 7, &name_len) < 0)
          break;
        if (name_len < 0 || name_len >= (int)sizeof(name_buf))
          break;

        if (h_bit) {
          // Name is Huffman encoded
          if (huffman_decode(p, (size_t)name_len, name_buf, sizeof(name_buf)) <
              0)
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
        // Name is from static table
        name = hpack_static_table[name_index][0];
      }

      // if (name) fprintf(stderr, "DEBUG: Literal Name: %s\n", name);

      if (remaining == 0)
        break;

      // Decode value
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
        fprintf(stderr, "DEBUG: Decoded path: %s\n", out_path);
        return 0;
      }
      p += val_len;
      remaining -= val_len;
    } else {
      fprintf(stderr, "DEBUG: Unknown frame type\n");
      break;
    }
  }

  // If no :path found, return error
  fprintf(stderr, "DEBUG: No path found\n");
  return -1;
}

static unsigned char *hpack_encode_response(int status_code,
                                            const char *content_type,
                                            size_t content_length,
                                            size_t *out_size) {
  uint8_t status_idx = 0x88; // :status 200
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

  // content-type (index 31)
  buf[pos++] = 0x5f;
  buf[pos++] = (uint8_t)ct_len;
  memcpy(buf + pos, content_type, ct_len);
  pos += ct_len;

  // content-length (index 28)
  buf[pos++] = 0x5c;
  buf[pos++] = (uint8_t)cl_len;
  memcpy(buf + pos, cl_str, cl_len);
  pos += cl_len;

  *out_size = pos;
  return buf;
}

static int append_to_write_buffer(Connection *conn, const void *data,
                                  size_t len) {
  char *new_buf = realloc(conn->write_buffer, conn->write_buffer_size + len);
  if (!new_buf)
    return -1;
  memcpy(new_buf + conn->write_buffer_size, data, len);
  conn->write_buffer = new_buf;
  conn->write_buffer_size += len;
  return 0;
}

void http2_send_response(Connection *conn, uint32_t stream_id, int status_code,
                         const char *content_type, const void *body,
                         size_t body_len) {
  size_t headers_len;
  unsigned char *headers =
      hpack_encode_response(status_code, content_type, body_len, &headers_len);
  if (!headers)
    return;

  unsigned char headers_frame[9] = {0};
  headers_frame[0] = (headers_len >> 16) & 0xFF;
  headers_frame[1] = (headers_len >> 8) & 0xFF;
  headers_frame[2] = headers_len & 0xFF;
  headers_frame[3] = H2_FRAME_HEADERS;
  headers_frame[4] = 0x04;                     // END_HEADERS
  headers_frame[5] = (stream_id >> 24) & 0x7F; // Stream ID is 31-bit
  headers_frame[6] = (stream_id >> 16) & 0xFF;
  headers_frame[7] = (stream_id >> 8) & 0xFF;
  headers_frame[8] = stream_id & 0xFF;

  append_to_write_buffer(conn, headers_frame, 9);
  append_to_write_buffer(conn, headers, headers_len);
  free(headers);

  unsigned char data_frame[9] = {0};
  data_frame[0] = (body_len >> 16) & 0xFF;
  data_frame[1] = (body_len >> 8) & 0xFF;
  data_frame[2] = body_len & 0xFF;
  data_frame[3] = H2_FRAME_DATA;
  data_frame[4] = 0x01; // END_STREAM
  data_frame[5] = (stream_id >> 24) & 0x7F;
  data_frame[6] = (stream_id >> 16) & 0xFF;
  data_frame[7] = (stream_id >> 8) & 0xFF;
  data_frame[8] = stream_id & 0xFF;

  append_to_write_buffer(conn, data_frame, 9);
  append_to_write_buffer(conn, body, body_len);

  mod_epoll(conn->fd, EPOLLIN | EPOLLOUT | EPOLLET);
}

// Public entry point for h2c upgrade: handle the original HTTP/1.1 request
// as HTTP/2 stream 1, with already-parsed method and path.
void http2_handle_upgrade_request(Connection *conn, const char *method,
                                  const char *path) {
  // Normalize path: strip query string
  char clean_path[1024];
  strncpy(clean_path, path, sizeof(clean_path) - 1);
  clean_path[sizeof(clean_path) - 1] = '\0';
  char *q = strchr(clean_path, '?');
  if (q)
    *q = '\0';

  // Security check
  if (strstr(clean_path, "..") || clean_path[0] != '/') {
    http2_send_response(conn, 1, 400, "text/html", ___src_errors_404_html,
                        ___src_errors_404_html_len);
    return;
  }

  log_message(conn->config, LOG_INFO,
              "HTTP/2 upgrade request: %s %s (stream 1)", method, clean_path);

  // Try plugin first
  set_plugin_h2_context(conn, 1);
  int plugin_rv = handle_plugin_request(conn->fd, clean_path, (char *)method,
                                        "HTTP/2.0", NULL, 0, conn->config);
  set_plugin_h2_context(NULL, 0);
  if (plugin_rv == 0)
    return;

  // Serve static file
  char local_path[2048];
  if (strcmp(clean_path, "/") == 0) {
    snprintf(local_path, sizeof(local_path), "%s/index.html",
             conn->config->root);
  } else {
    snprintf(local_path, sizeof(local_path), "%s%s", conn->config->root,
             clean_path);
  }

  struct stat st;
  if (stat(local_path, &st) < 0) {
    if (!conn->config->require_extensions && strchr(clean_path, '.') == NULL) {
      snprintf(local_path, sizeof(local_path), "%s%s.html", conn->config->root,
               clean_path);
      if (stat(local_path, &st) == 0)
        goto file_found;
    }
    http2_send_response(conn, 1, 404, "text/html", ___src_errors_404_html,
                        ___src_errors_404_html_len);
    return;
  }

file_found:
  if (S_ISDIR(st.st_mode)) {
    strncat(local_path, "/index.html",
            sizeof(local_path) - strlen(local_path) - 1);
    if (stat(local_path, &st) < 0) {
      http2_send_response(conn, 1, 404, "text/html", ___src_errors_404_html,
                          ___src_errors_404_html_len);
      return;
    }
  }

  int fd = open(local_path, O_RDONLY);
  if (fd < 0) {
    http2_send_response(conn, 1, 500, "text/html", ___src_errors_500_html,
                        ___src_errors_500_html_len);
    return;
  }

  char *file_content = malloc(st.st_size);
  if (!file_content) {
    close(fd);
    http2_send_response(conn, 1, 500, "text/html", ___src_errors_500_html,
                        ___src_errors_500_html_len);
    return;
  }

  ssize_t bytes_read = read(fd, file_content, st.st_size);
  close(fd);
  if (bytes_read != st.st_size) {
    free(file_content);
    http2_send_response(conn, 1, 500, "text/html", ___src_errors_500_html,
                        ___src_errors_500_html_len);
    return;
  }

  const char *mime = get_mime_type(local_path);
  http2_send_response(conn, 1, 200, mime, file_content, bytes_read);
  free(file_content);
}

static void http2_handle_request(Connection *conn, uint32_t stream_id,
                                 const uint8_t *headers_data,
                                 size_t headers_len) {
  char path[1024];
  char method[16];

  if (hpack_decode_path(headers_data, headers_len, path, sizeof(path)) != 0) {
    log_message(conn->config, LOG_WARN, "Failed to decode :path header");
    http2_send_response(conn, stream_id, 400, "text/html",
                        ___src_errors_404_html, ___src_errors_404_html_len);
    return;
  }

  if (hpack_decode_method(headers_data, headers_len, method, sizeof(method)) !=
      0) {
    // If method isn't found, default to GET
    strcpy(method, "GET");
  }

  log_message(conn->config, LOG_INFO, "HTTP/2 request: %s %s (stream %u)",
              method, path, stream_id);

  // Remove query string
  char *query = strchr(path, '?');
  if (query)
    *query = '\0';

  // Normalize path: prevent directory traversal
  if (strstr(path, "..") || path[0] != '/') {
    http2_send_response(conn, stream_id, 400, "text/html",
                        ___src_errors_404_html, ___src_errors_404_html_len);
    return;
  }

  // First, try to handle as plugin request
  // Since we don't have the raw request in HTTP/2, we'll pass minimal data
  set_plugin_h2_context(conn, stream_id);
  int plugin_rv = handle_plugin_request(conn->fd, path, method, "HTTP/2.0",
                                        NULL, 0, conn->config);
  set_plugin_h2_context(NULL, 0);

  if (plugin_rv == 0) {
    // Plugin handled the request - for now, we'll return and assume the plugin
    // sent its response directly In a more sophisticated implementation, we'd
    // intercept the plugin's response and convert it to HTTP/2 frames
    return;
  }

  char local_path[2048];
  if (strcmp(path, "/") == 0) {
    snprintf(local_path, sizeof(local_path), "%s/index.html",
             conn->config->root);
  } else {
    snprintf(local_path, sizeof(local_path), "%s%s", conn->config->root, path);
  }

  struct stat st;
  if (stat(local_path, &st) < 0) {
    // Try .html extension if allowed and no dot in path
    if (!conn->config->require_extensions && strchr(path, '.') == NULL) {
      snprintf(local_path, sizeof(local_path), "%s%s.html", conn->config->root,
               path);
      if (stat(local_path, &st) == 0)
        goto file_found;
    }

    http2_send_response(conn, stream_id, 404, "text/html",
                        ___src_errors_404_html, ___src_errors_404_html_len);
    return;
  }

file_found:
  if (S_ISDIR(st.st_mode)) {
    strncat(local_path, "/index.html",
            sizeof(local_path) - strlen(local_path) - 1);
    if (stat(local_path, &st) < 0) {
      http2_send_response(conn, stream_id, 404, "text/html",
                          ___src_errors_404_html, ___src_errors_404_html_len);
      return;
    }
  }

  int fd = open(local_path, O_RDONLY);
  if (fd < 0) {
    http2_send_response(conn, stream_id, 500, "text/html",
                        ___src_errors_500_html, ___src_errors_500_html_len);
    return;
  }

  char *file_content = malloc(st.st_size);
  if (!file_content) {
    close(fd);
    http2_send_response(conn, stream_id, 500, "text/html",
                        ___src_errors_500_html, ___src_errors_500_html_len);
    return;
  }

  ssize_t bytes_read = read(fd, file_content, st.st_size);
  close(fd);

  if (bytes_read != st.st_size) {
    free(file_content);
    http2_send_response(conn, stream_id, 500, "text/html",
                        ___src_errors_500_html, ___src_errors_500_html_len);
    return;
  }

  const char *mime = get_mime_type(local_path);
  http2_send_response(conn, stream_id, 200, mime, file_content, bytes_read);
  free(file_content);
}

void http2_init(Connection *conn) {
  Http2State *state = calloc(1, sizeof(Http2State));
  if (state) {
    state->initialized = 1;
    state->remote_window_size = 65535;
    state->local_window_size = 65535;
    state->next_stream_id = 1;
    conn->http2_state = state;
  }
}

int http2_check_preface(const char *buffer, size_t len) {
  if (len < H2_PREFACE_LEN)
    return 0;
  return (memcmp(buffer, H2_PREFACE, H2_PREFACE_LEN) == 0);
}

void http2_handle_read(Connection *conn) {
  while (1) {
    ssize_t n = read(conn->fd, conn->read_buffer + conn->bytes_read,
                     conn->read_buffer_size - conn->bytes_read - 1);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return;
    }
    if (n == 0)
      return;

    conn->bytes_read += n;
    if (conn->bytes_read >= conn->read_buffer_size - 1) {
      conn->read_buffer_size *= 2;
      char *new_buf = realloc(conn->read_buffer, conn->read_buffer_size);
      if (!new_buf)
        return;
      conn->read_buffer = new_buf;
    }
  }

  if (conn->http2_state == NULL) {
    if (conn->bytes_read >= H2_PREFACE_LEN &&
        memcmp(conn->read_buffer, H2_PREFACE, H2_PREFACE_LEN) == 0) {
      http2_init(conn);
      memmove(conn->read_buffer, conn->read_buffer + H2_PREFACE_LEN,
              conn->bytes_read - H2_PREFACE_LEN);
      conn->bytes_read -= H2_PREFACE_LEN;

      log_message(conn->config, LOG_INFO, "HTTP/2 connection initialized");

      char settings_frame[9] = {0, 0, 0, 4, 0, 0, 0, 0, 0};
      conn->write_buffer = malloc(9);
      if (conn->write_buffer) {
        memcpy(conn->write_buffer, settings_frame, 9);
        conn->write_buffer_size = 9;
        conn->bytes_written = 0;
        mod_epoll(conn->fd, EPOLLIN | EPOLLOUT | EPOLLET);
      }
    } else {
      // Not HTTP/2 — should close or handle as HTTP/1.1, but we assume HTTP/2
      // only
      return;
    }
  } else {
    // http2_state already initialized (h2c upgrade path).
    // Client will send the HTTP/2 preface as first bytes after 101.
    // Strip it if present at the start of the buffer.
    if (conn->bytes_read >= H2_PREFACE_LEN &&
        memcmp(conn->read_buffer, H2_PREFACE, H2_PREFACE_LEN) == 0) {
      memmove(conn->read_buffer, conn->read_buffer + H2_PREFACE_LEN,
              conn->bytes_read - H2_PREFACE_LEN);
      conn->bytes_read -= H2_PREFACE_LEN;
      log_message(conn->config, LOG_DEBUG,
                  "HTTP/2 client preface received (h2c upgrade)");
    }
  }

  while (conn->bytes_read >= 9) {
    uint32_t length = ((uint8_t)conn->read_buffer[0] << 16) |
                      ((uint8_t)conn->read_buffer[1] << 8) |
                      (uint8_t)conn->read_buffer[2];
    uint8_t type = conn->read_buffer[3];
    uint8_t flags = conn->read_buffer[4];
    uint32_t stream_id =
        read_uint32((uint8_t *)conn->read_buffer + 5) & 0x7FFFFFFF;

    if (conn->bytes_read < 9 + length)
      return;

    log_message(conn->config, LOG_DEBUG,
                "HTTP/2 Frame: Type=%d, Len=%u, Stream=%u, Flags=0x%02x", type,
                length, stream_id, flags);

    if (type == H2_FRAME_SETTINGS) {
      if (!(flags & 0x1)) {
        char ack_frame[9] = {0, 0, 0, 4, 1, 0, 0, 0, 0};
        append_to_write_buffer(conn, ack_frame, 9);
        log_message(conn->config, LOG_DEBUG, "Sent SETTINGS ACK");
      }
    } else if (type == H2_FRAME_HEADERS) {
      // Handle PADDED and PRIORITY flags per RFC 7540 Section 6.2
      const uint8_t *headers_payload = (uint8_t *)conn->read_buffer + 9;
      size_t headers_payload_len = length;
      uint8_t pad_length = 0;

      if (flags & 0x08) { // PADDED flag
        if (headers_payload_len < 1)
          goto next_frame;
        pad_length = headers_payload[0];
        headers_payload++;
        headers_payload_len--;
        if (headers_payload_len < pad_length)
          goto next_frame;
        headers_payload_len -= pad_length;
      }

      if (flags & 0x20) { // PRIORITY flag
        if (headers_payload_len < 5)
          goto next_frame;
        headers_payload += 5;
        headers_payload_len -= 5;
      }

      http2_handle_request(conn, stream_id, headers_payload,
                           headers_payload_len);
    }

  next_frame:;
    size_t frame_len = 9 + length;
    memmove(conn->read_buffer, conn->read_buffer + frame_len,
            conn->bytes_read - frame_len);
    conn->bytes_read -= frame_len;
  }
}