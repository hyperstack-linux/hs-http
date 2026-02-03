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

// HPACK static table (subset - just the pseudo-headers we need)
static const char *hpack_static_table[][2] = {
    {NULL, NULL},             // 0 - unused
    {":authority", ""},       // 1
    {":method", "GET"},       // 2
    {":method", "POST"},      // 3
    {":path", "/"},           // 4
    {":path", "/index.html"}, // 5
    {":scheme", "http"},      // 6
    {":scheme", "https"},     // 7
    {":status", "200"},       // 8
    {":status", "204"},       // 9
    {":status", "206"},       // 10
    {":status", "304"},       // 11
    {":status", "400"},       // 12
    {":status", "404"},       // 13
    {":status", "500"},       // 14
    // ... more entries up to 61, but we only need these for now
};
#define HPACK_STATIC_TABLE_SIZE 15

// Huffman decoding for HPACK (RFC 7541 Appendix B)

// Simple Huffman decoder - decodes common URL characters
// Returns decoded length, or -1 on error
static int huffman_decode(const uint8_t *src, size_t src_len, char *dst,
                          size_t dst_size) {
  // Huffman codes from RFC 7541 - we use bit-by-bit decoding with a state
  // machine For simplicity, we implement a basic decoder for common ASCII chars

  uint32_t bits = 0;
  int nbits = 0;
  size_t dst_pos = 0;

  for (size_t i = 0; i < src_len && dst_pos < dst_size - 1; i++) {
    bits = (bits << 8) | src[i];
    nbits += 8;

    while (nbits >= 5 && dst_pos < dst_size - 1) {
      // Try to match prefixes (simplified - handles common URL chars)
      int matched = 0;

      // Check for common characters (5-8 bit codes)
      // '/' = 0b00111 (5 bits, value 7)
      if (nbits >= 5 && ((bits >> (nbits - 5)) & 0x1F) == 0x07) {
        dst[dst_pos++] = '/';
        nbits -= 5;
        matched = 1;
      }
      // 'a'-'z' = 5-6 bit codes (0x04-0x1d range for lowercase)
      else if (nbits >= 5) {
        uint8_t top5 = (bits >> (nbits - 5)) & 0x1F;
        // Map common letters
        if (top5 >= 0x04 && top5 <= 0x0d) {
          // Letters a-j have 5-bit codes
          static const char letters[] = "0abeginot"; // Approx mapping
          if (top5 - 3 < sizeof(letters)) {
            // More accurate: use lookup
            // 0x04='a', 0x05='c', 0x06='e', 0x07='/', 0x08='i'...
            static const char map5bit[] = {0,   0,   0,   0,   'a', 'c',
                                           'e', 0,   'i', 'o', 's', 't',
                                           ' ', '%', '-', '.'};
            if (top5 < 16 && map5bit[top5]) {
              dst[dst_pos++] = map5bit[top5];
              nbits -= 5;
              matched = 1;
            }
          }
        }
      }

      // 6-bit codes
      if (!matched && nbits >= 6) {
        uint8_t top6 = (bits >> (nbits - 6)) & 0x3F;
        // 6-bit character codes
        static const char map6bit[64] = {
            0,   0,   0,   0,   0,   0,   0,   0,   // 0x00-0x07
            0,   0,   0,   0,   0,   0,   0,   0,   // 0x08-0x0f
            0,   0,   0,   0,   0,   0,   0,   0,   // 0x10-0x17
            0,   0,   0,   0,   '0', '1', '2', 'b', // 0x18-0x1f
            'd', 'f', 'g', 'h', 'l', 'm', 'n', 'p', // 0x20-0x27
            'r', 'u', ':', 'B', 'C', 'D', 'E', 'F', // 0x28-0x2f
            'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', // 0x30-0x37
            'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V'  // 0x38-0x3f
        };
        if (map6bit[top6]) {
          dst[dst_pos++] = map6bit[top6];
          nbits -= 6;
          matched = 1;
        }
      }

      // 7-bit codes
      if (!matched && nbits >= 7) {
        uint8_t top7 = (bits >> (nbits - 7)) & 0x7F;
        // Some 7-bit codes
        if (top7 >= 0x5c && top7 <= 0x7f) {
          static const char map7bit[] = "3456789=AWXYZ_jkqvwxyz";
          int idx = top7 - 0x5c;
          if (idx >= 0 && idx < (int)sizeof(map7bit) - 1 && map7bit[idx]) {
            dst[dst_pos++] = map7bit[idx];
            nbits -= 7;
            matched = 1;
          }
        }
      }

      // 8-bit codes
      if (!matched && nbits >= 8) {
        uint8_t top8 = (bits >> (nbits - 8)) & 0xFF;
        // Handle remaining ASCII
        if (top8 >= 0xc0 && top8 <= 0xfb) {
          // Various punctuation and special chars
          static const char map8bit[] = "&*,;X!\"()+-./[\\]^`{|}";
          int idx = (top8 - 0xc0) / 2;
          if (idx >= 0 && idx < (int)sizeof(map8bit) - 1) {
            dst[dst_pos++] = map8bit[idx];
            nbits -= 8;
            matched = 1;
          }
        }
      }

      if (!matched) {
        // Unknown code - skip one bit and try again, or break
        nbits--;
        if (nbits < 5)
          break;
      }
    }

    bits &= (1 << nbits) - 1; // Keep only remaining bits
  }

  dst[dst_pos] = '\0';
  return dst_pos;
}

