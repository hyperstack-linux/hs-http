#ifndef HTTP2_H
#define HTTP2_H

#include "server.h"
#include <stdint.h>

// HTTP/2 Connection Preface
#define H2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_PREFACE_LEN 24

// Frame Types
typedef enum {
  H2_FRAME_DATA = 0x0,
  H2_FRAME_HEADERS = 0x1,
  H2_FRAME_PRIORITY = 0x2,
  H2_FRAME_RST_STREAM = 0x3,
  H2_FRAME_SETTINGS = 0x4,
  H2_FRAME_PUSH_PROMISE = 0x5,
  H2_FRAME_PING = 0x6,
  H2_FRAME_GOAWAY = 0x7,
  H2_FRAME_WINDOW_UPDATE = 0x8,
  H2_FRAME_CONTINUATION = 0x9
} Http2FrameType;

// Frame Flags
#define H2_FLAG_END_STREAM 0x1
#define H2_FLAG_END_HEADERS 0x4
#define H2_FLAG_PADDED 0x8
#define H2_FLAG_PRIORITY 0x20

// Frame Header
typedef struct {
  uint32_t length;
  uint8_t type;
  uint8_t flags;
  uint32_t stream_id;
} Http2FrameHeader;

// HTTP/2 State for a connection
typedef struct {
  int initialized;
  uint32_t remote_window_size;
  uint32_t local_window_size;
  uint32_t next_stream_id;
  // Add more state as needed (HPACK context, streams map, etc.)
} Http2State;

// Initialize HTTP/2 state for a connection
void http2_init(Connection *conn);

// Check if buffer contains HTTP/2 preface
int http2_check_preface(const char *buffer, size_t len);

// Handle read events for HTTP/2 connection
void http2_handle_read(Connection *conn);

// Send a simple SETTINGS frame
void http2_send_settings(Connection *conn);

#endif