// Simple HPACK decoder - extracts :path from headers
// Returns path in out_path (must be at least 1024 bytes)
static int hpack_decode_path(const uint8_t *data, size_t len, char *out_path,
                             size_t out_path_size) {
  size_t pos = 0;
  out_path[0] = '/';
  out_path[1] = '\0';

  while (pos < len) {
    uint8_t byte = data[pos];

    if (byte & 0x80) {
      // Indexed header field
      int index = byte & 0x7F;
      if (index < HPACK_STATIC_TABLE_SIZE && hpack_static_table[index][0]) {
        if (strcmp(hpack_static_table[index][0], ":path") == 0) {
          strncpy(out_path, hpack_static_table[index][1], out_path_size - 1);
          out_path[out_path_size - 1] = '\0';
        }
      }
      pos++;
    } else if ((byte & 0xC0) == 0x40) {
      // Literal header field with incremental indexing
      int name_index = byte & 0x3F;
      pos++;

      if (name_index == 0 && pos < len) {
        // Name is a literal string
        int h_bit = data[pos] & 0x80;
        int name_len = data[pos] & 0x7F;
        pos++;

        char name[256] = {0};
        if (!h_bit && pos + name_len <= len) {
          memcpy(name, data + pos, name_len);
          pos += name_len;
        } else {
          pos += name_len; // Skip Huffman encoded
        }

        // Value
        if (pos < len) {
          h_bit = data[pos] & 0x80;
          int val_len = data[pos] & 0x7F;
          pos++;

          if (!h_bit && pos + val_len <= len) {
            if (strcmp(name, ":path") == 0) {
              size_t copy_len =
                  val_len < out_path_size - 1 ? val_len : out_path_size - 1;
              memcpy(out_path, data + pos, copy_len);
              out_path[copy_len] = '\0';
            }
            pos += val_len;
          } else {
            pos += val_len;
          }
        }
      } else if (name_index > 0) {
        // Name from index, value is literal
        const char *name = (name_index < HPACK_STATIC_TABLE_SIZE)
                               ? hpack_static_table[name_index][0]
                               : NULL;

        if (pos < len) {
          int h_bit = data[pos] & 0x80;
          int val_len = data[pos] & 0x7F;
          pos++;

          if (name && strcmp(name, ":path") == 0 && pos + val_len <= len) {
            if (h_bit) {
              // Huffman encoded - decode it
              huffman_decode(data + pos, val_len, out_path, out_path_size);
            } else {
              size_t copy_len = (size_t)val_len < out_path_size - 1
                                    ? (size_t)val_len
                                    : out_path_size - 1;
              memcpy(out_path, data + pos, copy_len);
              out_path[copy_len] = '\0';
            }
          }
          pos += val_len;
        }
      }
    } else if ((byte & 0xF0) == 0x00) {
      // Literal without indexing
      int name_index = byte & 0x0F;
      pos++;

      if (name_index == 0 && pos < len) {
        int name_len = data[pos] & 0x7F;
        pos++;
        pos += name_len;

        if (pos < len) {
          int val_len = data[pos] & 0x7F;
          pos++;
          pos += val_len;
        }
      } else if (name_index > 0 && pos < len) {
        int val_len = data[pos] & 0x7F;
        pos++;
        pos += val_len;
      }
    } else {
      // Skip unknown
      pos++;
    }
  }

  return 0;
}

// Create HPACK encoded response headers
// Returns allocated buffer (caller must free), size in out_size
static unsigned char *hpack_encode_response(int status_code,
                                            const char *content_type,
                                            size_t content_length,
                                            size_t *out_size) {
  // Status code index: 200->8, 404->13, 500->14
  uint8_t status_idx = 0x88; // Default 200
  if (status_code == 404)
    status_idx = 0x8d; // Index 13
  else if (status_code == 500)
    status_idx = 0x8e; // Index 14

  // Build headers
  size_t ct_len = strlen(content_type);
  char cl_str[32];
  int cl_len = snprintf(cl_str, sizeof(cl_str), "%zu", content_length);

  // Total size: status(1) + content-type(2+ct_len) + content-length(2+cl_len)
  size_t total = 1 + 2 + ct_len + 2 + cl_len;
  unsigned char *buf = malloc(total);
  if (!buf)
    return NULL;

  size_t pos = 0;

  // :status
  buf[pos++] = status_idx;

  // content-type (index 31)
  buf[pos++] = 0x5f; // Literal with name index 31
  buf[pos++] = (uint8_t)ct_len;
  memcpy(buf + pos, content_type, ct_len);
  pos += ct_len;

  // content-length (index 28)
  buf[pos++] = 0x5c; // Literal with name index 28
  buf[pos++] = (uint8_t)cl_len;
  memcpy(buf + pos, cl_str, cl_len);
  pos += cl_len;

  *out_size = pos;
  return buf;
}

// Append data to write buffer
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

// Send HTTP/2 response for a stream
static void http2_send_response(Connection *conn, uint32_t stream_id,
                                int status_code, const char *content_type,
                                const void *body, size_t body_len) {
  // Encode headers
  size_t headers_len;
  unsigned char *headers =
      hpack_encode_response(status_code, content_type, body_len, &headers_len);
  if (!headers)
    return;

  // HEADERS frame
  unsigned char headers_frame[9];
  headers_frame[0] = (headers_len >> 16) & 0xFF;
  headers_frame[1] = (headers_len >> 8) & 0xFF;
  headers_frame[2] = headers_len & 0xFF;
  headers_frame[3] = H2_FRAME_HEADERS;
  headers_frame[4] = 0x04; // END_HEADERS
  headers_frame[5] = (stream_id >> 24) & 0xFF;
  headers_frame[6] = (stream_id >> 16) & 0xFF;
  headers_frame[7] = (stream_id >> 8) & 0xFF;
  headers_frame[8] = stream_id & 0xFF;

  append_to_write_buffer(conn, headers_frame, 9);
  append_to_write_buffer(conn, headers, headers_len);
  free(headers);

  // DATA frame
  unsigned char data_frame[9];
  data_frame[0] = (body_len >> 16) & 0xFF;
  data_frame[1] = (body_len >> 8) & 0xFF;
  data_frame[2] = body_len & 0xFF;
  data_frame[3] = H2_FRAME_DATA;
  data_frame[4] = 0x01; // END_STREAM
  data_frame[5] = (stream_id >> 24) & 0xFF;
  data_frame[6] = (stream_id >> 16) & 0xFF;
  data_frame[7] = (stream_id >> 8) & 0xFF;
  data_frame[8] = stream_id & 0xFF;

  append_to_write_buffer(conn, data_frame, 9);
  append_to_write_buffer(conn, body, body_len);

  mod_epoll(conn->fd, EPOLLIN | EPOLLOUT | EPOLLET);
}

// Handle HTTP/2 request and serve file
static void http2_handle_request(Connection *conn, uint32_t stream_id,
                                 const uint8_t *headers_data,
                                 size_t headers_len) {
  char path[1024];
  hpack_decode_path(headers_data, headers_len, path, sizeof(path));

  log_message(conn->config, LOG_INFO, "HTTP/2 request: %s (stream %d)", path,
              stream_id);

  // Remove query string
  char *query = strchr(path, '?');
  if (query)
    *query = '\0';

  // Build local path
  char local_path[2048];
  snprintf(local_path, sizeof(local_path), "%s%s", conn->config->root,
           strcmp(path, "/") == 0 ? "/index.html" : path);

  // Check file
  struct stat st;
  if (stat(local_path, &st) < 0) {
    // Try with .html extension
    if (!conn->config->require_extensions && strchr(path, '.') == NULL) {
      snprintf(local_path, sizeof(local_path), "%s%s.html", conn->config->root,
               path);
      if (stat(local_path, &st) >= 0)
        goto file_found;
    }

    http2_send_response(conn, stream_id, 404, "text/html",
                        ___src_errors_404_html, ___src_errors_404_html_len);
    return;
  }

file_found:
  if (S_ISDIR(st.st_mode)) {
    strcat(local_path, "/index.html");
    if (stat(local_path, &st) < 0) {
      http2_send_response(conn, stream_id, 404, "text/html",
                          ___src_errors_404_html, ___src_errors_404_html_len);
      return;
    }
  }

  // Read file
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
  // First, read any available data
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
      conn->read_buffer = realloc(conn->read_buffer, conn->read_buffer_size);
    }
  }

  // Initialize if needed
  if (conn->http2_state == NULL) {
    http2_init(conn);
    if (conn->bytes_read >= H2_PREFACE_LEN) {
      memmove(conn->read_buffer, conn->read_buffer + H2_PREFACE_LEN,
              conn->bytes_read - H2_PREFACE_LEN);
      conn->bytes_read -= H2_PREFACE_LEN;

      log_message(conn->config, LOG_INFO, "HTTP/2 connection initialized");

      // Send SETTINGS
      char settings_frame[9] = {0, 0, 0, 4, 0, 0, 0, 0, 0};
      conn->write_buffer = malloc(9);
      memcpy(conn->write_buffer, settings_frame, 9);
      conn->write_buffer_size = 9;
      conn->bytes_written = 0;

      mod_epoll(conn->fd, EPOLLIN | EPOLLOUT | EPOLLET);
    }
  }

  // Process frames
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
                "HTTP/2 Frame: Type=%d, Len=%d, Stream=%d, Flags=%d", type,
                length, stream_id, flags);

    if (type == H2_FRAME_SETTINGS) {
      if ((flags & 0x1) == 0) {
        char ack_frame[9] = {0, 0, 0, 4, 1, 0, 0, 0, 0};
        append_to_write_buffer(conn, ack_frame, 9);
        log_message(conn->config, LOG_DEBUG, "Sent SETTINGS ACK");
      }
    } else if (type == H2_FRAME_HEADERS) {
      http2_handle_request(conn, stream_id, (uint8_t *)conn->read_buffer + 9,
                           length);
    }

    // Remove processed frame
    size_t frame_len = 9 + length;
    memmove(conn->read_buffer, conn->read_buffer + frame_len,
            conn->bytes_read - frame_len);
    conn->bytes_read -= frame_len;
  }
}
